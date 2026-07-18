#include "openocpp/module/get_diagnostics_module.h"

namespace chargelab {

GetDiagnosticsModule::GetDiagnosticsModule(
        Settings& settings,
        std::shared_ptr<SystemInterface> system_interface,
        std::shared_ptr<UploadInterface> upload
) :
        system_interface_(std::move(system_interface)),
        upload_(std::move(upload)),
        charge_point_id_ {settings.getSetting(settings::CommonKey::kChargePointId)}
{
    listener_ = std::make_shared<LoggingListenerFunction>([&](LogMetadata const& metadata, std::string_view const& message) {
        if (metadata.level != LogLevel::warning && metadata.level != LogLevel::error && metadata.level != LogLevel::fatal)
            return;

        detail::DiagnosticsLine line {};
        line.level = metadata.level;
#if defined(LOG_WITH_FILE_AND_LINE)
        line.file = metadata.file;
        line.line = metadata.line;
#endif
        line.function = metadata.function;
        line.message = message;

        line.timestamp = system_interface_->systemClockNow();

        std::lock_guard lock {mutex_};

        int total_size = history_byte_size_ + line.size();
        while (total_size > kBufferLimitBytes && !history_.empty()) {
            total_size -= history_.front().size();
            history_.popFront();
        }
        if (total_size < kBufferLimitBytes) {
            history_byte_size_ = total_size;
            history_.pushBack(std::move(line));
        } else {
            history_byte_size_ = 0;
        }
    });

    RegisterLoggingListener(listener_); // register this callback to the global listener
}

GetDiagnosticsModule::~GetDiagnosticsModule() {
    CHARGELAB_LOG_MESSAGE(debug) << "Deleting GetDiagnosticsModule";
    UnregisterLoggingListener(listener_);
}

void GetDiagnosticsModule::runStep(ocpp1_6::ChargePointRemoteInterface &remote) {
    if (!in_progress_.has_value())
        return;

    auto &state = in_progress_.value();
    detail::DiagnosticsStatus const operation_state = state.operation_state;
    detail::DiagnosticsStatus const messaging_state = state.messaging_state;

    if (operation_state == messaging_state)
        return;

    ocpp1_6::DiagnosticsStatus status;

    switch (operation_state) {
        default:
            assert(false && "Unexpected diagnostics status");

        case detail::DiagnosticsStatus::kPending:
        case detail::DiagnosticsStatus::kUploading:
            status = ocpp1_6::DiagnosticsStatus::kUploading;
            break;
        case detail::DiagnosticsStatus::kUploaded:
            status = ocpp1_6::DiagnosticsStatus::kUploaded;
            state.operation->join();
            in_progress_ = std::nullopt;
            break;
        case detail::DiagnosticsStatus::kUploadFailed:
            status = ocpp1_6::DiagnosticsStatus::kUploadFailed;
            state.operation->join();
            in_progress_ = std::nullopt;
            break;
    }

    state.messaging_state = operation_state;
    state.pending_notification.setWithTimeout(
            settings_->DefaultMessageTimeout.getValue(),
            remote.sendDiagnosticsStatusNotificationReq({status})
    );
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetDiagnosticsRsp>>
GetDiagnosticsModule::onGetDiagnosticsReq(
        const ocpp1_6::GetDiagnosticsReq& req
) {
    if (in_progress_.has_value()) {
        return ocpp1_6::CallError {
                ocpp1_6::ErrorCode::kGenericError,
                "InProgress",
                "An uploading diagnostics operation is already in progress"
        };
    }

    ocpp1_6::GetDiagnosticsRsp rsp;

    std::queue<detail::DiagnosticsLine> pending_upload;
    {
        std::lock_guard lock{mutex_};
        int size = history_.size();
        for (int i = 0; i < size; ++i) {
            if (req.startTime.has_value() && history_[i].timestamp < req.startTime->getTimestamp())
                continue;
            if (req.stopTime.has_value() && history_[i].timestamp > req.stopTime->getTimestamp())
                continue;
            pending_upload.push(history_[i]);
        }
    }

    if (pending_upload.empty()) {
        rsp.fileName = std::nullopt;
        return rsp;
    }

    in_progress_.emplace(system_interface_);
    in_progress_->pending_upload = std::move(pending_upload);

    rsp.fileName = buildDiagnosticsFileName();

    auto &state = in_progress_.value();
    // Trim leading and trailing whitespaces
    std::string location = req.location.value();
    auto trim = [](const std::string &str, const std::string &whitespaces = " \t") -> std::string {
        auto begin = str.find_first_not_of(whitespaces);
        if (begin == std::string::npos) return "";
        auto end = str.find_last_not_of(whitespaces);
        return str.substr(begin, end - begin + 1);
    };
    location = trim(location);

    if (!location.empty() && location.back() != '/')
        state.location = location + "/" + rsp.fileName.value();
    else
        state.location = location + rsp.fileName.value();

    state.total_retries = optional::GetOrDefault(req.retries, kDefaultUploadRetries);
    state.retry_interval_seconds = optional::GetOrDefault(req.retryInterval,
                                                          kDefaultUploadRetryIntervalSeconds);

    state.operation = std::thread([&]() {
        using namespace std::chrono_literals;

        CHARGELAB_TRY {
            CHARGELAB_LOG_MESSAGE(info) << "Starting diagnostics upload: " << state.location;

            state.operation_state = detail::DiagnosticsStatus::kUploading;
            bool append = false;

            while (!state.pending_upload.empty() || state.current_upload.has_value()) {
                if (!state.current_upload.has_value()) {
                    std::string content;
                    size_t content_len = 0;
                    while (!state.pending_upload.empty()) {
                        std::string s = state.pending_upload.front().to_string();
                        if (content_len + s.size() > kMaxBytesPerUpload)
                            break;
                        content.append(s);
                        content_len += s.size();
                        state.pending_upload.pop();
                    }
                    state.current_upload.emplace(std::vector<uint8_t>(content.begin(), content.end()));
                }
                // uploading
                std::vector<uint8_t> const &content = state.current_upload.value();
                auto result = upload_->write(state.location, content, append, [&](std::size_t) {});
                if (result == UploadInterface::Result::kFailed) {
                    if (++state.failed_attempts > state.total_retries) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Diagnostics upload failed writing";
                        state.operation_state = detail::DiagnosticsStatus::kUploadFailed;
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(state.retry_interval_seconds));
                } else {
                    state.current_upload = std::nullopt;
                    append = true; // appending for next block
                }
            }

            CHARGELAB_LOG_MESSAGE(info) << "Diagnostics upload succeeded";
            state.operation_state = detail::DiagnosticsStatus::kUploaded;
        } CHARGELAB_CATCH {
            CHARGELAB_LOG_MESSAGE(error) << "Diagnostics upload failed unexpectedly: " << e.what();
            state.operation_state = detail::DiagnosticsStatus::kUploadFailed;
        }
    });

    return rsp;
}

void GetDiagnosticsModule::onDiagnosticsStatusNotificationRsp(
        std::string const& unique_id,
        ocpp1_6::ResponseMessage<::chargelab::ocpp1_6::DiagnosticsStatusNotificationRsp> const& rsp
) {
    if (in_progress_.has_value()) {
        auto& state = in_progress_.value();
        if (state.pending_notification == unique_id) {
            state.pending_notification = kNoOperation;

            if (std::holds_alternative<ocpp1_6::CallError>(rsp)) {
                CHARGELAB_LOG_MESSAGE(warning) << "Error response to FirmwareStatusNotification request: " << std::get<ocpp1_6::CallError> (rsp);
            }
        }
    }
}

std::string GetDiagnosticsModule::buildDiagnosticsFileName() {
    std::time_t t = system_interface_->systemClockNow()/1000;
    std::tm *tm = gmtime(&t);

    char buf[100];
    sprintf(buf, "%d-%02d-%02dT%02d-%02d-%02dZ", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

    std::string file_name = "diagn_" + charge_point_id_.getValue() + "_" + std::string(buf) + ".txt";

    return file_name;
}

} // namespace chargelab

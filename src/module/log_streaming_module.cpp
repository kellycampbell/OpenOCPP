#include "openocpp/module/log_streaming_module.h"

namespace chargelab {

LogStreamingModule::LogStreamingModule(
        std::shared_ptr<Settings> settings,
        std::shared_ptr<SystemInterface> const& system_interface
) : settings_(std::move(settings)),
    system_interface_(system_interface),
    pending_dump_request_ {system_interface}
{
    listener_ = std::make_shared<logging::LoggingListenerFunction>([&](logging::LogMetadata const& metadata, std::string_view const& message) {
        // TODO: Find a better way to filter out specific messages related to the dump itself
        if (message.find("Writing text message") != std::string::npos && message.find("StreamingLogMessage") != std::string::npos) {
            return;
        }
        if (!settings_->LogStreamingEnabled.getValue())
            return;

        detail::StreamingLogLine line {};
        line.level = metadata.level;
#if defined(LOG_WITH_FILE_AND_LINE)
        line.file = metadata.file;
        line.line = metadata.line;
        line.file.shrink_to_fit();
#endif
        line.function = metadata.function;
        line.message = message;

        line.function.shrink_to_fit();
        line.message.shrink_to_fit();

        std::lock_guard lock{mutex_};
        history_.pushBack(std::move(line));
    });

    logging::RegisterLoggingListener(listener_);
}

LogStreamingModule::~LogStreamingModule() {
    CHARGELAB_LOG_MESSAGE(debug) << "Deleting LogStreamingModule";
    logging::UnregisterLoggingListener(listener_);
}

void LogStreamingModule::runStep(ocpp1_6::OcppRemote &remote) {
    {
        std::lock_guard lock{mutex_};
        if (history_.empty())
            return;

        auto const now = system_interface_->steadyClockNow();
        if (history_.size() < kMaxLinesPerMessage && !getHistoryLimitIndex().has_value()) {
            if (!last_dump_.has_value())
                last_dump_ = now;

            auto const delta = now - last_dump_.value();
            if (delta < kBufferLimitMillis) {
                return;
            }
        }
    }

    if (!pending_dump_request_.operationInProgress()) {
        CHARGELAB_LOG_MESSAGE(debug) << "Flushing log messages - total size was: " << getHistorySize() << " bytes" << ", total memory size: " << getHistoryMemorySize()
            << ",ring buffer size: " << history_.size();

        detail::StreamingLogMessage payload {};
        {
            std::lock_guard lock{mutex_};
            int total_size = 0;
            for (int i=0; i < kMaxLinesPerMessage && !history_.empty(); i++) {
                if (total_size + history_.front().size() > kBufferLimitBytes) break;
                total_size += history_.front().size();

                detail::StreamingLogLine value = std::move(history_.front());

                history_.popFront();

                payload.messages.push_back(std::move(value));
            }
        }

        pending_dump_request_.setWithTimeout(
                settings_->DefaultMessageTimeout.getValue(),
                remote.sendDataTransferReq(ocpp1_6::DataTransferReq {
                        ocpp1_6::CiString255Type("ChargeLab"),
                        ocpp1_6::CiString50Type("StreamingLogMessage"),
                        write_json_to_string(payload)
                })
        );
    }

    std::lock_guard lock{mutex_};
    auto limit = getHistoryLimitIndex();
    if (limit.has_value()) {
        for (int i=0; i <= limit.value(); i++) {
            history_.front() = detail::StreamingLogLine {};
            history_.popFront();
        }
    }
}

void LogStreamingModule::onDataTransferRsp(
        const std::string &unique_id,
        const ocpp1_6::ResponseMessage<chargelab::ocpp1_6::DataTransferRsp> &rsp
) {
    if (pending_dump_request_ == unique_id) {
        pending_dump_request_ = kNoOperation;
        last_dump_ = system_interface_->steadyClockNow();

        if (std::holds_alternative<ocpp1_6::CallError>(rsp)) {
            CHARGELAB_LOG_MESSAGE(warning) << "Error response to DataTransfer request: " << std::get<ocpp1_6::CallError> (rsp);
        }
    }
}

std::optional<int> LogStreamingModule::getHistoryLimitIndex() {
    int index;
    std::size_t total_bytes = 0;
    for (index = history_.size()-1; index >= 0; index--) {
        auto const& value = history_[index];
        total_bytes += sizeof(value.level);
#if defined(LOG_WITH_FILE_AND_LINE)
        total_bytes += sizeof(std::string) + value.file.size();
        total_bytes += sizeof(value.line);
#endif
        total_bytes += sizeof(std::string) + value.function.size();
        total_bytes += sizeof(std::string) + value.message.size();

        if (total_bytes >= kBufferLimitBytes)
            return index;
    }

    return std::nullopt;
}

std::size_t LogStreamingModule::getHistorySize() {
    std::size_t total_bytes = 0;
    for (int index=0; index < history_.size(); index++) {
        auto const& value = history_[index];
        total_bytes += sizeof(value.level);
#if defined(LOG_WITH_FILE_AND_LINE)
        total_bytes += sizeof(std::string) + value.file.size();
        total_bytes += sizeof(value.line);
#endif
        total_bytes += sizeof(std::string) + value.function.size();
        total_bytes += sizeof(std::string) + value.message.size();
    }

    return total_bytes;
}

std::size_t LogStreamingModule::getHistoryMemorySize() {
    std::size_t total_bytes = 0;
    for (int index=0; index < history_.size(); index++) {
        auto const& value = history_[index];
        total_bytes += sizeof(value.level);
#if defined(LOG_WITH_FILE_AND_LINE)
        total_bytes += sizeof(std::string) + value.file.capacity();
        total_bytes += sizeof(value.line);
#endif
        total_bytes += sizeof(std::string) + value.function.capacity();
        total_bytes += sizeof(std::string) + value.message.capacity();
    }

    return total_bytes;
}

} // namespace chargelab

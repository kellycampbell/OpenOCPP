#ifndef CHARGELAB_OPEN_FIRMWARE_GET_DIAGNOSTICS_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_GET_DIAGNOSTICS_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/logging.h"
#include "openocpp/common/ring_buffer.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/interface/component/upload_interface.h"
#include "openocpp/helpers/chrono.h"
#include "openocpp/common/macro.h"

#include <queue>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <ctime>
#include <utility>

namespace chargelab {
    namespace detail {
        enum class DiagnosticsStatus {
            kPending,
            kUploading,
            kUploaded,
            kUploadFailed
        };

        struct DiagnosticsLine {
            logging::LogLevel level = logging::LogLevel::error;
#if defined(LOG_WITH_FILE_AND_LINE)
            std::string file {};
            int line = -1;
#endif
            std::string function {};
            std::string message {};
            SystemTimeMillis timestamp {};

            [[nodiscard]] int size() const {
                return sizeof(DiagnosticsLine)
#if defined(LOG_WITH_FILE_AND_LINE)
                    + static_cast<int>(file.size())
#endif
                    + static_cast<int>(function.size())
                    + static_cast<int>(message.size());
            }

            [[nodiscard]] std::string to_string() const {
                std::string result;
                result += std::to_string(static_cast<std::int64_t>(timestamp));
                result += " ";
                result += function;
                result += ": ";
                result += message;
                result += "\n";
                return result;
            }
        };

        struct DiagnosticsUploadState {
            explicit DiagnosticsUploadState(std::shared_ptr<SystemInterface> const& system)
                    : pending_notification {system} {
            }

            OperationHolder<std::string> pending_notification;

            std::string location {};
            int failed_attempts = 0;
            int total_retries = 0;
            int retry_interval_seconds = 0;

            std::atomic<DiagnosticsStatus> operation_state = DiagnosticsStatus::kPending;
            std::atomic<DiagnosticsStatus> messaging_state = DiagnosticsStatus::kPending;
            std::optional<std::thread> operation = std::nullopt;

            std::optional<std::vector<uint8_t>> current_upload = std::nullopt;
            std::queue<detail::DiagnosticsLine> pending_upload;
        };
    }

    class GetDiagnosticsModule : public ServiceStatefulGeneral {
    private:
        static constexpr int kBufferLimitBytes = 3*1024; // 10*1024;
        static constexpr int kRingBufferMaxSize = 25; // 50;
        // for diagnostic uploading
        static constexpr int kDefaultUploadRetries = 1;
        static constexpr int kDefaultUploadRetryIntervalSeconds = 10;
        static constexpr int kMaxBytesPerUpload = 3*1024; // 5*1024;

    public:
        explicit GetDiagnosticsModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> system_interface,
                std::shared_ptr<UploadInterface> upload
        );

        ~GetDiagnosticsModule() override;

    private:
        void runStep(ocpp1_6::OcppRemote &remote) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetDiagnosticsRsp>>
        onGetDiagnosticsReq(
                const ocpp1_6::GetDiagnosticsReq& req
        ) override;

        void onDiagnosticsStatusNotificationRsp(
                std::string const& unique_id,
                ocpp1_6::ResponseMessage<::chargelab::ocpp1_6::DiagnosticsStatusNotificationRsp> const& rsp
        ) override;

    private:
        std::string buildDiagnosticsFileName();

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_interface_;

        std::shared_ptr<logging::LoggingListenerFunction> listener_ = nullptr;
        std::mutex mutex_;
        RingBuffer<detail::DiagnosticsLine, kRingBufferMaxSize> history_;  // keep the original diagnostics lines
        int history_byte_size_ = 0;

        std::shared_ptr<UploadInterface> upload_;
        std::optional<detail::DiagnosticsUploadState> in_progress_ = std::nullopt;
    };
}


#endif //CHARGELAB_OPEN_FIRMWARE_GET_DIAGNOSTICS_MODULE_H

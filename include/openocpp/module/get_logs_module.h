#ifndef CHARGELAB_OPEN_FIRMWARE_GET_LOGS_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_GET_LOGS_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/module/pending_messages_module.h"
#include "openocpp/protocol/common/small_string.h"
#include "openocpp/interface/element/rest_connection_interface.h"
#include "openocpp/interface/platform_interface.h"
#include "openocpp/common/log_message_queue.h"
#include "openocpp/common/serialization.h"

namespace chargelab {
    namespace detail {
        struct UploadState {
            ocpp2_0::GetLogRequest request;
            std::string filename;

            std::shared_ptr<RestConnectionInterface> connection = nullptr;
            int total_failures = 0;

            std::size_t bytes_written = 0;
            std::size_t content_length = 0;
            int min_index = 0;
            int max_index = 0;
            int last_index = 0;

            std::optional<ocpp2_0::UploadLogStatusEnumType> last_status2_0 = std::nullopt;
            std::optional<SteadyPointMillis> first_attempt = std::nullopt;
        };

        struct LogLine {
            int message_index;
            SystemTimeMillis timestamp;
            logging::LogLevel level;
            std::string message;
        };

        class LogLineSerializer {
        public:
            static std::optional<LogLine> read(std::string_view const& text) {
                LogLine result;
                std::optional<int> index = 0;

                index = readPrimitive(text, index, result.message_index);
                index = readPrimitive(text, index, result.timestamp);
                index = readPrimitive(text, index, result.level);
                index = readPrimitive(text, index, result.message);

                if (!index.has_value())
                    return std::nullopt;

                return result;
            }

            static std::string write(LogLine const& wrapper) {
                std::string result;

                writePrimitive(result, wrapper.message_index);
                writePrimitive(result, wrapper.timestamp);
                writePrimitive(result, wrapper.level);
                writePrimitive(result, wrapper.message);

                return result;
            }
        };
    }

    class GetLogsModule : public ServiceStatefulGeneral {
    private:
        // Note: this is the decompressed size of the dropped records
        static constexpr int kTargetDroppedLogMessageBytes = 1024;
        static constexpr int kMaxRetainedLogMessagesBytes = 2*1024;
        static constexpr int kUploadStepMaxTimeMillis = 100;
        static constexpr int kPriorityLogStatusNotification = 100;
        static constexpr int kQueueReportFrequencySeconds = 10;

        // Note: the log buffer is statically sized because it is written from the logging callback,
        // which runs on arbitrary tasks and must not allocate. kMaxLogLineBytes is the truncation
        // point for a single line.
        static constexpr int kMaxRingBufferSize = 12;
        // Note: Entry carries a 16 byte header, so 240 makes each slot exactly 256 bytes. Lines
        // longer than this are truncated, and the overflow is recorded per line and rendered into
        // the uploaded log rather than being dropped silently.
        static constexpr int kMaxLogLineBytes = 240;

        // Note: arbitrary random assigned ID
        static constexpr std::uint64_t kOperationGroupId = 0xDB0A571F9D6E2A10ull;

    public:
        explicit GetLogsModule(
                std::shared_ptr<PlatformInterface> platform,
                std::shared_ptr<PendingMessagesModule> pending_messages
        );

        ~GetLogsModule() override;

    private:
        void runUnconditionally() override;
        void flushLogMessages();

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetLogResponse>>
        onGetLogReq(const ocpp2_0::GetLogRequest& request) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
        onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
        onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest &req) override;

    private:
        void reportQueueSize();
        void uploadLogs();
        static std::string renderLine(detail::LogLine const& line);
        bool checkOrRetryConnection();
        void checkAndUpdateStatus(ocpp2_0::UploadLogStatusEnumType status);

    private:
        std::shared_ptr<PlatformInterface> platform_;
        std::shared_ptr<PendingMessagesModule> pending_messages_;
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<logging::LoggingListenerFunction> listener_;

        std::optional<detail::UploadState> operation_ = std::nullopt;
        std::optional<SteadyPointMillis> last_queue_size_report_ = std::nullopt;
        using LogBuffer = LogMessageQueue<kMaxRingBufferSize, kMaxLogLineBytes>;
        static_assert(sizeof(LogBuffer::Entry) == 256, "Log entries are sized to land on an exact 256 byte slot");

        LogBuffer log_buffer_;

        // Note: only touched from the consuming task, so it needs no synchronization.
        int truncated_lines_ = 0;

        std::atomic<int> index_ = 0;
        CompressedQueueCustom<detail::LogLine, detail::LogLineSerializer> log_queue_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_GET_LOGS_MODULE_H

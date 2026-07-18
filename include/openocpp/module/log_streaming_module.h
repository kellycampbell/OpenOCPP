#ifndef CHARGELAB_OPEN_FIRMWARE_LOG_STREAMING_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_LOG_STREAMING_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/common/settings.h"

#include <utility>
#include <sstream>

namespace chargelab {
    namespace detail {
        struct LogLine {
            LogLevel level = LogLevel::kValueNotFoundInEnum;
#if defined(LOG_WITH_FILE_AND_LINE)
            std::string file {};
            int line = -1;
#endif
            std::string function {};
            std::string message {};

            int size() {
                return sizeof(LogLine) +
#if defined(LOG_WITH_FILE_AND_LINE)
                file.size() +
#endif
                function.size() + message.size();
            }
        };
#if defined(LOG_WITH_FILE_AND_LINE)
        CHARGELAB_OPTIONAL_NULL_DEFINE_TYPE_NON_INTRUSIVE(LogLine, level, file, line, function, message)
#else
        CHARGELAB_OPTIONAL_NULL_DEFINE_TYPE_NON_INTRUSIVE(LogLine, level, function, message)
#endif
        struct LogMessage {
            std::vector<LogLine> messages;
        };
        CHARGELAB_OPTIONAL_NULL_DEFINE_TYPE_NON_INTRUSIVE(LogMessage, messages)
    }

    class LogStreamingModule : public ChargePointServiceStateful {
    private:
        static const int kBufferLimitBytes = 3*1024; // 10*1024;
        static const int kRingBufferMaxSize = 25; // 50;
        static const int kMaxLinesPerMessage = 15; // 20;
        static const int kBufferLimitMillis = 30*1000; // 30*1000;
    public:
        LogStreamingModule(
                Settings& settings,
                std::shared_ptr<SystemInterface> const& system_interface
        );

        ~LogStreamingModule() override;

    private:
        void runStep(ocpp1_6::ChargePointRemoteInterface &remote) override;

        void onDataTransferRsp(
                const std::string &unique_id,
                const ocpp1_6::ResponseMessage<chargelab::ocpp1_6::DataTransferRsp> &rsp
        ) override;

    private:
        std::optional<int> getHistoryLimitIndex();
        std::size_t getHistorySize();
        std::size_t getHistoryMemorySize();

    private:
        std::shared_ptr<SystemInterface> system_interface_;
        OperationHolder<std::string> pending_dump_request_;
        BasicBooleanSetting<false> log_streaming_enabled_;

        std::optional<SteadyPointMillis> last_dump_ = std::nullopt;
        std::shared_ptr<LoggingListenerFunction> listener_ = nullptr;

        std::mutex mutex_;
        RingBuffer<detail::LogLine, kRingBufferMaxSize> history_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_LOG_STREAMING_MODULE_H

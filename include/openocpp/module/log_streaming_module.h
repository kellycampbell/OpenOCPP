#ifndef CHARGELAB_OPEN_FIRMWARE_LOG_STREAMING_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_LOG_STREAMING_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/logging.h"
#include "openocpp/common/ring_buffer.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/helpers/json.h"

#include <utility>
#include <sstream>
#include <mutex>
#include <vector>

namespace chargelab {
    namespace detail {
        // Note: these types are only ever serialized (uploaded), never parsed, so they provide a
        // write-only write_json rather than the full intrusive JSON macro (logging::LogLevel has no
        // read support).
        struct StreamingLogLine {
            logging::LogLevel level = logging::LogLevel::error;
#if defined(LOG_WITH_FILE_AND_LINE)
            std::string file {};
            int line = -1;
#endif
            std::string function {};
            std::string message {};

            int size() {
                return sizeof(StreamingLogLine) +
#if defined(LOG_WITH_FILE_AND_LINE)
                file.size() +
#endif
                function.size() + message.size();
            }

            static void write_json(::chargelab::json::JsonWriter& writer, StreamingLogLine const& value) {
                writer.StartObject();
                writer.Key("level");
                ::chargelab::json::WriteValue<logging::LogLevel>::write_json(writer, value.level);
#if defined(LOG_WITH_FILE_AND_LINE)
                writer.Key("file");
                ::chargelab::json::WriteValue<std::string>::write_json(writer, value.file);
                writer.Key("line");
                ::chargelab::json::WriteValue<int>::write_json(writer, value.line);
#endif
                writer.Key("function");
                ::chargelab::json::WriteValue<std::string>::write_json(writer, value.function);
                writer.Key("message");
                ::chargelab::json::WriteValue<std::string>::write_json(writer, value.message);
                writer.EndObject();
            }
        };

        struct StreamingLogMessage {
            std::vector<StreamingLogLine> messages;

            static void write_json(::chargelab::json::JsonWriter& writer, StreamingLogMessage const& value) {
                writer.StartObject();
                writer.Key("messages");
                ::chargelab::json::WriteValue<std::vector<StreamingLogLine>>::write_json(writer, value.messages);
                writer.EndObject();
            }
        };
    }

    class LogStreamingModule : public ServiceStateful1_6 {
    private:
        static const int kBufferLimitBytes = 3*1024; // 10*1024;
        static const int kRingBufferMaxSize = 25; // 50;
        static const int kMaxLinesPerMessage = 15; // 20;
        static const int kBufferLimitMillis = 30*1000; // 30*1000;
    public:
        LogStreamingModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> const& system_interface
        );

        ~LogStreamingModule() override;

    private:
        void runStep(ocpp1_6::OcppRemote &remote) override;

        void onDataTransferRsp(
                const std::string &unique_id,
                const ocpp1_6::ResponseMessage<chargelab::ocpp1_6::DataTransferRsp> &rsp
        ) override;

    private:
        std::optional<int> getHistoryLimitIndex();
        std::size_t getHistorySize();
        std::size_t getHistoryMemorySize();

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_interface_;
        OperationHolder<std::string> pending_dump_request_;

        std::optional<SteadyPointMillis> last_dump_ = std::nullopt;
        std::shared_ptr<logging::LoggingListenerFunction> listener_ = nullptr;

        std::mutex mutex_;
        RingBuffer<detail::StreamingLogLine, kRingBufferMaxSize> history_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_LOG_STREAMING_MODULE_H

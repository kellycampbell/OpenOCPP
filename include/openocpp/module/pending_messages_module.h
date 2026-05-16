#ifndef CHARGELAB_OPEN_FIRMWARE_PENDING_MESSAGES_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_PENDING_MESSAGES_MODULE_H

#include "openocpp/interface/platform_interface.h"
#include "openocpp/module/common_templates.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/common/compressed_queue.h"
#include "openocpp/common/logging.h"
#include "openocpp/common/serialization.h"

#include <string>
#include <optional>
#include <unordered_set>

namespace chargelab {
    CHARGELAB_JSON_ENUM(PendingMessageType,
        TransactionEvent,
        SecurityEvent,
        NotificationEvent,
        Other
    );

    struct PendingMessagePolicy {
        PendingMessageType message_type;
        std::optional<uint64_t> group_id = std::nullopt;
        int message_attempts = 1;
        int retry_interval_seconds = 10;

        // Determines which records are dropped first when available storage is exceeded - higher priority takes precedence
        int priority = 0;
        bool add_remote_transaction_id = false;
        bool add_message_sequence_number = false;
        bool must_flush_to_disk = false;
        CHARGELAB_JSON_INTRUSIVE(
                PendingMessagePolicy,
                message_type,
                group_id,
                message_attempts,
                retry_interval_seconds,
                priority,
                add_remote_transaction_id,
                add_message_sequence_number,
                must_flush_to_disk
        )
    };

    namespace detail {
        struct PendingMessageWrapper {
            int64_t unique_id;
            std::string payload;
            PendingMessagePolicy policy;
            std::optional<ocpp1_6::ActionId> action_id1_6;
            std::optional<ocpp2_0::ActionId> action_id2_0;

            int attempts = 0;
            CHARGELAB_JSON_INTRUSIVE(PendingMessageWrapper, unique_id, payload, policy, action_id1_6, action_id2_0, attempts)
        };

        class PendingMessageSerializer {
        public:
            static std::optional<PendingMessageWrapper> read(std::string_view const& text) {
                PendingMessageWrapper result;
                std::optional<int> index = 0;

                index = readPrimitive(text, index, result.unique_id);
                index = readPrimitive(text, index, result.payload);
                index = readPrimitive(text, index, result.policy.message_type);
                index = readPrimitive(text, index, result.policy.group_id);
                index = readPrimitive(text, index, result.policy.retry_interval_seconds);
                index = readPrimitive(text, index, result.policy.message_attempts);
                index = readPrimitive(text, index, result.policy.priority);
                index = readPrimitive(text, index, result.policy.add_remote_transaction_id);
                index = readPrimitive(text, index, result.policy.add_message_sequence_number);
                index = readPrimitive(text, index, result.policy.must_flush_to_disk);
                index = readPrimitive(text, index, result.action_id1_6);
                index = readPrimitive(text, index, result.action_id2_0);
                index = readPrimitive(text, index, result.attempts);

                if (!index.has_value())
                    return std::nullopt;

                return result;
            }

            static std::string write(PendingMessageWrapper const& wrapper) {
                std::string result;
                
                writePrimitive(result, wrapper.unique_id);
                writePrimitive(result, wrapper.payload);
                writePrimitive(result, wrapper.policy.message_type);
                writePrimitive(result, wrapper.policy.group_id);
                writePrimitive(result, wrapper.policy.retry_interval_seconds);
                writePrimitive(result, wrapper.policy.message_attempts);
                writePrimitive(result, wrapper.policy.priority);
                writePrimitive(result, wrapper.policy.add_remote_transaction_id);
                writePrimitive(result, wrapper.policy.add_message_sequence_number);
                writePrimitive(result, wrapper.policy.must_flush_to_disk);
                writePrimitive(result, wrapper.action_id1_6);
                writePrimitive(result, wrapper.action_id2_0);
                writePrimitive(result, wrapper.attempts);
                return result;
            }
        };
    }

    class PendingMessagesModule : public ServiceStatefulGeneral {
    public:
        using saved_message_supplier = std::function<void(std::function<void(detail::PendingMessageWrapper const&)> const&)>;

    public:
        PendingMessagesModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> const& system,
                std::shared_ptr<StorageInterface> storage
        );

        void registerOnSaveMessageSupplier(std::shared_ptr<saved_message_supplier> const& supplier);
        void unregisterOnSaveMessageSupplier(std::shared_ptr<saved_message_supplier> const& supplier);

    public:
        template<typename T>
        detail::PendingMessageWrapper generateRequest1_6(T const& request, PendingMessagePolicy policy = PendingMessagePolicy{}) {
            return detail::PendingMessageWrapper {
                    request_id_++,
                    write_json_to_string(request),
                    policy,
                    T::kActionId,
                    std::nullopt
            };
        }

        template<typename T>
        detail::PendingMessageWrapper generateRequest2_0(T const& request, PendingMessagePolicy const& policy) {
            return detail::PendingMessageWrapper {
                    request_id_++,
                    write_json_to_string(request),
                    policy,
                    std::nullopt,
                    T::kActionId
            };
        }

        template<typename T>
        std::string sendRequest1_6(T const& request, PendingMessagePolicy policy = PendingMessagePolicy{}) {
            detail::PendingMessageWrapper wrapper = generateRequest1_6(request, policy);
            if (activeGroupsContains(policy.group_id)) {
                offline_queue_.pushBack(wrapper);
                pending_messages_changed_ = true;
            } else {
                live_queue_.push_back(wrapper);
            }

            if (policy.must_flush_to_disk)
                must_flush_to_disk_ = true;

            return std::to_string(wrapper.unique_id);
        }

        template<typename T>
        std::string sendRequest2_0(T const& request, PendingMessagePolicy const& policy) {
            detail::PendingMessageWrapper wrapper = generateRequest2_0(request, policy);

            if (activeGroupsContains(policy.group_id)) {
                offline_queue_.pushBack(wrapper);
                pending_messages_changed_ = true;
            } else {
                live_queue_.push_back(wrapper);
            }

            if (policy.must_flush_to_disk)
                must_flush_to_disk_ = true;

            return std::to_string(wrapper.unique_id);
        }

        template <typename Visitor>
        void visitPending(Visitor&& visitor) {
            for (auto const& x : live_queue_)
                visitor(x.policy, x.payload);

            offline_queue_.visit([&] (std::string_view const&, detail::PendingMessageWrapper const& wrapper) {
                visitor(wrapper.policy, wrapper.payload);
            });
        }

    public:
        void runUnconditionally() override;
        void runStep(ocpp1_6::OcppRemote& remote) override;
        void runStep(ocpp2_0::OcppRemote &remote) override;

        void onStartTransactionRsp(
                const std::string &unique_id,
                const ocpp1_6::ResponseMessage<ocpp1_6::StartTransactionRsp> &rsp
        ) override;

    private:
        [[nodiscard]] bool activeGroupsContains(std::optional<int64_t> const& group_id) const;
        [[nodiscard]] bool blacklistContains(std::optional<int64_t> const& group_id) const;
        bool sendWithTransactionId(ocpp1_6::OcppRemote& remote, detail::PendingMessageWrapper const& wrapper);
        bool sendWithSequenceNumber(ocpp2_0::OcppRemote& remote, detail::PendingMessageWrapper const& wrapper);
        void limitOfflineQueueSize();
        void onCallRsp(const std::string &unique_id, const ocpp1_6::ResponseMessage<common::RawJson>& payload) override;
        void onCallRsp(const std::string &unique_id, const ocpp2_0::ResponseMessage<common::RawJson>& payload) override;

    private:
        void updateCacheAndStats();
        void flushToDisk();
        void saveToStorage();

        template <typename T>
        static void readFromFile(FILE* file, T& value) {
            auto parsed = file::json_read_object_from_file<T>(file);
            if (!parsed.has_value()) {
                CHARGELAB_LOG_MESSAGE(warning) << "Failed reading value from file";
                return;
            }

            value = parsed.value();
        }

        void loadFromStorage();
        int calculateAllPendingSavedInfoSize();
        bool shouldRemoveSequenceId(detail::PendingMessageWrapper const& wrapper);

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_;
        std::shared_ptr<StorageInterface> storage_;

        std::default_random_engine random_engine_;
        std::vector<detail::PendingMessageWrapper> live_queue_;
        CompressedQueueCustom<detail::PendingMessageWrapper, detail::PendingMessageSerializer> offline_queue_;
        OperationHolder<std::string> request_operation_;
        std::optional<detail::PendingMessageWrapper> request_object_;
        std::vector<std::shared_ptr<saved_message_supplier>> saved_message_suppliers_ {};

        bool must_flush_to_disk_ = false;
        int64_t request_id_;
        std::unordered_set<int64_t> active_group_ids_;
        std::unordered_set<int64_t> group_blacklist_;
        std::unordered_map<int64_t, int> transaction_ids_;
        std::unordered_map<int64_t, int> sequence_ids_;
        std::optional<SteadyPointMillis> last_cache_update_ = std::nullopt;
        std::optional<SteadyPointMillis> last_flush_to_disk_ = std::nullopt;
        bool pending_messages_changed_ = false;
        int32_t pending_messages_write_count_ = 0;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_PENDING_MESSAGES_MODULE_H

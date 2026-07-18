#ifndef CHARGELAB_OPEN_FIRMWARE_TRANSACTION_MODULE1_6_H
#define CHARGELAB_OPEN_FIRMWARE_TRANSACTION_MODULE1_6_H

#include "openocpp/interface/platform_interface.h"
#include "openocpp/interface/station_interface.h"
#include "openocpp/module/common_templates.h"
#include "openocpp/module/pending_messages_module.h"
#include "openocpp/module/boot_notification_module.h"
#include "openocpp/module/power_management_module1_6.h"
#include "openocpp/module/connector_status_module.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/common/logging.h"
#include "openocpp/model/transaction_container1_6.h"
#include "openocpp/interface/transaction_listener1_6.h"

#include <utility>
#include <random>
#include <unordered_set>
#include <deque>
#include <vector>
#include <memory>
#include <string>
#include <optional>

namespace chargelab {
    namespace transaction_module1_6 {
        struct PendingStartRequest {
            PendingStartRequest(std::shared_ptr<PlatformInterface> const& platform, int connector_id, std::string tag_id)
                : created_timestamp {platform->steadyClockNow()},
                  pending_authorize_req {platform},
                  connector_id {connector_id},
                  tag_id {tag_id},
                  charging_profile {},
                  authorize_finished {false}
            {
            }

            PendingStartRequest(std::shared_ptr<PlatformInterface> const& platform, ocpp1_6::RemoteStartTransactionReq const& req)
                : created_timestamp {platform->steadyClockNow()},
                  pending_authorize_req {platform},
                  connector_id {req.connectorId.value_or(0)},
                  tag_id {req.idTag.value()},
                  charging_profile {req.chargingProfile},
                  authorize_finished {!platform->getSettings()->AuthorizeRemoteTxRequests.getValue()}
            {
            }

            SteadyPointMillis created_timestamp;
            OperationHolder<std::string> pending_authorize_req;
            int connector_id;
            std::string tag_id;
            std::optional<ocpp1_6::ChargingProfile> charging_profile;

            bool authorize_finished;
        };
    }

    class TransactionModule1_6 : public ServiceStateful1_6 {
    private:
        static constexpr int kPriorityStartTransaction = 0;
        static constexpr int kPriorityStopTransaction = 0;
        static constexpr int kPriorityMeterValue = 5;

        static const int kMaxReadingsBytes = 10*1024;
        static const int kGeneralOperationMaxFailures = 4;
        static constexpr int kOfflineTransactions = 3;
        static constexpr char const* kDefaultTagId = "missing-tag";

        static constexpr int kTransactionMessageRetryIntervalSeconds = 10;
        static const int kSecondsPerDay = 86400; // 24*60*60
    public:
        TransactionModule1_6(
                std::shared_ptr<PlatformInterface> const& platform,
                std::shared_ptr<BootNotificationModule> boot_notification_module,
                std::shared_ptr<PowerManagementModule1_6> power_management_module,
                std::shared_ptr<PendingMessagesModule> pending_messages_module,
                std::shared_ptr<ConnectorStatusModule> connector_status_module,
                std::shared_ptr<StationInterface> station,
                std::shared_ptr<TransactionListener1_6> transaction_listener
        );

        ~TransactionModule1_6() override;

    private:
        void runStep(ocpp1_6::OcppRemote& remote) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::RemoteStartTransactionRsp>>
        onRemoteStartTransactionReq(const ocpp1_6::RemoteStartTransactionReq &req) override;

        void onAuthorizeRsp(
                const std::string &unique_id,
                const std::variant<ocpp1_6::AuthorizeRsp, ocpp1_6::CallError> &rsp
        ) override;

        void onStartTransactionRsp(
                const std::string &unique_id,
                const std::variant<ocpp1_6::StartTransactionRsp, ocpp1_6::CallError> &rsp
        ) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::RemoteStopTransactionRsp>>
        onRemoteStopTransactionReq(const ocpp1_6::RemoteStopTransactionReq &req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
        onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) override;

    private:
        void startTransaction(
                int connector_id,
                std::string const& id_tag,
                std::optional<ocpp1_6::ChargingProfile> const& charging_profile = std::nullopt
        );
        std::optional<detail::PendingMessageWrapper> generateStopRequest(int connector_id, ocpp1_6::Reason const& reason);
        void stopTransaction(int connector_id, ocpp1_6::Reason const& reason);
        void processRfidTap(std::string const& id_tag);
        void processVehicleConnectedStateChanged(int connector_id, bool connected);
        void processPendingStartRequest(ocpp1_6::OcppRemote& remote, std::optional<transaction_module1_6::PendingStartRequest>& pending);
        void addReading(transaction_module1_6::TransactionContainer& entry, ocpp1_6::ReadingContext const& context);
        bool shouldTakeReading(std::optional<SteadyPointMillis>& last, int interval_seconds);
        bool treatAsConnected();
        static std::unordered_set<ocpp1_6::Measurand::Value> parseMeasurandsString(std::string const& measurands);

    private:
        std::shared_ptr<PlatformInterface> platform_;
        std::shared_ptr<BootNotificationModule> boot_notification_module_;
        std::shared_ptr<PowerManagementModule1_6> power_management_module_;
        std::shared_ptr<PendingMessagesModule> pending_messages_module_;
        std::shared_ptr<ConnectorStatusModule> connector_status_module_;
        std::shared_ptr<StationInterface> station_;
        std::shared_ptr<TransactionListener1_6> transaction_listener_;
        std::shared_ptr<PendingMessagesModule::saved_message_supplier> stop_transaction_supplier_;

        std::shared_ptr<Settings> settings_;
        std::atomic<uint64_t> unique_index_;
        // key: connector ID
        std::unordered_map<int, std::optional<transaction_module1_6::PendingStartRequest>> pending_start_req_;
        // key: connector ID
        std::unordered_map<int, bool> last_plugged_in_state_;
        std::optional<ocpp1_6::IdToken> last_rfid_tag_id_ = std::nullopt;

        // Note: a transaction will remain here until the connector is unplugged or a new transaction starts
        // key: connector ID
        std::unordered_map<int, std::optional<transaction_module1_6::TransactionContainer>> active_transactions_;

        std::optional<int> connector_hold_id_ {std::nullopt};
        std::optional<int> force_stop_transaction_ = std::nullopt;

#if 0        // For OCTT _012
        std::unordered_map<int, SteadyPointMillis> pending_stop_transaction_times_;
#endif
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_TRANSACTION_MODULE1_6_H

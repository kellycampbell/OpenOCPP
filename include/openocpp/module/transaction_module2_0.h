#ifndef CHARGELAB_OPEN_FIRMWARE_TRANSACTION_MODULE2_0_H
#define CHARGELAB_OPEN_FIRMWARE_TRANSACTION_MODULE2_0_H

#include "openocpp/interface/platform_interface.h"
#include "openocpp/interface/station_interface.h"
#include "openocpp/module/common_templates.h"
#include "openocpp/module/pending_messages_module.h"
#include "openocpp/module/boot_notification_module.h"
#include "openocpp/module/power_management_module2_0.h"
#include "openocpp/module/connector_status_module.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/common/logging.h"
#include "openocpp/helpers/set.h"
#include "openocpp/protocol/ocpp2_0/types/tx_start_point_values.h"
#include "openocpp/protocol/ocpp2_0/types/tx_stop_point_values.h"
#include "openocpp/model/transaction_container2_0.h"
#include "openocpp/interface/transaction_listener2_0.h"

#include <utility>
#include <random>
#include <unordered_set>
#include <deque>
#include <vector>
#include <memory>
#include <string>
#include <optional>

namespace chargelab {
    /**
     * Note: the following are not supported:
     * - F01.FR.03:
     * - 2.6.6.1. TxStartPoint values:
     * -- EnergyTransfer
     * -- DataSigned
     * -- These two options introduce a new type of potentially long-lived "pending transaction" state after the
     *    charging session has been authorised but before either charging starts or the first signed meter values data.
     *
     */

    namespace transaction_module2_0 {
        struct PendingStartRequest {
            PendingStartRequest(std::shared_ptr<PlatformInterface> const& platform, std::optional<ocpp2_0::EVSEType> evse, ocpp2_0::IdTokenType id_token)
                : created_timestamp {platform->steadyClockNow()},
                  pending_authorize_req {platform},
                  id_token {std::move(id_token)},
                  charging_profile {},
                  remote_start_id {},
                  evse {evse},
                  // AuthCtrlr.AuthEnabled - if authorization is disabled, treat every presented token as pre-authorized
                  authorize_finished {!platform->getSettings()->AuthEnabled.getValue()}
            {
            }

            PendingStartRequest(std::shared_ptr<PlatformInterface> const& platform, ocpp2_0::RequestStartTransactionRequest const& req)
                : created_timestamp {platform->steadyClockNow()},
                  pending_authorize_req {platform},
                  id_token {req.idToken},
                  group_id_token {req.groupIdToken},
                  charging_profile {req.chargingProfile},
                  remote_start_id {req.remoteStartId},
                  // F01.FR.01/F01.FR.02, AuthCtrlr.AuthEnabled
                  authorize_finished {
                      !platform->getSettings()->AuthEnabled.getValue() ||
                      !platform->getSettings()->AuthorizeRemoteTxRequests.getValue()
                  }
            {
                if (req.evseId.has_value()) {
                    evse = ocpp2_0::EVSEType {req.evseId.value()};
                } else {
                    evse = std::nullopt;
                }
            }

            SteadyPointMillis created_timestamp;
            OperationHolder<std::string> pending_authorize_req;
            ocpp2_0::IdTokenType id_token;
            std::optional<ocpp2_0::IdTokenType> group_id_token;
            std::optional<ocpp2_0::ChargingProfileType> charging_profile;
            std::optional<int> remote_start_id;
            std::optional<ocpp2_0::EVSEType> evse;
            std::optional<uint64_t> transaction_id;

            bool authorize_finished;
        };

        struct MeterValuesResult {
            std::optional<std::vector<ocpp2_0::SampledValueType>> original;
            std::optional<std::vector<ocpp2_0::MeterValueType>> filtered;
        };
    }

    class TransactionModule2_0 : public ServiceStateful2_0 {
    private:
        static constexpr int kPriorityStartTransaction = 0;
        static constexpr int kPriorityStopTransaction = 0;
        static constexpr int kPriorityMeterValue = 5;
        static constexpr int kClockDriftThresholdSeconds = 10;
        static constexpr int kMillisecondsInDay = 1000*60*60*24;
        static constexpr std::uint64_t kMeterValuesGroupId = 0x3735DB9FF7144036ull;
        static constexpr int kPriorityMeterValuesNotification = 90;

    public:
        TransactionModule2_0(
                std::shared_ptr<PlatformInterface> platform,
                std::shared_ptr<BootNotificationModule> boot_notification_module,
                std::shared_ptr<PowerManagementModule2_0> power_management_module,
                std::shared_ptr<PendingMessagesModule> pending_messages_module,
                std::shared_ptr<ConnectorStatusModule> connector_status_module,
                std::shared_ptr<StationInterface> station,
                std::shared_ptr<TransactionListener2_0> transaction_listener
        );

        ~TransactionModule2_0() override;

    public:
        void runStep(ocpp2_0::OcppRemote& remote) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::RequestStartTransactionResponse>> onRequestStartTransactionReq(
                const ocpp2_0::RequestStartTransactionRequest &req
        ) override;

        void onAuthorizeRsp(
                const std::string &unique_id,
                const std::variant<ocpp2_0::AuthorizeResponse, ocpp2_0::CallError> &rsp
        ) override;

        void onTransactionEventRsp(
                const std::string &unique_id,
                const std::variant<ocpp2_0::TransactionEventResponse, ocpp2_0::CallError> &rsp
        ) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::RequestStopTransactionResponse>> onRequestStopTransactionReq(
                const ocpp2_0::RequestStopTransactionRequest &req
        ) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
        onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest &req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetTransactionStatusResponse>>
        onGetTransactionStatusReq(const ocpp2_0::GetTransactionStatusRequest &request) override;

    private:
        chargelab::transaction_module2_0::TransactionContainer& startTransaction(
                std::optional<ocpp2_0::EVSEType> evse,
                std::optional<ocpp2_0::IdTokenType> const& id_token,
                std::optional<ocpp2_0::IdTokenType> const& group_id_token,
                ocpp2_0::TriggerReasonEnumType const& trigger_reason,
                bool authorized,
                SystemTimeMillis timestamp,
                std::optional<std::vector<ocpp2_0::SampledValueType>> original_meter_values = std::nullopt,
                std::optional<std::vector<ocpp2_0::MeterValueType>> meter_values = std::nullopt,
                std::optional<ocpp2_0::ChargingProfileType> const& charging_profile = std::nullopt,
                std::optional<int> const& remote_start_id = std::nullopt
        );

        bool updateTransaction(
                std::optional<ocpp2_0::EVSEType> evse,
                std::optional<ocpp2_0::IdTokenType> const& id_token,
                std::optional<ocpp2_0::IdTokenType> const& group_id_token,
                ocpp2_0::TriggerReasonEnumType const& trigger_reason,
                SystemTimeMillis timestamp,
                std::optional<std::vector<ocpp2_0::MeterValueType>> meter_values = std::nullopt
        );

        detail::PendingMessageWrapper generateStopRequest(
                chargelab::transaction_module2_0::TransactionContainer const& transaction,
                std::optional<ocpp2_0::EVSEType> const& evse,
                ocpp2_0::TriggerReasonEnumType const& trigger_reason,
                ocpp2_0::ReasonEnumType const& stopped_reason,
                std::optional<ocpp2_0::IdTokenType> id_token
        );

        void stopTransaction(
                std::optional<ocpp2_0::EVSEType> const& evse,
                ocpp2_0::TriggerReasonEnumType const& trigger_reason,
                ocpp2_0::ReasonEnumType const& stopped_reason,
                std::optional<ocpp2_0::IdTokenType> id_token
        );

        void processRfidTap(ocpp2_0::IdTokenType const& id_token);
        bool stopByIdTag(ocpp2_0::IdTokenType id_token);

        void deauthorizeTransaction(
                std::optional<ocpp2_0::EVSEType> evse_id,
                chargelab::transaction_module2_0::TransactionContainer& transaction,
                ocpp2_0::TriggerReasonEnumType trigger_reason
        );

        bool stopByGroupId(ocpp2_0::IdTokenType id_token, std::optional<ocpp2_0::IdTokenType> const& group_id);

        void processVehicleConnectedStateChanged(ocpp2_0::EVSEType const& evse, bool connected);
        void processChargingEnabledStateChanged(ocpp2_0::EVSEType const& evse, bool charging_enabled);

        void processPendingStartRequest(ocpp2_0::OcppRemote& remote, std::optional<transaction_module2_0::PendingStartRequest>& pending);

        void addSampledReading(
                SystemTimeMillis now,
                std::optional<ocpp2_0::EVSEType> const& evse,
                chargelab::transaction_module2_0::TransactionContainer& entry,
                ocpp2_0::TriggerReasonEnumType const& trigger_reason,
                ocpp2_0::ReadingContextEnumType const& context,
                std::string const& measurands
        );

        void addEndedReading(
                SystemTimeMillis now,
                std::optional<ocpp2_0::EVSEType> const& evse,
                chargelab::transaction_module2_0::TransactionContainer& entry,
                ocpp2_0::ReadingContextEnumType const& context,
                std::string const& measurands
        );

        bool shouldTakeReading(SystemTimeMillis& last, int interval_seconds);

        ocpp2_0::ChargingStateEnumType getChargingState(
                std::optional<ocpp2_0::EVSEType> evse_id,
                chargelab::transaction_module2_0::TransactionContainer& transaction
        );

        bool treatAsConnected();
        bool isWebsocketConnected();

        transaction_module2_0::MeterValuesResult getMeterValues(
                std::optional<ocpp2_0::EVSEType> const& evse,
                ocpp2_0::ReadingContextEnumType const& context,
                std::string const& measurands,
                SystemTimeMillis now
        );

        std::set<ocpp2_0::TxStartPointValues> getStartPoints();
        std::set<ocpp2_0::TxStopPointValues> getStopPoints();

        static std::unordered_set<ocpp2_0::MeasurandEnumType::Value> parseMeasurandsString(std::string const& measurands);
        static SystemTimeMillis floorToSecond(SystemTimeMillis ts);

    //private:
    public:
        std::shared_ptr<PlatformInterface> platform_;
        std::shared_ptr<BootNotificationModule> boot_notification_module_;
        std::shared_ptr<PowerManagementModule2_0> power_management_module_;
        std::shared_ptr<PendingMessagesModule> pending_messages_module_;
        std::shared_ptr<ConnectorStatusModule> connector_status_module_;
        std::shared_ptr<StationInterface> station_;
        std::shared_ptr<TransactionListener2_0> transaction_listener_;
        std::default_random_engine random_engine_;
        std::shared_ptr<PendingMessagesModule::saved_message_supplier> stop_transaction_supplier_;

        std::shared_ptr<Settings> settings_;
        std::atomic<int64_t> unique_index_;
        std::map<std::optional<ocpp2_0::EVSEType>, std::optional<transaction_module2_0::PendingStartRequest>> pending_start_req_;
        std::map<ocpp2_0::EVSEType, bool> last_plugged_in_state_;
        std::map<ocpp2_0::EVSEType, bool> last_charging_enabled_state_;
        std::optional<ocpp2_0::IdTokenType> last_rfid_tag_id_ = std::nullopt;
        SystemTimeMillis last_non_transaction_meter_value_trigger_ = static_cast<SystemTimeMillis> (0);

        // Note: a transaction will remain here until the connector is unplugged or a new transaction starts
        std::map<std::optional<ocpp2_0::EVSEType>, std::optional<chargelab::transaction_module2_0::TransactionContainer>> active_transactions_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_TRANSACTION_MODULE2_0_H

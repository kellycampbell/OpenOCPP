#ifndef CHARGELAB_OPEN_FIRMWARE_CONNECTOR_STATUS_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_CONNECTOR_STATUS_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/platform_interface.h"
#include "openocpp/interface/station_interface.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/logging.h"
#include "openocpp/common/operation_holder.h"

#include <utility>
#include <set>

namespace chargelab {
    namespace detail {
        struct ReportedConnectorStatus {
            std::optional<SteadyPointMillis> last_status_sent = std::nullopt;
            std::optional<ocpp2_0::StatusNotificationRequest> last_reported_status2_0 = std::nullopt;
            std::optional<ocpp1_6::StatusNotificationReq> last_reported_status1_6 = std::nullopt;
            bool current_plug_was_charging = false;
            bool force_update = false;
        };
    }

    class ConnectorStatusModule : public ServiceStatefulGeneral {
    public:
        ConnectorStatusModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<PlatformInterface> const& platform,
                std::shared_ptr<StationInterface> station
        );

        ~ConnectorStatusModule() override;

    public:
        bool isChargingEnabled();
        void setPendingReset(bool value, bool hard_reset);
        [[nodiscard]] bool getPendingReset() const;
        [[nodiscard]] bool getResetReasonHard() const;
        void setPendingStartRequests(std::vector<std::optional<ocpp2_0::EVSEType>> const& pending);
        std::unordered_map<int, ocpp1_6::ChargePointStatus> getChargePointStatus1_6();
        bool setConnector0Inoperative(bool inoperative);

    private:
        void runUnconditionally() override;
        void runStep(ocpp2_0::OcppRemote &remote) override;
        void onStatusNotificationRsp(const std::string &uniqueId, const ocpp2_0::ResponseMessage<ocpp2_0::StatusNotificationResponse> &rsp) override;
        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>> onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest &req) override;
        void runStep(ocpp1_6::OcppRemote &remote) override;
        void onStatusNotificationRsp(const std::string &uniqueId, const ocpp1_6::ResponseMessage<ocpp1_6::StatusNotificationRsp> &rsp) override;
        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>> onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) override;
        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeAvailabilityRsp>> onChangeAvailabilityReq(const ocpp1_6::ChangeAvailabilityReq &req) override;
        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ChangeAvailabilityResponse>> onChangeAvailabilityReq(const ocpp2_0::ChangeAvailabilityRequest &req) override;

    private:
        void advanceConnectorState(detail::ReportedConnectorStatus& reported, charger::ConnectorStatus const& current);
        void saveInoperativeConnectors();
        void loadInoperativeConnectors();
        ocpp2_0::ConnectorStatusEnumType getStatus2_0(ocpp2_0::EVSEType const& evse, charger::ConnectorStatus const& status);
        ocpp1_6::ChargePointStatus getStatus1_6(ocpp2_0::EVSEType const& evse, charger::ConnectorStatus const& current, bool was_charging);
        void addAndUpdateStateSettings();

        template <typename Map, typename Key, typename Generator>
        auto& getOrCreateSetting(Map& map, Key&& key, Generator&& generator) {
            auto it = map.find(key);
            if (it != map.end() && it->second != nullptr)
                return *it->second;

            auto setting = generator();
            map[key] = setting;
            settings_->registerCustomSetting(setting);
            return *setting;
        }

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<PlatformInterface> platform_;
        std::shared_ptr<StationInterface> station_;

        OperationHolder<std::string> pending_connector_status_req_;
        std::optional<std::pair<chargelab::ocpp2_0::EVSEType, ocpp2_0::StatusNotificationRequest>> pending_connector_status_update2_0_ = std::nullopt;
        std::optional<std::pair<chargelab::ocpp2_0::EVSEType, ocpp1_6::StatusNotificationReq>> pending_connector_status_update1_6_ = std::nullopt;
        std::map<chargelab::ocpp2_0::EVSEType, detail::ReportedConnectorStatus> reported_status_;
        std::map<std::optional<chargelab::ocpp2_0::EVSEType>, bool> inoperative_connectors_;
        std::atomic<bool> pending_reset_ = false;
        std::atomic<bool> reset_reason_hard_ = false;
        std::optional<SteadyPointMillis> last_online_ = std::nullopt;

        std::vector<std::optional<ocpp2_0::EVSEType>> pending_start_;

        std::optional<SteadyPointMillis> last_settings_update_ = std::nullopt;
        std::map<chargelab::ocpp2_0::EVSEType, std::shared_ptr<SettingBool>> settings_available_ {};
        std::map<chargelab::ocpp2_0::EVSEType, std::shared_ptr<SettingString>> settings_availability_state_ {};
        std::map<chargelab::ocpp2_0::EVSEType, std::shared_ptr<SettingString>> settings_connector_type_ {};
        std::map<chargelab::ocpp2_0::EVSEType, std::shared_ptr<SettingInt>> settings_supply_phases_ {};
        std::map<chargelab::ocpp2_0::EVSEType, std::shared_ptr<SettingDouble>> settings_power_ {};

        std::unordered_map<int, ocpp1_6::ChargePointStatus> last_charge_point_status_map1_6_ {};
        std::mutex last_charge_point_status_map1_6_mutex_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_CONNECTOR_STATUS_MODULE_H

#ifndef CHARGELAB_OPEN_FIRMWARE_HEARTBEAT_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_HEARTBEAT_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/module/boot_notification_module.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/common/settings.h"

#include <utility>

namespace chargelab {
    class HeartbeatModule : public ServiceStatefulGeneral {
    public:
        HeartbeatModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> const& system_interface,
                std::shared_ptr<BootNotificationModule> boot_notification_module
        );

        ~HeartbeatModule() override;

    public:
        void runStep(ocpp1_6::OcppRemote &remote) override;

        void onHeartbeatRsp(
                const std::string &unique_id,
                const std::variant<ocpp1_6::HeartbeatRsp, ocpp1_6::CallError> &rsp
        ) override;

        void runStep(ocpp2_0::OcppRemote &remote) override;

        void onHeartbeatRsp(
                const std::string &unique_id,
                const std::variant<ocpp2_0::HeartbeatResponse, ocpp2_0::CallError> &rsp
        ) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
        onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
        onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest &request) override;

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_interface_;
        std::shared_ptr<BootNotificationModule> boot_notification_module_;
        OperationHolder<std::string> pending_heartbeat_req_;
        bool force_heartbeat_ = false;
        // Set once, the first time registrationComplete() is observed true, so the idle timer
        // below can be started from that point rather than from construction.
        bool boot_notification_synced_ = false;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_HEARTBEAT_MODULE_H

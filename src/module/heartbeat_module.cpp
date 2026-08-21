#include "openocpp/module/heartbeat_module.h"

namespace chargelab {

HeartbeatModule::HeartbeatModule(
        std::shared_ptr<Settings> settings,
        std::shared_ptr<SystemInterface> const& system_interface,
        std::shared_ptr<BootNotificationModule> boot_notification_module
)
    : settings_(std::move(settings)),
      system_interface_(system_interface),
      boot_notification_module_(std::move(boot_notification_module)),
      pending_heartbeat_req_ {system_interface}
{
}

HeartbeatModule::~HeartbeatModule() {
    CHARGELAB_LOG_MESSAGE(debug) << "Deleting HeartbeatModule";
}

void HeartbeatModule::runStep(ocpp1_6::OcppRemote &remote) {
    // Don't spontaneously send heartbeats before BootNotification has completed - the CSMS just
    // heard from us via BootNotification (and will again via the connectors' initial
    // StatusNotification), so an immediate Heartbeat on top of those is redundant traffic. A
    // TriggerMessage-forced heartbeat is still honoured; boot_notification_module_ already
    // rejects those before registration completes.
    if (!boot_notification_synced_) {
        if (!boot_notification_module_->registrationComplete()) {
            if (!force_heartbeat_)
                return;
        } else {
            // Registration just completed - start the idle timer now (rather than leaving it at
            // its "infinitely idle" construction-time state) so the first heartbeat waits a full
            // HeartbeatInterval instead of firing immediately.
            pending_heartbeat_req_ = kNoOperation;
            boot_notification_synced_ = true;
        }
    }

    if (!pending_heartbeat_req_.wasIdleFor(settings_->HeartbeatInterval.getValue())) {
        if (!force_heartbeat_ || pending_heartbeat_req_.operationInProgress())
            return;
    }

    pending_heartbeat_req_.setWithTimeout(
            settings_->DefaultMessageTimeout.getValue(),
            remote.sendHeartbeatReq({})
    );
    force_heartbeat_ = false;
}

void HeartbeatModule::onHeartbeatRsp(
        const std::string &unique_id,
        const std::variant<ocpp1_6::HeartbeatRsp, ocpp1_6::CallError> &rsp
) {
    if (pending_heartbeat_req_ == unique_id) {
        pending_heartbeat_req_ = kNoOperation;

        if (std::holds_alternative<ocpp1_6::HeartbeatRsp>(rsp)) {
            auto const& value = std::get<ocpp1_6::HeartbeatRsp>(rsp);
            auto const& ts = value.currentTime.getTimestamp();

            if (ts.has_value()) {
                system_interface_->setSystemClock(ts.value());
            } else {
                CHARGELAB_LOG_MESSAGE(warning) << "Invalid Heartbeat response timestamp: "
                    << value.currentTime;
            }
        } else {
            CHARGELAB_LOG_MESSAGE(warning) << "Error response to Hearbeat request: " << std::get<ocpp1_6::CallError> (rsp);
        }
    }
}

void HeartbeatModule::runStep(ocpp2_0::OcppRemote &remote) {
    // See the OCPP 1.6 runStep() above for why registration completion gates this and resets
    // the idle timer.
    if (!boot_notification_synced_) {
        if (!boot_notification_module_->registrationComplete()) {
            if (!force_heartbeat_)
                return;
        } else {
            pending_heartbeat_req_ = kNoOperation;
            boot_notification_synced_ = true;
        }
    }

    if (!pending_heartbeat_req_.wasIdleFor(settings_->HeartbeatInterval.getValue())) {
        if (!force_heartbeat_ || pending_heartbeat_req_.operationInProgress())
            return;
    }

    pending_heartbeat_req_.setWithTimeout(
            settings_->DefaultMessageTimeout.getValue(),
            remote.sendHeartbeatReq({})
    );
    force_heartbeat_ = false;
}

void HeartbeatModule::onHeartbeatRsp(
        const std::string &unique_id,
        const std::variant<ocpp2_0::HeartbeatResponse, ocpp2_0::CallError> &rsp
) {
    if (pending_heartbeat_req_ == unique_id) {
        pending_heartbeat_req_ = kNoOperation;

        if (std::holds_alternative<ocpp2_0::HeartbeatResponse>(rsp)) {
            auto const& value = std::get<ocpp2_0::HeartbeatResponse>(rsp);
            auto const& ts = value.currentTime.getTimestamp();

            if (ts.has_value()) {
                system_interface_->setSystemClock(ts.value());
            } else {
                CHARGELAB_LOG_MESSAGE(warning) << "Invalid Heartbeat response timestamp: "
                                               << value.currentTime;
            }
        } else {
            CHARGELAB_LOG_MESSAGE(warning) << "Error response to Hearbeat request: " << std::get<ocpp2_0::CallError> (rsp);
        }
    }
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
HeartbeatModule::onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) {
    if (req.requestedMessage == ocpp1_6::MessageTrigger::kHeartbeat) {
        force_heartbeat_ = true;
        return ocpp1_6::TriggerMessageRsp {ocpp1_6::TriggerMessageStatus::kAccepted};
    }

    return std::nullopt;
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
HeartbeatModule::onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest &request) {
    if (request.requestedMessage == ocpp2_0::MessageTriggerEnumType::kHeartbeat) {
        force_heartbeat_ = true;
        return ocpp2_0::TriggerMessageResponse {ocpp2_0::TriggerMessageStatusEnumType::kAccepted};
    }

    return std::nullopt;
}

} // namespace chargelab

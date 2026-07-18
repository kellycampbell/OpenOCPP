#ifndef CHARGELAB_OPEN_FIRMWARE_BOOT_NOTIFICATION_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_BOOT_NOTIFICATION_MODULE_H

#include <utility>

#include "openocpp/module/common_templates.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/logging.h"

namespace chargelab {
    class BootNotificationModule : public ServiceStatefulGeneral
    {
        /* OCPP 1.6 Section 4.2 on Pending registration statuses:
         *
         * The Central System MAY also return a Pending registration status to indicate that it wants to retrieve or set
         * certain information on the Charge Point before the Central System will accept the Charge Point. If the Central
         * System returns the Pending status, the communication channel SHOULD NOT be closed by either the Charge
         * Point or the Central System. The Central System MAY send request messages to retrieve information from the
         * Charge Point or change its configuration. The Charge Point SHOULD respond to these messages. The Charge
         * Point SHALL NOT send request messages to the Central System unless it has been instructed by the Central
         * System to do so with a TriggerMessage.req request.
         */

    private:
        static constexpr const int kSecurityProfileMigrationResetGracePeriodMillis = 5*1000;

    public:
        BootNotificationModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> const& system_interface
        );

        ~BootNotificationModule() override;

    public:
        bool registrationComplete();

    private:
        // OCPP 1.6 implementation
        void runStep(ocpp1_6::OcppRemote &remote) override;

        void onBootNotificationRsp(
                const std::string &unique_id,
                const ocpp1_6::ResponseMessage<ocpp1_6::BootNotificationRsp> &rsp
        ) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
        onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::RemoteStartTransactionRsp>>
        onRemoteStartTransactionReq(const ocpp1_6::RemoteStartTransactionReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::RemoteStopTransactionRsp>>
        onRemoteStopTransactionReq(const ocpp1_6::RemoteStopTransactionReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::CancelReservationRsp>>
        onCancelReservationReq(const ocpp1_6::CancelReservationReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeAvailabilityRsp>>
        onChangeAvailabilityReq(const ocpp1_6::ChangeAvailabilityReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeConfigurationRsp>>
        onChangeConfigurationReq(const ocpp1_6::ChangeConfigurationReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearCacheRsp>>
        onClearCacheReq(const ocpp1_6::ClearCacheReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearChargingProfileRsp>>
        onClearChargingProfileReq(const ocpp1_6::ClearChargingProfileReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::DataTransferRsp>>
        onDataTransferReq(const ocpp1_6::DataTransferReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetCompositeScheduleRsp>>
        onGetCompositeScheduleReq(const ocpp1_6::GetCompositeScheduleReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetConfigurationRsp>>
        onGetConfigurationReq(const ocpp1_6::GetConfigurationReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetDiagnosticsRsp>>
        onGetDiagnosticsReq(const ocpp1_6::GetDiagnosticsReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetLocalListVersionRsp>>
        onGetLocalListVersionReq(const ocpp1_6::GetLocalListVersionReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ReserveNowRsp>>
        onReserveNowReq(const ocpp1_6::ReserveNowReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ResetRsp>>
        onResetReq(const ocpp1_6::ResetReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SendLocalListRsp>>
        onSendLocalListReq(const ocpp1_6::SendLocalListReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SetChargingProfileRsp>>
        onSetChargingProfileReq(const ocpp1_6::SetChargingProfileReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UnlockConnectorRsp>>
        onUnlockConnectorReq(const ocpp1_6::UnlockConnectorReq&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UpdateFirmwareRsp>>
        onUpdateFirmwareReq(const ocpp1_6::UpdateFirmwareReq&) override;

        // OCPP 2.0.1 implementation
        void runStep(ocpp2_0::OcppRemote &remote) override;

        void onBootNotificationRsp(
                const std::string &unique_id,
                const ocpp2_0::ResponseMessage<ocpp2_0::BootNotificationResponse> &rsp
        ) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
        onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest& req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CancelReservationResponse>>
        onCancelReservationReq(const ocpp2_0::CancelReservationRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CertificateSignedResponse>>
        onCertificateSignedReq(const ocpp2_0::CertificateSignedRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ChangeAvailabilityResponse>>
        onChangeAvailabilityReq(const ocpp2_0::ChangeAvailabilityRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearCacheResponse>>
        onClearCacheReq(const ocpp2_0::ClearCacheRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearChargingProfileResponse>>
        onClearChargingProfileReq(const ocpp2_0::ClearChargingProfileRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearDisplayMessageResponse>>
        onClearDisplayMessageReq(const ocpp2_0::ClearDisplayMessageRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearVariableMonitoringResponse>>
        onClearVariableMonitoringReq(const ocpp2_0::ClearVariableMonitoringRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CostUpdatedResponse>>
        onCostUpdatedReq(const ocpp2_0::CostUpdatedRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CustomerInformationResponse>>
        onCustomerInformationReq(const ocpp2_0::CustomerInformationRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::DataTransferResponse>>
        onDataTransferReq(const ocpp2_0::DataTransferRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::DeleteCertificateResponse>>
        onDeleteCertificateReq(const ocpp2_0::DeleteCertificateRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetBaseReportResponse>>
        onGetBaseReportReq(const ocpp2_0::GetBaseReportRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetChargingProfilesResponse>>
        onGetChargingProfilesReq(const ocpp2_0::GetChargingProfilesRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetCompositeScheduleResponse>>
        onGetCompositeScheduleReq(const ocpp2_0::GetCompositeScheduleRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetDisplayMessagesResponse>>
        onGetDisplayMessagesReq(const ocpp2_0::GetDisplayMessagesRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetInstalledCertificateIdsResponse>>
        onGetInstalledCertificateIdsReq(const ocpp2_0::GetInstalledCertificateIdsRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetLocalListVersionResponse>>
        onGetLocalListVersionReq(const ocpp2_0::GetLocalListVersionRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetLogResponse>>
        onGetLogReq(const ocpp2_0::GetLogRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetMonitoringReportResponse>>
        onGetMonitoringReportReq(const ocpp2_0::GetMonitoringReportRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetReportResponse>>
        onGetReportReq(const ocpp2_0::GetReportRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetTransactionStatusResponse>>
        onGetTransactionStatusReq(const ocpp2_0::GetTransactionStatusRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetVariablesResponse>>
        onGetVariablesReq(const ocpp2_0::GetVariablesRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::InstallCertificateResponse>>
        onInstallCertificateReq(const ocpp2_0::InstallCertificateRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::PublishFirmwareResponse>>
        onPublishFirmwareReq(const ocpp2_0::PublishFirmwareRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::RequestStartTransactionResponse>>
        onRequestStartTransactionReq(const ocpp2_0::RequestStartTransactionRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::RequestStopTransactionResponse>>
        onRequestStopTransactionReq(const ocpp2_0::RequestStopTransactionRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ReserveNowResponse>>
        onReserveNowReq(const ocpp2_0::ReserveNowRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ResetResponse>>
        onResetReq(const ocpp2_0::ResetRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SendLocalListResponse>>
        onSendLocalListReq(const ocpp2_0::SendLocalListRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetChargingProfileResponse>>
        onSetChargingProfileReq(const ocpp2_0::SetChargingProfileRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetDisplayMessageResponse>>
        onSetDisplayMessageReq(const ocpp2_0::SetDisplayMessageRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringBaseResponse>>
        onSetMonitoringBaseReq(const ocpp2_0::SetMonitoringBaseRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringLevelResponse>>
        onSetMonitoringLevelReq(const ocpp2_0::SetMonitoringLevelRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetNetworkProfileResponse>>
        onSetNetworkProfileReq(const ocpp2_0::SetNetworkProfileRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariableMonitoringResponse>>
        onSetVariableMonitoringReq(const ocpp2_0::SetVariableMonitoringRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariablesResponse>>
        onSetVariablesReq(const ocpp2_0::SetVariablesRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnlockConnectorResponse>>
        onUnlockConnectorReq(const ocpp2_0::UnlockConnectorRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnpublishFirmwareResponse>>
        onUnpublishFirmwareReq(const ocpp2_0::UnpublishFirmwareRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UpdateFirmwareResponse>>
        onUpdateFirmwareReq(const ocpp2_0::UpdateFirmwareRequest&) override;

    private:
        template <typename T>
        void runStepImpl(T&& sendBootNotification) {
            auto const now = system_interface_->steadyClockNow();
            if (connection_transition_timeout_.has_value()) {
                if (allow_ocpp_calls_) {
                    CHARGELAB_LOG_MESSAGE(debug) << "Connection transition completed - committing";
                    connection_transition_timeout_ = std::nullopt;
                    settings_->commit(SettingTransitionType::kConnection);
                } else if (now >= connection_transition_timeout_.value()) {
                    CHARGELAB_LOG_MESSAGE(debug) << "Connection transition failed - rolling back to previous settings";
                    settings_->rollback(SettingTransitionType::kConnection);
                    settings_->saveIfModified();
                    system_interface_->resetHard();
                }
            }

            // Note: only updating the security profile when first connecting to the back-end
            if (!security_profile_checked_ && allow_ocpp_calls_ && !settings_->hasRunningTransition(SettingTransitionType::kConnection)) {
                auto const activeNetworkProfileSlot = settings_->ActiveNetworkProfile.getValue();
                auto const activeNetworkProfile = settings_->NetworkConnectionProfiles.transitionCurrentValue(activeNetworkProfileSlot);
                if (activeNetworkProfile.has_value() &&
                    activeNetworkProfile->securityProfile > settings_->SecurityProfile.getValue()) {
                    // A05.FR.06
                    for (std::size_t i = 0; i < settings_->NetworkConnectionProfiles.transitionSize(); i++) {
                        auto const value = settings_->NetworkConnectionProfiles.transitionCurrentValue(i);
                        if (!value.has_value())
                            continue;
                        if (value->securityProfile >= activeNetworkProfile->securityProfile)
                            continue;

                        CHARGELAB_LOG_MESSAGE(info) << "Removing profile " << i << ": " << value;
                        settings_->NetworkConnectionProfiles.forceCurrentValue(i, std::nullopt);

                        char const* ifs = "";
                        std::string filtered_priorities;
                        string::SplitVisitor(settings_->NetworkConfigurationPriority.transitionCurrentValue(), ",", [&](std::string const& value) {
                            if (string::ToInteger(value) != std::make_optional(i)) {
                                filtered_priorities += ifs + value;
                                ifs = ",";
                            }
                        });

                        CHARGELAB_LOG_MESSAGE(info) << "Priorities before: " << settings_->NetworkConfigurationPriority.transitionCurrentValue();
                        settings_->NetworkConfigurationPriority.forceValue(filtered_priorities);
                        CHARGELAB_LOG_MESSAGE(info) << "Priorities after: " << settings_->NetworkConfigurationPriority.transitionCurrentValue();
                    }

                    settings_->SecurityProfile.setValue(activeNetworkProfile->securityProfile);
                    CHARGELAB_LOG_MESSAGE(info) << "Security profile updated to: " << activeNetworkProfile->securityProfile;
                }

                security_profile_checked_ = true;
            }

            if (force_boot_notification_req_) {
                force_boot_notification_req_ = false;
                pending_boot_notification_req_.setWithTimeout(
                        settings_->DefaultMessageTimeout.getValue(),
                        sendBootNotification()
                );
                return;
            }

            if (registrationComplete())
                return;

            if (requested_next_boot_notification_req_.has_value()) {
                if (now - requested_next_boot_notification_req_.value() < 0)
                    return;
            }

            if (pending_boot_notification_req_.wasIdleFor(settings_->HeartbeatInterval.getValue())) {
                pending_boot_notification_req_.setWithTimeout(
                        settings_->DefaultMessageTimeout.getValue(),
                        sendBootNotification()
                );
            }
        }

        std::optional<ocpp1_6::CallError> unauthorizedCallHandler1_6();
        std::optional<ocpp2_0::CallError> unauthorizedCallHandler2_0();
        static ocpp1_6::CallError unauthorizedError1_6();
        static ocpp2_0::CallError unauthorizedError2_0();

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_interface_;
        OperationHolder<std::string> pending_boot_notification_req_;

        std::optional<SteadyPointMillis> connection_transition_timeout_ = std::nullopt;
        std::optional<SteadyPointMillis> requested_next_boot_notification_req_ = std::nullopt;
        std::atomic<bool> registration_complete_ = false;
        std::atomic<bool> allow_ocpp_calls_ = false;
        std::atomic<bool> force_boot_notification_req_ = false;
        std::atomic<bool> security_profile_checked_ = false;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_BOOT_NOTIFICATION_MODULE_H

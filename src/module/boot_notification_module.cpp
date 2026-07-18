#include "openocpp/module/boot_notification_module.h"

namespace chargelab {

    BootNotificationModule::BootNotificationModule(
            std::shared_ptr<Settings> settings,
            std::shared_ptr<SystemInterface> const& system_interface
    )
        : settings_(std::move(settings)),
          system_interface_(system_interface),
          pending_boot_notification_req_ {system_interface}
    {
        assert(system_interface_ != nullptr);

        if (settings_->hasPendingTransition(SettingTransitionType::kConnection)) {
            auto timeout = system_interface_->steadyClockNow() + settings_->ConnectionTransitionTimeout.getValue()*1000;
            if (settings_->startTransition(SettingTransitionType::kConnection)) {
                CHARGELAB_LOG_MESSAGE(info) << "Starting connection transition";
                connection_transition_timeout_ = static_cast<SteadyPointMillis>(timeout);
            }
        }

        auto const reason_text = settings_->CustomBootReason.getValue();
        if (!reason_text.empty())
            CHARGELAB_LOG_MESSAGE(info) << "Custom boot reason was: " << settings_->CustomBootReason.getValue();
    }

    BootNotificationModule::~BootNotificationModule() {
        CHARGELAB_LOG_MESSAGE(debug) << "Deleting BootNotificationModule";
    }

    bool BootNotificationModule::registrationComplete() {
        return registration_complete_;
    }

    // OCPP 1.6 implementation
    void BootNotificationModule::runStep(ocpp1_6::OcppRemote &remote) {
        // Clear this flag if present
        settings_->CustomBootReason.setValue("");

        runStepImpl([&]() {
            // TODO: Improved validation/length safety (truncate?)
            ocpp1_6::BootNotificationReq req {};
            req.chargePointVendor = ocpp1_6::CiString20Type(settings_->ChargerVendor.getValue());
            req.chargePointModel = ocpp1_6::CiString20Type(settings_->ChargerModel.getValue());
            req.chargePointSerialNumber = ocpp1_6::CiString25Type(settings_->ChargerSerialNumber.getValue());
            req.firmwareVersion = ocpp1_6::CiString50Type(settings_->ChargerFirmwareVersion.getValue());
            if (auto iccid = settings_->ChargerICCID.getValue(); !iccid.empty())
                req.iccid = ocpp1_6::CiString20Type(iccid);
            if (auto imsi = settings_->ChargerIMSI.getValue(); !imsi.empty())
                req.imsi = ocpp1_6::CiString20Type(imsi);
            if (auto meter_serial_number = settings_->ChargerMeterSerialNumber.getValue(); !meter_serial_number.empty())
                req.meterSerialNumber = ocpp1_6::CiString25Type(meter_serial_number);
            if (auto meter_type = settings_->ChargerMeterType.getValue(); !meter_type.empty())
                req.meterType = ocpp1_6::CiString25Type(meter_type);

            return remote.sendBootNotificationReq(req);
        });
    }

    void BootNotificationModule::onBootNotificationRsp(
            const std::string &unique_id,
            const ocpp1_6::ResponseMessage<ocpp1_6::BootNotificationRsp> &rsp
    ) {
        if (pending_boot_notification_req_ == unique_id) {
            pending_boot_notification_req_ = kNoOperation;

            if (std::holds_alternative<ocpp1_6::BootNotificationRsp>(rsp)) {
                auto const& value = std::get<ocpp1_6::BootNotificationRsp>(rsp);
                auto const& ts = value.currentTime.getTimestamp();

                if (ts.has_value()) {
                    system_interface_->setSystemClock(ts.value());
                } else {
                    CHARGELAB_LOG_MESSAGE(warning) << "Invalid BootNotification response timestamp: "
                        << value.currentTime;
                }

                CHARGELAB_LOG_MESSAGE(info) << "BootNotification response was: " << value;
                switch (value.status) {
                    case ocpp1_6::RegistrationStatus::kAccepted:
                        requested_next_boot_notification_req_ = std::nullopt;
                        settings_->HeartbeatInterval.setValue(value.interval);
                        break;

                    case ocpp1_6::RegistrationStatus::kValueNotFoundInEnum:
                        CHARGELAB_LOG_MESSAGE(error) << "Invalid BootNotification response status for request ID: " << unique_id;
                        [[fallthrough]];

                    case ocpp1_6::RegistrationStatus::kPending:
                    case ocpp1_6::RegistrationStatus::kRejected:
                        requested_next_boot_notification_req_ = static_cast<SteadyPointMillis> (
                                system_interface_->steadyClockNow() + value.interval*1000
                        );
                        break;
                }
                switch (value.status) {
                    case ocpp1_6::RegistrationStatus::kRejected:
                    case ocpp1_6::RegistrationStatus::kValueNotFoundInEnum:
                        registration_complete_ = false;
                        allow_ocpp_calls_ = false;
                        break;

                    case ocpp1_6::RegistrationStatus::kPending:
                        registration_complete_ = false;
                        allow_ocpp_calls_ = true;
                        break;

                    case ocpp1_6::RegistrationStatus::kAccepted:
                        registration_complete_ = true;
                        allow_ocpp_calls_ = true;
                        break;
                }
            } else {
                CHARGELAB_LOG_MESSAGE(warning) << "Error response to BootNotification request: " << std::get<ocpp1_6::CallError> (rsp);
            }
        }
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
    BootNotificationModule::onTriggerMessageReq(const ocpp1_6::TriggerMessageReq &req) {
        switch (req.requestedMessage) {
            case ocpp1_6::MessageTrigger::kBootNotification:
                // Note: deviating from the 2.0.1 specification and allowing boot notification trigger messages
                //       after registration completed in 1.6 here.
                force_boot_notification_req_ = true;
                return ocpp1_6::TriggerMessageRsp{ocpp1_6::TriggerMessageStatus::kAccepted};

            default:
            case ocpp1_6::MessageTrigger::kValueNotFoundInEnum:
            case ocpp1_6::MessageTrigger::kDiagnosticsStatusNotification:
            case ocpp1_6::MessageTrigger::kFirmwareStatusNotification:
            case ocpp1_6::MessageTrigger::kHeartbeat:
            case ocpp1_6::MessageTrigger::kMeterValues:
            case ocpp1_6::MessageTrigger::kStatusNotification:
                if (!registrationComplete()) {
                    return unauthorizedError1_6();
                } else {
                    return std::nullopt;
                }
        }
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::RemoteStartTransactionRsp>>
    BootNotificationModule::onRemoteStartTransactionReq(const ocpp1_6::RemoteStartTransactionReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::RemoteStopTransactionRsp>>
    BootNotificationModule::onRemoteStopTransactionReq(const ocpp1_6::RemoteStopTransactionReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::CancelReservationRsp>>
    BootNotificationModule::onCancelReservationReq(const ocpp1_6::CancelReservationReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeAvailabilityRsp>>
    BootNotificationModule::onChangeAvailabilityReq(const ocpp1_6::ChangeAvailabilityReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeConfigurationRsp>>
    BootNotificationModule::onChangeConfigurationReq(const ocpp1_6::ChangeConfigurationReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearCacheRsp>>
    BootNotificationModule::onClearCacheReq(const ocpp1_6::ClearCacheReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearChargingProfileRsp>>
    BootNotificationModule::onClearChargingProfileReq(const ocpp1_6::ClearChargingProfileReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::DataTransferRsp>>
    BootNotificationModule::onDataTransferReq(const ocpp1_6::DataTransferReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetCompositeScheduleRsp>>
    BootNotificationModule::onGetCompositeScheduleReq(const ocpp1_6::GetCompositeScheduleReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetConfigurationRsp>>
    BootNotificationModule::onGetConfigurationReq(const ocpp1_6::GetConfigurationReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetDiagnosticsRsp>>
    BootNotificationModule::onGetDiagnosticsReq(const ocpp1_6::GetDiagnosticsReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetLocalListVersionRsp>>
    BootNotificationModule::onGetLocalListVersionReq(const ocpp1_6::GetLocalListVersionReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ReserveNowRsp>>
    BootNotificationModule::onReserveNowReq(const ocpp1_6::ReserveNowReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ResetRsp>>
    BootNotificationModule::onResetReq(const ocpp1_6::ResetReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SendLocalListRsp>>
    BootNotificationModule::onSendLocalListReq(const ocpp1_6::SendLocalListReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SetChargingProfileRsp>>
    BootNotificationModule::onSetChargingProfileReq(const ocpp1_6::SetChargingProfileReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UnlockConnectorRsp>>
    BootNotificationModule::onUnlockConnectorReq(const ocpp1_6::UnlockConnectorReq&) {
        return unauthorizedCallHandler1_6();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UpdateFirmwareRsp>>
    BootNotificationModule::onUpdateFirmwareReq(const ocpp1_6::UpdateFirmwareReq&) {
        return unauthorizedCallHandler1_6();
    }

    // OCPP 2.0.1 implementation
    void BootNotificationModule::runStep(ocpp2_0::OcppRemote &remote) {
        auto reason = ocpp2_0::BootReasonEnumType::kPowerUp;

        auto const reason_text = settings_->CustomBootReason.getValue();
        if (!reason_text.empty()) {
            reason = ocpp2_0::BootReasonEnumType::from_string(reason_text);
        } else if (force_boot_notification_req_) {
            reason = ocpp2_0::BootReasonEnumType::kTriggered;
        }

        runStepImpl([&]() {
            ocpp2_0::BootNotificationRequest req {};
            req.reason = reason;
            req.chargingStation.serialNumber = settings_->ChargerSerialNumber.getValue();
            req.chargingStation.model = settings_->ChargerModel.getValue();
            req.chargingStation.vendorName = settings_->ChargerVendor.getValue();
            req.chargingStation.firmwareVersion = settings_->ChargerFirmwareVersion.getValue();

            auto modem = ocpp2_0::ModemType {};
            if (auto iccid = settings_->ChargerICCID.getValue(); !iccid.empty())
                modem.iccid = iccid;
            if (auto imsi = settings_->ChargerIMSI.getValue(); !imsi.empty())
                modem.imsi = imsi;
            if (modem.iccid.has_value() || modem.imsi.has_value())
                req.chargingStation.modem = std::move(modem);

            return remote.sendBootNotificationReq(req);
        });
    }

    void BootNotificationModule::onBootNotificationRsp(
            const std::string &unique_id,
            const ocpp2_0::ResponseMessage<ocpp2_0::BootNotificationResponse> &rsp
    ) {
        if (pending_boot_notification_req_ == unique_id) {
            pending_boot_notification_req_ = kNoOperation;
            settings_->CustomBootReason.setValue("");

            if (std::holds_alternative<ocpp2_0::BootNotificationResponse>(rsp)) {
                auto const& value = std::get<ocpp2_0::BootNotificationResponse>(rsp);
                auto const& ts = value.currentTime.getTimestamp();

                if (ts.has_value()) {
                    system_interface_->setSystemClock(ts.value());
                } else {
                    CHARGELAB_LOG_MESSAGE(warning) << "Invalid BootNotification response timestamp: "
                                                   << value.currentTime;
                }

                CHARGELAB_LOG_MESSAGE(info) << "BootNotification response was: " << value;
                switch (value.status) {
                    case ocpp2_0::RegistrationStatusEnumType::kAccepted:
                        requested_next_boot_notification_req_ = std::nullopt;
                        settings_->HeartbeatInterval.setValue(value.interval);
                        break;

                    case ocpp2_0::RegistrationStatusEnumType::kValueNotFoundInEnum:
                        CHARGELAB_LOG_MESSAGE(error) << "Invalid BootNotification response status for request ID: " << unique_id;
                        [[fallthrough]];

                    case ocpp2_0::RegistrationStatusEnumType::kPending:
                    case ocpp2_0::RegistrationStatusEnumType::kRejected:
                        requested_next_boot_notification_req_ = static_cast<SteadyPointMillis> (
                                system_interface_->steadyClockNow() + value.interval*1000
                        );
                        break;
                }
                switch (value.status) {
                    case ocpp2_0::RegistrationStatusEnumType::kRejected:
                    case ocpp2_0::RegistrationStatusEnumType::kValueNotFoundInEnum:
                        registration_complete_ = false;
                        allow_ocpp_calls_ = false;
                        break;

                    case ocpp2_0::RegistrationStatusEnumType::kPending:
                        registration_complete_ = false;
                        allow_ocpp_calls_ = true;
                        break;

                    case ocpp2_0::RegistrationStatusEnumType::kAccepted:
                        registration_complete_ = true;
                        allow_ocpp_calls_ = true;
                        break;
                }
            } else {
                CHARGELAB_LOG_MESSAGE(warning) << "Error response to BootNotification request: " << std::get<ocpp2_0::CallError> (rsp);
            }
        }
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
    BootNotificationModule::onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest& req) {
        switch (req.requestedMessage) {
            case ocpp2_0::MessageTriggerEnumType::kBootNotification:
                if (!registration_complete_) {
                    force_boot_notification_req_ = true;
                    return ocpp2_0::TriggerMessageResponse{ocpp2_0::TriggerMessageStatusEnumType::kAccepted};
                } else {
                    // F06.FR.17
                    return ocpp2_0::TriggerMessageResponse{ocpp2_0::TriggerMessageStatusEnumType::kRejected};
                }

            default:
                return unauthorizedCallHandler2_0();
        }
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CancelReservationResponse>>
    BootNotificationModule::onCancelReservationReq(const ocpp2_0::CancelReservationRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CertificateSignedResponse>>
    BootNotificationModule::onCertificateSignedReq(const ocpp2_0::CertificateSignedRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ChangeAvailabilityResponse>>
    BootNotificationModule::onChangeAvailabilityReq(const ocpp2_0::ChangeAvailabilityRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearCacheResponse>>
    BootNotificationModule::onClearCacheReq(const ocpp2_0::ClearCacheRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearChargingProfileResponse>>
    BootNotificationModule::onClearChargingProfileReq(const ocpp2_0::ClearChargingProfileRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearDisplayMessageResponse>>
    BootNotificationModule::onClearDisplayMessageReq(const ocpp2_0::ClearDisplayMessageRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearVariableMonitoringResponse>>
    BootNotificationModule::onClearVariableMonitoringReq(const ocpp2_0::ClearVariableMonitoringRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CostUpdatedResponse>>
    BootNotificationModule::onCostUpdatedReq(const ocpp2_0::CostUpdatedRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CustomerInformationResponse>>
    BootNotificationModule::onCustomerInformationReq(const ocpp2_0::CustomerInformationRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::DataTransferResponse>>
    BootNotificationModule::onDataTransferReq(const ocpp2_0::DataTransferRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::DeleteCertificateResponse>>
    BootNotificationModule::onDeleteCertificateReq(const ocpp2_0::DeleteCertificateRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetBaseReportResponse>>
    BootNotificationModule::onGetBaseReportReq(const ocpp2_0::GetBaseReportRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetChargingProfilesResponse>>
    BootNotificationModule::onGetChargingProfilesReq(const ocpp2_0::GetChargingProfilesRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetCompositeScheduleResponse>>
    BootNotificationModule::onGetCompositeScheduleReq(const ocpp2_0::GetCompositeScheduleRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetDisplayMessagesResponse>>
    BootNotificationModule::onGetDisplayMessagesReq(const ocpp2_0::GetDisplayMessagesRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetInstalledCertificateIdsResponse>>
    BootNotificationModule::onGetInstalledCertificateIdsReq(const ocpp2_0::GetInstalledCertificateIdsRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetLocalListVersionResponse>>
    BootNotificationModule::onGetLocalListVersionReq(const ocpp2_0::GetLocalListVersionRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetLogResponse>>
    BootNotificationModule::onGetLogReq(const ocpp2_0::GetLogRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetMonitoringReportResponse>>
    BootNotificationModule::onGetMonitoringReportReq(const ocpp2_0::GetMonitoringReportRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetReportResponse>>
    BootNotificationModule::onGetReportReq(const ocpp2_0::GetReportRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetTransactionStatusResponse>>
    BootNotificationModule::onGetTransactionStatusReq(const ocpp2_0::GetTransactionStatusRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetVariablesResponse>>
    BootNotificationModule::onGetVariablesReq(const ocpp2_0::GetVariablesRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::InstallCertificateResponse>>
    BootNotificationModule::onInstallCertificateReq(const ocpp2_0::InstallCertificateRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::PublishFirmwareResponse>>
    BootNotificationModule::onPublishFirmwareReq(const ocpp2_0::PublishFirmwareRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::RequestStartTransactionResponse>>
    BootNotificationModule::onRequestStartTransactionReq(const ocpp2_0::RequestStartTransactionRequest&) {
        // B02.FR.05
        if (!registration_complete_) {
            return ocpp2_0::RequestStartTransactionResponse {ocpp2_0::RequestStartStopStatusEnumType::kRejected};
        }

        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::RequestStopTransactionResponse>>
    BootNotificationModule::onRequestStopTransactionReq(const ocpp2_0::RequestStopTransactionRequest&) {
        // B02.FR.05
        if (!registration_complete_) {
            return ocpp2_0::RequestStopTransactionResponse {ocpp2_0::RequestStartStopStatusEnumType::kRejected};
        }

        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ReserveNowResponse>>
    BootNotificationModule::onReserveNowReq(const ocpp2_0::ReserveNowRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ResetResponse>>
    BootNotificationModule::onResetReq(const ocpp2_0::ResetRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SendLocalListResponse>>
    BootNotificationModule::onSendLocalListReq(const ocpp2_0::SendLocalListRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetChargingProfileResponse>>
    BootNotificationModule::onSetChargingProfileReq(const ocpp2_0::SetChargingProfileRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetDisplayMessageResponse>>
    BootNotificationModule::onSetDisplayMessageReq(const ocpp2_0::SetDisplayMessageRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringBaseResponse>>
    BootNotificationModule::onSetMonitoringBaseReq(const ocpp2_0::SetMonitoringBaseRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringLevelResponse>>
    BootNotificationModule::onSetMonitoringLevelReq(const ocpp2_0::SetMonitoringLevelRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetNetworkProfileResponse>>
    BootNotificationModule::onSetNetworkProfileReq(const ocpp2_0::SetNetworkProfileRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariableMonitoringResponse>>
    BootNotificationModule::onSetVariableMonitoringReq(const ocpp2_0::SetVariableMonitoringRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariablesResponse>>
    BootNotificationModule::onSetVariablesReq(const ocpp2_0::SetVariablesRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnlockConnectorResponse>>
    BootNotificationModule::onUnlockConnectorReq(const ocpp2_0::UnlockConnectorRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnpublishFirmwareResponse>>
    BootNotificationModule::onUnpublishFirmwareReq(const ocpp2_0::UnpublishFirmwareRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UpdateFirmwareResponse>>
    BootNotificationModule::onUpdateFirmwareReq(const ocpp2_0::UpdateFirmwareRequest&) {
        return unauthorizedCallHandler2_0();
    }

    std::optional<ocpp1_6::CallError> BootNotificationModule::unauthorizedCallHandler1_6() {
        if (allow_ocpp_calls_) {
            return std::nullopt;
        } else {
            return unauthorizedError1_6();
        }
    }

    std::optional<ocpp2_0::CallError> BootNotificationModule::unauthorizedCallHandler2_0() {
        if (allow_ocpp_calls_) {
            return std::nullopt;
        } else {
            return unauthorizedError2_0();
        }
    }

    ocpp1_6::CallError BootNotificationModule::unauthorizedError1_6() {
        return ocpp1_6::CallError {
                ocpp1_6::ErrorCode::kSecurityError,
                "Waiting for CSMS to accept boot notification",
                common::RawJson::empty_object()
        };
    }

    ocpp2_0::CallError BootNotificationModule::unauthorizedError2_0() {
        return ocpp2_0::CallError {
            ocpp2_0::ErrorCode::kSecurityError,
            "Waiting for CSMS to accept boot notification",
            common::RawJson::empty_object()
        };
    }

} // namespace chargelab

#include "openocpp/module/fallback_module.h"

namespace chargelab {
    FallbackModule::FallbackModule(std::shared_ptr<SystemInterface> system)
        : system_(std::move(system))
    {
    }

    void FallbackModule::runStep(ocpp1_6::OcppRemote&) {
    }

    void FallbackModule::runStep(ocpp2_0::OcppRemote &remote) {
        if (pending_customer_report_.has_value()) {
            auto id = remote.sendNotifyCustomerInformationReq(ocpp2_0::NotifyCustomerInformationRequest {
                "",
                false,
                0,
                system_->systemClockNow(),
                pending_customer_report_->requestId
            });

            if (id.has_value()) {
                pending_customer_report_ = std::nullopt;
            }
        }
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
    FallbackModule::onTriggerMessageReq(const ocpp1_6::TriggerMessageReq&) {
        return ocpp1_6::TriggerMessageRsp {ocpp1_6::TriggerMessageStatus::kNotImplemented};
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
    FallbackModule::onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest&) {
        // F06.FR.08
        return ocpp2_0::TriggerMessageResponse {ocpp2_0::TriggerMessageStatusEnumType::kNotImplemented};
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::DataTransferRsp>>
    FallbackModule::onDataTransferReq(const ocpp1_6::DataTransferReq&) {
        return ocpp1_6::DataTransferRsp {ocpp1_6::DataTransferStatus::kUnknownVendorId};
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::DataTransferResponse>>
    FallbackModule::onDataTransferReq(const ocpp2_0::DataTransferRequest&) {
        // P01.FR.05
        return ocpp2_0::DataTransferResponse {ocpp2_0::DataTransferStatusEnumType::kUnknownVendorId};
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CustomerInformationResponse>>
    FallbackModule::onCustomerInformationReq(const ocpp2_0::CustomerInformationRequest& request) {
        // N10.FR.05
        if (pending_customer_report_.has_value())
            return ocpp2_0::CustomerInformationResponse {ocpp2_0::CustomerInformationStatusEnumType::kRejected};

        // N10.FR.07
        if (!request.clear && !request.report)
            return ocpp2_0::CustomerInformationResponse {ocpp2_0::CustomerInformationStatusEnumType::kRejected};

        // N10.FR.01
        pending_customer_report_ = request;
        return ocpp2_0::CustomerInformationResponse {ocpp2_0::CustomerInformationStatusEnumType::kAccepted};
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UnlockConnectorRsp>>
    FallbackModule::onUnlockConnectorReq(const ocpp1_6::UnlockConnectorReq&) {
        return ocpp1_6::UnlockConnectorRsp {ocpp1_6::UnlockStatus::kNotSupported};
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnlockConnectorResponse>>
    FallbackModule::onUnlockConnectorReq(const ocpp2_0::UnlockConnectorRequest&) {
        // Note - responding here to satisfy TC_E_16_CS, however a NotImplemented default response seems more
        // appropriate.
        return ocpp2_0::UnlockConnectorResponse {ocpp2_0::UnlockStatusEnumType::kUnlockFailed};
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearCacheRsp>>
    FallbackModule::onClearCacheReq(const ocpp1_6::ClearCacheReq&) {
        return ocpp1_6::ClearCacheRsp {ocpp1_6::ClearCacheStatus::kAccepted};
    }

    std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearCacheResponse>>
    FallbackModule::onClearCacheReq(const ocpp2_0::ClearCacheRequest&) {
        return ocpp2_0::ClearCacheResponse {ocpp2_0::ClearCacheStatusEnumType::kAccepted};
    }
}

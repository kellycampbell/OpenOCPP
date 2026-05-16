#ifndef CHARGELAB_OPEN_FIRMWARE_FALLBACK_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_FALLBACK_MODULE_H

#include "openocpp/module/common_templates.h"

namespace chargelab {
    class FallbackModule : public ServiceStatefulGeneral {
    public:
        explicit FallbackModule(std::shared_ptr<SystemInterface> system);

    private:
        void runStep(ocpp1_6::OcppRemote&) override;
        void runStep(ocpp2_0::OcppRemote &remote) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
        onTriggerMessageReq(const ocpp1_6::TriggerMessageReq&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
        onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::DataTransferRsp>>
        onDataTransferReq(const ocpp1_6::DataTransferReq&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::DataTransferResponse>>
        onDataTransferReq(const ocpp2_0::DataTransferRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::CustomerInformationResponse>>
        onCustomerInformationReq(const ocpp2_0::CustomerInformationRequest& request) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UnlockConnectorRsp>>
        onUnlockConnectorReq(const ocpp1_6::UnlockConnectorReq&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnlockConnectorResponse>>
        onUnlockConnectorReq(const ocpp2_0::UnlockConnectorRequest&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearCacheRsp>>
        onClearCacheReq(const ocpp1_6::ClearCacheReq&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearCacheResponse>>
        onClearCacheReq(const ocpp2_0::ClearCacheRequest&) override;

    private:
        std::shared_ptr<SystemInterface> system_;

        std::optional<ocpp2_0::CustomerInformationRequest> pending_customer_report_ = std::nullopt;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_FALLBACK_MODULE_H

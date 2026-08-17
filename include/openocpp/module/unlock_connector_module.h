#ifndef CHARGELAB_OPEN_FIRMWARE_UNLOCK_CONNECTOR_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_UNLOCK_CONNECTOR_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/module/connector_status_module.h"
#include "openocpp/interface/station_interface.h"

namespace chargelab {
    class UnlockConnectorModule : public ServiceStatefulGeneral {
    public:
        UnlockConnectorModule(
                std::shared_ptr<StationInterface> station,
                std::shared_ptr<ConnectorStatusModule> connector_status_module
        );

        ~UnlockConnectorModule() override;

    private:
        void runStep(ocpp1_6::OcppRemote&) override;
        void runStep(ocpp2_0::OcppRemote&) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UnlockConnectorRsp>>
        onUnlockConnectorReq(const ocpp1_6::UnlockConnectorReq &req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnlockConnectorResponse>>
        onUnlockConnectorReq(const ocpp2_0::UnlockConnectorRequest &req) override;

    private:
        std::shared_ptr<StationInterface> station_;
        std::shared_ptr<ConnectorStatusModule> connector_status_module_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_UNLOCK_CONNECTOR_MODULE_H

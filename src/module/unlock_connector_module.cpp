#include "openocpp/module/unlock_connector_module.h"

namespace chargelab {

UnlockConnectorModule::UnlockConnectorModule(
        std::shared_ptr<StationInterface> station,
        std::shared_ptr<ConnectorStatusModule> connector_status_module
)
    : station_(std::move(station)),
      connector_status_module_(std::move(connector_status_module))
{
}

UnlockConnectorModule::~UnlockConnectorModule() {
    CHARGELAB_LOG_MESSAGE(debug) << "Deleting UnlockConnectorModule";
}

void UnlockConnectorModule::runStep(ocpp1_6::OcppRemote&) {
}

void UnlockConnectorModule::runStep(ocpp2_0::OcppRemote&) {
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UnlockConnectorRsp>>
UnlockConnectorModule::onUnlockConnectorReq(const ocpp1_6::UnlockConnectorReq &req) {
    std::optional<ocpp2_0::EVSEType> evse;
    for (auto const& entry : station_->getConnectorMetadata()) {
        if (entry.second.connector_id1_6 == req.connectorId) {
            evse = entry.first;
            break;
        }
    }

    if (!evse.has_value()) {
        CHARGELAB_LOG_MESSAGE(warning) << "UnlockConnector: unknown connectorId " << req.connectorId;
        return ocpp1_6::UnlockConnectorRsp {ocpp1_6::UnlockStatus::kUnlockFailed};
    }

    switch (station_->unlockConnector(evse.value())) {
        case StationInterface::UnlockResult::kUnlocked:
            return ocpp1_6::UnlockConnectorRsp {ocpp1_6::UnlockStatus::kUnlocked};
        case StationInterface::UnlockResult::kNotSupported:
            return ocpp1_6::UnlockConnectorRsp {ocpp1_6::UnlockStatus::kNotSupported};
        case StationInterface::UnlockResult::kUnlockFailed:
        default:
            return ocpp1_6::UnlockConnectorRsp {ocpp1_6::UnlockStatus::kUnlockFailed};
    }
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UnlockConnectorResponse>>
UnlockConnectorModule::onUnlockConnectorReq(const ocpp2_0::UnlockConnectorRequest &req) {
    ocpp2_0::EVSEType const evse {req.evseId, req.connectorId};

    bool found = false;
    for (auto const& entry : station_->getConnectorMetadata()) {
        if (entry.first.id == evse.id && entry.first.connectorId == evse.connectorId) {
            found = true;
            break;
        }
    }

    if (!found) {
        CHARGELAB_LOG_MESSAGE(warning) << "UnlockConnector: unknown evseId/connectorId "
                                        << req.evseId << "/" << req.connectorId;
        return ocpp2_0::UnlockConnectorResponse {ocpp2_0::UnlockStatusEnumType::kUnknownConnector};
    }

    // B12.FR.01-style guard: refuse to unlock out from under a running transaction rather than
    // silently interrupting the driver's charging session.
    if (connector_status_module_->isChargingEnabled()) {
        return ocpp2_0::UnlockConnectorResponse {ocpp2_0::UnlockStatusEnumType::kOngoingAuthorizedTransaction};
    }

    switch (station_->unlockConnector(evse)) {
        case StationInterface::UnlockResult::kUnlocked:
            return ocpp2_0::UnlockConnectorResponse {ocpp2_0::UnlockStatusEnumType::kUnlocked};
        case StationInterface::UnlockResult::kNotSupported:
        case StationInterface::UnlockResult::kUnlockFailed:
        default:
            return ocpp2_0::UnlockConnectorResponse {ocpp2_0::UnlockStatusEnumType::kUnlockFailed};
    }
}

} // namespace chargelab

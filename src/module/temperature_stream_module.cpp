#include "openocpp/module/temperature_stream_module.h"
#include "openocpp/protocol/common/protocol_constants.h"
#include "openocpp/protocol/ocpp2_1/messages/notify_periodic_event_stream.h"
#include "openocpp/helpers/string.h"

#include <cstdio>
#include <string>

namespace chargelab {
namespace {
    std::string formatCelsius(double value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        return std::string(buffer);
    }
}

TemperatureStreamModule::TemperatureStreamModule(
        std::shared_ptr<Settings> settings,
        std::shared_ptr<SystemInterface> system,
        std::shared_ptr<StationInterface> station
)
    : settings_(std::move(settings)),
      system_(std::move(system)),
      station_(std::move(station))
{
}

TemperatureStreamModule::~TemperatureStreamModule() {
    CHARGELAB_LOG_MESSAGE(debug) << "Deleting TemperatureStreamModule";
}

void TemperatureStreamModule::runStep(ocpp2_0::OcppRemote& remote) {
    // NotifyPeriodicEventStream is an OCPP 2.1 SEND message; only stream when the negotiated
    // websocket subprotocol is ocpp2.1, otherwise it would be sent to a server that can't handle it.
    auto const subprotocol = remote.getSubprotocol();
    if (!subprotocol.has_value() || !string::EqualsIgnoreCaseAscii(subprotocol.value(), ProtocolConstants::kProtocolOcpp2_1)) {
        return;
    }

    // Only stream temperatures while a transaction is active; the flag is driven by
    // onTransactionUpdate below.
    if (!transaction_active_) {
        return;
    }

    auto const now = system_->steadyClockNow();
    if (last_sent_.has_value()) {
        auto const elapsed_seconds = ((std::int64_t)now - (std::int64_t)last_sent_.value()) / 1000;
        if (elapsed_seconds < kReportIntervalSeconds)
            return;
    }

    // Gate the next attempt on the interval regardless of outcome, so we don't busy-retry
    // while disconnected or waiting for registration to complete.
    last_sent_ = now;

    auto const temperatures = station_->getTemperatures();
    if (temperatures.empty())
        return;

    // Build the two-row CSV value: header row of names, then the row of Celsius values.
    std::string names;
    std::string values;
    for (std::size_t i = 0; i < temperatures.size(); i++) {
        if (i > 0) {
            names += ",";
            values += ",";
        }
        names += temperatures[i].location;
        values += formatCelsius(temperatures[i].celsius);
    }

    ocpp2_1::NotifyPeriodicEventStreamRequest req {};
    req.data.push_back(ocpp2_1::StreamDataElementType {0.0, names + "\n" + values});
    req.id = kStreamId;
    req.pending = 0;
    req.basetime = ocpp2_1::DateTime {system_->systemClockNow()};

    remote.sendSend(req);
}

void TemperatureStreamModule::onTransactionUpdate(
        Status status,
        std::optional<ocpp2_0::EVSEType> const& evse,
        transaction_module2_0::TransactionContainer const& transaction,
        std::optional<charger::ConnectorStatus> const& connector_status,
        std::optional<std::vector<ocpp2_0::SampledValueType>> const& sampled_values
) {
    (void)evse;
    (void)transaction;
    (void)connector_status;
    (void)sampled_values;

    bool const active = status != TransactionListener2_0::Status::kStopped;
    if (active && !transaction_active_) {
        // Stream on the next runStep rather than waiting out any remaining interval from a
        // previous transaction.
        last_sent_ = std::nullopt;
    }
    transaction_active_ = active;
}

} // namespace chargelab

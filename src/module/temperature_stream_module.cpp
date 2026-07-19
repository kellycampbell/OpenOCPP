#include "openocpp/module/temperature_stream_module.h"
#include "openocpp/protocol/ocpp2_1/messages/notify_periodic_event_stream.h"

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

} // namespace chargelab

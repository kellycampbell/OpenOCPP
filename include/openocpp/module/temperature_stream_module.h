#ifndef CHARGELAB_OPEN_FIRMWARE_TEMPERATURE_STREAM_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_TEMPERATURE_STREAM_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/interface/station_interface.h"
#include "openocpp/interface/transaction_listener2_0.h"
#include "openocpp/common/settings.h"

#include <memory>
#include <optional>

namespace chargelab {
    // Periodically streams the station's temperature readings to the CSMS using the OCPP 2.1
    // NotifyPeriodicEventStream message, sent as a fire-and-forget SEND (message type 6).
    //
    // All temperatures are packed into a single stream. Because OCPP 2.1 has no standard way to
    // label the columns of a stream, each message's value string is a two-row CSV: the first row
    // is the comma-separated sensor names (header) and the second row is the comma-separated
    // Celsius values.
    //
    // Streaming is gated on an active transaction: the module implements TransactionListener2_0
    // so it can observe transaction start/stop and only emit events while a transaction is running.
    class TemperatureStreamModule : public ServiceStateful2_0, public TransactionListener2_0 {
    public:
        TemperatureStreamModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> system,
                std::shared_ptr<StationInterface> station
        );

        ~TemperatureStreamModule() override;

    public:
        void runStep(ocpp2_0::OcppRemote& remote) override;

        void onTransactionUpdate(
                Status status,
                std::optional<ocpp2_0::EVSEType> const& evse,
                transaction_module2_0::TransactionContainer const& transaction,
                std::optional<charger::ConnectorStatus> const& connector_status,
                std::optional<std::vector<ocpp2_0::SampledValueType>> const& sampled_values
        ) override;

    private:
        static constexpr int kStreamId = 1;
        static constexpr int kReportIntervalSeconds = 60;

        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_;
        std::shared_ptr<StationInterface> station_;
        std::optional<SteadyPointMillis> last_sent_ = std::nullopt;
        bool transaction_active_ = false;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_TEMPERATURE_STREAM_MODULE_H

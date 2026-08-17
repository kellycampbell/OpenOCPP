#ifndef CHARGELAB_OPEN_FIRMWARE_STATION_INTERFACE_H
#define CHARGELAB_OPEN_FIRMWARE_STATION_INTERFACE_H

#include "openocpp/protocol/ocpp1_6/types/charge_point_error_code.h"
#include "openocpp/protocol/ocpp1_6/types/ci_string_type.h"
#include "openocpp/protocol/ocpp1_6/types/sampled_value.h"
#include "openocpp/protocol/ocpp1_6/types/charging_schedule_period.h"
#include "openocpp/protocol/ocpp1_6/messages/set_charging_profile.h"
#include "openocpp/protocol/ocpp2_0/types/sampled_value_type.h"
#include "openocpp/protocol/ocpp2_0/types/evse_type.h"
#include "openocpp/protocol/ocpp2_0/messages/set_charging_profile.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/helpers/json.h"

#include <string>
#include <vector>
#include <optional>

namespace chargelab {
    namespace charger {
        // TODO: Move these into nested classes under StationInterface?
        struct FaultedStatus1_6 {
            ocpp1_6::ChargePointErrorCode errorCode = ocpp1_6::ChargePointErrorCode::kValueNotFoundInEnum;
            std::optional<ocpp1_6::CiString50Type> info;
            std::optional<ocpp1_6::CiString255Type> vendorId;
            std::optional<ocpp1_6::CiString50Type> vendorErrorCode;
            CHARGELAB_JSON_INTRUSIVE(FaultedStatus1_6, errorCode, info, vendorId, vendorErrorCode)
        };

        struct FaultedStatus2_0 {
            // TODO
            CHARGELAB_JSON_INTRUSIVE_EMPTY(FaultedStatus2_0)
        };

        struct FaultedStatus {
            FaultedStatus1_6 status1_6;
            FaultedStatus2_0 status2_0;
            CHARGELAB_JSON_INTRUSIVE(FaultedStatus, status1_6, status2_0)
        };

        // TODO: Need to add type information and flags to enable an implementation to enable/disable connectors (if,
        //  for example, multiple physical connectors are present but only one can be used at a time).
        struct ConnectorStatus {
            bool connector_available;
            bool vehicle_connected;
            bool charging_enabled;
            bool suspended_by_vehicle;
            bool suspended_by_charger;
            double meter_watt_hours;
            std::optional<FaultedStatus> faulted_status = std::nullopt;
            CHARGELAB_JSON_INTRUSIVE(ConnectorStatus, connector_available, vehicle_connected, charging_enabled, suspended_by_vehicle,
                                     suspended_by_charger, faulted_status)
        };

        struct ConnectorMetadata {
            /**
             * OCPP 1.6 connector ID
             */
            int connector_id1_6;
            std::string connector_type;
            int supply_phases;
            double power_max_watts;
            double power_max_amps;
            CHARGELAB_JSON_INTRUSIVE(ConnectorMetadata, connector_id1_6, connector_type, supply_phases, power_max_watts, power_max_amps)
        };

        struct EvseMetadata {
            int supply_phases;
            double power_max_watts;
            CHARGELAB_JSON_INTRUSIVE(EvseMetadata, supply_phases, power_max_watts)
        };

        struct StationMetadata {
            int supply_phases;
            CHARGELAB_JSON_INTRUSIVE(StationMetadata, supply_phases)
        };

        // A single named temperature reading, in degrees Celsius. 'valid' is false when the
        // sensor is not installed/readable.
        struct TemperatureReading {
            std::string location;
            double celsius;
            bool valid;
            CHARGELAB_JSON_INTRUSIVE(TemperatureReading, location, celsius, valid)
        };
    }

    class StationInterface {
    public:
        using schedule_type1_6 = std::pair<ocpp1_6::SetChargingProfileReq, ocpp1_6::ChargingSchedulePeriod>;
        using schedule_type2_0 = std::pair<ocpp2_0::SetChargingProfileRequest, ocpp2_0::ChargingSchedulePeriodType>;

        enum class Result {
            kSucceeded,
            kFailed,

            // Permanent failure - the firmware image failed verification (e.g. it targets
            // different hardware). The update should not be retried.
            kVerificationFailed
        };

        enum class UpdatePhase {
            kDownloading,
            kInstalling
        };

    public:
        virtual ~StationInterface() = default;

        /**
         * Returns the station metadata required by OCPP 2.0.1.
         *
         * @return station metadata
         */
        virtual charger::StationMetadata getStationMetadata() = 0;

        /**
         * Returns the EVSE metadata required by OCPP 2.0.1. The connectorId field should not be set for all entries in
         * the returned map.
         *
         * @return EVSE metadata
         */
        virtual std::map<ocpp2_0::EVSEType, charger::EvseMetadata> getEvseMetadata() = 0;

        /**
         * Returns the connector metadata containing the OCPP 1.6 connectorId mappings and the information required by
         * OCPP 2.0.1. The connectorId field should be set for all entries in the returned map.
         *
         * @return connector metadata
         */
        virtual std::map<ocpp2_0::EVSEType, charger::ConnectorMetadata> getConnectorMetadata() = 0;

        /**
         * Polls the status of a particular connector. Note that this call is expected to be provided with a connector
         * ID - when absent this call should always return an empty optional.
         *
         * @param evse
         * @return a non-empty optional with the connector status if the connector exists, otherwise an empty optional
         */
        virtual std::optional<charger::ConnectorStatus> pollConnectorStatus(ocpp2_0::EVSEType const& evse) = 0;

        /**
         * Polls the meter values for the station, EVSE, or connector if they're available.
         *
         * @param evse an empty optional for station meter values, an EVSE with no connector for EVSE meter values, and
         *             an EVSE/connector for connector meter values.
         * @return the meter values data if available, otherwise an empty vector
         */
        virtual std::vector<ocpp1_6::SampledValue> pollMeterValues1_6(std::optional<ocpp2_0::EVSEType> const& evse) = 0;

        /**
         * Polls the meter values for the station, EVSE, or connector if they're available.
         *
         * @param evse an empty optional for station meter values, an EVSE with no connector for EVSE meter values, and
         *             an EVSE/connector for connector meter values.
         * @return the meter values data if available, otherwise an empty vector
         */
        virtual std::vector<ocpp2_0::SampledValueType> pollMeterValues2_0(std::optional<ocpp2_0::EVSEType> const& evse) = 0;

        /**
         * Returns the station's current named temperature readings, in degrees Celsius. Used to stream
         * temperature telemetry via OCPP 2.1 periodic event streams. Default is an empty list.
         *
         * @return the current temperature readings, or an empty vector if none are available
         */
        virtual std::vector<charger::TemperatureReading> getTemperatures() { return {}; }

        /**
         * Enables/disables charging on a particular connector.
         *
         * @param evse
         * @param value true to enable charging, otherwise false
         */
        virtual void setChargingEnabled(ocpp2_0::EVSEType const& evse, bool value) = 0;

        // Note: providing the charger with the active profile and period explicitly for the following reasons:
        // - If a TxProfile limits based on Amps and a ChargePointMaxProfile limits based on Watts the interpretation
        //   will need to be left to the vendor. Do they apply the limit based on the maximum voltage or a live reading?
        //   If the reading fluctuates does the implementation switch from one limit to another?
        // - A vendor *may* want to communicate more details to a user. For example, they may want to display the
        //   current profile limits (or other associated metadata) on screen.
        // This method will be called whenever the active schedule changes.

        /**
         * Sets the active OCPP 1.6 charging schedules that were assigned to the charger.
         *
         * @param active_schedules the active schedule periods and their associated charging profiles
         */
        virtual void setActiveChargePointMaxProfiles(std::vector<schedule_type1_6> const& active_schedules) = 0;

        /**
         * Sets the active OCPP 2.0 charging schedules that were assigned to the charger.
         *
         * @param active_schedules the active schedule periods and their associated charging profiles
         */
        virtual void setActiveChargePointMaxProfiles(std::vector<schedule_type2_0> const& active_schedules) = 0;

        /**
         * Sets the active OCPP 1.6 charging schedules that were assigned to a specific EVSE ID.
         *
         * @param active_schedules the active schedule periods and their associated charging profiles
         */
        virtual void setActiveEvseProfiles(int evse_id, std::vector<schedule_type1_6> const& active_schedules) = 0;

        /**
         * Sets the active OCPP 2.0 charging schedules that were assigned to a specific EVSE ID.
         *
         * @param active_schedules the active schedule periods and their associated charging profiles
         */
        virtual void setActiveEvseProfiles(int evse_id, std::vector<schedule_type2_0> const& active_schedules) = 0;

        /**
         * Reads an OCPP 1.6 ID token that was presented to the station. This method may either return the token once
         * every interaction, or it may return the IdToken for as long as the RFID tag is detected by the RFID reader.
         * If the latter approach is used the running transaction module is expected to only respond to changes in
         * output.
         *
         * @return the IdToken value associated with an RFID (or other local) interaction
         */
        [[nodiscard]] virtual std::optional<ocpp1_6::IdToken> readToken1_6() = 0;

        /**
         * Returns an OCPP 2.0 ID token that was presented to the station. This method may either return the token once
         * every interaction, or it may return the IdToken for as long as the RFID tag is detected by the RFID reader.
         * If the latter approach is used the running transaction module is expected to only respond to changes in
         * output.
         *
         * @return
         */
        [[nodiscard]] virtual std::optional<ocpp2_0::IdTokenType> readToken2_0() = 0;

        /**
         * Result of a physical connector unlock attempt, requested remotely via the OCPP UnlockConnector
         * operation (e.g. to release a stuck cable after a session ends).
         */
        enum class UnlockResult {
            kUnlocked,
            kUnlockFailed,
            kNotSupported
        };

        /**
         * Attempts to physically unlock the given connector. Default implementation reports the operation as
         * unsupported; stations with a connector lock actuator should override this.
         *
         * @param evse the EVSE/connector to unlock
         * @return the outcome of the unlock attempt
         */
        virtual UnlockResult unlockConnector(ocpp2_0::EVSEType const& evse) {
            (void)evse;
            return UnlockResult::kNotSupported;
        }

        /**
         * Gets the active firmware slot (generally some kind of partition ID). This is compared to the update slot to
         * determine whether or not the firmware update succeeded.
         *
         * @return active slot ID
         */
        virtual std::string getActiveSlotId() = 0;

        /**
         * Starts a firmware update operation. Must be paired with a terminating finishUpdateProcess call on success or
         * failure.
         *
         * @param update_size
         * @return kSucceeded on success, otherwise kFailed
         */
        virtual Result startUpdateProcess(std::size_t update_size) = 0;

        /**
         * Process an incoming block of data as part of a firmware update, generally writing that block to the update
         * partition. May only be called during a firmware update operation (after a call to startUpdateProcess but
         * before the associated call to finishUpdateProcess).
         *
         * @param block
         * @param size
         * @return kSucceeded on success, otherwise kFailed
         */
        virtual Result processFirmwareChunk(std::uint8_t const* block, std::size_t size) = 0;

        /**
         * Gets the update slot ID (generally some kind of partition ID). This is compared against the active slot ID
         * after an update completes to determine whether or not the firmware update succeeded. May only be called
         * during a firmware update operation (after a call to startUpdateProcess but before the associated call to
         * finishUpdateProcess).
         *
         * @return update slot ID
         */
        virtual std::string getUpdateSlotId() = 0;

        /**
         * Finishes a firmware update. If succeeded == true and this call returns kSucceeded then the next restart is
         * expected to attempt to boot into the updated firmware. If succeeded == false or this call returns kFailed
         * then the next restart is expected to boot into the active firmware slot. A return value of kFailed when
         * provided with a succeeded == false flag is treated as an error state and the next restart is expected to boot
         * into the active firmware slot;
         *
         * @param succeeded
         * @return kSucceeded on success, otherwise kFailed
         */
        virtual Result finishUpdateProcess(bool succeeded) = 0;

        /**
         * Notifies the station of firmware update download progress so it can drive a local
         * display (e.g. LEDs). Called by the firmware update module during phases that don't
         * otherwise touch the station interface, such as the OCPP 2.0 download/signature pass.
         * Default is a no-op.
         *
         * @param phase the current update phase
         * @param bytes bytes processed so far in this phase
         * @param total total bytes expected in this phase; may be 0 if unknown
         */
        virtual void notifyUpdateProgress(UpdatePhase phase, std::size_t bytes, std::size_t total) {}

        /**
         * Notifies the station of a firmware update status change, so it can keep a local
         * record of an update the CSMS drove. Covers the parts of an update the rest of this
         * interface never sees - a download that never connects, a server returning 404, the
         * retry backoff - which would otherwise leave no trace once the station reboots.
         * Default is a no-op.
         *
         * @param status the OCPP firmware status being reported, as a string
         * @param detail context that doesn't fit the OCPP status (a server status code, how
         *               long until the next attempt); may be empty
         * @param is_failure true if the status reports the update going wrong
         */
        virtual void notifyUpdateStatus(std::string const& status, std::string const& detail, bool is_failure) {}

    public:
        /**
         * Helper method to lookup an OCPP 1.6 connector ID based on the provided metadata.
         *
         * @param id an OCPP 1.6 connector ID
         * @return the associated EVSE if the connector was found, otherwise std::nullopt
         */
        virtual std::optional<ocpp2_0::EVSEType> lookupConnectorId1_6(int id) {
            for (auto const& entry : getConnectorMetadata()) {
                if (entry.second.connector_id1_6 == id)
                    return entry.first;
            }

            return std::nullopt;
        }
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_STATION_INTERFACE_H

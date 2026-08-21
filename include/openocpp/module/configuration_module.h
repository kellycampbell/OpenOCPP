#ifndef CHARGELAB_OPEN_FIRMWARE_CONFIGURATION_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_CONFIGURATION_MODULE_H

#include "openocpp/protocol/common/protocol_constants.h"
#include "openocpp/protocol/ocpp2_0/types/event_data_type.h"
#include "openocpp/module/common_templates.h"
#include "openocpp/common/settings.h"
#include "openocpp/helpers/string.h"
#include "openocpp/interface/component/system_interface.h"

#include <cmath>
#include <utility>
#include <functional>

namespace chargelab {
    class ConfigurationModule : public ServiceStatefulGeneral {
    private:
        static constexpr char const* kMaskedValue = "****";
        static constexpr int kOcpp20NotifyReportRequestOverheadBytes = 100;

    public:
        explicit ConfigurationModule(std::shared_ptr<Settings> settings, std::shared_ptr<SystemInterface> system);
        ~ConfigurationModule() override;

    public:
        void runUnconditionally() override;
        void runStep(ocpp1_6::OcppRemote&) override;
        void runStep(ocpp2_0::OcppRemote &remote) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetVariablesResponse>>
        onGetVariablesReq(const ocpp2_0::GetVariablesRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariablesResponse>>
        onSetVariablesReq(const ocpp2_0::SetVariablesRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetBaseReportResponse>>
        onGetBaseReportReq(const ocpp2_0::GetBaseReportRequest &request) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeConfigurationRsp>>
        onChangeConfigurationReq(const ocpp1_6::ChangeConfigurationReq& req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeConfigurationRsp>>
        changeConfiguration(const ocpp1_6::ChangeConfigurationReq& req, bool force_change);

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetConfigurationRsp>>
        onGetConfigurationReq(const ocpp1_6::GetConfigurationReq &req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetConfigurationRsp>>
        getConfiguration(const ocpp1_6::GetConfigurationReq &req);

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetNetworkProfileResponse>>
        onSetNetworkProfileReq(const ocpp2_0::SetNetworkProfileRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetMonitoringReportResponse>>
        onGetMonitoringReportReq(const ocpp2_0::GetMonitoringReportRequest&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariableMonitoringResponse>>
        onSetVariableMonitoringReq(const ocpp2_0::SetVariableMonitoringRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearVariableMonitoringResponse>>
        onClearVariableMonitoringReq(const ocpp2_0::ClearVariableMonitoringRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringBaseResponse>>
        onSetMonitoringBaseReq(const ocpp2_0::SetMonitoringBaseRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringLevelResponse>>
        onSetMonitoringLevelReq(const ocpp2_0::SetMonitoringLevelRequest &request) override;

    private:
        // A single variable monitor as tracked by this charging station (B08). Not yet persisted across
        // reboots - see the TODO on monitors_ below.
        struct MonitorRecord {
            int id {};
            ocpp2_0::ComponentType component {};
            ocpp2_0::VariableType variable {};
            bool transaction {};
            double value {};
            ocpp2_0::MonitorEnumType type {};
            int severity {};

            // Evaluation-only runtime state - not part of the wire type, reset whenever the monitor is
            // (re)created via SetVariableMonitoring.
            bool threshold_active = false;      // For UpperThreshold/LowerThreshold: currently past the threshold.
            SteadyPointMillis threshold_debounce_until {}; // Earliest time threshold_active is allowed to flip again.
            bool has_last_reported_value = false;
            double last_reported_value = 0;     // For numeric Delta: value as of the last report.
            std::string last_reported_text {};  // For non-numeric Delta: raw text as of the last report.
            SteadyPointMillis next_periodic_report {}; // For Periodic/PeriodicClockAligned.
        };

        // A monitored variable's current value, as both the raw OCPP-formatted string (always available, used
        // verbatim as EventDataType::actualValue) and - for numeric variable types - the parsed double used by
        // UpperThreshold/LowerThreshold/numeric-Delta comparisons.
        struct MonitoredValue {
            std::string text;
            std::optional<double> numeric;
        };

    private:
        void evaluateMonitors();
        std::optional<MonitoredValue> readMonitoredValue(MonitorRecord const& monitor) const;
        void raiseMonitorEvent(
                MonitorRecord const& monitor,
                std::string const& actual_value,
                bool cleared
        );

        // Hysteresis/debounce guard against event spam when a value chatters right around a threshold: once a
        // threshold monitor flips, it must clear back past the threshold by this fraction (of the threshold
        // value) before it's allowed to flip again, and flips are further rate-limited to at most one per
        // kThresholdDebounceMillis regardless of how fast the value is chattering.
        static constexpr double kThresholdHysteresisFraction = 0.02;
        static constexpr double kThresholdMinHysteresis = 1e-6;
        static constexpr std::int64_t kThresholdDebounceMillis = 1000;

    private:
        template<int N>
        static bool ciEquals(ocpp2_0::IdentifierStringPrimitive<N> const& lhs, ocpp2_0::IdentifierStringPrimitive<N> const& rhs) {
            return string::EqualsIgnoreCaseAscii(lhs.value(), rhs.value());
        }

        template<int N>
        static bool ciEquals(std::optional<ocpp2_0::IdentifierStringPrimitive<N>> const& lhs, std::optional<ocpp2_0::IdentifierStringPrimitive<N>> const& rhs) {
            if (lhs.has_value() != rhs.has_value())
                return false;
            if (!lhs.has_value())
                return true;

            return ciEquals(lhs.value(), rhs.value());
        }

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_;

        std::optional<ocpp2_0::GetBaseReportRequest> ocpp2_0_pending_base_report_ = std::nullopt;
        std::optional<ocpp2_0::GetMonitoringReportRequest> ocpp2_0_pending_monitoring_report_ = std::nullopt;

        // TODO: Persist across reboots (e.g. as a delimited-list Setting, similar to NetworkConnectionProfiles)
        // once the monitor evaluation/NotifyEvent pipeline exists to make that worthwhile.
        std::vector<MonitorRecord> monitors_ {};
        int next_monitor_id_ = 1;
        ocpp2_0::MonitoringBaseEnumType monitoring_base_ = ocpp2_0::MonitoringBaseEnumType::kAll;
        int monitoring_level_ = 9;

        std::vector<ocpp2_0::EventDataType> pending_events_ {};
        int next_event_id_ = 1;

        static constexpr std::size_t kMaxVariableMonitors = 64;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_CONFIGURATION_MODULE_H

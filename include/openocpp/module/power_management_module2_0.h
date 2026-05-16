#ifndef CHARGELAB_OPEN_FIRMWARE_POWER_MANAGEMENT_MODULE2_0_H
#define CHARGELAB_OPEN_FIRMWARE_POWER_MANAGEMENT_MODULE2_0_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/interface/station_interface.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/macro.h"
#include "openocpp/common/compressed_journal.h"
#include "openocpp/helpers/set.h"
#include "openocpp/helpers/container.h"

namespace chargelab {
    class TransactionModule2_0;

    namespace detail {
        struct JournalUpdate {
            std::optional<ocpp2_0::SetChargingProfileRequest> setChargingProfileRequest = std::nullopt;
            std::optional<ocpp2_0::ClearChargingProfileRequest> clearChargingProfileRequest = std::nullopt;
            CHARGELAB_JSON_INTRUSIVE(JournalUpdate, setChargingProfileRequest, clearChargingProfileRequest)
        };

        struct ActiveSchedulePeriod {
            std::optional<ocpp2_0::ChargingSchedulePeriodType> period;
            SystemTimeMillis next_update;
        };

        struct ReportCharingProfileState {
            ocpp2_0::ChargingLimitSourceEnumType current_source = ocpp2_0::ChargingLimitSourceEnumType::kEMS;
            int current_evseId = 0;
        };
    }

    // Note: this module treats connector 0 as a separate physical connector. Profiles assigned to connector ID 0 are
    // passed as a schedule to connector 0, not to each physical connector. This is intended to allow the station to
    // define a suitable load balancing mechanism when more than one connector is present.
    class PowerManagementModule2_0 : public ServiceStateful2_0 {
        friend class TransactionModule2_0;
    public:
        PowerManagementModule2_0(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> system,
                std::shared_ptr<StationInterface> station,
                std::unique_ptr<FlashBlockInterface> storage
        );

        ~PowerManagementModule2_0() override;

    private:
        void onActiveTransactionStarted(int evse_id, std::optional<ocpp2_0::ChargingProfileType> transaction_profile, SteadyPointMillis start_ts);
        void onActiveTransactionIdAssigned(int evse_id, std::string const& transaction_id);
        void onActiveTransactionFinished(int evse_id);

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetChargingProfileResponse>>
        onSetChargingProfileReq(const ocpp2_0::SetChargingProfileRequest &req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearChargingProfileResponse>>
        onClearChargingProfileReq(const ocpp2_0::ClearChargingProfileRequest& req) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetChargingProfilesResponse>>
        onGetChargingProfilesReq(const ocpp2_0::GetChargingProfilesRequest &request) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetCompositeScheduleResponse>>
        onGetCompositeScheduleReq(const ocpp2_0::GetCompositeScheduleRequest& req) override;

        void runStep(ocpp2_0::OcppRemote& remote) override;

    private:
        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetChargingProfileResponse>>
        onSetChargingProfileReqInternal(const ocpp2_0::SetChargingProfileRequest &req, bool persist_to_journal);

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearChargingProfileResponse>>
        onClearChargingProfileReqInternal(const ocpp2_0::ClearChargingProfileRequest& req, bool persist_to_journal);

        void reportJournalCapacity();
        void sendChargingProfilesReport(ocpp2_0::OcppRemote& remote);
        SystemTimeMillis updateActiveChargingProfiles(SystemTimeMillis now);

        std::vector<StationInterface::schedule_type2_0> getActiveChargingProfiles(
                int evse_id, SystemTimeMillis now, SystemTimeMillis& next_update);

        detail::ActiveSchedulePeriod getActiveSchedulePeriod(
                ocpp2_0::SetChargingProfileRequest const& profile,
                SystemTimeMillis const now,
                std::optional<SystemTimeMillis> const& tx_power_path_closed_ts);

        void sortChargingProfiles();
        std::vector<detail::JournalUpdate> chargingProfilesToState();
        int currentJournalCapacityBytes();
        bool allowJournalUpdate();

        bool includeProfile(
                ocpp2_0::GetChargingProfilesRequest const& request,
                detail::ReportCharingProfileState const& state,
                ocpp2_0::SetChargingProfileRequest const& profile);

        int totalMatchingProfiles(
                ocpp2_0::GetChargingProfilesRequest const& request,
                detail::ReportCharingProfileState const& state);

        bool advanceState(detail::ReportCharingProfileState& state);

        int totalRemainingProfiles(
                ocpp2_0::GetChargingProfilesRequest const& request,
                detail::ReportCharingProfileState const& state);

        std::set<int> getEvseIds();

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_;
        std::shared_ptr<StationInterface> station_;
        chargelab::CompressedJournalJson<detail::JournalUpdate> journal_;
        SteadyPointMillis start_timestamp_;

        std::optional<std::pair<SystemTimeMillis, SystemTimeMillis>> last_charging_profile_update_;
        std::optional<SteadyPointMillis> last_reported_journal_capacity_;
        std::optional<std::pair<ocpp2_0::GetChargingProfilesRequest, detail::ReportCharingProfileState>> pending_get_charging_profiles_request_;

        std::map<int, std::optional<SteadyPointMillis>> transaction_start_times_;
        std::map<int, std::optional<std::string>> transaction_id_map_;
        std::vector<ocpp2_0::SetChargingProfileRequest> charging_profiles_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_POWER_MANAGEMENT_MODULE2_0_H

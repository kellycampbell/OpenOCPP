#ifndef CHARGELAB_OPEN_FIRMWARE_POWER_MANAGEMENT_MODULE1_6_H
#define CHARGELAB_OPEN_FIRMWARE_POWER_MANAGEMENT_MODULE1_6_H

#include "openocpp/module/common_templates.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/interface/station_interface.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/compressed_journal.h"
#include <functional>

namespace chargelab {
    class TransactionModule1_6;

    namespace detail {
        struct SchedulePeriod {
            std::int64_t startPeriod;  // exclusive, in seconds
            std::int64_t endTime;  // inclusive, for last period it could be INT64_MAX, in seconds
            double limit;
            int stackLevel;
            ocpp1_6::ChargingProfilePurposeType purpose;
            int chargingProfileId; // which charging profile is used for this period
            CHARGELAB_JSON_INTRUSIVE(SchedulePeriod, startPeriod, endTime, limit, stackLevel, purpose, chargingProfileId)
        };

        struct JournalUpdate1_6 {
            std::optional<ocpp1_6::SetChargingProfileReq> setChargingProfileReq = std::nullopt;
            std::optional<ocpp1_6::ClearChargingProfileReq> clearChargingProfileReq = std::nullopt;
            CHARGELAB_JSON_INTRUSIVE(JournalUpdate1_6, setChargingProfileReq, clearChargingProfileReq)
        };
    }

    // Note: this module treats connector 0 as a separate physical connector. Profiles assigned to connector ID 0 are
    // passed as a schedule to connector 0, not to each physical connector. This is intended to allow the station to
    // define a suitable load balancing mechanism when more than one connector is present.
    class PowerManagementModule1_6 : public ServiceStateful1_6 {
        static constexpr int kCriticalWriteCreditsInitial = 100;
        static constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

        static constexpr int kMillisInDay = 1000*60*60*24;
        static constexpr int kMillisInWeek = kMillisInDay * 7;
        static constexpr int kJournalCapacityReportFrequencySeconds = 10;
        static constexpr int kFlashWriteCapacityOnBoot = 1024;
        static constexpr int kRequiredFlashLifetimeYears = 10;
        static constexpr double kRequiredFlashLifetimeMillis = kRequiredFlashLifetimeYears * (double)kMillisInDay * 365;
        static constexpr int kFlashLifetimeWriteCycles = 100000;

        friend class TransactionModule1_6;
    public:
        PowerManagementModule1_6(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> system,
                std::shared_ptr<StationInterface> station,
                std::unique_ptr<FlashBlockInterface> storage
        );

        ~PowerManagementModule1_6() override;

    private:
        void onActiveTransactionStarted(int connector_id, std::optional<ocpp1_6::ChargingProfile> transaction_profile);
        void onActiveTransactionIdAssigned(int connector_id, int transaction_id);
        void onActiveTransactionFinished(int connector_id);

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SetChargingProfileRsp>>
        onSetChargingProfileReq(const ocpp1_6::SetChargingProfileReq &req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SetChargingProfileRsp>>
        onSetChargingProfileReqInternal(const ocpp1_6::SetChargingProfileReq &req, bool persist_to_journal);

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearChargingProfileRsp>>
        onClearChargingProfileReq(const ocpp1_6::ClearChargingProfileReq& req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearChargingProfileRsp>>
        onClearChargingProfileReqInternal(const ocpp1_6::ClearChargingProfileReq& req, bool persist_to_journal);

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetCompositeScheduleRsp>>
        onGetCompositeScheduleReq(const ocpp1_6::GetCompositeScheduleReq& req) override;

        void runStep(ocpp1_6::OcppRemote&) override;

    private:
        void getCompositeSchedule(
                const ocpp1_6::GetCompositeScheduleReq& req,
                SystemTimeMillis const& time_now,
                std::function<void(std::vector<detail::SchedulePeriod> const& periods, int duration)> handler) const;

        std::vector<detail::SchedulePeriod> generateCompositeSchedule(
                std::vector<detail::SchedulePeriod> const& raw_schedule,
                SystemTimeMillis const& time_now,
                bool transaction_max_mixed_schedules) const;

        std::vector<detail::SchedulePeriod> generateUnorderedSchedule(
                std::vector<ocpp1_6::ChargingProfile> const& profiles,
                int requested_duration_seconds,
                SystemTimeMillis const& time_now,
                std::optional<SystemTimeMillis> const& transaction_start_time) const;

        std::vector<detail::SchedulePeriod> generateSchedulePeriods(
                ocpp1_6::ChargingProfile const& profile,
                std::int64_t start_time_now,
                std::int64_t end_time,
                std::optional<SystemTimeMillis> const& transaction_start_time) const;

        void updateActiveSchedules();
        bool persistChargingProfile(const ocpp1_6::SetChargingProfileReq& req);
        void publishProfileUpdates();

        std::pair<std::optional<ocpp1_6::SetChargingProfileReq>, std::optional<SystemTimeMillis>>
        getProfileAtTime(int connectorId, SystemTimeMillis timestamp, ocpp1_6::ChargingProfilePurposeType profileType);

        std::optional<ocpp1_6::ChargingSchedulePeriod> getPeriodAtTime(
                ocpp1_6::SetChargingProfileReq const& profile,
                SystemTimeMillis timestamp);

        int currentJournalCapacityBytes();
        static int clampToInt(double value);
        bool allowJournalUpdate();
        std::vector<detail::JournalUpdate1_6> chargingProfilesToState();

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_;
        std::shared_ptr<StationInterface> station_;

        // key: connectorId
        std::unordered_map<int, std::optional<SystemTimeMillis>> transaction_start_times_;
        // key: connectorId, value: transactionId
        std::unordered_map<int, int> active_transactions_;
        std::vector<ocpp1_6::SetChargingProfileReq> charging_profiles_;
        std::optional<SystemTimeMillis> next_profile_update_;

        // Saving charging profiles to storage but Tx Profiles are excluded because once power cycled the transaction would be stopped.
        chargelab::CompressedJournalJson<detail::JournalUpdate1_6> journal_;
        SteadyPointMillis start_timestamp_;
        bool charging_profile_applied_ {false};
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_POWER_MANAGEMENT_MODULE1_6_H

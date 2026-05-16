#include "openocpp/module/power_management_module1_6.h"

namespace chargelab {
    namespace {
        constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;
        constexpr int kMillisInDay = 1000*60*60*24;
        constexpr int kFlashWriteCapacityOnBoot = 1024;
        constexpr int kRequiredFlashLifetimeYears = 10;
        constexpr double kRequiredFlashLifetimeMillis = kRequiredFlashLifetimeYears * (double)kMillisInDay * 365;
        constexpr int kFlashLifetimeWriteCycles = 100000;
    }

    PowerManagementModule1_6::PowerManagementModule1_6(
            std::shared_ptr<Settings> settings,
            std::shared_ptr<SystemInterface> system,
            std::shared_ptr<StationInterface> station,
            std::unique_ptr<FlashBlockInterface> storage
    )
        : settings_(std::move(settings)),
          system_(std::move(system)),
          station_(std::move(station)),
          journal_(std::move(storage), "1.6-1")
    {
        start_timestamp_ = system_->steadyClockNow();
        CHARGELAB_LOG_MESSAGE(info) << "Re-applying saved 1.6 charging profiles";
        journal_.visit([&](std::string_view const& text, detail::JournalUpdate1_6 const& update) {
            {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(1ms);
            }

            if (update.setChargingProfileReq.has_value()) {
                auto const &request = update.setChargingProfileReq.value();
                auto response = onSetChargingProfileReqInternal(request, false);
                if (!response.has_value()) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Persisted profile was not accepted - null response: " << text;
                } else if (std::holds_alternative<ocpp1_6::CallError>(response.value())) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Persisted profile was not accepted - error response: " << text;
                    CHARGELAB_LOG_MESSAGE(warning) << "Error response was: " << std::get<ocpp1_6::CallError>(response.value());
                } else if (std::holds_alternative<ocpp1_6::SetChargingProfileRsp>(response.value())) {
                    auto const &value = std::get<ocpp1_6::SetChargingProfileRsp>(response.value());
                    if (value.status != ocpp1_6::ChargingProfileStatus::kAccepted) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Persisted profile was not accepted - not accepted: " << text;
                        CHARGELAB_LOG_MESSAGE(warning) << "Response was: " << value;
                    }
                }
            }

            if (update.clearChargingProfileReq.has_value()) {
                auto const& request = update.clearChargingProfileReq.value();
                auto response = onClearChargingProfileReqInternal(request, false);
                if (!response.has_value()) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Persisted clear profile request was not accepted - null response: " << text;
                } else if (std::holds_alternative<ocpp1_6::CallError>(response.value())) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Persisted clear profile request was not accepted - error response: " << text;
                    CHARGELAB_LOG_MESSAGE(warning) << "Error response was: " << std::get<ocpp1_6::CallError>(response.value());
                } else if (std::holds_alternative<ocpp1_6::ClearChargingProfileRsp>(response.value())) {
                    auto const& value = std::get<ocpp1_6::ClearChargingProfileRsp>(response.value());
                    if (value.status != ocpp1_6::ClearChargingProfileStatus::kAccepted) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Persisted clear profile request was not accepted - not accepted: " << text;
                        CHARGELAB_LOG_MESSAGE(warning) << "Response was: " << value;
                    }
                }
            }
        });

        CHARGELAB_LOG_MESSAGE(info) << "final charging profiles: " << charging_profiles_;
    }

    PowerManagementModule1_6::~PowerManagementModule1_6() {
        CHARGELAB_LOG_MESSAGE(debug) << "Deleting PowerManagementModule1_6";
    }

    void PowerManagementModule1_6::onActiveTransactionStarted(int connector_id, std::optional<ocpp1_6::ChargingProfile> transaction_profile) {
        if (transaction_profile.has_value()) {
            charging_profiles_.resize(charging_profiles_.size()+1);
            charging_profiles_.back() = ocpp1_6::SetChargingProfileReq {connector_id, transaction_profile.value()};
            publishProfileUpdates();
        }
    }

    void PowerManagementModule1_6::onActiveTransactionIdAssigned(int connector_id, int transaction_id) {
        transaction_start_times_[connector_id] = system_->systemClockNow();
        active_transactions_[connector_id] = transaction_id;
    }

    void PowerManagementModule1_6::onActiveTransactionFinished(int connector_id) {
        charging_profiles_.erase(
                std::remove_if(
                        charging_profiles_.begin(),
                        charging_profiles_.end(),
                        [&](ocpp1_6::SetChargingProfileReq const& profile) {
                            if (profile.csChargingProfiles.chargingProfilePurpose != ocpp1_6::ChargingProfilePurposeType::kTxProfile)
                                return false;
                            if (profile.connectorId != connector_id)
                                return false;
                            return true;
                        }
                ),
                charging_profiles_.end()
        );

        transaction_start_times_[connector_id] = std::nullopt;
        auto it = active_transactions_.find(connector_id);
        if (it != active_transactions_.end())
            active_transactions_.erase(it);

        publishProfileUpdates();
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SetChargingProfileRsp>>
    PowerManagementModule1_6::onSetChargingProfileReq(const ocpp1_6::SetChargingProfileReq &req) {
        return onSetChargingProfileReqInternal(req, true);
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::SetChargingProfileRsp>>
    PowerManagementModule1_6::onSetChargingProfileReqInternal(const ocpp1_6::SetChargingProfileReq &req, bool persist_to_journal) {
        if (req.connectorId != 0 && !station_->lookupConnectorId1_6(req.connectorId).has_value()) {
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "ConnectorId does not exist", common::RawJson::empty_object()};
        }

        switch (req.csChargingProfiles.chargingProfilePurpose) {
            case ocpp1_6::ChargingProfilePurposeType::kTxProfile:
                if (req.connectorId == 0)
                    return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "TxProfile must specify a connector ID", common::RawJson::empty_object()};
                if (!req.csChargingProfiles.transactionId.has_value())
                    return ocpp1_6::CallError {ocpp1_6::ErrorCode::kFormationViolation, "TxProfile must specify transactionId", common::RawJson::empty_object()};
                if (active_transactions_[req.connectorId] != req.csChargingProfiles.transactionId)
                    return ocpp1_6::SetChargingProfileRsp {ocpp1_6::ChargingProfileStatus::kRejected};
                break;

            case ocpp1_6::ChargingProfilePurposeType::kTxDefaultProfile:
                if (req.csChargingProfiles.transactionId.has_value())
                    return ocpp1_6::CallError {ocpp1_6::ErrorCode::kFormationViolation, "TxDefaultProfile must not specify transactionId", common::RawJson::empty_object()};
                break;

            case ocpp1_6::ChargingProfilePurposeType::kChargePointMaxProfile:
                if (req.connectorId != 0)
                    return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "ChargePointMaxProfile may only be set for connectorId 0", common::RawJson::empty_object()};
                if (req.csChargingProfiles.transactionId.has_value())
                    return ocpp1_6::CallError {ocpp1_6::ErrorCode::kFormationViolation, "ChargePointMaxProfile must not specify transactionId", common::RawJson::empty_object()};
                break;

            default:
                return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "Unknown charging profile purpose", common::RawJson::empty_object()};
        }

        auto const& schedule = req.csChargingProfiles.chargingSchedule;
        if (schedule.chargingRateUnit == ocpp1_6::ChargingRateUnitType::kValueNotFoundInEnum)
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "Unknown charging rate unit", common::RawJson::empty_object()};

        if (schedule.chargingSchedulePeriod.empty())
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "No schedule periods in received charging profile", common::RawJson::empty_object()};

        if (req.csChargingProfiles.stackLevel > settings_->ChargeProfileMaxStackLevel.getValue())
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "Stack level exceeded the limit: " + std::to_string(settings_->ChargeProfileMaxStackLevel.getValue()), common::RawJson::empty_object()};

        for (auto& x : charging_profiles_) {
            if (x == req)
                return ocpp1_6::SetChargingProfileRsp {ocpp1_6::ChargingProfileStatus::kAccepted};
        }

        if (persist_to_journal) {
            if (!allowJournalUpdate())
                return ocpp1_6::SetChargingProfileRsp{ocpp1_6::ChargingProfileStatus::kRejected};
        }

        auto old_profiles = charging_profiles_;
        charging_profiles_.erase(
                std::remove_if(
                        charging_profiles_.begin(),
                        charging_profiles_.end(),
                        [&](ocpp1_6::SetChargingProfileReq const& profile) {
                            auto const& lhs = profile.csChargingProfiles;
                            auto const& rhs = req.csChargingProfiles;
                            if (lhs.chargingProfileId == rhs.chargingProfileId)
                                return true;
                            if (lhs.chargingProfilePurpose == rhs.chargingProfilePurpose && lhs.stackLevel == rhs.stackLevel)
                                return true;
                            return false;
                        }
                ),
                charging_profiles_.end()
        );

        if ((int)charging_profiles_.size() >= settings_->MaxChargingProfilesInstalled.getValue()) {
            charging_profiles_ = old_profiles;
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation,
                    "Installed profiles reached the limit of " + std::to_string(settings_->MaxChargingProfilesInstalled.getValue()),
                    common::RawJson::empty_object()};
        }

        if (persist_to_journal && req.csChargingProfiles.chargingProfilePurpose != ocpp1_6::ChargingProfilePurposeType::kTxProfile) {
            if (!journal_.addUpdate(chargingProfilesToState(), detail::JournalUpdate1_6{req})) {
                charging_profiles_ = old_profiles;
                return ocpp1_6::SetChargingProfileRsp{ocpp1_6::ChargingProfileStatus::kRejected};
            }
        }

        charging_profiles_.resize(charging_profiles_.size()+1);
        charging_profiles_.back() = req;

        if (persist_to_journal)
            publishProfileUpdates();

        return ocpp1_6::SetChargingProfileRsp {ocpp1_6::ChargingProfileStatus::kAccepted};
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearChargingProfileRsp>>
    PowerManagementModule1_6::onClearChargingProfileReq(const ocpp1_6::ClearChargingProfileReq& req) {
        return onClearChargingProfileReqInternal(req, true);
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ClearChargingProfileRsp>>
    PowerManagementModule1_6::onClearChargingProfileReqInternal(const ocpp1_6::ClearChargingProfileReq& req, bool persist_to_journal) {
        if (req.stackLevel.has_value() && req.stackLevel.value() < 0)
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kPropertyConstraintViolation, "Stack level is negative", common::RawJson::empty_object()};

        if (req.connectorId.has_value() && req.connectorId.value() == 0)
            return ocpp1_6::CallError {ocpp1_6::ErrorCode::kNotSupported, "Connector Id = 0 is not supported", common::RawJson::empty_object()};

        std::vector<ocpp1_6::SetChargingProfileReq> old_profiles;
        if (persist_to_journal)
            old_profiles = charging_profiles_;

        charging_profiles_.erase(
                std::remove_if(
                        charging_profiles_.begin(),
                        charging_profiles_.end(),
                        [&](ocpp1_6::SetChargingProfileReq const& x) {
                            if (req.id.has_value()) {
                                return req.id.value() == x.csChargingProfiles.chargingProfileId;
                            } else {
                                bool match = true;
                                if (req.connectorId.has_value())
                                    match &= req.connectorId.value() == x.connectorId;
                                if (req.chargingProfilePurpose.has_value())
                                    match &= req.chargingProfilePurpose.value() == x.csChargingProfiles.chargingProfilePurpose;
                                if (req.stackLevel.has_value())
                                    match &= req.stackLevel.value() == x.csChargingProfiles.stackLevel;
                                return match;
                            }
                        }
                ),
                charging_profiles_.end()
        );

        if (persist_to_journal && old_profiles.size() > charging_profiles_.size()) {
            if (allowJournalUpdate()) {
                if (!journal_.addUpdate(chargingProfilesToState(), detail::JournalUpdate1_6{std::nullopt, {req}})) {
                    charging_profiles_ = old_profiles;
                    return ocpp1_6::CallError {ocpp1_6::ErrorCode::kNotSupported, "Failed writing update to flash storage", common::RawJson::empty_object()};
                }
            }
        }

        publishProfileUpdates();
        return ocpp1_6::ClearChargingProfileRsp {ocpp1_6::ClearChargingProfileStatus::kAccepted};
    }

    std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetCompositeScheduleRsp>>
    PowerManagementModule1_6::onGetCompositeScheduleReq(const ocpp1_6::GetCompositeScheduleReq& req) {
        auto time_now = system_->systemClockNow();
        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetCompositeScheduleRsp>> rsp;

        getCompositeSchedule(req, time_now, [&](std::vector<detail::SchedulePeriod> const& periods, int duration) {
            std::vector<ocpp1_6::ChargingSchedulePeriod> target(periods.size());
            std::transform(periods.begin(), periods.end(), target.begin(),
                           [=](detail::SchedulePeriod const &source) {
                               return ocpp1_6::ChargingSchedulePeriod{
                                       std::max(static_cast<std::int64_t>(0), source.startPeriod - time_now/1000),
                                       source.limit, std::nullopt};
                           });

            if (duration < req.duration) {
                double connector_default_max_amps = 0;
                for (auto const& x : station_->getConnectorMetadata()) {
                    if (x.second.connector_id1_6 == req.connectorId) {
                        connector_default_max_amps = x.second.power_max_amps;
                        break;
                    }
                }
                target.emplace_back(ocpp1_6::ChargingSchedulePeriod{duration, connector_default_max_amps, std::nullopt});
                duration = req.duration;
            }

            rsp = std::make_optional(ocpp1_6::GetCompositeScheduleRsp{
                    ocpp1_6::GetCompositeScheduleStatus::kAccepted,
                    req.connectorId,
                    target.empty() ? std::nullopt : std::make_optional(ocpp1_6::DateTime(time_now)),
                    target.empty() ? std::nullopt : std::make_optional(ocpp1_6::ChargingSchedule {
                            duration,
                            ocpp1_6::DateTime(time_now),
                            ocpp1_6::ChargingRateUnitType::kA,
                            std::move(target),
                            std::nullopt
                    })
            });
        });

        return rsp;
    }

    void PowerManagementModule1_6::runStep(ocpp1_6::OcppRemote&) {
        if (!charging_profile_applied_) {
            if (system_->isClockOutOfSync())
                return;

            start_timestamp_ = system_->steadyClockNow();
            publishProfileUpdates();
            charging_profile_applied_ = true;
        }

        if (next_profile_update_.has_value()) {
            if (system_->systemClockNow() - next_profile_update_.value() >= 0)
                publishProfileUpdates();
        }
    }

    void PowerManagementModule1_6::getCompositeSchedule(
            const ocpp1_6::GetCompositeScheduleReq& req,
            SystemTimeMillis const& time_now,
            std::function<void(std::vector<detail::SchedulePeriod> const& periods, int duration)> handler) const
    {
        std::vector<ocpp1_6::ChargingProfile> transactionProfiles;
        std::vector<ocpp1_6::ChargingProfile> maxProfiles;

        for (auto const& profile : charging_profiles_) {
            if (profile.connectorId == 0 || profile.connectorId == req.connectorId) {
                if (profile.csChargingProfiles.chargingProfilePurpose == ocpp1_6::ChargingProfilePurposeType::kChargePointMaxProfile) {
                    maxProfiles.push_back(profile.csChargingProfiles);
                } else {
                    transactionProfiles.push_back(profile.csChargingProfiles);
                }
            }
        }

        auto it = transaction_start_times_.find(req.connectorId);
        auto raw_transaction_schedule = generateUnorderedSchedule(transactionProfiles, req.duration, time_now,
                it == transaction_start_times_.end() ? std::nullopt : it->second);
        auto transaction_schedule = generateCompositeSchedule(raw_transaction_schedule, time_now, false);

        auto raw_max_schedule = generateUnorderedSchedule(maxProfiles, req.duration, time_now,
                it == transaction_start_times_.end() ? std::nullopt : it->second);
        auto max_schedule = generateCompositeSchedule(raw_max_schedule, time_now, false);

        max_schedule.insert(max_schedule.end(), transaction_schedule.begin(), transaction_schedule.end());
        auto composite_schedule = generateCompositeSchedule(max_schedule, time_now, true);

        int duration = !composite_schedule.empty() ? std::min(composite_schedule.back().endTime - time_now/1000, static_cast<std::int64_t>(req.duration)) : 0;
        handler(composite_schedule, duration);
    }

    std::vector<detail::SchedulePeriod> PowerManagementModule1_6::generateCompositeSchedule(
            std::vector<detail::SchedulePeriod> const& raw_schedule,
            SystemTimeMillis const& time_now,
            bool transaction_max_mixed_schedules) const
    {
        std::int64_t time_now_seconds = time_now/1000;

        auto cmp = [](detail::SchedulePeriod const& a, detail::SchedulePeriod const& b) {
            if (a.startPeriod == b.startPeriod) {
                if (a.purpose != b.purpose) return a.purpose < b.purpose;
                else return a.stackLevel < b.stackLevel;
            } else {
                return a.startPeriod > b.startPeriod;
            }
        };

        std::priority_queue<detail::SchedulePeriod, std::vector<detail::SchedulePeriod>, decltype(cmp)>
                pq(std::make_move_iterator(raw_schedule.begin()), std::make_move_iterator(raw_schedule.end()), cmp);

        if (pq.empty())
            return {};

        std::vector<detail::SchedulePeriod> composite;
        std::optional<detail::SchedulePeriod> last = std::nullopt;

        while (!pq.empty()) {
            auto current = pq.top();
            pq.pop();
            if (current.startPeriod == current.endTime)
                continue;
            if (last && last->startPeriod >= last->endTime)
                last = std::nullopt;

            if (!last) {
                last = std::move(current);
                continue;
            }

            if (current.startPeriod > last->startPeriod) {
                auto last2 = last.value();
                last2.endTime = current.startPeriod;
                if (last2.endTime > time_now_seconds)
                    composite.push_back(last2);

                last->startPeriod = current.startPeriod;
                if (last->startPeriod != last->endTime) {
                    pq.push(std::move(last.value()));
                    last = std::nullopt;
                }
                pq.push(current);
                continue;
            }

            if (current.endTime > last->endTime) {
                auto current2 = current;
                current2.startPeriod = last->endTime;
                pq.push(current2);
                current.endTime = last->endTime;
            }

            if (transaction_max_mixed_schedules) {
                if (current.endTime < last->endTime) {
                    auto last2 = last.value();
                    last2.startPeriod = current.endTime;
                    pq.push(last2);
                    last->endTime = current.endTime;
                }

                if (current.purpose != last->purpose) {
                    if (last->purpose == ocpp1_6::ChargingProfilePurposeType::kChargePointMaxProfile) {
                        if (last->limit >= current.limit)
                            last = current;
                    } else {
                        if (last->limit > current.limit)
                            last = current;
                    }
                }
            }
        }

        if (last.has_value())
            composite.push_back(last.value());

        auto it = composite.begin();
        while (it != composite.end()) {
            if (it->endTime <= time_now_seconds) {
                it = composite.erase(it);
                continue;
            }
            auto it2 = it + 1;
            if (it2 == composite.end())
                break;
            if (it2->limit == it->limit) {
                it->endTime = it2->endTime;
                composite.erase(it2);
            } else {
                it = it2;
            }
        }

        return composite;
    }

    std::vector<detail::SchedulePeriod> PowerManagementModule1_6::generateUnorderedSchedule(
            std::vector<ocpp1_6::ChargingProfile> const& profiles,
            int requested_duration_seconds,
            SystemTimeMillis const& time_now,
            std::optional<SystemTimeMillis> const& transaction_start_time) const
    {
        std::vector<detail::SchedulePeriod> merged;
        std::int64_t time_now_seconds = time_now/1000;
        std::int64_t end_time = time_now_seconds + requested_duration_seconds;

        for (auto const& profile : profiles) {
            if (profile.validFrom && profile.validFrom.value().getTimestamp() &&
                (profile.validFrom.value().getTimestamp().value() - (time_now + requested_duration_seconds*1000))/1000 >= 0)
                continue;
            if (profile.validTo && profile.validTo.value().getTimestamp() && profile.validTo.value().getTimestamp().value()/1000 <= time_now_seconds)
                continue;

            auto adjusted_end_time = end_time;
            if (profile.validTo && profile.validTo.value().getTimestamp())
                adjusted_end_time = std::min(adjusted_end_time, profile.validTo.value().getTimestamp().value()/1000);

            if (profile.chargingProfileKind == ocpp1_6::ChargingProfileKindType::kRecurring && profile.recurrencyKind.has_value()) {
                auto start_time = time_now_seconds;
                while (start_time < end_time) {
                    auto periods = generateSchedulePeriods(profile, start_time, adjusted_end_time, transaction_start_time);
                    merged.insert(merged.begin(), periods.begin(), periods.end());
                    start_time += profile.recurrencyKind.value() == ocpp1_6::RecurrencyKindType::kDaily ? kSecondsPerDay : 7 * kSecondsPerDay;
                }
            } else {
                auto periods = generateSchedulePeriods(profile, time_now_seconds, adjusted_end_time, transaction_start_time);
                merged.insert(merged.begin(), periods.begin(), periods.end());
            }
        }

        return merged;
    }

    std::vector<detail::SchedulePeriod> PowerManagementModule1_6::generateSchedulePeriods(
            ocpp1_6::ChargingProfile const& profile,
            std::int64_t start_time_now,
            std::int64_t end_time,
            std::optional<SystemTimeMillis> const& transaction_start_time) const
    {
        auto startSchedule = profile.chargingSchedule.startSchedule;
        std::int64_t base_start_time = startSchedule.has_value() && startSchedule.value().getTimestamp().has_value() ?
                startSchedule->getTimestamp().value()/1000 : start_time_now;

        std::vector<detail::SchedulePeriod> periods;

        for (auto const& period : profile.chargingSchedule.chargingSchedulePeriod) {
            std::int64_t absolute_start_time = base_start_time + period.startPeriod;

            if (profile.chargingProfileKind == ocpp1_6::ChargingProfileKindType::kRelative) {
                if (transaction_start_time.has_value())
                    absolute_start_time = transaction_start_time.value()/1000 + period.startPeriod;
            }

            if (absolute_start_time < end_time) {
                if (profile.chargingSchedule.duration.has_value()) {
                    if (absolute_start_time - base_start_time < profile.chargingSchedule.duration.value()) {
                        if (!periods.empty())
                            periods.back().endTime = absolute_start_time;
                        periods.emplace_back(detail::SchedulePeriod{
                                absolute_start_time,
                                std::min(base_start_time + profile.chargingSchedule.duration.value(), end_time),
                                period.limit,
                                profile.stackLevel,
                                profile.chargingProfilePurpose,
                                profile.chargingProfileId});
                    } else {
                        break;
                    }
                } else {
                    if (!periods.empty())
                        periods.back().endTime = absolute_start_time;
                    periods.emplace_back(detail::SchedulePeriod{
                            absolute_start_time, INT64_MAX, period.limit, profile.stackLevel,
                            profile.chargingProfilePurpose, profile.chargingProfileId});
                }
            }
        }

        return periods;
    }

    void PowerManagementModule1_6::updateActiveSchedules() {
        auto const now = system_->systemClockNow();
        auto const time_now_seconds = now/1000;
        const int one_hour_duration = 60*60;
        std::int64_t shortest_duration = one_hour_duration;

        std::set<int> connector_ids;
        connector_ids.insert(0);
        for (auto const& x : station_->getConnectorMetadata())
            connector_ids.insert(x.second.connector_id1_6);

        for (auto const& connector : connector_ids) {
            getCompositeSchedule(ocpp1_6::GetCompositeScheduleReq {connector, one_hour_duration, ocpp1_6::ChargingRateUnitType::kA},
                                 now,
                                 [&](std::vector<detail::SchedulePeriod> const& periods, int duration) {
                for (auto const& p : periods) {
                    if (p.endTime > time_now_seconds) {
                        shortest_duration = std::min(shortest_duration, p.endTime - std::max(p.startPeriod, time_now_seconds));
                        std::vector<StationInterface::schedule_type1_6> active_schedules;

                        auto it = std::find_if(charging_profiles_.begin(), charging_profiles_.end(), [=](auto const& profile) {
                            return profile.csChargingProfiles.chargingProfileId == p.chargingProfileId;
                        });

                        active_schedules.push_back({*it, ocpp1_6::ChargingSchedulePeriod {p.startPeriod - time_now_seconds, p.limit}});

                        if (connector == 0) {
                            station_->setActiveChargePointMaxProfiles(active_schedules);
                        } else {
                            station_->setActiveEvseProfiles(connector, active_schedules);
                        }
                        break;
                    }
                }
            });
        }

        auto time_now2 = static_cast<std::int64_t>(now) + shortest_duration * 1000;
        next_profile_update_ = static_cast<SystemTimeMillis>(time_now2);
    }

    bool PowerManagementModule1_6::persistChargingProfile(const ocpp1_6::SetChargingProfileReq &req) {
        (void)req;
        return true;
    }

    void PowerManagementModule1_6::publishProfileUpdates() {
        if (system_->isClockOutOfSync())
            return;
        updateActiveSchedules();
    }

    std::pair<std::optional<ocpp1_6::SetChargingProfileReq>, std::optional<SystemTimeMillis>>
    PowerManagementModule1_6::getProfileAtTime(int connectorId, SystemTimeMillis timestamp, ocpp1_6::ChargingProfilePurposeType profileType) {
        std::optional<ocpp1_6::SetChargingProfileReq> active;
        for (auto const& profile : charging_profiles_) {
            if (profile.connectorId != connectorId)
                continue;
            if (profile.csChargingProfiles.chargingProfilePurpose != profileType)
                continue;

            if (profile.csChargingProfiles.validFrom.has_value()) {
                auto valid_from_ts = profile.csChargingProfiles.validFrom->getTimestamp();
                if (!valid_from_ts.has_value()) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Charging profile has validFrom but the value wasn't convertable into a timestamp: " << profile.csChargingProfiles.validFrom->getText();
                } else if (timestamp < valid_from_ts.value()) {
                    continue;
                }
            }

            if (profile.csChargingProfiles.validTo.has_value()) {
                auto valid_to_ts = profile.csChargingProfiles.validTo->getTimestamp();
                if (!valid_to_ts.has_value()) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Charging profile has validTo but the value wasn't convertable into a timestamp: " << profile.csChargingProfiles.validTo->getText();
                } else if (timestamp >= valid_to_ts.value()) {
                    continue;
                }
            }

            if (!active.has_value() || profile.csChargingProfiles.stackLevel > active->csChargingProfiles.stackLevel)
                active = profile;
        }

        std::optional<SystemTimeMillis> active_valid_to;
        if (active.has_value() && active->csChargingProfiles.validTo.has_value())
            active_valid_to = active->csChargingProfiles.validTo->getTimestamp();

        std::optional<ocpp1_6::SetChargingProfileReq> next;
        for (auto const& profile : charging_profiles_) {
            if (profile.connectorId != connectorId)
                continue;
            if (profile.csChargingProfiles.chargingProfilePurpose != profileType)
                continue;

            if (profile.csChargingProfiles.validFrom.has_value()) {
                auto valid_from_ts = profile.csChargingProfiles.validFrom->getTimestamp();
                if (!valid_from_ts.has_value()) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Charging profile has validFrom but the value wasn't convertable into a timestamp: " << profile.csChargingProfiles.validFrom->getText();
                } else if (valid_from_ts.value() <= timestamp) {
                    continue;
                } else if (active_valid_to.has_value() && active_valid_to.value() <= valid_from_ts.value()) {
                    continue;
                }
            }

            if (profile.csChargingProfiles.validTo.has_value()) {
                auto valid_to_ts = profile.csChargingProfiles.validTo->getTimestamp();
                if (!valid_to_ts.has_value()) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Charging profile has validTo but the value wasn't convertable into a timestamp: " << profile.csChargingProfiles.validTo->getText();
                } else if (timestamp >= valid_to_ts.value()) {
                    continue;
                }
            }

            if (!next.has_value() || profile.csChargingProfiles.stackLevel > next->csChargingProfiles.stackLevel)
                next = profile;
        }

        std::optional<SystemTimeMillis> valid_until;
        if (next.has_value() && next->csChargingProfiles.validFrom.has_value()) {
            valid_until = next->csChargingProfiles.validFrom->getTimestamp();
        } else if (active.has_value() && active->csChargingProfiles.validTo.has_value()) {
            valid_until = active->csChargingProfiles.validTo->getTimestamp();
        }

        return std::make_pair(active, valid_until);
    }

    std::optional<ocpp1_6::ChargingSchedulePeriod> PowerManagementModule1_6::getPeriodAtTime(
            ocpp1_6::SetChargingProfileReq const& profile, SystemTimeMillis timestamp)
    {
        if (profile.csChargingProfiles.validFrom.has_value()) {
            auto valid_from_ts = profile.csChargingProfiles.validFrom->getTimestamp();
            if (valid_from_ts.has_value() && timestamp < valid_from_ts.value())
                return std::nullopt;
        }

        if (profile.csChargingProfiles.validTo.has_value()) {
            auto valid_to_ts = profile.csChargingProfiles.validTo->getTimestamp();
            if (valid_to_ts.has_value() && timestamp >= valid_to_ts.value())
                return std::nullopt;
        }

        if (!profile.csChargingProfiles.chargingSchedule.startSchedule.has_value()) {
            CHARGELAB_LOG_MESSAGE(error) << "Charging profile missing startSchedule: " << profile;
            return std::nullopt;
        }

        SystemTimeMillis schedule_start;
        {
            auto schedule_start_ts = profile.csChargingProfiles.chargingSchedule.startSchedule->getTimestamp();
            if (!schedule_start_ts.has_value()) {
                CHARGELAB_LOG_MESSAGE(error) << "Charging profile startSchedule not convertable to a timestamp: " << profile;
                return std::nullopt;
            }
            schedule_start = schedule_start_ts.value();
        }

        if (profile.csChargingProfiles.chargingProfileKind == ocpp1_6::ChargingProfileKindType::kRecurring) {
            if (!profile.csChargingProfiles.recurrencyKind.has_value()) {
                CHARGELAB_LOG_MESSAGE(error) << "Recurring charging profile missing recurrencyKind: " << profile;
                return std::nullopt;
            }

            int recurrency_period_millis;
            switch (profile.csChargingProfiles.recurrencyKind.value()) {
                case ocpp1_6::RecurrencyKindType::kDaily:
                    recurrency_period_millis = 1000 * 60 * 60 * 24;
                    break;
                case ocpp1_6::RecurrencyKindType::kWeekly:
                    recurrency_period_millis = 1000 * 60 * 60 * 24 * 7;
                    break;
                default:
                    CHARGELAB_LOG_MESSAGE(error) << "Bad charging profile recurrencyKind: " << profile;
                    return std::nullopt;
            }

            auto delta = timestamp - schedule_start;
            auto periods = delta / recurrency_period_millis;
            if (delta < 0)
                periods -= 1;

            schedule_start = static_cast<SystemTimeMillis>(schedule_start + (periods * recurrency_period_millis));
        }

        if (profile.csChargingProfiles.chargingSchedule.chargingSchedulePeriod.empty()) {
            CHARGELAB_LOG_MESSAGE(error) << "Expected at least one chargingSchedulePeriod: " << profile;
            return std::nullopt;
        }

        auto const& periods = profile.csChargingProfiles.chargingSchedule.chargingSchedulePeriod;
        std::size_t active_period_index = 0;
        auto delta = timestamp - schedule_start;
        for (std::size_t i=1; i < periods.size(); i++) {
            if (delta < periods[i].startPeriod)
                break;
            active_period_index = i;
        }

        return periods[active_period_index];
    }

    int PowerManagementModule1_6::currentJournalCapacityBytes() {
        auto const written = journal_.totalBytesWritten();
        auto const elapsed_millis = system_->steadyClockNow() - start_timestamp_;

        double accumulated_writable_bytes = journal_.storageSize();
        accumulated_writable_bytes *= kFlashLifetimeWriteCycles;
        accumulated_writable_bytes /= kRequiredFlashLifetimeMillis;
        accumulated_writable_bytes *= elapsed_millis;
        accumulated_writable_bytes += kFlashWriteCapacityOnBoot;

        auto const result = accumulated_writable_bytes - written;
        return clampToInt(result);
    }

    int PowerManagementModule1_6::clampToInt(double value) {
        if (value >= std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        else if (value <= std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
        else return static_cast<int>(value);
    }

    bool PowerManagementModule1_6::allowJournalUpdate() {
        return currentJournalCapacityBytes() > 0;
    }

    std::vector<detail::JournalUpdate1_6> PowerManagementModule1_6::chargingProfilesToState() {
        std::vector<detail::JournalUpdate1_6> result;
        for (auto const& x : charging_profiles_)
            if (x.csChargingProfiles.chargingProfilePurpose != ocpp1_6::ChargingProfilePurposeType::kTxProfile)
                result.push_back(detail::JournalUpdate1_6{x});
        return result;
    }
}

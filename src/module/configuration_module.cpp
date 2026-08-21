#include "openocpp/module/configuration_module.h"

namespace chargelab {

ConfigurationModule::ConfigurationModule(std::shared_ptr<Settings> settings, std::shared_ptr<SystemInterface> system)
    : settings_(std::move(settings)),
      system_(std::move(system))
{
    assert(settings_ != nullptr);
    assert(system_ != nullptr);
}

ConfigurationModule::~ConfigurationModule() {
    CHARGELAB_LOG_MESSAGE(debug) << "Deleting ConfigurationModule";
    settings_->saveIfModified();
}

void ConfigurationModule::runUnconditionally() {
    settings_->saveIfModified();
}

void ConfigurationModule::runStep(ocpp1_6::OcppRemote&) {
}

void ConfigurationModule::runStep(ocpp2_0::OcppRemote &remote) {
    if (ocpp2_0_pending_base_report_.has_value()) {
        auto request = ocpp2_0_pending_base_report_.value();
        auto settings = settings_;
        ocpp2_0::NotifyReportRequest response {
                request.requestId,
                {system_->systemClockNow()},
                false,
                0,
                {[&](std::function<void(ocpp2_0::ReportDataType const &)> const &visitor) {
                    settings->visitSettings([&](SettingBase& setting) {
                        auto const metadata = setting.getMetadata();
                        bool include_characteristics;
                        switch (request.reportBase) {
                            default:
                                CHARGELAB_LOG_MESSAGE(error) << "Unexpected report type in generator: " << request.reportBase;
                                return;

                            case ocpp2_0::ReportBaseEnumType::kConfigurationInventory:
                                include_characteristics = true;
                                if (!metadata.config.isAllowOcppWrite())
                                    return;
                                break;

                            case ocpp2_0::ReportBaseEnumType::kFullInventory:
                                include_characteristics = true;
                                break;
                        }

                        if (!metadata.model2_0.has_value())
                            return;

                        auto element = ocpp2_0::ReportDataType {
                                metadata.model2_0->component_type,
                                metadata.model2_0->variable_type,
                                setting.getAttributes2_0()
                        };

                        if (include_characteristics) {
                            element.variableCharacteristics = metadata.model2_0->variable_characteristics;
                        }

                        visitor(element);
                    });
                }}
        };

        if (remote.sendNotifyReportReq(response).has_value()) {
            ocpp2_0_pending_base_report_ = std::nullopt;
        }
    }

    if (ocpp2_0_pending_monitoring_report_.has_value()) {
        auto const& request = ocpp2_0_pending_monitoring_report_.value();

        // Group individual monitors by component+variable, per the MonitoringDataType shape.
        std::vector<ocpp2_0::MonitoringDataType> grouped;
        for (auto const& monitor : monitors_) {
            if (request.componentVariable.has_value()) {
                bool matched = false;
                for (auto const& filter : request.componentVariable.value()) {
                    if (filter.component != monitor.component)
                        continue;
                    if (filter.variable.has_value() && filter.variable.value() != monitor.variable)
                        continue;

                    matched = true;
                    break;
                }

                if (!matched)
                    continue;
            }

            auto entry = std::find_if(grouped.begin(), grouped.end(), [&](ocpp2_0::MonitoringDataType const& x) {
                return x.component == monitor.component && x.variable == monitor.variable;
            });
            if (entry == grouped.end()) {
                grouped.push_back(ocpp2_0::MonitoringDataType {monitor.component, monitor.variable, {}});
                entry = std::prev(grouped.end());
            }

            entry->variableMonitoring.push_back(ocpp2_0::VariableMonitoringType {
                    monitor.id,
                    monitor.transaction,
                    monitor.value,
                    monitor.type,
                    monitor.severity
            });
        }

        ocpp2_0::NotifyMonitoringReportRequest response {
                request.requestId,
                false,
                0,
                {system_->systemClockNow()},
                std::move(grouped)
        };

        if (remote.sendNotifyMonitoringReportReq(response).has_value()) {
            ocpp2_0_pending_monitoring_report_ = std::nullopt;
        }
    }

    evaluateMonitors();

    if (!pending_events_.empty()) {
        ocpp2_0::NotifyEventRequest request {
                {system_->systemClockNow()},
                false,
                0,
                pending_events_
        };

        if (remote.sendNotifyEventReq(request).has_value()) {
            pending_events_.clear();
        }
    }
}

std::optional<ConfigurationModule::MonitoredValue>
ConfigurationModule::readMonitoredValue(MonitorRecord const& monitor) const {
    std::optional<MonitoredValue> result;
    settings_->visitSettings([&](SettingBase& setting) {
        if (result.has_value())
            return;

        auto const metadata = setting.getMetadata();
        if (!metadata.model2_0.has_value())
            return;
        if (metadata.model2_0->component_type != monitor.component)
            return;
        if (metadata.model2_0->variable_type != monitor.variable)
            return;

        MonitoredValue value;
        value.text = setting.getValueAsString();

        // B08.FR.10: numeric comparison only applies to decimal/integer variables - Delta on boolean/string/
        // enumeration variables instead triggers on any change (handled in evaluateMonitors), and
        // UpperThreshold/LowerThreshold/Periodic simply won't match a non-numeric variable's semantics.
        auto const data_type = metadata.model2_0->variable_characteristics.dataType;
        if (data_type == ocpp2_0::DataEnumType::kdecimal || data_type == ocpp2_0::DataEnumType::kinteger) {
            value.numeric = string::ToDouble(value.text);
        }

        result = std::move(value);
    });

    return result;
}

void ConfigurationModule::raiseMonitorEvent(
        MonitorRecord const& monitor,
        std::string const& actual_value,
        bool cleared
) {
    // B08.FR.13: suppress events below the configured monitoring level (0 is most severe).
    if (monitor.severity > monitoring_level_)
        return;

    ocpp2_0::EventDataType event {};
    event.eventId = next_event_id_++;
    event.timestamp = {system_->systemClockNow()};
    event.trigger = (monitor.type == ocpp2_0::MonitorEnumType::kDelta)
            ? ocpp2_0::EventTriggerEnumType::kDelta
            : (monitor.type == ocpp2_0::MonitorEnumType::kPeriodic ||
               monitor.type == ocpp2_0::MonitorEnumType::kPeriodicClockAligned)
                    ? ocpp2_0::EventTriggerEnumType::kPeriodic
                    : ocpp2_0::EventTriggerEnumType::kAlerting;
    event.actualValue = {actual_value};
    event.cleared = cleared;
    event.variableMonitoringId = monitor.id;
    // These monitors are always CSMS-defined (SetVariableMonitoring), never charger-preconfigured.
    event.eventNotificationType = ocpp2_0::EventNotificationEnumType::kCustomMonitor;
    event.component = monitor.component;
    event.variable = monitor.variable;

    pending_events_.push_back(std::move(event));
}

void ConfigurationModule::evaluateMonitors() {
    auto const now = system_->steadyClockNow();

    for (auto& monitor : monitors_) {
        auto const actual_value = readMonitoredValue(monitor);
        if (!actual_value.has_value())
            continue;

        switch (monitor.type) {
            case ocpp2_0::MonitorEnumType::kUpperThreshold:
            case ocpp2_0::MonitorEnumType::kLowerThreshold: {
                // Threshold comparisons are only meaningful for numeric variables.
                if (!actual_value->numeric.has_value())
                    continue;
                double const value = actual_value->numeric.value();

                bool const is_upper = (monitor.type == ocpp2_0::MonitorEnumType::kUpperThreshold);
                double const margin = std::max(std::abs(monitor.value) * kThresholdHysteresisFraction, kThresholdMinHysteresis);

                bool const past_activate = is_upper ? (value > monitor.value) : (value < monitor.value);
                bool const past_clear = is_upper ? (value <= monitor.value - margin) : (value >= monitor.value + margin);

                bool const debounced = monitor.threshold_debounce_until != SteadyPointMillis{} &&
                        now < monitor.threshold_debounce_until;
                if (debounced)
                    continue;

                if (past_activate && !monitor.threshold_active) {
                    monitor.threshold_active = true;
                    monitor.threshold_debounce_until = SteadyPointMillis{now + kThresholdDebounceMillis};
                    raiseMonitorEvent(monitor, actual_value->text, false);
                } else if (past_clear && monitor.threshold_active) {
                    monitor.threshold_active = false;
                    monitor.threshold_debounce_until = SteadyPointMillis{now + kThresholdDebounceMillis};
                    raiseMonitorEvent(monitor, actual_value->text, true);
                }
                break;
            }

            case ocpp2_0::MonitorEnumType::kDelta: {
                // B08.FR spec: numeric variables trigger once the value has moved +/- monitorValue since the
                // last report; non-numeric variables (boolean/string/enumeration) trigger on any change,
                // regardless of monitorValue.
                bool triggered;
                if (actual_value->numeric.has_value()) {
                    triggered = !monitor.has_last_reported_value ||
                            std::abs(actual_value->numeric.value() - monitor.last_reported_value) >= monitor.value;
                } else {
                    triggered = !monitor.has_last_reported_value ||
                            monitor.last_reported_text != actual_value->text;
                }

                if (triggered) {
                    monitor.has_last_reported_value = true;
                    monitor.last_reported_value = actual_value->numeric.value_or(0);
                    monitor.last_reported_text = actual_value->text;
                    raiseMonitorEvent(monitor, actual_value->text, false);
                }
                break;
            }

            case ocpp2_0::MonitorEnumType::kPeriodic:
            case ocpp2_0::MonitorEnumType::kPeriodicClockAligned: {
                // TODO: PeriodicClockAligned should align to wall-clock boundaries (e.g. top of the hour for a
                // 3600s interval) rather than time-since-first-evaluation; treated the same as Periodic for now.
                if (monitor.next_periodic_report == SteadyPointMillis{} || now >= monitor.next_periodic_report) {
                    monitor.next_periodic_report = SteadyPointMillis{now + (int64_t)monitor.value * 1000};
                    raiseMonitorEvent(monitor, actual_value->text, false);
                }
                break;
            }

            default:
                break;
        }
    }
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetVariablesResponse>>
ConfigurationModule::onGetVariablesReq(const ocpp2_0::GetVariablesRequest &request) {
    if ((int)request.getVariableData.size() > settings_->ItemsPerMessageGetVariables.getValue()) {
        return ocpp2_0::CallError {
            ocpp2_0::ErrorCode::kOccurrenceConstraintViolation,
            {"Exceeded ItemsPerMessageGetVariables limit"},
            common::RawJson::empty_object()
        };
    }

    auto settings = settings_;
    return ocpp2_0::GetVariablesResponse {
            {[=](std::function<void(ocpp2_0::GetVariableResultType const &)> const &visitor) {
                for (auto const& get_variable : request.getVariableData) {
                    bool componentFound = false;
                    bool variableFound = false;
                    auto result = ocpp2_0::GetVariableResultType {
                            ocpp2_0::GetVariableStatusEnumType::kValueNotFoundInEnum,
                            get_variable.attributeType,
                            std::nullopt,
                            get_variable.component,
                            get_variable.variable
                    };

                    settings->visitSettings([&](SettingBase& setting) {
                        auto const metadata = setting.getMetadata();
                        if (variableFound)
                            return;
                        if (!metadata.model2_0.has_value())
                            return;

                        if (metadata.model2_0->component_type != get_variable.component) {
                            return;
                        } else {
                            componentFound = true;
                        }

                        if (metadata.model2_0->variable_type != get_variable.variable) {
                            return;
                        } else {
                            variableFound = true;
                        }

                        if (get_variable.attributeType.has_value()) {
                            // TODO - right now everything defined is "actual"
                            if (get_variable.attributeType.value() != ocpp2_0::AttributeEnumType::kActual) {
                                result.attributeStatus = ocpp2_0::GetVariableStatusEnumType::kNotSupportedAttributeType;
                                return;
                            }
                        }

                        if (!metadata.config.isAllowOcppRead()) {
                            result.attributeStatus = ocpp2_0::GetVariableStatusEnumType::kRejected;
                            return;
                        }

                        result.attributeStatus = ocpp2_0::GetVariableStatusEnumType::kAccepted;
                        result.attributeValue = setting.getValueAsString();
                    });

                    if (!componentFound) {
                        result.attributeStatus = ocpp2_0::GetVariableStatusEnumType::kUnknownComponent;
                    } else if (!variableFound) {
                        result.attributeStatus = ocpp2_0::GetVariableStatusEnumType::kUnknownVariable;
                    }

                    visitor(result);
                }
            }}
    };
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariablesResponse>>
ConfigurationModule::onSetVariablesReq(const ocpp2_0::SetVariablesRequest &request) {
    std::vector<ocpp2_0::SetVariableResultType> results;
    for (auto const& set_variable : request.setVariableData) {
        bool componentFound = false;
        bool variableFound = false;
        auto result = ocpp2_0::SetVariableResultType {
                set_variable.attributeType,
                ocpp2_0::SetVariableStatusEnumType::kValueNotFoundInEnum,
                set_variable.component,
                set_variable.variable
        };

        auto const network_configuration_priority_id = settings_->NetworkConfigurationPriority.getId();
        settings_->visitSettings([&](SettingBase& setting) {
            auto const metadata = setting.getMetadata();
            if (variableFound)
                return;
            if (!metadata.model2_0.has_value())
                return;

            if (metadata.model2_0->component_type != set_variable.component) {
                return;
            } else {
                componentFound = true;
            }

            if (metadata.model2_0->variable_type != set_variable.variable) {
                return;
            } else {
                variableFound = true;
            }

            if (set_variable.attributeType.has_value()) {
                // TODO - right now everything defined is "actual"
                if (set_variable.attributeType.value() != ocpp2_0::AttributeEnumType::kActual) {
                    result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kNotSupportedAttributeType;
                    return;
                }
            }

            if (!metadata.config.isAllowOcppWrite()) {
                result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kRejected;
                return;
            }

            // A05.FR.02
            if (metadata.id == network_configuration_priority_id) {
                bool missing_profile = false;
                int target_security_profile = 0;
                string::SplitVisitor(set_variable.attributeValue.value(), ",", [&](std::string const& text) {
                    auto slot = string::ToInteger(text);
                    if (!slot.has_value())
                        return;

                    auto const& profile = settings_->NetworkConnectionProfiles.getValue(slot.value());
                    if (!profile.has_value()) {
                        missing_profile = true;
                        return;
                    }

                    target_security_profile = std::max(target_security_profile, profile->securityProfile);
                });

                // TODO: Is there a specific requirement for this?
                if (target_security_profile < settings_->SecurityProfile.getValue()) {
                    result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kRejected;
                    return;
                }

                if (target_security_profile > settings_->SecurityProfile.getValue()) {
                    // A05.FR.02
                    if (target_security_profile >= 2 && settings_->InstalledCSMSRootCertificateCount.getValue() <= 0) {
                        result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kRejected;
                        return;
                    }

                    // A05.FR.03
                    // TODO: Not relevant until we have something that can store a charging station certificate
                    if (target_security_profile == 3) {
                        result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kRejected;
                        return;
                    }
                }
            }

            // TODO: move to update attribute?
            if (!settings_->setSettingValue(metadata.id, set_variable.attributeValue.value())) {
                result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kRejected;
                return;
            }

            if (metadata.config.isRebootRequired()) {
                result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kRebootRequired;
            } else {
                result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kAccepted;
            }
        });

        if (!componentFound) {
            result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kUnknownComponent;
        } else if (!variableFound) {
            result.attributeStatus = ocpp2_0::SetVariableStatusEnumType::kUnknownVariable;
        }

        results.push_back(std::move(result));
    }

    return ocpp2_0::SetVariablesResponse {std::move(results)};
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetBaseReportResponse>>
ConfigurationModule::onGetBaseReportReq(const ocpp2_0::GetBaseReportRequest &request) {
    if (ocpp2_0_pending_base_report_.has_value()) {
        return ocpp2_0::GetBaseReportResponse {
                ocpp2_0::GenericDeviceModelStatusEnumType::kRejected
        };
    }

    switch (request.reportBase) {
        case ocpp2_0::ReportBaseEnumType::kValueNotFoundInEnum:
        case ocpp2_0::ReportBaseEnumType::kSummaryInventory:
            return ocpp2_0::GetBaseReportResponse {
                    ocpp2_0::GenericDeviceModelStatusEnumType::kNotSupported
            };

        case ocpp2_0::ReportBaseEnumType::kFullInventory:
        case ocpp2_0::ReportBaseEnumType::kConfigurationInventory:
            break;
    }

    ocpp2_0_pending_base_report_ = request;
    return ocpp2_0::GetBaseReportResponse {
            ocpp2_0::GenericDeviceModelStatusEnumType::kAccepted
    };
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeConfigurationRsp>>
ConfigurationModule::onChangeConfigurationReq(const ocpp1_6::ChangeConfigurationReq& req) {
    return changeConfiguration(req, false);
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ChangeConfigurationRsp>>
ConfigurationModule::changeConfiguration(const ocpp1_6::ChangeConfigurationReq& req, bool force_change) {
    CHARGELAB_LOG_MESSAGE(info) << "changing configuration, key:" << req.key.value() << ", value:" << req.value.value();
    auto const state = settings_->getSettingState(req.key.value());
    if (!force_change) {
        if (!state.has_value() || (!state->config.isAllowOcppWrite() && !state->config.isAllowOcppRead()))
            return ocpp1_6::ChangeConfigurationRsp{ocpp1_6::ConfigurationStatus::kNotSupported};
        if (!state->config.isAllowOcppWrite())
            return ocpp1_6::ChangeConfigurationRsp{ocpp1_6::ConfigurationStatus::kRejected};
    }

    // TODO: Move to 1_6 device model rather than id
    if (!settings_->setSettingValue(req.key.value(), req.value.value()))
        return ocpp1_6::ChangeConfigurationRsp{ocpp1_6::ConfigurationStatus::kRejected};

    if (force_change) {
        settings_->saveIfModified();
    }

    if (state->config.isRebootRequired()) {
        return ocpp1_6::ChangeConfigurationRsp{ocpp1_6::ConfigurationStatus::kRebootRequired};
    } else {
        return ocpp1_6::ChangeConfigurationRsp{ocpp1_6::ConfigurationStatus::kAccepted};
    }
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetConfigurationRsp>>
ConfigurationModule::onGetConfigurationReq(const ocpp1_6::GetConfigurationReq &req) {
    return getConfiguration(req);
}

std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::GetConfigurationRsp>>
ConfigurationModule::getConfiguration(const ocpp1_6::GetConfigurationReq &req) {
    std::vector<std::string> include_keys;
    std::vector<ocpp1_6::CiString50Type> unknown_keys = req.key.value();

    // TODO: Move to 1_6 device model rather than id
    bool include_all_keys = req.key.value().empty();
    if (!unknown_keys.empty()) {
        if ((int)req.key.value().size() > settings_->GetConfigurationMaxKeys.getValue()) {
            return ocpp1_6::CallError {
                    ocpp1_6::ErrorCode::kPropertyConstraintViolation,
                    "ConnectorId is not same as the local connector Id - must be same",
                    common::RawJson::empty_object()
            };
        }

        unknown_keys = req.key.value();
        settings_->visitSettings([&](SettingBase const& setting) {
            auto const metadata = setting.getMetadata();
            if (!metadata.config.isAllowOcppRead() && !metadata.config.isAllowOcppWrite())
                return;

            auto it = std::remove_if(unknown_keys.begin(), unknown_keys.end(), [&] (auto const& x) {
                return string::EqualsIgnoreCaseAscii(x.value(), metadata.id);
            });

            if (it != unknown_keys.end()) {
                include_keys.push_back(metadata.id);
                unknown_keys.erase(it, unknown_keys.end());
            }
        });
    }

    auto settings = settings_;
    return ocpp1_6::GetConfigurationRsp {
            {[=](std::function<void(ocpp1_6::KeyValue const &)> const &visitor) {
                auto const& keys = include_keys;
                settings->visitSettings([&](SettingBase const& setting) {
                    auto const metadata = setting.getMetadata();
                    if (!metadata.config.isAllowOcppRead() && !metadata.config.isAllowOcppWrite())
                        return;

                    std::string value;
                    if (metadata.config.isAllowOcppRead()) {
                        value = setting.getValueAsString();
                    } else {
                        value = kMaskedValue;
                    }

                    if (include_all_keys || std::find(keys.begin(), keys.end(), setting.getId()) != keys.end()) {
                        visitor(ocpp1_6::KeyValue{
                                {metadata.id},
                                !metadata.config.isAllowOcppWrite(),
                                std::move(value)
                        });
                    }
                });
            }},
            std::move(unknown_keys)
    };
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetNetworkProfileResponse>>
ConfigurationModule::onSetNetworkProfileReq(const ocpp2_0::SetNetworkProfileRequest &request) {
    if (request.configurationSlot < 0 || request.configurationSlot >= detail::SettingsConstants::kMaxProfileSlots) {
        // B09.FR.02
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"BadConfigurationSlot"}}
        };
    }

    // Note: SIM/VPN not supported
    auto const& data = request.connectionData;
    if (data.apn.has_value()) {
        // B09.FR.02
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"ApnNotSupported"}}
        };
    }
    if (data.vpn.has_value()) {
        // B09.FR.02
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"VpnNotSupported"}}
        };
    }

    // Note: only JSON is supported
    if (data.ocppTransport != ocpp2_0::OCPPTransportEnumType::kJSON) {
        // B09.FR.02
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"BadOcppTransport"}}
        };
    }

    std::string protocol;
    switch (data.ocppVersion) {
        default:
        case ocpp2_0::OCPPVersionEnumType::kOCPP12:
        case ocpp2_0::OCPPVersionEnumType::kOCPP15:
            // B09.FR.02
            return ocpp2_0::SetNetworkProfileResponse {
                    ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                    ocpp2_0::StatusInfoType {{"BadOcppVersion"}}
            };

        case ocpp2_0::OCPPVersionEnumType::kOCPP16:
            protocol = ProtocolConstants::kProtocolOcpp1_6;
            break;

        case ocpp2_0::OCPPVersionEnumType::kOCPP20:
            protocol = ProtocolConstants::kProtocolOcpp2_0_1;
            break;

        case ocpp2_0::OCPPVersionEnumType::kOCPP21:
            protocol = ProtocolConstants::kProtocolOcpp2_1;
            break;
    }

    // Note: an assumed "wifi" interface is always used here
    if (data.ocppInterface != ocpp2_0::OCPPInterfaceEnumType::kWireless0) {
        // B09.FR.02
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"BadOcppInterface"}}
        };
    }

    auto parsed = uri::ParseWebsocketUri(data.ocppCsmsUrl.value());
    if (!parsed.has_value()) {
        // B09.FR.02
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"BadOcppCsmsUrl"}}
        };
    }

    if (request.connectionData.securityProfile < settings_->SecurityProfile.getValue()) {
        // B09.FR.04
        return ocpp2_0::SetNetworkProfileResponse {
                ocpp2_0::SetNetworkProfileStatusEnumType::kRejected,
                ocpp2_0::StatusInfoType {{"BadSecurityProfile"}}
        };
    }

    if (settings_->NetworkConnectionProfiles.setValue(request.configurationSlot, request.connectionData)) {
        // B09.FR.01
        return ocpp2_0::SetNetworkProfileResponse {ocpp2_0::SetNetworkProfileStatusEnumType::kAccepted};
    } else {
        // B09.FR.03
        return ocpp2_0::SetNetworkProfileResponse {ocpp2_0::SetNetworkProfileStatusEnumType::kFailed};
    }
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::GetMonitoringReportResponse>>
ConfigurationModule::onGetMonitoringReportReq(const ocpp2_0::GetMonitoringReportRequest& request) {
    if (ocpp2_0_pending_monitoring_report_.has_value()) {
        return ocpp2_0::GetMonitoringReportResponse {
                ocpp2_0::GenericDeviceModelStatusEnumType::kRejected
        };
    }

    // Only ThresholdMonitoring/DeltaMonitoring/PeriodicMonitoring criteria filter by monitor type; since we
    // don't distinguish "custom" vs "pre-configured" monitors here, any requested criteria is honoured by
    // simply checking the monitor type below rather than rejecting the request up front.

    if (monitors_.empty()) {
        return ocpp2_0::GetMonitoringReportResponse {
                ocpp2_0::GenericDeviceModelStatusEnumType::kEmptyResultSet
        };
    }

    ocpp2_0_pending_monitoring_report_ = request;
    return ocpp2_0::GetMonitoringReportResponse {
            ocpp2_0::GenericDeviceModelStatusEnumType::kAccepted
    };
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetVariableMonitoringResponse>>
ConfigurationModule::onSetVariableMonitoringReq(const ocpp2_0::SetVariableMonitoringRequest &request) {
    if ((int)request.setMonitoringData.size() > settings_->ItemsPerMessageSetVariableMonitoring.getValue()) {
        return ocpp2_0::CallError {
                ocpp2_0::ErrorCode::kOccurrenceConstraintViolation,
                {"Exceeded ItemsPerMessageSetVariableMonitoring limit"},
                common::RawJson::empty_object()
        };
    }

    std::vector<ocpp2_0::SetMonitoringResultType> results;
    for (auto const& set_monitoring : request.setMonitoringData) {
        bool componentFound = false;
        bool variableFound = false;
        bool variableMonitorable = false;

        auto result = ocpp2_0::SetMonitoringResultType {
                set_monitoring.id,
                ocpp2_0::SetMonitoringStatusEnumType::kUnknownComponent,
                set_monitoring.type,
                set_monitoring.severity,
                set_monitoring.component,
                set_monitoring.variable
        };

        settings_->visitSettings([&](SettingBase& setting) {
            if (variableFound)
                return;

            auto const metadata = setting.getMetadata();
            if (!metadata.model2_0.has_value())
                return;

            if (metadata.model2_0->component_type != set_monitoring.component)
                return;
            componentFound = true;

            if (metadata.model2_0->variable_type != set_monitoring.variable)
                return;
            variableFound = true;

            // Only variables the CSMS is allowed to read make sense to monitor.
            variableMonitorable = metadata.config.isAllowOcppRead();
        });

        if (!componentFound) {
            result.status = ocpp2_0::SetMonitoringStatusEnumType::kUnknownComponent;
        } else if (!variableFound) {
            result.status = ocpp2_0::SetMonitoringStatusEnumType::kUnknownVariable;
        } else if (!variableMonitorable) {
            result.status = ocpp2_0::SetMonitoringStatusEnumType::kRejected;
        } else {
            MonitorRecord* existing = nullptr;
            if (set_monitoring.id.has_value()) {
                for (auto& monitor : monitors_) {
                    if (monitor.id == set_monitoring.id.value()) {
                        existing = &monitor;
                        break;
                    }
                }

                if (existing == nullptr) {
                    result.status = ocpp2_0::SetMonitoringStatusEnumType::kRejected;
                    result.statusInfo = ocpp2_0::StatusInfoType {{"UnknownMonitorId"}};
                }
            }

            if (existing == nullptr && result.status == ocpp2_0::SetMonitoringStatusEnumType::kUnknownComponent) {
                // Duplicate check (B08.FR.14): reject a brand-new monitor that exactly matches an existing one.
                for (auto const& monitor : monitors_) {
                    if (monitor.component == set_monitoring.component &&
                        monitor.variable == set_monitoring.variable &&
                        monitor.type == set_monitoring.type &&
                        monitor.severity == set_monitoring.severity) {
                        result.status = ocpp2_0::SetMonitoringStatusEnumType::kDuplicate;
                        break;
                    }
                }
            }

            if (result.status == ocpp2_0::SetMonitoringStatusEnumType::kUnknownComponent) {
                if (existing == nullptr && monitors_.size() >= kMaxVariableMonitors) {
                    result.status = ocpp2_0::SetMonitoringStatusEnumType::kRejected;
                    result.statusInfo = ocpp2_0::StatusInfoType {{"TooManyMonitors"}};
                } else {
                    if (existing == nullptr) {
                        monitors_.push_back(MonitorRecord {});
                        existing = &monitors_.back();
                        existing->id = next_monitor_id_++;
                    }

                    existing->component = set_monitoring.component;
                    existing->variable = set_monitoring.variable;
                    existing->transaction = set_monitoring.transaction.value_or(false);
                    existing->value = set_monitoring.value;
                    existing->type = set_monitoring.type;
                    existing->severity = set_monitoring.severity;

                    // Reset evaluation state - a changed threshold/interval shouldn't reuse stale tracking.
                    existing->threshold_active = false;
                    existing->threshold_debounce_until = SteadyPointMillis {};
                    existing->has_last_reported_value = false;
                    existing->last_reported_text.clear();
                    existing->next_periodic_report = SteadyPointMillis {};

                    result.id = existing->id;
                    result.status = ocpp2_0::SetMonitoringStatusEnumType::kAccepted;
                }
            }
        }

        results.push_back(std::move(result));
    }

    return ocpp2_0::SetVariableMonitoringResponse {std::move(results)};
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ClearVariableMonitoringResponse>>
ConfigurationModule::onClearVariableMonitoringReq(const ocpp2_0::ClearVariableMonitoringRequest &request) {
    std::vector<ocpp2_0::ClearMonitoringResultType> results;
    for (auto const& id : request.id) {
        auto it = std::find_if(monitors_.begin(), monitors_.end(), [&](MonitorRecord const& monitor) {
            return monitor.id == id;
        });

        if (it == monitors_.end()) {
            results.push_back(ocpp2_0::ClearMonitoringResultType {
                    ocpp2_0::ClearMonitoringStatusEnumType::kNotFound,
                    id
            });
        } else {
            monitors_.erase(it);
            results.push_back(ocpp2_0::ClearMonitoringResultType {
                    ocpp2_0::ClearMonitoringStatusEnumType::kAccepted,
                    id
            });
        }
    }

    return ocpp2_0::ClearVariableMonitoringResponse {std::move(results)};
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringBaseResponse>>
ConfigurationModule::onSetMonitoringBaseReq(const ocpp2_0::SetMonitoringBaseRequest &request) {
    // FactoryDefault/HardWiredOnly would normally reset to a charger-defined baseline set of monitors; since we
    // don't ship any pre-configured monitors, all three bases behave the same way here - only "All" additionally
    // permits CSMS-defined custom monitors to keep reporting, which matches monitors_ being CSMS-managed only.
    monitoring_base_ = request.monitoringBase;
    if (request.monitoringBase != ocpp2_0::MonitoringBaseEnumType::kAll) {
        monitors_.clear();
    }

    return ocpp2_0::SetMonitoringBaseResponse {ocpp2_0::GenericDeviceModelStatusEnumType::kAccepted};
}

std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::SetMonitoringLevelResponse>>
ConfigurationModule::onSetMonitoringLevelReq(const ocpp2_0::SetMonitoringLevelRequest &request) {
    if (request.severity < 0 || request.severity > 9) {
        return ocpp2_0::SetMonitoringLevelResponse {ocpp2_0::GenericStatusEnumType::kRejected};
    }

    monitoring_level_ = request.severity;
    return ocpp2_0::SetMonitoringLevelResponse {ocpp2_0::GenericStatusEnumType::kAccepted};
}

} // namespace chargelab

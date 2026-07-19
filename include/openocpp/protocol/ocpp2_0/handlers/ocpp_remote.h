#ifndef CHARGELAB_OPEN_FIRMWARE_2_0_OCPP_REMOTE_H
#define CHARGELAB_OPEN_FIRMWARE_2_0_OCPP_REMOTE_H

#include "openocpp/protocol/ocpp2_0/messages/authorize.h"
#include "openocpp/protocol/ocpp2_0/messages/boot_notification.h"
#include "openocpp/protocol/ocpp2_0/messages/cancel_reservation.h"
#include "openocpp/protocol/ocpp2_0/messages/certificate_signed.h"
#include "openocpp/protocol/ocpp2_0/messages/change_availability.h"
#include "openocpp/protocol/ocpp2_0/messages/clear_cache.h"
#include "openocpp/protocol/ocpp2_0/messages/clear_charging_profile.h"
#include "openocpp/protocol/ocpp2_0/messages/clear_display_message.h"
#include "openocpp/protocol/ocpp2_0/messages/clear_variable_monitoring.h"
#include "openocpp/protocol/ocpp2_0/messages/cleared_charging_limit.h"
#include "openocpp/protocol/ocpp2_0/messages/cost_updated.h"
#include "openocpp/protocol/ocpp2_0/messages/customer_information.h"
#include "openocpp/protocol/ocpp2_0/messages/data_transfer.h"
#include "openocpp/protocol/ocpp2_0/messages/delete_certificate.h"
#include "openocpp/protocol/ocpp2_0/messages/firmware_status_notification.h"
#include "openocpp/protocol/ocpp2_0/messages/get_15118_ev_certificate.h"
#include "openocpp/protocol/ocpp2_0/messages/get_base_report.h"
#include "openocpp/protocol/ocpp2_0/messages/get_certificate_status.h"
#include "openocpp/protocol/ocpp2_0/messages/get_charging_profiles.h"
#include "openocpp/protocol/ocpp2_0/messages/get_composite_schedule.h"
#include "openocpp/protocol/ocpp2_0/messages/get_display_messages.h"
#include "openocpp/protocol/ocpp2_0/messages/get_installed_certificate_ids.h"
#include "openocpp/protocol/ocpp2_0/messages/get_local_list_version.h"
#include "openocpp/protocol/ocpp2_0/messages/get_log.h"
#include "openocpp/protocol/ocpp2_0/messages/get_monitoring_report.h"
#include "openocpp/protocol/ocpp2_0/messages/get_report.h"
#include "openocpp/protocol/ocpp2_0/messages/get_transaction_status.h"
#include "openocpp/protocol/ocpp2_0/messages/get_variables.h"
#include "openocpp/protocol/ocpp2_0/messages/heartbeat.h"
#include "openocpp/protocol/ocpp2_0/messages/install_certificate.h"
#include "openocpp/protocol/ocpp2_0/messages/log_status_notification.h"
#include "openocpp/protocol/ocpp2_0/messages/meter_values.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_charging_limit.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_customer_information.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_display_messages.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_ev_charging_needs.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_ev_charging_schedule.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_event.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_monitoring_report.h"
#include "openocpp/protocol/ocpp2_0/messages/notify_report.h"
#include "openocpp/protocol/ocpp2_0/messages/publish_firmware.h"
#include "openocpp/protocol/ocpp2_0/messages/publish_firmware_status_notification.h"
#include "openocpp/protocol/ocpp2_0/messages/report_charging_profiles.h"
#include "openocpp/protocol/ocpp2_0/messages/request_start_transaction.h"
#include "openocpp/protocol/ocpp2_0/messages/request_stop_transaction.h"
#include "openocpp/protocol/ocpp2_0/messages/reservation_status_update.h"
#include "openocpp/protocol/ocpp2_0/messages/reserve_now.h"
#include "openocpp/protocol/ocpp2_0/messages/reset.h"
#include "openocpp/protocol/ocpp2_0/messages/security_event_notification.h"
#include "openocpp/protocol/ocpp2_0/messages/send_local_list.h"
#include "openocpp/protocol/ocpp2_0/messages/set_charging_profile.h"
#include "openocpp/protocol/ocpp2_0/messages/set_display_message.h"
#include "openocpp/protocol/ocpp2_0/messages/set_monitoring_base.h"
#include "openocpp/protocol/ocpp2_0/messages/set_monitoring_level.h"
#include "openocpp/protocol/ocpp2_0/messages/set_network_profile.h"
#include "openocpp/protocol/ocpp2_0/messages/set_variable_monitoring.h"
#include "openocpp/protocol/ocpp2_0/messages/set_variables.h"
#include "openocpp/protocol/ocpp2_0/messages/sign_certificate.h"
#include "openocpp/protocol/ocpp2_0/messages/status_notification.h"
#include "openocpp/protocol/ocpp2_0/messages/transaction_event.h"
#include "openocpp/protocol/ocpp2_0/messages/trigger_message.h"
#include "openocpp/protocol/ocpp2_0/messages/unlock_connector.h"
#include "openocpp/protocol/ocpp2_0/messages/unpublish_firmware.h"
#include "openocpp/protocol/ocpp2_0/messages/update_firmware.h"
#include "openocpp/protocol/ocpp2_0/types/action_id.h"
#include "openocpp/protocol/ocpp2_0/types/message_type.h"
#include "openocpp/protocol/ocpp2_0/types/call_error.h"
#include "openocpp/protocol/common/raw_json.h"
#include "openocpp/interface/element/websocket_interface.h"

#include <utility>
#include <random>

namespace chargelab::ocpp2_0 {
    class OcppRemote {
    private:
        using OnManagedCall = std::function<bool(std::string const&, ActionId const&)>;

    public:
        explicit OcppRemote(
                WebsocketInterface& websocket_interface,
                std::function<bool()> registration_complete,
                OnManagedCall on_managed_message
        )
            : websocket_interface_(websocket_interface),
              registration_complete_(std::move(registration_complete)),
              on_managed_message_(std::move(on_managed_message))
        {
            std::random_device random_device;
            std::default_random_engine random_engine {random_device()};
            std::uniform_int_distribution<unsigned long> distribution(
                    std::numeric_limits<unsigned long>::min(),
                    std::numeric_limits<unsigned long>::max()
            );
            request_id_ = distribution(random_engine);
        }

    public:
#define CHARGELAB_REQUEST_HANDLER_TEMPLATE(type) \
        std::optional<std::string> send ## type ## Req(const ocpp2_0::type ## Request &req) { \
            return executeCall(ocpp2_0::ActionId::k ## type, req); \
        }
        CHARGELAB_PASTE(CHARGELAB_REQUEST_HANDLER_TEMPLATE, CHARGELAB_OCPP_2_0_ACTION_IDS)
#undef CHARGELAB_REQUEST_HANDLER_TEMPLATE

        template <typename T>
        std::optional<std::string> sendCall(T const& req) {
            return executeCall(T::kActionId, req);
        }

        bool sendCall(std::string const& unique_id, ActionId const& action, std::string const& payload) {
            if (!websocket_interface_.isConnected())
                return false;

            if (!registration_complete_()) {
                switch (action) {
                    default:
                        CHARGELAB_LOG_MESSAGE(info) << "Registration not complete - blocking call: " << action;
                        return false;

                        // Allow these requests to be sent while registration is pending
                    case ActionId::kBootNotification:
                    case ActionId::kNotifyReport:
                        break;
                }
            }

            if (!on_managed_message_(unique_id, action))
                return false;

            websocket_interface_.sendCustom([&](ByteWriterInterface& stream) {
                json::JsonWriter writer {stream};
                writer.StartArray();
                writer.Int(MessageType::kCall);
                writer.String(unique_id);
                writer.String(action.to_string());
                json::WriteValue<common::RawJson>::write_json(writer,  common::RawJson{payload});
                writer.EndArray();
            });
            return true;
        }

        // OCPP 2.1 SEND (message type 6): a fire-and-forget message shaped like a CALL but with no
        // CALLRESULT expected in reply. Not registered as a pending call.
        template <typename T>
        std::optional<std::string> sendSend(T const& payload) {
            return executeSend(T::kActionId, payload);
        }

        std::optional<std::string> sendSend(ActionId const& action, std::string const& payload) {
            return executeSend(action, common::RawJson{payload});
        }

        // OCPP 2.1 CALLRESULTERROR (message type 5): sent to reject a CALLRESULT we received but could
        // not process. This is a reply, so it is not gated on registration and is not tracked as a call.
        bool sendCallResultError(std::string const& unique_id, CallError const& error) {
            if (!websocket_interface_.isConnected())
                return false;

            CHARGELAB_TRY {
                websocket_interface_.sendCustom([&](ByteWriterInterface& stream) {
                    json::JsonWriter writer {stream};
                    writer.StartArray();
                    writer.Int((int)MessageType::kCallResultError);
                    writer.String(unique_id);
                    json::WriteValue<ErrorCode>::write_json(writer, error.code);
                    writer.String(error.description.value());
                    json::WriteValue<common::RawJson>::write_json(writer, error.details);
                    writer.EndArray();
                });
                return true;
            } CHARGELAB_CATCH {
                CHARGELAB_LOG_MESSAGE(error) << "Failed sending call result error with: " << e.what();
                return false;
            }
        }

        void sendUnmanagedMessage(std::function<void(ByteWriterInterface&)> payload) {
            websocket_interface_.sendCustom(std::move(payload));
        }

    private:
        // Templated on the action-id type so it accepts both ocpp2_0::ActionId and the
        // OCPP 2.1 action ids (ocpp2_1::ActionId) - a SEND only needs action.to_string().
        template <typename ActionT, typename T>
        std::optional<std::string> executeSend(ActionT const& action, T const& payload) {
            if (!websocket_interface_.isConnected())
                return std::nullopt;

            // SEND messages are never boot-critical, so they are simply gated on
            // registration being complete (no per-action exceptions like executeCall).
            if (!registration_complete_()) {
                CHARGELAB_LOG_MESSAGE(info) << "Registration not complete - blocking send: " << action.to_string();
                return std::nullopt;
            }

            CHARGELAB_TRY {
                // A SEND still carries a message id per the OCPP-J framing, but no response is expected
                // so it is not registered with on_managed_message_ / tracked as a pending call.
                auto unique_id = std::to_string(request_id_++);

                websocket_interface_.sendCustom([&](ByteWriterInterface& stream) {
                    json::JsonWriter writer {stream};
                    writer.StartArray();
                    writer.Int(MessageType::kSend);
                    writer.String(unique_id);
                    writer.String(action.to_string());
                    json::WriteValue<T>::write_json(writer, payload);
                    writer.EndArray();
                });

                return unique_id;
            }  CHARGELAB_CATCH {
                CHARGELAB_LOG_MESSAGE(error) << "Failed sending send with: " << e.what();
                return std::nullopt;
            }
        }

        template <typename T>
        std::optional<std::string> executeCall(ActionId const& action, T const& payload) {
            if (!websocket_interface_.isConnected())
                return std::nullopt;

            if (!registration_complete_()) {
                switch (action) {
                    default:
                        CHARGELAB_LOG_MESSAGE(info) << "Registration not complete - blocking call: " << action;
                        return std::nullopt;

                    // Allow these requests to be sent while registration is pending
                    case ActionId::kBootNotification:
                    case ActionId::kNotifyReport:
                        break;
                }
            }

            CHARGELAB_TRY {
                auto unique_id = std::to_string(request_id_++);
                if (!on_managed_message_(unique_id, action))
                    return std::nullopt;

                websocket_interface_.sendCustom([&](ByteWriterInterface& stream) {
                    json::JsonWriter writer {stream};
                    writer.StartArray();
                    writer.Int(MessageType::kCall);
                    writer.String(unique_id);
                    writer.String(action.to_string());
                    json::WriteValue<T>::write_json(writer, payload);
                    writer.EndArray();
                });

                return unique_id;
            }  CHARGELAB_CATCH {
                CHARGELAB_LOG_MESSAGE(error) << "Failed sending call with: " << e.what();
                return std::nullopt;
            }
        }

    private:
        WebsocketInterface& websocket_interface_;
        std::function<bool()> registration_complete_;
        OnManagedCall on_managed_message_;

        unsigned long request_id_;
    };
}

#undef CHARGELAB_MERGE_NAMES
#undef CHARGELAB_REQUEST_HANDLER_TEMPLATE

#endif //CHARGELAB_OPEN_FIRMWARE_2_0_OCPP_REMOTE_H

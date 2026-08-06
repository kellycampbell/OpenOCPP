#ifndef CHARGELAB_OPEN_FIRMWARE_FIRMWARE_UPDATE_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_FIRMWARE_UPDATE_MODULE_H

#include "openocpp/interface/component/system_interface.h"
#include "openocpp/interface/component/fetch_interface.h"
#include "openocpp/interface/platform_interface.h"
#include "openocpp/common/operation_holder.h"
#include "openocpp/module/common_templates.h"
#include "openocpp/module/pending_messages_module.h"
#include "openocpp/module/connector_status_module.h"
#include "openocpp/module/reset_module.h"
#include "openocpp/common/settings.h"
#include "openocpp/common/logging.h"
#include "openocpp/common/macro.h"

#include <thread>
#include <condition_variable>
#include <charconv>

namespace chargelab {
    namespace detail {
        template <typename HM>
        struct FirmwareUpdateOperation {
            ocpp2_0::UpdateFirmwareRequest request;

            // for ocpp1.6 only
            std::optional<ocpp1_6::FirmwareStatus> last_status1_6 = std::nullopt;

            // L01.FR.04
            std::unique_ptr<typename HM::hash_calculator_type> signature_hash = HM::createCalculator(HM::kHashTypeSHA256);
            std::shared_ptr<RestConnectionInterface> connection = nullptr;
            std::vector<uint8_t> buffer {};
            std::size_t content_length = 0;
            std::size_t total_bytes_read = 0;

            // Consecutive failures - reset whenever the transfer makes forward progress, so
            // a long download over a marginal link isn't killed by unrelated blips spread
            // across the whole operation. The operation is bounded by deadline instead.
            std::size_t total_failures = 0;
            std::optional<SteadyPointMillis> next_attempt_time = std::nullopt;
            std::optional<SteadyPointMillis> deadline = std::nullopt;

            std::vector<std::vector<uint8_t>> block_hashes {};
            std::optional<std::vector<uint8_t>> expected_signature_hash = std::nullopt;
            std::optional<ocpp2_0::FirmwareStatusEnumType> last_status2_0 = std::nullopt;

            bool finished_signature_check = false;
            bool finished_flashing_firmware = false;
            bool running_firmware_update = false;
        };
    }

    template <typename HM>
    class FirmwareUpdateModule : public ServiceStatefulGeneral {
    private:
        static constexpr int kChunkSize = 20*1024;
        static constexpr int kPriorityFirmwareStatusNotification = 100;

        // Retry pacing used when the CSMS didn't specify a retryInterval: exponential
        // backoff from kBaseRetryDelaySeconds, capped at kMaxRetryDelaySeconds.
        static constexpr int kBaseRetryDelaySeconds = 10;
        static constexpr int kMaxRetryDelaySeconds = 5*60;
        static constexpr int kMaxBackoffShift = 5;

        // Overall bound on a single update operation, started when the first transfer
        // attempt is made. Replaces the attempt count as the thing that stops an update
        // that can never succeed, now that failures reset on forward progress.
        static constexpr std::int64_t kOperationTimeoutMillis = 60*60*1000;

        // Note: arbitrary random assigned ID
        static constexpr std::uint64_t kOperationGroupId = 0x78106033AC8E780Aull;
    public:
        explicit FirmwareUpdateModule(
                std::shared_ptr<PlatformInterface> platform,
                std::shared_ptr<ResetModule> reset,
                std::shared_ptr<PendingMessagesModule> pending_messages,
                std::shared_ptr<ConnectorStatusModule> connector_status,
                std::shared_ptr<StationInterface> station
        )
            : platform_(std::move(platform)),
              reset_(std::move(reset)),
              pending_messages_(std::move(pending_messages)),
              connector_status_(std::move(connector_status)),
              station_(std::move(station))
        {
            assert(platform_ != nullptr);
            assert(station_ != nullptr);

            settings_ = platform_->getSettings();
            assert(settings_ != nullptr);
        }

        ~FirmwareUpdateModule() override {
            CHARGELAB_LOG_MESSAGE(debug) << "Deleting FirmwareUpdateModule";
        }

    private:
        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::UpdateFirmwareRsp>>
        onUpdateFirmwareReq(const ocpp1_6::UpdateFirmwareReq &req) override {
            if (operation_.has_value()) {
                return ocpp1_6::CallError {
                        ocpp1_6::ErrorCode::kGenericError,
                        "InProgress",
                        common::RawJson::empty_object()
                };
            }

            // Check if there is transaction in progress
            bool found_charging_connector = false;
            for (auto const& entry: station_->getConnectorMetadata()) {
                if (entry.first.id == 0) continue;
                auto const connector_status = station_->pollConnectorStatus(entry.first);
                if (!connector_status.has_value())
                    continue;
                if (connector_status->charging_enabled) {
                    found_charging_connector = true;
                    break;
                }
            }

            if (found_charging_connector) {
                return ocpp1_6::CallError {
                        ocpp1_6::ErrorCode::kGenericError,
                        "ActiveChargeSession",
                        common::RawJson::empty_object()
                };
            }

            // Set all connectors to Unavailable
            need_to_restore_connector_0_operative_ = connector_status_->setConnector0Inoperative(true);

            operation_ = detail::FirmwareUpdateOperation<HM> {
                ocpp2_0::UpdateFirmwareRequest {
                    req.retries,
                    req.retryInterval,
                    0,
                    ocpp2_0::FirmwareType {
                        req.location.value(),
                        optional::GetOrDefault(req.retrieveDate.getTimestamp(), platform_->systemClockNow())
                    }
                }
            };

            return ocpp1_6::UpdateFirmwareRsp {};
        }

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::UpdateFirmwareResponse>>
        onUpdateFirmwareReq(const ocpp2_0::UpdateFirmwareRequest &request) override {
            // Note: there doesn't appear to be any specific requirement here in the 2.0.1 specification and this is
            // incompatible with a station simultaneously supporting L02...but this is required by TC_L_18_CS which is
            // mandatory for core certification.
            if (!request.firmware.signature.has_value()) {
                return ocpp2_0::UpdateFirmwareResponse{
                    ocpp2_0::UpdateFirmwareStatusEnumType::kRejected,
                    ocpp2_0::StatusInfoType {"MissingSignature"}
                };
            }
            if (!request.firmware.signingCertificate.has_value()) {
                return ocpp2_0::UpdateFirmwareResponse{
                        ocpp2_0::UpdateFirmwareStatusEnumType::kRejected,
                        ocpp2_0::StatusInfoType {"MissingSigningCert"}
                };
            }

            // L01.FR.21
            // L01.FR.22
            if (request.firmware.signingCertificate.has_value()) {
                if (!platform_->verifyManufacturerCertificate(request.firmware.signingCertificate->value(), std::nullopt)) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Bad manufacturer certificate";

                    // L01.FR.02
                    pending_messages_->sendRequest2_0(
                            ocpp2_0::SecurityEventNotificationRequest {
                                    "InvalidFirmwareSigningCertificate",
                                    platform_->systemClockNow()
                            },
                            buildPendingMessagePolicy(PendingMessageType::kSecurityEvent)
                    );

                    return ocpp2_0::UpdateFirmwareResponse{ocpp2_0::UpdateFirmwareStatusEnumType::kInvalidCertificate};
                }
            }

            // Note: this isn't strictly necessary, however passing a free-form text string to the platform URI parsing
            // method seems potentially dangerous.
            if (!uri::parseHttpUri(request.firmware.location.value()).has_value()) {
                CHARGELAB_LOG_MESSAGE(warning) << "Bad firmware location: " << request.firmware.location.value();
                return ocpp2_0::UpdateFirmwareResponse{ocpp2_0::UpdateFirmwareStatusEnumType::kRejected};
            }

            auto status = ocpp2_0::UpdateFirmwareStatusEnumType::kAccepted;
            if (operation_.has_value()) {
                // L01.FR.24
                operation_ = std::nullopt;
                status = ocpp2_0::UpdateFirmwareStatusEnumType::kAcceptedCanceled;
            }

            operation_ = detail::FirmwareUpdateOperation<HM>{request};
            return ocpp2_0::UpdateFirmwareResponse{status};
        }

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::TriggerMessageRsp>>
        onTriggerMessageReq(const ocpp1_6::TriggerMessageReq&) override {
            return std::nullopt;
        }

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::TriggerMessageResponse>>
        onTriggerMessageReq(const ocpp2_0::TriggerMessageRequest &req) override {
            if (req.requestedMessage != ocpp2_0::MessageTriggerEnumType::kFirmwareStatusNotification)
                return std::nullopt;

            if (req.evse.has_value()) {
                force_update_ = false;
            } else {
                force_update_ = true;
            }

            return ocpp2_0::TriggerMessageResponse {ocpp2_0::TriggerMessageStatusEnumType::kAccepted};
        }

        void checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType status) {
            if (operation_->last_status2_0 == std::make_optional(status))
                return;

            pending_messages_->sendRequest2_0(
                    ocpp2_0::FirmwareStatusNotificationRequest {
                        status,
                        operation_->request.requestId
                    },
                    buildPendingMessagePolicy(PendingMessageType::kNotificationEvent)
            );

            operation_->last_status2_0 = status;
        }

        void checkAndUpdateStatus(ocpp1_6::FirmwareStatus status) {
            if (operation_->last_status1_6 == std::make_optional(status))
                return;

            pending_messages_->sendRequest1_6(
                    ocpp1_6::FirmwareStatusNotificationReq {
                            status
                    },
                    buildPendingMessagePolicy(PendingMessageType::kNotificationEvent)
            );

            operation_->last_status1_6 = status;
        }


        /**
         * Records a failed attempt and schedules when the next one may start. The CSMS
         * supplied retryInterval is honoured when present; otherwise the delay backs off
         * exponentially. Without this the retry budget was spent in a handful of
         * milliseconds and any transient outage looked permanent.
         */
        static void recordFailure(
                std::shared_ptr<PlatformInterface> const& platform,
                detail::FirmwareUpdateOperation<HM>& operation
        ) {
            operation.total_failures++;
            operation.connection = nullptr;

            int delay_seconds = optional::GetOrDefault(operation.request.retryInterval, 0);
            if (delay_seconds <= 0) {
                auto const shift = std::min<std::size_t>(operation.total_failures - 1, kMaxBackoffShift);
                delay_seconds = std::min(kBaseRetryDelaySeconds << shift, kMaxRetryDelaySeconds);
            }

            operation.next_attempt_time = static_cast<SteadyPointMillis>(
                    platform->steadyClockNow() + (std::int64_t)delay_seconds*1000
            );
            CHARGELAB_LOG_MESSAGE(info) << "Firmware update attempt failed (" << operation.total_failures
                    << " consecutive) - retrying in " << delay_seconds << "s";
        }

        /**
         * Records that the transfer moved forward, clearing the consecutive failure count
         * so a long download over a marginal link can still complete.
         */
        static void recordProgress(detail::FirmwareUpdateOperation<HM>& operation) {
            operation.total_failures = 0;
            operation.next_attempt_time = std::nullopt;
        }

        /**
         * @return true if the operation should be abandoned - either too many consecutive
         *         failures, or the operation as a whole has run out of time.
         */
        bool operationExhausted(detail::FirmwareUpdateOperation<HM>& operation, int max_retries) {
            auto const now = platform_->steadyClockNow();
            if (!operation.deadline.has_value()) {
                operation.deadline = static_cast<SteadyPointMillis>(now + kOperationTimeoutMillis);
            }

            if ((int)operation.total_failures > max_retries) {
                return true;
            }

            if (now - operation.deadline.value() >= 0) {
                CHARGELAB_LOG_MESSAGE(warning) << "Firmware update operation timed out - abandoning";
                return true;
            }

            return false;
        }

        /**
         * @return true if the operation is waiting out a retry backoff.
         */
        bool waitingForRetry(detail::FirmwareUpdateOperation<HM>& operation) {
            if (!operation.next_attempt_time.has_value()) {
                return false;
            }

            if (platform_->steadyClockNow() - operation.next_attempt_time.value() < 0) {
                return true;
            }

            operation.next_attempt_time = std::nullopt;
            return false;
        }

        static bool checkOrRetryConnection(
                std::shared_ptr<PlatformInterface> const& platform,
                detail::FirmwareUpdateOperation<HM>& operation
        ) {
            if (operation.connection != nullptr)
                return true;

            auto const uri = operation.request.firmware.location.value();
            CHARGELAB_LOG_MESSAGE(info) << "Downloading firmware from: " << uri;
            operation.connection = platform->getRequest(uri);
            if (operation.connection == nullptr) {
                CHARGELAB_LOG_MESSAGE(warning) << "Failed establishing connection to: " << uri;
                recordFailure(platform, operation);
                return false;
            }

            if (!operation.connection->open(0)) {
                CHARGELAB_LOG_MESSAGE(warning) << "Failed opening connection to server";
                recordFailure(platform, operation);
                return false;
            }

            if (!operation.connection->send()) {
                CHARGELAB_LOG_MESSAGE(warning) << "Failed sending request";
                recordFailure(platform, operation);
                return false;
            }

            auto const status = operation.connection->getStatusCode();
            if (!(status >= 200 && status < 300)) {
                CHARGELAB_LOG_MESSAGE(warning) << "Bad status code - expecting 2xx response: " << status;
                recordFailure(platform, operation);
                return false;
            }

            operation.content_length = operation.connection->getContentLength();
            operation.total_bytes_read = 0;
            return true;
        }

        void runStep(ocpp1_6::OcppRemote &) override {
            performBootChecks1_6();
            performFirmwareUpdate1_6();
        }

        void runStep(ocpp2_0::OcppRemote& remote) override {
            performBootChecks2_0();
            performFirmwareUpdate2_0();

            if (force_update_.has_value()) {
                if (!force_update_.value()) {
                    // Note - doesn't seem to be defined by 2.0.1 requirements, following TC_F_17_CS
                    auto const result = remote.sendCall(ocpp2_0::FirmwareStatusNotificationRequest {
                        ocpp2_0::FirmwareStatusEnumType::kIdle
                    });

                    if (result.has_value())
                        force_update_ = std::nullopt;
                } else {
                    if (operation_.has_value() && operation_->last_status2_0.has_value()) {
                        auto const result = remote.sendCall(ocpp2_0::FirmwareStatusNotificationRequest {
                                operation_->last_status2_0.value(),
                                operation_->request.requestId
                        });

                        if (result.has_value())
                            force_update_ = std::nullopt;
                    } else {
                        // F06.FR.16
                        auto const result = remote.sendCall(ocpp2_0::FirmwareStatusNotificationRequest {
                                ocpp2_0::FirmwareStatusEnumType::kIdle
                        });

                        if (result.has_value())
                            force_update_ = std::nullopt;
                    }
                }
            }
        }

        void performBootChecks2_0() {
            if (performed_boot_checks_)
                return;

            auto const active = station_->getActiveSlotId();
            settings_->ActiveFirmwareSlotId.setValue(active);

            auto const update = settings_->ExpectedUpdateFirmwareSlotId.getValue();
            if (!update.empty()) {
                int request_id;
                std::string slot_id;

                parseExpectedUpdateFirmwareSlotId(update, request_id, slot_id);

                pending_messages_->sendRequest2_0(
                        ocpp2_0::FirmwareStatusNotificationRequest {
                                slot_id == active ?
                                ocpp2_0::FirmwareStatusEnumType::kInstalled :
                                ocpp2_0::FirmwareStatusEnumType::kInstallationFailed,
                                request_id
                        },
                        buildPendingMessagePolicy(PendingMessageType::kNotificationEvent)
                );

                pending_messages_->sendRequest2_0(
                        ocpp2_0::SecurityEventNotificationRequest{
                                "FirmwareUpdated",
                                platform_->systemClockNow()
                        },
                        buildPendingMessagePolicy(PendingMessageType::kSecurityEvent)
                );

                settings_->ExpectedUpdateFirmwareSlotId.setValue("");
            }

            performed_boot_checks_ = true;
        }

        /**
         * Streams the firmware image exactly once: each chunk is flashed to the inactive
         * partition and folded into the signature hash as it arrives. The image is never
         * committed - esp_ota_end() and set_boot_partition() only run from the install
         * step below - so nothing can boot an image whose signature hasn't been checked.
         *
         * This replaces an earlier two-pass flow that downloaded the whole image once to
         * verify it and then downloaded it a second time to flash it, which doubled both
         * the transfer cost and the window in which a connection drop could kill the
         * update, and left the bytes that were actually flashed unverified.
         */
        void performFirmwareUpdate2_0() {
            if (!operation_.has_value())
                return;

            auto const max_retries = optional::GetOrDefault(
                    operation_->request.retries,
                    settings_->FirmwareUpdateDefaultRetries.getValue()
            );

            if (operation_->request.firmware.retrieveDateTime.isAfter(platform_->systemClockNow(), false)) {
                checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kDownloadScheduled);
                return;
            }

            if (!operation_->finished_signature_check) {
                if (operationExhausted(operation_.value(), max_retries)) {
                    checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kDownloadFailed);
                    abandonUpdateProcess();
                    operation_ = std::nullopt;
                    return;
                }

                checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kDownloading);
                if (waitingForRetry(operation_.value()))
                    return;

                if (!checkOrRetryConnection(platform_, operation_.value()))
                    return;

                auto const remaining = (int)operation_->content_length - (int)operation_->total_bytes_read;
                if (remaining > 0) {
                    operation_->buffer.resize(kChunkSize);
                    auto const bytes_read = operation_->connection->read((char*)operation_->buffer.data(), (int)operation_->buffer.size());
                    // Note: treating zero bytes read as a failure condition here to prevent an infinite loop under
                    // those conditions.
                    if (bytes_read <= 0) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Failed reading data - retrying operation";
                        recordFailure(platform_, operation_.value());
                        return;
                    }

                    if (operation_->total_bytes_read == 0) {
                        // Restarting the transfer from the beginning - discard anything a
                        // previous attempt wrote and start the hash over with it.
                        abandonUpdateProcess();
                        operation_->signature_hash->reset();
                        operation_->block_hashes.clear();

                        if (station_->startUpdateProcess(operation_->content_length) != StationInterface::Result::kSucceeded) {
                            CHARGELAB_LOG_MESSAGE(warning) << "Failed starting firmware update process";
                            recordFailure(platform_, operation_.value());
                            return;
                        }

                        operation_->running_firmware_update = true;
                    } else if (!operation_->running_firmware_update) {
                        CHARGELAB_LOG_MESSAGE(error)
                            << "Unexpected state - expected running firmware update operation";
                        recordFailure(platform_, operation_.value());
                        return;
                    }

                    auto const chunk_result = station_->processFirmwareChunk(operation_->buffer.data(), bytes_read);
                    if (chunk_result == StationInterface::Result::kVerificationFailed) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Firmware image failed verification - abandoning update";
                        operation_->running_firmware_update = false;
                        checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kInstallVerificationFailed);
                        operation_ = std::nullopt;
                        return;
                    } else if (chunk_result != StationInterface::Result::kSucceeded) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Failed processing firmware chunk";
                        recordFailure(platform_, operation_.value());
                        return;
                    }

                    // Block hash
                    auto hash = HM::calculateHashBinary(
                            HM::kHashTypeSHA256,
                            (unsigned char const*)operation_->buffer.data(),
                            bytes_read
                    );
                    if (!hash.has_value()) {
                        CHARGELAB_LOG_MESSAGE(error) << "Failed hashing block";
                        recordFailure(platform_, operation_.value());
                        return;
                    }

                    operation_->block_hashes.push_back(std::move(hash.value()));
                    operation_->signature_hash->update((unsigned char const*)operation_->buffer.data(), bytes_read);
                    operation_->total_bytes_read += bytes_read;
                    recordProgress(operation_.value());
                    CHARGELAB_LOG_MESSAGE(debug) << "Download/flash progress: " << operation_->total_bytes_read << " / " << operation_->content_length;
                }

                if (operation_->total_bytes_read >= operation_->content_length) {
                    checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kDownloaded);

                    auto signature_hash = operation_->signature_hash->finishBinary();
                    if (!signature_hash.has_value()) {
                        reportInvalidSignature2_0();
                        return;
                    }

                    auto const& firmware = operation_->request.firmware;
                    if (firmware.signingCertificate.has_value() && firmware.signature.has_value()) {
                        auto signature_binary = HM::decodeBase64(firmware.signature->value());
                        if (!signature_binary.has_value()) {
                            reportInvalidSignature2_0();
                            return;
                        }

                        SignatureAndHash signature_and_hash {};
                        signature_and_hash.sig = signature_binary->data();
                        signature_and_hash.sig_len = signature_binary->size();
                        signature_and_hash.hash = signature_hash->data();
                        signature_and_hash.hash_len = signature_hash->size();

                        auto const valid = platform_->verifyManufacturerCertificate(
                                firmware.signingCertificate->value(),
                                signature_and_hash
                        );
                        if (!valid) {
                            reportInvalidSignature2_0();
                            return;
                        }

                        checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kSignatureVerified);
                    }

                    std::size_t total_block_hash_size = 0;
                    for (auto const& x : operation_->block_hashes)
                        total_block_hash_size += x.size();
                    CHARGELAB_LOG_MESSAGE(debug) << "Total block hash size (" << operation_->block_hashes.size() << " blocks): " << total_block_hash_size;

                    operation_->expected_signature_hash = std::move(signature_hash);
                    operation_->finished_signature_check = true;
                    operation_->connection = nullptr;
                }

                return;
            }

            if (!operation_->finished_flashing_firmware) {
                // L01.FR.16 - the image is already written to the inactive partition at this
                // point, but nothing is committed: the OTA handle stays open and the boot
                // partition is untouched until the scheduled install time arrives.
                if (operation_->request.firmware.installDateTime.has_value()) {
                    if (operation_->request.firmware.installDateTime->isAfter(platform_->systemClockNow(), false)) {
                        checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kInstallScheduled);
                        return;
                    }
                }

                checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kInstalling);

                auto const slot_id = station_->getUpdateSlotId();
                auto const finish_result = station_->finishUpdateProcess(true);
                operation_->running_firmware_update = false;
                if (finish_result != StationInterface::Result::kSucceeded) {
                    // The image is fully downloaded and signature checked by this point, so
                    // a failure here is the flash write or ESP's own image validation - not
                    // something another transfer attempt can fix.
                    CHARGELAB_LOG_MESSAGE(warning) << "Failed finishing update process - abandoning update";
                    checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kInstallationFailed);
                    operation_ = std::nullopt;
                    return;
                }

                operation_->finished_flashing_firmware = true;
                operation_->connection = nullptr;
                settings_->ExpectedUpdateFirmwareSlotId.setValueFromString(buildExpectedUpdateFirmwareSlotId(
                        operation_->request.requestId, slot_id));
                checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kInstallRebooting);

                reset_->resetOnIdle(ocpp2_0::BootReasonEnumType::kFirmwareUpdate);
                return;
            }
        }

        /**
         * Discards anything a previous attempt wrote to the inactive partition. Safe to
         * call when no update process is running.
         */
        void abandonUpdateProcess() {
            if (!operation_.has_value() || !operation_->running_firmware_update)
                return;

            station_->finishUpdateProcess(false);
            operation_->running_firmware_update = false;
        }

        // L01.FR.03
        void reportInvalidSignature2_0() {
            checkAndUpdateStatus(ocpp2_0::FirmwareStatusEnumType::kInvalidSignature);
            abandonUpdateProcess();

            pending_messages_->sendRequest2_0(
                    ocpp2_0::SecurityEventNotificationRequest {
                            "InvalidFirmwareSignature",
                            platform_->systemClockNow()
                    },
                    buildPendingMessagePolicy(PendingMessageType::kSecurityEvent)
            );

            operation_ = std::nullopt;
        }

        void performBootChecks1_6() {
            if (performed_boot_checks_)
                return;

            auto const active = station_->getActiveSlotId();
            settings_->ActiveFirmwareSlotId.setValue(active);

            auto const update = settings_->ExpectedUpdateFirmwareSlotId.getValue();
            if (!update.empty()) {
                int request_id;
                std::string slot_id;

                parseExpectedUpdateFirmwareSlotId(update, request_id, slot_id);

                pending_messages_->sendRequest1_6(
                        ocpp1_6::FirmwareStatusNotificationReq {
                                slot_id == active ?
                                ocpp1_6::FirmwareStatus::kInstalled :
                                ocpp1_6::FirmwareStatus::kInstallationFailed
                        },
                        buildPendingMessagePolicy(PendingMessageType::kNotificationEvent)
                );

                settings_->ExpectedUpdateFirmwareSlotId.setValue("");
            }

            performed_boot_checks_ = true;
        }

        void performFirmwareUpdate1_6() {
            if (!operation_.has_value())
                return;

            auto const max_retries = optional::GetOrDefault(
                    operation_->request.retries,
                    settings_->FirmwareUpdateDefaultRetries.getValue()
            );

            if (operationExhausted(operation_.value(), max_retries)) {
                if (operation_->content_length == 0 || operation_->content_length > operation_->total_bytes_read) {
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kDownloadFailed);
                } else {
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kInstallationFailed);
                }
                operation_ = std::nullopt;
                restoreConnector0OperativeIfNeeded();
                return;
            }

            if (!operation_->last_status1_6.has_value()) {
                checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kDownloading);
            } else {
                if (operation_->last_status1_6 == ocpp1_6::FirmwareStatus::kDownloaded) {
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kInstalling);
                }
            }

            if (waitingForRetry(operation_.value()))
                return;

            if (!checkOrRetryConnection(platform_, operation_.value()))
                return;

            auto const remaining = (int)operation_->content_length - (int)operation_->total_bytes_read;
            if (remaining > 0) {
                operation_->buffer.resize(kChunkSize);
                auto const bytes_read = operation_->connection->read((char*)operation_->buffer.data(), (int)operation_->buffer.size());
                // Note: treating zero bytes read as a failure condition here to prevent an infinite loop under
                // those conditions.
                if (bytes_read <= 0) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Failed reading data - retrying operation";
                    recordFailure(platform_, operation_.value());
                    return;
                }

                if (operation_->total_bytes_read == 0) {
                    if (operation_->running_firmware_update) {
                        station_->finishUpdateProcess(false);
                        operation_->running_firmware_update = false;
                    }

                    if (station_->startUpdateProcess(operation_->content_length) != StationInterface::Result::kSucceeded) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Failed starting firmware update process";
                        recordFailure(platform_, operation_.value());
                        return;
                    }

                    operation_->running_firmware_update = true;
                } else {
                    if (!operation_->running_firmware_update) {
                        CHARGELAB_LOG_MESSAGE(error)
                            << "Unexpected state - expected running firmware update operation";
                        recordFailure(platform_, operation_.value());
                        return;
                    }
                }

                auto const chunk_result = station_->processFirmwareChunk(operation_->buffer.data(), bytes_read);
                if (chunk_result == StationInterface::Result::kVerificationFailed) {
                    // OCPP 1.6 has no InstallVerificationFailed status - report InstallationFailed
                    CHARGELAB_LOG_MESSAGE(warning) << "Firmware image failed verification - abandoning update";
                    operation_->running_firmware_update = false;
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kInstallationFailed);
                    operation_ = std::nullopt;
                    return;
                } else if (chunk_result != StationInterface::Result::kSucceeded) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Failed processing firmware chunk";
                    recordFailure(platform_, operation_.value());
                    return;
                }

                // TODO: For ocpp1.6, the existing approach is to pre-set the hash value to the setting parameter and
                //  read the firmware hash value from the firmware itself. Then compare them to decide if the downloaded firmware is the right one.
                //  What approach is expected here?
                operation_->signature_hash->update((unsigned char const*)operation_->buffer.data(), bytes_read);
                operation_->total_bytes_read += bytes_read;
                recordProgress(operation_.value());
                CHARGELAB_LOG_MESSAGE(debug) << "Flashing progress: " << operation_->total_bytes_read << " / " << operation_->content_length;
            }

            if (operation_->total_bytes_read >= operation_->content_length) {
                // TODO: Check signature

                if (operation_->last_status1_6 == ocpp1_6::FirmwareStatus::kDownloading) {
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kDownloaded);
                    return;
                } else if (operation_->last_status1_6 == ocpp1_6::FirmwareStatus::kDownloaded) {
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kInstalling);
                    return;
                }

                auto const slot_id = station_->getUpdateSlotId();
                auto const finish_result = station_->finishUpdateProcess(true);
                if (finish_result == StationInterface::Result::kVerificationFailed) {
                    // OCPP 1.6 has no InstallVerificationFailed status - report InstallationFailed
                    CHARGELAB_LOG_MESSAGE(warning) << "Firmware image failed verification - abandoning update";
                    operation_->running_firmware_update = false;
                    checkAndUpdateStatus(ocpp1_6::FirmwareStatus::kInstallationFailed);
                    operation_ = std::nullopt;
                    restoreConnector0OperativeIfNeeded();
                    return;
                } else if (finish_result != StationInterface::Result::kSucceeded) {
                    CHARGELAB_LOG_MESSAGE(warning) << "Failed finishing update process";
                    operation_->running_firmware_update = false;
                    recordFailure(platform_, operation_.value());
                    return;
                }

                //settings_->ExpectedUpdateFirmwareSlotId.setValueFromString(std::to_string(operation_->request.requestId) + ":" + slot_id);
                settings_->ExpectedUpdateFirmwareSlotId.setValueFromString(buildExpectedUpdateFirmwareSlotId(
                        operation_->request.requestId, slot_id));

                operation_ = std::nullopt;
                restoreConnector0OperativeIfNeeded();

                reset_->resetOnIdle(ocpp2_0::BootReasonEnumType::kFirmwareUpdate);
            }
        }

        void restoreConnector0OperativeIfNeeded() {
            if (need_to_restore_connector_0_operative_) {
                connector_status_->setConnector0Inoperative(false);
                need_to_restore_connector_0_operative_ = false;
            }
        }

        static std::string buildExpectedUpdateFirmwareSlotId(int request_id, std::string const& slot_id) {
            return std::to_string(request_id) + ":" + slot_id;
        }

        static void parseExpectedUpdateFirmwareSlotId(std::string const& expectedUpdateFirmwareSlotId,
                                                      int& request_id, std::string& slot_id) {

            auto index = expectedUpdateFirmwareSlotId.find(':');
            if (index != std::string::npos) {
                request_id = optional::GetOrDefault(string::ToInteger(expectedUpdateFirmwareSlotId.substr(0, index)), 0);
                slot_id = expectedUpdateFirmwareSlotId.substr(index+1);
            } else {
                request_id = 0;
                slot_id = expectedUpdateFirmwareSlotId;
            }
        }

        PendingMessagePolicy buildPendingMessagePolicy(PendingMessageType message_type) {
            return PendingMessagePolicy {
                    message_type,
                    kOperationGroupId,
                    settings_->NotificationMessageDefaultRetries.getValue(),
                    settings_->NotificationMessageDefaultRetryInterval.getValue(),
                    kPriorityFirmwareStatusNotification,
                    false,
                    false
            };
        }

    private:
        std::shared_ptr<PlatformInterface> platform_;
        std::shared_ptr<ResetModule> reset_;
        std::shared_ptr<PendingMessagesModule> pending_messages_;
        std::shared_ptr<ConnectorStatusModule> connector_status_; // TODO: Remove dependency on ConnectorStatusModule
        std::shared_ptr<StationInterface> station_;
        std::shared_ptr<Settings> settings_;

        /**
         * Set to true to force an OCPP 2.0.1 or 1.6 firmware status update and false to force an "idle" firmware status
         * update (only applies when an associated trigger message is received with an EVSE filter).
         */
        std::optional<bool> force_update_ = std::nullopt;

        std::optional<detail::FirmwareUpdateOperation<HM>> operation_ = std::nullopt;
        bool performed_boot_checks_ = false;

        bool need_to_restore_connector_0_operative_ { false };
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_FIRMWARE_UPDATE_MODULE_H

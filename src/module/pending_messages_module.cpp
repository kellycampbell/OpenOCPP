#include "openocpp/module/pending_messages_module.h"

#include <random>
#include <limits>
#include <algorithm>
#include <map>

namespace chargelab {

PendingMessagesModule::PendingMessagesModule(
        std::shared_ptr<Settings> settings,
        std::shared_ptr<SystemInterface> const& system,
        std::shared_ptr<StorageInterface> storage
)
    : settings_ {std::move(settings)},
      system_ {system},
      storage_ {std::move(storage)},
      random_engine_ {std::random_device{}()},
      // TODO: Review this; timeout will not change until reboot
      request_operation_ {system}
{
    std::uniform_int_distribution<int64_t> distribution(0, std::numeric_limits<int64_t>::max());
    request_id_ = distribution(random_engine_);
    loadFromStorage();
    // Clear transaction_ids_ and sequence_ids_ if offline_queue_ and live_queue_ are both empty
    if (offline_queue_.empty() && live_queue_.empty()) {
        transaction_ids_.clear();
        sequence_ids_.clear();
    }
    settings_->MessageFlushCounter.setValue(pending_messages_write_count_);
}

void PendingMessagesModule::registerOnSaveMessageSupplier(std::shared_ptr<saved_message_supplier> const& supplier) {
    for (auto const& x : saved_message_suppliers_) {
        if (x == supplier) {
            CHARGELAB_LOG_MESSAGE(warning) << "Callback registered multiple times";
            return;
        }
    }
    saved_message_suppliers_.push_back(supplier);
}

void PendingMessagesModule::unregisterOnSaveMessageSupplier(std::shared_ptr<saved_message_supplier> const& supplier) {
    auto initial_size = saved_message_suppliers_.size();
    saved_message_suppliers_.erase(
            std::remove_if(saved_message_suppliers_.begin(), saved_message_suppliers_.end(), [&](auto& x) {return x == supplier;}),
            saved_message_suppliers_.end()
    );

    if (initial_size - saved_message_suppliers_.size() > 1)
        CHARGELAB_LOG_MESSAGE(warning) << "Callback registered multiple times";
}

void PendingMessagesModule::runUnconditionally() {
    updateCacheAndStats();
    limitOfflineQueueSize();
    flushToDisk();

    // If an operation is not in progress, flush messages from the live queue to the offline queue as necessary
    if (!request_operation_.operationInProgress() && !live_queue_.empty()) {
        while (live_queue_.size() > kMaxLiveMessages) {
            auto it = live_queue_.begin();
            if (it->policy.group_id.has_value())
                active_group_ids_.insert(it->policy.group_id.value());

            offline_queue_.pushBack(live_queue_.front());
            live_queue_.erase(live_queue_.begin());
            pending_messages_changed_ = true;
        }

        // Make sure live messages with the same group IDs also get moved to offline messages so that they're
        // not sent out of order.
        live_queue_.erase(
                std::remove_if(
                        live_queue_.begin(),
                        live_queue_.end(),
                        [&](detail::PendingMessageWrapper const& wrapper) {
                            if (activeGroupsContains(wrapper.policy.group_id)) {
                                offline_queue_.pushBack(wrapper);
                                pending_messages_changed_ = true;
                                return true;
                            } else {
                                return false;
                            }
                        }
                ),
                live_queue_.end()
        );
    }
}

void PendingMessagesModule::runStep(ocpp1_6::OcppRemote& remote) {
    // Send out a live message, if one is available
    if (!request_operation_.operationInProgress() && !live_queue_.empty()) {
        auto it = live_queue_.begin();
        bool delete_message = false;
        bool send_message = true;

        if (blacklistContains(it->policy.group_id))
            delete_message = true;
        if (!it->action_id1_6.has_value())
            delete_message = true;
        if (it->attempts > 0 && it->attempts >= it->policy.message_attempts)
            delete_message = true;

        if (it->attempts > 0) {
            auto const threshold = std::max(it->attempts, 1) * it->policy.retry_interval_seconds;
            if (request_operation_.getIdleDurationSeconds() < threshold)
                send_message = false;
        }

        if (delete_message) {
            live_queue_.erase(it);
        } else if (send_message) {
            if (sendWithTransactionId(remote, *it)) {
                it->attempts++;
                request_operation_.setWithTimeout(
                        settings_->DefaultMessageTimeout.getValue(),
                        std::to_string(it->unique_id)
                );
                request_object_ = *it;
            }
        }
    }

    // Next send out an offline message, if one is available
    if (!request_operation_.operationInProgress() && !offline_queue_.empty()) {
        auto record = offline_queue_.pollFront();
        if (record.has_value()) {
            bool delete_message = false;
            bool send_message = true;

            if (blacklistContains(record->policy.group_id))
                delete_message = true;
            if (!record->action_id1_6.has_value())
                delete_message = true;
            if (record->attempts > 0 && record->attempts >= record->policy.message_attempts)
                delete_message = true;

            if (record->attempts > 0) {
                auto const threshold = std::max(record->attempts, 1) * record->policy.retry_interval_seconds;
                if (request_operation_.getIdleDurationSeconds() < threshold)
                    send_message = false;
            }

            if (delete_message) {
                CHARGELAB_LOG_MESSAGE(info) << "popFront: delete_message";
                offline_queue_.popFront();
                pending_messages_changed_ = true;
            } else if (send_message) {
                if (sendWithTransactionId(remote, record.value())) {
                    record->attempts++;
                    offline_queue_.updateFront(record.value());
                    pending_messages_changed_ = true;
                    request_operation_.setWithTimeout(
                            settings_->DefaultMessageTimeout.getValue(),
                            std::to_string(record->unique_id)
                    );
                    request_object_ = record.value();
                }
            }
        }
    }
}

void PendingMessagesModule::runStep(ocpp2_0::OcppRemote &remote) {
    updateCacheAndStats();
    limitOfflineQueueSize();

    // If an operation is not in progress, flush messages from the live queue to the offline queue as necessary
    if (!request_operation_.operationInProgress() && !live_queue_.empty()) {
        while (live_queue_.size() > kMaxLiveMessages) {
            auto it = live_queue_.begin();
            if (it->policy.group_id.has_value())
                active_group_ids_.insert(it->policy.group_id.value());

            offline_queue_.pushBack(live_queue_.front());
            live_queue_.erase(live_queue_.begin());
            pending_messages_changed_ = true;
        }

        // Make sure live messages with the same group IDs also get moved to offline messages so that they're
        // not sent out of order.
        live_queue_.erase(
                std::remove_if(
                        live_queue_.begin(),
                        live_queue_.end(),
                        [&](detail::PendingMessageWrapper const& wrapper) {
                            if (activeGroupsContains(wrapper.policy.group_id)) {
                                offline_queue_.pushBack(wrapper);
                                pending_messages_changed_ = true;
                                return true;
                            } else {
                                return false;
                            }
                        }
                ),
                live_queue_.end()
        );
    }

    // First send out a live message, if one is available
    if (!request_operation_.operationInProgress() && !live_queue_.empty()) {
        auto it = live_queue_.begin();
        bool delete_message = false;
        bool send_message = true;

        if (blacklistContains(it->policy.group_id))
            delete_message = true;
        if (!it->action_id2_0.has_value())
            delete_message = true;
        if (it->attempts > 0 && it->attempts >= it->policy.message_attempts)
            delete_message = true;

        if (it->attempts > 0) {
            auto const threshold = std::max(it->attempts, 1) * it->policy.retry_interval_seconds;
            if (request_operation_.getIdleDurationSeconds() < threshold)
                send_message = false;
        }

        if (delete_message) {
            if (it->policy.group_id.has_value()) {
                auto sequence = sequence_ids_.find(it->policy.group_id.value());
                if (sequence != sequence_ids_.end()) {
                    if (shouldRemoveSequenceId(*it)) {
                        sequence_ids_.erase(sequence);
                        pending_messages_changed_ = true;
                    } else {
                        sequence->second++;
                    }
                }
            }

            live_queue_.erase(it);
        } else if (send_message) {
            if (sendWithSequenceNumber(remote, *it)) {
                it->attempts++;
                request_operation_.setWithTimeout(
                        settings_->DefaultMessageTimeout.getValue(),
                        std::to_string(it->unique_id)
                );
                request_object_ = *it;
            }
        }
    }

    // Next send out an offline message, if one is available
    if (!request_operation_.operationInProgress() && !offline_queue_.empty()) {
        auto record = offline_queue_.pollFront();
        if (record.has_value()) {
            bool delete_message = false;
            bool send_message = true;

            if (blacklistContains(record->policy.group_id))
                delete_message = true;
            if (!record->action_id2_0.has_value())
                delete_message = true;
            if (record->attempts > 0 && record->attempts >= record->policy.message_attempts)
                delete_message = true;

            if (record->attempts > 0) {
                auto const threshold = std::max(record->attempts, 1) * record->policy.retry_interval_seconds;
                if (request_operation_.getIdleDurationSeconds() < threshold)
                    send_message = false;
            }

            if (delete_message) {
                CHARGELAB_LOG_MESSAGE(info) << "popFront: delete_message";
                if (record->policy.group_id.has_value()) {
                    auto sequence = sequence_ids_.find(record->policy.group_id.value());
                    if (sequence != sequence_ids_.end()) {
                        if (shouldRemoveSequenceId(record.value())) {
                            sequence_ids_.erase(sequence);
                        } else {
                            sequence->second++;
                        }
                    }
                }

                offline_queue_.popFront();
                pending_messages_changed_ = true;
            } else if (send_message) {
                if (sendWithSequenceNumber(remote, record.value())) {
                    record->attempts++;
                    offline_queue_.updateFront(record.value());
                    pending_messages_changed_ = true;
                    request_operation_.setWithTimeout(
                            settings_->DefaultMessageTimeout.getValue(),
                            std::to_string(record->unique_id)
                    );
                    request_object_ = record.value();
                }
            }
        }
    }
}

void PendingMessagesModule::onStartTransactionRsp(
        const std::string &unique_id,
        const ocpp1_6::ResponseMessage<ocpp1_6::StartTransactionRsp> &rsp
) {
    if (request_operation_ == unique_id && std::holds_alternative<ocpp1_6::StartTransactionRsp>(rsp)) {
        auto const& message = std::get<ocpp1_6::StartTransactionRsp>(rsp);
        if (!request_object_.has_value())
            return;
        if (!request_object_->policy.group_id.has_value())
            return;

        transaction_ids_.insert(std::make_pair(request_object_->policy.group_id.value(), message.transactionId));

        // Flush the pending message to remove StartTransaction from the live_queue_ and
        // update the StopTransaction with transaction Id is the StartTransaction is accepted
        must_flush_to_disk_ = true;
    }
}

bool PendingMessagesModule::activeGroupsContains(std::optional<int64_t> const& group_id) const {
    if (!group_id.has_value())
        return false;
    return active_group_ids_.find(group_id.value()) != active_group_ids_.end();
}

bool PendingMessagesModule::blacklistContains(std::optional<int64_t> const& group_id) const {
    if (!group_id.has_value())
        return false;
    return group_blacklist_.find(group_id.value()) != group_blacklist_.end();
}

bool PendingMessagesModule::sendWithTransactionId(ocpp1_6::OcppRemote& remote, detail::PendingMessageWrapper const& wrapper) {
    if (!wrapper.action_id1_6.has_value())
        return false;

    std::string payload = wrapper.payload;
    if (wrapper.policy.group_id.has_value()) {
        auto it = transaction_ids_.find(wrapper.policy.group_id.value());
        if (it != transaction_ids_.end())
            payload = insert_into_object(payload, "transactionId", it->second);
    }

    return remote.sendCall(
            std::to_string(wrapper.unique_id),
            wrapper.action_id1_6.value(),
            payload
    );
}

bool PendingMessagesModule::sendWithSequenceNumber(ocpp2_0::OcppRemote& remote, detail::PendingMessageWrapper const& wrapper) {
    if (!wrapper.action_id2_0.has_value())
        return false;

    std::string payload = wrapper.payload;
    if (wrapper.policy.add_message_sequence_number && wrapper.policy.group_id.has_value()) {
        auto it = sequence_ids_.find(wrapper.policy.group_id.value());
        if (it == sequence_ids_.end()) {
            auto first = read_field_from_object<int>(payload, "seqNo");
            sequence_ids_[wrapper.policy.group_id.value()] = first.value_or(0);
            pending_messages_changed_ = true;
        }

        payload = insert_into_object(payload, "seqNo", sequence_ids_[wrapper.policy.group_id.value()]);
    }

    return remote.sendCall(
            std::to_string(wrapper.unique_id),
            wrapper.action_id2_0.value(),
            payload
    );
}

void PendingMessagesModule::limitOfflineQueueSize() {
    auto const initial_total_bytes = offline_queue_.totalBytes();
    if (initial_total_bytes < kOfflineSizeLimitBytes)
        return;

    CHARGELAB_LOG_MESSAGE(info) << "Dropping messages to limit offline queue size: "
                                << initial_total_bytes << " > " << kOfflineSizeLimitBytes;

    auto const start_ts = system_->steadyClockNow();
    std::map<std::pair<int, std::optional<int>>, std::pair<int, int>> stats;
    offline_queue_.visit([&] (std::string_view const& text, detail::PendingMessageWrapper const& wrapper) {
        auto& value = stats[std::make_pair(wrapper.policy.priority, wrapper.policy.group_id)];
        value.first += 1;
        value.second += (int)text.size();
    });
    auto const middle_ts1 = system_->steadyClockNow();

    int max_priority = std::numeric_limits<int>::min();
    for (auto const& x : stats)
        max_priority = std::max(x.first.first, max_priority);

    bool delete_safely = false;
    for (auto const& x : stats) {
        if (!x.first.second.has_value()) {
            delete_safely = true;
            break;
        }
        if (x.first.first == max_priority && x.second.first > 2) {
            delete_safely = true;
            break;
        }
    }

    int delete_count = 0;
    for (auto const& x : stats) {
        if (!x.first.second.has_value()) {
            delete_count++;
        } else if (delete_safely) {
            delete_count += std::max(x.second.first - 2, 0);
        } else {
            delete_count += x.second.first;
        }
    }

    std::size_t deleted_records = 0;
    std::size_t deleted_decompressed_bytes = 0;

    std::uniform_int_distribution<int64_t> distribution(0, delete_count-1);
    std::map<int64_t, int> counter;
    auto const middle_ts2 = system_->steadyClockNow();
    offline_queue_.removeIf([&] (std::string_view const& text, detail::PendingMessageWrapper const& wrapper) {
        if (wrapper.policy.priority != max_priority)
            return false;

        if (wrapper.policy.group_id.has_value() && delete_safely) {
            auto index = counter[wrapper.policy.group_id.value()]++;
            auto const& stat = stats[std::make_pair(wrapper.policy.priority, wrapper.policy.group_id)];
            if (index == 0 || index == stat.first-1)
                return false;
        }

        if (distribution(random_engine_) < kTargetDeleteRecordCount) {
            deleted_records++;
            deleted_decompressed_bytes += text.size();
            CHARGELAB_LOG_MESSAGE(info) << "Dropping offline message: " << wrapper.payload;
            pending_messages_changed_ = true;
            return true;
        } else {
            return false;
        }
    });
    auto const end_ts = system_->steadyClockNow();

    CHARGELAB_LOG_MESSAGE(info) << "Delete stats:"
                                << " processing_ms=" << (end_ts - start_ts)
                                << " deleted_records=" << deleted_records
                                << " deleted_decompressed_bytes=" << deleted_decompressed_bytes
                                << " new_compressed_size=" << offline_queue_.totalBytes();

    CHARGELAB_LOG_MESSAGE(info) << "Times: "
                                << " block1=" << (middle_ts1 - start_ts)
                                << " block2=" << (middle_ts2 - middle_ts1)
                                << " block3=" << (end_ts - middle_ts2);
}

void PendingMessagesModule::onCallRsp(const std::string &unique_id, const ocpp1_6::ResponseMessage<common::RawJson>& payload) {
    if (request_operation_ == unique_id) {
        request_operation_ = kNoOperation;

        if (std::holds_alternative<common::RawJson>(payload)) {
            if (!request_object_.has_value())
                return;

            auto onCallRsp = [this](detail::PendingMessageWrapper const &wrapper) {
                if (wrapper.action_id1_6 && wrapper.action_id1_6.value() == ocpp1_6::ActionId::kStopTransaction) {
                    auto const request = read_json_from_string<ocpp1_6::StopTransactionReq>(wrapper.payload);
                    if (!request.has_value()) {
                        CHARGELAB_LOG_MESSAGE(warning) << "Failed read StopTransaction request from: " << wrapper.payload;
                        return;
                    }

                    // remove transaction id from transaction_ids_ if there is one
                    for (auto it = transaction_ids_.begin(); it != transaction_ids_.end(); ) {
                        if (it->second == request->transactionId) {
                            it = transaction_ids_.erase(it);
                            pending_messages_changed_ = true;
                        } else {
                            ++it;
                        }
                    }

                    if (request->reason == ocpp1_6::Reason::kHardReset || request->reason == ocpp1_6::Reason::kSoftReset) {
                        must_flush_to_disk_ = true;
                        flushToDisk();
                    }
                }
            };

            if (!live_queue_.empty() && live_queue_.front().unique_id == request_object_->unique_id) {
                auto wrapper = live_queue_.front();
                live_queue_.erase(live_queue_.begin());
                onCallRsp(wrapper);
                return;
            }

            if (offline_queue_.pollFront().has_value() && offline_queue_.pollFront()->unique_id == request_object_->unique_id) {
                CHARGELAB_LOG_MESSAGE(info) << "popFront: onCallRsp";
                auto record = offline_queue_.pollFront();
                offline_queue_.popFront();
                onCallRsp(record.value());
                pending_messages_changed_ = true;
                return;
            }
        }
    }
}

void PendingMessagesModule::onCallRsp(const std::string &unique_id, const ocpp2_0::ResponseMessage<common::RawJson>& payload) {
    if (request_operation_ == unique_id) {
        request_operation_ = kNoOperation;

        if (std::holds_alternative<common::RawJson>(payload)) {
            if (!request_object_.has_value())
                return;

            if (!live_queue_.empty() && live_queue_.front().unique_id == request_object_->unique_id) {
                auto it = sequence_ids_.find(live_queue_.front().policy.group_id.value());
                if (it != sequence_ids_.end()) {
                    if (shouldRemoveSequenceId(live_queue_.front())) {
                        sequence_ids_.erase(it);
                        pending_messages_changed_ = true;
                    } else {
                        it->second++;
                    }
                }

                live_queue_.erase(live_queue_.begin());
                return;
            }

            auto const& record = offline_queue_.pollFront();
            if (record.has_value() && record->unique_id == request_object_->unique_id) {
                CHARGELAB_LOG_MESSAGE(info) << "popFront: onCallRsp";
                auto it = sequence_ids_.find(record->policy.group_id.value());
                if (it != sequence_ids_.end()) {
                    if (shouldRemoveSequenceId(record.value())) {
                        sequence_ids_.erase(it);
                    } else {
                        it->second++;
                    }
                }

                offline_queue_.popFront();
                pending_messages_changed_ = true;
                return;
            }
        }
    }
}

void PendingMessagesModule::updateCacheAndStats() {
    auto const now = system_->steadyClockNow();
    if (last_cache_update_.has_value()) {
        auto const delta = now - last_cache_update_.value();
        if (delta/1000 < kUpdateCacheAndStatsFrequencySeconds)
            return;
    }

    last_cache_update_ = now;

    std::size_t total_records = 0;
    std::size_t total_decompressed_size = 0;

    active_group_ids_.clear();
    offline_queue_.visit([&](std::string_view const& text, detail::PendingMessageWrapper const& wrapper) {
        if (wrapper.policy.group_id.has_value())
            active_group_ids_.insert(wrapper.policy.group_id.value());

        total_records++;
        total_decompressed_size += text.size();
    });

    auto const total_compressed_size = offline_queue_.totalBytes();
    double compression_ratio = 0;
    if (total_decompressed_size > 0)
        compression_ratio = (double)total_compressed_size/(double)total_decompressed_size;

    CHARGELAB_LOG_MESSAGE(info) << "Offline message stats:"
                                << " records=" << total_records
                                << " compressed_bytes=" << total_compressed_size
                                << " decompressed_bytes=" << total_decompressed_size
                                << " compression_ratio=" << compression_ratio;
}

void PendingMessagesModule::flushToDisk() {
    if (!must_flush_to_disk_ && !pending_messages_changed_) {
        return; // No need to flush anything
    }

    auto const now = system_->steadyClockNow();

    auto total_writing_bytes = calculateAllPendingSavedInfoSize();

    total_writing_bytes = ((total_writing_bytes + 255)/256)*256;
    int flush_interval = ((double)total_writing_bytes / kMaxStorageBlockSize) * kFlushToDiskFrequencyMillis;

    CHARGELAB_LOG_MESSAGE(trace) << "flush interval: " << flush_interval << ", offline_queue_ bytes: " << offline_queue_.totalBytes()
         << ", must_flush_to_disk_: " << must_flush_to_disk_ << ", pending_messages_changed_: " << pending_messages_changed_;

    if (!must_flush_to_disk_ && last_flush_to_disk_.has_value()) {
        // Note: flushing to disk explicitly after the historic backlog has cleared after an offline period or
        // after restarting.
        auto const delta = now - last_flush_to_disk_.value();
        if (delta < flush_interval)
            return;
    }
    last_flush_to_disk_ = now;
    must_flush_to_disk_ = false;

    // Note: must flush to disk here even if online, otherwise the Ended messages aren't added to the stream and
    //       a dangling transaction is created if the station looses power. On the other hand, at one extra
    //       write per hour here we'll end up exhausting one block of flash memory on an ESP32 every ~11 years,
    //       which may not be worth optimizing out (particularly with a wear-leveling filesystem).
    pending_messages_write_count_ ++;
    saveToStorage();
    settings_->MessageFlushCounter.setValue(pending_messages_write_count_);

    pending_messages_changed_ = false;
}

void PendingMessagesModule::saveToStorage() {
    // TODO: This could likely be optimized to avoid writing an empty file repeatedly while there's nothing in
    //  the buffer (and nothing added via the registered suppliers).

    CHARGELAB_LOG_MESSAGE(info) << "Saving pending messages to storage, write count: " << pending_messages_write_count_;
    storage_->write([&](auto file) {
        offline_queue_.write([&](void* data, std::size_t size) {
            auto length = (int)size;
            std::fwrite(&length, sizeof(length), 1, file);
            std::fwrite(data, size, 1, file);
        });

        int terminator = -1;
        std::fwrite(&terminator, sizeof(terminator), 1, file);

        file::json_write_object_to_file(file, active_group_ids_);
        file::json_write_object_to_file(file, group_blacklist_);
        file::json_write_object_to_file(file, transaction_ids_);
        file::json_write_object_to_file(file, sequence_ids_);
        file::json_write_object_to_file(file, pending_messages_write_count_);

        for (auto const& x : live_queue_) {
            file::json_write_object_to_file(file, x);
        }

        for (auto const& supplier : saved_message_suppliers_) {
            if (supplier == nullptr)
                continue;

            (*supplier)([&](auto const& record) {
                file::json_write_object_to_file(file, record);
            });
        }

        return true;
    });
}

void PendingMessagesModule::loadFromStorage() {
    CHARGELAB_LOG_MESSAGE(info) << "Reading pending messages from storage";
    storage_->read([&](auto file) {
        offline_queue_.read([&]() {
            std::optional<std::vector<uint8_t>> result = std::nullopt;

            int length;
            if (std::fread(&length, sizeof(length), 1, file) != 1)
                return result;
            if (length == -1)
                return result;

            if (length < 0 || length > kMaxStorageBlockSize) {
                CHARGELAB_LOG_MESSAGE(error) << "Bad block length encountered reading historic data: " << length;
                return result;
            }

            std::vector<uint8_t> buffer;
            buffer.resize(length);
            if (std::fread(buffer.data(), sizeof(uint8_t), buffer.size(), file) != buffer.size()) {
                CHARGELAB_LOG_MESSAGE(error) << "Failed reading block data";
                return result;
            }

            result = std::move(buffer);
            return result;
        });

        {
            readFromFile(file, active_group_ids_);
            readFromFile(file, group_blacklist_);
            readFromFile(file, transaction_ids_);
            readFromFile(file, sequence_ids_);
            readFromFile(file, pending_messages_write_count_);
        }

        // Loading previous live queue into offline queue
        while (true) {
            auto record = file::json_read_object_from_file<detail::PendingMessageWrapper>(file);
            if (!record.has_value())
                break;

            offline_queue_.pushBack(record.value());
        }

        return true;
    });
}

int PendingMessagesModule::calculateAllPendingSavedInfoSize() {
    int total_bytes = 0;
    total_bytes += offline_queue_.totalBytes();
    total_bytes += sizeof(int); // integer terminator size

    total_bytes += calculate_size(active_group_ids_) + 1; // Count the newline '\n' character
    total_bytes += calculate_size(group_blacklist_) + 1;  // Count the newline '\n' character
    total_bytes += calculate_size(transaction_ids_) + 1;  // Count the newline '\n' character
    total_bytes += calculate_size(sequence_ids_) + 1;  // Count the newline '\n' character
    total_bytes += calculate_size(pending_messages_write_count_) + 1;  // Count the newline '\n' character

    for (auto const& x : live_queue_) {
        if (x.action_id1_6.has_value() && x.action_id1_6.value() == ocpp1_6::ActionId::kStartTransaction)
            continue;
        total_bytes += calculate_size(x) + 1; // Count the newline '\n' character
    }

    for (auto const& supplier : saved_message_suppliers_) {
        if (supplier == nullptr)
            continue;

        (*supplier)([&](auto const& record) {
            total_bytes += calculate_size(record) + 1; // Count the newline '\n' character
        });
    }

    return total_bytes;
}

bool PendingMessagesModule::shouldRemoveSequenceId(detail::PendingMessageWrapper const& wrapper) {
    if (!wrapper.action_id2_0 || wrapper.action_id2_0.value() != chargelab::ocpp2_0::ActionId::kTransactionEvent)
        return false;

    auto const request = read_json_from_string<ocpp2_0::TransactionEventRequest>(wrapper.payload);
    if (!request.has_value()) {
        CHARGELAB_LOG_MESSAGE(warning) << "Failed read TransactionEvent request from: " << wrapper.payload;
        return false;
    }

    return request->eventType == chargelab::ocpp2_0::TransactionEventEnumType::kEnded;
}

} // namespace chargelab

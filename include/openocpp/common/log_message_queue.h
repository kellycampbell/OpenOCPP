#ifndef CHARGELAB_OPEN_FIRMWARE_LOG_MESSAGE_QUEUE_H
#define CHARGELAB_OPEN_FIRMWARE_LOG_MESSAGE_QUEUE_H

#include "openocpp/common/logging.h"
#include "openocpp/model/system_types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

namespace chargelab {
    /**
     * A bounded, fixed-capacity queue of pre-rendered log lines, sized entirely at compile time.
     *
     * This exists specifically for the logging listener path, which runs on whatever task emitted the
     * log message. That means:
     *
     *   - Entries are trivially copyable PODs. Handing one to the consumer is a memcpy, not a transfer
     *     of ownership, so there is no way for a producer and a consumer to end up owning the same
     *     heap block (the failure mode that RingBuffer<T> with a std::string payload has when it is
     *     used across tasks without a lock).
     *   - pushBack performs no allocation. Producers must not call malloc while inside the logging
     *     callback, which already runs under the global logging mutex.
     *   - The lock is held only for the duration of a memcpy, and no method here logs, so a producer
     *     can never block behind the consumer for a meaningful amount of time and the two mutexes can
     *     never be taken in opposing orders.
     *
     * When the queue is full the oldest entry is overwritten, matching the previous RingBuffer
     * behaviour. Overwrites are counted so the consumer can report them.
     */
    template <int Capacity, int MaxLineBytes>
    class LogMessageQueue {
        static_assert(MaxLineBytes > 0 && MaxLineBytes <= 255, "length is stored as a uint8_t");

    public:
        // Note: field order matters. Putting the 64-bit timestamp first and packing the small fields
        // behind it leaves no padding, which is what lets a 240 byte line share a 256 byte slot.
        struct Entry {
            SystemTimeMillis timestamp;
            int message_index;

            // Bytes of this line that did not fit in text; zero if it was captured in full.
            // Saturates rather than wrapping on absurdly long lines.
            std::uint16_t truncated_bytes;

            // Note: stored narrow; use logLevel() to read it back.
            std::uint8_t level;

            // Note: text is not null terminated; length is authoritative.
            std::uint8_t length;

            char text[MaxLineBytes];

            [[nodiscard]] logging::LogLevel logLevel() const {
                return (logging::LogLevel)level;
            }

            [[nodiscard]] bool truncated() const {
                return truncated_bytes > 0;
            }
        };

        static constexpr int capacity() {
            return Capacity;
        }

        static constexpr int maxLineBytes() {
            return MaxLineBytes;
        }

        /**
         * Called from arbitrary tasks. The message is truncated to MaxLineBytes; prefix is written
         * first so file/line context survives truncation.
         *
         * Returns false if this push overwrote an unread entry.
         */
        bool pushBack(
                int message_index,
                SystemTimeMillis timestamp,
                logging::LogLevel level,
                std::string_view const& prefix,
                std::string_view const& message
        ) {
            std::lock_guard lock {mutex_};

            bool overwrote = false;
            int index;
            if (size_ < Capacity) {
                index = (begin_ + size_) % Capacity;
                size_++;
            } else {
                index = begin_;
                begin_ = (begin_ + 1) % Capacity;
                overwrote = true;
                dropped_++;
            }

            auto& entry = buffer_[index];
            entry.message_index = message_index;
            entry.timestamp = timestamp;
            entry.level = (std::uint8_t)level;
            entry.length = 0;

            std::size_t overflow = 0;
            appendTruncated(entry, prefix, overflow);
            appendTruncated(entry, message, overflow);
            entry.truncated_bytes = (std::uint16_t)std::min(overflow, (std::size_t)UINT16_MAX);

            return !overwrote;
        }

        /**
         * Called from the consuming task. Copies the oldest entry into out and removes it.
         */
        bool popFront(Entry& out) {
            std::lock_guard lock {mutex_};
            if (size_ <= 0) {
                return false;
            }

            out = buffer_[begin_];
            begin_ = (begin_ + 1) % Capacity;
            size_--;
            return true;
        }

        [[nodiscard]] int size() const {
            std::lock_guard lock {mutex_};
            return size_;
        }

        [[nodiscard]] bool empty() const {
            return size() <= 0;
        }

        /**
         * Returns the number of entries dropped since the last call and resets the counter.
         */
        int takeDropped() {
            std::lock_guard lock {mutex_};
            auto const result = dropped_;
            dropped_ = 0;
            return result;
        }

    private:
        static void appendTruncated(Entry& entry, std::string_view const& text, std::size_t& overflow) {
            if (text.empty()) {
                return;
            }

            auto const remaining = MaxLineBytes - (int)entry.length;
            if (remaining <= 0) {
                overflow += text.size();
                return;
            }

            auto const count = std::min((std::size_t)remaining, text.size());
            std::memcpy(entry.text + entry.length, text.data(), count);
            entry.length = (std::uint8_t)(entry.length + count);
            overflow += text.size() - count;
        }

    private:
        mutable std::mutex mutex_;
        std::array<Entry, Capacity> buffer_ {};
        int begin_ = 0;
        int size_ = 0;
        int dropped_ = 0;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_LOG_MESSAGE_QUEUE_H

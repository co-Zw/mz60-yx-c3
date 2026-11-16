#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

// Bounded encoded-packet window. The caller serializes access and delivers
// popped packets in order. Deadlines use local arrival time, not server clocks.
template<typename Packet>
class AudioReorderBuffer {
public:
    static constexpr size_t kCapacity = 8;
    static constexpr int64_t kStartupUs = 60000;
    static constexpr int64_t kGapWaitUs = 120000;
    struct Stats {
        uint32_t delivered = 0, reordered = 0, missing = 0, late = 0, overflow = 0;
    };
    struct Output {
        std::unique_ptr<Packet> packet;
        uint32_t sequence = 0;
        uint32_t missing = 0;
    };

    void Reset() {
        pending_.clear();
        initialized_ = false;
        delivered_ = false;
        stats_ = {};
    }

    bool Push(uint32_t sequence, std::unique_ptr<Packet> packet, int64_t now_us) {
        if (!packet) {
            return false;
        }
        if (!initialized_) {
            initialized_ = true;
            expected_ = highest_ = sequence;
            startup_us_ = now_us;
        }
        const int32_t offset = static_cast<int32_t>(sequence - expected_);
        if (offset < 0) {
            if (!delivered_ && offset >= -static_cast<int32_t>(kCapacity)) {
                expected_ = sequence;
            } else {
                ++stats_.late;
                return false;
            }
        }
        if (pending_.find(sequence) != pending_.end()) {
            ++stats_.late;
            return false;
        }
        if (pending_.size() >= kCapacity) {
            ++stats_.overflow;
            return false;
        }
        const bool reordered = static_cast<int32_t>(sequence - highest_) < 0;
        if (static_cast<int32_t>(sequence - highest_) > 0) {
            highest_ = sequence;
        }
        pending_.emplace(sequence, Entry{std::move(packet), now_us, reordered});
        return true;
    }

    Output Pop(int64_t now_us, bool force = false) {
        if (pending_.empty() || (!delivered_ && !force && pending_.size() < kCapacity &&
                               now_us - startup_us_ < kStartupUs)) {
            return {};
        }
        auto next = pending_.find(expected_);
        uint32_t missing = 0;
        if (next == pending_.end()) {
            // Numeric map ordering alone is wrong across sequence rollover.
            uint32_t nearest = UINT32_MAX;
            int64_t oldest_us = now_us;
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                const uint32_t distance = it->first - expected_;
                if (distance < nearest) {
                    nearest = distance;
                    next = it;
                }
                if (it->second.arrived_us < oldest_us) {
                    oldest_us = it->second.arrived_us;
                }
            }
            if (!force && pending_.size() < kCapacity && now_us - oldest_us < kGapWaitUs) {
                return {};
            }
            missing = nearest;
        }
        Output result{std::move(next->second.packet), next->first, missing};
        ++stats_.delivered;
        stats_.missing += missing;
        stats_.reordered += next->second.reordered;
        expected_ = next->first + 1;
        delivered_ = true;
        pending_.erase(next);
        return result;
    }

    const Stats& stats() const { return stats_; }
    size_t size() const { return pending_.size(); }

private:
    struct Entry {
        std::unique_ptr<Packet> packet;
        int64_t arrived_us;
        bool reordered;
    };
    std::map<uint32_t, Entry> pending_;
    Stats stats_;
    bool initialized_ = false, delivered_ = false;
    uint32_t expected_ = 0, highest_ = 0;
    int64_t startup_us_ = 0;
};

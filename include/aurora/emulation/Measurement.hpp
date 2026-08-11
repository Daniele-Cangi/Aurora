#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aurora::emulation {

enum class FeedbackRttObservation {
    RECORDED,
    DUPLICATE,
    UNKNOWN_SEQUENCE
};

struct FeedbackRttSummary {
    std::size_t count = 0;
    std::uint64_t min_us = 0;
    std::uint64_t mean_us = 0;
    std::uint64_t max_us = 0;
};

// Correlates an authenticated forward sequence with the first authenticated
// feedback that echoes it. Both endpoints are sampled on the sender's steady
// clock; this is an application feedback RTT, not one-way network latency.
class FeedbackRttTracker {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    bool record_sent(std::uint64_t sequence,
                     TimePoint sent_at = Clock::now()) {
        return sent_.emplace(sequence, sent_at).second;
    }

    [[nodiscard]] FeedbackRttObservation observe(
        std::uint64_t echoed_sequence,
        bool terminal,
        TimePoint received_at = Clock::now()) {
        const auto sent = sent_.find(echoed_sequence);
        if (sent == sent_.end()) {
            return FeedbackRttObservation::UNKNOWN_SEQUENCE;
        }
        if (!observed_.insert(echoed_sequence).second) {
            return FeedbackRttObservation::DUPLICATE;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            received_at - sent->second).count();
        if (elapsed < 0) {
            throw std::invalid_argument(
                "feedback RTT: receive time precedes send time");
        }
        // Retain microsecond-scale evidence without turning a sub-microsecond
        // loopback observation into a missing/zero sample.
        const auto rounded_us = static_cast<std::uint64_t>(
            std::max<std::int64_t>(1, (elapsed + 999) / 1000));
        samples_us_.push_back(rounded_us);
        if (terminal) terminal_samples_us_.push_back(rounded_us);
        return FeedbackRttObservation::RECORDED;
    }

    [[nodiscard]] FeedbackRttSummary summary() const {
        return summarize(samples_us_);
    }

    [[nodiscard]] FeedbackRttSummary terminal_summary() const {
        return summarize(terminal_samples_us_);
    }

private:
    static FeedbackRttSummary summarize(
        const std::vector<std::uint64_t>& samples) {
        if (samples.empty()) return {};
        const auto [minimum, maximum] =
            std::minmax_element(samples.begin(), samples.end());
        const auto total = std::accumulate(
            samples.begin(), samples.end(), std::uint64_t{0});
        return {
            samples.size(),
            *minimum,
            (total + samples.size() / 2) / samples.size(),
            *maximum,
        };
    }

    std::unordered_map<std::uint64_t, TimePoint> sent_;
    std::unordered_set<std::uint64_t> observed_;
    std::vector<std::uint64_t> samples_us_;
    std::vector<std::uint64_t> terminal_samples_us_;
};

} // namespace aurora::emulation

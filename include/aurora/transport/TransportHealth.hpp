#pragma once

#include "Generation.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace aurora::transport {

// Health is derived only from authoritative decode reports. Nonterminal polls are
// progress observations, not delivery failures.
struct TransportHealth {
    double ewma_coverage = 0.0;
    double ewma_fail_rate = 0.0;
    double ewma_panic_rate = 0.0;
    int success_count = 0;
    int fail_count = 0;
    int recent_good_streak = 0;
    int recent_bad_streak = 0;
    DecodeStatus last_status = DecodeStatus::NO_PROGRESS;

    void observe(const DecodeReport& report) {
        auto& seen = generations_[report.generation_id];
        const bool new_progress = report.symbols_observed > seen.symbols_observed ||
                                  report.recovered_bytes > seen.recovered_bytes ||
                                  report.status != seen.status;
        if (new_progress) {
            constexpr double coverage_alpha = 0.2;
            if (!has_coverage_sample_) {
                ewma_coverage = report.coverage;
                has_coverage_sample_ = true;
            } else {
                ewma_coverage = (1.0 - coverage_alpha) * ewma_coverage +
                                coverage_alpha * report.coverage;
            }
            seen.symbols_observed = report.symbols_observed;
            seen.recovered_bytes = report.recovered_bytes;
            seen.status = report.status;
            last_status = report.status;
        }

        if (seen.terminal_recorded) {
            return;
        }
        if (report.delivered()) {
            record_terminal(false);
            seen.terminal_recorded = true;
        } else if (report.terminal_failure()) {
            record_terminal(true);
            seen.terminal_recorded = true;
        }
    }

private:
    struct ObservedGeneration {
        std::uint32_t symbols_observed = 0;
        std::size_t recovered_bytes = 0;
        DecodeStatus status = DecodeStatus::NO_PROGRESS;
        bool terminal_recorded = false;
    };

    void record_terminal(bool failed) {
        constexpr double failure_alpha = 0.1;
        const double instant_failure = failed ? 1.0 : 0.0;
        ewma_fail_rate = (1.0 - failure_alpha) * ewma_fail_rate +
                         failure_alpha * instant_failure;
        if (failed) {
            ++fail_count;
            ++recent_bad_streak;
            recent_good_streak = 0;
        } else {
            ++success_count;
            ++recent_good_streak;
            recent_bad_streak = 0;
        }
    }

    bool has_coverage_sample_ = false;
    std::unordered_map<std::string, ObservedGeneration> generations_;
};

} // namespace aurora::transport

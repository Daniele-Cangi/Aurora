#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace aurora::safety {

enum class SafetyState {
    NO_EVIDENCE = -1,
    HEALTHY = 0,
    DEGRADED = 1,
    CRITICAL = 2
};

struct SafetyConfig {
    double duty_budget_critical_threshold = 0.2;
    double duty_budget_degraded_threshold = 0.3;
    double duty_budget_recovery_margin = 0.05;
    double fail_rate_critical_threshold = 0.3;
    double fail_rate_degraded_threshold = 0.21;
    double fail_rate_recovery_margin = 0.05;
    int window_size = 50;
    int minimum_window_samples = 5;
    int escalation_samples = 2;
    int recovery_samples = 3;
    std::uint64_t maximum_observation_age_ms = 5'000;

    static SafetyConfig default_config() {
        return {};
    }

    bool operator==(const SafetyConfig&) const = default;
};

struct SafetyEvidenceSample {
    std::uint64_t observed_at_ms = 0;
    double duty_left = 0.0;
    double nerve_fail_rate = 0.0;
    double gland_fail_rate = 0.0;
    double muscle_fail_rate = 0.0;
    double nerve_cov = 0.0;
    double gland_cov = 0.0;
    double muscle_cov = 0.0;
    bool nerve_has_evidence = false;
    bool gland_has_evidence = false;
    bool muscle_has_evidence = false;

    bool operator==(const SafetyEvidenceSample&) const = default;
};

struct TelemetrySample {
    std::uint64_t observed_at_ms = 0;
    std::uint64_t now_ms = 0;
    int step = 0;
    int have = 0;
    int need = 0;
    std::string mode;
    int tries = 0;
    int successes = 0;
    double reward = 0.0;
    double snr_rf = 0.0;
    double snr_ir = 0.0;
    double snr_bs = 0.0;
    double soc_src = 0.0;
    double duty_left = 0.0;
    double elapsed_s = 0.0;
    double nerve_fail_rate = 0.0;
    double gland_fail_rate = 0.0;
    double muscle_fail_rate = 0.0;
    double nerve_cov = 0.0;
    double gland_cov = 0.0;
    double muscle_cov = 0.0;
    int nerve_bad_streak = 0;
    int gland_bad_streak = 0;
    int muscle_bad_streak = 0;
    bool nerve_has_evidence = false;
    bool gland_has_evidence = false;
    bool muscle_has_evidence = false;
};

inline SafetyEvidenceSample evidence_from(const TelemetrySample& sample) {
    return {
        sample.observed_at_ms,
        sample.duty_left,
        sample.nerve_fail_rate,
        sample.gland_fail_rate,
        sample.muscle_fail_rate,
        sample.nerve_cov,
        sample.gland_cov,
        sample.muscle_cov,
        sample.nerve_has_evidence,
        sample.gland_has_evidence,
        sample.muscle_has_evidence};
}

inline int safety_severity(SafetyState state) {
    switch (state) {
        case SafetyState::HEALTHY: return 0;
        case SafetyState::DEGRADED: return 1;
        case SafetyState::CRITICAL: return 2;
        case SafetyState::NO_EVIDENCE: return -1;
    }
    return -1;
}

inline bool valid_safety_state(SafetyState state) {
    switch (state) {
        case SafetyState::NO_EVIDENCE:
        case SafetyState::HEALTHY:
        case SafetyState::DEGRADED:
        case SafetyState::CRITICAL:
            return true;
    }
    return false;
}

inline bool valid_fraction(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

inline std::optional<std::string> safety_config_error(const SafetyConfig& config) {
    const bool valid_duty_thresholds =
        std::isfinite(config.duty_budget_critical_threshold) &&
        std::isfinite(config.duty_budget_degraded_threshold) &&
        std::isfinite(config.duty_budget_recovery_margin) &&
        config.duty_budget_critical_threshold >= 0.0 &&
        config.duty_budget_critical_threshold < config.duty_budget_degraded_threshold &&
        config.duty_budget_degraded_threshold <= 1.0 &&
        config.duty_budget_recovery_margin >= 0.0 &&
        config.duty_budget_degraded_threshold + config.duty_budget_recovery_margin <= 1.0;
    const bool valid_failure_thresholds =
        std::isfinite(config.fail_rate_critical_threshold) &&
        std::isfinite(config.fail_rate_degraded_threshold) &&
        std::isfinite(config.fail_rate_recovery_margin) &&
        config.fail_rate_degraded_threshold >= 0.0 &&
        config.fail_rate_degraded_threshold < config.fail_rate_critical_threshold &&
        config.fail_rate_critical_threshold <= 1.0 &&
        config.fail_rate_recovery_margin >= 0.0 &&
        config.fail_rate_recovery_margin <= config.fail_rate_degraded_threshold;
    const bool valid_windows = config.window_size > 0 &&
        config.minimum_window_samples > 0 &&
        config.minimum_window_samples <= config.window_size &&
        config.escalation_samples > 0 && config.recovery_samples > 0 &&
        config.maximum_observation_age_ms > 0;
    if (!valid_duty_thresholds || !valid_failure_thresholds || !valid_windows) {
        return "invalid hysteresis/freshness configuration";
    }
    return std::nullopt;
}

inline std::optional<std::string> safety_evidence_error(
    const SafetyEvidenceSample& sample) {
    if (!valid_fraction(sample.duty_left) ||
        !valid_fraction(sample.nerve_fail_rate) ||
        !valid_fraction(sample.gland_fail_rate) ||
        !valid_fraction(sample.muscle_fail_rate) ||
        !valid_fraction(sample.nerve_cov) ||
        !valid_fraction(sample.gland_cov) ||
        !valid_fraction(sample.muscle_cov)) {
        return "invalid telemetry fraction";
    }
    return std::nullopt;
}

struct SafetyMonitorSnapshot {
    SafetyConfig config;
    std::vector<SafetyEvidenceSample> samples;
    SafetyState current_state = SafetyState::NO_EVIDENCE;
    SafetyState pending_state = SafetyState::NO_EVIDENCE;
    int pending_observations = 0;
    std::uint64_t last_now_ms = 0;
    bool clock_initialized = false;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (const auto error = safety_config_error(config)) return error;
        if (!valid_safety_state(current_state) || !valid_safety_state(pending_state) ||
            pending_observations < 0 ||
            samples.size() > static_cast<std::size_t>(config.window_size)) {
            return "invalid safety monitor scalar state";
        }
        if (!clock_initialized) {
            if (!samples.empty() || current_state != SafetyState::NO_EVIDENCE ||
                pending_state != SafetyState::NO_EVIDENCE || pending_observations != 0 ||
                last_now_ms != 0) {
                return "uninitialized safety clock has controller state";
            }
            return std::nullopt;
        }
        std::uint64_t previous_observation = 0;
        bool have_previous = false;
        for (const auto& sample : samples) {
            if (const auto error = safety_evidence_error(sample)) return error;
            if (sample.observed_at_ms > last_now_ms ||
                last_now_ms - sample.observed_at_ms > config.maximum_observation_age_ms ||
                (have_previous && sample.observed_at_ms < previous_observation)) {
                return "invalid safety evidence time ordering";
            }
            previous_observation = sample.observed_at_ms;
            have_previous = true;
        }
        if (pending_observations == 0 && pending_state != current_state) {
            return "idle pending state does not match current state";
        }
        if (pending_observations > 0) {
            if (pending_state == current_state || pending_state == SafetyState::NO_EVIDENCE) {
                return "invalid active pending transition";
            }
            const bool escalating = safety_severity(pending_state) >
                                    safety_severity(current_state);
            const int required = escalating
                ? config.escalation_samples
                : config.recovery_samples;
            if (pending_observations >= required) {
                return "completed pending transition was not applied";
            }
        }
        return std::nullopt;
    }

    bool operator==(const SafetyMonitorSnapshot&) const = default;
};

class SafetyMonitor {
public:
    explicit SafetyMonitor(const SafetyConfig& config = SafetyConfig::default_config())
        : config_(config) {
        validate_config();
    }

    explicit SafetyMonitor(const SafetyMonitorSnapshot& snapshot)
        : config_(snapshot.config),
          samples_(snapshot.samples.begin(), snapshot.samples.end()),
          current_state_(snapshot.current_state),
          pending_state_(snapshot.pending_state),
          pending_observations_(snapshot.pending_observations),
          last_now_ms_(snapshot.last_now_ms),
          clock_initialized_(snapshot.clock_initialized) {
        if (const auto error = snapshot.validation_error()) {
            throw std::invalid_argument("safety monitor snapshot: " + *error);
        }
    }

    void observe(const TelemetrySample& sample) {
        observe(evidence_from(sample), sample.now_ms);
    }

    void observe(const SafetyEvidenceSample& sample, std::uint64_t now_ms) {
        if (const auto error = safety_evidence_error(sample)) {
            throw std::invalid_argument("safety monitor: " + *error);
        }
        if (sample.observed_at_ms > now_ms) {
            throw std::invalid_argument("safety monitor: observation is from the future");
        }
        if (clock_initialized_ && now_ms < last_now_ms_) {
            throw std::invalid_argument("safety monitor: evaluation time moved backwards");
        }
        if (!samples_.empty() &&
            sample.observed_at_ms < samples_.back().observed_at_ms) {
            throw std::invalid_argument("safety monitor: observation time moved backwards");
        }
        advance_clock(now_ms);

        prune_stale(now_ms);
        if (now_ms - sample.observed_at_ms <= config_.maximum_observation_age_ms) {
            samples_.push_back(sample);
            if (samples_.size() > static_cast<std::size_t>(config_.window_size)) {
                samples_.pop_front();
            }
        }
        apply_candidate(classify_window());
    }

    void advance_time(std::uint64_t now_ms) {
        advance_clock(now_ms);
        prune_stale(now_ms);
        apply_candidate(classify_window());
    }

    [[nodiscard]] SafetyState state() const { return current_state_; }
    [[nodiscard]] SafetyState pending_state() const { return pending_state_; }
    [[nodiscard]] int pending_observations() const { return pending_observations_; }

    [[nodiscard]] SafetyMonitorSnapshot snapshot() const {
        return {
            config_,
            {samples_.begin(), samples_.end()},
            current_state_,
            pending_state_,
            pending_observations_,
            last_now_ms_,
            clock_initialized_};
    }

private:
    void validate_config() const {
        if (const auto error = safety_config_error(config_)) {
            throw std::invalid_argument("safety monitor: " + *error);
        }
    }

    void advance_clock(std::uint64_t now_ms) {
        if (clock_initialized_ && now_ms < last_now_ms_) {
            throw std::invalid_argument("safety monitor: evaluation time moved backwards");
        }
        last_now_ms_ = now_ms;
        clock_initialized_ = true;
    }

    void prune_stale(std::uint64_t now_ms) {
        while (!samples_.empty() &&
               now_ms - samples_.front().observed_at_ms >
                   config_.maximum_observation_age_ms) {
            samples_.pop_front();
        }
    }

    [[nodiscard]] SafetyState classify_window() const {
        if (samples_.size() < static_cast<std::size_t>(
                config_.minimum_window_samples)) {
            return SafetyState::NO_EVIDENCE;
        }

        double fail_rate_sum = 0.0;
        std::size_t evidence_count = 0;
        double min_duty = 1.0;
        for (const auto& sample : samples_) {
            if (sample.nerve_has_evidence) {
                fail_rate_sum += sample.nerve_fail_rate;
                ++evidence_count;
            }
            if (sample.gland_has_evidence) {
                fail_rate_sum += sample.gland_fail_rate;
                ++evidence_count;
            }
            if (sample.muscle_has_evidence) {
                fail_rate_sum += sample.muscle_fail_rate;
                ++evidence_count;
            }
            min_duty = std::min(min_duty, sample.duty_left);
        }
        const double average_failure = evidence_count == 0
            ? 0.0
            : fail_rate_sum / static_cast<double>(evidence_count);

        const bool critical_entry =
            min_duty < config_.duty_budget_critical_threshold ||
            (evidence_count > 0 &&
             average_failure > config_.fail_rate_critical_threshold);
        if (critical_entry) return SafetyState::CRITICAL;
        if (evidence_count == 0) return SafetyState::NO_EVIDENCE;

        const bool degraded_entry =
            min_duty < config_.duty_budget_degraded_threshold ||
            average_failure > config_.fail_rate_degraded_threshold;
        if (current_state_ == SafetyState::CRITICAL) {
            const bool remain_critical =
                min_duty < config_.duty_budget_critical_threshold +
                               config_.duty_budget_recovery_margin ||
                average_failure > config_.fail_rate_critical_threshold -
                                      config_.fail_rate_recovery_margin;
            if (remain_critical) return SafetyState::CRITICAL;
        }
        if (current_state_ == SafetyState::CRITICAL ||
            current_state_ == SafetyState::DEGRADED) {
            const bool remain_degraded =
                min_duty < config_.duty_budget_degraded_threshold +
                               config_.duty_budget_recovery_margin ||
                average_failure > config_.fail_rate_degraded_threshold -
                                      config_.fail_rate_recovery_margin;
            if (remain_degraded) return SafetyState::DEGRADED;
        }
        return degraded_entry ? SafetyState::DEGRADED : SafetyState::HEALTHY;
    }

    void clear_pending() {
        pending_state_ = current_state_;
        pending_observations_ = 0;
    }

    void apply_candidate(SafetyState candidate) {
        if (candidate == SafetyState::NO_EVIDENCE) {
            current_state_ = SafetyState::NO_EVIDENCE;
            clear_pending();
            return;
        }
        if (current_state_ == SafetyState::NO_EVIDENCE) {
            current_state_ = candidate;
            clear_pending();
            return;
        }
        if (candidate == current_state_) {
            clear_pending();
            return;
        }
        if (pending_state_ != candidate) {
            pending_state_ = candidate;
            pending_observations_ = 1;
        } else {
            ++pending_observations_;
        }

        const bool escalating = safety_severity(candidate) >
                                safety_severity(current_state_);
        const int required = escalating
            ? config_.escalation_samples
            : config_.recovery_samples;
        if (pending_observations_ < required) return;

        if (!escalating && current_state_ == SafetyState::CRITICAL &&
            candidate == SafetyState::HEALTHY) {
            current_state_ = SafetyState::DEGRADED;
        } else {
            current_state_ = candidate;
        }
        clear_pending();
    }

    SafetyConfig config_;
    std::deque<SafetyEvidenceSample> samples_;
    SafetyState current_state_ = SafetyState::NO_EVIDENCE;
    SafetyState pending_state_ = SafetyState::NO_EVIDENCE;
    int pending_observations_ = 0;
    std::uint64_t last_now_ms_ = 0;
    bool clock_initialized_ = false;
};

} // namespace aurora::safety

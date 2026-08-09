#pragma once

#include <deque>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace aurora {
namespace safety {

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
    
    static SafetyConfig default_config() {
        return SafetyConfig();
    }
};

struct TelemetrySample {
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

class SafetyMonitor {
private:
    SafetyConfig config_;
    std::deque<TelemetrySample> samples_;
    SafetyState current_state_ = SafetyState::NO_EVIDENCE;
    SafetyState pending_state_ = SafetyState::NO_EVIDENCE;
    int pending_observations_ = 0;
    
public:
    SafetyMonitor(const SafetyConfig& cfg = SafetyConfig::default_config())
        : config_(cfg) {
        validate_config();
    }
    
    void observe(const TelemetrySample& sample) {
        validate_sample(sample);
        samples_.push_back(sample);
        if (samples_.size() > static_cast<size_t>(config_.window_size)) {
            samples_.pop_front();
        }

        apply_candidate(classify_window());
    }

    SafetyState state() const {
        return current_state_;
    }

    SafetyState pending_state() const {
        return pending_state_;
    }

    int pending_observations() const {
        return pending_observations_;
    }

private:
    void validate_config() const {
        const bool valid_duty_thresholds =
            std::isfinite(config_.duty_budget_critical_threshold) &&
            std::isfinite(config_.duty_budget_degraded_threshold) &&
            std::isfinite(config_.duty_budget_recovery_margin) &&
            config_.duty_budget_critical_threshold >= 0.0 &&
            config_.duty_budget_critical_threshold <
                config_.duty_budget_degraded_threshold &&
            config_.duty_budget_degraded_threshold <= 1.0 &&
            config_.duty_budget_recovery_margin >= 0.0 &&
            config_.duty_budget_degraded_threshold +
                config_.duty_budget_recovery_margin <= 1.0;
        const bool valid_failure_thresholds =
            std::isfinite(config_.fail_rate_critical_threshold) &&
            std::isfinite(config_.fail_rate_degraded_threshold) &&
            std::isfinite(config_.fail_rate_recovery_margin) &&
            config_.fail_rate_degraded_threshold >= 0.0 &&
            config_.fail_rate_degraded_threshold <
                config_.fail_rate_critical_threshold &&
            config_.fail_rate_critical_threshold <= 1.0 &&
            config_.fail_rate_recovery_margin >= 0.0 &&
            config_.fail_rate_recovery_margin <=
                config_.fail_rate_degraded_threshold;
        const bool valid_windows = config_.window_size > 0 &&
            config_.minimum_window_samples > 0 &&
            config_.minimum_window_samples <= config_.window_size &&
            config_.escalation_samples > 0 && config_.recovery_samples > 0;
        if (!valid_duty_thresholds || !valid_failure_thresholds || !valid_windows) {
            throw std::invalid_argument("safety monitor: invalid hysteresis configuration");
        }
    }

    static bool valid_fraction(double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    static void validate_sample(const TelemetrySample& sample) {
        if (!valid_fraction(sample.duty_left) ||
            (sample.nerve_has_evidence && !valid_fraction(sample.nerve_fail_rate)) ||
            (sample.gland_has_evidence && !valid_fraction(sample.gland_fail_rate)) ||
            (sample.muscle_has_evidence && !valid_fraction(sample.muscle_fail_rate))) {
            throw std::invalid_argument("safety monitor: invalid telemetry fraction");
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

        for (const auto& s : samples_) {
            if (s.nerve_has_evidence) {
                fail_rate_sum += s.nerve_fail_rate;
                ++evidence_count;
            }
            if (s.gland_has_evidence) {
                fail_rate_sum += s.gland_fail_rate;
                ++evidence_count;
            }
            if (s.muscle_has_evidence) {
                fail_rate_sum += s.muscle_fail_rate;
                ++evidence_count;
            }
            min_duty = std::min(min_duty, s.duty_left);
        }
        const double avg_fail_rate = evidence_count == 0
            ? 0.0
            : fail_rate_sum / static_cast<double>(evidence_count);

        const bool critical_entry =
            min_duty < config_.duty_budget_critical_threshold ||
            (evidence_count > 0 &&
             avg_fail_rate > config_.fail_rate_critical_threshold);
        if (critical_entry) return SafetyState::CRITICAL;
        if (evidence_count == 0) return SafetyState::NO_EVIDENCE;

        const bool degraded_entry =
            min_duty < config_.duty_budget_degraded_threshold ||
            avg_fail_rate > config_.fail_rate_degraded_threshold;

        // Recovery uses wider exit thresholds than entry. This Schmitt-trigger
        // shape prevents samples close to a boundary from repeatedly flipping
        // policy mode even before the consecutive-observation gate is applied.
        if (current_state_ == SafetyState::CRITICAL) {
            const bool remain_critical =
                min_duty < config_.duty_budget_critical_threshold +
                               config_.duty_budget_recovery_margin ||
                avg_fail_rate > config_.fail_rate_critical_threshold -
                                    config_.fail_rate_recovery_margin;
            if (remain_critical) return SafetyState::CRITICAL;
        }
        if (current_state_ == SafetyState::CRITICAL ||
            current_state_ == SafetyState::DEGRADED) {
            const bool remain_degraded =
                min_duty < config_.duty_budget_degraded_threshold +
                               config_.duty_budget_recovery_margin ||
                avg_fail_rate > config_.fail_rate_degraded_threshold -
                                    config_.fail_rate_recovery_margin;
            if (remain_degraded) return SafetyState::DEGRADED;
        }
        return degraded_entry ? SafetyState::DEGRADED : SafetyState::HEALTHY;
    }

    static int severity(SafetyState state) {
        switch (state) {
            case SafetyState::HEALTHY: return 0;
            case SafetyState::DEGRADED: return 1;
            case SafetyState::CRITICAL: return 2;
            case SafetyState::NO_EVIDENCE: return -1;
        }
        return -1;
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

        const bool escalating = severity(candidate) > severity(current_state_);
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
};

} // namespace safety
} // namespace aurora

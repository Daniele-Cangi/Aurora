#pragma once

#include <deque>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstddef>

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
    double fail_rate_critical_threshold = 0.3;
    int window_size = 50;
    
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
    
public:
    SafetyMonitor(const SafetyConfig& cfg = SafetyConfig::default_config())
        : config_(cfg) {}
    
    void observe(const TelemetrySample& sample) {
        samples_.push_back(sample);
        if (samples_.size() > static_cast<size_t>(config_.window_size)) {
            samples_.pop_front();
        }
        
        // Calcola stato basato su metriche recenti
        if (samples_.size() < 5) {
            current_state_ = SafetyState::NO_EVIDENCE;
            return;
        }
        
        // Analizza ultimi campioni
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
        
        // Determina stato
        if (min_duty < config_.duty_budget_critical_threshold) {
            current_state_ = SafetyState::CRITICAL;
        } else if (evidence_count == 0) {
            current_state_ = SafetyState::NO_EVIDENCE;
        } else if (
            avg_fail_rate > config_.fail_rate_critical_threshold) {
            current_state_ = SafetyState::CRITICAL;
        } else if (min_duty < config_.duty_budget_critical_threshold * 1.5 ||
                   avg_fail_rate > config_.fail_rate_critical_threshold * 0.7) {
            current_state_ = SafetyState::DEGRADED;
        } else {
            current_state_ = SafetyState::HEALTHY;
        }
    }
    
    SafetyState state() const {
        return current_state_;
    }
};

} // namespace safety
} // namespace aurora

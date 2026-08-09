#pragma once

#include "../safety/SafetyMonitor.hpp"

#include <cstdint>

namespace aurora::control {

enum class OperatingMode : std::uint8_t {
    CONSERVATIVE = 0,
    NORMAL = 1,
    AGGRESSIVE = 2
};

inline bool valid_operating_mode(OperatingMode mode) {
    switch (mode) {
        case OperatingMode::CONSERVATIVE:
        case OperatingMode::NORMAL:
        case OperatingMode::AGGRESSIVE:
            return true;
    }
    return false;
}

struct OperatingModeInput {
    safety::SafetyState safety_state = safety::SafetyState::NO_EVIDENCE;
    double nerve_fail_rate = 0.0;
    double gland_fail_rate = 0.0;
    double nerve_coverage = 0.0;
    double gland_coverage = 0.0;
};

inline OperatingModeInput operating_mode_input(
    safety::SafetyState state,
    const safety::SafetyEvidenceSample& evidence) {
    return {
        state,
        evidence.nerve_fail_rate,
        evidence.gland_fail_rate,
        evidence.nerve_cov,
        evidence.gland_cov};
}

inline OperatingMode select_operating_mode(const OperatingModeInput& input) {
    if (input.safety_state == safety::SafetyState::NO_EVIDENCE ||
        input.safety_state == safety::SafetyState::CRITICAL) {
        return OperatingMode::CONSERVATIVE;
    }
    if (input.safety_state == safety::SafetyState::DEGRADED) {
        return OperatingMode::NORMAL;
    }
    if (input.nerve_fail_rate < 0.05 &&
        input.gland_fail_rate < 0.05 &&
        input.nerve_coverage > 0.95 &&
        input.gland_coverage > 0.95) {
        return OperatingMode::AGGRESSIVE;
    }
    return OperatingMode::NORMAL;
}

} // namespace aurora::control

#pragma once

#include "OperatingModeController.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>

namespace aurora::control {

// Complete transition of the supervisory controller for one transport action.
// The before snapshot contains the full bounded evidence window and hysteresis
// counters, so replay does not rely on hidden process state.
struct ControllerTransition {
    bool recorded = false;
    std::uint64_t now_ms = 0;
    safety::SafetyEvidenceSample observation;
    safety::SafetyMonitorSnapshot before;
    safety::SafetyMonitorSnapshot after;
    OperatingMode mode_before = OperatingMode::NORMAL;
    OperatingMode mode_after = OperatingMode::NORMAL;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (!recorded) return "controller transition is not recorded";
        if (!valid_operating_mode(mode_before) || !valid_operating_mode(mode_after)) {
            return "invalid operating mode";
        }
        if (const auto error = before.validation_error()) {
            return "invalid before snapshot: " + *error;
        }
        if (const auto error = after.validation_error()) {
            return "invalid after snapshot: " + *error;
        }
        if (!(before.config == after.config)) {
            return "controller configuration changed inside a transition";
        }
        if (const auto error = safety::safety_evidence_error(observation)) {
            return "invalid controller observation: " + *error;
        }
        if (observation.observed_at_ms > now_ms) {
            return "controller observation is from the future";
        }
        if (after.last_now_ms != now_ms || !after.clock_initialized) {
            return "after snapshot does not end at the transition time";
        }

        safety::SafetyMonitor replay(before);
        try {
            replay.observe(observation, now_ms);
        } catch (const std::exception& error) {
            return std::string("controller replay rejected input: ") + error.what();
        }
        if (!(replay.snapshot() == after)) {
            return "safety monitor state transition mismatch";
        }
        const auto expected_mode = select_operating_mode(
            operating_mode_input(after.current_state, observation));
        if (mode_after != expected_mode) {
            return "operating mode transition mismatch";
        }
        return std::nullopt;
    }

    bool operator==(const ControllerTransition&) const = default;
};

} // namespace aurora::control

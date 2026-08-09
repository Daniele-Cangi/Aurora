#pragma once

#include "../transport/Generation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace aurora::safety {

enum class LinkMode : std::uint8_t {
    RF,
    OPTICAL,
    BACKSCATTER
};

enum class AttemptRefusal : std::uint8_t {
    NONE,
    EMPTY_BUFFER,
    NO_ELIGIBLE_PACKET,
    ENERGY,
    DUTY,
    HAL
};

struct SegmentTransportState {
    std::uint32_t segment_id = 0;
    std::uint64_t emitted_symbols = 0;
    std::uint32_t decoder_rank = 0;
    bool complete = false;
    bool expired = false;
};

struct TransportState {
    std::uint64_t observed_at_ms = 0;
    std::uint64_t now_ms = 0;
    double source_energy_reserve = 1.0;
    double rf_duty_remaining = 1.0;
    // Optional deterministic action-cost inputs. Zero means that the caller
    // has no cost estimate; the simulator populates every field.
    double source_energy_capacity_j = 0.0;
    double rf_energy_cost_per_attempt_j = 0.0;
    double optical_energy_cost_per_attempt_j = 0.0;
    double backscatter_energy_cost_per_attempt_j = 0.0;
    double rf_duty_remaining_s = 0.0;
    double rf_airtime_per_attempt_s = 0.0;
    std::uint64_t emitted_symbols = 0;
    std::uint64_t critical_emitted_symbols = 0;
    std::uint32_t decoder_rank = 0;
    std::uint32_t required_rank = 0;
    std::vector<SegmentTransportState> segments;
};

struct TransportDecision {
    LinkMode link = LinkMode::RF;
    std::uint32_t transmission_attempts = 1;
    std::uint32_t repair_symbols = 0;
    bool critical_only = false;
    bool permitted = true;
};

// Canonical action-level transition. HAL/channel samples are recorded inputs;
// replay deterministically recomputes their decisions and every resource delta.
struct TransportAttemptTrace {
    std::uint64_t simulated_now_ms = 0;
    std::uint32_t packet_sequence = 0;
    std::uint32_t symbol_seed = 0;
    std::uint32_t segment_id = 0;
    bool critical = false;
    bool attempted = false;
    bool hal_evaluated = false;
    bool hal_replayable = false;
    bool hal_accepted = false;
    bool transmitted = false;
    bool delivered = false;
    AttemptRefusal refusal = AttemptRefusal::NONE;
    double energy_before_j = 0.0;
    double energy_after_j = 0.0;
    double energy_cost_j = 0.0;
    double duty_before_s = 0.0;
    double duty_after_s = 0.0;
    double rf_airtime_s = 0.0;
    bool lbt_evaluated = false;
    int lbt_threshold_dbm = -95;
    int lbt_first_rssi_dbm = 0;
    bool lbt_second_valid = false;
    int lbt_second_rssi_dbm = 0;
    bool channel_evaluated = false;
    double channel_snr_db = 0.0;
    double channel_coding_gain_db = 0.0;
    double channel_fading_db = 0.0;
    double channel_threshold_db = 0.0;
};

struct TransportExecution {
    bool recorded = false;
    LinkMode link = LinkMode::RF;
    std::uint32_t transmission_attempts = 0;
    std::uint32_t hal_accepted_attempts = 0;
    std::uint32_t delivered_attempts = 0;
    std::uint32_t repair_symbols_emitted = 0;
    bool critical_only = false;
    std::vector<TransportAttemptTrace> attempts;
};

struct TransportDecisionTrace {
    std::string generation_id;
    TransportState observed;
    TransportDecision proposed;
    TransportDecision decision;
    TransportExecution execution;
    std::vector<std::string> constraints_applied;

    [[nodiscard]] std::optional<std::string> execution_error() const {
        if (!execution.recorded) return std::nullopt;
        if (execution.link != decision.link ||
            execution.critical_only != decision.critical_only) {
            return "execution link/scheduling does not match the constrained decision";
        }
        if (execution.transmission_attempts != decision.transmission_attempts ||
            execution.repair_symbols_emitted != decision.repair_symbols) {
            return "execution counts do not match the constrained decision";
        }
        if (execution.hal_accepted_attempts > execution.transmission_attempts ||
            execution.delivered_attempts > execution.hal_accepted_attempts) {
            return "execution outcome counts are inconsistent";
        }
        if (!decision.permitted &&
            (execution.transmission_attempts != 0 ||
             execution.repair_symbols_emitted != 0)) {
            return "rejected decision was executed";
        }
        if (execution.attempts.size() != execution.transmission_attempts) {
            return "execution attempt trace count does not match the decision";
        }
        std::uint32_t hal_accepted = 0;
        std::uint32_t delivered = 0;
        for (std::size_t index = 0; index < execution.attempts.size(); ++index) {
            const auto& attempt = execution.attempts[index];
            if (!attempt.attempted) {
                return "execution contains a non-attempt event";
            }
            if (attempt.simulated_now_ms != observed.now_ms) {
                return "attempt time does not match the admitted state";
            }
            if (execution.critical_only && !attempt.critical) {
                return "critical-only execution contains a non-critical packet";
            }
            if (!observed.segments.empty()) {
                if (attempt.segment_id >= observed.segments.size() ||
                    observed.segments[attempt.segment_id].segment_id != attempt.segment_id ||
                    observed.segments[attempt.segment_id].complete ||
                    observed.segments[attempt.segment_id].expired) {
                    return "attempt selected an unavailable segment";
                }
            }
            if (!valid_attempt_number(attempt.energy_before_j) ||
                !valid_attempt_number(attempt.energy_after_j) ||
                !valid_attempt_number(attempt.energy_cost_j) ||
                !valid_attempt_number(attempt.duty_before_s) ||
                !valid_attempt_number(attempt.duty_after_s) ||
                !valid_attempt_number(attempt.rf_airtime_s)) {
                return "execution contains an invalid resource state";
            }
            if (attempt.hal_accepted) ++hal_accepted;
            if (attempt.delivered) ++delivered;
            if (attempt.delivered && !attempt.transmitted) {
                return "channel delivered a transmission that was not emitted";
            }
            if (attempt.transmitted && !attempt.hal_accepted) {
                return "transmission bypassed HAL acceptance";
            }
            if (attempt.transmitted) {
                if (attempt.refusal != AttemptRefusal::NONE ||
                    !attempt.channel_evaluated) {
                    return "transmitted attempt has inconsistent refusal/channel state";
                }
                if (!near(attempt.energy_after_j,
                          attempt.energy_before_j - attempt.energy_cost_j)) {
                    return "transmitted attempt has an invalid energy transition";
                }
                const double admitted_energy_cost = execution.link == LinkMode::RF
                    ? observed.rf_energy_cost_per_attempt_j
                    : execution.link == LinkMode::OPTICAL
                        ? observed.optical_energy_cost_per_attempt_j
                        : observed.backscatter_energy_cost_per_attempt_j;
                if (admitted_energy_cost > 0.0 &&
                    !near(attempt.energy_cost_j, admitted_energy_cost)) {
                    return "attempt energy cost differs from the admitted estimate";
                }
                if (execution.link == LinkMode::RF &&
                    observed.rf_airtime_per_attempt_s > 0.0 &&
                    !near(attempt.rf_airtime_s,
                          observed.rf_airtime_per_attempt_s)) {
                    return "attempt airtime differs from the admitted estimate";
                }
                const double expected_duty = execution.link == LinkMode::RF
                    ? attempt.duty_before_s - attempt.rf_airtime_s
                    : attempt.duty_before_s;
                if (!near(attempt.duty_after_s, expected_duty)) {
                    return "transmitted attempt has an invalid duty transition";
                }
                if (!valid_channel_number(attempt.channel_snr_db) ||
                    !valid_channel_number(attempt.channel_coding_gain_db) ||
                    !valid_channel_number(attempt.channel_fading_db) ||
                    !valid_channel_number(attempt.channel_threshold_db)) {
                    return "transmitted attempt has invalid channel evidence";
                }
                const bool replayed_delivery =
                    attempt.channel_snr_db + attempt.channel_coding_gain_db +
                        attempt.channel_fading_db > attempt.channel_threshold_db;
                if (replayed_delivery != attempt.delivered) {
                    return "channel outcome does not replay from recorded evidence";
                }
            } else {
                if (attempt.delivered || attempt.channel_evaluated ||
                    attempt.refusal == AttemptRefusal::NONE ||
                    !near(attempt.energy_after_j, attempt.energy_before_j) ||
                    !near(attempt.duty_after_s, attempt.duty_before_s)) {
                    return "refused attempt changed resources or reached the channel";
                }
            }
            if (attempt.hal_replayable && attempt.hal_evaluated) {
                bool replayed_hal = true;
                if (execution.link == LinkMode::RF) {
                    if (!attempt.lbt_evaluated) {
                        return "RF HAL trace is missing LBT evidence";
                    }
                    if (attempt.lbt_first_rssi_dbm < attempt.lbt_threshold_dbm) {
                        if (!attempt.lbt_second_valid) {
                            return "RF LBT trace is missing its second sample";
                        }
                        replayed_hal =
                            attempt.lbt_second_rssi_dbm < attempt.lbt_threshold_dbm;
                    } else {
                        if (attempt.lbt_second_valid) {
                            return "RF LBT trace has an unexpected second sample";
                        }
                        replayed_hal = false;
                    }
                }
                if (replayed_hal != attempt.hal_accepted) {
                    return "HAL outcome does not replay from recorded evidence";
                }
            }
            if (index > 0) {
                const auto& previous = execution.attempts[index - 1];
                if (!near(previous.energy_after_j, attempt.energy_before_j) ||
                    !near(previous.duty_after_s, attempt.duty_before_s)) {
                    return "attempt resource states are not contiguous";
                }
            } else {
                if (observed.source_energy_capacity_j > 0.0 &&
                    !near(attempt.energy_before_j,
                          observed.source_energy_reserve *
                              observed.source_energy_capacity_j)) {
                    return "execution energy does not start from the admitted state";
                }
                if (!near(attempt.duty_before_s,
                          observed.rf_duty_remaining_s)) {
                    return "execution duty does not start from the admitted state";
                }
            }
        }
        if (hal_accepted != execution.hal_accepted_attempts ||
            delivered != execution.delivered_attempts) {
            return "execution aggregates do not match attempt traces";
        }
        return std::nullopt;
    }

private:
    static bool near(double left, double right) {
        return std::abs(left - right) <=
               1e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    }

    static bool valid_attempt_number(double value) {
        return std::isfinite(value) && value >= -1e-12;
    }

    static bool valid_channel_number(double value) {
        return std::isfinite(value);
    }
};

class SafetyEnvelope {
public:
    [[nodiscard]] TransportDecisionTrace constrain(
        const transport::TransportContract& contract,
        const transport::GenerationDescriptor& descriptor,
        const TransportState& state,
        TransportDecision proposed) const {
        TransportDecisionTrace trace;
        trace.generation_id = descriptor.generation_id;
        trace.observed = state;
        trace.proposed = proposed;
        trace.decision = proposed;

        if (!proposed.permitted) {
            reject(trace, "proposal was not permitted by its producer");
            return trace;
        }

        if (state.now_ms > descriptor.expires_at_ms) {
            reject(trace, "generation expired");
            return trace;
        }
        if (!std::isfinite(state.source_energy_reserve) ||
            state.source_energy_reserve < 0.0 || state.source_energy_reserve > 1.0) {
            reject(trace, "invalid source energy observation");
            return trace;
        }
        if (!std::isfinite(state.rf_duty_remaining) ||
            state.rf_duty_remaining < 0.0 || state.rf_duty_remaining > 1.0) {
            reject(trace, "invalid RF duty observation");
            return trace;
        }
        if (!valid_nonnegative(state.source_energy_capacity_j) ||
            !valid_nonnegative(state.rf_energy_cost_per_attempt_j) ||
            !valid_nonnegative(state.optical_energy_cost_per_attempt_j) ||
            !valid_nonnegative(state.backscatter_energy_cost_per_attempt_j) ||
            !valid_nonnegative(state.rf_duty_remaining_s) ||
            !valid_nonnegative(state.rf_airtime_per_attempt_s)) {
            reject(trace, "invalid deterministic action-cost observation");
            return trace;
        }
        if (state.now_ms < state.observed_at_ms ||
            state.now_ms - state.observed_at_ms > contract.maximum_observation_age_ms) {
            reject(trace, "observation too stale");
            return trace;
        }
        if (state.source_energy_reserve < contract.minimum_source_reserve) {
            reject(trace, "source energy reserve below contract floor");
            return trace;
        }

        if (!state.segments.empty()) {
            if (state.segments.size() != descriptor.segments.size()) {
                reject(trace, "segment runtime state does not match the descriptor");
                return trace;
            }
            for (std::size_t index = 0; index < state.segments.size(); ++index) {
                if (state.segments[index].segment_id != index) {
                    reject(trace, "segment runtime state is not contiguous and ordered");
                    return trace;
                }
            }
        }

        trace.decision.transmission_attempts = std::max(
            1U, trace.decision.transmission_attempts);

        if (!link_can_attempt(contract, state, trace.decision.link)) {
            const auto replacement = first_usable_link(contract, state);
            if (!replacement.has_value()) {
                reject(trace, "no permitted link satisfies the safety envelope");
                return trace;
            }
            trace.decision.link = *replacement;
            trace.constraints_applied.push_back("proposed link replaced by an allowed link");
        }

        const auto bounded_total = std::min<long double>(
            std::ceil(static_cast<long double>(descriptor.total_source_symbols) *
                      static_cast<long double>(contract.maximum_repair_amplification)),
            std::numeric_limits<std::uint32_t>::max());
        const auto maximum_total = static_cast<std::uint32_t>(bounded_total);
        if (state.emitted_symbols > maximum_total) {
            reject(trace, "emitted symbols already exceed repair amplification");
            return trace;
        }
        auto remaining_emission = static_cast<std::uint64_t>(maximum_total) -
                                  state.emitted_symbols;

        std::uint32_t critical_sources = 0;
        std::uint64_t active_capacity = 0;
        std::uint64_t active_critical_capacity = 0;
        std::uint64_t active_critical_emitted = 0;
        std::uint64_t active_critical_floor = 0;
        std::size_t active_segments = 0;
        for (std::size_t index = 0; index < descriptor.segments.size(); ++index) {
            const auto& segment = descriptor.segments[index];
            const auto* runtime = state.segments.empty() ? nullptr : &state.segments[index];
            const bool complete = runtime && runtime->complete;
            const bool expired = (runtime && runtime->expired) ||
                                 state.now_ms > segment.expires_at_ms;
            if (complete || expired) continue;
            ++active_segments;
            const auto maximum = maximum_segment_emission(segment, contract);
            const auto emitted = runtime
                ? runtime->emitted_symbols
                : static_cast<std::uint64_t>(segment.coding.emitted_symbols);
            if (emitted > maximum) {
                reject(trace, "segment emissions already exceed repair amplification");
                return trace;
            }
            active_capacity += maximum - emitted;
            if (segment.importance == transport::TransportImportance::CRITICAL) {
                critical_sources += segment.source_symbol_count;
                active_critical_capacity += maximum - emitted;
                active_critical_emitted += emitted;
                active_critical_floor += static_cast<std::uint64_t>(
                    std::min<long double>(
                        std::ceil(static_cast<long double>(segment.source_symbol_count) *
                                  static_cast<long double>(contract.minimum_critical_overhead)),
                        std::numeric_limits<std::uint32_t>::max()));
            }
        }
        if (state.segments.empty()) {
            active_capacity = remaining_emission;
            const auto active_critical_maximum = static_cast<std::uint64_t>(
                std::min<long double>(
                    std::ceil(static_cast<long double>(critical_sources) *
                              static_cast<long double>(contract.maximum_repair_amplification)),
                    std::numeric_limits<std::uint32_t>::max()));
            if (state.critical_emitted_symbols > active_critical_maximum) {
                reject(trace, "critical symbols already exceed repair amplification");
                return trace;
            }
            active_critical_capacity =
                active_critical_maximum - state.critical_emitted_symbols;
            active_critical_emitted = state.critical_emitted_symbols;
        }
        if (active_segments == 0) {
            reject(trace, "no unexpired incomplete segment remains");
            return trace;
        }
        remaining_emission = std::min(remaining_emission, active_capacity);
        if (trace.decision.critical_only && critical_sources == 0) {
            trace.decision.critical_only = false;
            trace.constraints_applied.push_back(
                "critical-only cleared because generation has no critical segment");
        }
        if (trace.decision.critical_only) {
            remaining_emission = std::min(remaining_emission, active_critical_capacity);
        }
        const auto remaining_repairs = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                remaining_emission, std::numeric_limits<std::uint32_t>::max()));
        if (trace.decision.repair_symbols > remaining_repairs) {
            trace.decision.repair_symbols = remaining_repairs;
            trace.constraints_applied.push_back("repair amplification capped by contract");
        }

        if (critical_sources > 0 && trace.decision.critical_only) {
            const auto minimum_repairs = active_critical_floor > active_critical_emitted
                ? active_critical_floor - active_critical_emitted
                : 0U;
            if (trace.decision.repair_symbols < minimum_repairs) {
                trace.decision.repair_symbols = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    minimum_repairs, remaining_repairs));
                trace.constraints_applied.push_back("critical protection raised to contract floor");
            }
        }

        const auto affordable_attempts = maximum_affordable_attempts(
            contract, state, trace.decision.link);
        if (affordable_attempts == 0) {
            reject(trace, "proposed action would cross an energy or duty floor");
            return trace;
        }
        if (trace.decision.transmission_attempts > affordable_attempts) {
            trace.decision.transmission_attempts = affordable_attempts;
            trace.constraints_applied.push_back(
                "transmission attempts capped by deterministic energy/duty cost");
        }
        return trace;
    }

private:
    static bool link_allowed(const transport::TransportContract& contract, LinkMode link) {
        switch (link) {
            case LinkMode::RF: return contract.allow_rf;
            case LinkMode::OPTICAL: return contract.allow_optical;
            case LinkMode::BACKSCATTER: return contract.allow_backscatter;
        }
        return false;
    }

    static bool valid_nonnegative(double value) {
        return std::isfinite(value) && value >= 0.0;
    }

    static std::uint64_t maximum_segment_emission(
        const transport::GenerationSegmentDescriptor& segment,
        const transport::TransportContract& contract) {
        return static_cast<std::uint64_t>(std::min<long double>(
            std::ceil(static_cast<long double>(segment.source_symbol_count) *
                      static_cast<long double>(contract.maximum_repair_amplification)),
            std::numeric_limits<std::uint32_t>::max()));
    }

    static double energy_cost(const TransportState& state, LinkMode link) {
        switch (link) {
            case LinkMode::RF: return state.rf_energy_cost_per_attempt_j;
            case LinkMode::OPTICAL: return state.optical_energy_cost_per_attempt_j;
            case LinkMode::BACKSCATTER: return state.backscatter_energy_cost_per_attempt_j;
        }
        return 0.0;
    }

    static std::uint32_t maximum_affordable_attempts(
        const transport::TransportContract& contract,
        const TransportState& state,
        LinkMode link) {
        std::uint64_t maximum = std::numeric_limits<std::uint32_t>::max();
        const auto cost = energy_cost(state, link);
        if (state.source_energy_capacity_j > 0.0 && cost > 0.0) {
            const auto headroom_j = std::max(
                0.0,
                (state.source_energy_reserve - contract.minimum_source_reserve) *
                    state.source_energy_capacity_j);
            maximum = std::min<std::uint64_t>(
                maximum,
                static_cast<std::uint64_t>(std::floor((headroom_j + 1e-12) / cost)));
        }
        if (link == LinkMode::RF && state.rf_airtime_per_attempt_s > 0.0) {
            maximum = std::min<std::uint64_t>(
                maximum,
                static_cast<std::uint64_t>(std::floor(
                    (state.rf_duty_remaining_s + 1e-12) /
                    state.rf_airtime_per_attempt_s)));
        }
        return static_cast<std::uint32_t>(maximum);
    }

    static bool link_can_attempt(const transport::TransportContract& contract,
                                 const TransportState& state,
                                 LinkMode link) {
        if (!link_allowed(contract, link)) return false;
        if (link == LinkMode::RF && state.rf_duty_remaining <= 0.0) return false;
        return maximum_affordable_attempts(contract, state, link) > 0;
    }

    static std::optional<LinkMode> first_usable_link(
        const transport::TransportContract& contract,
        const TransportState& state) {
        if (link_can_attempt(contract, state, LinkMode::OPTICAL)) {
            return LinkMode::OPTICAL;
        }
        if (link_can_attempt(contract, state, LinkMode::BACKSCATTER)) {
            return LinkMode::BACKSCATTER;
        }
        if (link_can_attempt(contract, state, LinkMode::RF)) {
            return LinkMode::RF;
        }
        return std::nullopt;
    }

    static void reject(TransportDecisionTrace& trace, const std::string& reason) {
        trace.decision.permitted = false;
        trace.decision.transmission_attempts = 0;
        trace.decision.repair_symbols = 0;
        trace.constraints_applied.push_back(reason);
    }
};

} // namespace aurora::safety

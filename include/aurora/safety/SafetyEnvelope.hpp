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
};

struct TransportDecision {
    LinkMode link = LinkMode::RF;
    std::uint32_t transmission_attempts = 1;
    std::uint32_t repair_symbols = 0;
    bool critical_only = false;
    bool permitted = true;
};

struct TransportExecution {
    bool recorded = false;
    LinkMode link = LinkMode::RF;
    std::uint32_t transmission_attempts = 0;
    std::uint32_t hal_accepted_attempts = 0;
    std::uint32_t delivered_attempts = 0;
    std::uint32_t repair_symbols_emitted = 0;
    bool critical_only = false;
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
        return std::nullopt;
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
        for (const auto& segment : descriptor.segments) {
            if (segment.importance == transport::TransportImportance::CRITICAL) {
                critical_sources += segment.source_symbol_count;
            }
        }
        if (trace.decision.critical_only && critical_sources == 0) {
            trace.decision.critical_only = false;
            trace.constraints_applied.push_back(
                "critical-only cleared because generation has no critical segment");
        }
        if (trace.decision.critical_only) {
            const auto critical_maximum = static_cast<std::uint64_t>(
                std::min<long double>(
                    std::ceil(static_cast<long double>(critical_sources) *
                              static_cast<long double>(contract.maximum_repair_amplification)),
                    std::numeric_limits<std::uint32_t>::max()));
            if (state.critical_emitted_symbols > critical_maximum) {
                reject(trace, "critical symbols already exceed repair amplification");
                return trace;
            }
            remaining_emission = std::min(
                remaining_emission,
                critical_maximum - state.critical_emitted_symbols);
        }
        const auto remaining_repairs = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                remaining_emission, std::numeric_limits<std::uint32_t>::max()));
        if (trace.decision.repair_symbols > remaining_repairs) {
            trace.decision.repair_symbols = remaining_repairs;
            trace.constraints_applied.push_back("repair amplification capped by contract");
        }

        if (critical_sources > 0 && trace.decision.critical_only) {
            const auto bounded_protected_total = std::min<long double>(
                std::ceil(static_cast<long double>(critical_sources) *
                          static_cast<long double>(contract.minimum_critical_overhead)),
                std::numeric_limits<std::uint32_t>::max());
            const auto protected_total = static_cast<std::uint32_t>(
                bounded_protected_total);
            const auto minimum_repairs = protected_total > state.critical_emitted_symbols
                ? protected_total - state.critical_emitted_symbols
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

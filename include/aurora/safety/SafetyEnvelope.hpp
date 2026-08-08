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

struct TransportDecisionTrace {
    std::string generation_id;
    TransportState observed;
    TransportDecision proposed;
    TransportDecision decision;
    std::vector<std::string> constraints_applied;
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
        if (state.now_ms < state.observed_at_ms ||
            state.now_ms - state.observed_at_ms > contract.maximum_observation_age_ms) {
            reject(trace, "observation too stale");
            return trace;
        }
        if (state.source_energy_reserve < contract.minimum_source_reserve) {
            reject(trace, "source energy reserve below contract floor");
            return trace;
        }

        if (!link_allowed(contract, trace.decision.link) ||
            (trace.decision.link == LinkMode::RF && state.rf_duty_remaining <= 0.0)) {
            const auto replacement = first_allowed_link(contract, state);
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
        const auto maximum_repairs = maximum_total > descriptor.total_source_symbols
            ? maximum_total - descriptor.total_source_symbols
            : 0U;
        if (trace.decision.repair_symbols > maximum_repairs) {
            trace.decision.repair_symbols = maximum_repairs;
            trace.constraints_applied.push_back("repair amplification capped by contract");
        }

        std::uint32_t critical_sources = 0;
        for (const auto& segment : descriptor.segments) {
            if (segment.importance == transport::TransportImportance::CRITICAL) {
                critical_sources += segment.source_symbol_count;
            }
        }
        if (critical_sources > 0 && trace.decision.critical_only) {
            const auto bounded_protected_total = std::min<long double>(
                std::ceil(static_cast<long double>(critical_sources) *
                          static_cast<long double>(contract.minimum_critical_overhead)),
                std::numeric_limits<std::uint32_t>::max());
            const auto protected_total = static_cast<std::uint32_t>(
                bounded_protected_total);
            const auto minimum_repairs = protected_total > critical_sources
                ? protected_total - critical_sources
                : 0U;
            if (trace.decision.repair_symbols < minimum_repairs) {
                trace.decision.repair_symbols = std::min(minimum_repairs, maximum_repairs);
                trace.constraints_applied.push_back("critical protection raised to contract floor");
            }
        }
        trace.decision.transmission_attempts = std::max(1U, trace.decision.transmission_attempts);
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

    static std::optional<LinkMode> first_allowed_link(
        const transport::TransportContract& contract,
        const TransportState& state) {
        if (contract.allow_optical) {
            return LinkMode::OPTICAL;
        }
        if (contract.allow_backscatter) {
            return LinkMode::BACKSCATTER;
        }
        if (contract.allow_rf && state.rf_duty_remaining > 0.0) {
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

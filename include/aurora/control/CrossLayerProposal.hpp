#pragma once

#include "OperatingModeController.hpp"
#include "../safety/SafetyEnvelope.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace aurora::control {

enum class ProposalPriority : std::uint8_t {
    CRITICAL = 0,
    NORMAL = 1,
    BULK = 2
};

inline bool valid_proposal_link(safety::LinkMode link) {
    switch (link) {
        case safety::LinkMode::RF:
        case safety::LinkMode::OPTICAL:
        case safety::LinkMode::BACKSCATTER:
            return true;
    }
    return false;
}

inline bool valid_proposal_priority(ProposalPriority priority) {
    switch (priority) {
        case ProposalPriority::CRITICAL:
        case ProposalPriority::NORMAL:
        case ProposalPriority::BULK:
            return true;
    }
    return false;
}

struct ProposalStateSnapshot {
    std::array<std::uint64_t, 3> ucb_counts{0, 0, 0};
    std::array<double, 3> ucb_reward_sums{0.0, 0.0, 0.0};
    safety::LinkMode selector_memory = safety::LinkMode::RF;
    double hysteresis_db = 1.0;
    std::uint64_t random_state = 0xC0FFEEBEEFULL;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (!valid_proposal_link(selector_memory) ||
            !std::isfinite(hysteresis_db) || hysteresis_db < 0.0 ||
            random_state == 0) {
            return "invalid proposal scalar state";
        }
        for (std::size_t index = 0; index < ucb_counts.size(); ++index) {
            const auto reward = ucb_reward_sums[index];
            if (!std::isfinite(reward) || reward < 0.0 ||
                reward > static_cast<double>(ucb_counts[index])) {
                return "invalid UCB reward state";
            }
        }
        return std::nullopt;
    }

    bool operator==(const ProposalStateSnapshot&) const = default;
};

struct ProposalInput {
    bool selector_argmax = true;
    bool allow_backscatter = true;
    double deadline_s = 0.0;
    double source_soc = 1.0;
    double rf_duty_remaining = 1.0;
    int symbols_have = 0;
    int symbols_need = 1;
    double deadline_left_s = 0.0;
    std::array<double, 3> snr_db{-3.0, -3.0, -3.0};
    std::array<double, 3> historical_per{0.4, 0.4, 0.4};
    double jamming_score = 0.0;
    ProposalPriority priority = ProposalPriority::NORMAL;
    bool emergency_mode = false;
    std::uint16_t covert_sequence = 0;
    OperatingMode operating_mode = OperatingMode::NORMAL;
    double epoch = 1.0;
    bool has_critical_segments = false;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (!std::isfinite(deadline_s) || deadline_s < 0.0 ||
            !safety::valid_fraction(source_soc) ||
            !safety::valid_fraction(rf_duty_remaining) ||
            symbols_have < 0 || symbols_need <= 0 || symbols_have > symbols_need ||
            !std::isfinite(deadline_left_s) || deadline_left_s < 0.0 ||
            deadline_left_s > deadline_s ||
            !safety::valid_fraction(jamming_score) ||
            !valid_proposal_priority(priority) ||
            !valid_operating_mode(operating_mode) ||
            !std::isfinite(epoch) || epoch < 1.0) {
            return "invalid proposal input";
        }
        for (std::size_t index = 0; index < snr_db.size(); ++index) {
            if (!std::isfinite(snr_db[index]) ||
                !safety::valid_fraction(historical_per[index])) {
                return "invalid channel proposal input";
            }
        }
        return std::nullopt;
    }

    bool operator==(const ProposalInput&) const = default;
};

struct ProposalDecision {
    safety::TransportDecision transport;
    int jitter_ms = 0;
    int minimum_spacing_ms = 0;
    int preamble_symbols = 0;
    int rf_bandwidth_khz = 0;
    std::uint8_t covert_sequence = 0;
    bool emergency = false;

    bool operator==(const ProposalDecision& other) const {
        return transport.link == other.transport.link &&
            transport.transmission_attempts == other.transport.transmission_attempts &&
            transport.repair_symbols == other.transport.repair_symbols &&
            transport.critical_only == other.transport.critical_only &&
            transport.permitted == other.transport.permitted &&
            jitter_ms == other.jitter_ms &&
            minimum_spacing_ms == other.minimum_spacing_ms &&
            preamble_symbols == other.preamble_symbols &&
            rf_bandwidth_khz == other.rf_bandwidth_khz &&
            covert_sequence == other.covert_sequence &&
            emergency == other.emergency;
    }
};

struct ProposalFeedback {
    bool applied = false;
    safety::LinkMode executed_link = safety::LinkMode::RF;
    double reward = 0.0;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (!valid_proposal_link(executed_link) ||
            !safety::valid_fraction(reward)) {
            return "invalid proposal feedback";
        }
        if (!applied && reward != 0.0) {
            return "unused proposal feedback has a reward";
        }
        return std::nullopt;
    }

    bool operator==(const ProposalFeedback&) const = default;
};

inline std::size_t proposal_link_index(safety::LinkMode link) {
    switch (link) {
        case safety::LinkMode::RF: return 0;
        case safety::LinkMode::OPTICAL: return 1;
        case safety::LinkMode::BACKSCATTER: return 2;
    }
    throw std::invalid_argument("proposal: invalid link");
}

inline double proposal_random_unit(std::uint64_t& state) {
    state ^= state << 7;
    state ^= state >> 9;
    state ^= state << 8;
    return static_cast<double>(state >> 11) *
        (1.0 / static_cast<double>((1ULL << 53) - 1ULL));
}

inline double proposal_target_reliability(ProposalPriority priority) {
    switch (priority) {
        case ProposalPriority::CRITICAL: return 0.999;
        case ProposalPriority::NORMAL: return 0.97;
        case ProposalPriority::BULK: return 0.9;
    }
    throw std::invalid_argument("proposal: invalid priority");
}

inline double proposal_urgency(const ProposalInput& input) {
    const auto need = std::max(input.symbols_need, 1);
    const double time_index = 1.0 - std::clamp(
        input.deadline_left_s / std::max(1.0, input.deadline_s), 0.0, 1.0);
    const double time_pressure = 1.0 - std::exp(-6.0 * time_index);
    const double fraction = static_cast<double>(need - input.symbols_have) /
        static_cast<double>(need);
    const double symbol_pressure = 1.0 /
        (1.0 + std::exp(-10.0 * (fraction - 0.5)));
    return std::max(time_pressure, symbol_pressure);
}

inline double proposal_duty_budget(double remaining, double urgency) {
    const double spend = std::min(
        0.6 * remaining, 0.1 + 0.7 * urgency * remaining);
    return std::clamp(spend, 0.02, std::max(0.02, remaining));
}

inline double proposal_per_from_snr(double snr_db, safety::LinkMode link) {
    const auto logistic = [](double value, double midpoint, double slope) {
        return 1.0 / (1.0 + std::exp(slope * (value - midpoint)));
    };
    switch (link) {
        case safety::LinkMode::RF: return logistic(snr_db, -7.5, 0.9);
        case safety::LinkMode::OPTICAL: return logistic(snr_db, 4.0, 1.1);
        case safety::LinkMode::BACKSCATTER: return logistic(snr_db, 1.5, 1.0);
    }
    throw std::invalid_argument("proposal: invalid link");
}

inline safety::LinkMode proposal_link_from_index(std::size_t index) {
    switch (index) {
        case 0: return safety::LinkMode::RF;
        case 1: return safety::LinkMode::OPTICAL;
        case 2: return safety::LinkMode::BACKSCATTER;
        default: throw std::invalid_argument("proposal: invalid link index");
    }
}

inline ProposalDecision derive_proposal(
    const ProposalInput& input,
    ProposalStateSnapshot& state) {
    if (const auto error = input.validation_error()) {
        throw std::invalid_argument("proposal: " + *error);
    }
    if (const auto error = state.validation_error()) {
        throw std::invalid_argument("proposal: " + *error);
    }

    safety::LinkMode selected = safety::LinkMode::RF;
    if (input.selector_argmax) {
        std::size_t best = 0;
        for (std::size_t index = 1; index < input.snr_db.size(); ++index) {
            if (input.snr_db[index] > input.snr_db[best]) best = index;
        }
        const auto previous = proposal_link_index(state.selector_memory);
        const double margin = input.snr_db[best] - input.snr_db[previous];
        selected = margin > state.hysteresis_db
            ? proposal_link_from_index(best)
            : state.selector_memory;
    } else {
        constexpr double exploration = 1.2;
        double best_score = -1e9;
        std::size_t best = 0;
        for (std::size_t index = 0; index < state.ucb_counts.size(); ++index) {
            const auto count = state.ucb_counts[index];
            const double average = count > 0
                ? state.ucb_reward_sums[index] / static_cast<double>(count)
                : 0.7;
            const double confidence = count > 0
                ? exploration * std::sqrt(
                    std::log(std::max(1.0, input.epoch)) /
                    static_cast<double>(count))
                : 1.0;
            const double score = average + confidence;
            if (score > best_score) {
                best_score = score;
                best = index;
            }
        }
        selected = proposal_link_from_index(best);
    }
    if (input.source_soc < 0.18 && input.allow_backscatter) {
        selected = safety::LinkMode::BACKSCATTER;
    }
    state.selector_memory = selected;

    double reliability = proposal_target_reliability(input.priority);
    if (input.emergency_mode) reliability = std::max(reliability, 0.999);
    if (input.operating_mode == OperatingMode::CONSERVATIVE &&
        input.priority != ProposalPriority::BULK) {
        reliability = std::max(reliability, 0.995);
    } else if (input.operating_mode == OperatingMode::AGGRESSIVE &&
               input.priority == ProposalPriority::BULK) {
        reliability = std::max(0.85, reliability - 0.05);
    }

    const double urgency = proposal_urgency(input);
    const double budget = proposal_duty_budget(input.rf_duty_remaining, urgency);
    const int base_cap = budget > 0.5 ? 48 : (budget > 0.25 ? 32 : 20);
    int cap = base_cap;
    if (input.operating_mode == OperatingMode::CONSERVATIVE) {
        cap = std::max(12, base_cap - 8);
    } else if (input.operating_mode == OperatingMode::AGGRESSIVE) {
        cap = std::min(64, base_cap + 8);
    }

    const auto selected_index = proposal_link_index(selected);
    const double weight = std::clamp(
        0.5 + 0.4 * input.jamming_score, 0.1, 0.9);
    const double packet_error = std::clamp(
        weight * input.historical_per[selected_index] +
            (1.0 - weight) * proposal_per_from_snr(
                input.snr_db[selected_index], selected),
        0.01, 0.99);
    const double success = std::clamp(1.0 - packet_error, 1e-3, 0.999);
    const int attempts = std::clamp(
        static_cast<int>(std::ceil(
            std::log(1.0 - reliability) / std::log(1.0 - success))),
        1, cap);

    int redundancy = static_cast<int>(std::ceil(
        std::log(1.0 - reliability) / std::log(packet_error) * 0.6));
    redundancy = std::max(5, redundancy);
    if (input.operating_mode == OperatingMode::CONSERVATIVE &&
        input.priority != ProposalPriority::BULK) {
        redundancy = static_cast<int>(static_cast<double>(redundancy) * 1.2);
    } else if (input.operating_mode == OperatingMode::AGGRESSIVE &&
               input.priority == ProposalPriority::BULK) {
        redundancy = std::max(
            3, static_cast<int>(static_cast<double>(redundancy) * 0.9));
    }

    ProposalDecision decision;
    decision.minimum_spacing_ms = input.source_soc < 0.3 ? 18 : 8;
    decision.jitter_ms = static_cast<int>(std::round(
        (1.0 - std::clamp(input.rf_duty_remaining, 0.0, 1.0)) * 40.0)) +
        (input.source_soc < 0.3 ? 12 : 0);
    decision.preamble_symbols = std::clamp(
        8 + static_cast<int>(urgency * 10.0) +
            static_cast<int>(proposal_random_unit(state.random_state) * 4.0),
        6, 24);
    decision.rf_bandwidth_khz =
        proposal_random_unit(state.random_state) < 0.5 ? 125 : 250;

    auto final_link = selected;
    if (input.operating_mode == OperatingMode::CONSERVATIVE &&
        input.priority != ProposalPriority::BULK &&
        input.snr_db[0] > input.snr_db[1] - 2.0) {
        final_link = safety::LinkMode::RF;
    }

    decision.transport.link = final_link;
    decision.transport.transmission_attempts =
        static_cast<std::uint32_t>(attempts);
    decision.transport.repair_symbols = static_cast<std::uint32_t>(redundancy);
    decision.transport.critical_only =
        input.priority == ProposalPriority::CRITICAL &&
        input.has_critical_segments;
    if (decision.transport.critical_only) {
        decision.transport.transmission_attempts = std::max(
            2U, decision.transport.transmission_attempts);
    }
    decision.transport.permitted = true;
    decision.covert_sequence = static_cast<std::uint8_t>(
        input.covert_sequence & 0xFFU);
    decision.emergency = input.emergency_mode;
    return decision;
}

inline void apply_proposal_feedback(
    ProposalStateSnapshot& state,
    const ProposalFeedback& feedback) {
    if (const auto error = state.validation_error()) {
        throw std::invalid_argument("proposal feedback: " + *error);
    }
    if (const auto error = feedback.validation_error()) {
        throw std::invalid_argument("proposal feedback: " + *error);
    }
    if (!feedback.applied) return;
    const auto index = proposal_link_index(feedback.executed_link);
    ++state.ucb_counts[index];
    state.ucb_reward_sums[index] += feedback.reward;
}

struct ProposalTransition {
    bool recorded = false;
    ProposalStateSnapshot before;
    ProposalInput input;
    ProposalDecision decision;
    ProposalStateSnapshot after_proposal;
    ProposalFeedback feedback;
    ProposalStateSnapshot after;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (!recorded) return "proposal transition is not recorded";
        if (const auto error = before.validation_error()) {
            return "invalid before state: " + *error;
        }
        if (const auto error = input.validation_error()) {
            return "invalid input: " + *error;
        }
        if (const auto error = after_proposal.validation_error()) {
            return "invalid post-proposal state: " + *error;
        }
        if (const auto error = feedback.validation_error()) {
            return "invalid feedback: " + *error;
        }
        if (const auto error = after.validation_error()) {
            return "invalid after state: " + *error;
        }

        auto replayed_state = before;
        ProposalDecision replayed_decision;
        try {
            replayed_decision = derive_proposal(input, replayed_state);
        } catch (const std::exception& error) {
            return std::string("proposal replay rejected input: ") + error.what();
        }
        if (!(replayed_decision == decision) ||
            !(replayed_state == after_proposal)) {
            return "proposal derivation mismatch";
        }
        try {
            apply_proposal_feedback(replayed_state, feedback);
        } catch (const std::exception& error) {
            return std::string("proposal feedback replay rejected input: ") +
                error.what();
        }
        if (!(replayed_state == after)) {
            return "proposal feedback transition mismatch";
        }
        return std::nullopt;
    }

    bool operator==(const ProposalTransition&) const = default;
};

} // namespace aurora::control

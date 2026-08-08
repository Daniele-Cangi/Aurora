#pragma once

#include "../transport/Generation.hpp"
#include "../transport/TransportContract.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace aurora::control {

enum class FlowClass : std::uint8_t {
    NERVE,
    MUSCLE,
    GLAND
};

enum class Genotype : std::uint8_t {
    BASELINE,
    HYPERVIGILANT,
    STOIC,
    EXPERIMENTAL
};

enum class GenotypeHint : std::uint8_t {
    AUTO,
    FORCE_BASELINE,
    FORCE_HYPERVIGILANT,
    FORCE_STOIC,
    FORCE_EXPERIMENTAL
};

struct FlowProfile {
    double deadline_s = 600.0;
    double reliability = 0.99;
    double duty_limit = 0.01;
    std::string priority = "IMPORTANT";
    FlowClass flow_class = FlowClass::MUSCLE;
    GenotypeHint genotype_hint = GenotypeHint::AUTO;
};

struct ProtectionPlan {
    std::string policy_id;
    std::uint16_t policy_version = 1;
    FlowProfile profile;
    double critical_overhead = 1.0;
    double important_overhead = 1.0;
    double elastic_overhead = 1.0;

    [[nodiscard]] double overhead_for(transport::TransportImportance importance) const {
        switch (importance) {
            case transport::TransportImportance::CRITICAL: return critical_overhead;
            case transport::TransportImportance::IMPORTANT: return important_overhead;
            case transport::TransportImportance::ELASTIC: return elastic_overhead;
        }
        return 1.0;
    }
};

class TransportPolicy {
public:
    virtual ~TransportPolicy() = default;

    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::uint16_t version() const = 0;
    [[nodiscard]] virtual FlowProfile profile_for(
        const transport::TransportContract& contract) const = 0;
    virtual ProtectionPlan plan(const transport::TransportContract& contract) = 0;
    virtual void observe(const FlowProfile& profile, const transport::DecodeReport& report) = 0;
};

inline FlowProfile transport_profile_for(const transport::TransportContract& contract) {
    FlowProfile profile;
    profile.deadline_s = contract.deadline_s;
    profile.reliability = contract.reliability;
    profile.duty_limit = contract.duty_frac;
    switch (contract.importance) {
        case transport::TransportImportance::CRITICAL: profile.priority = "CRITICAL"; break;
        case transport::TransportImportance::IMPORTANT: profile.priority = "IMPORTANT"; break;
        case transport::TransportImportance::ELASTIC: profile.priority = "ELASTIC"; break;
    }

    // These names select transport policy profiles only. Payload bytes are never inspected.
    if (profile.deadline_s < 2.0 && profile.reliability >= 0.90) {
        profile.flow_class = FlowClass::NERVE;
    } else if (profile.reliability > 0.95) {
        profile.flow_class = FlowClass::GLAND;
    } else {
        profile.flow_class = FlowClass::MUSCLE;
    }
    return profile;
}

class FixedTransportPolicy final : public TransportPolicy {
public:
    FixedTransportPolicy(std::string policy_id,
                         double critical_overhead,
                         double important_overhead,
                         double elastic_overhead)
        : policy_id_(std::move(policy_id)),
          critical_overhead_(validate_overhead(critical_overhead)),
          important_overhead_(validate_overhead(important_overhead)),
          elastic_overhead_(validate_overhead(elastic_overhead)) {
        if (policy_id_.empty()) {
            throw std::invalid_argument("fixed transport policy: id is required");
        }
    }

    [[nodiscard]] std::string id() const override { return policy_id_; }
    [[nodiscard]] std::uint16_t version() const override { return 1; }

    [[nodiscard]] FlowProfile profile_for(
        const transport::TransportContract& contract) const override {
        return transport_profile_for(contract);
    }

    ProtectionPlan plan(const transport::TransportContract& contract) override {
        ProtectionPlan result;
        result.policy_id = id();
        result.policy_version = version();
        result.profile = profile_for(contract);
        result.critical_overhead = clamp(contract, critical_overhead_, true);
        result.important_overhead = clamp(contract, important_overhead_, false);
        result.elastic_overhead = clamp(contract, elastic_overhead_, false);
        return result;
    }

    void observe(const FlowProfile&, const transport::DecodeReport&) override {}

private:
    static double validate_overhead(double value) {
        if (value < 1.0 || !std::isfinite(value)) {
            throw std::invalid_argument("fixed transport policy: overhead must be finite and >= 1");
        }
        return value;
    }

    static double clamp(const transport::TransportContract& contract,
                        double value,
                        bool critical) {
        if (critical) {
            value = std::max(value, contract.minimum_critical_overhead);
        }
        return std::clamp(value, 1.0, contract.maximum_repair_amplification);
    }

    std::string policy_id_;
    double critical_overhead_ = 1.0;
    double important_overhead_ = 1.0;
    double elastic_overhead_ = 1.0;
};

struct AdaptivePolicyConfig {
    double alpha_up = 0.10;
    double alpha_down = 0.02;
    int panic_boost_generations = 3;
};

struct BiologicalFlowState {
    double critical_overhead = 1.0;
    double important_overhead = 1.0;
    double base_critical_overhead = 1.0;
    double base_important_overhead = 1.0;
    double average_coverage = 0.0;
    int success_count = 0;
    int failure_count = 0;
    int panic_boost = 0;
    int good_streak = 0;
    int bad_streak = 0;
    Genotype genotype = Genotype::BASELINE;
    bool initialized = false;
    int generation_count = 0;
};

class BiologicalAdaptivePolicy final : public TransportPolicy {
public:
    explicit BiologicalAdaptivePolicy(AdaptivePolicyConfig config = {}) : config_(config) {
        if (config_.alpha_up < 0.0 || config_.alpha_down < 0.0 ||
            config_.panic_boost_generations < 0) {
            throw std::invalid_argument("biological transport policy: invalid adaptation configuration");
        }
    }

    [[nodiscard]] std::string id() const override { return "biological-adaptive"; }
    [[nodiscard]] std::uint16_t version() const override { return 1; }

    [[nodiscard]] FlowProfile profile_for(
        const transport::TransportContract& contract) const override {
        return transport_profile_for(contract);
    }

    ProtectionPlan plan(const transport::TransportContract& contract) override {
        const auto profile = profile_for(contract);
        auto& state = state_for(profile);
        ++state.generation_count;

        double critical = state.critical_overhead;
        double important = state.important_overhead;
        if (state.panic_boost > 0) {
            critical *= 2.0;
            important *= 1.5;
            // Panic is a generation budget: consume one unit when a new
            // generation is planned, regardless of that generation's outcome.
            --state.panic_boost;
        }

        ProtectionPlan result;
        result.policy_id = id();
        result.policy_version = version();
        result.profile = profile;
        result.critical_overhead = std::clamp(
            std::max(critical, contract.minimum_critical_overhead),
            1.0, contract.maximum_repair_amplification);
        result.important_overhead = std::clamp(
            important, 1.0, contract.maximum_repair_amplification);
        result.elastic_overhead = std::clamp(
            std::min(important, 1.25), 1.0, contract.maximum_repair_amplification);
        return result;
    }

    void observe(const FlowProfile& profile, const transport::DecodeReport& report) override {
        if (!report.delivered() && !report.terminal_failure()) {
            return;
        }
        auto& state = state_for(profile);
        constexpr double coverage_alpha = 0.2;
        state.average_coverage = state.success_count + state.failure_count == 0
            ? report.coverage
            : (1.0 - coverage_alpha) * state.average_coverage + coverage_alpha * report.coverage;

        if (report.delivered()) {
            ++state.success_count;
            ++state.good_streak;
            state.bad_streak = 0;
            if (state.good_streak >= 4 && state.panic_boost == 0 &&
                state.average_coverage >= 0.85) {
                state.critical_overhead = std::max(
                    state.base_critical_overhead,
                    state.critical_overhead - config_.alpha_down);
                state.important_overhead = std::max(
                    state.base_important_overhead,
                    state.important_overhead - config_.alpha_down);
            }
        } else {
            ++state.failure_count;
            ++state.bad_streak;
            state.good_streak = 0;
            state.critical_overhead = std::min(
                maximum_overhead(state.genotype), state.critical_overhead + config_.alpha_up);
            state.important_overhead = std::min(
                maximum_overhead(state.genotype), state.important_overhead + config_.alpha_up * 0.5);
            if (profile.flow_class != FlowClass::MUSCLE) {
                state.panic_boost = std::max(
                    state.panic_boost, config_.panic_boost_generations);
            }
        }
    }

    [[nodiscard]] std::optional<BiologicalFlowState> flow_state(
        const FlowProfile& profile) const {
        const auto found = states_.find(flow_key(profile));
        if (found == states_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

private:
    static Genotype initial_genotype(const FlowProfile& profile) {
        switch (profile.genotype_hint) {
            case GenotypeHint::FORCE_BASELINE: return Genotype::BASELINE;
            case GenotypeHint::FORCE_HYPERVIGILANT: return Genotype::HYPERVIGILANT;
            case GenotypeHint::FORCE_STOIC: return Genotype::STOIC;
            case GenotypeHint::FORCE_EXPERIMENTAL: return Genotype::EXPERIMENTAL;
            case GenotypeHint::AUTO: break;
        }
        switch (profile.flow_class) {
            case FlowClass::NERVE: return Genotype::HYPERVIGILANT;
            case FlowClass::GLAND: return Genotype::BASELINE;
            case FlowClass::MUSCLE: return Genotype::EXPERIMENTAL;
        }
        return Genotype::BASELINE;
    }

    static double base_critical_overhead(const FlowProfile& profile) {
        switch (profile.flow_class) {
            case FlowClass::NERVE: return 3.0;
            case FlowClass::GLAND: return 2.5;
            case FlowClass::MUSCLE: return 1.5;
        }
        return 1.5;
    }

    static double base_important_overhead(const FlowProfile& profile) {
        switch (profile.flow_class) {
            case FlowClass::NERVE: return 1.0;
            case FlowClass::GLAND: return 1.5;
            case FlowClass::MUSCLE: return 1.2;
        }
        return 1.2;
    }

    static double maximum_overhead(Genotype genotype) {
        switch (genotype) {
            case Genotype::HYPERVIGILANT: return 6.0;
            case Genotype::BASELINE: return 4.0;
            case Genotype::STOIC: return 3.5;
            case Genotype::EXPERIMENTAL: return 3.0;
        }
        return 4.0;
    }

    static std::string flow_key(const FlowProfile& profile) {
        return std::to_string(static_cast<int>(profile.flow_class)) + ':' + profile.priority;
    }

    BiologicalFlowState& state_for(const FlowProfile& profile) {
        auto& state = states_[flow_key(profile)];
        if (!state.initialized) {
            state.genotype = initial_genotype(profile);
            state.base_critical_overhead = base_critical_overhead(profile);
            state.base_important_overhead = base_important_overhead(profile);
            state.critical_overhead = state.base_critical_overhead;
            state.important_overhead = state.base_important_overhead;
            state.initialized = true;
        }
        return state;
    }

    AdaptivePolicyConfig config_;
    std::unordered_map<std::string, BiologicalFlowState> states_;
};

} // namespace aurora::control

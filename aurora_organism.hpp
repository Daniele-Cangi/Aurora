#pragma once

#include "aurora_intention.hpp"
#include "include/aurora/control/TransportPolicy.hpp"
#include "include/aurora/fec/GenerationCodec.hpp"
#include "include/aurora/transport/GenerationManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aurora {

using transport::DecodeReport;
using transport::DecodeStatus;
using transport::GenerationDescriptor;
using transport::GenerationIdentity;
using transport::GenerationSegmentDescriptor;
using transport::TransportContract;
using transport::TransportImportance;

using FlowClass = control::FlowClass;
using Genotype = control::Genotype;
using GenotypeHint = control::GenotypeHint;
using FlowProfile = control::FlowProfile;
using OrganismSpawnResult = transport::GenerationSpawnResult;
using OrganismRepairResult = transport::GenerationRepairResult;
using GenerationRuntimeState = transport::GenerationRuntimeState;

// Historical view retained at the simulator boundary. New code should consume
// control::BiologicalFlowState directly.
struct FlowState {
    double crit_overhead = 1.0;
    double bulk_overhead = 1.0;
    double base_crit_overhead = 1.0;
    double base_bulk_overhead = 1.0;
    double avg_coverage = 0.0;
    int success_count = 0;
    int fail_count = 0;
    int panic_boost = 0;
    int good_streak = 0;
    int bad_streak = 0;
    Genotype genotype = Genotype::BASELINE;
    bool initialized = false;
    int age = 0;
};

class AuroraOrganism {
public:
    virtual ~AuroraOrganism() = default;

    [[nodiscard]] virtual FlowProfile build_profile(
        const TransportContract& contract) const = 0;

    virtual GenerationIdentity reserve_identity(
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes) = 0;

    virtual OrganismSpawnResult spawn(
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes,
        std::size_t symbol_size = 128,
        std::uint64_t now_ms = 0) = 0;

    virtual OrganismSpawnResult spawn_reserved(
        const GenerationIdentity& identity,
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes,
        std::size_t symbol_size = 128,
        std::uint64_t now_ms = 0) = 0;

    virtual DecodeReport integrate(
        const std::string& generation_id,
        const std::vector<::fec::Pkt>& received_packets,
        std::uint64_t now_ms) = 0;

    virtual OrganismRepairResult emit_repairs(
        const std::string& generation_id,
        std::uint32_t requested_symbols,
        bool critical_only,
        std::uint64_t now_ms = 0) = 0;

    [[nodiscard]] virtual std::optional<GenerationDescriptor> descriptor(
        const std::string& generation_id) const = 0;

    [[nodiscard]] virtual std::optional<GenerationRuntimeState> runtime_state(
        const std::string& generation_id,
        std::uint64_t now_ms = 0) const = 0;
};

// Compatibility facade for the original biological controller. Generation
// lifecycle, coding, and adaptation now live in independently replaceable
// components composed by transport::GenerationManager.
class AlienFountainOrganism final : public AuroraOrganism {
public:
    explicit AlienFountainOrganism(std::size_t maximum_active_generations = 128)
        : policy_(std::make_shared<control::BiologicalAdaptivePolicy>()),
          manager_(std::make_shared<fec::ExperimentalLtLikeCodec>(),
                   policy_, maximum_active_generations) {}

    AlienFountainOrganism(std::size_t maximum_active_generations,
                          control::AdaptivePolicyConfig config)
        : policy_(std::make_shared<control::BiologicalAdaptivePolicy>(config)),
          manager_(std::make_shared<fec::ExperimentalLtLikeCodec>(),
                   policy_, maximum_active_generations) {}

    [[nodiscard]] FlowProfile build_profile(
        const TransportContract& contract) const override {
        return manager_.build_profile(contract);
    }

    GenerationIdentity reserve_identity(
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes) override {
        return manager_.reserve_identity(contract, token_id, payload_bytes);
    }

    OrganismSpawnResult spawn(
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes,
        std::size_t symbol_size = 128,
        std::uint64_t now_ms = 0) override {
        return manager_.spawn(contract, token_id, payload_bytes, symbol_size, now_ms);
    }

    OrganismSpawnResult spawn_reserved(
        const GenerationIdentity& identity,
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes,
        std::size_t symbol_size = 128,
        std::uint64_t now_ms = 0) override {
        return manager_.spawn_reserved(
            identity, contract, token_id, payload_bytes, symbol_size, now_ms);
    }

    DecodeReport integrate(
        const std::string& generation_id,
        const std::vector<::fec::Pkt>& received_packets,
        std::uint64_t now_ms) override {
        return manager_.integrate(generation_id, received_packets, now_ms);
    }

    OrganismRepairResult emit_repairs(
        const std::string& generation_id,
        std::uint32_t requested_symbols,
        bool critical_only,
        std::uint64_t now_ms = 0) override {
        return manager_.emit_repairs(
            generation_id, requested_symbols, critical_only, now_ms);
    }

    [[nodiscard]] std::optional<GenerationDescriptor> descriptor(
        const std::string& generation_id) const override {
        return manager_.descriptor(generation_id);
    }

    [[nodiscard]] std::optional<GenerationRuntimeState> runtime_state(
        const std::string& generation_id,
        std::uint64_t now_ms = 0) const override {
        return manager_.runtime_state(generation_id, now_ms);
    }

    [[nodiscard]] std::size_t generation_count() const {
        return manager_.generation_count();
    }

    [[nodiscard]] std::optional<FlowState> flow_state(
        const FlowProfile& profile) const {
        const auto state = policy_->flow_state(profile);
        if (!state) return std::nullopt;
        FlowState legacy;
        legacy.crit_overhead = state->critical_overhead;
        legacy.bulk_overhead = state->important_overhead;
        legacy.base_crit_overhead = state->base_critical_overhead;
        legacy.base_bulk_overhead = state->base_important_overhead;
        legacy.avg_coverage = state->average_coverage;
        legacy.success_count = state->success_count;
        legacy.fail_count = state->failure_count;
        legacy.panic_boost = state->panic_boost;
        legacy.good_streak = state->good_streak;
        legacy.bad_streak = state->bad_streak;
        legacy.genotype = state->genotype;
        legacy.initialized = state->initialized;
        legacy.age = state->generation_count;
        return legacy;
    }

    [[nodiscard]] transport::GenerationManager& generation_manager() {
        return manager_;
    }

    [[nodiscard]] const transport::GenerationManager& generation_manager() const {
        return manager_;
    }

private:
    std::shared_ptr<control::BiologicalAdaptivePolicy> policy_;
    transport::GenerationManager manager_;
};

} // namespace aurora

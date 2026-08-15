#include "../aurora_organism.hpp"
#include "../include/aurora/control/TransportPolicy.hpp"
#include "../include/aurora/emulation/ProcessWorkload.hpp"
#include "../include/aurora/fec/GenerationCodec.hpp"
#include "../include/aurora/transport/GenerationManager.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class TaggedCodec final : public aurora::fec::GenerationCodec {
public:
    [[nodiscard]] std::string id() const override { return "test-codec"; }
    [[nodiscard]] std::uint16_t version() const override { return 7; }

    [[nodiscard]] std::unique_ptr<aurora::fec::SymbolEncoder> make_encoder(
        const std::vector<std::uint8_t>& bytes,
        std::size_t symbol_size,
        std::uint64_t generation_seed) const override {
        return delegate_.make_encoder(bytes, symbol_size, generation_seed);
    }

    [[nodiscard]] std::unique_ptr<aurora::fec::SymbolDecoder> make_decoder(
        std::uint32_t source_symbol_count,
        std::size_t symbol_size,
        std::size_t source_bytes) const override {
        return delegate_.make_decoder(source_symbol_count, symbol_size, source_bytes);
    }

private:
    aurora::fec::ExperimentalLtLikeCodec delegate_;
};

class RecordingPolicy final : public aurora::control::TransportPolicy {
public:
    [[nodiscard]] std::string id() const override { return "recording-policy"; }
    [[nodiscard]] std::uint16_t version() const override { return 3; }

    [[nodiscard]] aurora::control::FlowProfile profile_for(
        const aurora::transport::TransportContract& contract) const override {
        return aurora::control::transport_profile_for(contract);
    }

    aurora::control::ProtectionPlan plan(
        const aurora::transport::TransportContract& contract) override {
        ++plans;
        aurora::control::ProtectionPlan result;
        result.policy_id = id();
        result.policy_version = version();
        result.profile = profile_for(contract);
        result.critical_overhead = 2.0;
        result.important_overhead = 1.25;
        result.elastic_overhead = 1.0;
        return result;
    }

    void observe(const aurora::control::FlowProfile&,
                 const aurora::transport::DecodeReport& report) override {
        ++observations;
        last_status = report.status;
    }

    int observations = 0;
    int plans = 0;
    aurora::transport::DecodeStatus last_status =
        aurora::transport::DecodeStatus::NO_PROGRESS;
};

} // namespace

int main() {
    for (const auto policy_id : aurora::control::transport_policy_ids) {
        const auto selected =
            aurora::control::make_transport_policy(policy_id);
        assert(selected->id() == policy_id);
        assert(selected->version() == 1);
    }
    bool unknown_policy_rejected = false;
    try {
        (void)aurora::control::make_transport_policy("unknown");
    } catch (const std::invalid_argument&) {
        unknown_policy_rejected = true;
    }
    assert(unknown_policy_rejected);

    const auto pilot_workload =
        aurora::emulation::process_workload("policy-pilot-v1");
    assert(pilot_workload.generation_count == 8);
    assert(pilot_workload.symbol_size == 64);
    assert(pilot_workload.payload_bytes(0) == 2560);
    assert(pilot_workload.payload_bytes(7) == 2560);
    const auto pilot_contract =
        aurora::transport::TransportContract::parse(
            std::string(pilot_workload.contract));
    assert(pilot_contract.segments.size() == 2);
    assert(pilot_contract.segments[0].importance ==
           aurora::transport::TransportImportance::CRITICAL);
    assert(pilot_contract.segments[0].deadline_ms == 10'000);

    auto codec = std::make_shared<TaggedCodec>();
    auto policy = std::make_shared<RecordingPolicy>();
    aurora::transport::GenerationManager manager(codec, policy, 2);
    const auto contract = aurora::transport::TransportContract::parse(
        "deadline:100ms;importance:important;seed:92");
    const std::vector<std::uint8_t> bytes(129, 0xA4);

    auto causal_policy = std::make_shared<RecordingPolicy>();
    aurora::transport::GenerationManager causal_manager(
        codec, causal_policy, 1);
    bool invalid_symbol_size_rejected = false;
    try {
        (void)causal_manager.spawn(
            contract, "invalid-symbol-size", bytes, 0, 0);
    } catch (const std::invalid_argument&) {
        invalid_symbol_size_rejected = true;
    }
    assert(invalid_symbol_size_rejected);
    const auto reserved = causal_manager.reserve_identity(
        contract, "reserved", bytes);
    assert(!reserved.validation_error().has_value());
    assert(reserved.sequence == 0);
    assert(causal_policy->plans == 0);
    assert(causal_manager.generation_count() == 0);
    const auto planned = causal_manager.spawn_reserved(
        reserved, contract, "reserved", bytes, 32, 25);
    assert(causal_policy->plans == 1);
    assert(causal_manager.generation_count() == 1);
    assert(planned.descriptor.generation_id == reserved.generation_id);
    assert(planned.descriptor.created_at_ms == 25);
    bool reused_identity_rejected = false;
    try {
        (void)causal_manager.spawn_reserved(
            reserved, contract, "reserved", bytes, 32, 26);
    } catch (const std::logic_error&) {
        reused_identity_rejected = true;
    }
    assert(reused_identity_rejected);

    const auto generated = manager.spawn(contract, "modular", bytes, 32, 0);
    assert(generated.descriptor.codec_id == "test-codec");
    assert(generated.descriptor.codec_version == 7);
    assert(generated.descriptor.policy_id == "recording-policy");
    assert(generated.descriptor.policy_version == 3);
    assert(generated.K == generated.source_symbol_count);

    const auto complete = manager.integrate(
        generated.descriptor.generation_id, generated.packets, 10);
    assert(complete.delivered());
    assert(complete.payload == bytes);
    assert(policy->observations == 1);
    assert(policy->last_status == aurora::transport::DecodeStatus::COMPLETE);

    const auto expiring = manager.spawn(contract, "modular-expired", bytes, 32, 0);
    const auto expired = manager.integrate(
        expiring.descriptor.generation_id, {}, 101);
    assert(expired.status == aurora::transport::DecodeStatus::EXPIRED);
    assert(policy->observations == 2);

    aurora::AlienFountainOrganism compatibility;
    const auto legacy = compatibility.spawn(contract, "compatibility", bytes, 32, 0);
    assert(legacy.descriptor.codec_id == "experimental-lt-like");
    assert(legacy.descriptor.policy_id == "biological-adaptive");

    auto injected_policy =
        std::make_shared<aurora::control::FixedTransportPolicy>(
            "injected-fixed", 2.0, 1.5, 1.0);
    aurora::AlienFountainOrganism injected(
        std::make_shared<TaggedCodec>(), injected_policy);
    const auto injected_generation = injected.spawn(
        contract, "injected", bytes, 32, 0);
    assert(injected_generation.descriptor.codec_id == "test-codec");
    assert(injected_generation.descriptor.policy_id == "injected-fixed");
    assert(!injected.flow_state(injected.build_profile(contract)).has_value());

    aurora::control::AdaptivePolicyConfig adaptive_config;
    adaptive_config.panic_boost_generations = 3;
    aurora::control::BiologicalAdaptivePolicy adaptive(adaptive_config);
    auto adaptive_contract = contract;
    adaptive_contract.deadline_s = 10.0;
    adaptive_contract.reliability = 0.99;
    adaptive_contract.maximum_repair_amplification = 6.0;
    const auto profile = adaptive.profile_for(adaptive_contract);
    const auto baseline_plan = adaptive.plan(adaptive_contract);

    aurora::transport::DecodeReport failure;
    failure.generation_id = "failed-generation";
    failure.status = aurora::transport::DecodeStatus::EXPIRED;
    failure.coverage = 0.0;
    adaptive.observe(profile, failure);
    const auto failed_state = adaptive.flow_state(profile);
    assert(failed_state.has_value());
    assert(failed_state->panic_boost == 3);
    assert(failed_state->important_overhead > baseline_plan.important_overhead);

    for (int generation = 0; generation < 3; ++generation) {
        const auto boosted = adaptive.plan(adaptive_contract);
        const auto state = adaptive.flow_state(profile);
        assert(state.has_value());
        assert(boosted.critical_overhead > state->critical_overhead);
        assert(boosted.important_overhead > state->important_overhead);
        assert(state->panic_boost == 2 - generation);
    }
    const auto unboosted = adaptive.plan(adaptive_contract);
    const auto post_panic = adaptive.flow_state(profile);
    assert(post_panic.has_value());
    assert(post_panic->panic_boost == 0);
    assert(unboosted.critical_overhead == post_panic->critical_overhead);
    assert(unboosted.important_overhead == post_panic->important_overhead);

    const double overhead_after_failure = post_panic->important_overhead;
    aurora::transport::DecodeReport success;
    success.status = aurora::transport::DecodeStatus::COMPLETE;
    success.payload_complete = true;
    success.coverage = 1.0;
    for (int generation = 0; generation < 12; ++generation) {
        success.generation_id = "successful-generation-" + std::to_string(generation);
        (void)adaptive.plan(adaptive_contract);
        adaptive.observe(profile, success);
    }
    const auto recovered = adaptive.flow_state(profile);
    assert(recovered.has_value());
    assert(recovered->panic_boost == 0);
    assert(recovered->good_streak == 12);
    assert(recovered->important_overhead < overhead_after_failure);
    assert(recovered->important_overhead >= recovered->base_important_overhead);

    std::cout << "modular transport tests passed\n";
    return 0;
}

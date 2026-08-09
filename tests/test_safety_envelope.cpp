#include "../aurora_organism.hpp"
#include "../include/aurora/safety/SafetyEnvelope.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using aurora::AlienFountainOrganism;
using aurora::safety::LinkMode;
using aurora::safety::SafetyEnvelope;
using aurora::safety::TransportDecision;
using aurora::safety::TransportState;
using aurora::transport::TransportContract;

int main() {
    auto contract = TransportContract::parse(
        "deadline:1s;rf:on;optical:off;backscatter:on;reserve_floor:0.2;"
        "max_observation_age:100ms;max_repair_amplification:2;seed:77");
    AlienFountainOrganism controller;
    const auto spawned = controller.spawn(contract, "safety", std::vector<std::uint8_t>(320, 0x5A), 32, 0);

    SafetyEnvelope envelope;
    TransportDecision proposed;
    proposed.link = LinkMode::RF;
    proposed.transmission_attempts = 3;
    proposed.repair_symbols = 10'000;

    TransportState state;
    state.observed_at_ms = 10;
    state.now_ms = 10;
    state.source_energy_reserve = 0.8;
    state.rf_duty_remaining = 0.0;
    state.required_rank = spawned.descriptor.total_source_symbols;
    const auto spawned_runtime = controller.runtime_state(
        spawned.descriptor.generation_id);
    assert(spawned_runtime.has_value());
    state.emitted_symbols = spawned_runtime->emitted_symbols;
    state.critical_emitted_symbols = spawned_runtime->critical_emitted_symbols;

    const auto constrained = envelope.constrain(contract, spawned.descriptor, state, proposed);
    assert(constrained.decision.permitted);
    assert(constrained.decision.link == LinkMode::BACKSCATTER);
    const auto maximum_emitted = static_cast<std::uint64_t>(
        std::ceil(spawned.descriptor.total_source_symbols *
                  contract.maximum_repair_amplification));
    assert(constrained.decision.repair_symbols ==
           maximum_emitted - state.emitted_symbols);
    assert(constrained.constraints_applied.size() == 2);

    auto critical_contract = contract;
    critical_contract.importance = aurora::transport::TransportImportance::CRITICAL;
    const auto critical = controller.spawn(
        critical_contract, "critical-safety", std::vector<std::uint8_t>(64, 0xA5), 32, 0);
    TransportDecision underprotected;
    underprotected.link = LinkMode::BACKSCATTER;
    underprotected.critical_only = true;
    underprotected.repair_symbols = 0;
    state.rf_duty_remaining = 1.0;
    state.emitted_symbols = critical.descriptor.total_source_symbols;
    state.critical_emitted_symbols = critical.descriptor.total_source_symbols;
    const auto protected_decision = envelope.constrain(
        critical_contract, critical.descriptor, state, underprotected);
    assert(protected_decision.decision.repair_symbols >= 1);
    assert(!protected_decision.constraints_applied.empty());

    state.now_ms = 111;
    const auto stale = envelope.constrain(contract, spawned.descriptor, state, proposed);
    assert(!stale.decision.permitted);
    assert(stale.decision.transmission_attempts == 0);

    state.observed_at_ms = 20;
    state.now_ms = 20;
    state.source_energy_reserve = 0.1;
    const auto energy_limited = envelope.constrain(contract, spawned.descriptor, state, proposed);
    assert(!energy_limited.decision.permitted);

    state.source_energy_reserve = 0.8;
    state.observed_at_ms = 1001;
    state.now_ms = 1001;
    const auto expired = envelope.constrain(contract, spawned.descriptor, state, proposed);
    assert(!expired.decision.permitted);

    state.now_ms = 20;
    state.observed_at_ms = 20;
    state.source_energy_reserve = std::numeric_limits<double>::quiet_NaN();
    const auto invalid_energy = envelope.constrain(
        contract, spawned.descriptor, state, proposed);
    assert(!invalid_energy.decision.permitted);
    assert(invalid_energy.constraints_applied.front() ==
           "invalid source energy observation");

    auto cost_contract = contract;
    cost_contract.allow_backscatter = false;
    cost_contract.allow_optical = false;
    state.source_energy_reserve = 0.21;
    state.source_energy_capacity_j = 10.0;
    state.rf_energy_cost_per_attempt_j = 0.01;
    state.rf_duty_remaining = 1.0;
    state.rf_duty_remaining_s = 0.012;
    state.rf_airtime_per_attempt_s = 0.005;
    state.observed_at_ms = 20;
    state.now_ms = 20;
    state.emitted_symbols = spawned_runtime->emitted_symbols;
    state.critical_emitted_symbols = spawned_runtime->critical_emitted_symbols;
    TransportDecision costly;
    costly.link = LinkMode::RF;
    costly.transmission_attempts = 10;
    const auto cost_limited = envelope.constrain(
        cost_contract, spawned.descriptor, state, costly);
    assert(cost_limited.decision.permitted);
    assert(cost_limited.decision.transmission_attempts == 2);

    state.source_energy_reserve = 0.2005;
    const auto crosses_reserve = envelope.constrain(
        cost_contract, spawned.descriptor, state, costly);
    assert(!crosses_reserve.decision.permitted);
    assert(crosses_reserve.decision.transmission_attempts == 0);

    const auto segmented_contract = TransportContract::parse(
        "deadline:2s;rf:off;optical:off;backscatter:on;seed:78;"
        "max_repair_amplification:3;"
        "segment:0-31,critical,100ms,0.999;"
        "segment:32-63,important,1s,0.99");
    const auto segmented = controller.spawn(
        segmented_contract, "segment-safety",
        std::vector<std::uint8_t>(64, 0x78), 16, 0);
    const auto segmented_runtime = controller.runtime_state(
        segmented.descriptor.generation_id, 101);
    assert(segmented_runtime.has_value());
    TransportState segment_state;
    segment_state.observed_at_ms = 101;
    segment_state.now_ms = 101;
    segment_state.source_energy_reserve = 0.8;
    segment_state.rf_duty_remaining = 1.0;
    segment_state.emitted_symbols = segmented_runtime->emitted_symbols;
    segment_state.critical_emitted_symbols =
        segmented_runtime->critical_emitted_symbols;
    for (const auto& segment : segmented_runtime->segments) {
        segment_state.segments.push_back({
            segment.segment_id, segment.emitted_symbols,
            segment.decoder_rank, segment.complete, segment.expired});
    }
    TransportDecision expired_critical;
    expired_critical.link = LinkMode::BACKSCATTER;
    expired_critical.critical_only = true;
    expired_critical.repair_symbols = 10'000;
    const auto live_segment_only = envelope.constrain(
        segmented_contract, segmented.descriptor,
        segment_state, expired_critical);
    assert(live_segment_only.decision.permitted);
    assert(!live_segment_only.decision.critical_only);
    assert(live_segment_only.decision.repair_symbols > 0);

    segment_state.now_ms = 1'001;
    segment_state.observed_at_ms = 1'001;
    for (auto& segment : segment_state.segments) segment.expired = true;
    const auto no_live_segment = envelope.constrain(
        segmented_contract, segmented.descriptor,
        segment_state, expired_critical);
    assert(!no_live_segment.decision.permitted);
    assert(no_live_segment.constraints_applied.back() ==
           "no unexpired incomplete segment remains");

    std::cout << "safety envelope tests passed\n";
    return 0;
}

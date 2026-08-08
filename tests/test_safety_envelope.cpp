#include "../aurora_organism.hpp"
#include "../include/aurora/safety/SafetyEnvelope.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
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

    const auto constrained = envelope.constrain(contract, spawned.descriptor, state, proposed);
    assert(constrained.decision.permitted);
    assert(constrained.decision.link == LinkMode::BACKSCATTER);
    assert(constrained.decision.repair_symbols <= spawned.descriptor.total_source_symbols);
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

    std::cout << "safety envelope tests passed\n";
    return 0;
}

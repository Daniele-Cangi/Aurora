#include "../aurora_organism.hpp"
#include "../include/aurora/safety/SafetyEnvelope.hpp"
#include "../include/aurora/telemetry/DecisionReplayLog.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    using namespace aurora;
    using namespace aurora::safety;

    auto contract = transport::TransportContract::parse(
        "deadline:1s;rf:on;optical:off;backscatter:on;reserve_floor:0.2;"
        "max_observation_age:100ms;max_repair_amplification:2;"
        "min_critical_overhead:1.5;importance:critical;seed:71");
    AlienFountainOrganism controller;
    const auto spawned = controller.spawn(
        contract, "replay-generation", std::vector<std::uint8_t>(96, 0x6A), 32, 0);

    SafetyEnvelope envelope;
    TransportState observed;
    observed.observed_at_ms = 10;
    observed.now_ms = 10;
    observed.source_energy_reserve = 0.75;
    observed.rf_duty_remaining = 0.0;
    observed.decoder_rank = 1;
    observed.required_rank = spawned.descriptor.total_source_symbols;

    TransportDecision proposed;
    proposed.link = LinkMode::RF;
    proposed.transmission_attempts = 0;
    proposed.repair_symbols = 10'000;
    proposed.critical_only = true;

    telemetry::DecisionReplayLog log;
    const auto constrained = envelope.constrain(
        contract, spawned.descriptor, observed, proposed);
    log.record(contract, spawned.descriptor, constrained);

    observed.now_ms = 200;
    const auto stale = envelope.constrain(
        contract, spawned.descriptor, observed, proposed);
    log.record(contract, spawned.descriptor, stale);

    const auto first_encoding = log.serialize();
    assert(first_encoding == log.serialize());
    const auto restored = telemetry::DecisionReplayLog::deserialize(first_encoding);
    assert(restored.serialize() == first_encoding);
    const auto verified = restored.verify();
    assert(verified.ok);
    assert(verified.records_verified == 2);

    auto contradictory = constrained;
    contradictory.decision.permitted = false;
    telemetry::DecisionReplayLog contradictory_log;
    contradictory_log.record(contract, spawned.descriptor, contradictory);
    const auto mismatch = contradictory_log.verify();
    assert(!mismatch.ok);
    assert(mismatch.records_verified == 0);

    auto corrupted = first_encoding;
    const auto record_position = corrupted.find("R|");
    assert(record_position != std::string::npos);
    corrupted[record_position + 2] = '9';
    bool corruption_rejected = false;
    try {
        (void)telemetry::DecisionReplayLog::deserialize(corrupted);
    } catch (const std::invalid_argument&) {
        corruption_rejected = true;
    }
    assert(corruption_rejected);

    const auto footer_position = first_encoding.rfind("END|");
    assert(footer_position != std::string::npos);
    bool truncation_rejected = false;
    try {
        (void)telemetry::DecisionReplayLog::deserialize(
            first_encoding.substr(0, footer_position));
    } catch (const std::invalid_argument&) {
        truncation_rejected = true;
    }
    assert(truncation_rejected);

    std::cout << "decision replay tests passed\n";
    return 0;
}

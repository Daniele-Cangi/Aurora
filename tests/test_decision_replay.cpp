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
    observed.rf_duty_remaining_s = 5.0;
    observed.decoder_rank = 1;
    observed.required_rank = spawned.descriptor.total_source_symbols;
    const auto runtime = controller.runtime_state(spawned.descriptor.generation_id);
    assert(runtime.has_value());
    observed.emitted_symbols = runtime->emitted_symbols;
    observed.critical_emitted_symbols = runtime->critical_emitted_symbols;
    for (const auto& segment : runtime->segments) {
        observed.segments.push_back({
            segment.segment_id,
            segment.emitted_symbols,
            segment.decoder_rank,
            segment.complete,
            segment.expired});
    }

    TransportDecision proposed;
    proposed.link = LinkMode::RF;
    proposed.transmission_attempts = 0;
    proposed.repair_symbols = 10'000;
    proposed.critical_only = true;

    telemetry::DecisionReplayLog log;
    auto constrained = envelope.constrain(
        contract, spawned.descriptor, observed, proposed);
    constrained.execution.recorded = true;
    constrained.execution.link = constrained.decision.link;
    constrained.execution.transmission_attempts =
        constrained.decision.transmission_attempts;
    constrained.execution.hal_accepted_attempts =
        constrained.decision.transmission_attempts;
    constrained.execution.delivered_attempts = 1;
    constrained.execution.repair_symbols_emitted =
        constrained.decision.repair_symbols;
    constrained.execution.critical_only = constrained.decision.critical_only;
    TransportAttemptTrace attempt;
    attempt.simulated_now_ms = observed.now_ms;
    attempt.packet_sequence = 1;
    attempt.symbol_seed = 17;
    attempt.segment_id = 0;
    attempt.critical = true;
    attempt.attempted = true;
    attempt.hal_evaluated = true;
    attempt.hal_replayable = true;
    attempt.hal_accepted = true;
    attempt.transmitted = true;
    attempt.delivered = true;
    attempt.energy_before_j = 2.0;
    attempt.energy_after_j = 1.75;
    attempt.energy_cost_j = 0.25;
    attempt.duty_before_s = 5.0;
    attempt.duty_after_s = 5.0;
    attempt.channel_evaluated = true;
    attempt.channel_snr_db = 1.0;
    attempt.channel_coding_gain_db = 1.0;
    attempt.channel_fading_db = -1.0;
    attempt.channel_threshold_db = 0.0;
    constrained.execution.attempts.push_back(attempt);
    assert(!constrained.execution_error().has_value());
    log.record(contract, spawned.descriptor, constrained);

    observed.now_ms = 200;
    const auto stale = envelope.constrain(
        contract, spawned.descriptor, observed, proposed);
    auto stale_execution = stale;
    stale_execution.execution.recorded = true;
    stale_execution.execution.link = stale_execution.decision.link;
    stale_execution.execution.critical_only = stale_execution.decision.critical_only;
    log.record(contract, spawned.descriptor, stale_execution);

    const auto first_encoding = log.serialize();
    assert(first_encoding.starts_with("AURORA_DECISION_TRACE_V3\n"));
    assert(first_encoding == log.serialize());
    const auto restored = telemetry::DecisionReplayLog::deserialize(first_encoding);
    assert(restored.serialize() == first_encoding);
    const auto verified = restored.verify();
    assert(verified.ok);
    assert(verified.records_verified == 2);

    auto contradictory = constrained;
    contradictory.decision.permitted = false;
    contradictory.decision.transmission_attempts = 0;
    contradictory.decision.repair_symbols = 0;
    contradictory.execution = {};
    contradictory.execution.recorded = true;
    contradictory.execution.link = contradictory.decision.link;
    contradictory.execution.critical_only = contradictory.decision.critical_only;
    telemetry::DecisionReplayLog contradictory_log;
    contradictory_log.record(contract, spawned.descriptor, contradictory);
    const auto mismatch = contradictory_log.verify();
    assert(!mismatch.ok);
    assert(mismatch.records_verified == 0);

    auto execution_mismatch = constrained;
    --execution_mismatch.execution.transmission_attempts;
    bool execution_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_execution_log;
        invalid_execution_log.record(
            contract, spawned.descriptor, execution_mismatch);
    } catch (const std::invalid_argument&) {
        execution_rejected = true;
    }
    assert(execution_rejected);

    auto energy_mismatch = constrained;
    energy_mismatch.execution.attempts.front().energy_after_j += 0.1;
    bool energy_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_energy_log;
        invalid_energy_log.record(contract, spawned.descriptor, energy_mismatch);
    } catch (const std::invalid_argument&) {
        energy_transition_rejected = true;
    }
    assert(energy_transition_rejected);

    auto channel_mismatch = constrained;
    channel_mismatch.execution.attempts.front().channel_fading_db = -10.0;
    bool channel_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_channel_log;
        invalid_channel_log.record(contract, spawned.descriptor, channel_mismatch);
    } catch (const std::invalid_argument&) {
        channel_transition_rejected = true;
    }
    assert(channel_transition_rejected);

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

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

    SafetyConfig safety_config;
    safety_config.window_size = 1;
    safety_config.minimum_window_samples = 1;
    safety_config.maximum_observation_age_ms = 100;
    SafetyMonitor safety_monitor(safety_config);
    auto operating_mode = control::OperatingMode::NORMAL;
    control::ProposalStateSnapshot proposal_state;
    proposal_state.random_state = 71;
    auto proposal_input = control::ProposalInput{};
    proposal_input.deadline_s = 1.0;
    proposal_input.deadline_left_s = 1.0;
    proposal_input.source_soc = observed.source_energy_reserve;
    proposal_input.rf_duty_remaining = observed.rf_duty_remaining;
    proposal_input.symbols_have = static_cast<int>(observed.decoder_rank);
    proposal_input.symbols_need = static_cast<int>(observed.required_rank);
    proposal_input.snr_db = {30.0, 20.0, 10.0};
    proposal_input.historical_per = {0.01, 0.01, 0.01};
    proposal_input.priority = control::ProposalPriority::BULK;
    proposal_input.has_critical_segments = true;
    auto begin_proposal = [&](double epoch) {
        control::ProposalTransition transition;
        transition.recorded = true;
        transition.before = proposal_state;
        transition.input = proposal_input;
        transition.input.epoch = epoch;
        transition.input.covert_sequence = static_cast<std::uint16_t>(
            (contract.experiment_seed +
             static_cast<std::uint64_t>(epoch - 1.0)) & 0xFFULL);
        transition.input.operating_mode = operating_mode;
        transition.decision = control::derive_proposal(
            transition.input, proposal_state);
        transition.after_proposal = proposal_state;
        return transition;
    };
    auto finish_proposal = [&](control::ProposalTransition transition,
                               const TransportDecisionTrace& trace) {
        transition.feedback.applied =
            trace.execution.transmission_attempts > 0;
        transition.feedback.executed_link = trace.execution.link;
        transition.feedback.reward = transition.feedback.applied
            ? static_cast<double>(trace.execution.delivered_attempts) /
                static_cast<double>(trace.execution.transmission_attempts)
            : 0.0;
        control::apply_proposal_feedback(proposal_state, transition.feedback);
        transition.after = proposal_state;
        assert(!transition.validation_error().has_value());
        return transition;
    };
    auto health_observation = SafetyEvidenceSample{};
    health_observation.observed_at_ms = 10;
    health_observation.duty_left = 1.0;
    health_observation.nerve_fail_rate = 0.1;
    health_observation.gland_fail_rate = 0.1;
    health_observation.nerve_cov = 0.5;
    health_observation.gland_cov = 0.5;
    health_observation.nerve_has_evidence = true;
    health_observation.gland_has_evidence = true;
    auto controller_transition = [&](const SafetyEvidenceSample& evidence,
                                     std::uint64_t now_ms) {
        control::ControllerTransition transition;
        transition.recorded = true;
        transition.now_ms = now_ms;
        transition.observation = evidence;
        transition.before = safety_monitor.snapshot();
        transition.mode_before = operating_mode;
        safety_monitor.observe(evidence, now_ms);
        transition.after = safety_monitor.snapshot();
        operating_mode = control::select_operating_mode(
            control::operating_mode_input(safety_monitor.state(), evidence));
        transition.mode_after = operating_mode;
        assert(!transition.validation_error().has_value());
        return transition;
    };

    telemetry::DecisionReplayLog log;
    auto first_proposal = begin_proposal(1.0);
    auto constrained = envelope.constrain(
        contract, spawned.descriptor, observed,
        first_proposal.decision.transport);
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
    first_proposal = finish_proposal(first_proposal, constrained);
    const auto first_controller = controller_transition(health_observation, 10);
    log.record(
        contract, spawned.descriptor, constrained,
        first_proposal, first_controller);

    observed.now_ms = 200;
    proposal_input.deadline_left_s = 0.8;
    auto second_proposal = begin_proposal(2.0);
    const auto stale = envelope.constrain(
        contract, spawned.descriptor, observed,
        second_proposal.decision.transport);
    auto stale_execution = stale;
    stale_execution.execution.recorded = true;
    stale_execution.execution.link = stale_execution.decision.link;
    stale_execution.execution.critical_only = stale_execution.decision.critical_only;
    second_proposal = finish_proposal(second_proposal, stale_execution);
    const auto stale_controller = controller_transition(health_observation, 200);
    assert(stale_controller.after.current_state == SafetyState::NO_EVIDENCE);
    assert(stale_controller.mode_after == control::OperatingMode::CONSERVATIVE);
    log.record(
        contract, spawned.descriptor, stale_execution,
        second_proposal, stale_controller);

    const auto first_encoding = log.serialize();
    assert(first_encoding.starts_with("AURORA_DECISION_TRACE_V6\n"));
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
    auto contradictory_proposal = first_proposal;
    contradictory_proposal.feedback.applied = false;
    contradictory_proposal.feedback.executed_link = contradictory.execution.link;
    contradictory_proposal.feedback.reward = 0.0;
    contradictory_proposal.after = contradictory_proposal.after_proposal;
    telemetry::DecisionReplayLog contradictory_log;
    contradictory_log.record(
        contract, spawned.descriptor, contradictory,
        contradictory_proposal, first_controller);
    const auto mismatch = contradictory_log.verify();
    assert(!mismatch.ok);
    assert(mismatch.records_verified == 0);

    auto execution_mismatch = constrained;
    --execution_mismatch.execution.transmission_attempts;
    bool execution_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_execution_log;
        invalid_execution_log.record(
            contract, spawned.descriptor, execution_mismatch,
            first_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        execution_rejected = true;
    }
    assert(execution_rejected);

    auto energy_mismatch = constrained;
    energy_mismatch.execution.attempts.front().energy_after_j += 0.1;
    bool energy_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_energy_log;
        invalid_energy_log.record(
            contract, spawned.descriptor, energy_mismatch,
            first_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        energy_transition_rejected = true;
    }
    assert(energy_transition_rejected);

    auto channel_mismatch = constrained;
    channel_mismatch.execution.attempts.front().channel_fading_db = -10.0;
    bool channel_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_channel_log;
        invalid_channel_log.record(
            contract, spawned.descriptor, channel_mismatch,
            first_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        channel_transition_rejected = true;
    }
    assert(channel_transition_rejected);

    auto contact_mismatch = constrained;
    contact_mismatch.observed.rf_contact_available = false;
    contact_mismatch.observed.optical_contact_available = false;
    contact_mismatch.observed.backscatter_contact_available = false;
    bool contact_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_contact_log;
        invalid_contact_log.record(
            contract, spawned.descriptor, contact_mismatch,
            first_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        contact_transition_rejected = true;
    }
    assert(contact_transition_rejected);

    auto invalid_controller = first_controller;
    invalid_controller.mode_after = control::OperatingMode::AGGRESSIVE;
    bool controller_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_controller_log;
        invalid_controller_log.record(
            contract, spawned.descriptor, constrained,
            first_proposal, invalid_controller);
    } catch (const std::invalid_argument&) {
        controller_transition_rejected = true;
    }
    assert(controller_transition_rejected);

    auto mistimed_controller = first_controller;
    ++mistimed_controller.now_ms;
    mistimed_controller.after.last_now_ms = mistimed_controller.now_ms;
    bool controller_time_rejected = false;
    try {
        telemetry::DecisionReplayLog mistimed_controller_log;
        mistimed_controller_log.record(
            contract, spawned.descriptor, constrained,
            first_proposal, mistimed_controller);
    } catch (const std::invalid_argument&) {
        controller_time_rejected = true;
    }
    assert(controller_time_rejected);

    auto invalid_proposal = first_proposal;
    ++invalid_proposal.decision.preamble_symbols;
    bool proposal_transition_rejected = false;
    try {
        telemetry::DecisionReplayLog invalid_proposal_log;
        invalid_proposal_log.record(
            contract, spawned.descriptor, constrained,
            invalid_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        proposal_transition_rejected = true;
    }
    assert(proposal_transition_rejected);

    auto wrong_seed_proposal = first_proposal;
    ++wrong_seed_proposal.before.random_state;
    auto wrong_seed_state = wrong_seed_proposal.before;
    wrong_seed_proposal.decision = control::derive_proposal(
        wrong_seed_proposal.input, wrong_seed_state);
    wrong_seed_proposal.after_proposal = wrong_seed_state;
    control::apply_proposal_feedback(
        wrong_seed_state, wrong_seed_proposal.feedback);
    wrong_seed_proposal.after = wrong_seed_state;
    assert(!wrong_seed_proposal.validation_error().has_value());
    bool proposal_seed_rejected = false;
    try {
        telemetry::DecisionReplayLog wrong_seed_log;
        wrong_seed_log.record(
            contract, spawned.descriptor, constrained,
            wrong_seed_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        proposal_seed_rejected = true;
    }
    assert(proposal_seed_rejected);

    bool controller_discontinuity_rejected = false;
    try {
        telemetry::DecisionReplayLog discontinuous_log;
        discontinuous_log.record(
            contract, spawned.descriptor, constrained,
            first_proposal, first_controller);
        discontinuous_log.record(
            contract, spawned.descriptor, constrained,
            first_proposal, first_controller);
    } catch (const std::invalid_argument&) {
        controller_discontinuity_rejected = true;
    }
    assert(controller_discontinuity_rejected);

    auto legacy_v5 = first_encoding;
    legacy_v5.replace(
        0,
        std::string("AURORA_DECISION_TRACE_V6").size(),
        "AURORA_DECISION_TRACE_V5");
    bool legacy_v5_rejected = false;
    try {
        (void)telemetry::DecisionReplayLog::deserialize(legacy_v5);
    } catch (const std::invalid_argument&) {
        legacy_v5_rejected = true;
    }
    assert(legacy_v5_rejected);

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

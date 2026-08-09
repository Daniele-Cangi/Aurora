#define AURORA_NO_MAIN
#include "../aurora_x.cpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

fec::Pkt packet(std::uint32_t seed, fec::SegmentKind kind) {
    fec::Pkt value;
    value.fp.seed = seed;
    value.fp.deg = 1;
    value.fp.data.assign(32, static_cast<std::uint8_t>(seed));
    value.seq = seed;
    value.token_id = "runtime-test";
    value.generation_id = "runtime-generation";
    value.segment_id = kind == fec::SegmentKind::CRITICAL ? 0U : 1U;
    value.kind = kind;
    value.descriptor_fingerprint = 1;
    return value;
}

void rolling_window_discards_oldest_observation() {
    telem::Window window(3);
    window.push(1.0);
    window.push(2.0);
    window.push(3.0);
    assert(std::abs(window.mean() - 2.0) < 1e-12);
    window.push(9.0);
    assert(window.q.size() == 3);
    assert(window.q.front() == 2.0);
    assert(window.q.back() == 9.0);
    assert(std::abs(window.mean() - (14.0 / 3.0)) < 1e-12);
}

void simulation_duty_uses_contract_budget() {
    HAL::SimulationDutyLimiter restrictive;
    HAL::SimulationDutyLimiter permissive;
    restrictive.configure(0.001);
    permissive.configure(0.01);

    auto permitted = [](HAL::SimulationDutyLimiter& limiter) {
        int count = 0;
        while (count < 100 && limiter.consume(0, 0.01)) ++count;
        return count;
    };
    const int restrictive_budget = permitted(restrictive);
    const int permissive_budget = permitted(permissive);
    assert(restrictive_budget == 6);
    assert(permissive_budget == 60);
    assert(permissive_budget > restrictive_budget);
}

void refused_transmissions_never_reach_receiver() {
    world::World world;
    Node source;
    Node receiver;
    source.pos = {0.0, 0.0};
    receiver.pos = {0.1, 0.1};
    source.configure_simulation_duty(1.0);
    source.buf.push_back(packet(1, fec::SegmentKind::BULK));
    const double energy_before = source.bat.E;
    bool hal_called = false;
    const auto hal_rejected = source.send_one(
        world, receiver, phy::Mode::RF, 0, false,
        [&](phy::Mode, const fec::Pkt&) {
            hal_called = true;
            return false;
        });
    assert(hal_called);
    assert(hal_rejected.attempted);
    assert(!hal_rejected.transmitted);
    assert(!hal_rejected.delivered);
    assert(hal_rejected.refusal == Node::SendRefusal::HAL);
    assert(receiver.inbox.empty());
    assert(source.bat.E == energy_before);

    Node duty_limited;
    Node duty_receiver;
    duty_limited.configure_simulation_duty(0.0);
    duty_limited.buf.push_back(packet(2, fec::SegmentKind::BULK));
    bool duty_hal_called = false;
    const auto duty_rejected = duty_limited.send_one(
        world, duty_receiver, phy::Mode::RF, 0, false,
        [&](phy::Mode, const fec::Pkt&) {
            duty_hal_called = true;
            return true;
        });
    assert(!duty_hal_called);
    assert(duty_rejected.attempted);
    assert(!duty_rejected.transmitted);
    assert(duty_rejected.refusal == Node::SendRefusal::DUTY);
    assert(duty_receiver.inbox.empty());
}

void critical_only_selects_only_critical_packets() {
    world::World world;
    Node source;
    Node receiver;
    source.configure_simulation_duty(1.0);
    source.buf.push_back(packet(3, fec::SegmentKind::BULK));
    source.buf.push_back(packet(4, fec::SegmentKind::CRITICAL));
    const auto sent = source.send_one(
        world, receiver, phy::Mode::IR, 0, true,
        [](phy::Mode, const fec::Pkt&) { return true; });
    assert(sent.attempted);
    assert(sent.transmitted);
    assert(sent.segment_kind == fec::SegmentKind::CRITICAL);
}

void simulation_attempts_replay_hal_channel_energy_and_duty() {
    world::World world;
    Node source;
    Node receiver;
    source.pos = {0.0, 0.0};
    receiver.pos = {0.1, 0.1};
    source.configure_simulation_duty(1.0);
    source.buf.push_back(packet(5, fec::SegmentKind::BULK));
    util::rng.reseed(0x515151ULL);

    aurora::safety::TransportDecisionTrace replay;
    replay.generation_id = "runtime-generation";
    replay.decision.link = aurora::safety::LinkMode::RF;
    replay.decision.repair_symbols = 0;
    replay.execution.recorded = true;
    replay.execution.link = aurora::safety::LinkMode::RF;
    replay.execution.repair_symbols_emitted = 0;

    bool channel_observed = false;
    for (int count = 0; count < 20 && !channel_observed; ++count) {
        const auto sent = source.send_one(
            world, receiver, phy::Mode::RF, 10, false);
        assert(sent.attempted);
        assert(sent.trace.hal_evaluated);
        assert(sent.trace.hal_replayable);
        assert(sent.trace.lbt_evaluated);
        replay.execution.attempts.push_back(sent.trace);
        if (sent.trace.hal_accepted) ++replay.execution.hal_accepted_attempts;
        if (sent.delivered) ++replay.execution.delivered_attempts;
        channel_observed = sent.trace.channel_evaluated;
    }
    assert(channel_observed);
    replay.decision.transmission_attempts = static_cast<std::uint32_t>(
        replay.execution.attempts.size());
    replay.execution.transmission_attempts = replay.decision.transmission_attempts;
    replay.observed.now_ms = 10;
    replay.observed.rf_duty_remaining_s =
        replay.execution.attempts.front().duty_before_s;
    assert(!replay.execution_error().has_value());
}

void safety_monitor_ignores_inactive_flow_classes() {
    aurora::safety::SafetyConfig config;
    config.window_size = 5;
    config.escalation_samples = 1;
    config.recovery_samples = 1;

    aurora::safety::SafetyMonitor empty(config);
    aurora::safety::TelemetrySample no_evidence;
    no_evidence.duty_left = 1.0;
    for (int i = 0; i < 5; ++i) empty.observe(no_evidence);
    assert(empty.state() == aurora::safety::SafetyState::NO_EVIDENCE);

    aurora::safety::SafetyMonitor active_failure(config);
    auto failed = no_evidence;
    failed.nerve_has_evidence = true;
    failed.nerve_fail_rate = 0.9;
    for (int i = 0; i < 5; ++i) active_failure.observe(failed);
    assert(active_failure.state() == aurora::safety::SafetyState::CRITICAL);

    auto recovered = failed;
    recovered.nerve_fail_rate = 0.0;
    for (int i = 0; i < 5; ++i) active_failure.observe(recovered);
    assert(active_failure.state() == aurora::safety::SafetyState::HEALTHY);
}

void safety_monitor_requires_stable_transitions() {
    aurora::safety::SafetyConfig config;
    config.window_size = 1;
    config.minimum_window_samples = 1;
    config.escalation_samples = 2;
    config.recovery_samples = 3;
    aurora::safety::SafetyMonitor monitor(config);

    aurora::safety::TelemetrySample healthy;
    healthy.duty_left = 1.0;
    healthy.nerve_has_evidence = true;
    healthy.nerve_fail_rate = 0.1;
    monitor.observe(healthy);
    assert(monitor.state() == aurora::safety::SafetyState::HEALTHY);

    auto failed = healthy;
    failed.nerve_fail_rate = 0.9;
    monitor.observe(failed);
    assert(monitor.state() == aurora::safety::SafetyState::HEALTHY);
    assert(monitor.pending_state() == aurora::safety::SafetyState::CRITICAL);
    assert(monitor.pending_observations() == 1);

    monitor.observe(healthy);
    assert(monitor.state() == aurora::safety::SafetyState::HEALTHY);
    assert(monitor.pending_observations() == 0);

    monitor.observe(failed);
    monitor.observe(failed);
    assert(monitor.state() == aurora::safety::SafetyState::CRITICAL);

    healthy.nerve_fail_rate = 0.0;
    for (int i = 0; i < 2; ++i) monitor.observe(healthy);
    assert(monitor.state() == aurora::safety::SafetyState::CRITICAL);
    monitor.observe(healthy);
    assert(monitor.state() == aurora::safety::SafetyState::DEGRADED);

    auto boundary = healthy;
    boundary.nerve_fail_rate = 0.18;
    monitor.observe(boundary);
    assert(monitor.state() == aurora::safety::SafetyState::DEGRADED);
    for (int i = 0; i < 3; ++i) monitor.observe(healthy);
    assert(monitor.state() == aurora::safety::SafetyState::HEALTHY);
}

void safety_monitor_rejects_invalid_inputs() {
    auto invalid_config = aurora::safety::SafetyConfig::default_config();
    invalid_config.recovery_samples = 0;
    bool config_rejected = false;
    try {
        (void)aurora::safety::SafetyMonitor(invalid_config);
    } catch (const std::invalid_argument&) {
        config_rejected = true;
    }
    assert(config_rejected);

    aurora::safety::SafetyMonitor monitor;
    aurora::safety::TelemetrySample invalid_sample;
    invalid_sample.duty_left = 1.1;
    bool sample_rejected = false;
    try {
        monitor.observe(invalid_sample);
    } catch (const std::invalid_argument&) {
        sample_rejected = true;
    }
    assert(sample_rejected);
}

void optimizer_treats_missing_evidence_as_conservative() {
    cl::Optimizer optimizer;
    FlowHealth apparently_healthy;
    apparently_healthy.ewma_fail_rate = 0.0;
    apparently_healthy.ewma_coverage = 1.0;

    optimizer.update_mode(
        aurora::safety::SafetyState::NO_EVIDENCE,
        apparently_healthy,
        apparently_healthy,
        apparently_healthy);
    assert(optimizer.mode() == cl::Mode::CONSERVATIVE);
}

} // namespace

int main() {
    rolling_window_discards_oldest_observation();
    simulation_duty_uses_contract_budget();
    refused_transmissions_never_reach_receiver();
    critical_only_selects_only_critical_packets();
    simulation_attempts_replay_hal_channel_energy_and_duty();
    safety_monitor_ignores_inactive_flow_classes();
    safety_monitor_requires_stable_transitions();
    safety_monitor_rejects_invalid_inputs();
    optimizer_treats_missing_evidence_as_conservative();
    std::cout << "runtime correctness tests passed\n";
    return 0;
}

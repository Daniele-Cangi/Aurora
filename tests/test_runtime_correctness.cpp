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

void safety_monitor_ignores_inactive_flow_classes() {
    aurora::safety::SafetyConfig config;
    config.window_size = 5;

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

} // namespace

int main() {
    rolling_window_discards_oldest_observation();
    simulation_duty_uses_contract_budget();
    refused_transmissions_never_reach_receiver();
    critical_only_selects_only_critical_packets();
    safety_monitor_ignores_inactive_flow_classes();
    std::cout << "runtime correctness tests passed\n";
    return 0;
}

#include "../include/aurora/telemetry/SimulationEventLedger.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

std::uint64_t next_random(std::uint64_t state) {
    state ^= state << 7U;
    state ^= state >> 9U;
    state ^= state << 8U;
    return state;
}

aurora::telemetry::SimulationEventSession session() {
    aurora::telemetry::SimulationEventSession value;
    value.experiment_seed = 71;
    value.initial_random_state = next_random(value.experiment_seed);
    value.initial_source_buffer = 0;
    value.source_energy_capacity_j = 10.0;
    value.source_initial_energy_j = 6.0;
    value.source_harvest_w = 0.2;
    value.destination_energy_capacity_j = 10.0;
    value.destination_initial_energy_j = 6.0;
    value.destination_harvest_w = 0.2;
    value.source_position = {0.06, 0.08};
    value.destination_position = {0.94, 0.92};
    value.ris_positions = {{0.4, 0.4}, {0.6, 0.6}};
    value.obstacles = {{{0.52, 0.52}, 0.22}};
    value.generation_arrival_schedule =
        aurora::simulation::GenerationArrivalSchedule({{0, "alpha"}});
    value.generations = {{
        0, "alpha", "event-ledger-generation", 0x1234ULL, 2, 3,
        aurora::transport::TransportImportance::IMPORTANT, 600'000}};
    return value;
}

aurora::telemetry::SimulationStepEvent first_event(
    const aurora::telemetry::SimulationEventSession& session) {
    aurora::telemetry::SimulationStepEvent event;
    event.active_generation_index = 0;
    event.arrived_generation_index = 0;
    event.arrived_source_packets = 3;
    event.random_before = session.initial_random_state;
    event.source_energy_before_tick_j = session.source_initial_energy_j;
    event.source_energy_after_tick_j = session.source_initial_energy_j + 0.08;
    event.source_energy_after_action_j = event.source_energy_after_tick_j;
    event.destination_energy_before_tick_j = session.destination_initial_energy_j;
    event.destination_energy_after_tick_j =
        session.destination_initial_energy_j + 0.08;
    event.destination_energy_after_action_j =
        event.destination_energy_after_tick_j;
    event.source_buffer_before = 3;
    event.source_buffer_after_action = 3;
    event.decode_status = static_cast<std::uint8_t>(
        aurora::transport::DecodeStatus::NO_PROGRESS);
    event.entropy_residual = 1.0;
    const auto environment = aurora::telemetry::derive_simulation_environment(
        session, event.random_before, event.entropy_residual);
    event.random_after_ris = environment.random_after_ris;
    event.random_after_action = environment.random_after_ris;
    event.illumination = environment.illumination;
    event.world_gain = environment.world_gain;
    event.snr_rf_db = environment.snr_rf_db;
    event.snr_optical_db = environment.snr_optical_db;
    event.snr_backscatter_db = environment.snr_backscatter_db;
    event.ris_phases = environment.ris_phases;
    event.contact_available = session.contact_schedule.availability_at(0);
    return event;
}

aurora::telemetry::SimulationStepEvent next_event(
    const aurora::telemetry::SimulationEventSession& session,
    const aurora::telemetry::SimulationStepEvent& previous) {
    aurora::telemetry::SimulationStepEvent event;
    event.step = 1;
    event.simulated_now_ms = 1'000;
    event.active_generation_index = 1;
    event.arrived_generation_index = 1;
    event.arrived_source_packets = 3;
    event.random_before = previous.random_after_action;
    event.source_energy_before_tick_j = previous.source_energy_after_action_j;
    event.source_energy_after_tick_j = event.source_energy_before_tick_j + 0.08;
    event.source_energy_after_action_j = event.source_energy_after_tick_j;
    event.destination_energy_before_tick_j =
        previous.destination_energy_after_action_j;
    event.destination_energy_after_tick_j =
        event.destination_energy_before_tick_j + 0.08;
    event.destination_energy_after_action_j =
        event.destination_energy_after_tick_j;
    event.source_buffer_before = previous.source_buffer_after_action + 3;
    event.source_buffer_after_action = event.source_buffer_before;
    event.destination_buffer_before = previous.destination_buffer_after_action;
    event.destination_inbox_before = previous.destination_inbox_after_action;
    event.destination_buffer_after_ingest = event.destination_buffer_before;
    event.destination_buffer_after_action = event.destination_buffer_before;
    event.decode_status = static_cast<std::uint8_t>(
        aurora::transport::DecodeStatus::NO_PROGRESS);
    event.entropy_residual = 1.0;
    const auto environment = aurora::telemetry::derive_simulation_environment(
        session, event.random_before, event.entropy_residual);
    event.random_after_ris = environment.random_after_ris;
    event.random_after_action = environment.random_after_ris;
    event.illumination = environment.illumination;
    event.world_gain = environment.world_gain;
    event.snr_rf_db = environment.snr_rf_db;
    event.snr_optical_db = environment.snr_optical_db;
    event.snr_backscatter_db = environment.snr_backscatter_db;
    event.ris_phases = environment.ris_phases;
    event.contact_available = session.contact_schedule.availability_at(1'000);
    return event;
}

} // namespace

int main() {
    using aurora::telemetry::SimulationEventLedger;

    const auto metadata = session();
    const auto event = first_event(metadata);
    SimulationEventLedger ledger;
    ledger.begin(metadata);
    ledger.record(event);

    const auto encoded = ledger.serialize();
    assert(encoded.starts_with("AURORA_SIMULATION_EVENT_LEDGER_V4\n"));
    assert(encoded == ledger.serialize());
    const auto restored = SimulationEventLedger::deserialize(encoded);
    assert(restored.serialize() == encoded);
    assert(restored.records().size() == 1);
    const auto structure = restored.verify_structure();
    assert(structure.ok);
    assert(structure.records_verified == 1);

    auto legacy_v3 = encoded;
    legacy_v3.replace(
        0,
        std::string("AURORA_SIMULATION_EVENT_LEDGER_V4").size(),
        "AURORA_SIMULATION_EVENT_LEDGER_V3");
    bool legacy_rejected = false;
    try {
        (void)SimulationEventLedger::deserialize(legacy_v3);
    } catch (const std::invalid_argument&) {
        legacy_rejected = true;
    }
    assert(legacy_rejected);

    auto false_environment = event;
    false_environment.ris_phases.front() ^= 1U;
    SimulationEventLedger semantic_tamper;
    semantic_tamper.begin(metadata);
    semantic_tamper.record(false_environment);
    const auto semantic_result = semantic_tamper.verify_structure();
    assert(!semantic_result.ok);
    assert(semantic_result.failure_reason.find("RIS/world transition") !=
           std::string::npos);

    auto false_contact = event;
    false_contact.contact_available.rf = false;
    SimulationEventLedger contact_tamper;
    contact_tamper.begin(metadata);
    contact_tamper.record(false_contact);
    const auto contact_result = contact_tamper.verify_structure();
    assert(!contact_result.ok);
    assert(contact_result.failure_reason.find("contact availability") !=
           std::string::npos);

    auto false_arrival = event;
    false_arrival.arrived_source_packets = 2;
    bool arrival_tamper_rejected = false;
    try {
        SimulationEventLedger arrival_tamper;
        arrival_tamper.begin(metadata);
        arrival_tamper.record(false_arrival);
    } catch (const std::invalid_argument&) {
        arrival_tamper_rejected = true;
    }
    assert(arrival_tamper_rejected);

    auto concurrent_metadata = metadata;
    concurrent_metadata.initial_random_state = next_random(
        concurrent_metadata.initial_random_state);
    concurrent_metadata.generation_arrival_schedule =
        aurora::simulation::GenerationArrivalSchedule({
            {0, "alpha"}, {1'000, "beta"}});
    concurrent_metadata.generations.push_back({
        1'000, "beta", "event-ledger-generation-beta",
        0x5678ULL, 2, 3,
        aurora::transport::TransportImportance::IMPORTANT, 601'000});
    auto concurrent_first = first_event(concurrent_metadata);
    concurrent_first.random_before = concurrent_metadata.initial_random_state;
    const auto first_environment =
        aurora::telemetry::derive_simulation_environment(
            concurrent_metadata, concurrent_first.random_before,
            concurrent_first.entropy_residual);
    concurrent_first.random_after_ris = first_environment.random_after_ris;
    concurrent_first.random_after_action = first_environment.random_after_ris;
    concurrent_first.illumination = first_environment.illumination;
    concurrent_first.world_gain = first_environment.world_gain;
    concurrent_first.snr_rf_db = first_environment.snr_rf_db;
    concurrent_first.snr_optical_db = first_environment.snr_optical_db;
    concurrent_first.snr_backscatter_db = first_environment.snr_backscatter_db;
    concurrent_first.ris_phases = first_environment.ris_phases;
    const auto wrong_active = next_event(concurrent_metadata, concurrent_first);
    SimulationEventLedger fifo_tamper;
    fifo_tamper.begin(concurrent_metadata);
    fifo_tamper.record(concurrent_first);
    fifo_tamper.record(wrong_active);
    const auto fifo_result = fifo_tamper.verify_structure();
    assert(!fifo_result.ok);
    assert(fifo_result.failure_reason.find("priority/deadline schedule") !=
           std::string::npos);

    auto corrupted = encoded;
    const auto step = corrupted.find("STEP|");
    assert(step != std::string::npos);
    const auto random_digit = corrupted.find('|', step + 5) + 1;
    assert(random_digit != 0);
    corrupted[random_digit] = corrupted[random_digit] == '0' ? '1' : '0';
    bool corruption_rejected = false;
    try {
        (void)SimulationEventLedger::deserialize(corrupted);
    } catch (const std::invalid_argument&) {
        corruption_rejected = true;
    }
    assert(corruption_rejected);

    const auto footer = encoded.find("END|");
    assert(footer != std::string::npos);
    bool truncation_rejected = false;
    try {
        (void)SimulationEventLedger::deserialize(encoded.substr(0, footer));
    } catch (const std::invalid_argument&) {
        truncation_rejected = true;
    }
    assert(truncation_rejected);

    auto discontinuous = event;
    discontinuous.step = 1;
    discontinuous.simulated_now_ms = 1'000;
    discontinuous.random_before ^= 1ULL;
    bool discontinuity_rejected = false;
    try {
        ledger.record(discontinuous);
    } catch (const std::invalid_argument&) {
        discontinuity_rejected = true;
    }
    assert(discontinuity_rejected);

    std::cout << "simulation event ledger tests passed\n";
    return 0;
}

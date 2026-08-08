#include "../aurora_organism.hpp"
#include "../include/aurora/transport/TransportHealth.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_set>
#include <vector>

using aurora::AlienFountainOrganism;
using aurora::OrganismSpawnResult;
using aurora::transport::DecodeStatus;
using aurora::transport::TransportContract;
using aurora::transport::TransportHealth;

namespace {

std::vector<std::uint8_t> payload(std::size_t size, std::uint32_t seed) {
    std::mt19937 random(seed);
    std::vector<std::uint8_t> bytes(size);
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(random() & 0xFFU);
    }
    return bytes;
}

std::vector<fec::Pkt> minimum_decodable_packets(const OrganismSpawnResult& spawn) {
    std::vector<fec::Pkt> selected;
    for (const auto& segment : spawn.descriptor.segments) {
        std::uint32_t count = 0;
        for (const auto& packet : spawn.packets) {
            if (packet.segment_id == segment.segment_id && count < segment.source_symbol_count) {
                selected.push_back(packet);
                ++count;
            }
        }
        assert(count == segment.source_symbol_count);
    }
    return selected;
}

void assert_same_packets(const OrganismSpawnResult& left, const OrganismSpawnResult& right) {
    assert(left.descriptor.generation_id == right.descriptor.generation_id);
    assert(left.descriptor.descriptor_fingerprint == right.descriptor.descriptor_fingerprint);
    assert(left.packets.size() == right.packets.size());
    for (std::size_t i = 0; i < left.packets.size(); ++i) {
        const auto& a = left.packets[i];
        const auto& b = right.packets[i];
        assert(a.fp.seed == b.fp.seed);
        assert(a.fp.deg == b.fp.deg);
        assert(a.fp.data == b.fp.data);
        assert(a.segment_id == b.segment_id);
        assert(a.descriptor_fingerprint == b.descriptor_fingerprint);
    }
}

void deterministic_generation_and_exact_length() {
    const auto contract = TransportContract::parse(
        "deadline:5s;reliability:0.99;importance:important;seed:12345");
    const auto bytes = payload(333, 7);

    AlienFountainOrganism first;
    AlienFountainOrganism replay;
    const auto generated = first.spawn(contract, "token-exact", bytes, 64, 100);
    const auto regenerated = replay.spawn(contract, "token-exact", bytes, 64, 100);
    assert_same_packets(generated, regenerated);

    auto received = minimum_decodable_packets(generated);
    std::reverse(received.begin(), received.end());
    received.insert(received.begin() + 1, received.front());
    const auto report = first.integrate(generated.descriptor.generation_id, received, 200);
    assert(report.status == DecodeStatus::COMPLETE);
    assert(report.delivered());
    assert(report.payload == bytes);
    assert(report.payload.size() == 333);
    assert(report.decoder_rank == report.required_rank);
    assert(report.duplicate_symbols == 1);
}

void concurrent_interleaved_generations() {
    const auto contract = TransportContract::parse(
        "deadline:2s;reliability:0.999;importance:important;seed:99;"
        "segment:0-63,critical,100ms,0.999");
    const auto bytes_a = payload(300, 11);
    const auto bytes_b = payload(197, 12);
    AlienFountainOrganism controller;
    const auto a = controller.spawn(contract, "token-a", bytes_a, 32, 0);
    const auto b = controller.spawn(contract, "token-b", bytes_b, 32, 0);
    assert(controller.generation_count() == 2);

    std::vector<fec::Pkt> critical_a;
    const auto critical_k = a.descriptor.segments.front().source_symbol_count;
    for (const auto& packet : a.packets) {
        if (packet.segment_id == 0 && critical_a.size() < critical_k) {
            critical_a.push_back(packet);
        }
    }
    const auto full_b = minimum_decodable_packets(b);
    std::vector<fec::Pkt> interleaved;
    const auto maximum = std::max(critical_a.size(), full_b.size());
    for (std::size_t i = 0; i < maximum; ++i) {
        if (i < critical_a.size()) interleaved.push_back(critical_a[i]);
        if (i < full_b.size()) interleaved.push_back(full_b[i]);
    }

    const auto partial_a = controller.integrate(a.descriptor.generation_id, interleaved, 50);
    assert(partial_a.status == DecodeStatus::CRITICAL_SEGMENT_COMPLETE);
    assert(partial_a.critical_complete);
    assert(!partial_a.payload_complete);
    assert(partial_a.recovered_bytes == 64);

    const auto complete_b = controller.integrate(b.descriptor.generation_id, interleaved, 50);
    assert(complete_b.delivered());
    assert(complete_b.payload == bytes_b);

    auto full_a = minimum_decodable_packets(a);
    std::reverse(full_a.begin(), full_a.end());
    const auto complete_a = controller.integrate(a.descriptor.generation_id, full_a, 500);
    assert(complete_a.delivered());
    assert(complete_a.payload == bytes_a);
}

void expiry_malformed_and_integrity_failures() {
    {
        const auto contract = TransportContract::parse("deadline:10ms;seed:1");
        AlienFountainOrganism controller;
        const auto generated = controller.spawn(contract, "expired", payload(80, 1), 32, 0);
        const auto expired = controller.integrate(generated.descriptor.generation_id, {}, 11);
        assert(expired.status == DecodeStatus::EXPIRED);

        TransportHealth health;
        health.observe(expired);
        health.observe(expired);
        assert(health.fail_count == 1);
        assert(health.success_count == 0);
    }

    {
        const auto contract = TransportContract::parse("deadline:1s;seed:2");
        AlienFountainOrganism controller;
        const auto bytes = payload(95, 2);
        const auto generated = controller.spawn(contract, "mismatch", bytes, 32, 0);
        auto wrong = generated.packets.front();
        wrong.descriptor_fingerprint ^= 1U;
        const auto malformed = controller.integrate(generated.descriptor.generation_id, {wrong}, 1);
        assert(malformed.status == DecodeStatus::MALFORMED_INPUT);
        assert(malformed.malformed_symbols == 1);

        const auto recovered = controller.integrate(
            generated.descriptor.generation_id, minimum_decodable_packets(generated), 2);
        assert(recovered.delivered());
        assert(recovered.payload == bytes);
    }

    {
        const auto contract = TransportContract::parse("deadline:1s;seed:3;integrity:on");
        AlienFountainOrganism controller;
        const auto generated = controller.spawn(contract, "corrupted", payload(81, 3), 32, 0);
        auto corrupted = minimum_decodable_packets(generated);
        corrupted.front().fp.data.front() ^= 0x40U;
        const auto failed = controller.integrate(generated.descriptor.generation_id, corrupted, 1);
        assert(failed.status == DecodeStatus::INTEGRITY_FAILURE);
        assert(failed.integrity_checked);
        assert(!failed.integrity_ok);
        assert(!failed.delivered());
    }
}

void empty_and_small_payloads() {
    const auto contract = TransportContract::parse(
        "deadline:1s;importance:critical;seed:44");
    AlienFountainOrganism controller;

    const auto empty = controller.spawn(contract, "empty", {}, 64, 0);
    assert(empty.K == 0);
    assert(empty.packets.empty());
    const auto empty_report = controller.integrate(empty.descriptor.generation_id, {}, 0);
    assert(empty_report.delivered());
    assert(empty_report.payload.empty());

    const std::vector<std::uint8_t> one{0xA5};
    const auto tiny = controller.spawn(contract, "tiny", one, 64, 0);
    assert(tiny.descriptor.segments.size() == 1);
    assert(tiny.descriptor.segments.front().source_symbol_count == 1);
    const auto tiny_report = controller.integrate(
        tiny.descriptor.generation_id, minimum_decodable_packets(tiny), 1);
    assert(tiny_report.delivered());
    assert(tiny_report.payload == one);
}

void codec_rejects_malformed_symbols_and_uses_unique_indexes() {
    const auto bytes = payload(320, 91);
    fec::Encoder encoder(bytes, 32, 999);
    const auto source_count = static_cast<std::uint32_t>(encoder.N());
    for (std::uint32_t i = 0; i < source_count; ++i) {
        (void)encoder.emit();
    }
    for (int i = 0; i < 100; ++i) {
        const auto packet = encoder.emit();
        const auto indexes = fec::detail::source_indexes(source_count, packet.seed, packet.deg);
        assert(indexes.has_value());
        std::unordered_set<std::uint32_t> unique(indexes->begin(), indexes->end());
        assert(unique.size() == indexes->size());
    }

    fec::Decoder decoder(static_cast<int>(source_count), 32);
    auto malformed = encoder.emit();
    malformed.deg = source_count + 1;
    assert(decoder.push(malformed) == fec::PushResult::MALFORMED);
    malformed = encoder.emit();
    malformed.data.pop_back();
    assert(decoder.push(malformed) == fec::PushResult::MALFORMED);
    assert(decoder.rank() == 0);
}

void health_does_not_treat_progress_as_failure() {
    const auto contract = TransportContract::parse("deadline:1s;seed:55");
    AlienFountainOrganism controller;
    const auto generated = controller.spawn(contract, "health", payload(256, 55), 32, 0);
    auto packets = minimum_decodable_packets(generated);

    TransportHealth health;
    const auto partial = controller.integrate(
        generated.descriptor.generation_id, {packets.front()}, 1);
    assert(partial.status == DecodeStatus::PARTIAL_PROGRESS);
    health.observe(partial);
    assert(health.fail_count == 0);
    assert(health.success_count == 0);

    const auto complete = controller.integrate(generated.descriptor.generation_id, packets, 2);
    health.observe(complete);
    assert(complete.delivered());
    assert(health.success_count == 1);
    assert(health.fail_count == 0);
    assert(health.last_status == DecodeStatus::COMPLETE);
}

void generation_store_is_bounded() {
    const auto contract = TransportContract::parse("deadline:1s;seed:80");
    AlienFountainOrganism controller(2);
    const auto first = controller.spawn(contract, "bounded-a", payload(64, 80), 32, 0);
    (void)controller.spawn(contract, "bounded-b", payload(64, 81), 32, 0);

    bool rejected = false;
    try {
        (void)controller.spawn(contract, "bounded-c", payload(64, 82), 32, 0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);

    const auto completed = controller.integrate(
        first.descriptor.generation_id, minimum_decodable_packets(first), 1);
    assert(completed.delivered());
    (void)controller.spawn(contract, "bounded-c", payload(64, 82), 32, 2);
    assert(controller.generation_count() == 2);
}

void generation_size_limits_are_enforced() {
    const auto contract = TransportContract::parse(
        "deadline:1s;max_generation_bytes:64;max_source_symbols:2;seed:90");
    AlienFountainOrganism controller;
    bool bytes_rejected = false;
    try {
        (void)controller.spawn(contract, "too-many-bytes", payload(65, 90), 64, 0);
    } catch (const std::invalid_argument&) {
        bytes_rejected = true;
    }
    assert(bytes_rejected);

    auto symbol_limited = contract;
    symbol_limited.maximum_generation_bytes = 1024;
    bool symbols_rejected = false;
    try {
        (void)controller.spawn(symbol_limited, "too-many-symbols", payload(65, 91), 32, 0);
    } catch (const std::invalid_argument&) {
        symbols_rejected = true;
    }
    assert(symbols_rejected);
}

void runtime_repairs_are_deterministic_segment_aware_and_bounded() {
    const auto contract = TransportContract::parse(
        "deadline:1s;seed:123;max_repair_amplification:5;"
        "segment:0-63,critical,100ms,0.999");
    const auto bytes = payload(192, 123);
    AlienFountainOrganism first;
    AlienFountainOrganism replay;
    const auto generated = first.spawn(contract, "runtime-repair", bytes, 32, 0);
    const auto regenerated = replay.spawn(contract, "runtime-repair", bytes, 32, 0);
    const auto descriptor_before = first.descriptor(generated.descriptor.generation_id);
    assert(descriptor_before.has_value());

    const auto first_repairs = first.emit_repairs(
        generated.descriptor.generation_id, 2, true);
    const auto replay_repairs = replay.emit_repairs(
        regenerated.descriptor.generation_id, 2, true);
    assert(first_repairs.emitted_symbols == 2);
    assert(first_repairs.packets.size() == 2);
    assert(replay_repairs.packets.size() == first_repairs.packets.size());

    std::unordered_set<std::uint64_t> initial_identities;
    for (const auto& packet : generated.packets) {
        initial_identities.insert(
            (static_cast<std::uint64_t>(packet.segment_id) << 32U) | packet.fp.seed);
    }
    for (std::size_t i = 0; i < first_repairs.packets.size(); ++i) {
        const auto& packet = first_repairs.packets[i];
        const auto& replay_packet = replay_repairs.packets[i];
        assert(packet.kind == fec::SegmentKind::CRITICAL);
        assert(packet.segment_id == 0);
        assert(packet.fp.seed == replay_packet.fp.seed);
        assert(packet.fp.deg == replay_packet.fp.deg);
        assert(packet.fp.data == replay_packet.fp.data);
        const auto identity =
            (static_cast<std::uint64_t>(packet.segment_id) << 32U) | packet.fp.seed;
        assert(initial_identities.insert(identity).second);
    }

    const auto descriptor_after = first.descriptor(generated.descriptor.generation_id);
    assert(descriptor_after.has_value());
    assert(descriptor_after->descriptor_fingerprint ==
           descriptor_before->descriptor_fingerprint);
    assert(descriptor_after->segments.size() == descriptor_before->segments.size());
    for (std::size_t i = 0; i < descriptor_after->segments.size(); ++i) {
        assert(descriptor_after->segments[i].coding.emitted_symbols ==
               descriptor_before->segments[i].coding.emitted_symbols);
    }

    const auto capped = first.emit_repairs(
        generated.descriptor.generation_id, 10'000, true);
    const auto exhausted = first.emit_repairs(
        generated.descriptor.generation_id, 1, true);
    assert(exhausted.emitted_symbols == 0);
    const auto runtime = first.runtime_state(generated.descriptor.generation_id);
    assert(runtime.has_value());
    const auto critical_sources = generated.descriptor.segments.front().source_symbol_count;
    assert(runtime->critical_emitted_symbols == critical_sources * 5U);
    assert(capped.emitted_symbols > 0);

    auto no_critical_contract = contract;
    no_critical_contract.segments.clear();
    no_critical_contract.importance = aurora::transport::TransportImportance::IMPORTANT;
    const auto no_critical = first.spawn(
        no_critical_contract, "no-critical-repair", bytes, 32, 0);
    assert(first.emit_repairs(
        no_critical.descriptor.generation_id, 3, true).emitted_symbols == 0);

    const auto rounding_contract = TransportContract::parse(
        "deadline:10s;reliability:0.99;seed:124;"
        "max_repair_amplification:1.5;min_critical_overhead:1.5;"
        "segment:0-0,important;segment:1-1,important");
    AlienFountainOrganism rounded;
    const auto rounded_generation = rounded.spawn(
        rounding_contract, "rounded-budget", {0x01, 0x02}, 1, 0);
    assert(rounded_generation.descriptor.total_source_symbols == 2);
    assert(rounded_generation.packets.size() == 3);
    assert(rounded.emit_repairs(
        rounded_generation.descriptor.generation_id, 1, false).emitted_symbols == 0);
}

} // namespace

int main() {
    deterministic_generation_and_exact_length();
    concurrent_interleaved_generations();
    expiry_malformed_and_integrity_failures();
    empty_and_small_payloads();
    codec_rejects_malformed_symbols_and_uses_unique_indexes();
    health_does_not_treat_progress_as_failure();
    generation_store_is_bounded();
    generation_size_limits_are_enforced();
    runtime_repairs_are_deterministic_segment_aware_and_bounded();
    std::cout << "generation lifecycle tests passed\n";
    return 0;
}

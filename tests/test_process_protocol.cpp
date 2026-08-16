#include "aurora/control/TransportPolicy.hpp"
#include "aurora/emulation/Measurement.hpp"
#include "aurora/emulation/ProcessProtocol.hpp"
#include "aurora/fec/GenerationCodec.hpp"
#include "aurora/transport/GenerationManager.hpp"
#include "aurora/transport/GenerationReceiver.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

int main() {
    using aurora::emulation::FeedbackFrame;

    const auto contract = aurora::transport::TransportContract::parse(
        "deadline:30s;reliability:0.99;duty:0.1;rf:on;optical:on;"
        "backscatter:on;ris:4;reserve:0.05;max_repair_amplification:4;"
        "min_critical_overhead:1.5;seed:701");
    std::vector<std::uint8_t> payload(300);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 29 + 7) & 0xffU);
    }

    auto codec = std::make_shared<aurora::fec::ExperimentalLtLikeCodec>();
    auto policy = std::make_shared<aurora::control::FixedTransportPolicy>(
        "emulation-test", 2.0, 2.0, 2.0);
    aurora::transport::GenerationManager sender(codec, policy);
    const auto spawned = sender.spawn(
        contract, "process-token", payload, 64, 0);

    const auto descriptor_frame =
        aurora::emulation::encode_descriptor(spawned.descriptor);
    assert(aurora::emulation::frame_type(descriptor_frame) ==
           aurora::emulation::FrameType::DESCRIPTOR);
    const auto descriptor =
        aurora::emulation::decode_descriptor(descriptor_frame);
    assert(descriptor.generation_id == spawned.descriptor.generation_id);
    assert(descriptor.descriptor_fingerprint ==
           spawned.descriptor.descriptor_fingerprint);
    assert(descriptor.segments.size() == spawned.descriptor.segments.size());

    const auto symbol_frame =
        aurora::emulation::encode_symbol(spawned.packets.front());
    const auto symbol = aurora::emulation::decode_symbol(symbol_frame);
    assert(symbol.seq == spawned.packets.front().seq);
    assert(symbol.fp.seed == spawned.packets.front().fp.seed);
    assert(symbol.fp.data == spawned.packets.front().fp.data);

    aurora::transport::GenerationReceiver receiver(descriptor, *codec, true);
    const auto first = receiver.integrate({symbol, symbol}, 1);
    assert(first.symbols_observed == 1);
    assert(first.duplicate_symbols == 1);

    std::vector<::fec::Pkt> remainder(
        spawned.packets.begin() + 1, spawned.packets.end());
    const auto remote = receiver.integrate(remainder, 2);
    const auto local = sender.integrate(
        descriptor.generation_id, spawned.packets, 2);
    assert(remote.delivered());
    assert(local.delivered());
    assert(remote.payload == payload);
    assert(remote.payload == local.payload);
    assert(remote.decoder_rank == local.decoder_rank);
    assert(remote.required_rank == local.required_rank);

    FeedbackFrame feedback{descriptor.descriptor_fingerprint, remote, 73};
    const auto feedback_frame = aurora::emulation::encode_feedback(feedback);
    assert(aurora::emulation::frame_type(feedback_frame) ==
           aurora::emulation::FrameType::FEEDBACK);
    const auto restored = aurora::emulation::decode_feedback(feedback_frame);
    assert(restored.descriptor_fingerprint ==
           descriptor.descriptor_fingerprint);
    assert(restored.echoed_forward_sequence == 73);
    assert(restored.report.delivered());
    assert(restored.report.generation_id == descriptor.generation_id);
    assert(restored.report.decoder_rank == descriptor.total_source_symbols);
    assert(restored.report.payload.empty());

    const aurora::emulation::TerminalAckFrame acknowledgement{
        descriptor.descriptor_fingerprint,
        descriptor.generation_id};
    const auto acknowledgement_frame =
        aurora::emulation::encode_terminal_ack(acknowledgement);
    assert(aurora::emulation::frame_type(acknowledgement_frame) ==
           aurora::emulation::FrameType::TERMINAL_ACK);
    const auto restored_acknowledgement =
        aurora::emulation::decode_terminal_ack(acknowledgement_frame);
    assert(restored_acknowledgement.descriptor_fingerprint ==
           descriptor.descriptor_fingerprint);
    assert(restored_acknowledgement.generation_id ==
           descriptor.generation_id);

    aurora::emulation::FeedbackRttTracker tracker;
    using namespace std::chrono_literals;
    const auto origin = aurora::emulation::FeedbackRttTracker::TimePoint{};
    assert(tracker.record_sent(73, origin));
    assert(!tracker.record_sent(73, origin + 1ms));
    assert(tracker.observe(73, false, origin + 1500us) ==
           aurora::emulation::FeedbackRttObservation::RECORDED);
    assert(tracker.observe(73, false, origin + 2ms) ==
           aurora::emulation::FeedbackRttObservation::DUPLICATE);
    assert(tracker.observe(74, false, origin + 2ms) ==
           aurora::emulation::FeedbackRttObservation::UNKNOWN_SEQUENCE);
    assert(tracker.record_sent(75, origin + 2ms));
    assert(tracker.observe(75, true, origin + 4500us) ==
           aurora::emulation::FeedbackRttObservation::RECORDED);
    const auto all_rtt = tracker.summary();
    assert(all_rtt.count == 2);
    assert(all_rtt.min_us == 1500);
    assert(all_rtt.mean_us == 2000);
    assert(all_rtt.max_us == 2500);
    const auto terminal_rtt = tracker.terminal_summary();
    assert(terminal_rtt.count == 1);
    assert(terminal_rtt.min_us == 2500);
    assert(terminal_rtt.mean_us == 2500);
    assert(terminal_rtt.max_us == 2500);

    auto corrupted = descriptor_frame;
    corrupted.back() ^= 0x80U;
    bool rejected = false;
    try {
        (void)aurora::emulation::decode_descriptor(corrupted);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    auto invalid_descriptor = descriptor;
    ++invalid_descriptor.payload_digest;
    rejected = false;
    try {
        aurora::transport::GenerationReceiver invalid(
            invalid_descriptor, *codec, true);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    std::cout << "process protocol and receiver tests passed\n";
    return 0;
}

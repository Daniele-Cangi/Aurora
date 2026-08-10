#include "aurora/control/TransportPolicy.hpp"
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

    FeedbackFrame feedback{descriptor.descriptor_fingerprint, remote};
    const auto feedback_frame = aurora::emulation::encode_feedback(feedback);
    assert(aurora::emulation::frame_type(feedback_frame) ==
           aurora::emulation::FrameType::FEEDBACK);
    const auto restored = aurora::emulation::decode_feedback(feedback_frame);
    assert(restored.descriptor_fingerprint ==
           descriptor.descriptor_fingerprint);
    assert(restored.report.delivered());
    assert(restored.report.generation_id == descriptor.generation_id);
    assert(restored.report.decoder_rank == descriptor.total_source_symbols);
    assert(restored.report.payload.empty());

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

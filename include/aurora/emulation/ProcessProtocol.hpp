#pragma once

#include "aurora/fec/GenerationCodec.hpp"
#include "aurora/transport/Generation.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace aurora::emulation {

inline constexpr std::uint16_t process_protocol_version = 3;
inline constexpr std::size_t maximum_datagram_bytes = 60'000;

enum class FrameType : std::uint8_t {
    DESCRIPTOR = 1,
    SYMBOL = 2,
    FEEDBACK = 3,
    TERMINAL_ACK = 4
};

struct FeedbackFrame {
    std::uint64_t descriptor_fingerprint = 0;
    transport::DecodeReport report;
    std::uint64_t echoed_forward_sequence = 0;
};

struct TerminalAckFrame {
    std::uint64_t descriptor_fingerprint = 0;
    std::string generation_id;
};

namespace detail {

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void u32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void size(std::size_t value) { u64(static_cast<std::uint64_t>(value)); }
    void real(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

    void string(const std::string& value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("process protocol: string is too large");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void data(const std::vector<std::uint8_t>& value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("process protocol: byte vector is too large");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1);
        return bytes_[cursor_++];
    }

    [[nodiscard]] std::uint16_t u16() {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(u8()) << 8U) | u8());
    }

    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8U) | u8();
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8U) | u8();
        return value;
    }

    [[nodiscard]] std::size_t size() {
        const auto value = u64();
        if (value > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument(
                "process protocol: size does not fit this platform");
        }
        return static_cast<std::size_t>(value);
    }

    [[nodiscard]] double real() { return std::bit_cast<double>(u64()); }

    [[nodiscard]] std::string string() {
        const auto length = u32();
        require(length);
        const auto start = bytes_.begin() +
            static_cast<std::ptrdiff_t>(cursor_);
        cursor_ += length;
        return {start, start + length};
    }

    [[nodiscard]] std::vector<std::uint8_t> data() {
        const auto length = u32();
        require(length);
        const auto start = bytes_.begin() +
            static_cast<std::ptrdiff_t>(cursor_);
        cursor_ += length;
        return {start, start + length};
    }

    void finish() const {
        if (cursor_ != bytes_.size()) {
            throw std::invalid_argument(
                "process protocol: trailing payload bytes");
        }
    }

private:
    void require(std::size_t count) const {
        if (count > bytes_.size() - cursor_) {
            throw std::invalid_argument("process protocol: truncated payload");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0;
};

struct Envelope {
    FrameType type;
    std::span<const std::uint8_t> payload;
};

inline std::vector<std::uint8_t> envelope(
    FrameType type,
    const std::vector<std::uint8_t>& payload) {
    constexpr std::size_t header_bytes = 20;
    if (payload.size() + header_bytes > maximum_datagram_bytes) {
        throw std::length_error("process protocol: datagram exceeds safe UDP size");
    }
    Writer writer;
    writer.u8('A');
    writer.u8('U');
    writer.u8('X');
    writer.u8('E');
    writer.u16(process_protocol_version);
    writer.u8(static_cast<std::uint8_t>(type));
    writer.u8(0);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u64(transport::fnv1a64(payload));
    auto encoded = writer.take();
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

inline Envelope open(std::span<const std::uint8_t> encoded) {
    constexpr std::size_t header_bytes = 20;
    if (encoded.size() < header_bytes || encoded.size() > maximum_datagram_bytes) {
        throw std::invalid_argument("process protocol: invalid datagram size");
    }
    Reader reader(encoded.first(header_bytes));
    if (reader.u8() != 'A' || reader.u8() != 'U' || reader.u8() != 'X' ||
        reader.u8() != 'E') {
        throw std::invalid_argument("process protocol: invalid magic");
    }
    if (reader.u16() != process_protocol_version) {
        throw std::invalid_argument("process protocol: unsupported version");
    }
    const auto raw_type = reader.u8();
    if (raw_type < static_cast<std::uint8_t>(FrameType::DESCRIPTOR) ||
        raw_type > static_cast<std::uint8_t>(FrameType::TERMINAL_ACK)) {
        throw std::invalid_argument("process protocol: unknown frame type");
    }
    if (reader.u8() != 0) {
        throw std::invalid_argument("process protocol: reserved flags are nonzero");
    }
    const auto payload_size = reader.u32();
    const auto checksum = reader.u64();
    reader.finish();
    if (payload_size != encoded.size() - header_bytes) {
        throw std::invalid_argument("process protocol: payload length mismatch");
    }
    const auto payload = encoded.subspan(header_bytes);
    if (transport::fnv1a64(payload.data(), payload.size()) != checksum) {
        throw std::invalid_argument("process protocol: payload checksum mismatch");
    }
    return {static_cast<FrameType>(raw_type), payload};
}

inline void require_type(const Envelope& envelope_value, FrameType expected) {
    if (envelope_value.type != expected) {
        throw std::invalid_argument("process protocol: unexpected frame type");
    }
}

} // namespace detail

inline FrameType frame_type(std::span<const std::uint8_t> encoded) {
    return detail::open(encoded).type;
}

inline std::vector<std::uint8_t> encode_descriptor(
    const transport::GenerationDescriptor& descriptor) {
    if (const auto error = descriptor.validation_error()) {
        throw std::invalid_argument(
            "process protocol: invalid descriptor: " + *error);
    }
    if (transport::compute_descriptor_fingerprint(descriptor) !=
        descriptor.descriptor_fingerprint) {
        throw std::invalid_argument(
            "process protocol: descriptor fingerprint mismatch");
    }
    detail::Writer writer;
    writer.u16(descriptor.protocol_version);
    writer.string(descriptor.generation_id);
    writer.string(descriptor.token_id);
    writer.string(descriptor.codec_id);
    writer.u16(descriptor.codec_version);
    writer.string(descriptor.policy_id);
    writer.u16(descriptor.policy_version);
    writer.size(descriptor.original_payload_length);
    writer.size(descriptor.symbol_size);
    writer.u32(descriptor.total_source_symbols);
    writer.u32(static_cast<std::uint32_t>(descriptor.segments.size()));
    for (const auto& segment : descriptor.segments) {
        writer.u32(segment.segment_id);
        writer.size(segment.offset);
        writer.size(segment.length);
        writer.u32(segment.source_symbol_count);
        writer.u8(static_cast<std::uint8_t>(segment.importance));
        writer.real(segment.target_reliability);
        writer.u64(segment.deadline_ms);
        writer.u64(segment.expires_at_ms);
        writer.u64(segment.coding.seed);
        writer.real(segment.coding.overhead_factor);
        writer.u32(segment.coding.emitted_symbols);
    }
    writer.u64(descriptor.created_at_ms);
    writer.u64(descriptor.expires_at_ms);
    writer.string(descriptor.integrity_id);
    writer.u64(descriptor.payload_digest);
    writer.u64(descriptor.descriptor_fingerprint);
    return detail::envelope(FrameType::DESCRIPTOR, writer.take());
}

inline transport::GenerationDescriptor decode_descriptor(
    std::span<const std::uint8_t> encoded) {
    const auto envelope_value = detail::open(encoded);
    detail::require_type(envelope_value, FrameType::DESCRIPTOR);
    detail::Reader reader(envelope_value.payload);
    transport::GenerationDescriptor descriptor;
    descriptor.protocol_version = reader.u16();
    descriptor.generation_id = reader.string();
    descriptor.token_id = reader.string();
    descriptor.codec_id = reader.string();
    descriptor.codec_version = reader.u16();
    descriptor.policy_id = reader.string();
    descriptor.policy_version = reader.u16();
    descriptor.original_payload_length = reader.size();
    descriptor.symbol_size = reader.size();
    descriptor.total_source_symbols = reader.u32();
    const auto segment_count = reader.u32();
    if (segment_count > 4096) {
        throw std::invalid_argument("process protocol: too many segments");
    }
    descriptor.segments.reserve(segment_count);
    for (std::uint32_t i = 0; i < segment_count; ++i) {
        transport::GenerationSegmentDescriptor segment;
        segment.segment_id = reader.u32();
        segment.offset = reader.size();
        segment.length = reader.size();
        segment.source_symbol_count = reader.u32();
        const auto importance = reader.u8();
        if (importance > static_cast<std::uint8_t>(
                transport::TransportImportance::ELASTIC)) {
            throw std::invalid_argument("process protocol: invalid importance");
        }
        segment.importance =
            static_cast<transport::TransportImportance>(importance);
        segment.target_reliability = reader.real();
        segment.deadline_ms = reader.u64();
        segment.expires_at_ms = reader.u64();
        segment.coding.seed = reader.u64();
        segment.coding.overhead_factor = reader.real();
        segment.coding.emitted_symbols = reader.u32();
        descriptor.segments.push_back(segment);
    }
    descriptor.created_at_ms = reader.u64();
    descriptor.expires_at_ms = reader.u64();
    descriptor.integrity_id = reader.string();
    descriptor.payload_digest = reader.u64();
    descriptor.descriptor_fingerprint = reader.u64();
    reader.finish();
    if (const auto error = descriptor.validation_error()) {
        throw std::invalid_argument(
            "process protocol: decoded descriptor is invalid: " + *error);
    }
    if (transport::compute_descriptor_fingerprint(descriptor) !=
        descriptor.descriptor_fingerprint) {
        throw std::invalid_argument(
            "process protocol: decoded descriptor fingerprint mismatch");
    }
    return descriptor;
}

inline std::vector<std::uint8_t> encode_symbol(const ::fec::Pkt& packet) {
    detail::Writer writer;
    writer.u32(packet.fp.seed);
    writer.u32(packet.fp.deg);
    writer.data(packet.fp.data);
    writer.u32(packet.seq);
    writer.string(packet.token_id);
    writer.u8(static_cast<std::uint8_t>(packet.kind));
    writer.string(packet.generation_id);
    writer.u32(packet.segment_id);
    writer.u64(packet.descriptor_fingerprint);
    return detail::envelope(FrameType::SYMBOL, writer.take());
}

inline ::fec::Pkt decode_symbol(std::span<const std::uint8_t> encoded) {
    const auto envelope_value = detail::open(encoded);
    detail::require_type(envelope_value, FrameType::SYMBOL);
    detail::Reader reader(envelope_value.payload);
    ::fec::Pkt packet;
    packet.fp.seed = reader.u32();
    packet.fp.deg = reader.u32();
    packet.fp.data = reader.data();
    packet.seq = reader.u32();
    packet.token_id = reader.string();
    const auto kind = reader.u8();
    if (kind > static_cast<std::uint8_t>(::fec::SegmentKind::BULK)) {
        throw std::invalid_argument("process protocol: invalid segment kind");
    }
    packet.kind = static_cast<::fec::SegmentKind>(kind);
    packet.generation_id = reader.string();
    packet.segment_id = reader.u32();
    packet.descriptor_fingerprint = reader.u64();
    reader.finish();
    return packet;
}

inline std::vector<std::uint8_t> encode_feedback(
    const FeedbackFrame& feedback) {
    const auto& report = feedback.report;
    detail::Writer writer;
    writer.u64(feedback.descriptor_fingerprint);
    writer.u64(feedback.echoed_forward_sequence);
    writer.string(report.generation_id);
    writer.u8(static_cast<std::uint8_t>(report.status));
    writer.size(report.source_bytes);
    writer.size(report.recovered_bytes);
    writer.u32(report.symbols_observed);
    writer.u32(report.innovative_symbols);
    writer.u32(report.dependent_symbols);
    writer.u32(report.duplicate_symbols);
    writer.u32(report.malformed_symbols);
    writer.u32(report.late_symbols);
    writer.u32(report.expired_segments);
    writer.u32(report.decoder_rank);
    writer.u32(report.required_rank);
    writer.real(report.coverage);
    std::uint8_t flags = 0;
    if (report.critical_complete) flags |= 1U;
    if (report.payload_complete) flags |= 2U;
    if (report.integrity_checked) flags |= 4U;
    if (report.integrity_ok) flags |= 8U;
    writer.u8(flags);
    writer.string(report.failure_reason);
    return detail::envelope(FrameType::FEEDBACK, writer.take());
}

inline FeedbackFrame decode_feedback(std::span<const std::uint8_t> encoded) {
    const auto envelope_value = detail::open(encoded);
    detail::require_type(envelope_value, FrameType::FEEDBACK);
    detail::Reader reader(envelope_value.payload);
    FeedbackFrame feedback;
    feedback.descriptor_fingerprint = reader.u64();
    feedback.echoed_forward_sequence = reader.u64();
    auto& report = feedback.report;
    report.generation_id = reader.string();
    const auto status = reader.u8();
    if (status > static_cast<std::uint8_t>(
            transport::DecodeStatus::INSUFFICIENT_RANK)) {
        throw std::invalid_argument("process protocol: invalid decode status");
    }
    report.status = static_cast<transport::DecodeStatus>(status);
    report.source_bytes = reader.size();
    report.recovered_bytes = reader.size();
    report.symbols_observed = reader.u32();
    report.innovative_symbols = reader.u32();
    report.dependent_symbols = reader.u32();
    report.duplicate_symbols = reader.u32();
    report.malformed_symbols = reader.u32();
    report.late_symbols = reader.u32();
    report.expired_segments = reader.u32();
    report.decoder_rank = reader.u32();
    report.required_rank = reader.u32();
    report.coverage = reader.real();
    const auto flags = reader.u8();
    if ((flags & 0xf0U) != 0) {
        throw std::invalid_argument("process protocol: invalid feedback flags");
    }
    report.critical_complete = (flags & 1U) != 0;
    report.payload_complete = (flags & 2U) != 0;
    report.integrity_checked = (flags & 4U) != 0;
    report.integrity_ok = (flags & 8U) != 0;
    report.failure_reason = reader.string();
    reader.finish();
    if (report.decoder_rank > report.required_rank ||
        report.recovered_bytes > report.source_bytes ||
        !std::isfinite(report.coverage) || report.coverage < 0.0 ||
        report.coverage > 1.0) {
        throw std::invalid_argument("process protocol: invalid feedback summary");
    }
    return feedback;
}

inline std::vector<std::uint8_t> encode_terminal_ack(
    const TerminalAckFrame& acknowledgement) {
    if (acknowledgement.generation_id.empty()) {
        throw std::invalid_argument(
            "process protocol: terminal acknowledgement has no generation");
    }
    detail::Writer writer;
    writer.u64(acknowledgement.descriptor_fingerprint);
    writer.string(acknowledgement.generation_id);
    return detail::envelope(FrameType::TERMINAL_ACK, writer.take());
}

inline TerminalAckFrame decode_terminal_ack(
    std::span<const std::uint8_t> encoded) {
    const auto envelope_value = detail::open(encoded);
    detail::require_type(envelope_value, FrameType::TERMINAL_ACK);
    detail::Reader reader(envelope_value.payload);
    TerminalAckFrame acknowledgement;
    acknowledgement.descriptor_fingerprint = reader.u64();
    acknowledgement.generation_id = reader.string();
    reader.finish();
    if (acknowledgement.generation_id.empty()) {
        throw std::invalid_argument(
            "process protocol: terminal acknowledgement has no generation");
    }
    return acknowledgement;
}

} // namespace aurora::emulation

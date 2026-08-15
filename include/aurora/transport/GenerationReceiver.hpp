#pragma once

#include "Generation.hpp"
#include "aurora/fec/GenerationCodec.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aurora::transport {

// Receiver-only generation state. It is deliberately constructed from the
// authenticated/trusted descriptor rather than from the source payload, so it
// can live in a different process from the encoder.
class GenerationReceiver {
public:
    GenerationReceiver(GenerationDescriptor descriptor,
                       const aurora::fec::GenerationCodec& codec,
                       bool require_payload_integrity = true,
                       std::optional<std::uint64_t> receiver_deadline_origin_ms =
                           std::nullopt)
        : descriptor_(std::move(descriptor)),
          require_payload_integrity_(require_payload_integrity),
          deadline_origin_ms_(receiver_deadline_origin_ms.value_or(
              descriptor_.created_at_ms)) {
        if (const auto error = descriptor_.validation_error()) {
            throw std::invalid_argument(
                "generation receiver: invalid descriptor: " + *error);
        }
        if (compute_descriptor_fingerprint(descriptor_) !=
            descriptor_.descriptor_fingerprint) {
            throw std::invalid_argument(
                "generation receiver: descriptor fingerprint mismatch");
        }
        if (descriptor_.codec_id != codec.id() ||
            descriptor_.codec_version != codec.version()) {
            throw std::invalid_argument(
                "generation receiver: codec identity does not match descriptor");
        }
        for (const auto& segment : descriptor_.segments) {
            segments_.emplace_back(
                segment, codec, descriptor_.symbol_size,
                saturating_add(deadline_origin_ms_, segment.deadline_ms));
        }
    }

    [[nodiscard]] const GenerationDescriptor& descriptor() const {
        return descriptor_;
    }

    [[nodiscard]] std::uint64_t deadline_origin_ms() const {
        return deadline_origin_ms_;
    }

    [[nodiscard]] std::uint64_t generation_deadline_duration_ms() const {
        return descriptor_.expires_at_ms - descriptor_.created_at_ms;
    }

    [[nodiscard]] std::uint64_t generation_expires_at_ms() const {
        return saturating_add(
            deadline_origin_ms_, generation_deadline_duration_ms());
    }

    [[nodiscard]] std::uint64_t segment_expires_at_ms(
        std::uint32_t segment_id) const {
        if (segment_id >= segments_.size()) {
            throw std::out_of_range("generation receiver: invalid segment id");
        }
        return segments_[segment_id].expires_at_ms;
    }

    DecodeReport integrate(const std::vector<::fec::Pkt>& received_packets,
                           std::uint64_t now_ms) {
        const auto started = std::chrono::steady_clock::now();
        if (terminal_report_) return *terminal_report_;

        auto report = make_report();
        if (now_ms > generation_expires_at_ms()) {
            report.status = DecodeStatus::EXPIRED;
            report.failure_reason = "generation expired before full recovery";
            terminal_report_ = finish_timing(std::move(report), started);
            return *terminal_report_;
        }

        expire_segments(now_ms);
        bool saw_matching_packet = false;
        bool rank_increased = false;
        for (const auto& packet : received_packets) {
            if (packet.generation_id != descriptor_.generation_id) continue;
            saw_matching_packet = true;
            if (packet.token_id != descriptor_.token_id ||
                packet.descriptor_fingerprint !=
                    descriptor_.descriptor_fingerprint ||
                packet.segment_id >= segments_.size()) {
                ++malformed_symbols_;
                continue;
            }
            auto& segment = segments_[packet.segment_id];
            const auto expected_kind = segment.descriptor.importance ==
                    TransportImportance::CRITICAL
                ? ::fec::SegmentKind::CRITICAL
                : ::fec::SegmentKind::BULK;
            if (packet.kind != expected_kind) {
                ++malformed_symbols_;
                continue;
            }
            if (segment.expired) {
                ++late_symbols_;
                continue;
            }
            const PacketKey key{packet.segment_id, packet.fp.seed};
            if (!seen_packets_.insert(key).second) {
                ++duplicate_symbols_;
                continue;
            }
            ++symbols_observed_;
            switch (segment.decoder->push(packet.fp)) {
                case ::fec::PushResult::INNOVATIVE:
                    ++innovative_symbols_;
                    rank_increased = true;
                    break;
                case ::fec::PushResult::DEPENDENT:
                    ++dependent_symbols_;
                    break;
                case ::fec::PushResult::MALFORMED:
                case ::fec::PushResult::INCONSISTENT:
                    ++malformed_symbols_;
                    break;
            }
        }

        recover_complete_segments();
        report = make_report();
        if (all_segments_complete()) {
            report.payload.assign(descriptor_.original_payload_length, 0);
            for (const auto& segment : segments_) {
                std::copy(
                    segment.recovered->begin(), segment.recovered->end(),
                    report.payload.begin() +
                        static_cast<std::ptrdiff_t>(segment.descriptor.offset));
            }
            report.payload_complete = true;
            report.coverage = 1.0;
            report.recovered_bytes = report.source_bytes;
            report.integrity_checked = require_payload_integrity_;
            report.integrity_ok = !report.integrity_checked ||
                fnv1a64(report.payload) == descriptor_.payload_digest;
            if (!report.integrity_ok) {
                report.payload_complete = false;
                report.payload.clear();
                report.status = DecodeStatus::INTEGRITY_FAILURE;
                report.failure_reason =
                    "decoded payload failed generation integrity check";
            } else {
                report.status = DecodeStatus::COMPLETE;
            }
            terminal_report_ = finish_timing(std::move(report), started);
            return *terminal_report_;
        }

        if (malformed_symbols_ > 0) {
            report.status = DecodeStatus::MALFORMED_INPUT;
            report.failure_reason =
                "one or more symbols did not match the generation descriptor";
        } else if (report.expired_segments > 0) {
            report.status = DecodeStatus::SEGMENT_EXPIRED;
            report.failure_reason =
                "one or more segments expired before exact recovery";
        } else if (report.critical_complete) {
            report.status = DecodeStatus::CRITICAL_SEGMENT_COMPLETE;
        } else if (rank_increased) {
            report.status = DecodeStatus::PARTIAL_PROGRESS;
        } else if (saw_matching_packet) {
            report.status = DecodeStatus::INSUFFICIENT_RANK;
            report.failure_reason =
                "new observations did not increase decoder rank";
        } else {
            report.status = DecodeStatus::NO_PROGRESS;
        }
        return finish_timing(std::move(report), started);
    }

private:
    struct SegmentState {
        GenerationSegmentDescriptor descriptor;
        std::uint64_t expires_at_ms = 0;
        std::unique_ptr<aurora::fec::SymbolDecoder> decoder;
        std::optional<std::vector<std::uint8_t>> recovered;
        bool expired = false;

        SegmentState(const GenerationSegmentDescriptor& value,
                     const aurora::fec::GenerationCodec& codec,
                     std::size_t symbol_size,
                     std::uint64_t local_expires_at_ms)
            : descriptor(value),
              expires_at_ms(local_expires_at_ms),
              decoder(codec.make_decoder(
                  value.source_symbol_count, symbol_size, value.length)) {}

        SegmentState(SegmentState&&) noexcept = default;
        SegmentState& operator=(SegmentState&&) noexcept = default;
        SegmentState(const SegmentState&) = delete;
        SegmentState& operator=(const SegmentState&) = delete;
    };

    struct PacketKey {
        std::uint32_t segment_id = 0;
        std::uint32_t seed = 0;
        bool operator==(const PacketKey&) const = default;
    };

    struct PacketKeyHash {
        std::size_t operator()(const PacketKey& key) const {
            const auto combined =
                (static_cast<std::uint64_t>(key.segment_id) << 32U) |
                static_cast<std::uint64_t>(key.seed);
            return std::hash<std::uint64_t>{}(combined);
        }
    };

    [[nodiscard]] bool all_segments_complete() const {
        return std::all_of(
            segments_.begin(), segments_.end(),
            [](const auto& segment) { return segment.recovered.has_value(); });
    }

    void recover_complete_segments() {
        for (auto& segment : segments_) {
            if (segment.expired || segment.recovered ||
                segment.decoder->rank() !=
                    static_cast<int>(segment.descriptor.source_symbol_count)) {
                continue;
            }
            auto [ok, bytes] = segment.decoder->solve();
            if (!ok) continue;
            bytes.resize(segment.descriptor.length);
            segment.recovered = std::move(bytes);
        }
    }

    void expire_segments(std::uint64_t now_ms) {
        for (auto& segment : segments_) {
            if (!segment.recovered &&
                now_ms > segment.expires_at_ms) {
                segment.expired = true;
            }
        }
    }

    [[nodiscard]] DecodeReport make_report() const {
        DecodeReport report;
        report.generation_id = descriptor_.generation_id;
        report.source_bytes = descriptor_.original_payload_length;
        report.required_rank = descriptor_.total_source_symbols;
        report.symbols_observed = symbols_observed_;
        report.innovative_symbols = innovative_symbols_;
        report.dependent_symbols = dependent_symbols_;
        report.duplicate_symbols = duplicate_symbols_;
        report.malformed_symbols = malformed_symbols_;
        report.late_symbols = late_symbols_;

        bool has_critical = false;
        bool all_critical = true;
        for (const auto& segment : segments_) {
            const auto rank =
                static_cast<std::uint32_t>(segment.decoder->rank());
            report.decoder_rank += rank;
            const auto status = segment.recovered
                ? SegmentDecodeStatus::COMPLETE
                : segment.expired ? SegmentDecodeStatus::EXPIRED
                                  : SegmentDecodeStatus::PENDING;
            report.segment_reports.push_back({
                segment.descriptor.segment_id,
                segment.descriptor.importance,
                status,
                rank,
                segment.descriptor.source_symbol_count,
                segment.recovered ? segment.descriptor.length : 0,
                segment.expires_at_ms,
                segment.descriptor.target_reliability,
                segment.recovered.has_value()});
            if (segment.expired) ++report.expired_segments;
            if (segment.descriptor.importance ==
                TransportImportance::CRITICAL) {
                has_critical = true;
                all_critical = all_critical && segment.recovered &&
                    !segment.expired;
            }
            if (segment.recovered) {
                report.recovered_bytes += segment.descriptor.length;
                report.recovered_segments.push_back({
                    segment.descriptor.segment_id,
                    segment.descriptor.offset,
                    *segment.recovered});
            }
        }
        report.critical_complete = has_critical && all_critical;
        report.payload_complete = all_segments_complete();
        report.coverage = report.source_bytes == 0
            ? 1.0
            : static_cast<double>(report.recovered_bytes) /
                static_cast<double>(report.source_bytes);
        return report;
    }

    static DecodeReport finish_timing(
        DecodeReport report,
        const std::chrono::steady_clock::time_point& started) {
        report.decode_time_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        return report;
    }

    static std::uint64_t saturating_add(std::uint64_t left,
                                        std::uint64_t right) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return left + right;
    }

    GenerationDescriptor descriptor_;
    bool require_payload_integrity_ = true;
    std::uint64_t deadline_origin_ms_ = 0;
    std::vector<SegmentState> segments_;
    std::unordered_set<PacketKey, PacketKeyHash> seen_packets_;
    std::uint32_t symbols_observed_ = 0;
    std::uint32_t innovative_symbols_ = 0;
    std::uint32_t dependent_symbols_ = 0;
    std::uint32_t duplicate_symbols_ = 0;
    std::uint32_t malformed_symbols_ = 0;
    std::uint32_t late_symbols_ = 0;
    std::optional<DecodeReport> terminal_report_;
};

} // namespace aurora::transport

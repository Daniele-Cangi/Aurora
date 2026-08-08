#pragma once

#include "TransportContract.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace aurora::transport {

inline std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::uint64_t fnv1a64(const std::vector<std::uint8_t>& data) {
    return fnv1a64(data.data(), data.size());
}

inline std::uint64_t fnv1a64(const std::string& data) {
    return fnv1a64(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

struct CodingParameters {
    std::uint64_t seed = 0;
    double overhead_factor = 1.0;
    std::uint32_t emitted_symbols = 0;
};

struct GenerationSegmentDescriptor {
    std::uint32_t segment_id = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
    std::uint32_t source_symbol_count = 0;
    TransportImportance importance = TransportImportance::IMPORTANT;
    double target_reliability = 0.99;
    std::uint64_t deadline_ms = 0;
    CodingParameters coding;
};

struct GenerationDescriptor {
    std::uint16_t protocol_version = 1;
    std::string generation_id;
    std::string token_id;
    std::string codec_id = "experimental-lt-like";
    std::uint16_t codec_version = 1;
    std::string policy_id = "unspecified";
    std::uint16_t policy_version = 1;
    std::size_t original_payload_length = 0;
    std::size_t symbol_size = 0;
    std::uint32_t total_source_symbols = 0;
    std::vector<GenerationSegmentDescriptor> segments;
    std::uint64_t created_at_ms = 0;
    std::uint64_t expires_at_ms = 0;
    std::string integrity_id = "fnv1a64-research";
    std::uint64_t payload_digest = 0;
    std::uint64_t descriptor_fingerprint = 0;

    [[nodiscard]] std::optional<std::string> validation_error() const {
        if (protocol_version != 1) {
            return "unsupported protocol version";
        }
        if (generation_id.empty() || token_id.empty()) {
            return "generation and token identifiers are required";
        }
        if (codec_id.empty() || codec_version == 0) {
            return "codec identity and version are required";
        }
        if (policy_id.empty() || policy_version == 0) {
            return "transport policy identity and version are required";
        }
        if (symbol_size == 0) {
            return "symbol size must be positive";
        }
        if (expires_at_ms < created_at_ms) {
            return "expiry precedes creation time";
        }

        std::size_t previous_end = 0;
        std::uint32_t source_total = 0;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const auto& segment = segments[i];
            if (segment.segment_id != i) {
                return "segment identifiers must be contiguous and ordered";
            }
            if (segment.length == 0) {
                return "empty segments are not encoded";
            }
            if (segment.offset != previous_end) {
                return "segments must provide contiguous payload coverage";
            }
            if (segment.offset + segment.length > original_payload_length) {
                return "segment exceeds payload length";
            }
            const auto expected_symbols = static_cast<std::uint32_t>(
                (segment.length + symbol_size - 1) / symbol_size);
            if (segment.source_symbol_count != expected_symbols) {
                return "segment source symbol count is inconsistent";
            }
            if (segment.coding.emitted_symbols < segment.source_symbol_count) {
                return "segment emits fewer symbols than its source count";
            }
            previous_end = segment.offset + segment.length;
            source_total += segment.source_symbol_count;
        }
        if (previous_end != original_payload_length) {
            return "segments do not cover the exact payload length";
        }
        if (source_total != total_source_symbols) {
            return "total source symbol count is inconsistent";
        }
        if (original_payload_length == 0 && (!segments.empty() || total_source_symbols != 0)) {
            return "empty payload descriptor contains coding segments";
        }
        return std::nullopt;
    }
};

inline std::uint64_t compute_descriptor_fingerprint(const GenerationDescriptor& descriptor) {
    std::ostringstream encoded;
    encoded.precision(17);
    encoded << descriptor.protocol_version << '|'
            << descriptor.generation_id << '|'
            << descriptor.token_id << '|'
            << descriptor.codec_id << '|'
            << descriptor.codec_version << '|'
            << descriptor.policy_id << '|'
            << descriptor.policy_version << '|'
            << descriptor.original_payload_length << '|'
            << descriptor.symbol_size << '|'
            << descriptor.total_source_symbols << '|'
            << descriptor.created_at_ms << '|'
            << descriptor.expires_at_ms << '|'
            << descriptor.integrity_id << '|'
            << descriptor.payload_digest;
    for (const auto& segment : descriptor.segments) {
        encoded << '|'
                << segment.segment_id << ','
                << segment.offset << ','
                << segment.length << ','
                << segment.source_symbol_count << ','
                << static_cast<int>(segment.importance) << ','
                << segment.target_reliability << ','
                << segment.deadline_ms << ','
                << segment.coding.seed << ','
                << segment.coding.overhead_factor << ','
                << segment.coding.emitted_symbols;
    }
    return fnv1a64(encoded.str());
}

enum class DecodeStatus : std::uint8_t {
    NO_PROGRESS,
    PARTIAL_PROGRESS,
    CRITICAL_SEGMENT_COMPLETE,
    COMPLETE,
    EXPIRED,
    INTEGRITY_FAILURE,
    MALFORMED_INPUT,
    INSUFFICIENT_RANK
};

struct RecoveredSegment {
    std::uint32_t segment_id = 0;
    std::size_t offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct DecodeReport {
    std::string generation_id;
    DecodeStatus status = DecodeStatus::NO_PROGRESS;
    std::size_t source_bytes = 0;
    std::size_t recovered_bytes = 0;
    std::uint32_t symbols_observed = 0;
    std::uint32_t innovative_symbols = 0;
    std::uint32_t dependent_symbols = 0;
    std::uint32_t duplicate_symbols = 0;
    std::uint32_t malformed_symbols = 0;
    std::uint32_t decoder_rank = 0;
    std::uint32_t required_rank = 0;
    double coverage = 0.0;
    bool critical_complete = false;
    bool payload_complete = false;
    bool integrity_checked = false;
    bool integrity_ok = false;
    std::uint64_t decode_time_us = 0;
    std::string failure_reason;
    std::vector<RecoveredSegment> recovered_segments;
    std::vector<std::uint8_t> payload;

    [[nodiscard]] bool delivered() const {
        return status == DecodeStatus::COMPLETE && payload_complete &&
               (!integrity_checked || integrity_ok);
    }

    [[nodiscard]] bool terminal_failure() const {
        return status == DecodeStatus::EXPIRED || status == DecodeStatus::INTEGRITY_FAILURE;
    }
};

} // namespace aurora::transport

#pragma once

#include "../control/TransportPolicy.hpp"
#include "../fec/GenerationCodec.hpp"
#include "Generation.hpp"
#include "TransportContract.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aurora::transport {

struct GenerationSpawnResult {
    GenerationDescriptor descriptor;
    std::vector<::fec::Pkt> packets;
    int source_symbol_count = 0;
    // Compatibility alias for the historical simulator API.
    int K = 0;
    std::size_t payload_size = 0;
};

// Owns protocol/generation state only. Coding and adaptation are injected strategies.
class GenerationManager {
public:
    GenerationManager(std::shared_ptr<aurora::fec::GenerationCodec> codec,
                      std::shared_ptr<control::TransportPolicy> policy,
                      std::size_t maximum_active_generations = 128)
        : codec_(std::move(codec)),
          policy_(std::move(policy)),
          maximum_active_generations_(maximum_active_generations) {
        if (!codec_ || !policy_) {
            throw std::invalid_argument("generation manager: codec and policy are required");
        }
        if (maximum_active_generations_ == 0) {
            throw std::invalid_argument("generation manager: capacity must be positive");
        }
    }

    [[nodiscard]] control::FlowProfile build_profile(const TransportContract& contract) const {
        return policy_->profile_for(contract);
    }

    GenerationSpawnResult spawn(const TransportContract& contract,
                                const std::string& token_id,
                                const std::vector<std::uint8_t>& payload_bytes,
                                std::size_t symbol_size = 128,
                                std::uint64_t now_ms = 0) {
        contract.validate();
        if (token_id.empty()) {
            throw std::invalid_argument("generation spawn: token id is required");
        }
        if (symbol_size == 0) {
            throw std::invalid_argument("generation spawn: symbol size must be positive");
        }
        if (payload_bytes.size() > contract.maximum_generation_bytes) {
            throw std::invalid_argument("generation spawn: payload exceeds contract generation-size limit");
        }

        const auto protection = policy_->plan(contract);
        const auto requirements = compile_segments(contract, payload_bytes.size());

        GenerationSpawnResult result;
        result.payload_size = payload_bytes.size();
        auto& descriptor = result.descriptor;
        descriptor.token_id = token_id;
        descriptor.codec_id = codec_->id();
        descriptor.codec_version = codec_->version();
        descriptor.policy_id = protection.policy_id;
        descriptor.policy_version = protection.policy_version;
        descriptor.original_payload_length = payload_bytes.size();
        descriptor.symbol_size = symbol_size;
        descriptor.created_at_ms = now_ms;
        descriptor.expires_at_ms = saturating_add(now_ms, contract.deadline_ms());
        descriptor.payload_digest = fnv1a64(payload_bytes);
        descriptor.generation_id = make_generation_id(
            token_id, descriptor.payload_digest, contract.experiment_seed, generation_counter_++);

        std::uint32_t packet_sequence = 1;
        for (std::size_t index = 0; index < requirements.size(); ++index) {
            const auto& requirement = requirements[index];
            GenerationSegmentDescriptor segment;
            segment.segment_id = static_cast<std::uint32_t>(index);
            segment.offset = requirement.offset;
            segment.length = requirement.length;
            segment.source_symbol_count = static_cast<std::uint32_t>(
                (requirement.length + symbol_size - 1) / symbol_size);
            segment.importance = requirement.importance;
            segment.target_reliability = requirement.target_reliability;
            segment.deadline_ms = requirement.deadline_ms == 0
                ? contract.deadline_ms()
                : requirement.deadline_ms;
            segment.coding.seed = segment_seed(
                contract.experiment_seed, descriptor.generation_id, index);
            segment.coding.overhead_factor = std::clamp(
                protection.overhead_for(requirement.importance),
                1.0, contract.maximum_repair_amplification);
            segment.coding.emitted_symbols = static_cast<std::uint32_t>(std::ceil(
                static_cast<double>(segment.source_symbol_count) * segment.coding.overhead_factor));
            segment.coding.emitted_symbols = std::max(
                segment.coding.emitted_symbols, segment.source_symbol_count);
            descriptor.total_source_symbols += segment.source_symbol_count;
            if (descriptor.total_source_symbols > contract.maximum_source_symbols) {
                throw std::invalid_argument("generation spawn: source symbol count exceeds contract limit");
            }
            descriptor.segments.push_back(segment);
        }

        descriptor.descriptor_fingerprint = compute_descriptor_fingerprint(descriptor);
        if (const auto error = descriptor.validation_error()) {
            throw std::logic_error("generation spawn produced invalid descriptor: " + *error);
        }

        for (const auto& segment : descriptor.segments) {
            const auto begin = payload_bytes.begin() + static_cast<std::ptrdiff_t>(segment.offset);
            const auto end = begin + static_cast<std::ptrdiff_t>(segment.length);
            const std::vector<std::uint8_t> bytes(begin, end);
            auto encoder = codec_->make_encoder(bytes, symbol_size, segment.coding.seed);
            if (encoder->source_symbol_count() != static_cast<int>(segment.source_symbol_count)) {
                throw std::logic_error("generation codec returned an inconsistent source symbol count");
            }
            for (std::uint32_t emitted = 0; emitted < segment.coding.emitted_symbols; ++emitted) {
                ::fec::Pkt packet;
                packet.fp = encoder->emit();
                packet.seq = packet_sequence++;
                packet.token_id = token_id;
                packet.kind = segment.importance == TransportImportance::CRITICAL
                    ? ::fec::SegmentKind::CRITICAL
                    : ::fec::SegmentKind::BULK;
                packet.generation_id = descriptor.generation_id;
                packet.segment_id = segment.segment_id;
                packet.descriptor_fingerprint = descriptor.descriptor_fingerprint;
                result.packets.push_back(std::move(packet));
            }
        }
        result.source_symbol_count = static_cast<int>(descriptor.total_source_symbols);
        result.K = result.source_symbol_count;

        GenerationState generation;
        generation.descriptor = descriptor;
        generation.profile = protection.profile;
        generation.contract = contract;
        for (const auto& segment : descriptor.segments) {
            generation.segments.emplace_back(segment, *codec_, descriptor.symbol_size);
        }
        reserve_generation_slot(now_ms);
        generations_.insert_or_assign(descriptor.generation_id, std::move(generation));
        generation_order_.push_back(descriptor.generation_id);
        return result;
    }

    DecodeReport integrate(const std::string& generation_id,
                           const std::vector<::fec::Pkt>& received_packets,
                           std::uint64_t now_ms) {
        const auto started = std::chrono::steady_clock::now();
        auto found = generations_.find(generation_id);
        if (found == generations_.end()) {
            DecodeReport report;
            report.generation_id = generation_id;
            report.status = DecodeStatus::MALFORMED_INPUT;
            report.failure_reason = "unknown generation id";
            return report;
        }
        auto& generation = found->second;
        if (generation.terminal_report.has_value()) {
            return *generation.terminal_report;
        }

        DecodeReport report = make_report(generation);
        if (const auto error = generation.descriptor.validation_error()) {
            report.status = DecodeStatus::MALFORMED_INPUT;
            report.failure_reason = "invalid generation descriptor: " + *error;
            return finish_timing(std::move(report), started);
        }
        if (compute_descriptor_fingerprint(generation.descriptor) !=
            generation.descriptor.descriptor_fingerprint) {
            report.status = DecodeStatus::MALFORMED_INPUT;
            report.failure_reason = "generation descriptor fingerprint mismatch";
            return finish_timing(std::move(report), started);
        }
        if (now_ms > generation.descriptor.expires_at_ms) {
            report.status = DecodeStatus::EXPIRED;
            report.failure_reason = "generation expired before full recovery";
            apply_terminal_feedback(generation, report);
            generation.terminal_report = finish_timing(std::move(report), started);
            return *generation.terminal_report;
        }

        bool saw_matching_packet = false;
        bool rank_increased = false;
        for (const auto& packet : received_packets) {
            if (packet.generation_id != generation_id) {
                continue;
            }
            saw_matching_packet = true;
            if (packet.token_id != generation.descriptor.token_id ||
                packet.descriptor_fingerprint != generation.descriptor.descriptor_fingerprint ||
                packet.segment_id >= generation.segments.size()) {
                ++generation.malformed_symbols;
                continue;
            }
            const PacketKey key{packet.segment_id, packet.fp.seed};
            if (!generation.seen_packets.insert(key).second) {
                ++generation.duplicate_symbols;
                continue;
            }
            ++generation.symbols_observed;

            auto& segment = generation.segments[packet.segment_id];
            const auto expected_kind = segment.descriptor.importance == TransportImportance::CRITICAL
                ? ::fec::SegmentKind::CRITICAL
                : ::fec::SegmentKind::BULK;
            if (packet.kind != expected_kind) {
                ++generation.malformed_symbols;
                continue;
            }
            switch (segment.decoder->push(packet.fp)) {
                case ::fec::PushResult::INNOVATIVE:
                    ++generation.innovative_symbols;
                    rank_increased = true;
                    break;
                case ::fec::PushResult::DEPENDENT:
                    ++generation.dependent_symbols;
                    break;
                case ::fec::PushResult::MALFORMED:
                case ::fec::PushResult::INCONSISTENT:
                    ++generation.malformed_symbols;
                    break;
            }
        }

        recover_complete_segments(generation);
        report = make_report(generation);
        if (all_segments_complete(generation)) {
            report.payload.assign(generation.descriptor.original_payload_length, 0);
            for (const auto& segment : generation.segments) {
                std::copy(segment.recovered->begin(), segment.recovered->end(),
                          report.payload.begin() + static_cast<std::ptrdiff_t>(segment.descriptor.offset));
            }
            report.payload_complete = true;
            report.coverage = 1.0;
            report.recovered_bytes = report.source_bytes;
            report.integrity_checked = generation.contract.require_payload_integrity;
            report.integrity_ok = !report.integrity_checked ||
                                  fnv1a64(report.payload) == generation.descriptor.payload_digest;
            if (!report.integrity_ok) {
                report.payload_complete = false;
                report.payload.clear();
                report.status = DecodeStatus::INTEGRITY_FAILURE;
                report.failure_reason = "decoded payload failed generation integrity check";
            } else {
                report.status = DecodeStatus::COMPLETE;
            }
            apply_terminal_feedback(generation, report);
            generation.terminal_report = finish_timing(std::move(report), started);
            return *generation.terminal_report;
        }

        if (generation.malformed_symbols > 0) {
            report.status = DecodeStatus::MALFORMED_INPUT;
            report.failure_reason = "one or more symbols did not match the generation descriptor";
        } else if (report.critical_complete) {
            report.status = DecodeStatus::CRITICAL_SEGMENT_COMPLETE;
        } else if (rank_increased) {
            report.status = DecodeStatus::PARTIAL_PROGRESS;
        } else if (saw_matching_packet) {
            report.status = DecodeStatus::INSUFFICIENT_RANK;
            report.failure_reason = "new observations did not increase decoder rank";
        } else {
            report.status = DecodeStatus::NO_PROGRESS;
        }
        return finish_timing(std::move(report), started);
    }

    [[nodiscard]] std::optional<GenerationDescriptor> descriptor(
        const std::string& generation_id) const {
        const auto found = generations_.find(generation_id);
        if (found == generations_.end()) {
            return std::nullopt;
        }
        return found->second.descriptor;
    }

    [[nodiscard]] std::size_t generation_count() const { return generations_.size(); }
    [[nodiscard]] const aurora::fec::GenerationCodec& codec() const { return *codec_; }
    [[nodiscard]] control::TransportPolicy& policy() { return *policy_; }
    [[nodiscard]] const control::TransportPolicy& policy() const { return *policy_; }

private:
    struct SegmentDecoderState {
        GenerationSegmentDescriptor descriptor;
        std::unique_ptr<aurora::fec::SymbolDecoder> decoder;
        std::optional<std::vector<std::uint8_t>> recovered;

        SegmentDecoderState(const GenerationSegmentDescriptor& value,
                            const aurora::fec::GenerationCodec& codec,
                            std::size_t symbol_size)
            : descriptor(value),
              decoder(codec.make_decoder(value.source_symbol_count, symbol_size)) {}

        SegmentDecoderState(SegmentDecoderState&&) noexcept = default;
        SegmentDecoderState& operator=(SegmentDecoderState&&) noexcept = default;
        SegmentDecoderState(const SegmentDecoderState&) = delete;
        SegmentDecoderState& operator=(const SegmentDecoderState&) = delete;
    };

    struct PacketKey {
        std::uint32_t segment_id = 0;
        std::uint32_t seed = 0;
        bool operator==(const PacketKey& other) const {
            return segment_id == other.segment_id && seed == other.seed;
        }
    };

    struct PacketKeyHash {
        std::size_t operator()(const PacketKey& key) const {
            return (static_cast<std::size_t>(key.segment_id) << 32U) ^ key.seed;
        }
    };

    struct GenerationState {
        GenerationDescriptor descriptor;
        control::FlowProfile profile;
        TransportContract contract;
        std::vector<SegmentDecoderState> segments;
        std::unordered_set<PacketKey, PacketKeyHash> seen_packets;
        std::uint32_t symbols_observed = 0;
        std::uint32_t innovative_symbols = 0;
        std::uint32_t dependent_symbols = 0;
        std::uint32_t duplicate_symbols = 0;
        std::uint32_t malformed_symbols = 0;
        bool policy_feedback_applied = false;
        std::optional<DecodeReport> terminal_report;
    };

    static std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return left + right;
    }

    static std::string make_generation_id(const std::string& token_id,
                                          std::uint64_t payload_digest,
                                          std::uint64_t seed,
                                          std::uint64_t counter) {
        std::ostringstream value;
        value << token_id << ':' << payload_digest << ':' << seed << ':' << counter;
        std::ostringstream encoded;
        encoded << std::hex << std::setw(16) << std::setfill('0') << fnv1a64(value.str());
        return encoded.str();
    }

    static std::uint64_t segment_seed(std::uint64_t experiment_seed,
                                      const std::string& generation_id,
                                      std::size_t segment_index) {
        std::ostringstream value;
        value << experiment_seed << ':' << generation_id << ':' << segment_index;
        return fnv1a64(value.str());
    }

    static std::vector<SegmentRequirement> compile_segments(
        const TransportContract& contract,
        std::size_t payload_size) {
        std::vector<SegmentRequirement> explicit_segments = contract.segments;
        std::sort(explicit_segments.begin(), explicit_segments.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });

        std::vector<SegmentRequirement> complete;
        std::size_t cursor = 0;
        auto add_default = [&](std::size_t offset, std::size_t length) {
            if (length == 0) return;
            complete.push_back({offset, length, contract.importance,
                                contract.deadline_ms(), contract.reliability});
        };
        for (const auto& segment : explicit_segments) {
            if (segment.offset + segment.length > payload_size) {
                throw std::invalid_argument("generation spawn: contract segment exceeds payload length");
            }
            add_default(cursor, segment.offset - cursor);
            complete.push_back(segment);
            cursor = segment.offset + segment.length;
        }
        add_default(cursor, payload_size - cursor);
        return complete;
    }

    static bool all_segments_complete(const GenerationState& generation) {
        return std::all_of(generation.segments.begin(), generation.segments.end(),
                           [](const auto& segment) { return segment.recovered.has_value(); });
    }

    static void recover_complete_segments(GenerationState& generation) {
        for (auto& segment : generation.segments) {
            if (segment.recovered.has_value() ||
                segment.decoder->rank() != static_cast<int>(segment.descriptor.source_symbol_count)) {
                continue;
            }
            auto [ok, bytes] = segment.decoder->solve();
            if (!ok) continue;
            bytes.resize(segment.descriptor.length);
            segment.recovered = std::move(bytes);
        }
    }

    static DecodeReport make_report(const GenerationState& generation) {
        DecodeReport report;
        report.generation_id = generation.descriptor.generation_id;
        report.source_bytes = generation.descriptor.original_payload_length;
        report.required_rank = generation.descriptor.total_source_symbols;
        report.symbols_observed = generation.symbols_observed;
        report.innovative_symbols = generation.innovative_symbols;
        report.dependent_symbols = generation.dependent_symbols;
        report.duplicate_symbols = generation.duplicate_symbols;
        report.malformed_symbols = generation.malformed_symbols;

        bool has_critical = false;
        bool all_critical = true;
        for (const auto& segment : generation.segments) {
            report.decoder_rank += static_cast<std::uint32_t>(segment.decoder->rank());
            if (segment.descriptor.importance == TransportImportance::CRITICAL) {
                has_critical = true;
                all_critical = all_critical && segment.recovered.has_value();
            }
            if (segment.recovered.has_value()) {
                report.recovered_bytes += segment.descriptor.length;
                report.recovered_segments.push_back({segment.descriptor.segment_id,
                                                     segment.descriptor.offset,
                                                     *segment.recovered});
            }
        }
        report.critical_complete = has_critical && all_critical;
        report.payload_complete = all_segments_complete(generation);
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
                std::chrono::steady_clock::now() - started).count());
        return report;
    }

    void apply_terminal_feedback(GenerationState& generation, const DecodeReport& report) {
        if (generation.policy_feedback_applied) return;
        generation.policy_feedback_applied = true;
        policy_->observe(generation.profile, report);
    }

    void reserve_generation_slot(std::uint64_t now_ms) {
        while (generations_.size() >= maximum_active_generations_) {
            auto evictable = generation_order_.end();
            for (auto it = generation_order_.begin(); it != generation_order_.end(); ++it) {
                const auto found = generations_.find(*it);
                if (found == generations_.end() || found->second.terminal_report.has_value() ||
                    now_ms > found->second.descriptor.expires_at_ms) {
                    evictable = it;
                    break;
                }
            }
            if (evictable == generation_order_.end()) {
                throw std::runtime_error(
                    "generation store capacity reached with no terminal or expired state to evict");
            }
            generations_.erase(*evictable);
            generation_order_.erase(evictable);
        }
    }

    std::shared_ptr<aurora::fec::GenerationCodec> codec_;
    std::shared_ptr<control::TransportPolicy> policy_;
    std::unordered_map<std::string, GenerationState> generations_;
    std::deque<std::string> generation_order_;
    std::size_t maximum_active_generations_ = 128;
    std::uint64_t generation_counter_ = 0;
};

} // namespace aurora::transport

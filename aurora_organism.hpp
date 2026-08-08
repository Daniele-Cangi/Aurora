#pragma once

#include "aurora_intention.hpp"
#include "aurora_extreme.hpp"
#include "include/aurora/fec/LtLikeCodec.hpp"
#include "include/aurora/transport/Generation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aurora {

using transport::DecodeReport;
using transport::DecodeStatus;
using transport::GenerationDescriptor;
using transport::GenerationSegmentDescriptor;
using transport::TransportContract;
using transport::TransportImportance;

// NERVE / GLAND / MUSCLE remain policy profiles. They do not encode payload meaning.
enum class FlowClass : std::uint8_t {
    NERVE,
    MUSCLE,
    GLAND
};

enum class Genotype {
    BASELINE,
    HYPERVIGILANT,
    STOIC,
    EXPERIMENTAL
};

enum class GenotypeHint {
    AUTO,
    FORCE_BASELINE,
    FORCE_HYPERVIGILANT,
    FORCE_STOIC,
    FORCE_EXPERIMENTAL
};

struct FlowProfile {
    double deadline_s = 600.0;
    double reliability = 0.99;
    double duty_limit = 0.01;
    std::string priority = "IMPORTANT";
    FlowClass flow_class = FlowClass::MUSCLE;
    GenotypeHint genotype_hint = GenotypeHint::AUTO;
};

struct OrganismSpawnResult {
    GenerationDescriptor descriptor;
    std::vector<fec::Pkt> packets;
    int K = 0;
    std::size_t payload_size = 0;
};

struct FlowState {
    double crit_overhead = 1.0;
    double bulk_overhead = 1.0;
    double base_crit_overhead = 1.0;
    double base_bulk_overhead = 1.0;
    double avg_coverage = 0.0;
    int success_count = 0;
    int fail_count = 0;
    int panic_boost = 0;
    int good_streak = 0;
    int bad_streak = 0;
    Genotype genotype = Genotype::BASELINE;
    bool initialized = false;
    int age = 0;
};

class AuroraOrganism {
public:
    virtual ~AuroraOrganism() = default;

    virtual FlowProfile build_profile(const TransportContract& contract) const = 0;

    virtual OrganismSpawnResult spawn(
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes,
        std::size_t symbol_size = 128,
        std::uint64_t now_ms = 0) = 0;

    virtual DecodeReport integrate(
        const std::string& generation_id,
        const std::vector<fec::Pkt>& received_packets,
        std::uint64_t now_ms) = 0;

    [[nodiscard]] virtual std::optional<GenerationDescriptor> descriptor(
        const std::string& generation_id) const = 0;
};

class AlienFountainOrganism final : public AuroraOrganism {
public:
    explicit AlienFountainOrganism(std::size_t maximum_active_generations = 128)
        : maximum_active_generations_(maximum_active_generations) {
        if (maximum_active_generations_ == 0) {
            throw std::invalid_argument("generation store capacity must be positive");
        }
    }

    FlowProfile build_profile(const TransportContract& contract) const override {
        FlowProfile profile;
        profile.deadline_s = contract.deadline_s;
        profile.reliability = contract.reliability;
        profile.duty_limit = contract.duty_frac;
        switch (contract.importance) {
            case TransportImportance::CRITICAL: profile.priority = "CRITICAL"; break;
            case TransportImportance::IMPORTANT: profile.priority = "IMPORTANT"; break;
            case TransportImportance::ELASTIC: profile.priority = "ELASTIC"; break;
        }

        // This classification selects a transport policy only. It never inspects bytes.
        if (profile.deadline_s < 2.0 && profile.reliability >= 0.90) {
            profile.flow_class = FlowClass::NERVE;
        } else if (profile.reliability > 0.95) {
            profile.flow_class = FlowClass::GLAND;
        } else {
            profile.flow_class = FlowClass::MUSCLE;
        }
        return profile;
    }

    OrganismSpawnResult spawn(
        const TransportContract& contract,
        const std::string& token_id,
        const std::vector<std::uint8_t>& payload_bytes,
        std::size_t symbol_size = 128,
        std::uint64_t now_ms = 0) override {
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

        const FlowProfile profile = build_profile(contract);
        auto& flow_state = state_for(profile);
        const auto requirements = compile_segments(contract, payload_bytes.size());

        OrganismSpawnResult result;
        result.payload_size = payload_bytes.size();
        auto& descriptor = result.descriptor;
        descriptor.token_id = token_id;
        descriptor.original_payload_length = payload_bytes.size();
        descriptor.symbol_size = symbol_size;
        descriptor.created_at_ms = now_ms;
        descriptor.expires_at_ms = saturating_add(now_ms, contract.deadline_ms());
        descriptor.payload_digest = transport::fnv1a64(payload_bytes);
        descriptor.generation_id = make_generation_id(token_id, descriptor.payload_digest,
                                                      contract.experiment_seed, generation_counter_++);

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
            segment.coding.seed = segment_seed(contract.experiment_seed,
                                               descriptor.generation_id, index);
            segment.coding.overhead_factor = bounded_overhead(
                contract, profile, flow_state, requirement.importance);
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

        descriptor.descriptor_fingerprint = transport::compute_descriptor_fingerprint(descriptor);
        if (const auto error = descriptor.validation_error()) {
            throw std::logic_error("generation spawn produced invalid descriptor: " + *error);
        }

        for (const auto& segment : descriptor.segments) {
            const auto begin = payload_bytes.begin() + static_cast<std::ptrdiff_t>(segment.offset);
            const auto end = begin + static_cast<std::ptrdiff_t>(segment.length);
            const std::vector<std::uint8_t> bytes(begin, end);
            fec::Encoder encoder(bytes, symbol_size, segment.coding.seed);
            for (std::uint32_t emitted = 0; emitted < segment.coding.emitted_symbols; ++emitted) {
                fec::Pkt packet;
                packet.fp = encoder.emit();
                packet.seq = packet_sequence++;
                packet.token_id = token_id;
                packet.kind = segment.importance == TransportImportance::CRITICAL
                    ? fec::SegmentKind::CRITICAL
                    : fec::SegmentKind::BULK;
                packet.generation_id = descriptor.generation_id;
                packet.segment_id = segment.segment_id;
                packet.descriptor_fingerprint = descriptor.descriptor_fingerprint;
                result.packets.push_back(std::move(packet));
            }
        }
        result.K = static_cast<int>(descriptor.total_source_symbols);

        GenerationState generation;
        generation.descriptor = descriptor;
        generation.profile = profile;
        generation.contract = contract;
        for (const auto& segment : descriptor.segments) {
            generation.segments.emplace_back(segment, descriptor.symbol_size);
        }
        reserve_generation_slot(now_ms);
        generations_.insert_or_assign(descriptor.generation_id, std::move(generation));
        generation_order_.push_back(descriptor.generation_id);
        return result;
    }

    DecodeReport integrate(
        const std::string& generation_id,
        const std::vector<fec::Pkt>& received_packets,
        std::uint64_t now_ms) override {
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
        if (transport::compute_descriptor_fingerprint(generation.descriptor) !=
            generation.descriptor.descriptor_fingerprint) {
            report.status = DecodeStatus::MALFORMED_INPUT;
            report.failure_reason = "generation descriptor fingerprint mismatch";
            return finish_timing(std::move(report), started);
        }
        if (now_ms > generation.descriptor.expires_at_ms) {
            report.status = DecodeStatus::EXPIRED;
            report.failure_reason = "generation expired before full recovery";
            finalize_failure(generation, report);
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
                ? fec::SegmentKind::CRITICAL
                : fec::SegmentKind::BULK;
            if (packet.kind != expected_kind) {
                ++generation.malformed_symbols;
                continue;
            }

            switch (segment.decoder.push(packet.fp)) {
                case fec::PushResult::INNOVATIVE:
                    ++generation.innovative_symbols;
                    rank_increased = true;
                    break;
                case fec::PushResult::DEPENDENT:
                    ++generation.dependent_symbols;
                    break;
                case fec::PushResult::MALFORMED:
                case fec::PushResult::INCONSISTENT:
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
                                  transport::fnv1a64(report.payload) == generation.descriptor.payload_digest;
            if (!report.integrity_ok) {
                report.payload_complete = false;
                report.payload.clear();
                report.status = DecodeStatus::INTEGRITY_FAILURE;
                report.failure_reason = "decoded payload failed generation integrity check";
                finalize_failure(generation, report);
            } else {
                report.status = DecodeStatus::COMPLETE;
                finalize_success(generation, report);
            }
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
        const std::string& generation_id) const override {
        const auto found = generations_.find(generation_id);
        if (found == generations_.end()) {
            return std::nullopt;
        }
        return found->second.descriptor;
    }

    [[nodiscard]] std::size_t generation_count() const {
        return generations_.size();
    }

    [[nodiscard]] std::optional<FlowState> flow_state(const FlowProfile& profile) const {
        const auto found = flow_states_.find(make_flow_key(profile));
        if (found == flow_states_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

private:
    struct SegmentDecoderState {
        GenerationSegmentDescriptor descriptor;
        fec::Decoder decoder;
        std::optional<std::vector<std::uint8_t>> recovered;

        SegmentDecoderState(const GenerationSegmentDescriptor& value, std::size_t symbol_size)
            : descriptor(value), decoder(static_cast<int>(value.source_symbol_count), symbol_size) {}
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
        FlowProfile profile;
        TransportContract contract;
        std::vector<SegmentDecoderState> segments;
        std::unordered_set<PacketKey, PacketKeyHash> seen_packets;
        std::uint32_t symbols_observed = 0;
        std::uint32_t innovative_symbols = 0;
        std::uint32_t dependent_symbols = 0;
        std::uint32_t duplicate_symbols = 0;
        std::uint32_t malformed_symbols = 0;
        bool adaptive_feedback_applied = false;
        std::optional<DecodeReport> terminal_report;
    };

    static std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return left + right;
    }

    static std::string make_generation_id(const std::string& token_id, std::uint64_t payload_digest,
                                          std::uint64_t seed, std::uint64_t counter) {
        std::ostringstream value;
        value << token_id << ':' << payload_digest << ':' << seed << ':' << counter;
        std::ostringstream encoded;
        encoded << std::hex << std::setw(16) << std::setfill('0') << transport::fnv1a64(value.str());
        return encoded.str();
    }

    static std::uint64_t segment_seed(std::uint64_t experiment_seed,
                                      const std::string& generation_id,
                                      std::size_t segment_index) {
        std::ostringstream value;
        value << experiment_seed << ':' << generation_id << ':' << segment_index;
        return transport::fnv1a64(value.str());
    }

    static std::vector<transport::SegmentRequirement> compile_segments(
        const TransportContract& contract, std::size_t payload_size) {
        std::vector<transport::SegmentRequirement> explicit_segments = contract.segments;
        std::sort(explicit_segments.begin(), explicit_segments.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });

        std::vector<transport::SegmentRequirement> complete;
        std::size_t cursor = 0;
        auto add_default = [&](std::size_t offset, std::size_t length) {
            if (length == 0) {
                return;
            }
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

    static Genotype initial_genotype(const FlowProfile& profile) {
        switch (profile.genotype_hint) {
            case GenotypeHint::FORCE_BASELINE: return Genotype::BASELINE;
            case GenotypeHint::FORCE_HYPERVIGILANT: return Genotype::HYPERVIGILANT;
            case GenotypeHint::FORCE_STOIC: return Genotype::STOIC;
            case GenotypeHint::FORCE_EXPERIMENTAL: return Genotype::EXPERIMENTAL;
            case GenotypeHint::AUTO: break;
        }
        switch (profile.flow_class) {
            case FlowClass::NERVE: return Genotype::HYPERVIGILANT;
            case FlowClass::GLAND: return Genotype::BASELINE;
            case FlowClass::MUSCLE: return Genotype::EXPERIMENTAL;
        }
        return Genotype::BASELINE;
    }

    static double critical_base_overhead(const FlowProfile& profile) {
        switch (profile.flow_class) {
            case FlowClass::NERVE: return 3.0;
            case FlowClass::GLAND: return 2.5;
            case FlowClass::MUSCLE: return 1.5;
        }
        return 1.5;
    }

    static double bulk_base_overhead(const FlowProfile& profile) {
        switch (profile.flow_class) {
            case FlowClass::NERVE: return 1.0;
            case FlowClass::GLAND: return 1.5;
            case FlowClass::MUSCLE: return 1.2;
        }
        return 1.2;
    }

    static std::string make_flow_key(const FlowProfile& profile) {
        return std::to_string(static_cast<int>(profile.flow_class)) + ':' + profile.priority;
    }

    FlowState& state_for(const FlowProfile& profile) {
        auto& state = flow_states_[make_flow_key(profile)];
        if (!state.initialized) {
            state.genotype = initial_genotype(profile);
            state.base_crit_overhead = critical_base_overhead(profile);
            state.base_bulk_overhead = bulk_base_overhead(profile);
            state.crit_overhead = state.base_crit_overhead;
            state.bulk_overhead = state.base_bulk_overhead;
            state.initialized = true;
        }
        ++state.age;
        return state;
    }

    static double bounded_overhead(const TransportContract& contract,
                                   const FlowProfile& profile,
                                   FlowState& state,
                                   TransportImportance importance) {
        double overhead = importance == TransportImportance::CRITICAL
            ? std::max(state.crit_overhead, contract.minimum_critical_overhead)
            : state.bulk_overhead;
        if (importance == TransportImportance::ELASTIC) {
            overhead = std::min(overhead, 1.25);
        }
        if (state.panic_boost > 0) {
            overhead *= importance == TransportImportance::CRITICAL ? 2.0 : 1.5;
        }
        (void)profile;
        return std::clamp(overhead, 1.0, contract.maximum_repair_amplification);
    }

    static bool all_segments_complete(const GenerationState& generation) {
        return std::all_of(generation.segments.begin(), generation.segments.end(),
                           [](const auto& segment) { return segment.recovered.has_value(); });
    }

    static void recover_complete_segments(GenerationState& generation) {
        for (auto& segment : generation.segments) {
            if (segment.recovered.has_value() ||
                segment.decoder.rank() != static_cast<int>(segment.descriptor.source_symbol_count)) {
                continue;
            }
            auto [ok, bytes] = segment.decoder.solve();
            if (!ok) {
                continue;
            }
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
            report.decoder_rank += static_cast<std::uint32_t>(segment.decoder.rank());
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
            : static_cast<double>(report.recovered_bytes) / static_cast<double>(report.source_bytes);
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

    void finalize_success(GenerationState& generation, const DecodeReport& report) {
        apply_adaptive_feedback(generation, report, true);
    }

    void finalize_failure(GenerationState& generation, const DecodeReport& report) {
        apply_adaptive_feedback(generation, report, false);
    }

    void apply_adaptive_feedback(GenerationState& generation,
                                 const DecodeReport& report,
                                 bool delivered) {
        if (generation.adaptive_feedback_applied) {
            return;
        }
        generation.adaptive_feedback_applied = true;
        auto& state = state_for(generation.profile);
        constexpr double coverage_alpha = 0.2;
        state.avg_coverage = state.success_count + state.fail_count == 0
            ? report.coverage
            : (1.0 - coverage_alpha) * state.avg_coverage + coverage_alpha * report.coverage;

        const auto& config = cl::get_interactive_config();
        if (delivered) {
            ++state.success_count;
            ++state.good_streak;
            state.bad_streak = 0;
            if (state.panic_boost > 0) {
                --state.panic_boost;
            }
            if (state.good_streak >= 4 && state.panic_boost == 0 && state.avg_coverage >= 0.85) {
                state.crit_overhead = std::max(state.base_crit_overhead,
                                               state.crit_overhead - config.alpha_down);
                state.bulk_overhead = std::max(state.base_bulk_overhead,
                                               state.bulk_overhead - config.alpha_down);
            }
        } else {
            ++state.fail_count;
            ++state.bad_streak;
            state.good_streak = 0;
            state.crit_overhead = std::min(6.0, state.crit_overhead + config.alpha_up);
            state.bulk_overhead = std::min(4.0, state.bulk_overhead + config.alpha_up * 0.5);
            if (generation.profile.flow_class != FlowClass::MUSCLE) {
                state.panic_boost = std::max(state.panic_boost, config.panic_boost_steps);
            }
        }
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
                throw std::runtime_error("generation store capacity reached with no terminal or expired state to evict");
            }
            generations_.erase(*evictable);
            generation_order_.erase(evictable);
        }
    }

    std::unordered_map<std::string, GenerationState> generations_;
    std::unordered_map<std::string, FlowState> flow_states_;
    std::deque<std::string> generation_order_;
    std::size_t maximum_active_generations_ = 128;
    std::uint64_t generation_counter_ = 0;
};

} // namespace aurora

#pragma once

#include "../safety/SafetyEnvelope.hpp"
#include "../transport/Generation.hpp"
#include "../transport/TransportContract.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::telemetry {

struct DecisionReplayRecord {
    transport::TransportContract contract;
    transport::GenerationDescriptor descriptor;
    safety::TransportDecisionTrace trace;
};

struct ReplayVerification {
    bool ok = true;
    std::size_t records_verified = 0;
    std::string failure_reason;
};

// Canonical, deterministic provenance for safety decisions. The checksum chain
// detects accidental corruption and reordering; it is not a cryptographic signature.
class DecisionReplayLog {
public:
    static constexpr std::string_view format_header = "AURORA_DECISION_TRACE_V2";

    void record(const transport::TransportContract& contract,
                const transport::GenerationDescriptor& descriptor,
                const safety::TransportDecisionTrace& trace) {
        contract.validate();
        if (descriptor.generation_id.empty() ||
            trace.generation_id != descriptor.generation_id) {
            throw std::invalid_argument(
                "decision trace: trace and descriptor generation IDs must match");
        }
        if (const auto error = trace.execution_error()) {
            throw std::invalid_argument("decision trace: " + *error);
        }
        records_.push_back({contract, descriptor, trace});
    }

    [[nodiscard]] const std::vector<DecisionReplayRecord>& records() const {
        return records_;
    }

    [[nodiscard]] std::string serialize() const {
        std::ostringstream output;
        output << format_header << '\n';
        std::uint64_t previous_checksum = 0;
        for (std::size_t index = 0; index < records_.size(); ++index) {
            const auto payload = encode_record(index, previous_checksum, records_[index]);
            const auto checksum = transport::fnv1a64(payload);
            output << payload << '|' << hex_u64(checksum) << '\n';
            previous_checksum = checksum;
        }
        const auto footer = std::string("END|") + std::to_string(records_.size()) +
                            '|' + hex_u64(previous_checksum);
        output << footer << '|' << hex_u64(transport::fnv1a64(footer)) << '\n';
        return output.str();
    }

    void save(const std::string& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("decision trace: cannot open output file: " + path);
        }
        const auto encoded = serialize();
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            throw std::runtime_error("decision trace: failed while writing: " + path);
        }
    }

    [[nodiscard]] static DecisionReplayLog load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("decision trace: cannot open input file: " + path);
        }
        std::ostringstream encoded;
        encoded << input.rdbuf();
        if (!input.good() && !input.eof()) {
            throw std::runtime_error("decision trace: failed while reading: " + path);
        }
        return deserialize(encoded.str());
    }

    [[nodiscard]] static DecisionReplayLog deserialize(const std::string& encoded) {
        std::istringstream input(encoded);
        std::string line;
        if (!std::getline(input, line) || line != format_header) {
            throw std::invalid_argument("decision trace: unsupported or missing format header");
        }

        DecisionReplayLog log;
        std::uint64_t previous_checksum = 0;
        std::size_t expected_index = 0;
        bool footer_seen = false;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (footer_seen) {
                throw std::invalid_argument("decision trace: data follows the end marker");
            }
            const auto checksum_separator = line.rfind('|');
            if (checksum_separator == std::string::npos) {
                throw std::invalid_argument("decision trace: record checksum is missing");
            }
            const auto payload = line.substr(0, checksum_separator);
            const auto stored_checksum = parse_u64(
                line.substr(checksum_separator + 1), 16, "record checksum");
            const auto computed_checksum = transport::fnv1a64(payload);
            if (stored_checksum != computed_checksum) {
                throw std::invalid_argument("decision trace: record checksum mismatch");
            }
            if (payload.starts_with("END|")) {
                verify_footer(payload, expected_index, previous_checksum);
                footer_seen = true;
                continue;
            }
            log.records_.push_back(decode_record(
                payload, expected_index, previous_checksum));
            previous_checksum = stored_checksum;
            ++expected_index;
        }
        if (!footer_seen) {
            throw std::invalid_argument("decision trace: end marker is missing");
        }
        return log;
    }

    [[nodiscard]] ReplayVerification verify(
        const safety::SafetyEnvelope& envelope = {}) const {
        ReplayVerification verification;
        for (std::size_t index = 0; index < records_.size(); ++index) {
            const auto& record = records_[index];
            const auto replayed = envelope.constrain(
                record.contract,
                record.descriptor,
                record.trace.observed,
                record.trace.proposed);
            if (!same_trace(replayed, record.trace)) {
                verification.ok = false;
                verification.failure_reason =
                    "decision mismatch at record " + std::to_string(index);
                return verification;
            }
            if (const auto error = record.trace.execution_error()) {
                verification.ok = false;
                verification.failure_reason =
                    "execution mismatch at record " + std::to_string(index) + ": " + *error;
                return verification;
            }
            ++verification.records_verified;
        }
        return verification;
    }

private:
    static std::string hex_u64(std::uint64_t value) {
        std::ostringstream encoded;
        encoded << std::hex << std::setw(16) << std::setfill('0') << value;
        return encoded.str();
    }

    static std::string hex_bytes(std::string_view value) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (const unsigned char byte : value) {
            encoded.push_back(digits[byte >> 4U]);
            encoded.push_back(digits[byte & 0x0FU]);
        }
        return encoded;
    }

    static unsigned char hex_nibble(char value) {
        if (value >= '0' && value <= '9') return static_cast<unsigned char>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned char>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F') return static_cast<unsigned char>(value - 'A' + 10);
        throw std::invalid_argument("decision trace: invalid hexadecimal data");
    }

    static std::string decode_hex_bytes(std::string_view encoded) {
        if (encoded.size() % 2 != 0) {
            throw std::invalid_argument("decision trace: hexadecimal string has odd length");
        }
        std::string value;
        value.reserve(encoded.size() / 2);
        for (std::size_t i = 0; i < encoded.size(); i += 2) {
            value.push_back(static_cast<char>(
                (hex_nibble(encoded[i]) << 4U) | hex_nibble(encoded[i + 1])));
        }
        return value;
    }

    static std::uint64_t double_bits(double value) {
        return std::bit_cast<std::uint64_t>(value);
    }

    static double bits_double(std::uint64_t value) {
        return std::bit_cast<double>(value);
    }

    static std::uint64_t parse_u64(const std::string& encoded,
                                   int base,
                                   const char* field) {
        if (encoded.empty() || encoded.front() == '-') {
            throw std::invalid_argument(std::string("decision trace: invalid ") + field);
        }
        std::size_t consumed = 0;
        unsigned long long value = 0;
        try {
            value = std::stoull(encoded, &consumed, base);
        } catch (const std::exception&) {
            throw std::invalid_argument(std::string("decision trace: invalid ") + field);
        }
        if (consumed != encoded.size() ||
            value > std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument(std::string("decision trace: invalid ") + field);
        }
        return static_cast<std::uint64_t>(value);
    }

    static std::uint32_t parse_u32(const std::string& encoded, const char* field) {
        const auto value = parse_u64(encoded, 10, field);
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument(std::string("decision trace: out-of-range ") + field);
        }
        return static_cast<std::uint32_t>(value);
    }

    static bool parse_bool(const std::string& encoded, const char* field) {
        if (encoded == "0") return false;
        if (encoded == "1") return true;
        throw std::invalid_argument(std::string("decision trace: invalid ") + field);
    }

    static safety::LinkMode parse_link(const std::string& encoded) {
        const auto value = parse_u32(encoded, "link mode");
        if (value > static_cast<std::uint32_t>(safety::LinkMode::BACKSCATTER)) {
            throw std::invalid_argument("decision trace: invalid link mode");
        }
        return static_cast<safety::LinkMode>(value);
    }

    static void append_decision(std::ostringstream& output,
                                const safety::TransportDecision& decision) {
        output << '|' << static_cast<unsigned>(decision.link)
               << '|' << decision.transmission_attempts
               << '|' << decision.repair_symbols
               << '|' << decision.critical_only
               << '|' << decision.permitted;
    }

    static safety::TransportDecision parse_decision(
        const std::vector<std::string>& fields,
        std::size_t& cursor) {
        safety::TransportDecision decision;
        decision.link = parse_link(fields.at(cursor++));
        decision.transmission_attempts = parse_u32(fields.at(cursor++), "transmission attempts");
        decision.repair_symbols = parse_u32(fields.at(cursor++), "repair symbols");
        decision.critical_only = parse_bool(fields.at(cursor++), "critical-only flag");
        decision.permitted = parse_bool(fields.at(cursor++), "permitted flag");
        return decision;
    }

    static void append_execution(std::ostringstream& output,
                                 const safety::TransportExecution& execution) {
        output << '|' << execution.recorded
               << '|' << static_cast<unsigned>(execution.link)
               << '|' << execution.transmission_attempts
               << '|' << execution.hal_accepted_attempts
               << '|' << execution.delivered_attempts
               << '|' << execution.repair_symbols_emitted
               << '|' << execution.critical_only;
    }

    static safety::TransportExecution parse_execution(
        const std::vector<std::string>& fields,
        std::size_t& cursor) {
        safety::TransportExecution execution;
        execution.recorded = parse_bool(fields.at(cursor++), "execution-recorded flag");
        execution.link = parse_link(fields.at(cursor++));
        execution.transmission_attempts = parse_u32(
            fields.at(cursor++), "executed transmission attempts");
        execution.hal_accepted_attempts = parse_u32(
            fields.at(cursor++), "HAL-accepted attempts");
        execution.delivered_attempts = parse_u32(
            fields.at(cursor++), "delivered attempts");
        execution.repair_symbols_emitted = parse_u32(
            fields.at(cursor++), "emitted repair symbols");
        execution.critical_only = parse_bool(
            fields.at(cursor++), "executed critical-only flag");
        return execution;
    }

    static std::uint32_t critical_source_count(
        const transport::GenerationDescriptor& descriptor) {
        std::uint32_t count = 0;
        for (const auto& segment : descriptor.segments) {
            if (segment.importance == transport::TransportImportance::CRITICAL) {
                count += segment.source_symbol_count;
            }
        }
        return count;
    }

    static std::string encode_constraints(const std::vector<std::string>& constraints) {
        std::ostringstream encoded;
        for (std::size_t i = 0; i < constraints.size(); ++i) {
            if (i != 0) encoded << ',';
            encoded << hex_bytes(constraints[i]);
        }
        return encoded.str();
    }

    static std::vector<std::string> decode_constraints(
        const std::string& encoded,
        std::size_t expected_count) {
        std::vector<std::string> constraints;
        if (!encoded.empty()) {
            std::istringstream input(encoded);
            std::string item;
            while (std::getline(input, item, ',')) {
                constraints.push_back(decode_hex_bytes(item));
            }
        }
        if (constraints.size() != expected_count) {
            throw std::invalid_argument("decision trace: constraint count mismatch");
        }
        return constraints;
    }

    static std::string encode_record(std::size_t index,
                                     std::uint64_t previous_checksum,
                                     const DecisionReplayRecord& record) {
        const auto& contract = record.contract;
        const auto& descriptor = record.descriptor;
        const auto& trace = record.trace;
        std::ostringstream output;
        output << "R|" << index
               << '|' << hex_u64(previous_checksum)
               << '|' << hex_bytes(trace.generation_id)
               << '|' << contract.allow_rf
               << '|' << contract.allow_optical
               << '|' << contract.allow_backscatter
               << '|' << hex_u64(double_bits(contract.minimum_source_reserve))
               << '|' << contract.maximum_observation_age_ms
               << '|' << hex_u64(double_bits(contract.maximum_repair_amplification))
               << '|' << hex_u64(double_bits(contract.minimum_critical_overhead))
               << '|' << descriptor.expires_at_ms
               << '|' << descriptor.total_source_symbols
               << '|' << critical_source_count(descriptor)
               << '|' << trace.observed.observed_at_ms
               << '|' << trace.observed.now_ms
               << '|' << hex_u64(double_bits(trace.observed.source_energy_reserve))
               << '|' << hex_u64(double_bits(trace.observed.rf_duty_remaining))
               << '|' << hex_u64(double_bits(trace.observed.source_energy_capacity_j))
               << '|' << hex_u64(double_bits(trace.observed.rf_energy_cost_per_attempt_j))
               << '|' << hex_u64(double_bits(trace.observed.optical_energy_cost_per_attempt_j))
               << '|' << hex_u64(double_bits(trace.observed.backscatter_energy_cost_per_attempt_j))
               << '|' << hex_u64(double_bits(trace.observed.rf_duty_remaining_s))
               << '|' << hex_u64(double_bits(trace.observed.rf_airtime_per_attempt_s))
               << '|' << trace.observed.emitted_symbols
               << '|' << trace.observed.critical_emitted_symbols
               << '|' << trace.observed.decoder_rank
               << '|' << trace.observed.required_rank;
        append_decision(output, trace.proposed);
        append_decision(output, trace.decision);
        append_execution(output, trace.execution);
        output << '|' << trace.constraints_applied.size()
               << '|' << encode_constraints(trace.constraints_applied);
        return output.str();
    }

    static std::vector<std::string> split_fields(const std::string& payload) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto separator = payload.find('|', start);
            if (separator == std::string::npos) {
                fields.push_back(payload.substr(start));
                break;
            }
            fields.push_back(payload.substr(start, separator - start));
            start = separator + 1;
        }
        return fields;
    }

    static void verify_footer(const std::string& payload,
                              std::size_t expected_count,
                              std::uint64_t expected_previous_checksum) {
        const auto fields = split_fields(payload);
        if (fields.size() != 3 || fields[0] != "END") {
            throw std::invalid_argument("decision trace: malformed end marker");
        }
        if (parse_u64(fields[1], 10, "record count") != expected_count ||
            parse_u64(fields[2], 16, "final record checksum") !=
                expected_previous_checksum) {
            throw std::invalid_argument("decision trace: incomplete checksum chain");
        }
    }

    static DecisionReplayRecord decode_record(const std::string& payload,
                                               std::size_t expected_index,
                                               std::uint64_t expected_previous_checksum) {
        const auto fields = split_fields(payload);
        if (fields.size() != 47 || fields.front() != "R") {
            throw std::invalid_argument("decision trace: malformed record field count");
        }
        std::size_t cursor = 1;
        if (parse_u64(fields.at(cursor++), 10, "record index") != expected_index) {
            throw std::invalid_argument("decision trace: non-contiguous record index");
        }
        if (parse_u64(fields.at(cursor++), 16, "previous checksum") !=
            expected_previous_checksum) {
            throw std::invalid_argument("decision trace: checksum chain mismatch");
        }

        DecisionReplayRecord record;
        auto& contract = record.contract;
        auto& descriptor = record.descriptor;
        auto& trace = record.trace;
        trace.generation_id = decode_hex_bytes(fields.at(cursor++));
        descriptor.generation_id = trace.generation_id;
        contract.allow_rf = parse_bool(fields.at(cursor++), "RF permission");
        contract.allow_optical = parse_bool(fields.at(cursor++), "optical permission");
        contract.allow_backscatter = parse_bool(fields.at(cursor++), "backscatter permission");
        contract.minimum_source_reserve = bits_double(
            parse_u64(fields.at(cursor++), 16, "reserve floor"));
        contract.maximum_observation_age_ms = parse_u64(
            fields.at(cursor++), 10, "maximum observation age");
        contract.maximum_repair_amplification = bits_double(
            parse_u64(fields.at(cursor++), 16, "repair amplification"));
        contract.minimum_critical_overhead = bits_double(
            parse_u64(fields.at(cursor++), 16, "critical overhead"));
        descriptor.expires_at_ms = parse_u64(fields.at(cursor++), 10, "expiry");
        descriptor.total_source_symbols = parse_u32(fields.at(cursor++), "source symbols");
        const auto critical_sources = parse_u32(fields.at(cursor++), "critical source symbols");
        if (critical_sources > descriptor.total_source_symbols) {
            throw std::invalid_argument("decision trace: critical source count exceeds total");
        }
        if (critical_sources > 0) {
            transport::GenerationSegmentDescriptor segment;
            segment.source_symbol_count = critical_sources;
            segment.importance = transport::TransportImportance::CRITICAL;
            descriptor.segments.push_back(segment);
        }
        trace.observed.observed_at_ms = parse_u64(fields.at(cursor++), 10, "observation time");
        trace.observed.now_ms = parse_u64(fields.at(cursor++), 10, "decision time");
        trace.observed.source_energy_reserve = bits_double(
            parse_u64(fields.at(cursor++), 16, "source energy reserve"));
        trace.observed.rf_duty_remaining = bits_double(
            parse_u64(fields.at(cursor++), 16, "RF duty remaining"));
        trace.observed.source_energy_capacity_j = bits_double(
            parse_u64(fields.at(cursor++), 16, "source energy capacity"));
        trace.observed.rf_energy_cost_per_attempt_j = bits_double(
            parse_u64(fields.at(cursor++), 16, "RF energy cost"));
        trace.observed.optical_energy_cost_per_attempt_j = bits_double(
            parse_u64(fields.at(cursor++), 16, "optical energy cost"));
        trace.observed.backscatter_energy_cost_per_attempt_j = bits_double(
            parse_u64(fields.at(cursor++), 16, "backscatter energy cost"));
        trace.observed.rf_duty_remaining_s = bits_double(
            parse_u64(fields.at(cursor++), 16, "RF duty seconds remaining"));
        trace.observed.rf_airtime_per_attempt_s = bits_double(
            parse_u64(fields.at(cursor++), 16, "RF airtime cost"));
        trace.observed.emitted_symbols = parse_u64(
            fields.at(cursor++), 10, "emitted symbols");
        trace.observed.critical_emitted_symbols = parse_u64(
            fields.at(cursor++), 10, "critical emitted symbols");
        trace.observed.decoder_rank = parse_u32(fields.at(cursor++), "decoder rank");
        trace.observed.required_rank = parse_u32(fields.at(cursor++), "required rank");
        trace.proposed = parse_decision(fields, cursor);
        trace.decision = parse_decision(fields, cursor);
        trace.execution = parse_execution(fields, cursor);
        const auto constraint_count = static_cast<std::size_t>(
            parse_u64(fields.at(cursor++), 10, "constraint count"));
        trace.constraints_applied = decode_constraints(fields.at(cursor++), constraint_count);
        if (cursor != fields.size()) {
            throw std::invalid_argument("decision trace: trailing fields");
        }
        contract.validate();
        if (trace.generation_id.empty()) {
            throw std::invalid_argument("decision trace: generation ID is required");
        }
        return record;
    }

    static bool same_decision(const safety::TransportDecision& left,
                              const safety::TransportDecision& right) {
        return left.link == right.link &&
               left.transmission_attempts == right.transmission_attempts &&
               left.repair_symbols == right.repair_symbols &&
               left.critical_only == right.critical_only &&
               left.permitted == right.permitted;
    }

    static bool same_state(const safety::TransportState& left,
                           const safety::TransportState& right) {
        return left.observed_at_ms == right.observed_at_ms &&
               left.now_ms == right.now_ms &&
               double_bits(left.source_energy_reserve) == double_bits(right.source_energy_reserve) &&
               double_bits(left.rf_duty_remaining) == double_bits(right.rf_duty_remaining) &&
               double_bits(left.source_energy_capacity_j) == double_bits(right.source_energy_capacity_j) &&
               double_bits(left.rf_energy_cost_per_attempt_j) == double_bits(right.rf_energy_cost_per_attempt_j) &&
               double_bits(left.optical_energy_cost_per_attempt_j) == double_bits(right.optical_energy_cost_per_attempt_j) &&
               double_bits(left.backscatter_energy_cost_per_attempt_j) == double_bits(right.backscatter_energy_cost_per_attempt_j) &&
               double_bits(left.rf_duty_remaining_s) == double_bits(right.rf_duty_remaining_s) &&
               double_bits(left.rf_airtime_per_attempt_s) == double_bits(right.rf_airtime_per_attempt_s) &&
               left.emitted_symbols == right.emitted_symbols &&
               left.critical_emitted_symbols == right.critical_emitted_symbols &&
               left.decoder_rank == right.decoder_rank &&
               left.required_rank == right.required_rank;
    }

    static bool same_trace(const safety::TransportDecisionTrace& left,
                           const safety::TransportDecisionTrace& right) {
        return left.generation_id == right.generation_id &&
               same_state(left.observed, right.observed) &&
               same_decision(left.proposed, right.proposed) &&
               same_decision(left.decision, right.decision) &&
               left.constraints_applied == right.constraints_applied;
    }

    std::vector<DecisionReplayRecord> records_;
};

} // namespace aurora::telemetry

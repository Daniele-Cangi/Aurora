#pragma once

#include "../control/ControllerTransition.hpp"
#include "../safety/SafetyEnvelope.hpp"
#include "../transport/Generation.hpp"
#include "../transport/TransportContract.hpp"

#include <bit>
#include <cmath>
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
    control::ControllerTransition controller;
};

struct ReplayVerification {
    bool ok = true;
    std::size_t records_verified = 0;
    std::string failure_reason;
};

// Canonical, deterministic provenance for action and supervisory-controller
// transitions. The checksum chain detects accidental corruption and reordering;
// it is not a cryptographic signature.
class DecisionReplayLog {
public:
    static constexpr std::string_view format_header = "AURORA_DECISION_TRACE_V4";

    void record(const transport::TransportContract& contract,
                const transport::GenerationDescriptor& descriptor,
                const safety::TransportDecisionTrace& trace,
                const control::ControllerTransition& controller) {
        contract.validate();
        if (descriptor.generation_id.empty() ||
            trace.generation_id != descriptor.generation_id) {
            throw std::invalid_argument(
                "decision trace: trace and descriptor generation IDs must match");
        }
        if (!trace.execution.recorded) {
            throw std::invalid_argument(
                "decision trace: V4 requires a recorded execution transition");
        }
        if (const auto error = trace.execution_error()) {
            throw std::invalid_argument("decision trace: " + *error);
        }
        if (const auto error = controller.validation_error()) {
            throw std::invalid_argument("decision trace: controller " + *error);
        }
        if (controller.now_ms != trace.observed.now_ms) {
            throw std::invalid_argument(
                "decision trace: controller and transport times do not match");
        }
        if (!records_.empty()) {
            const auto& previous = records_.back().controller;
            if (!(previous.after == controller.before) ||
                previous.mode_after != controller.mode_before) {
                throw std::invalid_argument(
                    "decision trace: controller state is not contiguous");
            }
        }
        records_.push_back({contract, descriptor, trace, controller});
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
            if (const auto error = record.controller.validation_error()) {
                verification.ok = false;
                verification.failure_reason =
                    "controller mismatch at record " + std::to_string(index) +
                    ": " + *error;
                return verification;
            }
            if (record.controller.now_ms != record.trace.observed.now_ms) {
                verification.ok = false;
                verification.failure_reason =
                    "controller time mismatch at record " + std::to_string(index);
                return verification;
            }
            if (index > 0) {
                const auto& previous = records_[index - 1].controller;
                if (!(previous.after == record.controller.before) ||
                    previous.mode_after != record.controller.mode_before) {
                    verification.ok = false;
                    verification.failure_reason =
                        "controller continuity mismatch at record " +
                        std::to_string(index);
                    return verification;
                }
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

    static int parse_int(const std::string& encoded, const char* field) {
        std::size_t consumed = 0;
        long value = 0;
        try {
            value = std::stol(encoded, &consumed, 10);
        } catch (const std::exception&) {
            throw std::invalid_argument(std::string("decision trace: invalid ") + field);
        }
        if (consumed != encoded.size() ||
            value < std::numeric_limits<int>::min() ||
            value > std::numeric_limits<int>::max()) {
            throw std::invalid_argument(std::string("decision trace: invalid ") + field);
        }
        return static_cast<int>(value);
    }

    static std::vector<std::string> split_delimited(
        const std::string& encoded, char delimiter) {
        std::vector<std::string> fields;
        if (encoded.empty()) return fields;
        std::istringstream input(encoded);
        std::string field;
        while (std::getline(input, field, delimiter)) fields.push_back(field);
        return fields;
    }

    static safety::LinkMode parse_link(const std::string& encoded) {
        const auto value = parse_u32(encoded, "link mode");
        if (value > static_cast<std::uint32_t>(safety::LinkMode::BACKSCATTER)) {
            throw std::invalid_argument("decision trace: invalid link mode");
        }
        return static_cast<safety::LinkMode>(value);
    }

    static safety::SafetyState parse_safety_state(const std::string& encoded) {
        const auto value = parse_int(encoded, "safety state");
        switch (value) {
            case -1: return safety::SafetyState::NO_EVIDENCE;
            case 0: return safety::SafetyState::HEALTHY;
            case 1: return safety::SafetyState::DEGRADED;
            case 2: return safety::SafetyState::CRITICAL;
            default:
                throw std::invalid_argument("decision trace: invalid safety state");
        }
    }

    static control::OperatingMode parse_operating_mode(const std::string& encoded) {
        const auto value = parse_u32(encoded, "operating mode");
        if (value > static_cast<std::uint32_t>(control::OperatingMode::AGGRESSIVE)) {
            throw std::invalid_argument("decision trace: invalid operating mode");
        }
        return static_cast<control::OperatingMode>(value);
    }

    static std::string encode_safety_evidence(
        const safety::SafetyEvidenceSample& sample) {
        std::ostringstream encoded;
        encoded << sample.observed_at_ms << ','
                << hex_u64(double_bits(sample.duty_left)) << ','
                << hex_u64(double_bits(sample.nerve_fail_rate)) << ','
                << hex_u64(double_bits(sample.gland_fail_rate)) << ','
                << hex_u64(double_bits(sample.muscle_fail_rate)) << ','
                << hex_u64(double_bits(sample.nerve_cov)) << ','
                << hex_u64(double_bits(sample.gland_cov)) << ','
                << hex_u64(double_bits(sample.muscle_cov)) << ','
                << sample.nerve_has_evidence << ','
                << sample.gland_has_evidence << ','
                << sample.muscle_has_evidence;
        return encoded.str();
    }

    static safety::SafetyEvidenceSample decode_safety_evidence(
        const std::string& encoded) {
        const auto fields = split_delimited(encoded, ',');
        if (fields.size() != 11) {
            throw std::invalid_argument("decision trace: malformed safety evidence");
        }
        safety::SafetyEvidenceSample sample;
        std::size_t cursor = 0;
        sample.observed_at_ms = parse_u64(fields[cursor++], 10, "health observation time");
        sample.duty_left = bits_double(parse_u64(fields[cursor++], 16, "health duty"));
        sample.nerve_fail_rate = bits_double(parse_u64(fields[cursor++], 16, "nerve failure rate"));
        sample.gland_fail_rate = bits_double(parse_u64(fields[cursor++], 16, "gland failure rate"));
        sample.muscle_fail_rate = bits_double(parse_u64(fields[cursor++], 16, "muscle failure rate"));
        sample.nerve_cov = bits_double(parse_u64(fields[cursor++], 16, "nerve coverage"));
        sample.gland_cov = bits_double(parse_u64(fields[cursor++], 16, "gland coverage"));
        sample.muscle_cov = bits_double(parse_u64(fields[cursor++], 16, "muscle coverage"));
        sample.nerve_has_evidence = parse_bool(fields[cursor++], "nerve evidence flag");
        sample.gland_has_evidence = parse_bool(fields[cursor++], "gland evidence flag");
        sample.muscle_has_evidence = parse_bool(fields[cursor++], "muscle evidence flag");
        if (const auto error = safety::safety_evidence_error(sample)) {
            throw std::invalid_argument("decision trace: " + *error);
        }
        return sample;
    }

    static std::string encode_safety_snapshot(
        const safety::SafetyMonitorSnapshot& snapshot) {
        const auto& config = snapshot.config;
        std::ostringstream encoded;
        encoded << hex_u64(double_bits(config.duty_budget_critical_threshold)) << ','
                << hex_u64(double_bits(config.duty_budget_degraded_threshold)) << ','
                << hex_u64(double_bits(config.duty_budget_recovery_margin)) << ','
                << hex_u64(double_bits(config.fail_rate_critical_threshold)) << ','
                << hex_u64(double_bits(config.fail_rate_degraded_threshold)) << ','
                << hex_u64(double_bits(config.fail_rate_recovery_margin)) << ','
                << config.window_size << ','
                << config.minimum_window_samples << ','
                << config.escalation_samples << ','
                << config.recovery_samples << ','
                << config.maximum_observation_age_ms << ','
                << static_cast<int>(snapshot.current_state) << ','
                << static_cast<int>(snapshot.pending_state) << ','
                << snapshot.pending_observations << ','
                << snapshot.last_now_ms << ','
                << snapshot.clock_initialized
                << '~' << snapshot.samples.size() << '~';
        if (snapshot.samples.empty()) encoded << '-';
        for (std::size_t index = 0; index < snapshot.samples.size(); ++index) {
            if (index != 0) encoded << ';';
            encoded << encode_safety_evidence(snapshot.samples[index]);
        }
        return encoded.str();
    }

    static safety::SafetyMonitorSnapshot decode_safety_snapshot(
        const std::string& encoded) {
        const auto parts = split_delimited(encoded, '~');
        if (parts.size() != 3) {
            throw std::invalid_argument("decision trace: malformed safety snapshot");
        }
        const auto fields = split_delimited(parts[0], ',');
        if (fields.size() != 16) {
            throw std::invalid_argument("decision trace: malformed safety snapshot header");
        }
        safety::SafetyMonitorSnapshot snapshot;
        auto& config = snapshot.config;
        std::size_t cursor = 0;
        config.duty_budget_critical_threshold = bits_double(
            parse_u64(fields[cursor++], 16, "critical duty threshold"));
        config.duty_budget_degraded_threshold = bits_double(
            parse_u64(fields[cursor++], 16, "degraded duty threshold"));
        config.duty_budget_recovery_margin = bits_double(
            parse_u64(fields[cursor++], 16, "duty recovery margin"));
        config.fail_rate_critical_threshold = bits_double(
            parse_u64(fields[cursor++], 16, "critical failure threshold"));
        config.fail_rate_degraded_threshold = bits_double(
            parse_u64(fields[cursor++], 16, "degraded failure threshold"));
        config.fail_rate_recovery_margin = bits_double(
            parse_u64(fields[cursor++], 16, "failure recovery margin"));
        config.window_size = parse_int(fields[cursor++], "safety window size");
        config.minimum_window_samples = parse_int(fields[cursor++], "minimum safety samples");
        config.escalation_samples = parse_int(fields[cursor++], "escalation samples");
        config.recovery_samples = parse_int(fields[cursor++], "recovery samples");
        config.maximum_observation_age_ms = parse_u64(
            fields[cursor++], 10, "maximum health observation age");
        snapshot.current_state = parse_safety_state(fields[cursor++]);
        snapshot.pending_state = parse_safety_state(fields[cursor++]);
        snapshot.pending_observations = parse_int(fields[cursor++], "pending observations");
        snapshot.last_now_ms = parse_u64(fields[cursor++], 10, "safety clock");
        snapshot.clock_initialized = parse_bool(fields[cursor++], "safety clock flag");

        const auto expected_samples = static_cast<std::size_t>(
            parse_u64(parts[1], 10, "safety sample count"));
        if (parts[2] != "-") {
            for (const auto& item : split_delimited(parts[2], ';')) {
                snapshot.samples.push_back(decode_safety_evidence(item));
            }
        }
        if (snapshot.samples.size() != expected_samples) {
            throw std::invalid_argument("decision trace: safety sample count mismatch");
        }
        if (const auto error = snapshot.validation_error()) {
            throw std::invalid_argument("decision trace: invalid safety snapshot: " + *error);
        }
        return snapshot;
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

    static std::string encode_descriptor_segments(
        const std::vector<transport::GenerationSegmentDescriptor>& segments) {
        std::ostringstream encoded;
        for (std::size_t index = 0; index < segments.size(); ++index) {
            if (index != 0) encoded << ';';
            const auto& segment = segments[index];
            encoded << segment.segment_id << ','
                    << segment.source_symbol_count << ','
                    << static_cast<unsigned>(segment.importance) << ','
                    << segment.expires_at_ms << ','
                    << hex_u64(double_bits(segment.target_reliability)) << ','
                    << segment.coding.emitted_symbols;
        }
        return encoded.str();
    }

    static std::vector<transport::GenerationSegmentDescriptor> decode_descriptor_segments(
        const std::string& encoded,
        std::size_t expected_count,
        std::uint64_t created_at_ms,
        std::uint64_t generation_expiry_ms,
        std::uint32_t expected_source_symbols) {
        std::vector<transport::GenerationSegmentDescriptor> segments;
        std::uint64_t source_total = 0;
        for (const auto& item : split_delimited(encoded, ';')) {
            const auto fields = split_delimited(item, ',');
            if (fields.size() != 6) {
                throw std::invalid_argument("decision trace: malformed descriptor segment");
            }
            transport::GenerationSegmentDescriptor segment;
            segment.segment_id = parse_u32(fields[0], "segment ID");
            segment.source_symbol_count = parse_u32(fields[1], "segment source symbols");
            const auto importance = parse_u32(fields[2], "segment importance");
            if (importance > static_cast<std::uint32_t>(
                    transport::TransportImportance::ELASTIC)) {
                throw std::invalid_argument("decision trace: invalid segment importance");
            }
            segment.importance = static_cast<transport::TransportImportance>(importance);
            segment.expires_at_ms = parse_u64(fields[3], 10, "segment expiry");
            if (segment.expires_at_ms < created_at_ms ||
                segment.expires_at_ms > generation_expiry_ms) {
                throw std::invalid_argument("decision trace: invalid segment expiry");
            }
            segment.deadline_ms = segment.expires_at_ms - created_at_ms;
            segment.target_reliability = bits_double(
                parse_u64(fields[4], 16, "segment reliability"));
            if (!std::isfinite(segment.target_reliability) ||
                segment.target_reliability <= 0.0 ||
                segment.target_reliability > 1.0) {
                throw std::invalid_argument("decision trace: invalid segment reliability");
            }
            segment.coding.emitted_symbols = parse_u32(
                fields[5], "segment spawn emissions");
            if (segment.segment_id != segments.size()) {
                throw std::invalid_argument("decision trace: non-contiguous segment IDs");
            }
            source_total += segment.source_symbol_count;
            segments.push_back(segment);
        }
        if (segments.size() != expected_count ||
            source_total != expected_source_symbols) {
            throw std::invalid_argument("decision trace: descriptor segment summary mismatch");
        }
        return segments;
    }

    static std::string encode_segment_states(
        const std::vector<safety::SegmentTransportState>& segments) {
        std::ostringstream encoded;
        for (std::size_t index = 0; index < segments.size(); ++index) {
            if (index != 0) encoded << ';';
            const auto& segment = segments[index];
            encoded << segment.segment_id << ','
                    << segment.emitted_symbols << ','
                    << segment.decoder_rank << ','
                    << segment.complete << ','
                    << segment.expired;
        }
        return encoded.str();
    }

    static std::vector<safety::SegmentTransportState> decode_segment_states(
        const std::string& encoded, std::size_t expected_count) {
        std::vector<safety::SegmentTransportState> segments;
        for (const auto& item : split_delimited(encoded, ';')) {
            const auto fields = split_delimited(item, ',');
            if (fields.size() != 5) {
                throw std::invalid_argument("decision trace: malformed segment runtime state");
            }
            safety::SegmentTransportState segment;
            segment.segment_id = parse_u32(fields[0], "runtime segment ID");
            segment.emitted_symbols = parse_u64(fields[1], 10, "runtime segment emissions");
            segment.decoder_rank = parse_u32(fields[2], "runtime segment rank");
            segment.complete = parse_bool(fields[3], "runtime segment completion");
            segment.expired = parse_bool(fields[4], "runtime segment expiry");
            if (segment.segment_id != segments.size()) {
                throw std::invalid_argument("decision trace: non-contiguous runtime segment IDs");
            }
            segments.push_back(segment);
        }
        if (segments.size() != expected_count) {
            throw std::invalid_argument("decision trace: runtime segment count mismatch");
        }
        return segments;
    }

    static std::string encode_attempts(
        const std::vector<safety::TransportAttemptTrace>& attempts) {
        std::ostringstream encoded;
        for (std::size_t index = 0; index < attempts.size(); ++index) {
            if (index != 0) encoded << ';';
            const auto& value = attempts[index];
            encoded << value.simulated_now_ms << ','
                    << value.packet_sequence << ',' << value.symbol_seed << ','
                    << value.segment_id << ',' << value.critical << ','
                    << value.attempted << ',' << value.hal_evaluated << ','
                    << value.hal_replayable << ',' << value.hal_accepted << ','
                    << value.transmitted << ',' << value.delivered << ','
                    << static_cast<unsigned>(value.refusal) << ','
                    << hex_u64(double_bits(value.energy_before_j)) << ','
                    << hex_u64(double_bits(value.energy_after_j)) << ','
                    << hex_u64(double_bits(value.energy_cost_j)) << ','
                    << hex_u64(double_bits(value.duty_before_s)) << ','
                    << hex_u64(double_bits(value.duty_after_s)) << ','
                    << hex_u64(double_bits(value.rf_airtime_s)) << ','
                    << value.lbt_evaluated << ',' << value.lbt_threshold_dbm << ','
                    << value.lbt_first_rssi_dbm << ',' << value.lbt_second_valid << ','
                    << value.lbt_second_rssi_dbm << ',' << value.channel_evaluated << ','
                    << hex_u64(double_bits(value.channel_snr_db)) << ','
                    << hex_u64(double_bits(value.channel_coding_gain_db)) << ','
                    << hex_u64(double_bits(value.channel_fading_db)) << ','
                    << hex_u64(double_bits(value.channel_threshold_db));
        }
        return encoded.str();
    }

    static std::vector<safety::TransportAttemptTrace> decode_attempts(
        const std::string& encoded, std::size_t expected_count) {
        std::vector<safety::TransportAttemptTrace> attempts;
        for (const auto& item : split_delimited(encoded, ';')) {
            const auto fields = split_delimited(item, ',');
            if (fields.size() != 28) {
                throw std::invalid_argument("decision trace: malformed transport attempt");
            }
            std::size_t cursor = 0;
            safety::TransportAttemptTrace value;
            value.simulated_now_ms = parse_u64(fields[cursor++], 10, "attempt time");
            value.packet_sequence = parse_u32(fields[cursor++], "packet sequence");
            value.symbol_seed = parse_u32(fields[cursor++], "symbol seed");
            value.segment_id = parse_u32(fields[cursor++], "attempt segment ID");
            value.critical = parse_bool(fields[cursor++], "attempt critical flag");
            value.attempted = parse_bool(fields[cursor++], "attempted flag");
            value.hal_evaluated = parse_bool(fields[cursor++], "HAL evaluated flag");
            value.hal_replayable = parse_bool(fields[cursor++], "HAL replayable flag");
            value.hal_accepted = parse_bool(fields[cursor++], "HAL accepted flag");
            value.transmitted = parse_bool(fields[cursor++], "transmitted flag");
            value.delivered = parse_bool(fields[cursor++], "delivered flag");
            const auto refusal = parse_u32(fields[cursor++], "attempt refusal");
            if (refusal > static_cast<std::uint32_t>(safety::AttemptRefusal::HAL)) {
                throw std::invalid_argument("decision trace: invalid attempt refusal");
            }
            value.refusal = static_cast<safety::AttemptRefusal>(refusal);
            value.energy_before_j = bits_double(parse_u64(fields[cursor++], 16, "energy before"));
            value.energy_after_j = bits_double(parse_u64(fields[cursor++], 16, "energy after"));
            value.energy_cost_j = bits_double(parse_u64(fields[cursor++], 16, "energy cost"));
            value.duty_before_s = bits_double(parse_u64(fields[cursor++], 16, "duty before"));
            value.duty_after_s = bits_double(parse_u64(fields[cursor++], 16, "duty after"));
            value.rf_airtime_s = bits_double(parse_u64(fields[cursor++], 16, "RF airtime"));
            value.lbt_evaluated = parse_bool(fields[cursor++], "LBT evaluated flag");
            value.lbt_threshold_dbm = parse_int(fields[cursor++], "LBT threshold");
            value.lbt_first_rssi_dbm = parse_int(fields[cursor++], "first LBT RSSI");
            value.lbt_second_valid = parse_bool(fields[cursor++], "second LBT flag");
            value.lbt_second_rssi_dbm = parse_int(fields[cursor++], "second LBT RSSI");
            value.channel_evaluated = parse_bool(fields[cursor++], "channel evaluated flag");
            value.channel_snr_db = bits_double(parse_u64(fields[cursor++], 16, "channel SNR"));
            value.channel_coding_gain_db = bits_double(parse_u64(fields[cursor++], 16, "channel coding gain"));
            value.channel_fading_db = bits_double(parse_u64(fields[cursor++], 16, "channel fading"));
            value.channel_threshold_db = bits_double(parse_u64(fields[cursor++], 16, "channel threshold"));
            attempts.push_back(value);
        }
        if (attempts.size() != expected_count) {
            throw std::invalid_argument("decision trace: transport attempt count mismatch");
        }
        return attempts;
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
               << '|' << hex_u64(double_bits(contract.duty_frac))
               << '|' << hex_u64(double_bits(contract.minimum_source_reserve))
               << '|' << contract.maximum_observation_age_ms
               << '|' << hex_u64(double_bits(contract.maximum_repair_amplification))
               << '|' << hex_u64(double_bits(contract.minimum_critical_overhead))
               << '|' << descriptor.created_at_ms
               << '|' << descriptor.expires_at_ms
               << '|' << descriptor.total_source_symbols
               << '|' << descriptor.segments.size()
               << '|' << encode_descriptor_segments(descriptor.segments)
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
               << '|' << trace.observed.required_rank
               << '|' << trace.observed.segments.size()
               << '|' << encode_segment_states(trace.observed.segments);
        append_decision(output, trace.proposed);
        append_decision(output, trace.decision);
        append_execution(output, trace.execution);
        output << '|' << trace.execution.attempts.size()
               << '|' << encode_attempts(trace.execution.attempts)
               << '|' << trace.constraints_applied.size()
               << '|' << encode_constraints(trace.constraints_applied)
               << '|' << record.controller.recorded
               << '|' << record.controller.now_ms
               << '|' << encode_safety_evidence(record.controller.observation)
               << '|' << encode_safety_snapshot(record.controller.before)
               << '|' << encode_safety_snapshot(record.controller.after)
               << '|' << static_cast<unsigned>(record.controller.mode_before)
               << '|' << static_cast<unsigned>(record.controller.mode_after);
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
        if (fields.size() != 61 || fields.front() != "R") {
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
        auto& controller = record.controller;
        trace.generation_id = decode_hex_bytes(fields.at(cursor++));
        descriptor.generation_id = trace.generation_id;
        contract.allow_rf = parse_bool(fields.at(cursor++), "RF permission");
        contract.allow_optical = parse_bool(fields.at(cursor++), "optical permission");
        contract.allow_backscatter = parse_bool(fields.at(cursor++), "backscatter permission");
        contract.duty_frac = bits_double(
            parse_u64(fields.at(cursor++), 16, "duty fraction"));
        contract.minimum_source_reserve = bits_double(
            parse_u64(fields.at(cursor++), 16, "reserve floor"));
        contract.maximum_observation_age_ms = parse_u64(
            fields.at(cursor++), 10, "maximum observation age");
        contract.maximum_repair_amplification = bits_double(
            parse_u64(fields.at(cursor++), 16, "repair amplification"));
        contract.minimum_critical_overhead = bits_double(
            parse_u64(fields.at(cursor++), 16, "critical overhead"));
        descriptor.created_at_ms = parse_u64(fields.at(cursor++), 10, "creation time");
        descriptor.expires_at_ms = parse_u64(fields.at(cursor++), 10, "expiry");
        descriptor.total_source_symbols = parse_u32(fields.at(cursor++), "source symbols");
        const auto descriptor_segment_count = static_cast<std::size_t>(
            parse_u64(fields.at(cursor++), 10, "descriptor segment count"));
        descriptor.segments = decode_descriptor_segments(
            fields.at(cursor++), descriptor_segment_count,
            descriptor.created_at_ms, descriptor.expires_at_ms,
            descriptor.total_source_symbols);
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
        const auto runtime_segment_count = static_cast<std::size_t>(
            parse_u64(fields.at(cursor++), 10, "runtime segment count"));
        trace.observed.segments = decode_segment_states(
            fields.at(cursor++), runtime_segment_count);
        trace.proposed = parse_decision(fields, cursor);
        trace.decision = parse_decision(fields, cursor);
        trace.execution = parse_execution(fields, cursor);
        const auto attempt_count = static_cast<std::size_t>(
            parse_u64(fields.at(cursor++), 10, "transport attempt count"));
        trace.execution.attempts = decode_attempts(
            fields.at(cursor++), attempt_count);
        const auto constraint_count = static_cast<std::size_t>(
            parse_u64(fields.at(cursor++), 10, "constraint count"));
        trace.constraints_applied = decode_constraints(fields.at(cursor++), constraint_count);
        controller.recorded = parse_bool(fields.at(cursor++), "controller-recorded flag");
        controller.now_ms = parse_u64(fields.at(cursor++), 10, "controller transition time");
        controller.observation = decode_safety_evidence(fields.at(cursor++));
        controller.before = decode_safety_snapshot(fields.at(cursor++));
        controller.after = decode_safety_snapshot(fields.at(cursor++));
        controller.mode_before = parse_operating_mode(fields.at(cursor++));
        controller.mode_after = parse_operating_mode(fields.at(cursor++));
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
        if (left.segments.size() != right.segments.size()) return false;
        for (std::size_t index = 0; index < left.segments.size(); ++index) {
            const auto& a = left.segments[index];
            const auto& b = right.segments[index];
            if (a.segment_id != b.segment_id ||
                a.emitted_symbols != b.emitted_symbols ||
                a.decoder_rank != b.decoder_rank ||
                a.complete != b.complete || a.expired != b.expired) {
                return false;
            }
        }
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

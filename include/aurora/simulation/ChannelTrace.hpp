#pragma once

#include "../transport/Generation.hpp"

#include <algorithm>
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
#include <vector>

namespace aurora::simulation {

enum class ChannelScenarioKind : std::uint8_t {
    IID,
    GILBERT_ELLIOTT,
    SCHEDULED_OUTAGE,
    SLOW_DRIFT,
    SHOCK_RECOVERY
};

inline const char* channel_scenario_name(ChannelScenarioKind kind) {
    switch (kind) {
        case ChannelScenarioKind::IID: return "iid";
        case ChannelScenarioKind::GILBERT_ELLIOTT: return "gilbert-elliott";
        case ChannelScenarioKind::SCHEDULED_OUTAGE: return "scheduled-outage";
        case ChannelScenarioKind::SLOW_DRIFT: return "slow-drift";
        case ChannelScenarioKind::SHOCK_RECOVERY: return "shock-recovery";
    }
    return "unknown";
}

inline ChannelScenarioKind parse_channel_scenario_kind(const std::string& value) {
    if (value == "iid") return ChannelScenarioKind::IID;
    if (value == "burst" || value == "gilbert-elliott") {
        return ChannelScenarioKind::GILBERT_ELLIOTT;
    }
    if (value == "outage" || value == "scheduled-outage") {
        return ChannelScenarioKind::SCHEDULED_OUTAGE;
    }
    if (value == "drift" || value == "slow-drift") {
        return ChannelScenarioKind::SLOW_DRIFT;
    }
    if (value == "shock" || value == "shock-recovery") {
        return ChannelScenarioKind::SHOCK_RECOVERY;
    }
    throw std::invalid_argument("channel trace: unknown scenario kind: " + value);
}

struct ChannelScenario {
    ChannelScenarioKind kind = ChannelScenarioKind::IID;

    double iid_loss_rate = 0.25;

    double good_state_loss_rate = 0.05;
    double bad_state_loss_rate = 0.85;
    double good_to_bad_probability = 0.08;
    double bad_to_good_probability = 0.25;

    double outage_base_loss_rate = 0.10;
    double outage_start_fraction = 0.25;
    double outage_duration_fraction = 0.25;

    double drift_start_loss_rate = 0.05;
    double drift_end_loss_rate = 0.55;

    double shock_base_loss_rate = 0.08;
    double shock_peak_loss_rate = 0.90;
    double shock_start_fraction = 0.20;
    double shock_duration_fraction = 0.20;
    double recovery_duration_fraction = 0.40;

    bool operator==(const ChannelScenario&) const = default;

    void validate() const {
        auto probability = [](double value, const char* name) {
            if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
                throw std::invalid_argument(
                    std::string("channel trace: ") + name + " must be in [0, 1]");
            }
        };
        probability(iid_loss_rate, "IID loss rate");
        probability(good_state_loss_rate, "good-state loss rate");
        probability(bad_state_loss_rate, "bad-state loss rate");
        probability(good_to_bad_probability, "good-to-bad probability");
        probability(bad_to_good_probability, "bad-to-good probability");
        probability(outage_base_loss_rate, "outage base loss rate");
        probability(outage_start_fraction, "outage start fraction");
        probability(outage_duration_fraction, "outage duration fraction");
        probability(drift_start_loss_rate, "drift start loss rate");
        probability(drift_end_loss_rate, "drift end loss rate");
        probability(shock_base_loss_rate, "shock base loss rate");
        probability(shock_peak_loss_rate, "shock peak loss rate");
        probability(shock_start_fraction, "shock start fraction");
        probability(shock_duration_fraction, "shock duration fraction");
        probability(recovery_duration_fraction, "recovery duration fraction");
        if (outage_start_fraction + outage_duration_fraction > 1.0) {
            throw std::invalid_argument(
                "channel trace: scheduled outage extends beyond the trace");
        }
        if (shock_start_fraction + shock_duration_fraction +
                recovery_duration_fraction > 1.0) {
            throw std::invalid_argument(
                "channel trace: shock and recovery extend beyond the trace");
        }
    }
};

namespace channel_trace_detail {

inline std::string hex_u64(std::uint64_t value) {
    std::ostringstream encoded;
    encoded << std::hex << std::setw(16) << std::setfill('0') << value;
    return encoded.str();
}

inline std::uint64_t parse_u64(const std::string& encoded,
                               int base,
                               const char* field) {
    if (encoded.empty() || encoded.front() == '-') {
        throw std::invalid_argument(std::string("channel trace: invalid ") + field);
    }
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(encoded, &consumed, base);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("channel trace: invalid ") + field);
    }
    if (consumed != encoded.size()) {
        throw std::invalid_argument(std::string("channel trace: invalid ") + field);
    }
    return static_cast<std::uint64_t>(parsed);
}

inline char hex_digit(unsigned value) {
    static constexpr char digits[] = "0123456789abcdef";
    return digits[value & 0x0FU];
}

inline unsigned hex_nibble(char value) {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
    throw std::invalid_argument("channel trace: invalid hexadecimal data");
}

inline std::string hex_bytes(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        encoded.push_back(hex_digit(byte >> 4U));
        encoded.push_back(hex_digit(byte));
    }
    return encoded;
}

inline std::string decode_hex_bytes(std::string_view encoded) {
    if (encoded.size() % 2 != 0) {
        throw std::invalid_argument("channel trace: hexadecimal text has odd length");
    }
    std::string value;
    value.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        value.push_back(static_cast<char>(
            (hex_nibble(encoded[i]) << 4U) | hex_nibble(encoded[i + 1])));
    }
    return value;
}

inline std::string encode_outcomes(const std::vector<std::uint8_t>& outcomes) {
    std::vector<std::uint8_t> packed((outcomes.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (outcomes[i] > 1) {
            throw std::invalid_argument("channel trace: outcomes must be binary");
        }
        packed[i / 8] |= static_cast<std::uint8_t>(outcomes[i] << (i % 8));
    }
    std::string encoded;
    encoded.reserve(packed.size() * 2);
    for (const auto byte : packed) {
        encoded.push_back(hex_digit(byte >> 4U));
        encoded.push_back(hex_digit(byte));
    }
    return encoded;
}

inline std::vector<std::uint8_t> decode_outcomes(const std::string& encoded,
                                                 std::size_t outcome_count) {
    if (encoded.size() != ((outcome_count + 7) / 8) * 2) {
        throw std::invalid_argument("channel trace: packed outcome length mismatch");
    }
    std::vector<std::uint8_t> packed(encoded.size() / 2, 0);
    for (std::size_t i = 0; i < packed.size(); ++i) {
        packed[i] = static_cast<std::uint8_t>(
            (hex_nibble(encoded[i * 2]) << 4U) | hex_nibble(encoded[i * 2 + 1]));
    }
    if (!packed.empty() && outcome_count % 8 != 0) {
        const auto used_bits = static_cast<unsigned>(outcome_count % 8);
        const auto unused_mask = static_cast<std::uint8_t>(0xFFU << used_bits);
        if ((packed.back() & unused_mask) != 0) {
            throw std::invalid_argument("channel trace: non-canonical trailing outcome bits");
        }
    }
    std::vector<std::uint8_t> outcomes(outcome_count, 0);
    for (std::size_t i = 0; i < outcome_count; ++i) {
        outcomes[i] = static_cast<std::uint8_t>((packed[i / 8] >> (i % 8)) & 1U);
    }
    return outcomes;
}

inline std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto separator = value.find('|', start);
        if (separator == std::string::npos) {
            fields.push_back(value.substr(start));
            return fields;
        }
        fields.push_back(value.substr(start, separator - start));
        start = separator + 1;
    }
}

inline std::string probability_bits(double value) {
    return hex_u64(std::bit_cast<std::uint64_t>(value));
}

} // namespace channel_trace_detail

inline std::string channel_scenario_id(const ChannelScenario& scenario) {
    scenario.validate();
    using channel_trace_detail::probability_bits;
    std::ostringstream encoded;
    encoded << channel_scenario_name(scenario.kind);
    switch (scenario.kind) {
        case ChannelScenarioKind::IID:
            encoded << ':' << probability_bits(scenario.iid_loss_rate);
            break;
        case ChannelScenarioKind::GILBERT_ELLIOTT:
            encoded << ':' << probability_bits(scenario.good_state_loss_rate)
                    << ':' << probability_bits(scenario.bad_state_loss_rate)
                    << ':' << probability_bits(scenario.good_to_bad_probability)
                    << ':' << probability_bits(scenario.bad_to_good_probability);
            break;
        case ChannelScenarioKind::SCHEDULED_OUTAGE:
            encoded << ':' << probability_bits(scenario.outage_base_loss_rate)
                    << ':' << probability_bits(scenario.outage_start_fraction)
                    << ':' << probability_bits(scenario.outage_duration_fraction);
            break;
        case ChannelScenarioKind::SLOW_DRIFT:
            encoded << ':' << probability_bits(scenario.drift_start_loss_rate)
                    << ':' << probability_bits(scenario.drift_end_loss_rate);
            break;
        case ChannelScenarioKind::SHOCK_RECOVERY:
            encoded << ':' << probability_bits(scenario.shock_base_loss_rate)
                    << ':' << probability_bits(scenario.shock_peak_loss_rate)
                    << ':' << probability_bits(scenario.shock_start_fraction)
                    << ':' << probability_bits(scenario.shock_duration_fraction)
                    << ':' << probability_bits(scenario.recovery_duration_fraction);
            break;
    }
    return encoded.str();
}

inline ChannelScenario channel_scenario_from_id(const std::string& identifier) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto separator = identifier.find(':', start);
        if (separator == std::string::npos) {
            fields.push_back(identifier.substr(start));
            break;
        }
        fields.push_back(identifier.substr(start, separator - start));
        start = separator + 1;
    }
    if (fields.empty()) {
        throw std::invalid_argument("channel trace: scenario identifier is empty");
    }
    ChannelScenario scenario;
    scenario.kind = parse_channel_scenario_kind(fields[0]);
    std::size_t cursor = 1;
    auto probability = [&]() {
        if (cursor >= fields.size()) {
            throw std::invalid_argument("channel trace: incomplete scenario identifier");
        }
        return std::bit_cast<double>(channel_trace_detail::parse_u64(
            fields[cursor++], 16, "scenario probability"));
    };
    switch (scenario.kind) {
        case ChannelScenarioKind::IID:
            scenario.iid_loss_rate = probability();
            break;
        case ChannelScenarioKind::GILBERT_ELLIOTT:
            scenario.good_state_loss_rate = probability();
            scenario.bad_state_loss_rate = probability();
            scenario.good_to_bad_probability = probability();
            scenario.bad_to_good_probability = probability();
            break;
        case ChannelScenarioKind::SCHEDULED_OUTAGE:
            scenario.outage_base_loss_rate = probability();
            scenario.outage_start_fraction = probability();
            scenario.outage_duration_fraction = probability();
            break;
        case ChannelScenarioKind::SLOW_DRIFT:
            scenario.drift_start_loss_rate = probability();
            scenario.drift_end_loss_rate = probability();
            break;
        case ChannelScenarioKind::SHOCK_RECOVERY:
            scenario.shock_base_loss_rate = probability();
            scenario.shock_peak_loss_rate = probability();
            scenario.shock_start_fraction = probability();
            scenario.shock_duration_fraction = probability();
            scenario.recovery_duration_fraction = probability();
            break;
    }
    if (cursor != fields.size()) {
        throw std::invalid_argument("channel trace: trailing scenario identifier fields");
    }
    scenario.validate();
    if (channel_scenario_id(scenario) != identifier) {
        throw std::invalid_argument("channel trace: non-canonical scenario identifier");
    }
    return scenario;
}

struct ChannelTrace {
    std::size_t trial_index = 0;
    std::uint64_t seed = 0;
    std::string scenario_id;
    // 1 means delivered and 0 means lost for the corresponding transmission slot.
    std::vector<std::uint8_t> outcomes;

    [[nodiscard]] bool delivered(std::size_t slot) const {
        if (slot >= outcomes.size()) {
            throw std::out_of_range("channel trace: transmission slot exceeds trace length");
        }
        return outcomes[slot] != 0;
    }

    bool operator==(const ChannelTrace&) const = default;
};

class ChannelTraceGenerator {
public:
    [[nodiscard]] ChannelTrace generate(const ChannelScenario& scenario,
                                        std::uint64_t experiment_seed,
                                        std::size_t trial_index,
                                        std::size_t slot_count) const {
        scenario.validate();
        if (slot_count == 0) {
            throw std::invalid_argument("channel trace: slot count must be positive");
        }

        ChannelTrace trace;
        trace.trial_index = trial_index;
        trace.seed = derive_seed(experiment_seed, trial_index);
        trace.scenario_id = channel_scenario_id(scenario);
        trace.outcomes.resize(slot_count, 0);

        std::uint64_t random_state = trace.seed;
        bool bad_state = false;
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
            double loss_probability = 0.0;
            switch (scenario.kind) {
                case ChannelScenarioKind::IID:
                    loss_probability = scenario.iid_loss_rate;
                    break;
                case ChannelScenarioKind::GILBERT_ELLIOTT:
                    loss_probability = bad_state
                        ? scenario.bad_state_loss_rate
                        : scenario.good_state_loss_rate;
                    break;
                case ChannelScenarioKind::SCHEDULED_OUTAGE:
                    loss_probability = inside_window(
                        slot, slot_count,
                        scenario.outage_start_fraction,
                        scenario.outage_duration_fraction)
                        ? 1.0
                        : scenario.outage_base_loss_rate;
                    break;
                case ChannelScenarioKind::SLOW_DRIFT: {
                    const auto progress = slot_count == 1
                        ? 0.0
                        : static_cast<double>(slot) / static_cast<double>(slot_count - 1);
                    loss_probability = scenario.drift_start_loss_rate +
                        (scenario.drift_end_loss_rate - scenario.drift_start_loss_rate) * progress;
                    break;
                }
                case ChannelScenarioKind::SHOCK_RECOVERY:
                    loss_probability = shock_loss_probability(scenario, slot, slot_count);
                    break;
            }

            trace.outcomes[slot] = next_unit(random_state) >= loss_probability ? 1U : 0U;
            if (scenario.kind == ChannelScenarioKind::GILBERT_ELLIOTT) {
                const auto transition = next_unit(random_state);
                if (bad_state) {
                    if (transition < scenario.bad_to_good_probability) bad_state = false;
                } else if (transition < scenario.good_to_bad_probability) {
                    bad_state = true;
                }
            }
        }
        return trace;
    }

private:
    static std::uint64_t splitmix64(std::uint64_t value) {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    static std::uint64_t derive_seed(std::uint64_t experiment_seed,
                                     std::size_t trial_index) {
        return splitmix64(
            experiment_seed ^
            (static_cast<std::uint64_t>(trial_index) * 0xD1B54A32D192ED03ULL));
    }

    static double next_unit(std::uint64_t& state) {
        state = splitmix64(state);
        return static_cast<double>(state >> 11U) * 0x1.0p-53;
    }

    static bool inside_window(std::size_t slot,
                              std::size_t slot_count,
                              double start_fraction,
                              double duration_fraction) {
        const auto start = static_cast<std::size_t>(std::floor(
            start_fraction * static_cast<double>(slot_count)));
        const auto end = static_cast<std::size_t>(std::floor(
            (start_fraction + duration_fraction) * static_cast<double>(slot_count)));
        return slot >= start && slot < std::min(end, slot_count);
    }

    static double shock_loss_probability(const ChannelScenario& scenario,
                                         std::size_t slot,
                                         std::size_t slot_count) {
        const auto start = scenario.shock_start_fraction * static_cast<double>(slot_count);
        const auto shock_end = start +
            scenario.shock_duration_fraction * static_cast<double>(slot_count);
        const auto recovery_end = shock_end +
            scenario.recovery_duration_fraction * static_cast<double>(slot_count);
        const auto position = static_cast<double>(slot);
        if (position < start || position >= recovery_end) {
            return scenario.shock_base_loss_rate;
        }
        if (position < shock_end) {
            return scenario.shock_peak_loss_rate;
        }
        const auto recovery_span = recovery_end - shock_end;
        if (recovery_span <= 0.0) return scenario.shock_base_loss_rate;
        const auto progress = (position - shock_end) / recovery_span;
        return scenario.shock_peak_loss_rate +
            (scenario.shock_base_loss_rate - scenario.shock_peak_loss_rate) * progress;
    }
};

class ChannelTraceCorpus {
public:
    static constexpr std::string_view format_magic = "AURORA_CHANNEL_TRACE_V1";

    std::uint64_t experiment_seed = 0;
    std::string scenario_id;
    std::vector<ChannelTrace> traces;

    [[nodiscard]] std::string serialize() const {
        std::ostringstream output;
        if (scenario_id.empty()) {
            throw std::invalid_argument("channel trace: corpus scenario ID is required");
        }
        (void)channel_scenario_from_id(scenario_id);
        const auto header = std::string(format_magic) + '|' +
            channel_trace_detail::hex_u64(experiment_seed) + '|' +
            channel_trace_detail::hex_bytes(scenario_id);
        output << header << '\n';
        std::uint64_t previous_checksum = transport::fnv1a64(header);
        for (std::size_t index = 0; index < traces.size(); ++index) {
            const auto& trace = traces[index];
            validate_trace(trace, index, scenario_id);
            std::ostringstream payload;
            payload << "T|" << index
                    << '|' << trace.trial_index
                    << '|' << channel_trace_detail::hex_u64(trace.seed)
                    << '|' << channel_trace_detail::hex_bytes(trace.scenario_id)
                    << '|' << trace.outcomes.size()
                    << '|' << channel_trace_detail::encode_outcomes(trace.outcomes)
                    << '|' << channel_trace_detail::hex_u64(previous_checksum);
            const auto encoded = payload.str();
            const auto checksum = transport::fnv1a64(encoded);
            output << encoded << '|' << channel_trace_detail::hex_u64(checksum) << '\n';
            previous_checksum = checksum;
        }
        const auto footer = std::string("END|") + std::to_string(traces.size()) +
            '|' + channel_trace_detail::hex_u64(previous_checksum);
        output << footer << '|' << channel_trace_detail::hex_u64(
            transport::fnv1a64(footer)) << '\n';
        return output.str();
    }

    [[nodiscard]] std::string fingerprint() const {
        return channel_trace_detail::hex_u64(transport::fnv1a64(serialize()));
    }

    void save(const std::string& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("channel trace: cannot open output file: " + path);
        }
        const auto encoded = serialize();
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            throw std::runtime_error("channel trace: failed while writing: " + path);
        }
    }

    [[nodiscard]] static ChannelTraceCorpus load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("channel trace: cannot open input file: " + path);
        }
        std::ostringstream encoded;
        encoded << input.rdbuf();
        if (!input.good() && !input.eof()) {
            throw std::runtime_error("channel trace: failed while reading: " + path);
        }
        return deserialize(encoded.str());
    }

    [[nodiscard]] static ChannelTraceCorpus deserialize(const std::string& encoded) {
        std::istringstream input(encoded);
        std::string line;
        if (!std::getline(input, line)) {
            throw std::invalid_argument("channel trace: unsupported or missing format header");
        }

        ChannelTraceCorpus corpus;
        const auto header = channel_trace_detail::split(line);
        if (header.size() != 3 || header[0] != format_magic) {
            throw std::invalid_argument("channel trace: unsupported or missing format header");
        }
        corpus.experiment_seed = channel_trace_detail::parse_u64(
            header[1], 16, "experiment seed");
        corpus.scenario_id = channel_trace_detail::decode_hex_bytes(header[2]);
        (void)channel_scenario_from_id(corpus.scenario_id);
        std::uint64_t previous_checksum = transport::fnv1a64(line);
        bool footer_seen = false;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (footer_seen) {
                throw std::invalid_argument("channel trace: data follows the end marker");
            }
            const auto checksum_separator = line.rfind('|');
            if (checksum_separator == std::string::npos) {
                throw std::invalid_argument("channel trace: checksum is missing");
            }
            const auto payload = line.substr(0, checksum_separator);
            const auto checksum = channel_trace_detail::parse_u64(
                line.substr(checksum_separator + 1), 16, "checksum");
            if (transport::fnv1a64(payload) != checksum) {
                throw std::invalid_argument("channel trace: checksum mismatch");
            }
            if (payload.starts_with("END|")) {
                verify_footer(payload, corpus.traces.size(), previous_checksum);
                footer_seen = true;
                continue;
            }
            corpus.traces.push_back(decode_trace(
                payload, corpus.traces.size(), previous_checksum, corpus.scenario_id));
            previous_checksum = checksum;
        }
        if (!footer_seen) {
            throw std::invalid_argument("channel trace: end marker is missing");
        }
        return corpus;
    }

    bool operator==(const ChannelTraceCorpus&) const = default;

private:
    static void validate_trace(const ChannelTrace& trace,
                               std::size_t expected_index,
                               const std::string& expected_scenario_id) {
        if (trace.trial_index != expected_index || trace.scenario_id.empty() ||
            trace.scenario_id != expected_scenario_id || trace.outcomes.empty()) {
            throw std::invalid_argument("channel trace: invalid or non-contiguous trace");
        }
        if (std::any_of(trace.outcomes.begin(), trace.outcomes.end(),
                        [](std::uint8_t value) { return value > 1; })) {
            throw std::invalid_argument("channel trace: outcomes must be binary");
        }
    }

    static ChannelTrace decode_trace(const std::string& payload,
                                     std::size_t expected_index,
                                     std::uint64_t expected_previous_checksum,
                                     const std::string& expected_scenario_id) {
        const auto fields = channel_trace_detail::split(payload);
        if (fields.size() != 8 || fields[0] != "T") {
            throw std::invalid_argument("channel trace: malformed trace record");
        }
        if (channel_trace_detail::parse_u64(fields[1], 10, "trace index") != expected_index ||
            channel_trace_detail::parse_u64(fields[2], 10, "trial index") != expected_index) {
            throw std::invalid_argument("channel trace: non-contiguous trace index");
        }
        if (channel_trace_detail::parse_u64(fields[7], 16, "previous checksum") !=
            expected_previous_checksum) {
            throw std::invalid_argument("channel trace: checksum chain mismatch");
        }

        ChannelTrace trace;
        trace.trial_index = expected_index;
        trace.seed = channel_trace_detail::parse_u64(fields[3], 16, "trace seed");
        trace.scenario_id = channel_trace_detail::decode_hex_bytes(fields[4]);
        const auto count_u64 = channel_trace_detail::parse_u64(fields[5], 10, "slot count");
        if (count_u64 == 0 || count_u64 > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("channel trace: slot count is out of range");
        }
        trace.outcomes = channel_trace_detail::decode_outcomes(
            fields[6], static_cast<std::size_t>(count_u64));
        validate_trace(trace, expected_index, expected_scenario_id);
        return trace;
    }

    static void verify_footer(const std::string& payload,
                              std::size_t expected_count,
                              std::uint64_t expected_previous_checksum) {
        const auto fields = channel_trace_detail::split(payload);
        if (fields.size() != 3 || fields[0] != "END" ||
            channel_trace_detail::parse_u64(fields[1], 10, "trace count") != expected_count ||
            channel_trace_detail::parse_u64(fields[2], 16, "final checksum") !=
                expected_previous_checksum) {
            throw std::invalid_argument("channel trace: incomplete checksum chain");
        }
    }
};

} // namespace aurora::simulation

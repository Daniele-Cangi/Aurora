#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::transport {

enum class TransportImportance : std::uint8_t {
    CRITICAL,
    IMPORTANT,
    ELASTIC
};

struct SegmentRequirement {
    std::size_t offset = 0;
    std::size_t length = 0;
    TransportImportance importance = TransportImportance::IMPORTANT;
    // These two values are currently retained as declared metadata. Global
    // generation expiry and policy selection are operational; independent
    // segment expiry/reliability enforcement is intentionally not claimed.
    std::uint64_t deadline_ms = 0;
    double target_reliability = 0.99;
};

enum class ContractFieldSemantics : std::uint8_t {
    ENFORCED,
    POLICY_INPUT,
    METADATA_ONLY,
    UNSUPPORTED
};

struct ContractFieldAudit {
    std::string_view field;
    ContractFieldSemantics semantics;
    std::string_view runtime_effect;
};

// Truth table for the public contract surface. Keep this list synchronized with
// TransportContract; tests make omissions and accidental semantic inflation visible.
inline constexpr std::array<ContractFieldAudit, 21> transport_contract_semantic_audit{{
    {"version", ContractFieldSemantics::ENFORCED, "validated parser/API version"},
    {"deadline_s", ContractFieldSemantics::ENFORCED, "absolute generation expiry"},
    {"reliability", ContractFieldSemantics::POLICY_INPUT, "flow class and protection plan input; not an SLA"},
    {"duty_frac", ContractFieldSemantics::ENFORCED, "simulation-time RF airtime budget and policy input"},
    {"allowed_links", ContractFieldSemantics::ENFORCED, "SafetyEnvelope link admission/replacement"},
    {"ris_tiles", ContractFieldSemantics::ENFORCED, "simulator RIS tile count"},
    {"selector_argmax", ContractFieldSemantics::POLICY_INPUT, "link selector choice"},
    {"importance", ContractFieldSemantics::POLICY_INPUT, "default segmentation and protection"},
    {"minimum_source_reserve", ContractFieldSemantics::ENFORCED, "pre-action energy floor"},
    {"maximum_observation_age_ms", ContractFieldSemantics::ENFORCED, "SafetyEnvelope freshness bound"},
    {"maximum_repair_amplification", ContractFieldSemantics::ENFORCED, "initial and runtime repair cap"},
    {"minimum_critical_overhead", ContractFieldSemantics::ENFORCED, "critical protection floor"},
    {"maximum_generation_bytes", ContractFieldSemantics::ENFORCED, "spawn size limit"},
    {"maximum_source_symbols", ContractFieldSemantics::ENFORCED, "spawn source-symbol limit"},
    {"require_payload_integrity", ContractFieldSemantics::ENFORCED, "terminal payload digest check"},
    {"experiment_seed", ContractFieldSemantics::ENFORCED, "deterministic generation and simulator randomness"},
    {"segments.range", ContractFieldSemantics::ENFORCED, "explicit byte segmentation"},
    {"segments.importance", ContractFieldSemantics::ENFORCED, "segment protection and critical scheduling"},
    {"segments.deadline_ms", ContractFieldSemantics::METADATA_ONLY, "recorded in descriptor; no independent expiry"},
    {"segments.target_reliability", ContractFieldSemantics::METADATA_ONLY, "recorded in descriptor; no independent guarantee"},
    {"payload_semantics", ContractFieldSemantics::UNSUPPORTED, "opaque bytes only"},
}};

// Application-facing requirements for moving opaque bytes.  This type deliberately
// contains transport properties only; it has no payload-semantic fields.
struct TransportContract {
    std::uint16_t version = 1;

    // Compatibility-shaped names are retained while the monolithic simulator is
    // split up. They are now parsed and validated instead of being ignored.
    double deadline_s = 600.0;
    double reliability = 0.99;
    double duty_frac = 0.01;
    bool allow_rf = true;
    bool allow_optical = true;
    bool allow_backscatter = true;
    int ris_tiles = 16;
    bool selector_argmax = true;

    TransportImportance importance = TransportImportance::IMPORTANT;
    double minimum_source_reserve = 0.05;
    std::uint64_t maximum_observation_age_ms = 5'000;
    double maximum_repair_amplification = 4.0;
    double minimum_critical_overhead = 1.5;
    std::size_t maximum_generation_bytes = 16U * 1024U * 1024U;
    std::uint32_t maximum_source_symbols = 4096;
    bool require_payload_integrity = true;
    std::uint64_t experiment_seed = 0xA607AULL;
    std::vector<SegmentRequirement> segments;

    [[nodiscard]] std::uint64_t deadline_ms() const {
        return static_cast<std::uint64_t>(std::llround(deadline_s * 1000.0));
    }

    void validate() const {
        if (version != 1) {
            throw std::invalid_argument("transport contract: unsupported version");
        }
        if (!std::isfinite(deadline_s) || deadline_s < 0.0) {
            throw std::invalid_argument("transport contract: deadline must be finite and non-negative");
        }
        if (!std::isfinite(reliability) || reliability <= 0.0 || reliability > 1.0) {
            throw std::invalid_argument("transport contract: reliability must be in (0, 1]");
        }
        if (!std::isfinite(duty_frac) || duty_frac < 0.0 || duty_frac > 1.0) {
            throw std::invalid_argument("transport contract: duty must be in [0, 1]");
        }
        if (!allow_rf && !allow_optical && !allow_backscatter) {
            throw std::invalid_argument("transport contract: at least one link mode must be allowed");
        }
        if (ris_tiles < 0) {
            throw std::invalid_argument("transport contract: ris tile count cannot be negative");
        }
        if (!std::isfinite(minimum_source_reserve) || minimum_source_reserve < 0.0 ||
            minimum_source_reserve > 1.0) {
            throw std::invalid_argument("transport contract: reserve floor must be in [0, 1]");
        }
        if (!std::isfinite(maximum_repair_amplification) || maximum_repair_amplification < 1.0) {
            throw std::invalid_argument("transport contract: maximum repair amplification must be >= 1");
        }
        if (!std::isfinite(minimum_critical_overhead) || minimum_critical_overhead < 1.0 ||
            minimum_critical_overhead > maximum_repair_amplification) {
            throw std::invalid_argument(
                "transport contract: critical overhead must be within [1, maximum repair amplification]");
        }
        if (maximum_generation_bytes == 0) {
            throw std::invalid_argument("transport contract: maximum generation size must be positive");
        }
        if (maximum_source_symbols == 0) {
            throw std::invalid_argument("transport contract: maximum source symbol count must be positive");
        }

        std::vector<SegmentRequirement> ordered = segments;
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });
        std::size_t previous_end = 0;
        bool first = true;
        for (const auto& segment : ordered) {
            if (segment.length == 0) {
                throw std::invalid_argument("transport contract: segment length must be positive");
            }
            if (segment.offset > std::numeric_limits<std::size_t>::max() - segment.length) {
                throw std::invalid_argument("transport contract: segment byte range overflows");
            }
            if (!first && segment.offset < previous_end) {
                throw std::invalid_argument("transport contract: segment byte ranges overlap");
            }
            if (segment.target_reliability <= 0.0 || segment.target_reliability > 1.0 ||
                !std::isfinite(segment.target_reliability)) {
                throw std::invalid_argument("transport contract: segment reliability must be in (0, 1]");
            }
            previous_end = segment.offset + segment.length;
            first = false;
        }
    }

    static TransportContract parse(const std::string& text) {
        TransportContract contract;
        std::stringstream input(text);
        std::string entry;

        while (std::getline(input, entry, ';')) {
            entry = trim(entry);
            if (entry.empty()) {
                continue;
            }
            const auto separator = entry.find(':');
            if (separator == std::string::npos) {
                throw std::invalid_argument("transport contract: expected key:value entry: " + entry);
            }
            const std::string key = lower(trim(entry.substr(0, separator)));
            const std::string value = trim(entry.substr(separator + 1));
            if (value.empty()) {
                throw std::invalid_argument("transport contract: empty value for " + key);
            }

            if (key == "deadline") {
                contract.deadline_s = parse_duration_ms(value) / 1000.0;
            } else if (key == "reliability") {
                contract.reliability = parse_double(value, key);
            } else if (key == "duty" || key == "duty_frac") {
                contract.duty_frac = parse_double(value, key);
            } else if (key == "rf") {
                contract.allow_rf = parse_bool(value, key);
            } else if (key == "optical") {
                contract.allow_optical = parse_bool(value, key);
            } else if (key == "backscatter") {
                contract.allow_backscatter = parse_bool(value, key);
            } else if (key == "ris") {
                contract.ris_tiles = parse_int(value, key);
            } else if (key == "selector") {
                const auto selector = lower(value);
                if (selector == "argmax") {
                    contract.selector_argmax = true;
                } else if (selector == "ucb") {
                    contract.selector_argmax = false;
                } else {
                    throw std::invalid_argument("transport contract: selector must be argmax or ucb");
                }
            } else if (key == "importance" || key == "class") {
                contract.importance = parse_importance(value);
            } else if (key == "reserve" || key == "reserve_floor") {
                contract.minimum_source_reserve = parse_double(value, key);
            } else if (key == "max_observation_age") {
                contract.maximum_observation_age_ms = parse_duration_ms(value);
            } else if (key == "max_repair_amplification") {
                contract.maximum_repair_amplification = parse_double(value, key);
            } else if (key == "min_critical_overhead") {
                contract.minimum_critical_overhead = parse_double(value, key);
            } else if (key == "max_generation_bytes") {
                const auto parsed = parse_u64(value, key);
                if (parsed > std::numeric_limits<std::size_t>::max()) {
                    throw std::invalid_argument("transport contract: maximum generation size is out of range");
                }
                contract.maximum_generation_bytes = static_cast<std::size_t>(parsed);
            } else if (key == "max_source_symbols") {
                const auto parsed = parse_u64(value, key);
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::invalid_argument("transport contract: maximum source symbol count is out of range");
                }
                contract.maximum_source_symbols = static_cast<std::uint32_t>(parsed);
            } else if (key == "integrity") {
                contract.require_payload_integrity = parse_bool(value, key);
            } else if (key == "seed") {
                contract.experiment_seed = parse_u64(value, key);
            } else if (key == "segment") {
                contract.segments.push_back(parse_segment(value, contract));
            } else {
                throw std::invalid_argument("transport contract: unknown key: " + key);
            }
        }

        contract.validate();
        return contract;
    }

private:
    static std::string trim(std::string value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        const auto trim_end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }).base();
        if (first >= trim_end) {
            return {};
        }
        return std::string(first, trim_end);
    }

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    static double parse_double(const std::string& value, const std::string& key) {
        std::size_t consumed = 0;
        double parsed = 0.0;
        try {
            parsed = std::stod(value, &consumed);
        } catch (const std::exception&) {
            throw std::invalid_argument("transport contract: invalid number for " + key);
        }
        if (consumed != value.size() || !std::isfinite(parsed)) {
            throw std::invalid_argument("transport contract: invalid number for " + key);
        }
        return parsed;
    }

    static int parse_int(const std::string& value, const std::string& key) {
        std::size_t consumed = 0;
        long parsed = 0;
        try {
            parsed = std::stol(value, &consumed, 10);
        } catch (const std::exception&) {
            throw std::invalid_argument("transport contract: invalid integer for " + key);
        }
        if (consumed != value.size() || parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            throw std::invalid_argument("transport contract: invalid integer for " + key);
        }
        return static_cast<int>(parsed);
    }

    static std::uint64_t parse_u64(const std::string& value, const std::string& key) {
        std::size_t consumed = 0;
        unsigned long long parsed = 0;
        try {
            parsed = std::stoull(value, &consumed, 0);
        } catch (const std::exception&) {
            throw std::invalid_argument("transport contract: invalid unsigned integer for " + key);
        }
        if (consumed != value.size()) {
            throw std::invalid_argument("transport contract: invalid unsigned integer for " + key);
        }
        return static_cast<std::uint64_t>(parsed);
    }

    static bool parse_bool(const std::string& value, const std::string& key) {
        const auto normalized = lower(trim(value));
        if (normalized == "on" || normalized == "true" || normalized == "yes" || normalized == "1") {
            return true;
        }
        if (normalized == "off" || normalized == "false" || normalized == "no" || normalized == "0") {
            return false;
        }
        throw std::invalid_argument("transport contract: invalid boolean for " + key);
    }

    static std::uint64_t parse_duration_ms(const std::string& raw) {
        const std::string value = lower(trim(raw));
        double multiplier = 1000.0;
        std::string number = value;
        if (value.size() >= 2 && value.ends_with("ms")) {
            multiplier = 1.0;
            number = value.substr(0, value.size() - 2);
        } else if (!value.empty() && value.back() == 's') {
            number = value.substr(0, value.size() - 1);
        }
        const double parsed = parse_double(trim(number), "duration");
        if (parsed < 0.0 || parsed * multiplier > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            throw std::invalid_argument("transport contract: duration is out of range");
        }
        return static_cast<std::uint64_t>(std::llround(parsed * multiplier));
    }

    static TransportImportance parse_importance(const std::string& raw) {
        const auto value = lower(trim(raw));
        if (value == "critical") {
            return TransportImportance::CRITICAL;
        }
        if (value == "important" || value == "normal") {
            return TransportImportance::IMPORTANT;
        }
        if (value == "elastic" || value == "bulk") {
            return TransportImportance::ELASTIC;
        }
        throw std::invalid_argument("transport contract: importance must be critical, important, or elastic");
    }

    static SegmentRequirement parse_segment(const std::string& value, const TransportContract& contract) {
        std::stringstream stream(value);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(stream, field, ',')) {
            fields.push_back(trim(field));
        }
        if (fields.size() < 2 || fields.size() > 4) {
            throw std::invalid_argument(
                "transport contract: segment must be start-end,importance[,deadline[,reliability]]");
        }

        const auto dash = fields[0].find('-');
        if (dash == std::string::npos) {
            throw std::invalid_argument("transport contract: segment range must be inclusive start-end");
        }
        const auto start = parse_u64(trim(fields[0].substr(0, dash)), "segment start");
        const auto end = parse_u64(trim(fields[0].substr(dash + 1)), "segment end");
        if (end < start || end - start >= std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("transport contract: invalid segment range");
        }

        SegmentRequirement segment;
        segment.offset = static_cast<std::size_t>(start);
        segment.length = static_cast<std::size_t>(end - start + 1);
        segment.importance = parse_importance(fields[1]);
        segment.deadline_ms = fields.size() >= 3 ? parse_duration_ms(fields[2]) : contract.deadline_ms();
        segment.target_reliability = fields.size() >= 4
            ? parse_double(fields[3], "segment reliability")
            : contract.reliability;
        return segment;
    }
};

} // namespace aurora::transport

// Transitional source compatibility for the existing simulator and experiments.
// New code should use aurora::transport::TransportContract explicitly.
using Intention = aurora::transport::TransportContract;

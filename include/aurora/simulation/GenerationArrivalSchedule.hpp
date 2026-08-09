#pragma once

#include "../transport/Generation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::simulation {

struct GenerationArrival {
    std::uint64_t arrives_at_ms = 0;
    std::string tag;

    friend bool operator==(const GenerationArrival&,
                           const GenerationArrival&) = default;
};

// Canonical external-generation arrivals. Times are unique, strictly ordered,
// and aligned to the simulator's one-second step boundary. Tags are stable
// payload identities rather than arbitrary user content.
class GenerationArrivalSchedule {
public:
    static constexpr std::string_view format_header =
        "AURORA_GENERATION_ARRIVAL_SCHEDULE_V1";
    // Matches the default bounded GenerationManager capacity used by the
    // simulator, which pre-constructs descriptors before timed release.
    static constexpr std::size_t maximum_arrivals = 128;

    GenerationArrivalSchedule() = default;

    explicit GenerationArrivalSchedule(std::vector<GenerationArrival> arrivals)
        : arrivals_(std::move(arrivals)) {
        validate();
    }

    [[nodiscard]] static GenerationArrivalSchedule single_immediate() {
        return GenerationArrivalSchedule({{0, "primary"}});
    }

    [[nodiscard]] const std::vector<GenerationArrival>& arrivals() const {
        return arrivals_;
    }

    void validate() const {
        if (arrivals_.empty()) {
            throw std::invalid_argument(
                "generation arrival schedule: at least one arrival is required");
        }
        if (arrivals_.size() > maximum_arrivals) {
            throw std::invalid_argument(
                "generation arrival schedule: too many arrivals");
        }
        if (arrivals_.front().arrives_at_ms != 0) {
            throw std::invalid_argument(
                "generation arrival schedule: first arrival must be at zero");
        }
        std::uint64_t previous_time = 0;
        std::vector<std::string> tags;
        tags.reserve(arrivals_.size());
        for (std::size_t index = 0; index < arrivals_.size(); ++index) {
            const auto& arrival = arrivals_[index];
            if (arrival.arrives_at_ms % 1000ULL != 0ULL) {
                throw std::invalid_argument(
                    "generation arrival schedule: arrival time must align to 1000 ms");
            }
            if (index > 0 && arrival.arrives_at_ms <= previous_time) {
                throw std::invalid_argument(
                    "generation arrival schedule: arrivals must be strictly ordered");
            }
            if (!valid_tag(arrival.tag)) {
                throw std::invalid_argument(
                    "generation arrival schedule: invalid tag");
            }
            if (std::find(tags.begin(), tags.end(), arrival.tag) != tags.end()) {
                throw std::invalid_argument(
                    "generation arrival schedule: duplicate tag");
            }
            tags.push_back(arrival.tag);
            previous_time = arrival.arrives_at_ms;
        }
    }

    [[nodiscard]] std::uint64_t fingerprint() const {
        return transport::fnv1a64(serialize());
    }

    [[nodiscard]] std::string serialize() const {
        validate();
        std::ostringstream output;
        output << format_header << '\n';
        std::uint64_t previous_checksum = 0;
        for (std::size_t index = 0; index < arrivals_.size(); ++index) {
            const auto& arrival = arrivals_[index];
            std::ostringstream payload;
            payload << "A|" << index << '|'
                    << hex_u64(previous_checksum) << '|'
                    << arrival.arrives_at_ms << '|' << arrival.tag;
            const auto encoded = payload.str();
            previous_checksum = transport::fnv1a64(encoded);
            output << encoded << '|' << hex_u64(previous_checksum) << '\n';
        }
        const auto footer = std::string("END|") +
            std::to_string(arrivals_.size()) + '|' + hex_u64(previous_checksum);
        output << footer << '|'
               << hex_u64(transport::fnv1a64(footer)) << '\n';
        return output.str();
    }

    void save(const std::string& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "generation arrival schedule: cannot open output file: " + path);
        }
        const auto encoded = serialize();
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            throw std::runtime_error(
                "generation arrival schedule: failed while writing: " + path);
        }
    }

    [[nodiscard]] static GenerationArrivalSchedule load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "generation arrival schedule: cannot open input file: " + path);
        }
        std::ostringstream encoded;
        encoded << input.rdbuf();
        if (!input.good() && !input.eof()) {
            throw std::runtime_error(
                "generation arrival schedule: failed while reading: " + path);
        }
        return deserialize(encoded.str());
    }

    [[nodiscard]] static GenerationArrivalSchedule deserialize(
        const std::string& encoded) {
        std::istringstream input(encoded);
        std::string line;
        if (!std::getline(input, line) || line != format_header) {
            throw std::invalid_argument(
                "generation arrival schedule: unsupported or missing format header");
        }
        std::vector<GenerationArrival> arrivals;
        std::uint64_t previous_checksum = 0;
        bool footer_seen = false;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (footer_seen) {
                throw std::invalid_argument(
                    "generation arrival schedule: data follows end marker");
            }
            const auto checksum_separator = line.rfind('|');
            if (checksum_separator == std::string::npos) {
                throw std::invalid_argument(
                    "generation arrival schedule: record checksum is missing");
            }
            const auto payload = line.substr(0, checksum_separator);
            const auto checksum = parse_u64(
                line.substr(checksum_separator + 1), 16, "checksum");
            if (transport::fnv1a64(payload) != checksum) {
                throw std::invalid_argument(
                    "generation arrival schedule: record checksum mismatch");
            }
            const auto fields = split(payload);
            if (!fields.empty() && fields[0] == "END") {
                if (fields.size() != 3 ||
                    parse_u64(fields[1], 10, "arrival count") != arrivals.size() ||
                    parse_u64(fields[2], 16, "final chain") != previous_checksum) {
                    throw std::invalid_argument(
                        "generation arrival schedule: invalid end marker");
                }
                footer_seen = true;
                continue;
            }
            if (fields.size() != 5 || fields[0] != "A" ||
                parse_u64(fields[1], 10, "arrival index") != arrivals.size() ||
                parse_u64(fields[2], 16, "previous checksum") !=
                    previous_checksum) {
                throw std::invalid_argument(
                    "generation arrival schedule: malformed or non-contiguous arrival");
            }
            if (arrivals.size() >= maximum_arrivals) {
                throw std::invalid_argument(
                    "generation arrival schedule: too many arrivals");
            }
            arrivals.push_back({
                parse_u64(fields[3], 10, "arrival time"), fields[4]});
            previous_checksum = checksum;
        }
        if (!footer_seen) {
            throw std::invalid_argument(
                "generation arrival schedule: end marker is missing");
        }
        return GenerationArrivalSchedule(std::move(arrivals));
    }

    friend bool operator==(const GenerationArrivalSchedule&,
                           const GenerationArrivalSchedule&) = default;

private:
    static bool valid_tag(const std::string& tag) {
        return !tag.empty() && tag.size() <= 64 &&
            std::all_of(tag.begin(), tag.end(), [](unsigned char value) {
                return (value >= 'a' && value <= 'z') ||
                    (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') || value == '-' ||
                    value == '_' || value == '.';
            });
    }

    static std::string hex_u64(std::uint64_t value) {
        std::ostringstream output;
        output << std::hex << std::setw(16) << std::setfill('0') << value;
        return output.str();
    }

    static std::uint64_t parse_u64(const std::string& text, int base,
                                   const char* field) {
        std::size_t consumed = 0;
        std::uint64_t value = 0;
        try {
            value = std::stoull(text, &consumed, base);
        } catch (...) {
            throw std::invalid_argument(
                std::string("generation arrival schedule: invalid ") + field);
        }
        if (consumed != text.size()) {
            throw std::invalid_argument(
                std::string("generation arrival schedule: invalid ") + field);
        }
        return value;
    }

    static std::vector<std::string> split(const std::string& value) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto end = value.find('|', start);
            fields.push_back(value.substr(start, end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return fields;
    }

    std::vector<GenerationArrival> arrivals_;
};

} // namespace aurora::simulation

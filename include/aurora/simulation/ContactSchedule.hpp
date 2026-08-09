#pragma once

#include "../transport/Generation.hpp"

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

namespace aurora::simulation {

struct ContactAvailability {
    bool rf = false;
    bool optical = false;
    bool backscatter = false;

    [[nodiscard]] bool any() const {
        return rf || optical || backscatter;
    }

    [[nodiscard]] std::uint8_t mask() const {
        return static_cast<std::uint8_t>(
            (rf ? 0x1U : 0U) |
            (optical ? 0x2U : 0U) |
            (backscatter ? 0x4U : 0U));
    }

    [[nodiscard]] static ContactAvailability from_mask(std::uint8_t mask) {
        if ((mask & ~0x7U) != 0U) {
            throw std::invalid_argument("contact schedule: invalid link mask");
        }
        return {
            (mask & 0x1U) != 0U,
            (mask & 0x2U) != 0U,
            (mask & 0x4U) != 0U};
    }

    friend bool operator==(const ContactAvailability&,
                           const ContactAvailability&) = default;
};

struct ContactWindow {
    std::uint64_t starts_at_ms = 0;
    std::uint64_t ends_at_ms = 0;
    ContactAvailability available;

    friend bool operator==(const ContactWindow&, const ContactWindow&) = default;
};

// Canonical half-open contact windows: [starts_at_ms, ends_at_ms). Gaps mean
// that no link is available. Windows may touch but never overlap.
class ContactSchedule {
public:
    static constexpr std::string_view format_header =
        "AURORA_CONTACT_SCHEDULE_V1";

    ContactSchedule() = default;

    explicit ContactSchedule(std::vector<ContactWindow> windows)
        : windows_(std::move(windows)) {
        validate();
    }

    [[nodiscard]] static ContactSchedule always_available() {
        return ContactSchedule({{
            0,
            std::numeric_limits<std::uint64_t>::max(),
            {true, true, true}}});
    }

    [[nodiscard]] const std::vector<ContactWindow>& windows() const {
        return windows_;
    }

    [[nodiscard]] ContactAvailability availability_at(
        std::uint64_t now_ms) const {
        for (const auto& window : windows_) {
            if (now_ms < window.starts_at_ms) break;
            if (now_ms < window.ends_at_ms) return window.available;
        }
        return {};
    }

    void validate() const {
        if (windows_.size() > 4096) {
            throw std::invalid_argument(
                "contact schedule: too many windows");
        }
        std::uint64_t previous_end = 0;
        bool first = true;
        for (const auto& window : windows_) {
            if (window.starts_at_ms >= window.ends_at_ms) {
                throw std::invalid_argument(
                    "contact schedule: window must be non-empty");
            }
            if (!first && window.starts_at_ms < previous_end) {
                throw std::invalid_argument(
                    "contact schedule: windows overlap or are unordered");
            }
            if (window.available.mask() > 0x7U) {
                throw std::invalid_argument(
                    "contact schedule: invalid availability");
            }
            previous_end = window.ends_at_ms;
            first = false;
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
        for (std::size_t index = 0; index < windows_.size(); ++index) {
            const auto& window = windows_[index];
            std::ostringstream payload;
            payload << "W|" << index << '|'
                    << hex_u64(previous_checksum) << '|'
                    << window.starts_at_ms << '|'
                    << window.ends_at_ms << '|'
                    << static_cast<unsigned>(window.available.mask());
            const auto encoded = payload.str();
            previous_checksum = transport::fnv1a64(encoded);
            output << encoded << '|' << hex_u64(previous_checksum) << '\n';
        }
        const auto footer = std::string("END|") +
            std::to_string(windows_.size()) + '|' + hex_u64(previous_checksum);
        output << footer << '|'
               << hex_u64(transport::fnv1a64(footer)) << '\n';
        return output.str();
    }

    void save(const std::string& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "contact schedule: cannot open output file: " + path);
        }
        const auto encoded = serialize();
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            throw std::runtime_error(
                "contact schedule: failed while writing: " + path);
        }
    }

    [[nodiscard]] static ContactSchedule load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "contact schedule: cannot open input file: " + path);
        }
        std::ostringstream encoded;
        encoded << input.rdbuf();
        if (!input.good() && !input.eof()) {
            throw std::runtime_error(
                "contact schedule: failed while reading: " + path);
        }
        return deserialize(encoded.str());
    }

    [[nodiscard]] static ContactSchedule deserialize(
        const std::string& encoded) {
        std::istringstream input(encoded);
        std::string line;
        if (!std::getline(input, line) || line != format_header) {
            throw std::invalid_argument(
                "contact schedule: unsupported or missing format header");
        }
        std::vector<ContactWindow> windows;
        std::uint64_t previous_checksum = 0;
        bool footer_seen = false;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (footer_seen) {
                throw std::invalid_argument(
                    "contact schedule: data follows end marker");
            }
            const auto checksum_separator = line.rfind('|');
            if (checksum_separator == std::string::npos) {
                throw std::invalid_argument(
                    "contact schedule: record checksum is missing");
            }
            const auto payload = line.substr(0, checksum_separator);
            const auto checksum = parse_u64(
                line.substr(checksum_separator + 1), 16, "checksum");
            if (transport::fnv1a64(payload) != checksum) {
                throw std::invalid_argument(
                    "contact schedule: record checksum mismatch");
            }
            const auto fields = split(payload);
            if (!fields.empty() && fields[0] == "END") {
                if (fields.size() != 3 ||
                    parse_u64(fields[1], 10, "window count") != windows.size() ||
                    parse_u64(fields[2], 16, "final chain") !=
                        previous_checksum) {
                    throw std::invalid_argument(
                        "contact schedule: invalid end marker");
                }
                footer_seen = true;
                continue;
            }
            if (fields.size() != 6 || fields[0] != "W" ||
                parse_u64(fields[1], 10, "window index") != windows.size() ||
                parse_u64(fields[2], 16, "previous checksum") !=
                    previous_checksum) {
                throw std::invalid_argument(
                    "contact schedule: malformed or non-contiguous window");
            }
            const auto mask = parse_u64(fields[5], 10, "link mask");
            if (mask > 0x7U) {
                throw std::invalid_argument("contact schedule: invalid link mask");
            }
            if (windows.size() >= 4096) {
                throw std::invalid_argument(
                    "contact schedule: too many windows");
            }
            windows.push_back({
                parse_u64(fields[3], 10, "window start"),
                parse_u64(fields[4], 10, "window end"),
                ContactAvailability::from_mask(static_cast<std::uint8_t>(mask))});
            previous_checksum = checksum;
        }
        if (!footer_seen) {
            throw std::invalid_argument("contact schedule: end marker is missing");
        }
        return ContactSchedule(std::move(windows));
    }

    friend bool operator==(const ContactSchedule&,
                           const ContactSchedule&) = default;

private:
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
                std::string("contact schedule: invalid ") + field);
        }
        if (consumed != text.size()) {
            throw std::invalid_argument(
                std::string("contact schedule: invalid ") + field);
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

    std::vector<ContactWindow> windows_;
};

} // namespace aurora::simulation

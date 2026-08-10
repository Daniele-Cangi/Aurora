#pragma once

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::emulation {

enum class ImpairmentAction { PASS, DROP, DUPLICATE };

struct ImpairmentDirective {
    ImpairmentAction action = ImpairmentAction::PASS;
    std::uint32_t delay_ms = 0;
};

class ImpairmentTrace {
public:
    static ImpairmentTrace parse(std::string_view text) {
        std::istringstream input{std::string(text)};
        std::string magic, name_line, actions_line, checksum_line, extra;
        std::getline(input, magic);
        std::getline(input, name_line);
        std::getline(input, actions_line);
        std::getline(input, checksum_line);
        strip_carriage_return(magic);
        strip_carriage_return(name_line);
        strip_carriage_return(actions_line);
        strip_carriage_return(checksum_line);
        if (std::getline(input, extra) && !extra.empty()) {
            throw std::invalid_argument("impairment trace: trailing content");
        }
        const bool v1 = magic == "AURORA_IMPAIRMENT_TRACE_V1";
        const bool v2 = magic == "AURORA_IMPAIRMENT_TRACE_V2";
        if ((!v1 && !v2) || !name_line.starts_with("name=") ||
            !(v1 ? actions_line.starts_with("actions=")
                 : actions_line.starts_with("events=")) ||
            !checksum_line.starts_with("checksum=")) {
            throw std::invalid_argument("impairment trace: invalid envelope");
        }
        ImpairmentTrace trace;
        trace.name_ = name_line.substr(5);
        if (trace.name_.empty()) {
            throw std::invalid_argument("impairment trace: empty name");
        }
        trace.version_ = v1 ? 1 : 2;
        const auto encoded = actions_line.substr(v1 ? 8 : 7);
        if (v1) {
            for (char value : encoded) {
                trace.directives_.push_back({parse_action(value), 0});
            }
        } else {
            std::size_t begin = 0;
            while (begin <= encoded.size()) {
                const auto end = encoded.find(',', begin);
                const auto token = encoded.substr(
                    begin, end == std::string::npos ? end : end - begin);
                const auto separator = token.find('@');
                if (separator != 1 || token.size() < 3) {
                    throw std::invalid_argument(
                        "impairment trace: invalid timed event");
                }
                std::size_t consumed = 0;
                const auto delay = std::stoul(token.substr(2), &consumed, 10);
                if (consumed != token.size() - 2 || delay > 60'000) {
                    throw std::invalid_argument(
                        "impairment trace: invalid event delay");
                }
                trace.directives_.push_back({
                    parse_action(token.front()),
                    static_cast<std::uint32_t>(delay)});
                if (end == std::string::npos) break;
                begin = end + 1;
            }
        }
        if (trace.directives_.empty()) {
            throw std::invalid_argument("impairment trace: no actions");
        }
        const auto canonical = magic + "\n" + name_line + "\n" + actions_line + "\n";
        if (checksum_line.substr(9) != hex(fnv1a64(canonical))) {
            throw std::invalid_argument("impairment trace: checksum mismatch");
        }
        trace.fingerprint_ = fnv1a64(canonical);
        return trace;
    }

    static ImpairmentTrace load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::invalid_argument("impairment trace: cannot open " + path);
        std::ostringstream contents;
        contents << input.rdbuf();
        return parse(contents.str());
    }

    static std::string make(std::string_view name, std::string_view actions) {
        const std::string canonical = "AURORA_IMPAIRMENT_TRACE_V1\nname=" +
            std::string(name) + "\nactions=" + std::string(actions) + "\n";
        return canonical + "checksum=" + hex(fnv1a64(canonical)) + "\n";
    }

    static std::string make_timed(std::string_view name,
                                  std::string_view events) {
        const std::string canonical = "AURORA_IMPAIRMENT_TRACE_V2\nname=" +
            std::string(name) + "\nevents=" + std::string(events) + "\n";
        return canonical + "checksum=" + hex(fnv1a64(canonical)) + "\n";
    }

    [[nodiscard]] ImpairmentAction action(std::uint64_t attempt) const {
        return directive(attempt).action;
    }
    [[nodiscard]] ImpairmentDirective directive(std::uint64_t attempt) const {
        return directives_[attempt % directives_.size()];
    }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] std::uint64_t fingerprint() const { return fingerprint_; }
    [[nodiscard]] std::size_t period() const { return directives_.size(); }
    [[nodiscard]] int version() const { return version_; }

private:
    static ImpairmentAction parse_action(char value) {
        switch (value) {
            case 'P': return ImpairmentAction::PASS;
            case 'D': return ImpairmentAction::DROP;
            case 'U': return ImpairmentAction::DUPLICATE;
            default: throw std::invalid_argument(
                "impairment trace: invalid action");
        }
    }

    static void strip_carriage_return(std::string& line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }

    static std::uint64_t fnv1a64(std::string_view bytes) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
        return hash;
    }
    static std::string hex(std::uint64_t value) {
        std::ostringstream output;
        output << std::hex << std::setfill('0') << std::setw(16) << value;
        return output.str();
    }

    std::string name_;
    std::vector<ImpairmentDirective> directives_;
    std::uint64_t fingerprint_ = 0;
    int version_ = 0;
};

} // namespace aurora::emulation

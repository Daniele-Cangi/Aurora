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
        if (magic != "AURORA_IMPAIRMENT_TRACE_V1" ||
            !name_line.starts_with("name=") ||
            !actions_line.starts_with("actions=") ||
            !checksum_line.starts_with("checksum=")) {
            throw std::invalid_argument("impairment trace: invalid V1 envelope");
        }
        ImpairmentTrace trace;
        trace.name_ = name_line.substr(5);
        if (trace.name_.empty()) {
            throw std::invalid_argument("impairment trace: empty name");
        }
        const auto encoded = actions_line.substr(8);
        for (char value : encoded) {
            switch (value) {
                case 'P': trace.actions_.push_back(ImpairmentAction::PASS); break;
                case 'D': trace.actions_.push_back(ImpairmentAction::DROP); break;
                case 'U': trace.actions_.push_back(ImpairmentAction::DUPLICATE); break;
                default: throw std::invalid_argument("impairment trace: invalid action");
            }
        }
        if (trace.actions_.empty()) {
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

    [[nodiscard]] ImpairmentAction action(std::uint64_t attempt) const {
        return actions_[attempt % actions_.size()];
    }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] std::uint64_t fingerprint() const { return fingerprint_; }
    [[nodiscard]] std::size_t period() const { return actions_.size(); }

private:
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
    std::vector<ImpairmentAction> actions_;
    std::uint64_t fingerprint_ = 0;
};

} // namespace aurora::emulation

#include "aurora/simulation/ContactSchedule.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint64_t number(const std::string& text, const char* field) {
    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(text, &consumed, 0);
    } catch (...) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    return value;
}

aurora::simulation::ContactAvailability links(const std::string& text) {
    if (text == "none") return {};
    if (text == "all") return {true, true, true};
    aurora::simulation::ContactAvailability result;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find('+', start);
        const auto link = text.substr(start, end - start);
        if (link == "rf") result.rf = true;
        else if (link == "optical") result.optical = true;
        else if (link == "backscatter") result.backscatter = true;
        else throw std::invalid_argument("invalid contact link set: " + text);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

aurora::simulation::ContactWindow window(const std::string& specification) {
    const auto first = specification.find(':');
    const auto second = specification.find(':', first == std::string::npos
        ? first : first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        specification.find(':', second + 1) != std::string::npos) {
        throw std::invalid_argument(
            "window must be <start-ms>:<end-ms>:<links>");
    }
    return {
        number(specification.substr(0, first), "window start"),
        number(specification.substr(first + 1, second - first - 1),
               "window end"),
        links(specification.substr(second + 1))};
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr
            << "usage: aurora_contact_schedule <output-file> "
               "<start-ms>:<end-ms>:<links> [...]\n"
            << "links: none, all, or rf+optical+backscatter subsets\n";
        return 2;
    }
    try {
        std::vector<aurora::simulation::ContactWindow> windows;
        windows.reserve(static_cast<std::size_t>(argc - 2));
        for (int index = 2; index < argc; ++index) {
            windows.push_back(window(argv[index]));
        }
        const aurora::simulation::ContactSchedule schedule(std::move(windows));
        schedule.save(argv[1]);
        std::cout << "CONTACT_SCHEDULE_OK windows="
                  << schedule.windows().size()
                  << " fingerprint=" << schedule.fingerprint() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CONTACT_SCHEDULE_INVALID reason=" << error.what() << '\n';
        return 1;
    }
}

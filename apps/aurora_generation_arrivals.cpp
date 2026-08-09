#include "aurora/simulation/GenerationArrivalSchedule.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint64_t number(const std::string& text) {
    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(text, &consumed, 0);
    } catch (...) {
        throw std::invalid_argument("invalid arrival time");
    }
    if (consumed != text.size()) {
        throw std::invalid_argument("invalid arrival time");
    }
    return value;
}

aurora::simulation::GenerationArrival arrival(
    const std::string& specification) {
    const auto separator = specification.find(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == specification.size() ||
        specification.find(':', separator + 1) != std::string::npos) {
        throw std::invalid_argument("arrival must be <time-ms>:<tag>");
    }
    return {number(specification.substr(0, separator)),
            specification.substr(separator + 1)};
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "usage: aurora_generation_arrivals <output-file> "
                     "<time-ms>:<tag> [...]\n";
        return 2;
    }
    try {
        std::vector<aurora::simulation::GenerationArrival> arrivals;
        arrivals.reserve(static_cast<std::size_t>(argc - 2));
        for (int index = 2; index < argc; ++index) {
            arrivals.push_back(arrival(argv[index]));
        }
        const aurora::simulation::GenerationArrivalSchedule schedule(
            std::move(arrivals));
        schedule.save(argv[1]);
        std::cout << "GENERATION_ARRIVAL_SCHEDULE_OK arrivals="
                  << schedule.arrivals().size()
                  << " fingerprint=" << schedule.fingerprint() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GENERATION_ARRIVAL_SCHEDULE_INVALID reason="
                  << error.what() << '\n';
        return 1;
    }
}

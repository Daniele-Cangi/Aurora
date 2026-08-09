#include "aurora/simulation/GenerationArrivalSchedule.hpp"

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

aurora::simulation::GenerationArrival arrival(
    const std::string& specification) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto separator = specification.find(':', start);
        fields.push_back(specification.substr(start, separator - start));
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    if ((fields.size() != 2 && fields.size() != 4) ||
        fields[0].empty() || fields[1].empty()) {
        throw std::invalid_argument(
            "arrival must be <time-ms>:<tag> or "
            "<time-ms>:<tag>:<class>:<deadline-ms|inherit>");
    }
    aurora::simulation::GenerationArrival result{
        number(fields[0], "arrival time"), fields[1]};
    if (fields.size() == 2) return result;
    if (fields[2] == "inherit") {
        result.service_class =
            aurora::simulation::GenerationServiceClass::INHERIT;
    } else if (fields[2] == "critical") {
        result.service_class =
            aurora::simulation::GenerationServiceClass::CRITICAL;
    } else if (fields[2] == "important") {
        result.service_class =
            aurora::simulation::GenerationServiceClass::IMPORTANT;
    } else if (fields[2] == "elastic") {
        result.service_class =
            aurora::simulation::GenerationServiceClass::ELASTIC;
    } else {
        throw std::invalid_argument("invalid generation service class");
    }
    result.deadline_ms = fields[3] == "inherit"
        ? 0 : number(fields[3], "generation deadline");
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr
            << "usage: aurora_generation_arrivals <output-file> "
               "<time-ms>:<tag> [...]\n"
            << "extended: <time-ms>:<tag>:"
               "<inherit|critical|important|elastic>:"
               "<deadline-ms|inherit>\n";
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

#include "../include/aurora/simulation/GenerationArrivalSchedule.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using aurora::simulation::GenerationArrivalSchedule;

    const GenerationArrivalSchedule schedule({
        {0, "alpha"}, {2'000, "beta-2"}, {9'000, "gamma_3"}});
    const auto encoded = schedule.serialize();
    assert(encoded.starts_with("AURORA_GENERATION_ARRIVAL_SCHEDULE_V1\n"));
    const auto restored = GenerationArrivalSchedule::deserialize(encoded);
    assert(restored == schedule);
    assert(restored.serialize() == encoded);
    assert(restored.fingerprint() == schedule.fingerprint());

    bool unaligned_rejected = false;
    try {
        (void)GenerationArrivalSchedule({{0, "alpha"}, {1'500, "beta"}});
    } catch (const std::invalid_argument&) {
        unaligned_rejected = true;
    }
    assert(unaligned_rejected);

    bool duplicate_rejected = false;
    try {
        (void)GenerationArrivalSchedule({{0, "alpha"}, {1'000, "alpha"}});
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    assert(duplicate_rejected);

    auto corrupted = encoded;
    const auto first = corrupted.find("A|");
    assert(first != std::string::npos);
    corrupted[first + 2] = '9';
    bool corruption_rejected = false;
    try {
        (void)GenerationArrivalSchedule::deserialize(corrupted);
    } catch (const std::invalid_argument&) {
        corruption_rejected = true;
    }
    assert(corruption_rejected);

    const auto footer = encoded.find("END|");
    assert(footer != std::string::npos);
    bool truncation_rejected = false;
    try {
        (void)GenerationArrivalSchedule::deserialize(encoded.substr(0, footer));
    } catch (const std::invalid_argument&) {
        truncation_rejected = true;
    }
    assert(truncation_rejected);

    std::cout << "generation arrival schedule tests passed\n";
    return 0;
}

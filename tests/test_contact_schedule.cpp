#include "../include/aurora/simulation/ContactSchedule.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

int main() {
    using aurora::simulation::ContactAvailability;
    using aurora::simulation::ContactSchedule;

    const ContactSchedule schedule({
        {0, 1'000, {true, false, false}},
        {2'000, 3'000, {false, true, true}},
        {3'000, std::numeric_limits<std::uint64_t>::max(),
         {true, true, true}}});

    assert(schedule.availability_at(0) ==
           (ContactAvailability{true, false, false}));
    assert(schedule.availability_at(999).rf);
    assert(!schedule.availability_at(1'000).any());
    assert(!schedule.availability_at(1'999).any());
    assert(schedule.availability_at(2'000) ==
           (ContactAvailability{false, true, true}));
    assert(schedule.availability_at(3'000) ==
           (ContactAvailability{true, true, true}));

    const auto encoded = schedule.serialize();
    assert(encoded.starts_with("AURORA_CONTACT_SCHEDULE_V1\n"));
    const auto restored = ContactSchedule::deserialize(encoded);
    assert(restored == schedule);
    assert(restored.serialize() == encoded);
    assert(restored.fingerprint() == schedule.fingerprint());

    bool overlap_rejected = false;
    try {
        (void)ContactSchedule({
            {0, 2'000, {true, false, false}},
            {1'000, 3'000, {false, true, false}}});
    } catch (const std::invalid_argument&) {
        overlap_rejected = true;
    }
    assert(overlap_rejected);

    auto corrupted = encoded;
    const auto first_window = corrupted.find("W|");
    assert(first_window != std::string::npos);
    corrupted[first_window + 2] = '9';
    bool corruption_rejected = false;
    try {
        (void)ContactSchedule::deserialize(corrupted);
    } catch (const std::invalid_argument&) {
        corruption_rejected = true;
    }
    assert(corruption_rejected);

    const auto footer = encoded.find("END|");
    assert(footer != std::string::npos);
    bool truncation_rejected = false;
    try {
        (void)ContactSchedule::deserialize(encoded.substr(0, footer));
    } catch (const std::invalid_argument&) {
        truncation_rejected = true;
    }
    assert(truncation_rejected);

    std::cout << "contact schedule tests passed\n";
    return 0;
}

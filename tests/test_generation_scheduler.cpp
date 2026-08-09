#include "../include/aurora/simulation/GenerationScheduler.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    using aurora::simulation::GenerationSchedulingCandidate;
    using aurora::simulation::select_scheduled_generation;
    using aurora::transport::TransportImportance;

    std::vector<GenerationSchedulingCandidate> candidates{
        {0, 0, 5'000, TransportImportance::ELASTIC, true, false},
        {1, 1'000, 9'000, TransportImportance::CRITICAL, true, false},
        {2, 2'000, 4'000, TransportImportance::IMPORTANT, true, false}};
    assert(select_scheduled_generation(candidates, 2'000) == 1);

    candidates[1].terminal = true;
    assert(select_scheduled_generation(candidates, 2'000) == 2);

    candidates[2].expires_at_ms = 5'000;
    assert(select_scheduled_generation(candidates, 2'000) == 2);

    candidates[2].importance = TransportImportance::ELASTIC;
    assert(select_scheduled_generation(candidates, 2'000) == 0);

    candidates[0].terminal = true;
    candidates[2].terminal = true;
    assert(!select_scheduled_generation(candidates, 2'000));

    bool invalid_rejected = false;
    try {
        (void)select_scheduled_generation({{
            0, 2'000, 1'000, TransportImportance::IMPORTANT, true, false}},
            2'000);
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    assert(invalid_rejected);

    std::cout << "generation scheduler tests passed\n";
    return 0;
}

#include "../include/aurora/simulation/GenerationScheduler.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    using aurora::simulation::GenerationSchedulingCandidate;
    using aurora::simulation::GenerationSchedulingPolicy;
    using aurora::simulation::maximum_service_gap_ms;
    using aurora::simulation::select_scheduled_generation;
    using aurora::transport::TransportImportance;

    std::vector<GenerationSchedulingCandidate> candidates{
        {0, 0, 5'000, TransportImportance::ELASTIC, true, false, {}},
        {1, 1'000, 9'000, TransportImportance::CRITICAL, true, false, {}},
        {2, 2'000, 4'000, TransportImportance::IMPORTANT, true, false, {}}};
    assert(select_scheduled_generation(candidates, 2'000) == 1);

    candidates[1].terminal = true;
    assert(select_scheduled_generation(candidates, 2'000) == 2);

    candidates[2].expires_at_ms = 5'000;
    assert(select_scheduled_generation(candidates, 2'000) == 0);

    candidates[2].importance = TransportImportance::ELASTIC;
    assert(select_scheduled_generation(candidates, 2'000) == 0);

    const GenerationSchedulingPolicy policy{1'000, 2'000, 3'000};
    std::vector<GenerationSchedulingCandidate> aging{
        {0, 0, 7'000, TransportImportance::ELASTIC, true, false, 0},
        {1, 0, 9'000, TransportImportance::CRITICAL, true, false, 3'000}};
    // Two aging intervals promote elastic to critical, then EDF wins.
    assert(select_scheduled_generation(aging, 4'000, policy) == 0);

    aging[0].expires_at_ms = 50'000;
    aging[1].expires_at_ms = 5'000;
    // The fairness deadline overrides both base priority and EDF.
    assert(select_scheduled_generation(aging, 3'000, policy) == 0);

    std::vector<GenerationSchedulingCandidate> bounded{
        {0, 0, 50'000, TransportImportance::CRITICAL, true, false, {}},
        {1, 0, 50'000, TransportImportance::CRITICAL, true, false, {}},
        {2, 0, 50'000, TransportImportance::CRITICAL, true, false, {}}};
    assert(select_scheduled_generation(bounded, 3'000, policy) == 0);
    bounded[0].last_served_at_ms = 3'000;
    assert(select_scheduled_generation(bounded, 4'000, policy) == 1);
    bounded[1].last_served_at_ms = 4'000;
    assert(select_scheduled_generation(bounded, 5'000, policy) == 2);
    assert(maximum_service_gap_ms(policy, bounded.size()) == 5'000);

    candidates[0].terminal = true;
    candidates[2].terminal = true;
    assert(!select_scheduled_generation(candidates, 2'000));

    bool invalid_rejected = false;
    try {
        (void)select_scheduled_generation({{
            0, 2'000, 1'000, TransportImportance::IMPORTANT, true, false, {}}},
            2'000);
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    assert(invalid_rejected);

    bool future_service_rejected = false;
    try {
        (void)select_scheduled_generation({{
            0, 0, 10'000, TransportImportance::IMPORTANT, true, false,
            2'001}}, 2'000, policy);
    } catch (const std::invalid_argument&) {
        future_service_rejected = true;
    }
    assert(future_service_rejected);

    bool invalid_policy_rejected = false;
    try {
        (void)select_scheduled_generation(candidates, 2'000, {0, 1, 1});
    } catch (const std::invalid_argument&) {
        invalid_policy_rejected = true;
    }
    assert(invalid_policy_rejected);

    std::cout << "generation scheduler tests passed\n";
    return 0;
}

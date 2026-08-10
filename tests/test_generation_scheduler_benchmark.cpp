#include "../include/aurora/simulation/GenerationSchedulerBenchmark.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    using aurora::simulation::GenerationSchedulingPolicy;
    using aurora::simulation::compare_adversarial_scheduler_policies;

    const auto comparison = compare_adversarial_scheduler_policies();
    assert(comparison == compare_adversarial_scheduler_policies());
    assert(comparison.strict.candidates == 3);
    assert(comparison.strict.steps == 20);
    assert(comparison.strict.selections.front() == 0);
    assert(comparison.strict.bound_violations > 0);
    assert(comparison.strict.maximum_observed_gap_ms >
           comparison.strict.comparison_bound_ms);

    assert(comparison.fair.bound_violations == 0);
    assert(comparison.fair.maximum_observed_gap_ms <=
           comparison.fair.comparison_bound_ms);
    assert(std::all_of(
        comparison.fair.selections.begin(), comparison.fair.selections.end(),
        [](std::uint64_t selections) { return selections > 0; }));
    assert(aurora::simulation::generation_scheduler_benchmark_gate_passes(
        comparison.strict));
    assert(aurora::simulation::generation_scheduler_benchmark_gate_passes(
        comparison.fair));

    const GenerationSchedulingPolicy slower{
        1'000, 4'000, 6'000,
        aurora::simulation::GenerationSchedulingDiscipline::AGING_FAIR};
    const auto custom = compare_adversarial_scheduler_policies(slower, 30, 3);
    assert(custom.fair.comparison_bound_ms == 9'000);
    assert(custom.fair.bound_violations == 0);
    assert(custom.strict.bound_violations > 0);

    const auto sweep = aurora::simulation::
        run_canonical_generation_scheduler_benchmark_sweep();
    assert(sweep == aurora::simulation::
        run_canonical_generation_scheduler_benchmark_sweep());
    assert(sweep.scenarios == 7);
    assert(sweep.failed_gates == 0);
    assert(sweep.report.starts_with(
        "AURORA_GENERATION_SCHEDULER_SWEEP_V1\n"));

    auto regressed = comparison.fair;
    regressed.bound_violations = 1;
    assert(!aurora::simulation::generation_scheduler_benchmark_gate_passes(
        regressed));

    bool invalid_rejected = false;
    try {
        (void)compare_adversarial_scheduler_policies({}, 1, 2);
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    assert(invalid_rejected);

    std::cout << "generation scheduler benchmark tests passed\n";
    return 0;
}

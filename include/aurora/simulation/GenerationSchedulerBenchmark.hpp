#pragma once

#include "GenerationScheduler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::simulation {

struct GenerationSchedulerBenchmarkMetrics {
    GenerationSchedulingDiscipline discipline =
        GenerationSchedulingDiscipline::AGING_FAIR;
    std::uint64_t steps = 0;
    std::size_t candidates = 0;
    std::uint64_t comparison_bound_ms = 0;
    std::uint64_t maximum_observed_gap_ms = 0;
    std::size_t bound_violations = 0;
    std::vector<std::uint64_t> selections;
    std::vector<std::uint64_t> maximum_gap_by_candidate_ms;

    friend bool operator==(const GenerationSchedulerBenchmarkMetrics&,
                           const GenerationSchedulerBenchmarkMetrics&) = default;
};

struct GenerationSchedulerBenchmarkComparison {
    GenerationSchedulerBenchmarkMetrics strict;
    GenerationSchedulerBenchmarkMetrics fair;

    friend bool operator==(const GenerationSchedulerBenchmarkComparison&,
                           const GenerationSchedulerBenchmarkComparison&) = default;
};

struct GenerationSchedulerBenchmarkScenario {
    std::string_view name;
    std::uint64_t steps;
    std::size_t critical_contenders;
    std::uint64_t aging_interval_ms;
    std::uint64_t starvation_limit_ms;

    friend bool operator==(const GenerationSchedulerBenchmarkScenario&,
                           const GenerationSchedulerBenchmarkScenario&) = default;
};

struct GenerationSchedulerBenchmarkSweep {
    std::string report;
    std::size_t scenarios = 0;
    std::size_t failed_gates = 0;

    friend bool operator==(const GenerationSchedulerBenchmarkSweep&,
                           const GenerationSchedulerBenchmarkSweep&) = default;
};

inline GenerationSchedulerBenchmarkMetrics run_adversarial_scheduler_benchmark(
    GenerationSchedulingPolicy policy,
    GenerationSchedulingDiscipline discipline,
    std::uint64_t steps,
    std::size_t critical_contenders) {
    policy.validate();
    if (steps < 2 || critical_contenders == 0 ||
        critical_contenders >= 128) {
        throw std::invalid_argument(
            "generation scheduler benchmark: invalid scenario bounds");
    }
    if (steps > std::numeric_limits<std::uint64_t>::max() /
            policy.service_quantum_ms) {
        throw std::overflow_error(
            "generation scheduler benchmark: horizon overflows");
    }
    const auto horizon_ms = steps * policy.service_quantum_ms;
    const auto expiry_quanta = static_cast<std::uint64_t>(
        critical_contenders + 2);
    if (policy.service_quantum_ms >
            std::numeric_limits<std::uint64_t>::max() / expiry_quanta ||
        horizon_ms > std::numeric_limits<std::uint64_t>::max() -
            expiry_quanta * policy.service_quantum_ms) {
        throw std::overflow_error(
            "generation scheduler benchmark: expiry overflows");
    }

    const std::size_t candidate_count = critical_contenders + 1;
    std::vector<GenerationSchedulingCandidate> candidates;
    candidates.reserve(candidate_count);
    candidates.push_back({
        0, 0, horizon_ms + policy.service_quantum_ms,
        transport::TransportImportance::ELASTIC, true, false, {}});
    for (std::size_t index = 1; index < candidate_count; ++index) {
        candidates.push_back({
            index, 0,
            horizon_ms + (index + 1) * policy.service_quantum_ms,
            transport::TransportImportance::CRITICAL, true, false, {}});
    }

    GenerationSchedulerBenchmarkMetrics result;
    result.discipline = discipline;
    result.steps = steps;
    result.candidates = candidate_count;
    result.selections.assign(candidate_count, 0);
    result.maximum_gap_by_candidate_ms.assign(candidate_count, 0);
    auto fair_policy = policy;
    fair_policy.discipline = GenerationSchedulingDiscipline::AGING_FAIR;
    result.comparison_bound_ms = maximum_service_gap_ms(
        fair_policy, candidate_count);
    policy.discipline = discipline;

    for (std::uint64_t step = 0; step < steps; ++step) {
        const auto now_ms = step * policy.service_quantum_ms;
        for (std::size_t index = 0; index < candidate_count; ++index) {
            const auto last_service = candidates[index].last_served_at_ms
                .value_or(candidates[index].arrives_at_ms);
            result.maximum_gap_by_candidate_ms[index] = std::max(
                result.maximum_gap_by_candidate_ms[index],
                now_ms - last_service);
        }
        const auto selected = select_scheduled_generation(
            candidates, now_ms, policy);
        if (!selected) {
            throw std::logic_error(
                "generation scheduler benchmark: no candidate selected");
        }
        ++result.selections[*selected];
        candidates[*selected].last_served_at_ms = now_ms;
    }

    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto last_service = candidates[index].last_served_at_ms
            .value_or(candidates[index].arrives_at_ms);
        result.maximum_gap_by_candidate_ms[index] = std::max(
            result.maximum_gap_by_candidate_ms[index],
            horizon_ms - last_service);
        result.maximum_observed_gap_ms = std::max(
            result.maximum_observed_gap_ms,
            result.maximum_gap_by_candidate_ms[index]);
        if (result.maximum_gap_by_candidate_ms[index] >
            result.comparison_bound_ms) {
            ++result.bound_violations;
        }
    }
    return result;
}

inline GenerationSchedulerBenchmarkComparison
compare_adversarial_scheduler_policies(
    const GenerationSchedulingPolicy& policy = {},
    std::uint64_t steps = 20,
    std::size_t critical_contenders = 2) {
    return {
        run_adversarial_scheduler_benchmark(
            policy, GenerationSchedulingDiscipline::STRICT_PRIORITY_EDF,
            steps, critical_contenders),
        run_adversarial_scheduler_benchmark(
            policy, GenerationSchedulingDiscipline::AGING_FAIR,
            steps, critical_contenders)};
}

inline bool generation_scheduler_benchmark_gate_passes(
    const GenerationSchedulerBenchmarkMetrics& metrics) {
    if (metrics.selections.empty()) return false;
    if (metrics.discipline ==
        GenerationSchedulingDiscipline::STRICT_PRIORITY_EDF) {
        return metrics.bound_violations > 0 &&
            metrics.maximum_observed_gap_ms > metrics.comparison_bound_ms &&
            metrics.selections.front() == 0;
    }
    return metrics.bound_violations == 0 &&
        metrics.maximum_observed_gap_ms <= metrics.comparison_bound_ms &&
        std::all_of(
            metrics.selections.begin(), metrics.selections.end(),
            [](std::uint64_t selections) { return selections > 0; });
}

inline const std::array<GenerationSchedulerBenchmarkScenario, 7>&
canonical_generation_scheduler_benchmark_scenarios() {
    static constexpr std::array<GenerationSchedulerBenchmarkScenario, 7>
        scenarios{{
            {"baseline", 20, 2, 2'000, 3'000},
            {"fast-aging", 24, 2, 1'000, 3'000},
            {"slow-aging", 30, 2, 4'000, 3'000},
            {"tight-bound", 24, 3, 2'000, 2'000},
            {"rounded-bound", 24, 3, 2'000, 2'501},
            {"wide-bound", 40, 4, 4'000, 6'000},
            {"dense", 80, 8, 2'000, 6'000},
        }};
    return scenarios;
}

inline void append_generation_scheduler_benchmark_sweep_row(
    std::ostringstream& output,
    const GenerationSchedulerBenchmarkScenario& scenario,
    std::string_view mode,
    const GenerationSchedulerBenchmarkMetrics& metrics,
    bool gate_passes) {
    output << scenario.name << ',' << mode << ',' << scenario.steps << ','
           << scenario.critical_contenders << ',' << metrics.candidates << ','
           << scenario.aging_interval_ms << ','
           << scenario.starvation_limit_ms << ','
           << metrics.comparison_bound_ms << ','
           << metrics.maximum_observed_gap_ms << ','
           << metrics.bound_violations << ',' << metrics.selections.front()
           << ',' << (gate_passes ? "PASS" : "FAIL") << '\n';
}

inline GenerationSchedulerBenchmarkSweep
run_canonical_generation_scheduler_benchmark_sweep() {
    GenerationSchedulerBenchmarkSweep sweep;
    const auto& scenarios =
        canonical_generation_scheduler_benchmark_scenarios();
    sweep.scenarios = scenarios.size();

    std::ostringstream output;
    output << "AURORA_GENERATION_SCHEDULER_SWEEP_V1\n"
           << "scenario,mode,steps,critical_contenders,candidates,aging_ms,"
              "starvation_ms,bound_ms,max_gap_ms,bound_violations,"
              "elastic_selections,gate\n";
    for (const auto& scenario : scenarios) {
        GenerationSchedulingPolicy policy;
        policy.aging_interval_ms = scenario.aging_interval_ms;
        policy.starvation_limit_ms = scenario.starvation_limit_ms;
        const auto comparison = compare_adversarial_scheduler_policies(
            policy, scenario.steps, scenario.critical_contenders);
        const bool strict_passes =
            generation_scheduler_benchmark_gate_passes(comparison.strict);
        const bool fair_passes =
            generation_scheduler_benchmark_gate_passes(comparison.fair);
        sweep.failed_gates += static_cast<std::size_t>(!strict_passes);
        sweep.failed_gates += static_cast<std::size_t>(!fair_passes);
        append_generation_scheduler_benchmark_sweep_row(
            output, scenario, "strict", comparison.strict, strict_passes);
        append_generation_scheduler_benchmark_sweep_row(
            output, scenario, "fair", comparison.fair, fair_passes);
    }
    sweep.report = output.str();
    return sweep;
}

} // namespace aurora::simulation

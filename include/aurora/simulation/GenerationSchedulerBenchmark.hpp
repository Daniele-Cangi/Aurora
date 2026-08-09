#pragma once

#include "GenerationScheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
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

} // namespace aurora::simulation

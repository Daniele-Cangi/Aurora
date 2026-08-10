#include "../include/aurora/simulation/GenerationSchedulerBenchmark.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::uint64_t positive_number(const char* text, const char* field) {
    const std::string input = text;
    if (input.empty() || input.front() == '-') {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (...) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    if (consumed != input.size() || value == 0) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    return value;
}

void print_metrics(
    const char* mode,
    const aurora::simulation::GenerationSchedulerBenchmarkMetrics& metrics) {
    std::cout << mode << ',' << metrics.candidates << ',' << metrics.steps
              << ',' << metrics.comparison_bound_ms << ','
              << metrics.maximum_observed_gap_ms << ','
              << metrics.bound_violations << ',' << metrics.selections.front()
              << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string_view(argv[1]) == "--sweep") {
        if (argc != 2) {
            std::cerr << "usage: aurora_scheduler_benchmark --sweep\n";
            return 2;
        }
        const auto sweep = aurora::simulation::
            run_canonical_generation_scheduler_benchmark_sweep();
        std::cout << sweep.report;
        return sweep.failed_gates == 0 ? 0 : 1;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--verify-sweep") {
        if (argc != 3) {
            std::cerr << "usage: aurora_scheduler_benchmark "
                         "--verify-sweep <canonical-report>\n";
            return 2;
        }
        std::ifstream input(argv[2], std::ios::binary);
        if (!input) {
            std::cerr << "scheduler benchmark: cannot open canonical report\n";
            return 2;
        }
        const std::string expected{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const auto sweep = aurora::simulation::
            run_canonical_generation_scheduler_benchmark_sweep();
        if (sweep.failed_gates != 0) {
            std::cerr << "scheduler benchmark: " << sweep.failed_gates
                      << " regression gate(s) failed\n";
            return 1;
        }
        if (expected != sweep.report) {
            const auto mismatch = std::mismatch(
                expected.begin(), expected.end(), sweep.report.begin(),
                sweep.report.end());
            std::cerr << "scheduler benchmark: canonical report mismatch at "
                      << "byte "
                      << std::distance(expected.begin(), mismatch.first)
                      << '\n';
            return 1;
        }
        std::cout << "scheduler benchmark sweep verified: "
                  << sweep.scenarios << " scenarios\n";
        return 0;
    }
    if (argc > 5) {
        std::cerr << "usage: aurora_scheduler_benchmark "
                     "[steps] [critical-contenders] [aging-ms] "
                     "[starvation-ms]\n";
        return 2;
    }
    try {
        std::uint64_t steps = 20;
        std::size_t critical_contenders = 2;
        aurora::simulation::GenerationSchedulingPolicy policy;
        if (argc > 1) steps = positive_number(argv[1], "step count");
        if (argc > 2) {
            const auto value = positive_number(argv[2], "contender count");
            if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
                throw std::invalid_argument("invalid contender count");
            }
            critical_contenders = static_cast<std::size_t>(value);
        }
        if (argc > 3) {
            policy.aging_interval_ms = positive_number(
                argv[3], "aging interval");
        }
        if (argc > 4) {
            policy.starvation_limit_ms = positive_number(
                argv[4], "starvation limit");
        }
        const auto comparison =
            aurora::simulation::compare_adversarial_scheduler_policies(
                policy, steps, critical_contenders);
        std::cout << "mode,candidates,steps,bound_ms,max_gap_ms,"
                     "bound_violations,elastic_selections\n";
        print_metrics("strict", comparison.strict);
        print_metrics("fair", comparison.fair);
        return comparison.strict.bound_violations > 0 &&
                comparison.fair.bound_violations == 0
            ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "scheduler benchmark: " << error.what() << '\n';
        return 2;
    }
}

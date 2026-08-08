#include "../include/aurora/simulation/BaselineBenchmark.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace aurora::simulation;

    BenchmarkScenario scenario;
    scenario.payload_size = 768;
    scenario.symbol_size = 64;
    scenario.critical_bytes = 192;
    scenario.packet_loss_rate = 0.30;
    scenario.trials = 40;
    scenario.seed = 0xBEEFULL;

    const BaselineBenchmark benchmark;
    const auto first = benchmark.run(scenario);
    const auto replay = benchmark.run(scenario);
    assert(first == replay);
    assert(first.size() == 5);
    assert(first[0].baseline == BaselineKind::NO_FEC);
    assert(first[1].baseline == BaselineKind::REPETITION_2X);
    assert(first[4].baseline == BaselineKind::ADAPTIVE_AURORA);
    assert(first[1].transmitted_bytes == first[0].transmitted_bytes * 2);
    for (const auto& result : first) {
        assert(result.trials == scenario.trials);
        assert(result.delivery_rate >= 0.0 && result.delivery_rate <= 1.0);
        assert(result.critical_delivery_rate >= 0.0 && result.critical_delivery_rate <= 1.0);
        assert(result.goodput >= 0.0 && result.goodput <= 1.0);
        assert(result.received_bytes <= result.transmitted_bytes);
    }

    auto perfect = scenario;
    perfect.packet_loss_rate = 0.0;
    perfect.trials = 3;
    const auto perfect_results = benchmark.run(perfect);
    for (const auto& result : perfect_results) {
        assert(result.delivery_rate == 1.0);
        assert(result.critical_delivery_rate == 1.0);
    }

    std::cout << "baseline benchmark tests passed\n";
    return 0;
}

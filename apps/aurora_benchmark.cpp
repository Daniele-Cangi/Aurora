#include "aurora/simulation/BaselineBenchmark.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    aurora::simulation::BenchmarkScenario scenario;
    try {
        if (argc > 1) scenario.packet_loss_rate = std::stod(argv[1]);
        if (argc > 2) scenario.trials = static_cast<std::size_t>(std::stoull(argv[2]));
        if (argc > 3) scenario.seed = std::stoull(argv[3], nullptr, 0);
        if (argc > 4) {
            std::cerr << "usage: aurora_benchmark [packet-loss-rate] [trials] [seed]\n";
            return 2;
        }

        const auto results = aurora::simulation::BaselineBenchmark{}.run(scenario);
        std::cout << "baseline,seed,packet_loss_rate,trials,payload_size,symbol_size,"
                     "critical_bytes,deadline_ms,delivery_rate,critical_delivery_rate,"
                     "goodput,transmitted_bytes,received_bytes\n";
        std::cout << std::fixed << std::setprecision(6);
        for (const auto& result : results) {
            std::cout << aurora::simulation::baseline_name(result.baseline) << ','
                      << scenario.seed << ','
                      << scenario.packet_loss_rate << ','
                      << result.trials << ','
                      << scenario.payload_size << ','
                      << scenario.symbol_size << ','
                      << scenario.critical_bytes << ','
                      << scenario.deadline_ms << ','
                      << result.delivery_rate << ','
                      << result.critical_delivery_rate << ','
                      << result.goodput << ','
                      << result.transmitted_bytes << ','
                      << result.received_bytes << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 2;
    }
}

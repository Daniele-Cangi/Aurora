#include "../include/aurora/simulation/BaselineBenchmark.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace aurora::simulation;

    BenchmarkScenario scenario;
    scenario.payload_size = 768;
    scenario.symbol_size = 64;
    scenario.critical_bytes = 192;
    scenario.channel.iid_loss_rate = 0.30;
    scenario.trials = 40;
    scenario.seed = 0xBEEFULL;

    const BaselineBenchmark benchmark;
    const auto first_report = benchmark.run_report(scenario);
    const auto replay_report = benchmark.replay(
        scenario, first_report.channel_traces);
    assert(first_report == replay_report);
    const auto& first = first_report.summaries;
    assert(first.size() == 5);
    assert(first_report.trial_results.size() == scenario.trials * first.size());
    assert(!first_report.channel_trace_fingerprint.empty());
    assert(!first_report.build_provenance.commit.empty());
    assert(first_report.build_provenance.schema == "AURORA_BUILD_PROVENANCE_V1");
    assert(!first_report.build_provenance.source_state.empty());
    assert(!first_report.build_provenance.compiler_id.empty());
    assert(!first_report.build_provenance.compiler_version.empty());
    assert(!first_report.build_provenance.target_system.empty());
    assert(!first_report.build_provenance.build_type.empty());
    assert(first_report.build_provenance.evidence_level == "simulation");
    assert(!first_report.build_provenance.hardware_validated);
    assert(first_report.build_provenance.fingerprint().size() == 16);
    assert(first_report.build_provenance.fingerprint() ==
           replay_report.build_provenance.fingerprint());

    auto mismatched = scenario;
    mismatched.channel.kind = ChannelScenarioKind::SLOW_DRIFT;
    bool mismatch_rejected = false;
    try {
        (void)benchmark.replay(mismatched, first_report.channel_traces);
    } catch (const std::invalid_argument&) {
        mismatch_rejected = true;
    }
    assert(mismatch_rejected);
    assert(first[0].baseline == BaselineKind::NO_FEC);
    assert(first[1].baseline == BaselineKind::REPETITION_2X);
    assert(first[4].baseline == BaselineKind::ADAPTIVE_AURORA);
    assert(first[1].transmitted_bytes == first[0].transmitted_bytes * 2);
    assert(!first[0].innovative_symbol_ratio.has_value());
    assert(!first[1].innovative_symbol_ratio.has_value());
    assert(!first[0].overhead_direction_changes.has_value());
    assert(!first[2].overhead_direction_changes.has_value());
    assert(first[4].overhead_direction_changes.has_value());
    for (const auto& result : first) {
        assert(result.trials == scenario.trials);
        assert(result.delivery_rate >= 0.0 && result.delivery_rate <= 1.0);
        assert(result.delivery_ci95_low <= result.delivery_rate);
        assert(result.delivery_ci95_high >= result.delivery_rate);
        assert(result.critical_delivery_rate >= 0.0 && result.critical_delivery_rate <= 1.0);
        assert(result.critical_delivery_ci95_low <= result.critical_delivery_rate);
        assert(result.critical_delivery_ci95_high >= result.critical_delivery_rate);
        assert(result.goodput >= 0.0 && result.goodput <= 1.0);
        assert(result.received_bytes <= result.transmitted_bytes);
    }

    auto perfect = scenario;
    perfect.channel.iid_loss_rate = 0.0;
    perfect.trials = 3;
    const auto perfect_results = benchmark.run(perfect);
    for (const auto& result : perfect_results) {
        assert(result.delivery_rate == 1.0);
        assert(result.critical_delivery_rate == 1.0);
        assert(result.transmitted_bytes_per_delivered_byte.has_value());
    }

    auto burst = scenario;
    burst.channel.kind = ChannelScenarioKind::GILBERT_ELLIOTT;
    const auto burst_first = benchmark.run_report(burst);
    const auto burst_replay = benchmark.run_report(burst);
    assert(burst_first == burst_replay);

    std::cout << "baseline benchmark tests passed\n";
    return 0;
}

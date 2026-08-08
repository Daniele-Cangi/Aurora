#include "aurora/simulation/BaselineBenchmark.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

void usage() {
    std::cerr
        << "usage: aurora_benchmark [loss trials seed]\n"
        << "       aurora_benchmark [options]\n"
        << "  --scenario iid|burst|outage|drift|shock\n"
        << "  --loss P                     IID loss rate\n"
        << "  --trials N --seed N\n"
        << "  --good-loss P --bad-loss P --good-to-bad P --bad-to-good P\n"
        << "  --base-loss P --outage-start F --outage-duration F\n"
        << "  --drift-start-loss P --drift-end-loss P\n"
        << "  --shock-peak-loss P --shock-start F --shock-duration F\n"
        << "  --recovery-duration F\n"
        << "  --trace-in FILE --trace-out FILE --runs FILE\n";
}

std::string require_value(int argc, char* argv[], int& index, const std::string& option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(option + " requires a value");
    }
    return argv[++index];
}

double parse_double(const std::string& value, const std::string& option) {
    std::size_t consumed = 0;
    const auto parsed = std::stod(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument("invalid value for " + option);
    }
    return parsed;
}

std::uint64_t parse_u64(const std::string& value, const std::string& option) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed, 0);
    if (consumed != value.size()) {
        throw std::invalid_argument("invalid value for " + option);
    }
    return parsed;
}

std::size_t parse_size(const std::string& value, const std::string& option) {
    const auto parsed = parse_u64(value, option);
    if (parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("value is out of range for " + option);
    }
    return static_cast<std::size_t>(parsed);
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace aurora::simulation;

    BenchmarkScenario scenario;
    std::string trace_input;
    std::string trace_output;
    std::string trial_output;
    bool trials_explicit = false;
    bool seed_explicit = false;
    bool channel_explicit = false;
    bool iid_parameter_explicit = false;
    bool burst_parameter_explicit = false;
    bool outage_parameter_explicit = false;
    bool drift_parameter_explicit = false;
    bool shock_parameter_explicit = false;
    bool base_loss_explicit = false;

    try {
        if (argc > 1 && std::string(argv[1]).starts_with("-") == false) {
            if (argc > 4) {
                usage();
                return 2;
            }
            scenario.channel.iid_loss_rate = parse_double(argv[1], "loss");
            channel_explicit = true;
            iid_parameter_explicit = true;
            if (argc > 2) {
                scenario.trials = parse_size(argv[2], "trials");
                trials_explicit = true;
            }
            if (argc > 3) {
                scenario.seed = parse_u64(argv[3], "seed");
                seed_explicit = true;
            }
        } else {
            for (int i = 1; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--help" || option == "-h") {
                    usage();
                    return 0;
                }
                if (option == "--scenario") {
                    scenario.channel.kind = parse_channel_scenario_kind(
                        require_value(argc, argv, i, option));
                    channel_explicit = true;
                } else if (option == "--loss") {
                    scenario.channel.iid_loss_rate = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    iid_parameter_explicit = true;
                } else if (option == "--trials") {
                    scenario.trials = parse_size(require_value(argc, argv, i, option), option);
                    trials_explicit = true;
                } else if (option == "--seed") {
                    scenario.seed = parse_u64(require_value(argc, argv, i, option), option);
                    seed_explicit = true;
                } else if (option == "--good-loss") {
                    scenario.channel.good_state_loss_rate = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    burst_parameter_explicit = true;
                } else if (option == "--bad-loss") {
                    scenario.channel.bad_state_loss_rate = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    burst_parameter_explicit = true;
                } else if (option == "--good-to-bad") {
                    scenario.channel.good_to_bad_probability = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    burst_parameter_explicit = true;
                } else if (option == "--bad-to-good") {
                    scenario.channel.bad_to_good_probability = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    burst_parameter_explicit = true;
                } else if (option == "--base-loss") {
                    const auto value = parse_double(require_value(argc, argv, i, option), option);
                    scenario.channel.outage_base_loss_rate = value;
                    scenario.channel.shock_base_loss_rate = value;
                    channel_explicit = true;
                    base_loss_explicit = true;
                } else if (option == "--outage-start") {
                    scenario.channel.outage_start_fraction = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    outage_parameter_explicit = true;
                } else if (option == "--outage-duration") {
                    scenario.channel.outage_duration_fraction = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    outage_parameter_explicit = true;
                } else if (option == "--drift-start-loss") {
                    scenario.channel.drift_start_loss_rate = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    drift_parameter_explicit = true;
                } else if (option == "--drift-end-loss") {
                    scenario.channel.drift_end_loss_rate = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    drift_parameter_explicit = true;
                } else if (option == "--shock-peak-loss") {
                    scenario.channel.shock_peak_loss_rate = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    shock_parameter_explicit = true;
                } else if (option == "--shock-start") {
                    scenario.channel.shock_start_fraction = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    shock_parameter_explicit = true;
                } else if (option == "--shock-duration") {
                    scenario.channel.shock_duration_fraction = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    shock_parameter_explicit = true;
                } else if (option == "--recovery-duration") {
                    scenario.channel.recovery_duration_fraction = parse_double(
                        require_value(argc, argv, i, option), option);
                    channel_explicit = true;
                    shock_parameter_explicit = true;
                } else if (option == "--trace-in") {
                    trace_input = require_value(argc, argv, i, option);
                } else if (option == "--trace-out") {
                    trace_output = require_value(argc, argv, i, option);
                } else if (option == "--runs") {
                    trial_output = require_value(argc, argv, i, option);
                } else {
                    throw std::invalid_argument("unknown option: " + option);
                }
            }
        }

        const auto kind = scenario.channel.kind;
        if (iid_parameter_explicit && kind != ChannelScenarioKind::IID) {
            throw std::invalid_argument("--loss is valid only for the IID scenario");
        }
        if (burst_parameter_explicit && kind != ChannelScenarioKind::GILBERT_ELLIOTT) {
            throw std::invalid_argument("burst parameters require --scenario burst");
        }
        if (outage_parameter_explicit && kind != ChannelScenarioKind::SCHEDULED_OUTAGE) {
            throw std::invalid_argument("outage parameters require --scenario outage");
        }
        if (drift_parameter_explicit && kind != ChannelScenarioKind::SLOW_DRIFT) {
            throw std::invalid_argument("drift parameters require --scenario drift");
        }
        if (shock_parameter_explicit && kind != ChannelScenarioKind::SHOCK_RECOVERY) {
            throw std::invalid_argument("shock parameters require --scenario shock");
        }
        if (base_loss_explicit && kind != ChannelScenarioKind::SCHEDULED_OUTAGE &&
            kind != ChannelScenarioKind::SHOCK_RECOVERY) {
            throw std::invalid_argument(
                "--base-loss is valid only for outage or shock scenarios");
        }

        BaselineBenchmark benchmark;
        BenchmarkReport report;
        if (!trace_input.empty()) {
            const auto corpus = ChannelTraceCorpus::load(trace_input);
            const auto recorded_scenario = channel_scenario_from_id(corpus.scenario_id);
            if (channel_explicit && channel_scenario_id(scenario.channel) != corpus.scenario_id) {
                throw std::invalid_argument(
                    "configured channel does not match the imported trace");
            }
            if (seed_explicit && scenario.seed != corpus.experiment_seed) {
                throw std::invalid_argument(
                    "configured seed does not match the imported trace");
            }
            if (trials_explicit && scenario.trials != corpus.traces.size()) {
                throw std::invalid_argument(
                    "configured trial count does not match the imported trace");
            }
            scenario.channel = recorded_scenario;
            scenario.seed = corpus.experiment_seed;
            scenario.trials = corpus.traces.size();
            report = benchmark.replay(scenario, corpus);
        } else {
            report = benchmark.run_report(scenario);
        }

        if (!trace_output.empty()) report.channel_traces.save(trace_output);
        if (!trial_output.empty()) save_benchmark_trial_csv(report, trial_output);

        std::cout
            << "baseline,scenario,scenario_id,trace_fingerprint,seed,trials,payload_size,symbol_size,"
               "critical_bytes,deadline_ms,delivery_rate,delivery_ci95_low,"
               "delivery_ci95_high,critical_delivery_rate,critical_ci95_low,"
               "critical_ci95_high,goodput,bytes_per_delivered_byte,"
               "innovative_symbol_ratio,mean_effective_overhead,"
               "overhead_direction_changes,transmitted_bytes,received_bytes\n";
        std::cout << std::fixed << std::setprecision(6);
        for (const auto& result : report.summaries) {
            std::cout << baseline_name(result.baseline) << ','
                      << channel_scenario_name(report.scenario.channel.kind) << ','
                      << report.channel_traces.scenario_id << ','
                      << report.channel_trace_fingerprint << ','
                      << report.scenario.seed << ','
                      << result.trials << ','
                      << report.scenario.payload_size << ','
                      << report.scenario.symbol_size << ','
                      << report.scenario.critical_bytes << ','
                      << report.scenario.deadline_ms << ','
                      << result.delivery_rate << ','
                      << result.delivery_ci95_low << ','
                      << result.delivery_ci95_high << ','
                      << result.critical_delivery_rate << ','
                      << result.critical_delivery_ci95_low << ','
                      << result.critical_delivery_ci95_high << ','
                      << result.goodput << ',';
            if (result.transmitted_bytes_per_delivered_byte) {
                std::cout << *result.transmitted_bytes_per_delivered_byte;
            } else {
                std::cout << "N/A";
            }
            std::cout << ',';
            if (result.innovative_symbol_ratio) {
                std::cout << *result.innovative_symbol_ratio;
            } else {
                std::cout << "N/A";
            }
            std::cout << ',' << result.mean_effective_overhead << ',';
            if (result.overhead_direction_changes) {
                std::cout << *result.overhead_direction_changes;
            } else {
                std::cout << "N/A";
            }
            std::cout << ','
                      << result.transmitted_bytes << ','
                      << result.received_bytes << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        usage();
        return 2;
    }
}

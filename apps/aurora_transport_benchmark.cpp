#define AURORA_NO_MAIN
#include "../aurora_x.cpp"
#ifdef AURORA_USE_WIREHAIR
#include "aurora/fec/WirehairCodec.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view report_schema =
    "AURORA_END_TO_END_TRANSPORT_BENCHMARK_V1";
constexpr std::string_view evidence_level =
    "simulation-common-inputs-action-dependent-rng";
constexpr std::size_t symbol_size = 56;
constexpr std::size_t wire_overhead_bytes = 8;
constexpr std::array<std::uint64_t, 4> trial_seeds{
    0xA607A001ULL, 0xA607A002ULL, 0xA607A003ULL, 0xA607A004ULL};

class NullBuffer final : public std::streambuf {
public:
    int overflow(int character) override { return character; }
};

class ScopedQuietCout {
public:
    ScopedQuietCout() : previous_(std::cout.rdbuf(&sink_)) {}
    ~ScopedQuietCout() { std::cout.rdbuf(previous_); }

    ScopedQuietCout(const ScopedQuietCout&) = delete;
    ScopedQuietCout& operator=(const ScopedQuietCout&) = delete;

private:
    NullBuffer sink_;
    std::streambuf* previous_;
};

struct Scenario {
    std::string name;
    std::string intention;
    aurora::simulation::ContactSchedule contacts;
    aurora::simulation::GenerationArrivalSchedule arrivals;
    aurora::simulation::GenerationSchedulingPolicy scheduler;
};

struct Controller {
    std::string name;
    enum class Kind { FIXED_MINIMUM, FIXED_CLASS_AWARE, BIOLOGICAL } kind;
    enum class Codec { EXPERIMENTAL_LT, WIREHAIR } codec =
        Codec::EXPERIMENTAL_LT;
};

struct Metrics {
    std::string scenario;
    std::string controller;
    std::string world_fingerprint;
    std::size_t trials = 0;
    std::uint64_t generations = 0;
    std::uint64_t delivered = 0;
    std::uint64_t deadline_misses = 0;
    std::uint64_t critical_generations = 0;
    std::uint64_t critical_delivered = 0;
    std::uint64_t source_symbols = 0;
    std::uint64_t initially_emitted_symbols = 0;
    std::uint64_t scheduled_turns = 0;
    std::uint64_t effective_service_turns = 0;
    std::uint64_t turns_without_effective_service = 0;
    std::uint64_t transmission_attempts = 0;
    std::uint64_t hal_accepted_attempts = 0;
    std::uint64_t delivered_attempts = 0;
    std::uint64_t accepted_wire_bytes = 0;
    std::uint64_t useful_payload_bytes = 0;
    double energy_j = 0.0;
    std::uint64_t terminal_latency_sum_ms = 0;
    std::uint64_t maximum_terminal_latency_ms = 0;
    std::uint64_t decision_records = 0;
    std::uint64_t event_records = 0;
    std::uint64_t trace_hash = 14695981039346656037ULL;
    bool replay_verified = true;
};

std::uint64_t fnv_append(std::uint64_t hash, std::string_view value) {
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string seed_set() {
    std::ostringstream output;
    for (std::size_t index = 0; index < trial_seeds.size(); ++index) {
        if (index > 0) output << '+';
        output << trial_seeds[index];
    }
    return output.str();
}

std::string world_fingerprint(const Scenario& scenario) {
    std::ostringstream scheduler;
    scheduler << scenario.scheduler.service_quantum_ms << '|'
              << scenario.scheduler.aging_interval_ms << '|'
              << scenario.scheduler.starvation_limit_ms << '|'
              << static_cast<unsigned>(scenario.scheduler.discipline);
    std::uint64_t hash = 14695981039346656037ULL;
    hash = fnv_append(hash, scenario.name);
    hash = fnv_append(hash, scenario.intention);
    hash = fnv_append(hash, scenario.contacts.serialize());
    hash = fnv_append(hash, scenario.arrivals.serialize());
    hash = fnv_append(hash, scheduler.str());
    hash = fnv_append(hash, seed_set());
    return hex_u64(hash);
}

std::unique_ptr<aurora::AuroraOrganism> organism_for(
    const Controller& controller) {
    std::shared_ptr<aurora::control::TransportPolicy> policy;
    switch (controller.kind) {
        case Controller::Kind::FIXED_MINIMUM:
            policy = std::make_shared<aurora::control::FixedTransportPolicy>(
                "fixed-minimum", 1.0, 1.0, 1.0);
            break;
        case Controller::Kind::FIXED_CLASS_AWARE:
            policy = std::make_shared<aurora::control::FixedTransportPolicy>(
                "fixed-class-aware", 2.5, 1.5, 1.1);
            break;
        case Controller::Kind::BIOLOGICAL:
            policy = std::make_shared<aurora::control::BiologicalAdaptivePolicy>();
            break;
    }
    std::shared_ptr<aurora::fec::GenerationCodec> codec;
    switch (controller.codec) {
        case Controller::Codec::EXPERIMENTAL_LT:
            codec = std::make_shared<aurora::fec::ExperimentalLtLikeCodec>();
            break;
        case Controller::Codec::WIREHAIR:
#ifdef AURORA_USE_WIREHAIR
            codec = std::make_shared<aurora::fec::WirehairCodec>();
            break;
#else
            throw std::logic_error(
                "Wirehair controller requested without USE_WIREHAIR");
#endif
    }
    return std::make_unique<aurora::AlienFountainOrganism>(
        std::move(codec), std::move(policy));
}

std::vector<Scenario> canonical_scenarios() {
    using aurora::simulation::ContactAvailability;
    using aurora::simulation::ContactWindow;
    using aurora::simulation::GenerationArrival;
    using aurora::simulation::GenerationServiceClass;

    aurora::simulation::GenerationSchedulingPolicy scheduler;
    scheduler.service_quantum_ms = 1'000;
    scheduler.aging_interval_ms = 2'000;
    scheduler.starvation_limit_ms = 4'000;
    scheduler.discipline =
        aurora::simulation::GenerationSchedulingDiscipline::AGING_FAIR;

    const std::string intention =
        "deadline:30s;reliability:0.99;duty:0.02;rf:on;optical:on;"
        "backscatter:on;ris:16;selector:argmax;reserve:0.05;"
        "max_repair_amplification:4;min_critical_overhead:1.5";

    Scenario intermittent{
        "causal-feedback-waves",
        intention,
        aurora::simulation::ContactSchedule({
            ContactWindow{0, 5'000, ContactAvailability{}},
            ContactWindow{5'000, 12'000, ContactAvailability{true, true, true}},
            ContactWindow{12'000, 15'000, ContactAvailability{}},
            ContactWindow{15'000, std::numeric_limits<std::uint64_t>::max(),
                          ContactAvailability{true, true, true}}}),
        aurora::simulation::GenerationArrivalSchedule({
            GenerationArrival{0, "urgent-a", GenerationServiceClass::CRITICAL, 5'000},
            GenerationArrival{7'000, "urgent-b", GenerationServiceClass::CRITICAL, 12'000},
            GenerationArrival{8'000, "service-c", GenerationServiceClass::IMPORTANT, 14'000},
            GenerationArrival{9'000, "service-d", GenerationServiceClass::IMPORTANT, 16'000}}),
        scheduler};

    Scenario constrained{
        "deadline-contention",
        intention,
        aurora::simulation::ContactSchedule({
            ContactWindow{0, 3'000, ContactAvailability{true, false, false}},
            ContactWindow{3'000, 6'000, ContactAvailability{}},
            ContactWindow{6'000, 10'000, ContactAvailability{false, true, true}},
            ContactWindow{10'000, std::numeric_limits<std::uint64_t>::max(),
                          ContactAvailability{true, true, true}}}),
        aurora::simulation::GenerationArrivalSchedule({
            GenerationArrival{0, "bulk-a", GenerationServiceClass::ELASTIC, 16'000},
            GenerationArrival{1'000, "urgent-b", GenerationServiceClass::CRITICAL, 12'000},
            GenerationArrival{2'000, "service-c", GenerationServiceClass::IMPORTANT, 14'000},
            GenerationArrival{4'000, "urgent-d", GenerationServiceClass::CRITICAL, 15'000}}),
        scheduler};

    return {std::move(intermittent), std::move(constrained)};
}

std::vector<Controller> controllers() {
    return {
        {"fixed-minimum", Controller::Kind::FIXED_MINIMUM},
        {"fixed-class-aware", Controller::Kind::FIXED_CLASS_AWARE},
        {"biological-adaptive", Controller::Kind::BIOLOGICAL}};
}

#ifdef AURORA_USE_WIREHAIR
std::vector<Controller> external_fec_controllers() {
    return {
        {"fixed-class-aware/experimental-lt-like",
         Controller::Kind::FIXED_CLASS_AWARE,
         Controller::Codec::EXPERIMENTAL_LT},
        {"fixed-class-aware/wirehair-legacy-fixups-2026-07",
         Controller::Kind::FIXED_CLASS_AWARE,
         Controller::Codec::WIREHAIR}};
}
#endif

void accumulate_trial(Metrics& metrics, const Scenario& scenario,
                      const Controller& controller, std::uint64_t seed) {
    Engine engine;
    engine.telemetry.disable();
    engine.T = symbol_size;
    engine.contact_schedule = scenario.contacts;
    engine.generation_arrival_schedule = scenario.arrivals;
    engine.generation_scheduling_policy = scenario.scheduler;
    engine.organism = organism_for(controller);

    const auto intention = scenario.intention + ";seed:" +
        std::to_string(seed);
    {
        ScopedQuietCout quiet;
        engine.init(intention);
        (void)engine.run();
    }

    const auto verification =
        engine.simulation_event_log.verify(engine.decision_trace_log);
    if (!verification.ok) {
        throw std::runtime_error(
            "paired ledger replay failed for " + scenario.name + "/" +
            controller.name + ": " + verification.failure_reason);
    }

    ++metrics.trials;
    metrics.generations += engine.scheduled_generations.size();
    for (const auto& generation : engine.scheduled_generations) {
        if (!generation.terminal || !generation.terminal_at_ms) {
            throw std::runtime_error(
                "benchmark ended before a generation became terminal");
        }
        const bool critical = generation.scheduling_importance ==
            aurora::transport::TransportImportance::CRITICAL;
        if (critical) ++metrics.critical_generations;
        metrics.source_symbols += generation.descriptor.total_source_symbols;
        for (const auto& segment : generation.descriptor.segments) {
            metrics.initially_emitted_symbols +=
                segment.coding.emitted_symbols;
        }
        if (generation.delivered) {
            ++metrics.delivered;
            metrics.useful_payload_bytes += generation.payload.size();
            if (critical) ++metrics.critical_delivered;
        } else {
            ++metrics.deadline_misses;
        }
        metrics.scheduled_turns += generation.scheduled_turns;
        const auto latency = *generation.terminal_at_ms -
            generation.arrival.arrives_at_ms;
        metrics.terminal_latency_sum_ms += latency;
        metrics.maximum_terminal_latency_ms = std::max(
            metrics.maximum_terminal_latency_ms, latency);
    }

    for (const auto& event : engine.simulation_event_log.records()) {
        if (event.effective_transport_service_attempts > 0) {
            ++metrics.effective_service_turns;
        } else {
            ++metrics.turns_without_effective_service;
        }
    }
    std::ostringstream structural_trace;
    for (const auto& event : engine.simulation_event_log.records()) {
        structural_trace << "E|" << event.step << '|'
                         << event.simulated_now_ms << '|'
                         << event.active_generation_index << '|'
                         << event.arrived_generation_index << '|'
                         << event.arrived_source_packets << '|'
                         << static_cast<unsigned>(event.contact_available.mask())
                         << '|' << event.effective_transport_service_attempts
                         << '|' << event.random_before << '|'
                         << event.random_after_ris << '|'
                         << event.random_after_action << '|'
                         << event.source_buffer_after_action << '|'
                         << event.destination_buffer_after_action << '|'
                         << event.decoder_rank_before << '|'
                         << event.decoder_rank_after << '|'
                         << static_cast<unsigned>(event.decode_status) << '\n';
    }
    for (const auto& record : engine.decision_trace_log.records()) {
        const auto& execution = record.trace.execution;
        metrics.transmission_attempts += execution.transmission_attempts;
        metrics.hal_accepted_attempts += execution.hal_accepted_attempts;
        metrics.delivered_attempts += execution.delivered_attempts;
        for (const auto& attempt : execution.attempts) {
            if (attempt.transmitted) {
                metrics.energy_j +=
                    attempt.energy_before_j - attempt.energy_after_j;
            }
        }
        structural_trace << "D|" << record.descriptor.generation_id << '|'
                         << record.descriptor.policy_id << '|'
                         << record.descriptor.total_source_symbols << '|'
                         << static_cast<unsigned>(execution.link) << '|'
                         << execution.transmission_attempts << '|'
                         << execution.hal_accepted_attempts << '|'
                         << execution.delivered_attempts << '|'
                         << execution.repair_symbols_emitted << '|'
                         << execution.critical_only << '\n';
        for (const auto& segment : record.descriptor.segments) {
            structural_trace << "S|" << segment.segment_id << '|'
                             << segment.source_symbol_count << '|'
                             << segment.coding.emitted_symbols << '|'
                             << segment.coding.seed << '|'
                             << static_cast<unsigned>(segment.importance)
                             << '|' << segment.expires_at_ms << '\n';
        }
        for (const auto& attempt : execution.attempts) {
            structural_trace << "A|" << attempt.simulated_now_ms << '|'
                             << attempt.packet_sequence << '|'
                             << attempt.symbol_seed << '|'
                             << attempt.segment_id << '|'
                             << attempt.critical << '|'
                             << static_cast<unsigned>(attempt.refusal) << '|'
                             << attempt.hal_accepted << '|'
                             << attempt.transmitted << '|'
                             << attempt.delivered << '\n';
        }
    }
    const auto accepted_this_trial = std::accumulate(
        engine.decision_trace_log.records().begin(),
        engine.decision_trace_log.records().end(), std::uint64_t{0},
        [](std::uint64_t total, const auto& record) {
            return total + record.trace.execution.hal_accepted_attempts;
        });
    metrics.accepted_wire_bytes += accepted_this_trial *
        (symbol_size + wire_overhead_bytes);
    metrics.decision_records += engine.decision_trace_log.records().size();
    metrics.event_records += engine.simulation_event_log.records().size();
    metrics.trace_hash = fnv_append(metrics.trace_hash, structural_trace.str());
    metrics.replay_verified = metrics.replay_verified && verification.ok;
}

std::string report_row(const Metrics& metrics) {
    const double mean_latency = metrics.generations == 0
        ? 0.0
        : static_cast<double>(metrics.terminal_latency_sum_ms) /
            static_cast<double>(metrics.generations);
    std::ostringstream output;
    output << report_schema << ',' << metrics.scenario << ','
           << metrics.controller << ',' << evidence_level << ','
           << metrics.world_fingerprint << ',' << seed_set() << ','
           << metrics.trials << ',' << metrics.generations << ','
           << metrics.delivered << ',' << metrics.deadline_misses << ','
           << metrics.critical_generations << ','
           << metrics.critical_delivered << ',' << metrics.source_symbols
           << ',' << metrics.initially_emitted_symbols << ','
           << metrics.scheduled_turns
           << ',' << metrics.effective_service_turns << ','
           << metrics.turns_without_effective_service << ','
           << metrics.transmission_attempts << ','
           << metrics.hal_accepted_attempts << ','
           << metrics.delivered_attempts << ','
           << metrics.accepted_wire_bytes << ','
           << metrics.useful_payload_bytes << ',' << std::fixed
           << std::setprecision(9) << metrics.energy_j << ','
           << std::setprecision(3) << mean_latency << ','
           << metrics.maximum_terminal_latency_ms << ','
           << metrics.decision_records << ',' << metrics.event_records << ','
           << hex_u64(metrics.trace_hash) << ','
           << (metrics.replay_verified ? "true" : "false") << '\n';
    return output.str();
}

struct SweepResult {
    std::string report;
    std::size_t scenarios = 0;
    std::size_t controllers = 0;
};

SweepResult run_sweep(const std::vector<Controller>& compared_controllers) {
    const auto scenarios = canonical_scenarios();
    std::ostringstream report;
    report << "schema,scenario,controller,evidence_level,world_fingerprint,"
              "seeds,trials,generations,delivered,deadline_misses,"
              "critical_generations,critical_delivered,source_symbols,"
              "initially_emitted_symbols,scheduled_turns,"
              "effective_service_turns,turns_without_effective_service,"
              "transmission_attempts,hal_accepted_attempts,delivered_attempts,"
              "accepted_wire_bytes,useful_payload_bytes,energy_j,"
              "mean_terminal_latency_ms,max_terminal_latency_ms,"
              "decision_records,event_records,structural_trace_fingerprint,"
              "paired_ledger_replay_verified\n";

    for (const auto& scenario : scenarios) {
        const auto fingerprint = world_fingerprint(scenario);
        std::uint64_t expected_generations = 0;
        for (const auto& controller : compared_controllers) {
            Metrics metrics;
            metrics.scenario = scenario.name;
            metrics.controller = controller.name;
            metrics.world_fingerprint = fingerprint;
            for (const auto seed : trial_seeds) {
                accumulate_trial(metrics, scenario, controller, seed);
            }
            if (metrics.scheduled_turns != metrics.effective_service_turns +
                    metrics.turns_without_effective_service ||
                metrics.transmission_attempts < metrics.hal_accepted_attempts ||
                metrics.hal_accepted_attempts < metrics.delivered_attempts ||
                metrics.generations != metrics.delivered +
                    metrics.deadline_misses ||
                !metrics.replay_verified) {
                throw std::runtime_error(
                    "end-to-end benchmark semantic gate failed for " +
                    scenario.name + "/" + controller.name);
            }
            if (expected_generations == 0) {
                expected_generations = metrics.generations;
            } else if (metrics.generations != expected_generations) {
                throw std::runtime_error(
                    "controllers did not receive the same generation count");
            }
            report << report_row(metrics);
        }
    }
    return {report.str(), scenarios.size(), compared_controllers.size()};
}

SweepResult run_sweep() { return run_sweep(controllers()); }

int verify_report(const char* path,
                  const std::vector<Controller>& compared_controllers) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "transport benchmark: cannot open canonical report\n";
        return 2;
    }
    const std::string expected{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto sweep = run_sweep(compared_controllers);
    if (expected != sweep.report) {
        const auto mismatch = std::mismatch(
            expected.begin(), expected.end(), sweep.report.begin(),
            sweep.report.end());
        std::cerr << "transport benchmark: canonical report mismatch at byte "
                  << std::distance(expected.begin(), mismatch.first) << '\n';
        return 1;
    }
    std::cout << "end-to-end transport benchmark verified: "
              << sweep.scenarios << " scenarios, " << sweep.controllers
              << " controllers, " << trial_seeds.size() << " seeds\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--sweep") {
            std::cout << run_sweep().report;
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--verify-sweep") {
            return verify_report(argv[2], controllers());
        }
#ifdef AURORA_USE_WIREHAIR
        if (argc == 2 && std::string_view(argv[1]) == "--external-fec-sweep") {
            std::cout << run_sweep(external_fec_controllers()).report;
            return 0;
        }
        if (argc == 3 &&
            std::string_view(argv[1]) == "--verify-external-fec-sweep") {
            return verify_report(argv[2], external_fec_controllers());
        }
#endif
        std::cerr << "usage: aurora_transport_benchmark --sweep\n"
                     "       aurora_transport_benchmark --verify-sweep "
                     "<canonical-report>\n"
#ifdef AURORA_USE_WIREHAIR
                     "       aurora_transport_benchmark --external-fec-sweep\n"
                     "       aurora_transport_benchmark "
                     "--verify-external-fec-sweep <canonical-report>\n"
#endif
            ;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "transport benchmark: " << error.what() << '\n';
        return 1;
    }
}

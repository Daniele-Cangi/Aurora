#pragma once

#include "aurora/build/BuildProvenance.hpp"
#include "ChannelTrace.hpp"
#include "../control/TransportPolicy.hpp"
#include "../fec/GenerationCodec.hpp"
#include "../transport/GenerationManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace aurora::simulation {

enum class BaselineKind : std::uint8_t {
    NO_FEC,
    REPETITION_2X,
    FIXED_LT_LIKE,
    CLASS_AWARE_FIXED,
    ADAPTIVE_AURORA
};

inline const char* baseline_name(BaselineKind kind) {
    switch (kind) {
        case BaselineKind::NO_FEC: return "no-fec";
        case BaselineKind::REPETITION_2X: return "repetition-2x";
        case BaselineKind::FIXED_LT_LIKE: return "fixed-lt-like";
        case BaselineKind::CLASS_AWARE_FIXED: return "class-aware-fixed";
        case BaselineKind::ADAPTIVE_AURORA: return "adaptive-aurora";
    }
    return "unknown";
}

struct BenchmarkScenario {
    std::size_t payload_size = 4096;
    std::size_t symbol_size = 128;
    std::size_t critical_bytes = 512;
    std::size_t trials = 200;
    std::uint64_t seed = 0xA607AULL;
    std::uint64_t deadline_ms = 1'000;
    ChannelScenario channel;

    bool operator==(const BenchmarkScenario&) const = default;
};

struct BenchmarkTrialResult {
    BaselineKind baseline = BaselineKind::NO_FEC;
    std::size_t trial_index = 0;
    bool payload_delivered = false;
    bool critical_delivered = false;
    std::uint64_t transmitted_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint32_t transmitted_symbols = 0;
    std::uint32_t received_symbols = 0;
    std::uint32_t source_symbols_recovered = 0;
    std::optional<std::uint32_t> innovative_symbols;
    std::uint32_t required_source_symbols = 0;
    double effective_overhead = 0.0;
    transport::DecodeStatus final_status = transport::DecodeStatus::NO_PROGRESS;

    bool operator==(const BenchmarkTrialResult&) const = default;
};

struct BenchmarkResult {
    BaselineKind baseline = BaselineKind::NO_FEC;
    std::size_t trials = 0;
    std::size_t payloads_delivered = 0;
    std::size_t critical_segments_delivered = 0;
    std::uint64_t transmitted_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t useful_payload_bytes = 0;
    std::uint64_t useful_critical_bytes = 0;
    std::uint64_t transmitted_symbols = 0;
    std::uint64_t received_symbols = 0;
    std::optional<std::uint64_t> innovative_symbols;
    double delivery_rate = 0.0;
    double delivery_ci95_low = 0.0;
    double delivery_ci95_high = 0.0;
    double critical_delivery_rate = 0.0;
    double critical_delivery_ci95_low = 0.0;
    double critical_delivery_ci95_high = 0.0;
    double goodput = 0.0;
    std::optional<double> transmitted_bytes_per_delivered_byte;
    std::optional<double> innovative_symbol_ratio;
    double mean_effective_overhead = 0.0;
    std::optional<std::size_t> overhead_direction_changes;

    bool operator==(const BenchmarkResult&) const = default;
};

struct BenchmarkReport {
    BenchmarkScenario scenario;
    build::BuildProvenance build_provenance;
    std::string channel_trace_fingerprint;
    ChannelTraceCorpus channel_traces;
    std::vector<BenchmarkResult> summaries;
    std::vector<BenchmarkTrialResult> trial_results;

    bool operator==(const BenchmarkReport&) const = default;
};

// Deterministic benchmark driven by an explicit channel trace. Every policy sees
// the same delivered/lost outcome at a given transmission slot. This is synthetic
// simulation evidence and deliberately excludes wall-clock scheduling.
class BaselineBenchmark {
public:
    [[nodiscard]] std::vector<BenchmarkResult> run(
        const BenchmarkScenario& scenario) const {
        return run_report(scenario).summaries;
    }

    [[nodiscard]] BenchmarkReport run_report(
        const BenchmarkScenario& scenario) const {
        validate(scenario);
        ChannelTraceCorpus corpus;
        corpus.experiment_seed = scenario.seed;
        corpus.scenario_id = channel_scenario_id(scenario.channel);
        const auto slots = required_trace_slots(scenario);
        ChannelTraceGenerator generator;
        corpus.traces.reserve(scenario.trials);
        for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
            corpus.traces.push_back(generator.generate(
                scenario.channel, scenario.seed, trial, slots));
        }
        return replay(scenario, corpus);
    }

    [[nodiscard]] BenchmarkReport replay(
        const BenchmarkScenario& scenario,
        const ChannelTraceCorpus& corpus) const {
        validate(scenario);
        validate_corpus(scenario, corpus);

        BenchmarkReport report;
        report.scenario = scenario;
        report.build_provenance = build::current_build_provenance();
        report.channel_traces = corpus;
        report.channel_trace_fingerprint = corpus.fingerprint();
        report.summaries = {
            result_for(BaselineKind::NO_FEC, scenario.trials),
            result_for(BaselineKind::REPETITION_2X, scenario.trials),
            result_for(BaselineKind::FIXED_LT_LIKE, scenario.trials),
            result_for(BaselineKind::CLASS_AWARE_FIXED, scenario.trials),
            result_for(BaselineKind::ADAPTIVE_AURORA, scenario.trials)};
        report.trial_results.reserve(scenario.trials * report.summaries.size());

        auto raw_policy = std::make_shared<control::FixedTransportPolicy>(
            "raw-systematic", 1.0, 1.0, 1.0);
        auto fixed_policy = std::make_shared<control::FixedTransportPolicy>(
            "fixed-lt-like", 1.5, 1.5, 1.5);
        auto class_policy = std::make_shared<control::FixedTransportPolicy>(
            "class-aware-fixed", 2.5, 1.5, 1.0);
        auto adaptive_policy = std::make_shared<control::BiologicalAdaptivePolicy>();

        auto codec = std::make_shared<fec::ExperimentalLtLikeCodec>();
        transport::GenerationManager raw_manager(codec, raw_policy, 4);
        transport::GenerationManager fixed_manager(codec, fixed_policy, 4);
        transport::GenerationManager class_manager(codec, class_policy, 4);
        transport::GenerationManager adaptive_manager(codec, adaptive_policy, 4);

        const auto contract = make_contract(scenario);
        auto raw_contract = contract;
        raw_contract.minimum_critical_overhead = 1.0;

        for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
            const auto bytes = make_payload(scenario.payload_size, scenario.seed, trial);
            const auto token = "benchmark-" + std::to_string(trial);
            const auto& trace = corpus.traces[trial];

            const auto raw = raw_manager.spawn(
                raw_contract, token, bytes, scenario.symbol_size, 0);
            append_trial(report, observe_systematic_baseline(
                BaselineKind::NO_FEC, raw, trace, trial, 1));
            append_trial(report, observe_systematic_baseline(
                BaselineKind::REPETITION_2X, raw, trace, trial, 2));
            // Complete the internal raw manager independently of the simulated
            // baseline result so its bounded generation store remains reusable.
            (void)raw_manager.integrate(raw.descriptor.generation_id, raw.packets, 0);

            append_trial(report, observe_coded_baseline(
                BaselineKind::FIXED_LT_LIKE, fixed_manager, contract,
                token, bytes, scenario, trace, trial));
            append_trial(report, observe_coded_baseline(
                BaselineKind::CLASS_AWARE_FIXED, class_manager, contract,
                token, bytes, scenario, trace, trial));
            append_trial(report, observe_coded_baseline(
                BaselineKind::ADAPTIVE_AURORA, adaptive_manager, contract,
                token, bytes, scenario, trace, trial));
        }

        for (auto& summary : report.summaries) {
            finalize(summary, report.trial_results);
        }
        return report;
    }

private:
    static void validate(const BenchmarkScenario& scenario) {
        scenario.channel.validate();
        if (scenario.payload_size == 0 || scenario.symbol_size == 0 || scenario.trials == 0) {
            throw std::invalid_argument(
                "baseline benchmark: payload, symbol size, and trial count must be positive");
        }
        if (scenario.critical_bytes > scenario.payload_size) {
            throw std::invalid_argument(
                "baseline benchmark: critical byte count exceeds payload size");
        }
        const auto source_symbols = segmented_source_symbols(scenario);
        if (source_symbols == 0 || source_symbols >= std::numeric_limits<std::uint32_t>::max() / 4U) {
            throw std::invalid_argument(
                "baseline benchmark: source symbol count is out of range");
        }
        if (scenario.deadline_ms == 0 ||
            scenario.deadline_ms == std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument(
                "baseline benchmark: deadline must permit a terminal expiry observation");
        }
    }

    static std::size_t segmented_source_symbols(const BenchmarkScenario& scenario) {
        const auto symbols_for = [&](std::size_t bytes) {
            return (bytes + scenario.symbol_size - 1) / scenario.symbol_size;
        };
        if (scenario.critical_bytes == 0 ||
            scenario.critical_bytes == scenario.payload_size) {
            return symbols_for(scenario.payload_size);
        }
        return symbols_for(scenario.critical_bytes) +
               symbols_for(scenario.payload_size - scenario.critical_bytes);
    }

    static std::size_t required_trace_slots(const BenchmarkScenario& scenario) {
        return segmented_source_symbols(scenario) * 4U;
    }

    static void validate_corpus(const BenchmarkScenario& scenario,
                                const ChannelTraceCorpus& corpus) {
        if (corpus.traces.size() != scenario.trials) {
            throw std::invalid_argument(
                "baseline benchmark: channel trace count does not match trials");
        }
        const auto required_slots = required_trace_slots(scenario);
        const auto expected_scenario_id = channel_scenario_id(scenario.channel);
        if (corpus.experiment_seed != scenario.seed ||
            corpus.scenario_id != expected_scenario_id) {
            throw std::invalid_argument(
                "baseline benchmark: channel trace metadata does not match configuration");
        }
        for (std::size_t i = 0; i < corpus.traces.size(); ++i) {
            const auto& trace = corpus.traces[i];
            if (trace.trial_index != i || trace.outcomes.size() < required_slots) {
                throw std::invalid_argument(
                    "baseline benchmark: channel trace is too short or out of order");
            }
            if (trace.scenario_id != expected_scenario_id) {
                throw std::invalid_argument(
                    "baseline benchmark: channel trace scenario does not match configuration");
            }
        }
    }

    static BenchmarkResult result_for(BaselineKind kind, std::size_t trials) {
        BenchmarkResult result;
        result.baseline = kind;
        result.trials = trials;
        return result;
    }

    static transport::TransportContract make_contract(const BenchmarkScenario& scenario) {
        transport::TransportContract contract;
        contract.deadline_s = static_cast<double>(scenario.deadline_ms) / 1000.0;
        contract.reliability = 0.99;
        contract.importance = transport::TransportImportance::IMPORTANT;
        contract.minimum_critical_overhead = 1.5;
        contract.maximum_repair_amplification = 4.0;
        contract.maximum_generation_bytes = scenario.payload_size;
        contract.maximum_source_symbols = static_cast<std::uint32_t>(
            segmented_source_symbols(scenario));
        contract.experiment_seed = scenario.seed;
        if (scenario.critical_bytes > 0) {
            contract.segments.push_back({
                0,
                scenario.critical_bytes,
                transport::TransportImportance::CRITICAL,
                scenario.deadline_ms,
                0.999});
        }
        contract.validate();
        return contract;
    }

    static std::vector<std::uint8_t> make_payload(
        std::size_t size,
        std::uint64_t seed,
        std::size_t trial) {
        std::vector<std::uint8_t> bytes(size);
        std::uint64_t state = seed ^ (static_cast<std::uint64_t>(trial) << 32U);
        for (auto& byte : bytes) {
            state = ::fec::detail::splitmix64(state);
            byte = static_cast<std::uint8_t>(state & 0xFFU);
        }
        return bytes;
    }

    static BenchmarkTrialResult observe_systematic_baseline(
        BaselineKind kind,
        const transport::GenerationSpawnResult& generation,
        const ChannelTrace& trace,
        std::size_t trial,
        std::uint32_t repetitions) {
        BenchmarkTrialResult result;
        result.baseline = kind;
        result.trial_index = trial;
        result.required_source_symbols = generation.descriptor.total_source_symbols;
        result.transmitted_symbols = static_cast<std::uint32_t>(
            generation.packets.size() * repetitions);
        result.transmitted_bytes = static_cast<std::uint64_t>(result.transmitted_symbols) *
                                   generation.descriptor.symbol_size;
        result.effective_overhead = result.required_source_symbols == 0
            ? 0.0
            : static_cast<double>(result.transmitted_symbols) /
              static_cast<double>(result.required_source_symbols);

        std::vector<bool> source_recovered(generation.packets.size(), false);
        std::vector<bool> segment_complete(generation.descriptor.segments.size(), true);
        std::size_t slot = 0;
        // Copy-major ordering makes the complete first pass identical to no-FEC.
        for (std::uint32_t copy = 0; copy < repetitions; ++copy) {
            for (std::size_t packet_index = 0;
                 packet_index < generation.packets.size(); ++packet_index, ++slot) {
                if (trace.delivered(slot)) {
                    ++result.received_symbols;
                    result.received_bytes += generation.packets[packet_index].fp.data.size();
                    source_recovered[packet_index] = true;
                }
            }
        }
        for (std::size_t i = 0; i < generation.packets.size(); ++i) {
            if (source_recovered[i]) {
                ++result.source_symbols_recovered;
            } else {
                segment_complete.at(generation.packets[i].segment_id) = false;
            }
        }

        result.payload_delivered = std::all_of(
            segment_complete.begin(), segment_complete.end(), [](bool value) { return value; });
        bool has_critical = false;
        result.critical_delivered = true;
        for (std::size_t i = 0; i < generation.descriptor.segments.size(); ++i) {
            if (generation.descriptor.segments[i].importance ==
                transport::TransportImportance::CRITICAL) {
                has_critical = true;
                result.critical_delivered = result.critical_delivered && segment_complete[i];
            }
        }
        if (!has_critical) result.critical_delivered = result.payload_delivered;
        result.final_status = result.payload_delivered
            ? transport::DecodeStatus::COMPLETE
            : transport::DecodeStatus::INSUFFICIENT_RANK;
        return result;
    }

    static BenchmarkTrialResult observe_coded_baseline(
        BaselineKind kind,
        transport::GenerationManager& manager,
        const transport::TransportContract& contract,
        const std::string& token,
        const std::vector<std::uint8_t>& bytes,
        const BenchmarkScenario& scenario,
        const ChannelTrace& trace,
        std::size_t trial) {
        const auto generation = manager.spawn(
            contract, token, bytes, scenario.symbol_size, 0);
        BenchmarkTrialResult result;
        result.baseline = kind;
        result.trial_index = trial;
        result.required_source_symbols = generation.descriptor.total_source_symbols;
        result.transmitted_symbols = static_cast<std::uint32_t>(generation.packets.size());
        result.transmitted_bytes = static_cast<std::uint64_t>(result.transmitted_symbols) *
                                   generation.descriptor.symbol_size;
        result.effective_overhead = result.required_source_symbols == 0
            ? 0.0
            : static_cast<double>(result.transmitted_symbols) /
              static_cast<double>(result.required_source_symbols);

        std::vector<::fec::Pkt> received;
        received.reserve(generation.packets.size());
        for (std::size_t slot = 0; slot < generation.packets.size(); ++slot) {
            if (trace.delivered(slot)) {
                ++result.received_symbols;
                result.received_bytes += generation.packets[slot].fp.data.size();
                received.push_back(generation.packets[slot]);
            }
        }
        auto decode = manager.integrate(
            generation.descriptor.generation_id, received, scenario.deadline_ms);
        if (!decode.delivered()) {
            decode = manager.integrate(
                generation.descriptor.generation_id, {}, scenario.deadline_ms + 1);
        }
        result.payload_delivered = decode.delivered();
        result.critical_delivered = scenario.critical_bytes == 0
            ? result.payload_delivered
            : decode.critical_complete;
        result.source_symbols_recovered = decode.decoder_rank;
        result.innovative_symbols = decode.innovative_symbols;
        result.final_status = decode.status;
        return result;
    }

    static void append_trial(BenchmarkReport& report, BenchmarkTrialResult trial) {
        auto found = std::find_if(report.summaries.begin(), report.summaries.end(),
            [&](const auto& summary) { return summary.baseline == trial.baseline; });
        if (found == report.summaries.end()) {
            throw std::logic_error("baseline benchmark: missing aggregate result");
        }
        const auto& descriptor_bytes = report.scenario.payload_size;
        const auto critical_bytes = report.scenario.critical_bytes == 0
            ? report.scenario.payload_size
            : report.scenario.critical_bytes;
        if (trial.payload_delivered) {
            ++found->payloads_delivered;
            found->useful_payload_bytes += descriptor_bytes;
        }
        if (trial.critical_delivered) {
            ++found->critical_segments_delivered;
            found->useful_critical_bytes += critical_bytes;
        }
        found->transmitted_bytes += trial.transmitted_bytes;
        found->received_bytes += trial.received_bytes;
        found->transmitted_symbols += trial.transmitted_symbols;
        found->received_symbols += trial.received_symbols;
        if (trial.innovative_symbols.has_value()) {
            found->innovative_symbols = found->innovative_symbols.value_or(0) +
                                        *trial.innovative_symbols;
        }
        report.trial_results.push_back(std::move(trial));
    }

    static std::pair<double, double> wilson_interval(std::size_t successes,
                                                     std::size_t trials) {
        if (trials == 0) return {0.0, 0.0};
        constexpr double z = 1.959963984540054;
        const auto n = static_cast<double>(trials);
        const auto p = static_cast<double>(successes) / n;
        const auto z2 = z * z;
        const auto denominator = 1.0 + z2 / n;
        const auto center = (p + z2 / (2.0 * n)) / denominator;
        const auto radius = z * std::sqrt((p * (1.0 - p) / n) +
                                          (z2 / (4.0 * n * n))) / denominator;
        return {std::max(0.0, center - radius), std::min(1.0, center + radius)};
    }

    static void finalize(BenchmarkResult& result,
                         const std::vector<BenchmarkTrialResult>& trials) {
        result.delivery_rate = static_cast<double>(result.payloads_delivered) /
                               static_cast<double>(result.trials);
        const auto delivery_interval = wilson_interval(
            result.payloads_delivered, result.trials);
        result.delivery_ci95_low = delivery_interval.first;
        result.delivery_ci95_high = delivery_interval.second;
        result.critical_delivery_rate =
            static_cast<double>(result.critical_segments_delivered) /
            static_cast<double>(result.trials);
        const auto critical_interval = wilson_interval(
            result.critical_segments_delivered, result.trials);
        result.critical_delivery_ci95_low = critical_interval.first;
        result.critical_delivery_ci95_high = critical_interval.second;
        result.goodput = result.transmitted_bytes == 0
            ? 0.0
            : static_cast<double>(result.useful_payload_bytes) /
              static_cast<double>(result.transmitted_bytes);
        if (result.useful_payload_bytes > 0) {
            result.transmitted_bytes_per_delivered_byte =
                static_cast<double>(result.transmitted_bytes) /
                static_cast<double>(result.useful_payload_bytes);
        }
        if (result.innovative_symbols.has_value() && result.received_symbols > 0) {
            result.innovative_symbol_ratio =
                static_cast<double>(*result.innovative_symbols) /
                static_cast<double>(result.received_symbols);
        }

        double overhead_sum = 0.0;
        double previous_overhead = 0.0;
        int previous_direction = 0;
        bool have_previous = false;
        for (const auto& trial : trials) {
            if (trial.baseline != result.baseline) continue;
            overhead_sum += trial.effective_overhead;
            if (have_previous) {
                const auto delta = trial.effective_overhead - previous_overhead;
                const auto direction = delta > 0.0 ? 1 : delta < 0.0 ? -1 : 0;
                if (direction != 0 && previous_direction != 0 && direction != previous_direction) {
                    result.overhead_direction_changes =
                        result.overhead_direction_changes.value_or(0) + 1;
                }
                if (direction != 0) previous_direction = direction;
            }
            previous_overhead = trial.effective_overhead;
            have_previous = true;
        }
        result.mean_effective_overhead = overhead_sum /
                                         static_cast<double>(result.trials);
        if (result.baseline == BaselineKind::ADAPTIVE_AURORA &&
            !result.overhead_direction_changes.has_value()) {
            result.overhead_direction_changes = 0;
        }
    }
};

inline void save_benchmark_trial_csv(const BenchmarkReport& report,
                                     const std::string& path) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("baseline benchmark: cannot open trial output: " + path);
    }
    output << "build_schema,build_fingerprint,build_commit,build_source_state,compiler_id,"
              "compiler_version,target_system,target_processor,build_type,build_generator,"
              "execution_profile,evidence_level,hardware_validated,crypto_profile,fec_profile,"
              "trace_fingerprint,scenario,scenario_id,experiment_seed,baseline,trial,"
              "payload_delivered,critical_delivered,transmitted_bytes,received_bytes,"
              "transmitted_symbols,received_symbols,source_symbols_recovered,"
              "innovative_symbols,required_source_symbols,effective_overhead,final_status\n";
    output.precision(17);
    for (const auto& trial : report.trial_results) {
        output << report.build_provenance.schema << ','
               << report.build_provenance.fingerprint() << ','
               << report.build_provenance.commit << ','
               << report.build_provenance.source_state << ','
               << report.build_provenance.compiler_id << ','
               << report.build_provenance.compiler_version << ','
               << report.build_provenance.target_system << ','
               << report.build_provenance.target_processor << ','
               << report.build_provenance.build_type << ','
               << report.build_provenance.generator << ','
               << report.build_provenance.execution_profile << ','
               << report.build_provenance.evidence_level << ','
               << report.build_provenance.hardware_validated << ','
               << report.build_provenance.crypto_profile << ','
               << report.build_provenance.fec_profile << ','
               << report.channel_trace_fingerprint << ','
               << channel_scenario_name(report.scenario.channel.kind) << ','
               << report.channel_traces.scenario_id << ','
               << report.scenario.seed << ','
               << baseline_name(trial.baseline) << ','
               << trial.trial_index << ','
               << trial.payload_delivered << ','
               << trial.critical_delivered << ','
               << trial.transmitted_bytes << ','
               << trial.received_bytes << ','
               << trial.transmitted_symbols << ','
               << trial.received_symbols << ','
               << trial.source_symbols_recovered << ','
               << (trial.innovative_symbols.has_value()
                       ? std::to_string(*trial.innovative_symbols)
                       : std::string("N/A")) << ','
               << trial.required_source_symbols << ','
               << trial.effective_overhead << ','
               << static_cast<unsigned>(trial.final_status) << '\n';
    }
    if (!output) {
        throw std::runtime_error("baseline benchmark: failed while writing trial output: " + path);
    }
}

} // namespace aurora::simulation

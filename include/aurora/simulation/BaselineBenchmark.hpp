#pragma once

#include "../control/TransportPolicy.hpp"
#include "../fec/GenerationCodec.hpp"
#include "../transport/GenerationManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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
    double packet_loss_rate = 0.25;
    std::size_t trials = 200;
    std::uint64_t seed = 0xA607AULL;
    std::uint64_t deadline_ms = 1'000;
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
    double delivery_rate = 0.0;
    double critical_delivery_rate = 0.0;
    double goodput = 0.0;

    bool operator==(const BenchmarkResult&) const = default;
};

// Deterministic shared-seed harness. Packet survival is keyed by the packet's
// segment/seed identity, so symbols common to multiple policies see the same
// channel outcome. This is a controlled simulation baseline, not field evidence.
class BaselineBenchmark {
public:
    [[nodiscard]] std::vector<BenchmarkResult> run(
        const BenchmarkScenario& scenario) const {
        validate(scenario);

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

        std::vector<BenchmarkResult> results{
            result_for(BaselineKind::NO_FEC, scenario.trials),
            result_for(BaselineKind::REPETITION_2X, scenario.trials),
            result_for(BaselineKind::FIXED_LT_LIKE, scenario.trials),
            result_for(BaselineKind::CLASS_AWARE_FIXED, scenario.trials),
            result_for(BaselineKind::ADAPTIVE_AURORA, scenario.trials)};

        const auto contract = make_contract(scenario);
        auto raw_contract = contract;
        raw_contract.minimum_critical_overhead = 1.0;

        for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
            const auto bytes = make_payload(scenario.payload_size, scenario.seed, trial);
            const auto token = "benchmark-" + std::to_string(trial);

            const auto raw = raw_manager.spawn(
                raw_contract, token, bytes, scenario.symbol_size, 0);
            observe_systematic_baseline(results[0], raw, scenario, trial, 1);
            observe_systematic_baseline(results[1], raw, scenario, trial, 2);
            // Keep the shared raw generation store bounded without using its
            // synthetic baseline loss outcome as adaptive feedback.
            (void)raw_manager.integrate(
                raw.descriptor.generation_id, raw.packets, 0);

            observe_coded_baseline(
                results[2], fixed_manager, contract, token, bytes, scenario, trial);
            observe_coded_baseline(
                results[3], class_manager, contract, token, bytes, scenario, trial);
            observe_coded_baseline(
                results[4], adaptive_manager, contract, token, bytes, scenario, trial);
        }

        for (auto& result : results) finalize(result);
        return results;
    }

private:
    static void validate(const BenchmarkScenario& scenario) {
        if (scenario.payload_size == 0 || scenario.symbol_size == 0 || scenario.trials == 0) {
            throw std::invalid_argument(
                "baseline benchmark: payload, symbol size, and trial count must be positive");
        }
        if (scenario.critical_bytes > scenario.payload_size) {
            throw std::invalid_argument(
                "baseline benchmark: critical byte count exceeds payload size");
        }
        const auto source_symbols =
            (scenario.payload_size + scenario.symbol_size - 1) / scenario.symbol_size;
        if (source_symbols >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument(
                "baseline benchmark: source symbol count is out of range");
        }
        if (scenario.deadline_ms == 0 ||
            scenario.deadline_ms == std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument(
                "baseline benchmark: deadline must permit a terminal expiry observation");
        }
        if (!std::isfinite(scenario.packet_loss_rate) ||
            scenario.packet_loss_rate < 0.0 || scenario.packet_loss_rate > 1.0) {
            throw std::invalid_argument(
                "baseline benchmark: packet loss rate must be in [0, 1]");
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
            (scenario.payload_size + scenario.symbol_size - 1) / scenario.symbol_size + 1);
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

    static bool survives(const BenchmarkScenario& scenario,
                         std::size_t trial,
                         const ::fec::Pkt& packet,
                         std::uint32_t copy) {
        std::uint64_t value = scenario.seed;
        value = ::fec::detail::splitmix64(value ^ static_cast<std::uint64_t>(trial));
        value = ::fec::detail::splitmix64(
            value ^ (static_cast<std::uint64_t>(packet.segment_id) << 32U) ^ packet.fp.seed);
        value = ::fec::detail::splitmix64(
            value ^ (static_cast<std::uint64_t>(copy) * 0xD1B54A32D192ED03ULL));
        const double sample = static_cast<double>(value >> 11U) * 0x1.0p-53;
        return sample >= scenario.packet_loss_rate;
    }

    static std::size_t critical_payload_bytes(
        const transport::GenerationDescriptor& descriptor) {
        std::size_t bytes = 0;
        for (const auto& segment : descriptor.segments) {
            if (segment.importance == transport::TransportImportance::CRITICAL) {
                bytes += segment.length;
            }
        }
        return bytes;
    }

    static void observe_systematic_baseline(
        BenchmarkResult& result,
        const transport::GenerationSpawnResult& generation,
        const BenchmarkScenario& scenario,
        std::size_t trial,
        std::uint32_t repetitions) {
        std::vector<bool> segment_complete(generation.descriptor.segments.size(), true);
        for (const auto& packet : generation.packets) {
            bool recovered = false;
            for (std::uint32_t copy = 0; copy < repetitions; ++copy) {
                result.transmitted_bytes += packet.fp.data.size();
                if (survives(scenario, trial, packet, copy)) {
                    result.received_bytes += packet.fp.data.size();
                    recovered = true;
                }
            }
            if (!recovered) segment_complete.at(packet.segment_id) = false;
        }

        const bool payload_complete = std::all_of(
            segment_complete.begin(), segment_complete.end(), [](bool value) { return value; });
        bool has_critical = false;
        bool critical_complete = true;
        for (std::size_t i = 0; i < generation.descriptor.segments.size(); ++i) {
            if (generation.descriptor.segments[i].importance ==
                transport::TransportImportance::CRITICAL) {
                has_critical = true;
                critical_complete = critical_complete && segment_complete[i];
            }
        }
        if (!has_critical) critical_complete = payload_complete;
        observe_delivery(result, generation.descriptor, payload_complete, critical_complete);
    }

    static void observe_coded_baseline(
        BenchmarkResult& result,
        transport::GenerationManager& manager,
        const transport::TransportContract& contract,
        const std::string& token,
        const std::vector<std::uint8_t>& bytes,
        const BenchmarkScenario& scenario,
        std::size_t trial) {
        const auto generation = manager.spawn(
            contract, token, bytes, scenario.symbol_size, 0);
        std::vector<::fec::Pkt> received;
        received.reserve(generation.packets.size());
        for (const auto& packet : generation.packets) {
            result.transmitted_bytes += packet.fp.data.size();
            if (survives(scenario, trial, packet, 0)) {
                result.received_bytes += packet.fp.data.size();
                received.push_back(packet);
            }
        }
        auto report = manager.integrate(
            generation.descriptor.generation_id,
            received,
            scenario.deadline_ms);
        if (!report.delivered()) {
            report = manager.integrate(
                generation.descriptor.generation_id,
                {},
                scenario.deadline_ms + 1);
        }
        observe_delivery(
            result, generation.descriptor, report.delivered(), report.critical_complete);
    }

    static void observe_delivery(BenchmarkResult& result,
                                 const transport::GenerationDescriptor& descriptor,
                                 bool payload_complete,
                                 bool critical_complete) {
        if (payload_complete) {
            ++result.payloads_delivered;
            result.useful_payload_bytes += descriptor.original_payload_length;
        }
        if (critical_complete) {
            ++result.critical_segments_delivered;
            result.useful_critical_bytes += critical_payload_bytes(descriptor);
        }
    }

    static void finalize(BenchmarkResult& result) {
        result.delivery_rate = static_cast<double>(result.payloads_delivered) /
                               static_cast<double>(result.trials);
        result.critical_delivery_rate =
            static_cast<double>(result.critical_segments_delivered) /
            static_cast<double>(result.trials);
        result.goodput = result.transmitted_bytes == 0
            ? 0.0
            : static_cast<double>(result.useful_payload_bytes) /
              static_cast<double>(result.transmitted_bytes);
    }
};

} // namespace aurora::simulation

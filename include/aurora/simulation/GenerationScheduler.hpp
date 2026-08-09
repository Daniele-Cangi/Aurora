#pragma once

#include "../transport/TransportContract.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace aurora::simulation {

struct GenerationSchedulingCandidate {
    std::size_t index = 0;
    std::uint64_t arrives_at_ms = 0;
    std::uint64_t expires_at_ms = 0;
    transport::TransportImportance importance =
        transport::TransportImportance::IMPORTANT;
    bool arrived = false;
    bool terminal = false;
    std::optional<std::uint64_t> last_served_at_ms;
};

struct GenerationSchedulingPolicy {
    std::uint64_t service_quantum_ms = 1'000;
    std::uint64_t aging_interval_ms = 2'000;
    std::uint64_t starvation_limit_ms = 3'000;

    void validate() const {
        if (service_quantum_ms == 0 || aging_interval_ms == 0 ||
            starvation_limit_ms == 0) {
            throw std::invalid_argument(
                "generation scheduler: policy intervals must be positive");
        }
    }

    friend bool operator==(const GenerationSchedulingPolicy&,
                           const GenerationSchedulingPolicy&) = default;
};

inline std::uint64_t maximum_service_gap_ms(
    const GenerationSchedulingPolicy& policy,
    std::size_t eligible_generations) {
    policy.validate();
    if (eligible_generations == 0) return 0;
    const auto additional_quanta = eligible_generations - 1;
    if (additional_quanta >
        (std::numeric_limits<std::uint64_t>::max() -
         policy.starvation_limit_ms) / policy.service_quantum_ms) {
        throw std::overflow_error(
            "generation scheduler: service-gap bound overflows");
    }
    return policy.starvation_limit_ms +
        static_cast<std::uint64_t>(additional_quanta) *
            policy.service_quantum_ms;
}

// Deterministic aging/fair scheduling. Starved candidates are ordered by their
// fairness deadline; otherwise effective priority (after aging) precedes EDF.
// Arrival/index tie-breaks keep selection independently replayable.
inline std::optional<std::size_t> select_scheduled_generation(
    const std::vector<GenerationSchedulingCandidate>& candidates,
    std::uint64_t now_ms,
    const GenerationSchedulingPolicy& policy = {}) {
    policy.validate();
    std::optional<std::size_t> selected;
    auto selected_key = std::tuple{
        std::uint8_t{0}, std::uint64_t{0}, std::uint64_t{0},
        std::uint64_t{0}, std::size_t{0}};
    for (const auto& candidate : candidates) {
        if (candidate.expires_at_ms < candidate.arrives_at_ms) {
            throw std::invalid_argument(
                "generation scheduler: expiry precedes arrival");
        }
        if (!candidate.arrived || candidate.terminal ||
            candidate.arrives_at_ms > now_ms) {
            continue;
        }
        const auto priority = static_cast<std::uint8_t>(candidate.importance);
        if (priority > static_cast<std::uint8_t>(
                transport::TransportImportance::ELASTIC)) {
            throw std::invalid_argument(
                "generation scheduler: invalid importance");
        }
        const auto last_service = candidate.last_served_at_ms.value_or(
            candidate.arrives_at_ms);
        if (last_service < candidate.arrives_at_ms || last_service > now_ms) {
            throw std::invalid_argument(
                "generation scheduler: invalid last-service time");
        }
        const auto waiting_ms = now_ms - last_service;
        const auto promotions = std::min<std::uint64_t>(
            priority, waiting_ms / policy.aging_interval_ms);
        const auto effective_priority = static_cast<std::uint8_t>(
            priority - promotions);
        const bool starved = waiting_ms >= policy.starvation_limit_ms;
        const auto fairness_deadline =
            last_service > std::numeric_limits<std::uint64_t>::max() -
                    policy.starvation_limit_ms
                ? std::numeric_limits<std::uint64_t>::max()
                : last_service + policy.starvation_limit_ms;
        const auto key = std::tuple{
            static_cast<std::uint8_t>(starved ? 0 : 1),
            starved ? fairness_deadline
                    : static_cast<std::uint64_t>(effective_priority),
            candidate.expires_at_ms,
            candidate.arrives_at_ms, candidate.index};
        if (!selected || key < selected_key) {
            selected = candidate.index;
            selected_key = key;
        }
    }
    return selected;
}

} // namespace aurora::simulation

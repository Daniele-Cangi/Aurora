#pragma once

#include "../transport/TransportContract.hpp"

#include <cstddef>
#include <cstdint>
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
};

// Deterministic priority-then-EDF scheduling. Stable arrival/index tie-breaks
// make the selection independently replayable.
inline std::optional<std::size_t> select_scheduled_generation(
    const std::vector<GenerationSchedulingCandidate>& candidates,
    std::uint64_t now_ms) {
    std::optional<std::size_t> selected;
    auto selected_key = std::tuple{
        std::uint8_t{0}, std::uint64_t{0}, std::uint64_t{0}, std::size_t{0}};
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
        const auto key = std::tuple{
            priority, candidate.expires_at_ms,
            candidate.arrives_at_ms, candidate.index};
        if (!selected || key < selected_key) {
            selected = candidate.index;
            selected_key = key;
        }
    }
    return selected;
}

} // namespace aurora::simulation

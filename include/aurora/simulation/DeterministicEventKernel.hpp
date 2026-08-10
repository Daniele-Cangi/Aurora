#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace aurora::simulation {

enum class SimulationEventType : std::uint8_t {
    GENERATION_ARRIVAL,
    TRANSPORT_QUANTUM
};

struct DeterministicSimulationEvent {
    std::uint64_t time_ms = 0;
    SimulationEventType type = SimulationEventType::TRANSPORT_QUANTUM;
    std::uint64_t sequence = 0;
    std::size_t subject = 0;

    [[nodiscard]] std::uint8_t phase() const {
        // State entering the system at a timestamp must be visible to work
        // scheduled at that same timestamp.
        switch (type) {
            case SimulationEventType::GENERATION_ARRIVAL: return 0;
            case SimulationEventType::TRANSPORT_QUANTUM: return 1;
        }
        throw std::logic_error("discrete-event kernel: invalid event type");
    }

    friend bool operator==(const DeterministicSimulationEvent&,
                           const DeterministicSimulationEvent&) = default;
};

class DeterministicEventKernel {
public:
    static constexpr std::size_t maximum_pending_events = 1'000'000;

    void schedule(std::uint64_t time_ms,
                  SimulationEventType type,
                  std::size_t subject = 0) {
        if (current_time_ms_ && time_ms < *current_time_ms_) {
            throw std::invalid_argument(
                "discrete-event kernel: cannot schedule an event in the past");
        }
        if (pending_.size() >= maximum_pending_events) {
            throw std::overflow_error(
                "discrete-event kernel: pending-event capacity exceeded");
        }
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "discrete-event kernel: event sequence exhausted");
        }
        DeterministicSimulationEvent event{
            time_ms, type, next_sequence_, subject};
        (void)event.phase();
        ++next_sequence_;
        pending_.push(event);
    }

    [[nodiscard]] std::optional<DeterministicSimulationEvent> pop_next() {
        if (pending_.empty()) return std::nullopt;
        auto event = pending_.top();
        pending_.pop();
        if (current_time_ms_ && event.time_ms < *current_time_ms_) {
            throw std::logic_error(
                "discrete-event kernel: event time regressed");
        }
        current_time_ms_ = event.time_ms;
        return event;
    }

    [[nodiscard]] bool empty() const { return pending_.empty(); }
    [[nodiscard]] std::size_t pending_events() const { return pending_.size(); }
    [[nodiscard]] std::optional<std::uint64_t> current_time_ms() const {
        return current_time_ms_;
    }

private:
    struct LaterEvent {
        bool operator()(const DeterministicSimulationEvent& left,
                        const DeterministicSimulationEvent& right) const {
            return std::tuple{left.time_ms, left.phase(), left.sequence} >
                std::tuple{right.time_ms, right.phase(), right.sequence};
        }
    };

    std::priority_queue<
        DeterministicSimulationEvent,
        std::vector<DeterministicSimulationEvent>,
        LaterEvent> pending_;
    std::optional<std::uint64_t> current_time_ms_;
    std::uint64_t next_sequence_ = 0;
};

} // namespace aurora::simulation

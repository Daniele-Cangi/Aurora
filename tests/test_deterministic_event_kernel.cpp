#include "../include/aurora/simulation/DeterministicEventKernel.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    using aurora::simulation::DeterministicEventKernel;
    using aurora::simulation::SimulationEventType;

    DeterministicEventKernel kernel;
    kernel.schedule(1'000, SimulationEventType::TRANSPORT_QUANTUM, 10);
    kernel.schedule(0, SimulationEventType::TRANSPORT_QUANTUM, 0);
    kernel.schedule(1'000, SimulationEventType::GENERATION_ARRIVAL, 1);
    kernel.schedule(1'000, SimulationEventType::GENERATION_ARRIVAL, 2);
    assert(kernel.pending_events() == 4);

    const auto first = kernel.pop_next();
    assert(first && first->time_ms == 0);
    assert(first->type == SimulationEventType::TRANSPORT_QUANTUM);

    const auto second = kernel.pop_next();
    const auto third = kernel.pop_next();
    const auto fourth = kernel.pop_next();
    assert(second && third && fourth);
    assert(second->type == SimulationEventType::GENERATION_ARRIVAL);
    assert(third->type == SimulationEventType::GENERATION_ARRIVAL);
    assert(second->subject == 1);
    assert(third->subject == 2);
    assert(fourth->type == SimulationEventType::TRANSPORT_QUANTUM);
    assert(kernel.current_time_ms() == 1'000);
    assert(kernel.empty());

    bool past_rejected = false;
    try {
        kernel.schedule(999, SimulationEventType::TRANSPORT_QUANTUM);
    } catch (const std::invalid_argument&) {
        past_rejected = true;
    }
    assert(past_rejected);

    bool invalid_type_rejected = false;
    try {
        kernel.schedule(
            1'000, static_cast<SimulationEventType>(255));
    } catch (const std::logic_error&) {
        invalid_type_rejected = true;
    }
    assert(invalid_type_rejected);

    // Events scheduled by a handler at the current timestamp still obey the
    // phase order before later timestamps.
    kernel.schedule(1'000, SimulationEventType::TRANSPORT_QUANTUM, 3);
    kernel.schedule(2'000, SimulationEventType::TRANSPORT_QUANTUM, 4);
    kernel.schedule(1'000, SimulationEventType::GENERATION_ARRIVAL, 5);
    assert(kernel.pop_next()->subject == 5);
    assert(kernel.pop_next()->subject == 3);
    assert(kernel.pop_next()->subject == 4);

    std::cout << "deterministic event kernel tests passed\n";
    return 0;
}

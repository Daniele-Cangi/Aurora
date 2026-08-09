#include "aurora/telemetry/DecisionReplayLog.hpp"
#include "aurora/telemetry/SimulationEventLedger.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: aurora_replay <decision-trace-file> [simulation-event-ledger]\n";
        return 2;
    }

    try {
        const auto log = aurora::telemetry::DecisionReplayLog::load(argv[1]);
        const auto verification = log.verify();
        if (!verification.ok) {
            std::cerr << "REPLAY_MISMATCH records=" << verification.records_verified
                      << " reason=" << verification.failure_reason << '\n';
            return 1;
        }
        if (argc == 3) {
            const auto events =
                aurora::telemetry::SimulationEventLedger::load(argv[2]);
            const auto event_verification = events.verify(log);
            if (!event_verification.ok) {
                std::cerr << "EVENT_REPLAY_MISMATCH records="
                          << event_verification.records_verified
                          << " reason=" << event_verification.failure_reason << '\n';
                return 1;
            }
            std::cout << "EVENT_REPLAY_OK records="
                      << event_verification.records_verified << '\n';
        } else {
            std::cout << "REPLAY_OK records=" << verification.records_verified << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "REPLAY_INVALID reason=" << error.what() << '\n';
        return 2;
    }
}

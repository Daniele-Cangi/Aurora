#include "aurora/telemetry/DecisionReplayLog.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: aurora_replay <decision-trace-file>\n";
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
        std::cout << "REPLAY_OK records=" << verification.records_verified << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "REPLAY_INVALID reason=" << error.what() << '\n';
        return 2;
    }
}

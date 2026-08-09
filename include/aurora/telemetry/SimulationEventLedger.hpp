#pragma once

#include "DecisionReplayLog.hpp"
#include "../simulation/ContactSchedule.hpp"
#include "../simulation/GenerationArrivalSchedule.hpp"
#include "../simulation/GenerationScheduler.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aurora::telemetry {

struct SimulationPoint {
    double x = 0.0;
    double y = 0.0;
};

struct SimulationObstacle {
    SimulationPoint centre;
    double radius = 0.0;
};

struct SimulationGenerationIdentity {
    std::uint64_t arrives_at_ms = 0;
    std::string tag;
    std::string generation_id;
    std::uint64_t descriptor_fingerprint = 0;
    std::uint32_t required_rank = 0;
    std::uint64_t initial_source_packets = 0;
    transport::TransportImportance scheduling_importance =
        transport::TransportImportance::IMPORTANT;
    std::uint64_t expires_at_ms = 0;

    friend bool operator==(const SimulationGenerationIdentity&,
                           const SimulationGenerationIdentity&) = default;
};

struct SimulationEventSession {
    std::uint64_t experiment_seed = 0;
    std::uint64_t initial_random_state = 0;
    std::uint64_t initial_source_buffer = 0;
    double source_energy_capacity_j = 0.0;
    double source_initial_energy_j = 0.0;
    double source_harvest_w = 0.0;
    double destination_energy_capacity_j = 0.0;
    double destination_initial_energy_j = 0.0;
    double destination_harvest_w = 0.0;
    SimulationPoint source_position;
    SimulationPoint destination_position;
    std::vector<SimulationPoint> ris_positions;
    std::vector<SimulationObstacle> obstacles;
    simulation::ContactSchedule contact_schedule =
        simulation::ContactSchedule::always_available();
    simulation::GenerationArrivalSchedule generation_arrival_schedule =
        simulation::GenerationArrivalSchedule::single_immediate();
    simulation::GenerationSchedulingPolicy generation_scheduling_policy;
    std::vector<SimulationGenerationIdentity> generations;
};

struct SimulationStepEvent {
    static constexpr std::uint32_t no_generation_arrival =
        std::numeric_limits<std::uint32_t>::max();

    std::uint64_t step = 0;
    std::uint64_t simulated_now_ms = 0;
    std::uint32_t active_generation_index = 0;
    std::uint32_t arrived_generation_index = no_generation_arrival;
    std::uint64_t arrived_source_packets = 0;
    std::uint64_t random_before = 0;
    std::uint64_t random_after_ris = 0;
    std::uint64_t random_after_action = 0;

    double source_energy_before_tick_j = 0.0;
    double source_energy_after_tick_j = 0.0;
    double source_energy_after_action_j = 0.0;
    double destination_energy_before_tick_j = 0.0;
    double destination_energy_after_tick_j = 0.0;
    double destination_energy_after_action_j = 0.0;

    std::uint64_t source_buffer_before = 0;
    std::uint64_t source_buffer_after_action = 0;
    std::uint64_t destination_buffer_before = 0;
    std::uint64_t destination_inbox_before = 0;
    std::uint64_t destination_buffer_after_ingest = 0;
    std::uint64_t destination_inbox_after_ingest = 0;
    std::uint64_t destination_buffer_after_action = 0;
    std::uint64_t destination_inbox_after_action = 0;

    std::uint32_t decoder_rank_before = 0;
    std::uint32_t decoder_rank_after = 0;
    std::uint8_t decode_status = 0;
    double entropy_residual = 0.0;
    double illumination = 0.0;
    double world_gain = 0.0;
    double snr_rf_db = 0.0;
    double snr_optical_db = 0.0;
    double snr_backscatter_db = 0.0;
    std::vector<std::uint8_t> ris_phases;
    simulation::ContactAvailability contact_available;
};

struct SimulationReplayVerification {
    bool ok = true;
    std::size_t records_verified = 0;
    std::string failure_reason;
};

struct DerivedSimulationEnvironment {
    std::uint64_t random_after_ris = 0;
    std::vector<std::uint8_t> ris_phases;
    double illumination = 0.0;
    double world_gain = 0.0;
    double snr_rf_db = 0.0;
    double snr_optical_db = 0.0;
    double snr_backscatter_db = 0.0;
};

namespace simulation_event_detail {

inline std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 7U;
    state ^= state >> 9U;
    state ^= state << 8U;
    return state;
}

inline double uniform(std::uint64_t& state) {
    return (next_random(state) >> 11U) *
        (1.0 / static_cast<double>((1ULL << 53U) - 1ULL));
}

inline bool near(double left, double right, double scale = 1.0) {
    return std::abs(left - right) <=
        1e-11 * std::max({1.0, std::abs(left), std::abs(right), scale});
}

inline double distance(const SimulationPoint& left,
                       const SimulationPoint& right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

inline SimulationPoint normalized(SimulationPoint value) {
    const double norm = std::hypot(value.x, value.y) + 1e-9;
    value.x /= norm;
    value.y /= norm;
    return value;
}

inline double free_space(const SimulationEventSession& session,
                         const SimulationPoint& left,
                         const SimulationPoint& right) {
    const double d = std::max(1e-1, distance(left, right));
    double gain = 1.0 / (d * d);
    for (const auto& obstacle : session.obstacles) {
        if (distance(left, obstacle.centre) < obstacle.radius ||
            distance(right, obstacle.centre) < obstacle.radius) {
            gain *= 0.1;
        }
    }
    return gain;
}

inline double bounce(const SimulationPoint& left,
                     const SimulationPoint& right,
                     const SimulationPoint& tile,
                     std::uint8_t phase) {
    const double dt = std::max(1e-1, distance(left, tile));
    const double dr = std::max(1e-1, distance(tile, right));
    const double radians = (phase % 4U) * (3.14159265358979323846 / 2.0);
    const double beam = 0.6 + 0.4 * std::cos(radians);
    const auto incoming = normalized({tile.x - left.x, tile.y - left.y});
    const auto outgoing = normalized({right.x - tile.x, right.y - tile.y});
    const double incidence = std::abs(
        incoming.x * outgoing.x + incoming.y * outgoing.y);
    return 0.15 * (1.0 / (dt * dt * dr * dr)) * beam * incidence;
}

inline double world_gain(const SimulationEventSession& session,
                         const std::vector<std::uint8_t>& phases) {
    double best = free_space(
        session, session.source_position, session.destination_position);
    for (std::size_t index = 0; index < session.ris_positions.size(); ++index) {
        best = std::max(best, bounce(
            session.source_position, session.destination_position,
            session.ris_positions[index], phases[index]));
    }
    for (std::size_t left = 0; left < session.ris_positions.size(); ++left) {
        for (std::size_t right = 0; right < session.ris_positions.size(); ++right) {
            if (left == right) continue;
            best = std::max(best,
                bounce(session.source_position, session.ris_positions[left],
                       session.ris_positions[left], phases[left]) *
                bounce(session.ris_positions[left], session.destination_position,
                       session.ris_positions[right], phases[right]) * 5e1);
        }
    }
    return best;
}

inline double snr(double gain, safety::LinkMode link, double illumination) {
    double value = 10.0 * std::log10(std::max(1e-12, gain));
    if (link == safety::LinkMode::BACKSCATTER) {
        value += illumination > 0.0 ? 8.0 : -6.0;
    } else if (link == safety::LinkMode::OPTICAL) {
        value -= 3.0;
    }
    return value + 15.0;
}

inline double historical_per(const std::vector<bool>& outcomes) {
    if (outcomes.empty()) return 0.4;
    const std::size_t begin = outcomes.size() > 50 ? outcomes.size() - 50 : 0;
    double failures = 0.0;
    for (std::size_t index = begin; index < outcomes.size(); ++index) {
        if (!outcomes[index]) failures += 1.0;
    }
    return std::clamp(
        failures / static_cast<double>(outcomes.size() - begin), 0.01, 0.99);
}

inline std::size_t link_index(safety::LinkMode link) {
    switch (link) {
        case safety::LinkMode::RF: return 0;
        case safety::LinkMode::OPTICAL: return 1;
        case safety::LinkMode::BACKSCATTER: return 2;
    }
    return 0;
}

inline std::string hex_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

inline std::uint64_t parse_u64(const std::string& text, int base,
                               const char* field) {
    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(text, &consumed, base);
    } catch (...) {
        throw std::invalid_argument(
            std::string("simulation event ledger: invalid ") + field);
    }
    if (consumed != text.size()) {
        throw std::invalid_argument(
            std::string("simulation event ledger: invalid ") + field);
    }
    return value;
}

inline std::string double_hex(double value) {
    return hex_u64(std::bit_cast<std::uint64_t>(value));
}

inline double parse_double(const std::string& text, const char* field) {
    return std::bit_cast<double>(parse_u64(text, 16, field));
}

inline std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto end = value.find(delimiter, start);
        fields.push_back(value.substr(start, end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

inline std::string encode_points(const std::vector<SimulationPoint>& points) {
    std::ostringstream output;
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (index != 0) output << ';';
        output << double_hex(points[index].x) << ',' << double_hex(points[index].y);
    }
    return output.str();
}

inline std::vector<SimulationPoint> decode_points(const std::string& encoded) {
    std::vector<SimulationPoint> points;
    if (encoded.empty()) return points;
    for (const auto& item : split(encoded, ';')) {
        const auto fields = split(item, ',');
        if (fields.size() != 2) {
            throw std::invalid_argument("simulation event ledger: invalid point");
        }
        points.push_back({
            parse_double(fields[0], "point x"),
            parse_double(fields[1], "point y")});
    }
    return points;
}

inline std::string encode_obstacles(
    const std::vector<SimulationObstacle>& obstacles) {
    std::ostringstream output;
    for (std::size_t index = 0; index < obstacles.size(); ++index) {
        if (index != 0) output << ';';
        output << double_hex(obstacles[index].centre.x) << ','
               << double_hex(obstacles[index].centre.y) << ','
               << double_hex(obstacles[index].radius);
    }
    return output.str();
}

inline std::vector<SimulationObstacle> decode_obstacles(
    const std::string& encoded) {
    std::vector<SimulationObstacle> obstacles;
    if (encoded.empty()) return obstacles;
    for (const auto& item : split(encoded, ';')) {
        const auto fields = split(item, ',');
        if (fields.size() != 3) {
            throw std::invalid_argument("simulation event ledger: invalid obstacle");
        }
        obstacles.push_back({
            {parse_double(fields[0], "obstacle x"),
             parse_double(fields[1], "obstacle y")},
            parse_double(fields[2], "obstacle radius")});
    }
    return obstacles;
}

inline std::string encode_phases(const std::vector<std::uint8_t>& phases) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(phases.size());
    for (const auto phase : phases) encoded.push_back(digits[phase & 0x0fU]);
    return encoded;
}

inline std::vector<std::uint8_t> decode_phases(const std::string& encoded) {
    std::vector<std::uint8_t> phases;
    phases.reserve(encoded.size());
    for (const char value : encoded) {
        if (value < '0' || value > '3') {
            throw std::invalid_argument("simulation event ledger: invalid RIS phase");
        }
        phases.push_back(static_cast<std::uint8_t>(value - '0'));
    }
    return phases;
}

inline std::string encode_bytes(const std::string& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        encoded.push_back(digits[value >> 4U]);
        encoded.push_back(digits[value & 0x0fU]);
    }
    return encoded;
}

inline std::string decode_bytes(const std::string& encoded) {
    if (encoded.size() % 2 != 0) {
        throw std::invalid_argument(
            "simulation event ledger: malformed byte string");
    }
    const auto nibble = [](char value) -> unsigned {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10U;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10U;
        throw std::invalid_argument(
            "simulation event ledger: malformed byte string");
    };
    std::string bytes;
    bytes.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        bytes.push_back(static_cast<char>(
            (nibble(encoded[index]) << 4U) | nibble(encoded[index + 1])));
    }
    return bytes;
}

} // namespace simulation_event_detail

// Identity over the descriptor fields intentionally carried by the V6
// decision trace. This remains stable after DecisionReplayLog serialization,
// unlike the richer in-memory generation fingerprint.
inline std::uint64_t simulation_descriptor_identity(
    const transport::GenerationDescriptor& descriptor) {
    std::ostringstream encoded;
    encoded << descriptor.generation_id << '|'
            << descriptor.created_at_ms << '|'
            << descriptor.expires_at_ms << '|'
            << descriptor.total_source_symbols;
    for (const auto& segment : descriptor.segments) {
        encoded << '|'
                << segment.segment_id << ','
                << segment.source_symbol_count << ','
                << static_cast<unsigned>(segment.importance) << ','
                << segment.expires_at_ms << ','
                << simulation_event_detail::double_hex(
                    segment.target_reliability) << ','
                << segment.coding.emitted_symbols;
    }
    return transport::fnv1a64(encoded.str());
}

inline DerivedSimulationEnvironment derive_simulation_environment(
    const SimulationEventSession& session,
    std::uint64_t random_before,
    double entropy_residual) {
    using namespace simulation_event_detail;
    DerivedSimulationEnvironment result;
    std::uint64_t state = random_before;
    result.ris_phases.reserve(session.ris_positions.size());
    for (const auto& tile : session.ris_positions) {
        const double angle =
            std::atan2(tile.y - session.source_position.y,
                       tile.x - session.source_position.x) +
            std::atan2(session.destination_position.y - tile.y,
                       session.destination_position.x - tile.x);
        const double jitter = entropy_residual * (uniform(state) - 0.5) * 0.9;
        const double phase = std::fmod(
            angle + jitter, 2.0 * 3.14159265358979323846);
        const int index = static_cast<int>(std::llround(
            phase / (3.14159265358979323846 / 2.0))) & 3;
        result.ris_phases.push_back(static_cast<std::uint8_t>(index));
    }
    result.random_after_ris = state;
    result.illumination = std::clamp((entropy_residual - 0.10) / 0.25, 0.0, 1.0);
    result.world_gain = world_gain(session, result.ris_phases);
    result.snr_rf_db = snr(result.world_gain, safety::LinkMode::RF,
                           result.illumination);
    result.snr_optical_db = snr(result.world_gain, safety::LinkMode::OPTICAL,
                                result.illumination);
    result.snr_backscatter_db = snr(
        result.world_gain, safety::LinkMode::BACKSCATTER, result.illumination);
    return result;
}

class SimulationEventLedger {
public:
    static constexpr std::string_view format_header =
        "AURORA_SIMULATION_EVENT_LEDGER_V6";

    void begin(SimulationEventSession session) {
        if (!records_.empty() || session_.initial_random_state != 0) {
            throw std::logic_error("simulation event ledger: session already started");
        }
        validate_session(session);
        session_ = std::move(session);
    }

    void record(const SimulationStepEvent& event) {
        ensure_started();
        validate_event(event);
        verify_generation_arrival(event);
        if (event.step != records_.size() ||
            event.simulated_now_ms != event.step * 1000ULL) {
            throw std::invalid_argument(
                "simulation event ledger: step/time is not contiguous");
        }
        if (records_.empty()) {
            if (event.random_before != session_.initial_random_state ||
                !simulation_event_detail::near(
                    event.source_energy_before_tick_j,
                    session_.source_initial_energy_j) ||
                !simulation_event_detail::near(
                    event.destination_energy_before_tick_j,
                    session_.destination_initial_energy_j) ||
                event.source_buffer_before != session_.initial_source_buffer +
                    event.arrived_source_packets ||
                event.destination_buffer_before != 0 ||
                event.destination_inbox_before != 0 ||
                event.decoder_rank_before != 0) {
                throw std::invalid_argument(
                    "simulation event ledger: initial state does not match session");
            }
        } else {
            const auto& previous = records_.back();
            if (event.random_before != previous.random_after_action ||
                !simulation_event_detail::near(
                    event.source_energy_before_tick_j,
                    previous.source_energy_after_action_j) ||
                !simulation_event_detail::near(
                    event.destination_energy_before_tick_j,
                    previous.destination_energy_after_action_j) ||
                event.source_buffer_before != previous.source_buffer_after_action +
                    event.arrived_source_packets ||
                event.destination_buffer_before !=
                    previous.destination_buffer_after_action ||
                event.destination_inbox_before !=
                    previous.destination_inbox_after_action) {
                throw std::invalid_argument(
                    "simulation event ledger: inter-step state is not contiguous");
            }
        }
        std::uint32_t expected_rank = 0;
        for (auto previous = records_.rbegin(); previous != records_.rend(); ++previous) {
            if (previous->active_generation_index == event.active_generation_index) {
                expected_rank = previous->decoder_rank_after;
                break;
            }
        }
        if (event.decoder_rank_before != expected_rank) {
            throw std::invalid_argument(
                "simulation event ledger: generation rank is not contiguous");
        }
        records_.push_back(event);
    }

    [[nodiscard]] const SimulationEventSession& session() const {
        return session_;
    }

    [[nodiscard]] const std::vector<SimulationStepEvent>& records() const {
        return records_;
    }

    [[nodiscard]] SimulationReplayVerification verify_structure() const {
        SimulationReplayVerification result;
        try {
            ensure_started();
            validate_session(session_);
            std::vector<std::uint32_t> ranks(session_.generations.size(), 0);
            std::vector<bool> terminal(session_.generations.size(), false);
            std::vector<std::optional<std::uint64_t>> last_served_at_ms(
                session_.generations.size());
            for (std::size_t index = 0; index < records_.size(); ++index) {
                const auto& event = records_[index];
                validate_event(event);
                verify_continuity(index);
                verify_generation_arrival(event);
                std::vector<simulation::GenerationSchedulingCandidate>
                    candidates;
                candidates.reserve(session_.generations.size());
                for (std::size_t generation = 0;
                     generation < session_.generations.size(); ++generation) {
                    const auto& identity = session_.generations[generation];
                    candidates.push_back({
                        generation,
                        identity.arrives_at_ms,
                        identity.expires_at_ms,
                        identity.scheduling_importance,
                        identity.arrives_at_ms <= event.simulated_now_ms,
                        terminal[generation],
                        last_served_at_ms[generation]});
                }
                auto selected = simulation::select_scheduled_generation(
                    candidates, event.simulated_now_ms,
                    session_.generation_scheduling_policy);
                std::size_t expected_active = session_.generations.size();
                if (selected) {
                    expected_active = *selected;
                } else {
                    for (std::size_t generation = session_.generations.size();
                         generation-- > 0;) {
                        if (session_.generations[generation].arrives_at_ms <=
                            event.simulated_now_ms) {
                            expected_active = generation;
                            break;
                        }
                    }
                }
                if (event.active_generation_index != expected_active) {
                    throw std::invalid_argument(
                        "active generation does not match aging/fairness schedule");
                }
                if (selected) {
                    last_served_at_ms[*selected] = event.simulated_now_ms;
                }
                if (event.decoder_rank_before !=
                    ranks[event.active_generation_index]) {
                    throw std::invalid_argument(
                        "generation rank is not contiguous");
                }
                ranks[event.active_generation_index] = event.decoder_rank_after;
                const auto status = static_cast<transport::DecodeStatus>(
                    event.decode_status);
                if (status == transport::DecodeStatus::COMPLETE ||
                    status == transport::DecodeStatus::EXPIRED) {
                    terminal[event.active_generation_index] = true;
                }
                verify_environment(event);
                verify_harvest(event);
                verify_contact(event);
                ++result.records_verified;
            }
        } catch (const std::exception& error) {
            result.ok = false;
            result.failure_reason = error.what();
        }
        return result;
    }

    [[nodiscard]] SimulationReplayVerification verify(
        const DecisionReplayLog& decisions) const {
        SimulationReplayVerification result;
        const auto structure = verify_structure();
        if (!structure.ok) {
            result.ok = false;
            result.failure_reason =
                "event ledger failed first: " + structure.failure_reason;
            return result;
        }
        const auto decision_verification = decisions.verify();
        if (!decision_verification.ok) {
            result.ok = false;
            result.failure_reason =
                "decision trace failed first: " + decision_verification.failure_reason;
            return result;
        }
        try {
            ensure_started();
            validate_session(session_);
            if (records_.size() != decisions.records().size()) {
                throw std::invalid_argument(
                    "record count does not match decision trace");
            }
            if (records_.empty()) return result;
            std::array<std::vector<bool>, 3> link_outcomes;
            using PacketIdentity =
                std::tuple<std::string, std::uint32_t, std::uint32_t>;
            std::set<PacketIdentity> destination_seen;
            std::vector<PacketIdentity> pending_inbox;

            for (std::size_t index = 0; index < records_.size(); ++index) {
                const auto& event = records_[index];
                const auto& decision = decisions.records()[index];
                verify_session_binding(event, decision);
                verify_decision_binding(event, decision, link_outcomes);
                verify_arrivals(event, decision, destination_seen, pending_inbox);
                verify_action_rng(event, decision);
                for (const auto& attempt : decision.trace.execution.attempts) {
                    link_outcomes[simulation_event_detail::link_index(
                        decision.trace.execution.link)].push_back(attempt.delivered);
                }
                ++result.records_verified;
            }
        } catch (const std::exception& error) {
            result.ok = false;
            result.failure_reason = error.what();
        }
        return result;
    }

    [[nodiscard]] std::string serialize() const {
        ensure_started();
        std::ostringstream output;
        output << format_header << '\n';
        const auto meta = encode_session(session_);
        std::uint64_t previous_checksum = transport::fnv1a64(meta);
        output << meta << '|' << simulation_event_detail::hex_u64(previous_checksum)
               << '\n';
        for (const auto& event : records_) {
            const auto payload = encode_event(event, previous_checksum);
            previous_checksum = transport::fnv1a64(payload);
            output << payload << '|'
                   << simulation_event_detail::hex_u64(previous_checksum) << '\n';
        }
        const auto footer = std::string("END|") +
            std::to_string(records_.size()) + '|' +
            simulation_event_detail::hex_u64(previous_checksum);
        output << footer << '|'
               << simulation_event_detail::hex_u64(transport::fnv1a64(footer))
               << '\n';
        return output.str();
    }

    void save(const std::string& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "simulation event ledger: cannot open output file: " + path);
        }
        const auto encoded = serialize();
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        if (!output) {
            throw std::runtime_error(
                "simulation event ledger: failed while writing: " + path);
        }
    }

    [[nodiscard]] static SimulationEventLedger load(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "simulation event ledger: cannot open input file: " + path);
        }
        std::ostringstream encoded;
        encoded << input.rdbuf();
        return deserialize(encoded.str());
    }

    [[nodiscard]] static SimulationEventLedger deserialize(
        const std::string& encoded) {
        using namespace simulation_event_detail;
        std::istringstream input(encoded);
        std::string line;
        if (!std::getline(input, line) || line != format_header) {
            throw std::invalid_argument(
                "simulation event ledger: unsupported or missing format header");
        }
        if (!std::getline(input, line)) {
            throw std::invalid_argument(
                "simulation event ledger: session metadata is missing");
        }
        const auto meta_separator = line.rfind('|');
        if (meta_separator == std::string::npos) {
            throw std::invalid_argument(
                "simulation event ledger: session checksum is missing");
        }
        const auto meta = line.substr(0, meta_separator);
        std::uint64_t previous_checksum = parse_u64(
            line.substr(meta_separator + 1), 16, "session checksum");
        if (transport::fnv1a64(meta) != previous_checksum) {
            throw std::invalid_argument(
                "simulation event ledger: session checksum mismatch");
        }
        SimulationEventLedger ledger;
        ledger.begin(decode_session(meta));
        bool footer_seen = false;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (footer_seen) {
                throw std::invalid_argument(
                    "simulation event ledger: data follows end marker");
            }
            const auto checksum_separator = line.rfind('|');
            if (checksum_separator == std::string::npos) {
                throw std::invalid_argument(
                    "simulation event ledger: record checksum is missing");
            }
            const auto payload = line.substr(0, checksum_separator);
            const auto checksum = parse_u64(
                line.substr(checksum_separator + 1), 16, "record checksum");
            if (transport::fnv1a64(payload) != checksum) {
                throw std::invalid_argument(
                    "simulation event ledger: record checksum mismatch");
            }
            if (payload.rfind("END|", 0) == 0) {
                const auto fields = split(payload, '|');
                if (fields.size() != 3 ||
                    parse_u64(fields[1], 10, "footer count") !=
                        ledger.records_.size() ||
                    parse_u64(fields[2], 16, "footer chain") !=
                        previous_checksum) {
                    throw std::invalid_argument(
                        "simulation event ledger: invalid end marker");
                }
                footer_seen = true;
                continue;
            }
            auto event = decode_event(payload, previous_checksum);
            ledger.record(event);
            previous_checksum = checksum;
        }
        if (!footer_seen) {
            throw std::invalid_argument(
                "simulation event ledger: end marker is missing");
        }
        return ledger;
    }

private:
    static void validate_session(const SimulationEventSession& session) {
        session.contact_schedule.validate();
        session.generation_arrival_schedule.validate();
        session.generation_scheduling_policy.validate();
        if (session.initial_random_state == 0 ||
            session.initial_source_buffer != 0 ||
            session.generation_scheduling_policy.service_quantum_ms != 1'000 ||
            session.generations.size() !=
                session.generation_arrival_schedule.arrivals().size() ||
            session.ris_positions.size() > 4096 ||
            session.obstacles.size() > 4096) {
            throw std::invalid_argument(
                "simulation event ledger: invalid session identity or bounds");
        }
        for (std::size_t index = 0; index < session.generations.size(); ++index) {
            const auto& identity = session.generations[index];
            const auto& arrival =
                session.generation_arrival_schedule.arrivals()[index];
            if (identity.arrives_at_ms != arrival.arrives_at_ms ||
                identity.tag != arrival.tag || identity.generation_id.empty() ||
                identity.generation_id.find('|') != std::string::npos ||
                identity.descriptor_fingerprint == 0 ||
                identity.required_rank == 0 ||
                identity.initial_source_packets == 0 ||
                static_cast<std::uint8_t>(identity.scheduling_importance) >
                    static_cast<std::uint8_t>(
                        transport::TransportImportance::ELASTIC) ||
                identity.expires_at_ms < identity.arrives_at_ms ||
                (arrival.deadline_ms != 0 &&
                 identity.expires_at_ms != identity.arrives_at_ms +
                    arrival.deadline_ms) ||
                (arrival.service_class !=
                    simulation::GenerationServiceClass::INHERIT &&
                 identity.scheduling_importance !=
                    simulation::resolve_service_class(
                        arrival.service_class,
                        transport::TransportImportance::IMPORTANT))) {
                throw std::invalid_argument(
                    "simulation event ledger: invalid generation identity");
            }
        }
        const auto valid_energy = [](double capacity, double energy, double harvest) {
            return std::isfinite(capacity) && capacity > 0.0 &&
                std::isfinite(energy) && energy >= 0.0 && energy <= capacity &&
                std::isfinite(harvest) && harvest >= 0.0;
        };
        if (!valid_energy(session.source_energy_capacity_j,
                          session.source_initial_energy_j,
                          session.source_harvest_w) ||
            !valid_energy(session.destination_energy_capacity_j,
                          session.destination_initial_energy_j,
                          session.destination_harvest_w)) {
            throw std::invalid_argument(
                "simulation event ledger: invalid session energy model");
        }
        const auto valid_point = [](const SimulationPoint& point) {
            return std::isfinite(point.x) && std::isfinite(point.y);
        };
        if (!valid_point(session.source_position) ||
            !valid_point(session.destination_position) ||
            std::any_of(session.ris_positions.begin(), session.ris_positions.end(),
                [&](const SimulationPoint& point) { return !valid_point(point); }) ||
            std::any_of(session.obstacles.begin(), session.obstacles.end(),
                [&](const SimulationObstacle& obstacle) {
                    return !valid_point(obstacle.centre) ||
                        !std::isfinite(obstacle.radius) || obstacle.radius <= 0.0;
                })) {
            throw std::invalid_argument(
                "simulation event ledger: invalid session topology");
        }
    }

    void ensure_started() const {
        if (session_.initial_random_state == 0) {
            throw std::logic_error(
                "simulation event ledger: session has not started");
        }
    }

    void validate_event(const SimulationStepEvent& event) const {
        const auto valid_energy = [](double value) {
            return std::isfinite(value) && value >= 0.0;
        };
        if (event.active_generation_index >= session_.generations.size() ||
            event.random_before == 0 || event.random_after_ris == 0 ||
            event.random_after_action == 0 ||
            !valid_energy(event.source_energy_before_tick_j) ||
            !valid_energy(event.source_energy_after_tick_j) ||
            !valid_energy(event.source_energy_after_action_j) ||
            !valid_energy(event.destination_energy_before_tick_j) ||
            !valid_energy(event.destination_energy_after_tick_j) ||
            !valid_energy(event.destination_energy_after_action_j) ||
            !std::isfinite(event.entropy_residual) ||
            event.entropy_residual < 0.0 || event.entropy_residual > 1.0 ||
            !std::isfinite(event.illumination) || event.illumination < 0.0 ||
            event.illumination > 1.0 || !std::isfinite(event.world_gain) ||
            event.world_gain <= 0.0 || !std::isfinite(event.snr_rf_db) ||
            !std::isfinite(event.snr_optical_db) ||
            !std::isfinite(event.snr_backscatter_db) ||
            event.ris_phases.size() != session_.ris_positions.size() ||
            std::any_of(event.ris_phases.begin(), event.ris_phases.end(),
                [](std::uint8_t phase) { return phase > 3U; }) ||
            event.decoder_rank_before >
                session_.generations[event.active_generation_index].required_rank ||
            event.decoder_rank_after >
                session_.generations[event.active_generation_index].required_rank ||
            event.decode_status > static_cast<std::uint8_t>(
                transport::DecodeStatus::INSUFFICIENT_RANK)) {
            throw std::invalid_argument(
                "simulation event ledger: invalid step event");
        }
    }

    void verify_continuity(std::size_t index) const {
        const auto& event = records_[index];
        if (event.step != index || event.simulated_now_ms != index * 1000ULL) {
            throw std::invalid_argument("step/time is not contiguous");
        }
        if (index == 0) {
            std::uint64_t seeded = session_.experiment_seed != 0
                ? session_.experiment_seed
                : 0xC0FFEEBEEFULL;
            for (std::size_t generation = 0;
                 generation < session_.generations.size(); ++generation) {
                simulation_event_detail::next_random(seeded);
            }
            if (event.random_before != session_.initial_random_state ||
                seeded != session_.initial_random_state) {
                throw std::invalid_argument(
                    "initial simulator RNG is not derived from the contract seed");
            }
            return;
        }
        const auto& previous = records_[index - 1];
        if (event.random_before != previous.random_after_action ||
            !simulation_event_detail::near(event.source_energy_before_tick_j,
                                           previous.source_energy_after_action_j) ||
            !simulation_event_detail::near(
                event.destination_energy_before_tick_j,
                previous.destination_energy_after_action_j) ||
            event.source_buffer_before != previous.source_buffer_after_action +
                event.arrived_source_packets ||
            event.destination_buffer_before !=
                previous.destination_buffer_after_action ||
            event.destination_inbox_before !=
                previous.destination_inbox_after_action) {
            throw std::invalid_argument("inter-step state is not contiguous");
        }
    }

    void verify_generation_arrival(const SimulationStepEvent& event) const {
        std::uint32_t expected_index = SimulationStepEvent::no_generation_arrival;
        std::uint64_t expected_packets = 0;
        for (std::size_t index = 0; index < session_.generations.size(); ++index) {
            const auto& generation = session_.generations[index];
            if (generation.arrives_at_ms == event.simulated_now_ms) {
                expected_index = static_cast<std::uint32_t>(index);
                expected_packets = generation.initial_source_packets;
                break;
            }
        }
        if (event.arrived_generation_index != expected_index ||
            event.arrived_source_packets != expected_packets) {
            throw std::invalid_argument(
                "generation arrival does not match embedded schedule");
        }
        if (session_.generations[event.active_generation_index].arrives_at_ms >
            event.simulated_now_ms) {
            throw std::invalid_argument(
                "active generation has not arrived yet");
        }
    }

    void verify_session_binding(const SimulationStepEvent& event,
                                const DecisionReplayRecord& record) const {
        const auto& identity =
            session_.generations[event.active_generation_index];
        const auto& arrival = session_.generation_arrival_schedule.arrivals()[
            event.active_generation_index];
        auto expected_importance = record.contract.importance;
        if (arrival.service_class !=
            simulation::GenerationServiceClass::INHERIT) {
            expected_importance = simulation::resolve_service_class(
                arrival.service_class, expected_importance);
        } else if (!record.descriptor.segments.empty()) {
            expected_importance = std::min_element(
                record.descriptor.segments.begin(),
                record.descriptor.segments.end(),
                [](const auto& left, const auto& right) {
                    return static_cast<std::uint8_t>(left.importance) <
                        static_cast<std::uint8_t>(right.importance);
                })->importance;
        }
        if (session_.experiment_seed != record.contract.experiment_seed ||
            identity.generation_id != record.descriptor.generation_id ||
            identity.descriptor_fingerprint !=
                simulation_descriptor_identity(record.descriptor) ||
            identity.required_rank != record.descriptor.total_source_symbols ||
            identity.arrives_at_ms != record.descriptor.created_at_ms ||
            identity.expires_at_ms != record.descriptor.expires_at_ms ||
            identity.scheduling_importance != expected_importance) {
            std::ostringstream reason;
            reason << "session does not match decision trace generation"
                   << " seed=" << session_.experiment_seed << '/'
                   << record.contract.experiment_seed
                   << " generation=" << identity.generation_id << '/'
                   << record.descriptor.generation_id
                   << " fingerprint="
                   << simulation_event_detail::hex_u64(
                        identity.descriptor_fingerprint) << '/'
                   << simulation_event_detail::hex_u64(
                        simulation_descriptor_identity(record.descriptor))
                   << " rank=" << identity.required_rank << '/'
                   << record.descriptor.total_source_symbols;
            throw std::invalid_argument(reason.str());
        }
        std::uint64_t emitted = 0;
        for (const auto& segment : record.descriptor.segments) {
            emitted += segment.coding.emitted_symbols;
        }
        if (identity.initial_source_packets != emitted) {
            throw std::invalid_argument(
                "scheduled generation arrival does not match descriptor emission");
        }
    }

    void verify_decision_binding(
        const SimulationStepEvent& event,
        const DecisionReplayRecord& record,
        const std::array<std::vector<bool>, 3>& link_outcomes) const {
        using simulation_event_detail::near;
        const auto& identity =
            session_.generations[event.active_generation_index];
        if (record.contract.experiment_seed != session_.experiment_seed ||
            simulation_descriptor_identity(record.descriptor) !=
                identity.descriptor_fingerprint ||
            record.trace.observed.now_ms != event.simulated_now_ms ||
            record.trace.observed.decoder_rank != event.decoder_rank_before ||
            record.trace.observed.rf_contact_available !=
                event.contact_available.rf ||
            record.trace.observed.optical_contact_available !=
                event.contact_available.optical ||
            record.trace.observed.backscatter_contact_available !=
                event.contact_available.backscatter ||
            !near(record.trace.observed.source_energy_reserve,
                  event.source_energy_after_tick_j /
                      session_.source_energy_capacity_j) ||
            !near(record.proposal.input.snr_db[0], event.snr_rf_db) ||
            !near(record.proposal.input.snr_db[1], event.snr_optical_db) ||
            !near(record.proposal.input.snr_db[2], event.snr_backscatter_db)) {
            throw std::invalid_argument(
                "step environment does not match decision context");
        }
        for (std::size_t link = 0; link < 3; ++link) {
            if (!near(record.proposal.input.historical_per[link],
                      simulation_event_detail::historical_per(
                          link_outcomes[link]))) {
                throw std::invalid_argument(
                    "channel history does not reconstruct proposal PER");
            }
        }
        if (event.source_buffer_after_action != event.source_buffer_before +
                record.trace.execution.repair_symbols_emitted) {
            throw std::invalid_argument(
                "source buffer does not reconstruct repair generation");
        }
        const double expected_source_after =
            record.trace.execution.attempts.empty()
            ? event.source_energy_after_tick_j
            : record.trace.execution.attempts.back().energy_after_j;
        if (!near(event.source_energy_after_action_j, expected_source_after) ||
            !near(event.destination_energy_after_action_j,
                  event.destination_energy_after_tick_j)) {
            throw std::invalid_argument(
                "action energy does not match decision execution");
        }
    }

    void verify_environment(const SimulationStepEvent& event) const {
        const auto expected = derive_simulation_environment(
            session_, event.random_before, event.entropy_residual);
        using simulation_event_detail::near;
        if (event.random_after_ris != expected.random_after_ris ||
            event.ris_phases != expected.ris_phases ||
            !near(event.illumination, expected.illumination) ||
            !near(event.world_gain, expected.world_gain) ||
            !near(event.snr_rf_db, expected.snr_rf_db) ||
            !near(event.snr_optical_db, expected.snr_optical_db) ||
            !near(event.snr_backscatter_db, expected.snr_backscatter_db)) {
            throw std::invalid_argument(
                "RIS/world transition is not reproducible from RNG and topology");
        }
    }

    void verify_harvest(const SimulationStepEvent& event) const {
        const double source_after = std::min(
            session_.source_energy_capacity_j,
            event.source_energy_before_tick_j +
                session_.source_harvest_w * 1.0 * 0.4);
        const double destination_after = std::min(
            session_.destination_energy_capacity_j,
            event.destination_energy_before_tick_j +
                session_.destination_harvest_w * 1.0 * 0.4);
        if (!simulation_event_detail::near(
                event.source_energy_after_tick_j, source_after) ||
            !simulation_event_detail::near(
                event.destination_energy_after_tick_j, destination_after)) {
            throw std::invalid_argument(
                "harvesting transition does not match the recorded model");
        }
    }

    void verify_contact(const SimulationStepEvent& event) const {
        if (!(event.contact_available ==
              session_.contact_schedule.availability_at(
                  event.simulated_now_ms))) {
            throw std::invalid_argument(
                "contact availability does not match the declared schedule");
        }
    }

    void verify_arrivals(
        const SimulationStepEvent& event,
        const DecisionReplayRecord& decision,
        std::set<std::tuple<std::string, std::uint32_t, std::uint32_t>>& seen,
        std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>>&
            inbox) const {
        if (event.destination_inbox_before != inbox.size()) {
            throw std::invalid_argument("destination inbox continuity mismatch");
        }
        std::uint64_t expected_buffer = event.destination_buffer_before;
        for (const auto& packet : inbox) {
            if (seen.insert(packet).second) ++expected_buffer;
        }
        inbox.clear();
        if (event.destination_buffer_after_ingest != expected_buffer ||
            event.destination_inbox_after_ingest != 0 ||
            event.destination_buffer_after_action != expected_buffer) {
            throw std::invalid_argument(
                "destination ingest transition is not reproducible");
        }
        for (const auto& attempt : decision.trace.execution.attempts) {
            if (attempt.delivered) {
                const auto packet = std::make_tuple(
                    decision.descriptor.generation_id,
                    attempt.segment_id, attempt.symbol_seed);
                if (!seen.contains(packet)) inbox.push_back(packet);
            }
        }
        if (event.destination_inbox_after_action != inbox.size()) {
            throw std::invalid_argument(
                "delivered packet arrivals do not match destination inbox");
        }
    }

    void verify_action_rng(const SimulationStepEvent& event,
                           const DecisionReplayRecord& decision) const {
        using namespace simulation_event_detail;
        std::uint64_t state = event.random_after_ris;
        const auto link = decision.trace.execution.link;
        for (const auto& attempt : decision.trace.execution.attempts) {
            if (attempt.hal_evaluated && link == safety::LinkMode::RF) {
                const int first = -110 + static_cast<int>(uniform(state) * 20.0);
                if (!attempt.lbt_evaluated || first != attempt.lbt_first_rssi_dbm) {
                    throw std::invalid_argument(
                        "LBT first sample does not match simulator RNG");
                }
                const bool second_valid = first < attempt.lbt_threshold_dbm;
                if (second_valid != attempt.lbt_second_valid) {
                    throw std::invalid_argument(
                        "LBT dwell transition does not match first sample");
                }
                if (second_valid) {
                    const int second =
                        -110 + static_cast<int>(uniform(state) * 20.0);
                    if (second != attempt.lbt_second_rssi_dbm) {
                        throw std::invalid_argument(
                            "LBT second sample does not match simulator RNG");
                    }
                }
            }
            if (attempt.channel_evaluated) {
                const double u1 = std::max(1e-12, uniform(state));
                const double u2 = std::max(1e-12, uniform(state));
                const double fading =
                    std::sqrt(-2.0 * std::log(u1)) *
                    std::cos(2.0 * 3.14159265358979323846 * u2) * 3.5 - 5.0;
                if (!near(fading, attempt.channel_fading_db)) {
                    throw std::invalid_argument(
                        "channel fading does not match simulator RNG");
                }
            }
            if (decision.proposal.decision.jitter_ms > 0) {
                (void)uniform(state);
            }
        }
        if (state != event.random_after_action) {
            throw std::invalid_argument(
                "action RNG checkpoint does not match LBT/fading/jitter events");
        }
    }

    static std::string encode_generation_identities(
        const std::vector<SimulationGenerationIdentity>& generations) {
        std::ostringstream output;
        for (std::size_t index = 0; index < generations.size(); ++index) {
            if (index > 0) output << ';';
            const auto& generation = generations[index];
            output << generation.arrives_at_ms << ',' << generation.tag << ','
                   << generation.generation_id << ','
                   << simulation_event_detail::hex_u64(
                        generation.descriptor_fingerprint) << ','
                   << generation.required_rank << ','
                   << generation.initial_source_packets << ','
                   << static_cast<unsigned>(
                        generation.scheduling_importance) << ','
                   << generation.expires_at_ms;
        }
        return output.str();
    }

    static std::vector<SimulationGenerationIdentity>
    decode_generation_identities(const std::string& encoded) {
        using namespace simulation_event_detail;
        std::vector<SimulationGenerationIdentity> generations;
        if (encoded.empty()) return generations;
        for (const auto& item : split(encoded, ';')) {
            const auto fields = split(item, ',');
            if (fields.size() != 8) {
                throw std::invalid_argument(
                    "simulation event ledger: invalid generation identities");
            }
            const auto required_rank = parse_u64(
                fields[4], 10, "generation rank");
            if (required_rank > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument(
                    "simulation event ledger: generation rank is out of range");
            }
            const auto importance = parse_u64(
                fields[6], 10, "generation scheduling importance");
            if (importance > static_cast<std::uint8_t>(
                    transport::TransportImportance::ELASTIC)) {
                throw std::invalid_argument(
                    "simulation event ledger: generation importance is out of range");
            }
            generations.push_back({
                parse_u64(fields[0], 10, "generation arrival time"),
                fields[1], fields[2],
                parse_u64(fields[3], 16, "generation fingerprint"),
                static_cast<std::uint32_t>(required_rank),
                parse_u64(fields[5], 10, "generation packet count"),
                static_cast<transport::TransportImportance>(importance),
                parse_u64(fields[7], 10, "generation expiry")});
        }
        return generations;
    }

    static std::string encode_session(const SimulationEventSession& session) {
        using namespace simulation_event_detail;
        std::ostringstream output;
        output << "SESSION|" << hex_u64(session.experiment_seed) << '|'
               << hex_u64(session.initial_random_state) << '|'
               << session.initial_source_buffer << '|'
               << double_hex(session.source_energy_capacity_j) << '|'
               << double_hex(session.source_initial_energy_j) << '|'
               << double_hex(session.source_harvest_w) << '|'
               << double_hex(session.destination_energy_capacity_j) << '|'
               << double_hex(session.destination_initial_energy_j) << '|'
               << double_hex(session.destination_harvest_w) << '|'
               << double_hex(session.source_position.x) << '|'
               << double_hex(session.source_position.y) << '|'
               << double_hex(session.destination_position.x) << '|'
               << double_hex(session.destination_position.y) << '|'
               << encode_points(session.ris_positions) << '|'
               << encode_obstacles(session.obstacles) << '|'
               << encode_bytes(session.contact_schedule.serialize()) << '|'
               << encode_bytes(
                    session.generation_arrival_schedule.serialize()) << '|'
               << session.generation_scheduling_policy.service_quantum_ms << '|'
               << session.generation_scheduling_policy.aging_interval_ms << '|'
               << session.generation_scheduling_policy.starvation_limit_ms << '|'
               << static_cast<unsigned>(
                    session.generation_scheduling_policy.discipline) << '|'
               << encode_bytes(encode_generation_identities(
                    session.generations));
        return output.str();
    }

    static SimulationEventSession decode_session(const std::string& encoded) {
        using namespace simulation_event_detail;
        const auto fields = split(encoded, '|');
        if (fields.size() != 23 || fields[0] != "SESSION") {
            throw std::invalid_argument(
                "simulation event ledger: invalid session metadata");
        }
        SimulationEventSession session;
        std::size_t cursor = 1;
        session.experiment_seed = parse_u64(fields[cursor++], 16, "seed");
        session.initial_random_state = parse_u64(
            fields[cursor++], 16, "initial RNG");
        session.initial_source_buffer = parse_u64(
            fields[cursor++], 10, "initial source buffer");
        session.source_energy_capacity_j = parse_double(fields[cursor++], "source capacity");
        session.source_initial_energy_j = parse_double(fields[cursor++], "source energy");
        session.source_harvest_w = parse_double(fields[cursor++], "source harvest");
        session.destination_energy_capacity_j = parse_double(fields[cursor++], "destination capacity");
        session.destination_initial_energy_j = parse_double(fields[cursor++], "destination energy");
        session.destination_harvest_w = parse_double(fields[cursor++], "destination harvest");
        session.source_position.x = parse_double(fields[cursor++], "source x");
        session.source_position.y = parse_double(fields[cursor++], "source y");
        session.destination_position.x = parse_double(fields[cursor++], "destination x");
        session.destination_position.y = parse_double(fields[cursor++], "destination y");
        session.ris_positions = decode_points(fields[cursor++]);
        session.obstacles = decode_obstacles(fields[cursor++]);
        session.contact_schedule = simulation::ContactSchedule::deserialize(
            decode_bytes(fields[cursor++]));
        session.generation_arrival_schedule =
            simulation::GenerationArrivalSchedule::deserialize(
                decode_bytes(fields[cursor++]));
        session.generation_scheduling_policy.service_quantum_ms = parse_u64(
            fields[cursor++], 10, "scheduler service quantum");
        session.generation_scheduling_policy.aging_interval_ms = parse_u64(
            fields[cursor++], 10, "scheduler aging interval");
        session.generation_scheduling_policy.starvation_limit_ms = parse_u64(
            fields[cursor++], 10, "scheduler starvation limit");
        const auto discipline = parse_u64(
            fields[cursor++], 10, "scheduler discipline");
        if (discipline > static_cast<std::uint8_t>(
                simulation::GenerationSchedulingDiscipline::AGING_FAIR)) {
            throw std::invalid_argument(
                "simulation event ledger: invalid scheduler discipline");
        }
        session.generation_scheduling_policy.discipline =
            static_cast<simulation::GenerationSchedulingDiscipline>(
                discipline);
        session.generations = decode_generation_identities(
            decode_bytes(fields[cursor++]));
        return session;
    }

    static std::string encode_event(const SimulationStepEvent& event,
                                    std::uint64_t previous_checksum) {
        using namespace simulation_event_detail;
        std::ostringstream output;
        output << "STEP|" << event.step << '|'
               << hex_u64(previous_checksum) << '|'
               << event.simulated_now_ms << '|'
               << event.active_generation_index << '|'
               << event.arrived_generation_index << '|'
               << event.arrived_source_packets << '|'
               << hex_u64(event.random_before) << '|'
               << hex_u64(event.random_after_ris) << '|'
               << hex_u64(event.random_after_action) << '|'
               << double_hex(event.source_energy_before_tick_j) << '|'
               << double_hex(event.source_energy_after_tick_j) << '|'
               << double_hex(event.source_energy_after_action_j) << '|'
               << double_hex(event.destination_energy_before_tick_j) << '|'
               << double_hex(event.destination_energy_after_tick_j) << '|'
               << double_hex(event.destination_energy_after_action_j) << '|'
               << event.source_buffer_before << '|'
               << event.source_buffer_after_action << '|'
               << event.destination_buffer_before << '|'
               << event.destination_inbox_before << '|'
               << event.destination_buffer_after_ingest << '|'
               << event.destination_inbox_after_ingest << '|'
               << event.destination_buffer_after_action << '|'
               << event.destination_inbox_after_action << '|'
               << event.decoder_rank_before << '|'
               << event.decoder_rank_after << '|'
               << static_cast<unsigned>(event.decode_status) << '|'
               << double_hex(event.entropy_residual) << '|'
               << double_hex(event.illumination) << '|'
               << double_hex(event.world_gain) << '|'
               << double_hex(event.snr_rf_db) << '|'
               << double_hex(event.snr_optical_db) << '|'
               << double_hex(event.snr_backscatter_db) << '|'
               << encode_phases(event.ris_phases) << '|'
               << static_cast<unsigned>(event.contact_available.mask());
        return output.str();
    }

    static SimulationStepEvent decode_event(const std::string& encoded,
                                            std::uint64_t previous_checksum) {
        using namespace simulation_event_detail;
        const auto fields = split(encoded, '|');
        if (fields.size() != 35 || fields[0] != "STEP") {
            throw std::invalid_argument(
                "simulation event ledger: invalid step record");
        }
        std::size_t cursor = 1;
        SimulationStepEvent event;
        event.step = parse_u64(fields[cursor++], 10, "step");
        if (parse_u64(fields[cursor++], 16, "previous checksum") !=
            previous_checksum) {
            throw std::invalid_argument(
                "simulation event ledger: checksum chain mismatch");
        }
        event.simulated_now_ms = parse_u64(fields[cursor++], 10, "time");
        const auto active_generation_index = parse_u64(
            fields[cursor++], 10, "active generation index");
        const auto arrived_generation_index = parse_u64(
            fields[cursor++], 10, "arrived generation index");
        if (active_generation_index >
                std::numeric_limits<std::uint32_t>::max() ||
            arrived_generation_index >
                std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument(
                "simulation event ledger: generation index is out of range");
        }
        event.active_generation_index = static_cast<std::uint32_t>(
            active_generation_index);
        event.arrived_generation_index = static_cast<std::uint32_t>(
            arrived_generation_index);
        event.arrived_source_packets = parse_u64(
            fields[cursor++], 10, "arrived source packets");
        event.random_before = parse_u64(fields[cursor++], 16, "RNG before");
        event.random_after_ris = parse_u64(fields[cursor++], 16, "RNG after RIS");
        event.random_after_action = parse_u64(fields[cursor++], 16, "RNG after action");
        event.source_energy_before_tick_j = parse_double(fields[cursor++], "source energy before tick");
        event.source_energy_after_tick_j = parse_double(fields[cursor++], "source energy after tick");
        event.source_energy_after_action_j = parse_double(fields[cursor++], "source energy after action");
        event.destination_energy_before_tick_j = parse_double(fields[cursor++], "destination energy before tick");
        event.destination_energy_after_tick_j = parse_double(fields[cursor++], "destination energy after tick");
        event.destination_energy_after_action_j = parse_double(fields[cursor++], "destination energy after action");
        event.source_buffer_before = parse_u64(fields[cursor++], 10, "source buffer before");
        event.source_buffer_after_action = parse_u64(fields[cursor++], 10, "source buffer after");
        event.destination_buffer_before = parse_u64(fields[cursor++], 10, "destination buffer before");
        event.destination_inbox_before = parse_u64(fields[cursor++], 10, "destination inbox before");
        event.destination_buffer_after_ingest = parse_u64(fields[cursor++], 10, "destination buffer after ingest");
        event.destination_inbox_after_ingest = parse_u64(fields[cursor++], 10, "destination inbox after ingest");
        event.destination_buffer_after_action = parse_u64(fields[cursor++], 10, "destination buffer after action");
        event.destination_inbox_after_action = parse_u64(fields[cursor++], 10, "destination inbox after action");
        event.decoder_rank_before = static_cast<std::uint32_t>(parse_u64(fields[cursor++], 10, "rank before"));
        event.decoder_rank_after = static_cast<std::uint32_t>(parse_u64(fields[cursor++], 10, "rank after"));
        event.decode_status = static_cast<std::uint8_t>(parse_u64(fields[cursor++], 10, "decode status"));
        event.entropy_residual = parse_double(fields[cursor++], "entropy residual");
        event.illumination = parse_double(fields[cursor++], "illumination");
        event.world_gain = parse_double(fields[cursor++], "world gain");
        event.snr_rf_db = parse_double(fields[cursor++], "RF SNR");
        event.snr_optical_db = parse_double(fields[cursor++], "optical SNR");
        event.snr_backscatter_db = parse_double(fields[cursor++], "backscatter SNR");
        event.ris_phases = decode_phases(fields[cursor++]);
        const auto contact_mask = parse_u64(
            fields[cursor++], 10, "contact availability");
        if (contact_mask > 0x7U) {
            throw std::invalid_argument(
                "simulation event ledger: invalid contact availability");
        }
        event.contact_available = simulation::ContactAvailability::from_mask(
            static_cast<std::uint8_t>(contact_mask));
        return event;
    }

    SimulationEventSession session_;
    std::vector<SimulationStepEvent> records_;
};

} // namespace aurora::telemetry

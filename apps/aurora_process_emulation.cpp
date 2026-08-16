#include "aurora/control/TransportPolicy.hpp"
#include "aurora/emulation/ImpairmentTrace.hpp"
#include "aurora/emulation/Measurement.hpp"
#include "aurora/emulation/ProcessAuthentication.hpp"
#include "aurora/emulation/ProcessProtocol.hpp"
#include "aurora/emulation/ProcessWorkload.hpp"
#include "aurora/fec/GenerationCodec.hpp"
#include "aurora/transport/GenerationManager.hpp"
#include "aurora/transport/GenerationReceiver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint32_t terminal_ack_copy_count = 3;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
#endif

class SocketRuntime {
public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("process emulation: WSAStartup failed");
        }
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

class UdpSocket {
public:
    UdpSocket(std::string_view bind_host, std::uint16_t bind_port) {
        in_addr parsed_address{};
        if (::inet_pton(AF_INET, std::string(bind_host).c_str(),
                        &parsed_address) != 1) {
            throw std::invalid_argument(
                "process emulation: bind host must be an IPv4 literal");
        }
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) fail("socket creation");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(bind_port);
        address.sin_addr = parsed_address;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) != 0) {
            fail("socket bind");
        }
        set_timeout(250);
    }

    ~UdpSocket() {
        if (socket_ == invalid_socket) return;
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
    }

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void send(const std::vector<std::uint8_t>& bytes,
              std::string_view destination_host,
              std::uint16_t destination_port) const {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(destination_port);
        if (::inet_pton(AF_INET, std::string(destination_host).c_str(),
                        &destination.sin_addr) != 1) {
            throw std::invalid_argument(
                "process emulation: destination host must be an IPv4 literal");
        }
#ifdef _WIN32
        const auto sent = ::sendto(
            socket_, reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent == SOCKET_ERROR || sent != static_cast<int>(bytes.size())) {
            fail("socket send");
        }
#else
        const auto sent = ::sendto(
            socket_, bytes.data(), bytes.size(), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
            fail("socket send");
        }
#endif
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> receive() const {
        std::vector<std::uint8_t> bytes(
            aurora::emulation::maximum_datagram_bytes + 128);
#ifdef _WIN32
        const auto received = ::recvfrom(
            socket_, reinterpret_cast<char*>(bytes.data()),
            static_cast<int>(bytes.size()), 0, nullptr, nullptr);
        if (received == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                return std::nullopt;
            }
            fail("socket receive");
        }
#else
        const auto received = ::recvfrom(
            socket_, bytes.data(), bytes.size(), 0, nullptr, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return std::nullopt;
            fail("socket receive");
        }
#endif
        bytes.resize(static_cast<std::size_t>(received));
        return bytes;
    }

    void receive_timeout(int milliseconds) const {
        if (milliseconds <= 0) {
            throw std::invalid_argument(
                "process emulation: receive timeout must be positive");
        }
        set_timeout(milliseconds);
    }

private:
    void set_timeout(int milliseconds) const {
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(milliseconds);
        if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout)) != 0) {
            fail("socket timeout configuration");
        }
#else
        timeval timeout{};
        timeout.tv_sec = milliseconds / 1000;
        timeout.tv_usec = (milliseconds % 1000) * 1000;
        if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) != 0) {
            fail("socket timeout configuration");
        }
#endif
    }

    [[noreturn]] static void fail(const char* operation) {
#ifdef _WIN32
        throw std::runtime_error(
            std::string("process emulation: ") + operation +
            " failed with WSA error " + std::to_string(WSAGetLastError()));
#else
        throw std::runtime_error(
            std::string("process emulation: ") + operation +
            " failed with errno " + std::to_string(errno));
#endif
    }

    NativeSocket socket_ = invalid_socket;
};

std::uint16_t parse_port(const char* value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed, 10);
    if (value[consumed] != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("process emulation: invalid UDP port");
    }
    return static_cast<std::uint16_t>(parsed);
}

std::size_t parse_generation_count(const char* value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed, 10);
    if (value[consumed] != '\0' || parsed == 0 || parsed > 1024) {
        throw std::invalid_argument(
            "process emulation: invalid generation count");
    }
    return static_cast<std::size_t>(parsed);
}

std::size_t parse_generation_index(const char* value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed, 10);
    if (value[consumed] != '\0' || parsed > 1023) {
        throw std::invalid_argument(
            "process emulation: invalid generation index");
    }
    return static_cast<std::size_t>(parsed);
}

std::uint64_t parse_timeout_ms(const char* value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed, 10);
    if (value[consumed] != '\0' || parsed == 0 || parsed > 600'000) {
        throw std::invalid_argument(
            "process emulation: timeout must be 1..600000 milliseconds");
    }
    return parsed;
}

std::uint64_t elapsed_ms(
    const std::chrono::steady_clock::time_point& started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

std::optional<aurora::emulation::FeedbackFrame> receive_feedback(
    const UdpSocket& socket,
    const aurora::emulation::ProcessAuthenticator& authenticator,
    aurora::emulation::ReplayWindow& replay_window,
    std::uint32_t& authentication_rejections,
    std::uint32_t& replay_rejections) {
    const auto datagram = socket.receive();
    if (!datagram) return std::nullopt;
    try {
        auto opened = authenticator.open(
            aurora::emulation::ProcessDirection::REVERSE, *datagram);
        if (!replay_window.accept(opened.sequence)) {
            ++replay_rejections;
            return std::nullopt;
        }
        return aurora::emulation::decode_feedback(opened.payload);
    } catch (const std::invalid_argument&) {
        ++authentication_rejections;
        return std::nullopt;
    }
}

int run_sender(std::string_view forward_host, std::uint16_t forward_port,
               std::string_view feedback_bind_host,
               std::uint16_t feedback_port,
               const std::string& trace_path,
               const std::string& key_path,
               std::uint64_t session_id,
               std::string_view policy_id,
               std::string_view workload_id) {
    const auto started = std::chrono::steady_clock::now();
    UdpSocket feedback_socket(feedback_bind_host, feedback_port);
    auto codec = std::make_shared<aurora::fec::ExperimentalLtLikeCodec>();
    auto policy = aurora::control::make_transport_policy(policy_id);
    const auto workload = aurora::emulation::process_workload(workload_id);
    aurora::transport::GenerationManager manager(codec, policy);
    const auto contract = aurora::transport::TransportContract::parse(
        std::string(workload.contract));
    const auto profile = manager.build_profile(contract);
    const auto trace = aurora::emulation::ImpairmentTrace::load(trace_path);
    const aurora::emulation::ProcessAuthenticator authenticator(
        aurora::emulation::ProcessAuthenticator::load_key(key_path),
        session_id);
    aurora::emulation::ReplayWindow reverse_replay_window;
    struct SenderGeneration {
        aurora::transport::GenerationSpawnResult spawned;
        std::optional<aurora::control::BiologicalFlowState>
            adaptive_state_at_plan;
        std::optional<aurora::emulation::FeedbackFrame> feedback;
        bool policy_feedback_applied = false;
        std::uint64_t repair_symbols_requested = 0;
        std::uint64_t repair_symbols_emitted = 0;
    };
    std::vector<SenderGeneration> generations;

    UdpSocket forward_socket("0.0.0.0", 0);
    std::uint32_t forward_datagrams = 0;
    std::uint32_t descriptor_datagrams = 0;
    std::uint32_t wire_symbol_datagrams = 0;
    std::uint32_t terminal_ack_datagrams = 0;
    std::uint32_t feedback_datagrams = 0;
    std::uint32_t stale_feedback_datagrams = 0;
    std::uint32_t authentication_rejections = 0;
    std::uint32_t replay_rejections = 0;
    std::uint64_t forward_auth_sequence = 0;
    std::uint64_t impairment_attempt = 0;
    std::uint32_t dropped = 0;
    std::uint32_t duplicated = 0;
    std::uint32_t delayed = 0;
    std::uint32_t reordered = 0;
    std::uint32_t unknown_feedback_echoes = 0;
    std::uint64_t feedback_applied = 0;
    std::uint64_t total_source_symbols = 0;
    std::uint64_t total_initial_symbols = 0;
    std::uint64_t total_repair_requested = 0;
    std::uint64_t total_repair_emitted = 0;
    std::uint64_t delivered_generations = 0;
    std::uint64_t critical_generations = 0;
    std::uint64_t critical_delivered_before_deadline = 0;
    aurora::emulation::FeedbackRttTracker feedback_rtt;

    auto accept_feedback = [&](aurora::emulation::FeedbackFrame incoming) {
        for (auto& generation : generations) {
            const auto& descriptor = generation.spawned.descriptor;
            if (incoming.report.generation_id == descriptor.generation_id &&
                incoming.descriptor_fingerprint ==
                    descriptor.descriptor_fingerprint &&
                incoming.report.source_bytes ==
                    descriptor.original_payload_length &&
                incoming.report.required_rank == descriptor.total_source_symbols) {
                if (generation.feedback) {
                    const auto& current = generation.feedback->report;
                    if (current.delivered() || current.terminal_failure() ||
                        incoming.report.decoder_rank < current.decoder_rank ||
                        (incoming.report.decoder_rank == current.decoder_rank &&
                         incoming.report.symbols_observed <
                             current.symbols_observed)) {
                        ++stale_feedback_datagrams;
                        return;
                    }
                }
                const auto observation = feedback_rtt.observe(
                    incoming.echoed_forward_sequence,
                    incoming.report.delivered() ||
                        incoming.report.terminal_failure());
                if (observation == aurora::emulation::
                        FeedbackRttObservation::UNKNOWN_SEQUENCE) {
                    ++unknown_feedback_echoes;
                    return;
                }
                generation.feedback = std::move(incoming);
                if (!generation.policy_feedback_applied &&
                    (generation.feedback->report.delivered() ||
                     generation.feedback->report.terminal_failure())) {
                    policy->observe(profile, generation.feedback->report);
                    generation.policy_feedback_applied = true;
                    ++feedback_applied;
                }
                ++feedback_datagrams;
                return;
            }
        }
    };
    auto poll_feedback = [&] {
        if (auto incoming = receive_feedback(
                feedback_socket, authenticator, reverse_replay_window,
                authentication_rejections, replay_rejections)) {
            accept_feedback(std::move(*incoming));
        }
    };

    struct PendingDatagram {
        std::chrono::steady_clock::time_point release;
        std::uint64_t attempt = 0;
        std::uint64_t order = 0;
        std::uint64_t sequence = 0;
        std::vector<std::uint8_t> frame;
    };
    std::vector<PendingDatagram> pending;
    std::uint64_t enqueue_order = 0;
    std::uint64_t highest_sent_attempt = 0;
    bool sent_any = false;
    auto release_epoch = std::chrono::steady_clock::now();

    auto flush_pending = [&](bool flush_all) {
        while (!pending.empty()) {
            const auto next = std::min_element(
                pending.begin(), pending.end(),
                [](const auto& left, const auto& right) {
                    if (left.release != right.release) {
                        return left.release < right.release;
                    }
                    return left.order < right.order;
                });
            const auto now = std::chrono::steady_clock::now();
            if (next->release > now) {
                if (!flush_all) return;
                std::this_thread::sleep_until(next->release);
            }
            if (sent_any && next->attempt < highest_sent_attempt) ++reordered;
            highest_sent_attempt = std::max(
                highest_sent_attempt, next->attempt);
            sent_any = true;
            feedback_rtt.record_sent(next->sequence);
            forward_socket.send(
                next->frame, forward_host, forward_port);
            ++forward_datagrams;
            ++wire_symbol_datagrams;
            pending.erase(next);
            poll_feedback();
        }
    };

    auto send_symbol = [&](const ::fec::Pkt& packet) {
        const auto sequence = forward_auth_sequence++;
        const auto frame = authenticator.seal(
            aurora::emulation::ProcessDirection::FORWARD,
            sequence,
            aurora::emulation::encode_symbol(packet));
        const auto attempt = impairment_attempt++;
        const auto directive = trace.directive(attempt);
        if (directive.action == aurora::emulation::ImpairmentAction::DROP) {
            ++dropped;
            return;
        }
        if (directive.delay_ms > 0) ++delayed;
        const auto release = release_epoch +
            std::chrono::milliseconds(directive.delay_ms);
        pending.push_back(
            {release, attempt, enqueue_order++, sequence, frame});
        if (directive.action ==
            aurora::emulation::ImpairmentAction::DUPLICATE) {
            pending.push_back(
                {release, attempt, enqueue_order++, sequence, frame});
            ++duplicated;
        }
    };

    auto acknowledge_terminal = [&](const SenderGeneration& generation) {
        const auto& descriptor = generation.spawned.descriptor;
        const auto acknowledgement = aurora::emulation::encode_terminal_ack({
            descriptor.descriptor_fingerprint,
            descriptor.generation_id});
        for (std::uint32_t copy = 0; copy < terminal_ack_copy_count; ++copy) {
            const auto sequence = forward_auth_sequence++;
            const auto frame = authenticator.seal(
                aurora::emulation::ProcessDirection::FORWARD,
                sequence, acknowledgement);
            forward_socket.send(frame, forward_host, forward_port);
            ++forward_datagrams;
            ++terminal_ack_datagrams;
        }
    };

    for (std::size_t generation_index = 0;
         generation_index < workload.generation_count; ++generation_index) {
        std::vector<std::uint8_t> payload(
            workload.payload_bytes(generation_index));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>(
                (i * (41 + generation_index * 2) + 13 + generation_index) &
                0xffU);
        }
        SenderGeneration state;
        state.spawned = manager.spawn(
            contract,
            "process-emulation-" + std::to_string(generation_index),
            payload, workload.symbol_size, elapsed_ms(started));
        if (const auto adaptive = std::dynamic_pointer_cast<
                aurora::control::BiologicalAdaptivePolicy>(policy)) {
            state.adaptive_state_at_plan = adaptive->flow_state(profile);
        }
        generations.push_back(std::move(state));
        auto& generation = generations.back();
        const auto& descriptor = generation.spawned.descriptor;
        total_source_symbols += descriptor.total_source_symbols;
        total_initial_symbols += generation.spawned.packets.size();
        const bool has_critical = std::any_of(
            descriptor.segments.begin(), descriptor.segments.end(),
            [](const auto& segment) {
                return segment.importance ==
                    aurora::transport::TransportImportance::CRITICAL;
            });
        if (has_critical) ++critical_generations;

        feedback_socket.receive_timeout(10);
        for (int attempt = 0; attempt < 20 && !generation.feedback;
             ++attempt) {
            const auto encoded = aurora::emulation::encode_descriptor(
                descriptor);
            const auto sequence = forward_auth_sequence++;
            const auto frame = authenticator.seal(
                aurora::emulation::ProcessDirection::FORWARD,
                sequence, encoded);
            feedback_rtt.record_sent(sequence);
            forward_socket.send(frame, forward_host, forward_port);
            ++forward_datagrams;
            ++descriptor_datagrams;
            for (int poll = 0; poll < 32 && !generation.feedback; ++poll) {
                poll_feedback();
            }
        }
        if (!generation.feedback) {
            throw std::runtime_error(
                "process emulation: receiver did not acknowledge descriptor");
        }
        // Descriptor establishment may wait. Symbol service uses short polls
        // so reverse reports remain asynchronous and multiple can coexist.
        feedback_socket.receive_timeout(2);

        release_epoch = std::chrono::steady_clock::now();
        for (const auto& packet : generation.spawned.packets) {
            if (generation.feedback->report.delivered() ||
                generation.feedback->report.terminal_failure()) {
                break;
            }
            send_symbol(packet);
        }
        flush_pending(true);

        const int maximum_repair_rounds = workload.id == "policy-pilot-v1"
            ? 2048 : 256;
        for (int round = 0; round < maximum_repair_rounds; ++round) {
            if (generation.feedback->report.delivered() ||
                generation.feedback->report.terminal_failure()) {
                break;
            }
            release_epoch = std::chrono::steady_clock::now();
            ++generation.repair_symbols_requested;
            const auto emitted = manager.emit_repairs(
                descriptor.generation_id, 1, false, elapsed_ms(started));
            generation.repair_symbols_emitted += emitted.emitted_symbols;
            if (!emitted.packets.empty()) {
                send_symbol(emitted.packets.front());
                flush_pending(true);
            } else {
                const auto sequence = forward_auth_sequence++;
                const auto frame = authenticator.seal(
                    aurora::emulation::ProcessDirection::FORWARD,
                    sequence,
                    aurora::emulation::encode_descriptor(descriptor));
                feedback_rtt.record_sent(sequence);
                forward_socket.send(frame, forward_host, forward_port);
                ++forward_datagrams;
                ++descriptor_datagrams;
                poll_feedback();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (!generation.feedback->report.delivered() &&
            !generation.feedback->report.terminal_failure()) {
            throw std::runtime_error(
                "process emulation: generation did not become terminal");
        }
        acknowledge_terminal(generation);
        total_repair_requested += generation.repair_symbols_requested;
        total_repair_emitted += generation.repair_symbols_emitted;
        if (generation.feedback->report.delivered()) ++delivered_generations;
        if (has_critical && generation.feedback->report.critical_complete) {
            ++critical_delivered_before_deadline;
        }
        std::optional<aurora::control::BiologicalFlowState>
            adaptive_state_after_terminal;
        if (const auto adaptive = std::dynamic_pointer_cast<
                aurora::control::BiologicalAdaptivePolicy>(policy)) {
            adaptive_state_after_terminal = adaptive->flow_state(profile);
        }
        const aurora::control::BiologicalFlowState empty_adaptive_state{};
        const auto& adaptive_at_plan = generation.adaptive_state_at_plan
            .value_or(empty_adaptive_state);
        const auto& adaptive_after_terminal = adaptive_state_after_terminal
            .value_or(empty_adaptive_state);
        std::cout << "sender_generation_complete index=" << generation_index
                  << " generation=" << descriptor.generation_id
                  << " policy_id=" << descriptor.policy_id
                  << " policy_version=" << descriptor.policy_version
                  << " delivered="
                  << (generation.feedback->report.delivered() ? 1 : 0)
                  << " terminal_failure="
                  << (generation.feedback->report.terminal_failure() ? 1 : 0)
                  << " critical_before_deadline="
                  << (has_critical &&
                      generation.feedback->report.critical_complete ? 1 : 0)
                  << " source_symbols=" << descriptor.total_source_symbols
                  << " initial_symbols="
                  << generation.spawned.packets.size()
                  << " critical_protection_factor="
                  << generation.spawned.protection.critical_overhead
                  << " important_protection_factor="
                  << generation.spawned.protection.important_overhead
                  << " elastic_protection_factor="
                  << generation.spawned.protection.elastic_overhead
                  << " adaptive_state_present="
                  << (generation.adaptive_state_at_plan ? 1 : 0)
                  << " adaptive_generation_count_at_plan="
                  << adaptive_at_plan.generation_count
                  << " adaptive_success_count_at_plan="
                  << adaptive_at_plan.success_count
                  << " adaptive_failure_count_at_plan="
                  << adaptive_at_plan.failure_count
                  << " adaptive_panic_boost_at_plan="
                  << adaptive_at_plan.panic_boost
                  << " adaptive_critical_overhead_at_plan="
                  << adaptive_at_plan.critical_overhead
                  << " adaptive_important_overhead_at_plan="
                  << adaptive_at_plan.important_overhead
                  << " adaptive_success_count_after_terminal="
                  << adaptive_after_terminal.success_count
                  << " adaptive_failure_count_after_terminal="
                  << adaptive_after_terminal.failure_count
                  << " adaptive_panic_boost_after_terminal="
                  << adaptive_after_terminal.panic_boost
                  << " repair_requested="
                  << generation.repair_symbols_requested
                  << " repair_emitted="
                  << generation.repair_symbols_emitted
                  << " symbols_observed="
                  << generation.feedback->report.symbols_observed
                  << '\n';
        for (int drain = 0; drain < 64; ++drain) poll_feedback();
    }

    // Drain bounded late/duplicate reverse datagrams so replay evidence is
    // observed before the process exits. The short post-handshake timeout
    // keeps this bounded even when the reverse path dropped its final events.
    for (int drain = 0; drain < 32; ++drain) poll_feedback();

    if (feedback_applied != generations.size()) {
        throw std::runtime_error(
            "process emulation: reverse feedback was not applied to policy");
    }
    if (const auto adaptive = std::dynamic_pointer_cast<
            aurora::control::BiologicalAdaptivePolicy>(policy)) {
        const auto state = adaptive->flow_state(profile);
        if (!state || state->success_count + state->failure_count !=
                          static_cast<int>(generations.size())) {
            throw std::runtime_error(
                "process emulation: adaptive policy state is incomplete");
        }
    }
    const auto rtt = feedback_rtt.summary();
    const auto terminal_rtt = feedback_rtt.terminal_summary();
    if (rtt.count < terminal_rtt.count ||
        terminal_rtt.count != generations.size() ||
        unknown_feedback_echoes != 0) {
        throw std::runtime_error(
            "process emulation: incomplete sender-clock feedback RTT evidence");
    }

    std::cout << "sender_complete generations=" << generations.size()
              << " delivered=" << delivered_generations
              << " critical_generations=" << critical_generations
              << " critical_delivered_before_deadline="
              << critical_delivered_before_deadline
              << " policy_id=" << policy->id()
              << " policy_version=" << policy->version()
              << " workload_id=" << workload.id
              << " protocol_version="
              << aurora::emulation::process_protocol_version
              << " forward_port=" << forward_port
              << " feedback_port=" << feedback_port
              << " forward_datagrams=" << forward_datagrams
              << " descriptor_datagrams=" << descriptor_datagrams
              << " wire_symbol_datagrams=" << wire_symbol_datagrams
              << " terminal_ack_datagrams=" << terminal_ack_datagrams
              << " terminal_handshake=authenticated-ack-v1"
              << " source_symbols=" << total_source_symbols
              << " initial_symbols=" << total_initial_symbols
              << " repair_symbols_requested=" << total_repair_requested
              << " repair_symbols_emitted=" << total_repair_emitted
              << " feedback_datagrams=" << feedback_datagrams
              << " stale_feedback_datagrams="
              << stale_feedback_datagrams
              << " auth_rejected=" << authentication_rejections
              << " replay_rejected=" << replay_rejections
              << " auth_profile="
              << aurora::emulation::ProcessAuthenticator::profile()
              << " impairment_attempts=" << impairment_attempt
              << " impairment_dropped=" << dropped
              << " impairment_duplicated=" << duplicated
              << " impairment_delayed=" << delayed
              << " impairment_reordered=" << reordered
              << " trace_name=" << trace.name()
              << " trace_fingerprint=" << trace.fingerprint()
              << " sender_elapsed_ms=" << elapsed_ms(started)
              << " feedback_rtt_samples=" << rtt.count
              << " feedback_rtt_min_us=" << rtt.min_us
              << " feedback_rtt_mean_us=" << rtt.mean_us
              << " feedback_rtt_max_us=" << rtt.max_us
              << " terminal_feedback_rtt_samples=" << terminal_rtt.count
              << " terminal_feedback_rtt_min_us=" << terminal_rtt.min_us
              << " terminal_feedback_rtt_mean_us=" << terminal_rtt.mean_us
              << " terminal_feedback_rtt_max_us=" << terminal_rtt.max_us
              << " unknown_feedback_echoes=" << unknown_feedback_echoes
              << " feedback_applied=" << feedback_applied << '\n';
    return 0;
}

int run_receiver(std::string_view forward_bind_host,
                 std::uint16_t forward_port,
                 std::string_view feedback_host,
                 std::uint16_t feedback_port,
                 std::size_t expected_generations,
                 const std::string& reverse_trace_path,
                 const std::string& key_path,
                 std::uint64_t session_id,
                 std::uint64_t startup_timeout_ms,
                 std::uint64_t service_timeout_ms,
                 std::optional<std::size_t> outage_generation_index) {
    if (outage_generation_index &&
        *outage_generation_index >= expected_generations) {
        throw std::invalid_argument(
            "process emulation: outage generation is outside the workload");
    }
    UdpSocket forward_socket(forward_bind_host, forward_port);
    UdpSocket feedback_socket("0.0.0.0", 0);
    aurora::fec::ExperimentalLtLikeCodec codec;
    std::unordered_map<std::string,
        std::unique_ptr<aurora::transport::GenerationReceiver>> receivers;
    std::unordered_set<std::string> terminal_generations;
    std::unordered_set<std::string> acknowledged_terminal_generations;
    struct ReceiverObservation {
        std::optional<std::uint64_t> critical_completed_at_ms;
        std::uint32_t last_feedback_rank = 0;
        std::size_t generation_index = 0;
        std::uint64_t descriptor_received_at_ms = 0;
        bool regime_outage = false;
        std::uint64_t suppressed_symbols = 0;
    };
    std::unordered_map<std::string, ReceiverObservation> observations;
    std::uint64_t delivered_generations = 0;
    std::uint64_t critical_generations = 0;
    std::uint64_t critical_delivered_before_deadline = 0;
    std::uint64_t regime_suppressed_symbols = 0;
    const auto readiness_started = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> service_started;
    const auto reverse_trace =
        aurora::emulation::ImpairmentTrace::load(reverse_trace_path);
    const aurora::emulation::ProcessAuthenticator authenticator(
        aurora::emulation::ProcessAuthenticator::load_key(key_path),
        session_id);
    aurora::emulation::ReplayWindow forward_replay_window;
    struct PendingFeedback {
        std::chrono::steady_clock::time_point release;
        std::uint64_t attempt = 0;
        std::uint64_t order = 0;
        std::vector<std::uint8_t> frame;
    };
    std::vector<PendingFeedback> pending_feedback;
    std::uint64_t reverse_attempts = 0;
    std::uint64_t reverse_order = 0;
    std::uint64_t highest_reverse_sent = 0;
    std::uint32_t reverse_datagrams = 0;
    std::uint32_t reverse_dropped = 0;
    std::uint32_t reverse_duplicated = 0;
    std::uint32_t reverse_delayed = 0;
    std::uint32_t reverse_reordered = 0;
    bool reverse_sent_any = false;
    std::uint64_t reverse_auth_sequence = 0;
    std::uint32_t authentication_rejections = 0;
    std::uint32_t replay_rejections = 0;
    std::uint32_t terminal_feedback_retry_rounds = 0;
    std::optional<std::uint64_t> all_generations_terminal_at_ms;

    std::cout << "receiver_ready forward_port=" << forward_port
              << " protocol_version="
              << aurora::emulation::process_protocol_version
              << " feedback_port=" << feedback_port
              << " startup_timeout_ms=" << startup_timeout_ms
              << " service_timeout_ms=" << service_timeout_ms
              << " deadline_semantics=descriptor-relative-receiver-steady"
              << " regime_outage_generation_index="
              << (outage_generation_index
                      ? std::to_string(*outage_generation_index)
                      : std::string("none"))
              << " auth_profile="
              << aurora::emulation::ProcessAuthenticator::profile()
              << " terminal_handshake=authenticated-ack-v1"
              << std::endl;

    auto flush_feedback = [&](bool flush_all) {
        while (!pending_feedback.empty()) {
            const auto next = std::min_element(
                pending_feedback.begin(), pending_feedback.end(),
                [](const auto& left, const auto& right) {
                    if (left.release != right.release) {
                        return left.release < right.release;
                    }
                    return left.order < right.order;
                });
            const auto now = std::chrono::steady_clock::now();
            if (next->release > now) {
                if (!flush_all) return;
                std::this_thread::sleep_until(next->release);
            }
            if (reverse_sent_any &&
                next->attempt < highest_reverse_sent) {
                ++reverse_reordered;
            }
            highest_reverse_sent = std::max(
                highest_reverse_sent, next->attempt);
            reverse_sent_any = true;
            feedback_socket.send(
                next->frame, feedback_host, feedback_port);
            ++reverse_datagrams;
            pending_feedback.erase(next);
        }
    };

    auto send_feedback = [&](const std::vector<std::uint8_t>& frame) {
        const auto attempt = reverse_attempts++;
        const auto directive = reverse_trace.directive(attempt);
        if (directive.action ==
            aurora::emulation::ImpairmentAction::DROP) {
            ++reverse_dropped;
            return;
        }
        if (directive.delay_ms > 0) ++reverse_delayed;
        const auto release = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(directive.delay_ms);
        const auto authenticated = authenticator.seal(
            aurora::emulation::ProcessDirection::REVERSE,
            reverse_auth_sequence++, frame);
        pending_feedback.push_back(
            {release, attempt, reverse_order++, authenticated});
        if (directive.action ==
            aurora::emulation::ImpairmentAction::DUPLICATE) {
            pending_feedback.push_back(
                {release, attempt, reverse_order++, authenticated});
            ++reverse_duplicated;
        }
        flush_feedback(false);
    };

    auto integrate_report = [&] (
        const std::string& generation_id,
        const aurora::transport::GenerationReceiver& receiver,
        const aurora::transport::DecodeReport& report,
        std::uint64_t receiver_now_ms,
        std::uint64_t echoed_forward_sequence,
        bool force_feedback) {
        auto& observation = observations.at(generation_id);
        const auto& descriptor = receiver.descriptor();
        const bool newly_critical = report.critical_complete &&
            !observation.critical_completed_at_ms;
        if (newly_critical) {
            observation.critical_completed_at_ms = receiver_now_ms;
        }
        const auto feedback = aurora::emulation::encode_feedback({
            descriptor.descriptor_fingerprint, report,
            echoed_forward_sequence});
        const bool terminal =
            report.delivered() || report.terminal_failure();
        const bool terminal_was_known =
            terminal_generations.contains(generation_id);
        const bool periodic_progress = report.decoder_rank >=
            observation.last_feedback_rank + 8;
        if (force_feedback || terminal || newly_critical ||
            periodic_progress) {
            send_feedback(feedback);
            observation.last_feedback_rank = report.decoder_rank;
        }
        if (!terminal) return;

        // Repeat the terminal datagram to tolerate reverse loss while keeping
        // every copy authenticated and subject to replay rejection.
        send_feedback(feedback);
        send_feedback(feedback);
        if (terminal_was_known) ++terminal_feedback_retry_rounds;
        if (terminal_generations.insert(generation_id).second) {
            std::optional<std::uint64_t> critical_deadline_duration_ms;
            std::optional<std::uint64_t> critical_deadline_at_ms;
            for (const auto& segment : descriptor.segments) {
                if (segment.importance != aurora::transport::
                        TransportImportance::CRITICAL) {
                    continue;
                }
                critical_deadline_duration_ms = critical_deadline_duration_ms
                    ? std::min(*critical_deadline_duration_ms,
                               segment.deadline_ms)
                    : segment.deadline_ms;
                const auto local_deadline = receiver.segment_expires_at_ms(
                    segment.segment_id);
                critical_deadline_at_ms = critical_deadline_at_ms
                    ? std::min(*critical_deadline_at_ms, local_deadline)
                    : local_deadline;
            }
            const bool critical_before_deadline =
                critical_deadline_at_ms &&
                observation.critical_completed_at_ms &&
                *observation.critical_completed_at_ms <=
                    *critical_deadline_at_ms;
            if (report.delivered()) ++delivered_generations;
            if (critical_before_deadline) {
                ++critical_delivered_before_deadline;
            }
            std::cout << "receiver_generation_complete index="
                      << observation.generation_index
                      << " generation=" << generation_id
                      << " bytes=" << report.recovered_bytes
                      << " delivered=" << (report.delivered() ? 1 : 0)
                      << " terminal_failure="
                      << (report.terminal_failure() ? 1 : 0)
                      << " critical_before_deadline="
                      << (critical_before_deadline ? 1 : 0)
                      << " critical_completed_at_ms="
                      << observation.critical_completed_at_ms.value_or(0)
                      << " descriptor_received_at_ms="
                      << observation.descriptor_received_at_ms
                      << " critical_deadline_duration_ms="
                      << critical_deadline_duration_ms.value_or(0)
                      << " critical_deadline_at_ms="
                      << critical_deadline_at_ms.value_or(0)
                      << " terminal_at_ms=" << receiver_now_ms
                      << " generation_deadline_duration_ms="
                      << receiver.generation_deadline_duration_ms()
                      << " generation_deadline_at_ms="
                      << receiver.generation_expires_at_ms()
                      << " deadline_clock=receiver-steady-descriptor-relative"
                      << " regime_outage="
                      << (observation.regime_outage ? 1 : 0)
                      << " regime_suppressed_symbols="
                      << observation.suppressed_symbols
                      << " symbols_observed=" << report.symbols_observed
                      << '\n';
        }
        if (terminal_generations.size() == expected_generations &&
            !all_generations_terminal_at_ms) {
            all_generations_terminal_at_ms = receiver_now_ms;
        }
    };

    auto emit_receiver_complete = [&](std::uint64_t acknowledged_at_ms) {
        if (!all_generations_terminal_at_ms) {
            throw std::runtime_error(
                "process emulation: terminal acknowledgement preceded completion");
        }
        flush_feedback(true);
        std::cout << "receiver_complete generations="
                  << terminal_generations.size()
                  << " delivered=" << delivered_generations
                  << " critical_generations=" << critical_generations
                  << " critical_delivered_before_deadline="
                  << critical_delivered_before_deadline
                  << " deadline_semantics=descriptor-relative-receiver-steady"
                  << " regime_outage_generation_index="
                  << (outage_generation_index
                          ? std::to_string(*outage_generation_index)
                          : std::string("none"))
                  << " regime_suppressed_symbols="
                  << regime_suppressed_symbols
                  << " protocol_version="
                  << aurora::emulation::process_protocol_version
                  << " forward_port=" << forward_port
                  << " feedback_port=" << feedback_port
                  << " reverse_attempts=" << reverse_attempts
                  << " reverse_datagrams=" << reverse_datagrams
                  << " reverse_dropped=" << reverse_dropped
                  << " reverse_duplicated=" << reverse_duplicated
                  << " reverse_delayed=" << reverse_delayed
                  << " reverse_reordered=" << reverse_reordered
                  << " reverse_trace_name=" << reverse_trace.name()
                  << " reverse_trace_fingerprint="
                  << reverse_trace.fingerprint()
                  << " auth_rejected=" << authentication_rejections
                  << " replay_rejected=" << replay_rejections
                  << " auth_profile="
                  << aurora::emulation::ProcessAuthenticator::profile()
                  << " terminal_handshake=authenticated-ack-v1"
                  << " terminal_acknowledged="
                  << acknowledged_terminal_generations.size()
                  << " terminal_feedback_retry_rounds="
                  << terminal_feedback_retry_rounds
                  << " terminal_ack_wait_ms="
                  << acknowledged_at_ms - *all_generations_terminal_at_ms
                  << " service_elapsed_ms="
                  << *all_generations_terminal_at_ms << '\n';
    };

    while (true) {
        if (!service_started &&
            elapsed_ms(readiness_started) >= startup_timeout_ms) {
            throw std::runtime_error(
                "process emulation: receiver startup timed out");
        }
        if (service_started &&
            elapsed_ms(*service_started) >= service_timeout_ms) {
            throw std::runtime_error(
                "process emulation: receiver service timed out");
        }
        const auto wire_datagram = forward_socket.receive();
        if (!wire_datagram) {
            flush_feedback(false);
            continue;
        }
        std::optional<aurora::emulation::AuthenticatedPayload> opened;
        try {
            opened = authenticator.open(
                aurora::emulation::ProcessDirection::FORWARD,
                *wire_datagram);
        } catch (const std::invalid_argument&) {
            ++authentication_rejections;
            continue;
        }
        if (!forward_replay_window.accept(opened->sequence)) {
            ++replay_rejections;
            continue;
        }
        const auto& datagram = opened->payload;
        try {
            const auto type = aurora::emulation::frame_type(datagram);
            if (type == aurora::emulation::FrameType::TERMINAL_ACK) {
                if (!service_started) continue;
                const auto acknowledgement =
                    aurora::emulation::decode_terminal_ack(datagram);
                const auto found = receivers.find(
                    acknowledgement.generation_id);
                if (found == receivers.end() ||
                    found->second->descriptor().descriptor_fingerprint !=
                        acknowledgement.descriptor_fingerprint ||
                    !terminal_generations.contains(
                        acknowledgement.generation_id)) {
                    throw std::invalid_argument(
                        "process emulation: invalid terminal acknowledgement");
                }
                acknowledged_terminal_generations.insert(
                    acknowledgement.generation_id);
                if (terminal_generations.size() == expected_generations &&
                    acknowledged_terminal_generations.size() ==
                        expected_generations) {
                    emit_receiver_complete(elapsed_ms(*service_started));
                    return 0;
                }
                continue;
            }
            if (type == aurora::emulation::FrameType::DESCRIPTOR) {
                const auto descriptor =
                    aurora::emulation::decode_descriptor(datagram);
                if (!service_started) {
                    service_started = std::chrono::steady_clock::now();
                }
                const auto receiver_now_ms = elapsed_ms(*service_started);
                auto found = receivers.find(descriptor.generation_id);
                if (found == receivers.end()) {
                    const auto generation_index = receivers.size();
                    found = receivers.emplace(descriptor.generation_id,
                        std::make_unique<aurora::transport::GenerationReceiver>(
                            descriptor, codec, true, receiver_now_ms)).first;
                    observations.emplace(
                        descriptor.generation_id,
                        ReceiverObservation{
                            std::nullopt,
                            0,
                            generation_index,
                            receiver_now_ms,
                            outage_generation_index &&
                                generation_index == *outage_generation_index,
                            0});
                    if (std::any_of(
                            descriptor.segments.begin(),
                            descriptor.segments.end(),
                            [](const auto& segment) {
                                return segment.importance ==
                                    aurora::transport::
                                        TransportImportance::CRITICAL;
                            })) {
                        ++critical_generations;
                    }
                } else if (found->second->descriptor().descriptor_fingerprint !=
                           descriptor.descriptor_fingerprint) {
                    throw std::invalid_argument(
                        "process emulation: conflicting descriptor");
                }
                const auto report = found->second->integrate(
                    {}, receiver_now_ms);
                integrate_report(
                    descriptor.generation_id, *found->second, report,
                    receiver_now_ms, opened->sequence, true);
                continue;
            }
            if (type != aurora::emulation::FrameType::SYMBOL) {
                continue;
            }
            const auto packet = aurora::emulation::decode_symbol(datagram);
            const auto found = receivers.find(packet.generation_id);
            if (found == receivers.end()) continue;
            const auto receiver_now_ms = elapsed_ms(*service_started);
            auto& observation = observations.at(packet.generation_id);
            std::vector<::fec::Pkt> accepted_packets;
            if (observation.regime_outage) {
                ++observation.suppressed_symbols;
                ++regime_suppressed_symbols;
            } else {
                accepted_packets.push_back(packet);
            }
            const auto report = found->second->integrate(
                accepted_packets, receiver_now_ms);
            integrate_report(
                packet.generation_id, *found->second, report,
                receiver_now_ms, opened->sequence, false);
        } catch (const std::invalid_argument&) {
            // Malformed datagrams are isolated to the receiver boundary.
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto role = argc > 1 ? std::string_view(argv[1]) : "";
        const bool sender_arguments =
            role == "sender" && (argc == 9 || argc == 13);
        const bool receiver_arguments =
            role == "receiver" &&
            (argc == 10 || argc == 12 || argc == 14);
        const bool sender_runtime_options = argc != 13 ||
            (std::string_view(argv[9]) == "--policy" &&
             std::string_view(argv[11]) == "--workload");
        const bool receiver_runtime_options = argc != 14 ||
            std::string_view(argv[12]) == "--outage-generation";
        if ((!sender_arguments && !receiver_arguments) ||
            !sender_runtime_options || !receiver_runtime_options) {
            std::cerr << "usage: aurora_process_emulation "
                         "sender <forward-host> <forward-port> "
                         "<feedback-bind-host> <feedback-port> <trace-file> "
                         "<key-file> <session-id-hex> "
                         "[--policy <fixed-minimum|fixed-class-aware|"
                         "biological-adaptive> --workload "
                         "<smoke-v2|policy-pilot-v1>]\n"
                         "   or: aurora_process_emulation receiver "
                         "<forward-bind-host> <forward-port> "
                         "<feedback-host> <feedback-port> <generation-count> "
                         "<reverse-trace-file> <key-file> "
                         "<session-id-hex> [startup-timeout-ms "
                         "service-timeout-ms [--outage-generation "
                         "<zero-based-index>]]\n";
            return 2;
        }
        SocketRuntime runtime;
        const auto forward_port = parse_port(argv[3]);
        const auto feedback_port = parse_port(argv[5]);
        if (forward_port == feedback_port) {
            throw std::invalid_argument(
                "process emulation: forward and feedback ports must differ");
        }
        if (role == "sender") {
            const auto policy_id = argc == 13
                ? std::string_view(argv[10])
                : std::string_view("biological-adaptive");
            const auto workload_id = argc == 13
                ? std::string_view(argv[12])
                : std::string_view("smoke-v2");
            return run_sender(argv[2], forward_port, argv[4],
                              feedback_port, argv[6], argv[7],
                              aurora::emulation::ProcessAuthenticator::
                                  parse_session_id(argv[8]),
                              policy_id, workload_id);
        }
        if (role == "receiver") {
            const auto count = parse_generation_count(argv[6]);
            const auto startup_timeout_ms =
                argc >= 12 ? parse_timeout_ms(argv[10]) : 60'000;
            const auto service_timeout_ms =
                argc >= 12 ? parse_timeout_ms(argv[11]) : 15'000;
            const auto outage_generation_index = argc == 14
                ? std::optional<std::size_t>(parse_generation_index(argv[13]))
                : std::nullopt;
            return run_receiver(argv[2], forward_port, argv[4],
                                feedback_port, count, argv[7], argv[8],
                                aurora::emulation::ProcessAuthenticator::
                                    parse_session_id(argv[9]),
                                startup_timeout_ms, service_timeout_ms,
                                outage_generation_index);
        }
        throw std::invalid_argument("process emulation: unknown role");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

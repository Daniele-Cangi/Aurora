#include "aurora/control/TransportPolicy.hpp"
#include "aurora/emulation/ImpairmentTrace.hpp"
#include "aurora/emulation/ProcessProtocol.hpp"
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
            aurora::emulation::maximum_datagram_bytes);
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

std::uint64_t elapsed_ms(
    const std::chrono::steady_clock::time_point& started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

std::optional<aurora::emulation::FeedbackFrame> receive_feedback(
    const UdpSocket& socket) {
    const auto datagram = socket.receive();
    if (!datagram) return std::nullopt;
    try {
        return aurora::emulation::decode_feedback(*datagram);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

int run_sender(std::string_view forward_host, std::uint16_t forward_port,
               std::string_view feedback_bind_host,
               std::uint16_t feedback_port,
               const std::string& trace_path) {
    const auto started = std::chrono::steady_clock::now();
    UdpSocket feedback_socket(feedback_bind_host, feedback_port);
    auto codec = std::make_shared<aurora::fec::ExperimentalLtLikeCodec>();
    auto policy =
        std::make_shared<aurora::control::BiologicalAdaptivePolicy>();
    aurora::transport::GenerationManager manager(codec, policy);
    const auto contract = aurora::transport::TransportContract::parse(
        "deadline:30s;reliability:0.99;duty:0.1;rf:on;optical:on;"
        "backscatter:on;ris:4;reserve:0.05;max_repair_amplification:4;"
        "min_critical_overhead:1.5;seed:1701");
    const auto profile = manager.build_profile(contract);
    const auto trace = aurora::emulation::ImpairmentTrace::load(trace_path);
    struct SenderGeneration {
        aurora::transport::GenerationSpawnResult spawned;
        std::optional<aurora::emulation::FeedbackFrame> feedback;
    };
    std::vector<SenderGeneration> generations;
    for (std::size_t generation = 0; generation < 2; ++generation) {
        std::vector<std::uint8_t> payload(448 + generation * 128);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>(
                (i * (41 + generation * 2) + 13 + generation) & 0xffU);
        }
        SenderGeneration state;
        state.spawned = manager.spawn(
            contract, "process-emulation-" + std::to_string(generation),
            payload, 64, 0);
        generations.push_back(std::move(state));
    }

    UdpSocket forward_socket("0.0.0.0", 0);
    std::uint32_t forward_datagrams = 0;
    std::uint32_t feedback_datagrams = 0;
    std::uint64_t impairment_attempt = 0;
    std::uint32_t dropped = 0;
    std::uint32_t duplicated = 0;
    std::uint32_t delayed = 0;
    std::uint32_t reordered = 0;

    auto accept_feedback = [&](aurora::emulation::FeedbackFrame incoming) {
        for (auto& generation : generations) {
            const auto& descriptor = generation.spawned.descriptor;
            if (incoming.report.generation_id == descriptor.generation_id &&
                incoming.descriptor_fingerprint ==
                    descriptor.descriptor_fingerprint &&
                incoming.report.source_bytes ==
                    descriptor.original_payload_length &&
                incoming.report.required_rank == descriptor.total_source_symbols) {
                generation.feedback = std::move(incoming);
                ++feedback_datagrams;
                return;
            }
        }
    };
    auto poll_feedback = [&] {
        if (auto incoming = receive_feedback(feedback_socket)) {
            accept_feedback(std::move(*incoming));
        }
    };

    for (int attempt = 0; attempt < 20; ++attempt) {
        bool all_acknowledged = true;
        for (auto& generation : generations) {
            if (generation.feedback) continue;
            all_acknowledged = false;
            forward_socket.send(aurora::emulation::encode_descriptor(
                generation.spawned.descriptor), forward_host, forward_port);
            ++forward_datagrams;
            poll_feedback();
        }
        if (all_acknowledged) break;
    }
    for (const auto& generation : generations) {
        if (!generation.feedback) throw std::runtime_error(
            "process emulation: receiver did not acknowledge every descriptor");
    }

    struct PendingDatagram {
        std::chrono::steady_clock::time_point release;
        std::uint64_t attempt = 0;
        std::uint64_t order = 0;
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
            forward_socket.send(
                next->frame, forward_host, forward_port);
            ++forward_datagrams;
            pending.erase(next);
            poll_feedback();
        }
    };

    auto send_symbol = [&](const ::fec::Pkt& packet) {
        const auto frame = aurora::emulation::encode_symbol(packet);
        const auto attempt = impairment_attempt++;
        const auto directive = trace.directive(attempt);
        if (directive.action == aurora::emulation::ImpairmentAction::DROP) {
            ++dropped;
            return;
        }
        if (directive.delay_ms > 0) ++delayed;
        const auto release = release_epoch +
            std::chrono::milliseconds(directive.delay_ms);
        pending.push_back({release, attempt, enqueue_order++, frame});
        if (directive.action ==
            aurora::emulation::ImpairmentAction::DUPLICATE) {
            pending.push_back({release, attempt, enqueue_order++, frame});
            ++duplicated;
        }
    };

    std::size_t maximum_packets = 0;
    for (const auto& generation : generations) {
        maximum_packets = std::max(
            maximum_packets, generation.spawned.packets.size());
    }
    for (std::size_t index = 0; index < maximum_packets; ++index) {
        for (auto& generation : generations) {
            if (generation.feedback->report.delivered() ||
                index >= generation.spawned.packets.size()) continue;
            send_symbol(generation.spawned.packets[index]);
        }
    }
    flush_pending(true);

    for (int round = 0; round < 128; ++round) {
        bool all_delivered = true;
        release_epoch = std::chrono::steady_clock::now();
        for (auto& generation : generations) {
            if (generation.feedback->report.delivered()) continue;
            all_delivered = false;
            const auto emitted = manager.emit_repairs(
                generation.spawned.descriptor.generation_id, 1, false,
                elapsed_ms(started));
            if (!emitted.packets.empty()) send_symbol(emitted.packets.front());
        }
        flush_pending(true);
        if (all_delivered) break;
    }

    for (auto& generation : generations) {
        if (!generation.feedback->report.delivered()) throw std::runtime_error(
            "process emulation: receiver did not report every delivery");
        policy->observe(profile, generation.feedback->report);
    }
    const auto state = policy->flow_state(profile);
    if (!state || state->success_count !=
                      static_cast<int>(generations.size())) {
        throw std::runtime_error(
            "process emulation: reverse feedback was not applied to policy");
    }

    std::cout << "sender_complete generations=" << generations.size()
              << " forward_port=" << forward_port
              << " feedback_port=" << feedback_port
              << " forward_datagrams=" << forward_datagrams
              << " feedback_datagrams=" << feedback_datagrams
              << " impairment_attempts=" << impairment_attempt
              << " impairment_dropped=" << dropped
              << " impairment_duplicated=" << duplicated
              << " impairment_delayed=" << delayed
              << " impairment_reordered=" << reordered
              << " trace_name=" << trace.name()
              << " trace_fingerprint=" << trace.fingerprint()
              << " feedback_applied=2\n";
    return 0;
}

int run_receiver(std::string_view forward_bind_host,
                 std::uint16_t forward_port,
                 std::string_view feedback_host,
                 std::uint16_t feedback_port,
                 std::size_t expected_generations) {
    UdpSocket forward_socket(forward_bind_host, forward_port);
    UdpSocket feedback_socket("0.0.0.0", 0);
    aurora::fec::ExperimentalLtLikeCodec codec;
    std::unordered_map<std::string,
        std::unique_ptr<aurora::transport::GenerationReceiver>> receivers;
    std::unordered_set<std::string> completed;
    const auto started = std::chrono::steady_clock::now();

    while (elapsed_ms(started) < 15'000) {
        const auto datagram = forward_socket.receive();
        if (!datagram) continue;
        try {
            const auto type = aurora::emulation::frame_type(*datagram);
            if (type == aurora::emulation::FrameType::DESCRIPTOR) {
                const auto descriptor =
                    aurora::emulation::decode_descriptor(*datagram);
                auto found = receivers.find(descriptor.generation_id);
                if (found == receivers.end()) {
                    found = receivers.emplace(descriptor.generation_id,
                        std::make_unique<aurora::transport::GenerationReceiver>(
                            descriptor, codec, true)).first;
                } else if (found->second->descriptor().descriptor_fingerprint !=
                           descriptor.descriptor_fingerprint) {
                    throw std::invalid_argument(
                        "process emulation: conflicting descriptor");
                }
                const auto report = found->second->integrate({}, elapsed_ms(started));
                feedback_socket.send(
                    aurora::emulation::encode_feedback({
                        descriptor.descriptor_fingerprint, report}),
                    feedback_host, feedback_port);
                continue;
            }
            if (type != aurora::emulation::FrameType::SYMBOL) {
                continue;
            }
            const auto packet = aurora::emulation::decode_symbol(*datagram);
            const auto found = receivers.find(packet.generation_id);
            if (found == receivers.end()) continue;
            const auto report =
                found->second->integrate({packet}, elapsed_ms(started));
            const auto feedback = aurora::emulation::encode_feedback({
                found->second->descriptor().descriptor_fingerprint, report});
            feedback_socket.send(feedback, feedback_host, feedback_port);
            if (report.delivered()) {
                // Repeat the terminal datagram to tolerate one lost reverse
                // packet while keeping UDP semantics explicit.
                feedback_socket.send(feedback, feedback_host, feedback_port);
                feedback_socket.send(feedback, feedback_host, feedback_port);
                if (completed.insert(packet.generation_id).second) {
                    std::cout << "receiver_generation_complete generation="
                              << packet.generation_id
                              << " bytes=" << report.recovered_bytes << '\n';
                }
                if (completed.size() == expected_generations) {
                    std::cout << "receiver_complete generations="
                              << completed.size()
                              << " forward_port=" << forward_port
                              << " feedback_port=" << feedback_port << '\n';
                    return 0;
                }
            }
        } catch (const std::invalid_argument&) {
            // Malformed datagrams are isolated to the receiver boundary.
        }
    }
    throw std::runtime_error("process emulation: receiver timed out");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 7) {
            std::cerr << "usage: aurora_process_emulation "
                         "sender <forward-host> <forward-port> "
                         "<feedback-bind-host> <feedback-port> <trace-file>\n"
                         "   or: aurora_process_emulation receiver "
                         "<forward-bind-host> <forward-port> "
                         "<feedback-host> <feedback-port> <generation-count>\n";
            return 2;
        }
        SocketRuntime runtime;
        const auto forward_port = parse_port(argv[3]);
        const auto feedback_port = parse_port(argv[5]);
        if (forward_port == feedback_port) {
            throw std::invalid_argument(
                "process emulation: forward and feedback ports must differ");
        }
        if (std::string_view(argv[1]) == "sender") {
            return run_sender(argv[2], forward_port, argv[4],
                              feedback_port, argv[6]);
        }
        if (std::string_view(argv[1]) == "receiver") {
            const auto count = parse_generation_count(argv[6]);
            return run_receiver(argv[2], forward_port, argv[4],
                                feedback_port, count);
        }
        throw std::invalid_argument("process emulation: unknown role");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

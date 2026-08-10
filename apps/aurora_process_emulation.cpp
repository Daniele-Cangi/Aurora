#include "aurora/control/TransportPolicy.hpp"
#include "aurora/emulation/ProcessProtocol.hpp"
#include "aurora/fec/GenerationCodec.hpp"
#include "aurora/transport/GenerationManager.hpp"
#include "aurora/transport/GenerationReceiver.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
    explicit UdpSocket(std::uint16_t bind_port) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) fail("socket creation");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(bind_port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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
              std::uint16_t destination_port) const {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(destination_port);
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

std::uint64_t elapsed_ms(
    const std::chrono::steady_clock::time_point& started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

std::optional<aurora::emulation::FeedbackFrame> receive_feedback(
    const UdpSocket& socket,
    const aurora::transport::GenerationDescriptor& descriptor) {
    const auto datagram = socket.receive();
    if (!datagram) return std::nullopt;
    try {
        auto feedback = aurora::emulation::decode_feedback(*datagram);
        if (feedback.descriptor_fingerprint !=
                descriptor.descriptor_fingerprint ||
            feedback.report.generation_id != descriptor.generation_id ||
            feedback.report.source_bytes !=
                descriptor.original_payload_length ||
            feedback.report.required_rank !=
                descriptor.total_source_symbols) {
            return std::nullopt;
        }
        return feedback;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

int run_sender(std::uint16_t forward_port, std::uint16_t feedback_port) {
    const auto started = std::chrono::steady_clock::now();
    UdpSocket feedback_socket(feedback_port);
    auto codec = std::make_shared<aurora::fec::ExperimentalLtLikeCodec>();
    auto policy =
        std::make_shared<aurora::control::BiologicalAdaptivePolicy>();
    aurora::transport::GenerationManager manager(codec, policy);
    const auto contract = aurora::transport::TransportContract::parse(
        "deadline:30s;reliability:0.99;duty:0.1;rf:on;optical:on;"
        "backscatter:on;ris:4;reserve:0.05;max_repair_amplification:4;"
        "min_critical_overhead:1.5;seed:1701");
    const auto profile = manager.build_profile(contract);
    std::vector<std::uint8_t> payload(512);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 41 + 13) & 0xffU);
    }
    auto spawned = manager.spawn(
        contract, "process-emulation", payload, 64, 0);

    UdpSocket forward_socket(0);
    const auto descriptor_frame =
        aurora::emulation::encode_descriptor(spawned.descriptor);
    std::optional<aurora::emulation::FeedbackFrame> feedback;
    std::uint32_t forward_datagrams = 0;
    std::uint32_t feedback_datagrams = 0;
    for (int attempt = 0; attempt < 20 && !feedback; ++attempt) {
        forward_socket.send(descriptor_frame, forward_port);
        ++forward_datagrams;
        feedback = receive_feedback(feedback_socket, spawned.descriptor);
        if (feedback) ++feedback_datagrams;
    }
    if (!feedback) {
        throw std::runtime_error(
            "process emulation: receiver did not acknowledge descriptor");
    }

    auto send_symbol = [&](const ::fec::Pkt& packet) {
        forward_socket.send(
            aurora::emulation::encode_symbol(packet), forward_port);
        ++forward_datagrams;
        if (auto received =
                receive_feedback(feedback_socket, spawned.descriptor)) {
            ++feedback_datagrams;
            feedback = std::move(received);
        }
    };

    for (const auto& packet : spawned.packets) {
        // A declared deterministic forward-path impairment. The receiver's
        // explicit reverse feedback decides whether repairs are needed.
        if (packet.seq % 4U == 0U) continue;
        send_symbol(packet);
        if (feedback->report.delivered()) break;
    }

    for (int repair = 0;
         repair < 64 && !feedback->report.delivered(); ++repair) {
        const auto emitted = manager.emit_repairs(
            spawned.descriptor.generation_id, 1, false, elapsed_ms(started));
        if (emitted.packets.empty()) break;
        send_symbol(emitted.packets.front());
    }

    if (!feedback->report.delivered()) {
        throw std::runtime_error(
            "process emulation: receiver did not report delivery");
    }
    policy->observe(profile, feedback->report);
    const auto state = policy->flow_state(profile);
    if (!state || state->success_count != 1) {
        throw std::runtime_error(
            "process emulation: reverse feedback was not applied to policy");
    }

    std::cout << "sender_complete generation="
              << spawned.descriptor.generation_id
              << " forward_port=" << forward_port
              << " feedback_port=" << feedback_port
              << " forward_datagrams=" << forward_datagrams
              << " feedback_datagrams=" << feedback_datagrams
              << " feedback_applied=true\n";
    return 0;
}

int run_receiver(std::uint16_t forward_port, std::uint16_t feedback_port) {
    UdpSocket forward_socket(forward_port);
    UdpSocket feedback_socket(0);
    aurora::fec::ExperimentalLtLikeCodec codec;
    std::optional<aurora::transport::GenerationReceiver> receiver;
    const auto started = std::chrono::steady_clock::now();

    while (elapsed_ms(started) < 15'000) {
        const auto datagram = forward_socket.receive();
        if (!datagram) continue;
        try {
            const auto type = aurora::emulation::frame_type(*datagram);
            if (type == aurora::emulation::FrameType::DESCRIPTOR) {
                const auto descriptor =
                    aurora::emulation::decode_descriptor(*datagram);
                if (!receiver) {
                    receiver.emplace(descriptor, codec, true);
                } else if (receiver->descriptor().descriptor_fingerprint !=
                           descriptor.descriptor_fingerprint) {
                    throw std::invalid_argument(
                        "process emulation: conflicting descriptor");
                }
                const auto report = receiver->integrate({}, elapsed_ms(started));
                feedback_socket.send(
                    aurora::emulation::encode_feedback({
                        descriptor.descriptor_fingerprint, report}),
                    feedback_port);
                continue;
            }
            if (type != aurora::emulation::FrameType::SYMBOL || !receiver) {
                continue;
            }
            const auto packet = aurora::emulation::decode_symbol(*datagram);
            const auto report =
                receiver->integrate({packet}, elapsed_ms(started));
            const auto feedback = aurora::emulation::encode_feedback({
                receiver->descriptor().descriptor_fingerprint, report});
            feedback_socket.send(feedback, feedback_port);
            if (report.delivered()) {
                // Repeat the terminal datagram to tolerate one lost reverse
                // packet while keeping UDP semantics explicit.
                feedback_socket.send(feedback, feedback_port);
                feedback_socket.send(feedback, feedback_port);
                std::cout << "receiver_complete generation="
                          << receiver->descriptor().generation_id
                          << " bytes=" << report.recovered_bytes
                          << " forward_port=" << forward_port
                          << " feedback_port=" << feedback_port << '\n';
                return 0;
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
        if (argc != 4) {
            std::cerr << "usage: aurora_process_emulation "
                         "<sender|receiver> <forward-port> <feedback-port>\n";
            return 2;
        }
        SocketRuntime runtime;
        const auto forward_port = parse_port(argv[2]);
        const auto feedback_port = parse_port(argv[3]);
        if (forward_port == feedback_port) {
            throw std::invalid_argument(
                "process emulation: forward and feedback ports must differ");
        }
        if (std::string_view(argv[1]) == "sender") {
            return run_sender(forward_port, feedback_port);
        }
        if (std::string_view(argv[1]) == "receiver") {
            return run_receiver(forward_port, feedback_port);
        }
        throw std::invalid_argument("process emulation: unknown role");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

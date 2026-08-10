#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef AURORA_USE_REAL_CRYPTO
#include <sodium.h>
#endif

namespace aurora::emulation {

enum class ProcessDirection : std::uint8_t { FORWARD = 1, REVERSE = 2 };

struct AuthenticatedPayload {
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

class ReplayWindow {
public:
    bool accept(std::uint64_t sequence) {
        if (!initialized_) {
            initialized_ = true;
            highest_ = sequence;
            bitmap_ = 1;
            return true;
        }
        if (sequence > highest_) {
            const auto shift = sequence - highest_;
            bitmap_ = shift >= 64 ? 1 : (bitmap_ << shift) | 1;
            highest_ = sequence;
            return true;
        }
        const auto age = highest_ - sequence;
        if (age >= 64) return false;
        const std::uint64_t mask = std::uint64_t{1} << age;
        if ((bitmap_ & mask) != 0) return false;
        bitmap_ |= mask;
        return true;
    }

private:
    std::uint64_t highest_ = 0;
    std::uint64_t bitmap_ = 0;
    bool initialized_ = false;
};

class ProcessAuthenticator {
public:
    using Key = std::array<std::uint8_t, 32>;

    ProcessAuthenticator(Key key, std::uint64_t session_id)
        : key_(key), session_id_(session_id) {
#ifdef AURORA_USE_REAL_CRYPTO
        if (sodium_init() < 0) {
            throw std::runtime_error(
                "process authentication: sodium initialization failed");
        }
#endif
    }

    static Key load_key(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::invalid_argument(
            "process authentication: cannot open key file " + path);
        std::ostringstream contents;
        contents << input.rdbuf();
        std::string encoded;
        for (char value : contents.str()) {
            if (value != ' ' && value != '\t' && value != '\r' &&
                value != '\n') encoded.push_back(value);
        }
        if (encoded.size() != 64) throw std::invalid_argument(
            "process authentication: key must contain 64 hex digits");
        Key key{};
        for (std::size_t index = 0; index < key.size(); ++index) {
            key[index] = static_cast<std::uint8_t>(
                (hex_nibble(encoded[index * 2]) << 4) |
                hex_nibble(encoded[index * 2 + 1]));
        }
        return key;
    }

    static std::uint64_t parse_session_id(std::string_view encoded) {
        if (encoded.size() != 16) throw std::invalid_argument(
            "process authentication: session id must contain 16 hex digits");
        std::uint64_t value = 0;
        for (char digit : encoded) {
            value = (value << 4) | hex_nibble(digit);
        }
        return value;
    }

    [[nodiscard]] std::vector<std::uint8_t> seal(
        ProcessDirection direction, std::uint64_t sequence,
        const std::vector<std::uint8_t>& payload) const {
        if (payload.size() > 0xffffffffULL) throw std::invalid_argument(
            "process authentication: payload too large");
        std::vector<std::uint8_t> frame;
        frame.reserve(28 + payload.size() + tag_bytes);
        frame.insert(frame.end(), {'A', 'U', 'P', 'A'});
        frame.push_back(1);
        frame.push_back(static_cast<std::uint8_t>(direction));
        frame.push_back(0);
        frame.push_back(0);
        append_u64(frame, session_id_);
        append_u64(frame, sequence);
        append_u32(frame, static_cast<std::uint32_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());
        const auto tag = authenticate(frame);
        frame.insert(frame.end(), tag.begin(), tag.end());
        return frame;
    }

    [[nodiscard]] AuthenticatedPayload open(
        ProcessDirection expected_direction,
        const std::vector<std::uint8_t>& frame) const {
        constexpr std::size_t header_bytes = 28;
        if (frame.size() < header_bytes + tag_bytes ||
            frame[0] != 'A' || frame[1] != 'U' || frame[2] != 'P' ||
            frame[3] != 'A' || frame[4] != 1 || frame[5] !=
                static_cast<std::uint8_t>(expected_direction) ||
            frame[6] != 0 || frame[7] != 0) {
            throw std::invalid_argument(
                "process authentication: invalid envelope");
        }
        std::size_t offset = 8;
        const auto session = read_u64(frame, offset);
        const auto sequence = read_u64(frame, offset);
        const auto length = read_u32(frame, offset);
        if (session != session_id_ ||
            frame.size() != header_bytes + length + tag_bytes) {
            throw std::invalid_argument(
                "process authentication: session or length mismatch");
        }
        const std::vector<std::uint8_t> authenticated(
            frame.begin(), frame.end() - static_cast<std::ptrdiff_t>(tag_bytes));
        const auto expected = authenticate(authenticated);
        if (!constant_time_equal(
                expected.data(), frame.data() + authenticated.size(), tag_bytes)) {
            throw std::invalid_argument(
                "process authentication: tag mismatch");
        }
        return {sequence, std::vector<std::uint8_t>(
            frame.begin() + static_cast<std::ptrdiff_t>(header_bytes),
            frame.end() - static_cast<std::ptrdiff_t>(tag_bytes))};
    }

    [[nodiscard]] static constexpr bool secure_backend() {
#ifdef AURORA_USE_REAL_CRYPTO
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] static constexpr std::string_view profile() {
#ifdef AURORA_USE_REAL_CRYPTO
        return "hmac-sha256-libsodium";
#else
        return "insecure-test-placeholder-mac";
#endif
    }

private:
    static constexpr std::size_t tag_bytes = 32;

    [[nodiscard]] std::array<std::uint8_t, tag_bytes> authenticate(
        const std::vector<std::uint8_t>& bytes) const {
        std::array<std::uint8_t, tag_bytes> tag{};
#ifdef AURORA_USE_REAL_CRYPTO
        crypto_auth_hmacsha256_state state;
        crypto_auth_hmacsha256_init(&state, key_.data(), key_.size());
        crypto_auth_hmacsha256_update(&state, bytes.data(), bytes.size());
        crypto_auth_hmacsha256_final(&state, tag.data());
#else
        // Deterministic test oracle only. This is intentionally not described
        // as authentication unless the libsodium backend is enabled.
        for (std::size_t lane = 0; lane < 4; ++lane) {
            std::uint64_t hash = 14695981039346656037ULL ^
                (0x9e3779b97f4a7c15ULL * (lane + 1));
            for (auto byte : key_) { hash ^= byte; hash *= 1099511628211ULL; }
            for (auto byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
            for (int shift = 7; shift >= 0; --shift) {
                tag[lane * 8 + static_cast<std::size_t>(7 - shift)] =
                    static_cast<std::uint8_t>(hash >> (shift * 8));
            }
        }
#endif
        return tag;
    }

    static bool constant_time_equal(const std::uint8_t* left,
                                    const std::uint8_t* right,
                                    std::size_t size) {
#ifdef AURORA_USE_REAL_CRYPTO
        return sodium_memcmp(left, right, size) == 0;
#else
        std::uint8_t difference = 0;
        for (std::size_t index = 0; index < size; ++index) {
            difference |= left[index] ^ right[index];
        }
        return difference == 0;
#endif
    }

    static unsigned hex_nibble(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        throw std::invalid_argument(
            "process authentication: invalid hex digit");
    }

    static void append_u32(std::vector<std::uint8_t>& out,
                           std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    static void append_u64(std::vector<std::uint8_t>& out,
                           std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    static std::uint32_t read_u32(const std::vector<std::uint8_t>& in,
                                  std::size_t& offset) {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            value = (value << 8) | in[offset++];
        }
        return value;
    }
    static std::uint64_t read_u64(const std::vector<std::uint8_t>& in,
                                  std::size_t& offset) {
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index) {
            value = (value << 8) | in[offset++];
        }
        return value;
    }

    Key key_{};
    std::uint64_t session_id_ = 0;
};

} // namespace aurora::emulation

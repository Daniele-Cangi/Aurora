#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The wire-level namespace is intentionally retained for the existing simulator.
// This remains an experimental LT-like codec, not a standards-compliant LT/Raptor code.
namespace fec {

struct Fp {
    std::uint32_t seed = 0;
    std::uint32_t deg = 0;
    std::vector<std::uint8_t> data;
};

enum class SegmentKind : std::uint8_t {
    CRITICAL,
    BULK
};

struct Pkt {
    Fp fp;
    std::uint32_t seq = 0;
    std::string token_id;
    SegmentKind kind = SegmentKind::BULK;
    std::string generation_id;
    std::uint32_t segment_id = 0;
    std::uint64_t descriptor_fingerprint = 0;
};

namespace detail {

constexpr std::uint32_t systematic_flag = 0x80000000U;

inline std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

inline std::uint32_t repair_seed(std::uint64_t generation_seed, std::uint64_t symbol_number) {
    return static_cast<std::uint32_t>(
        splitmix64(generation_seed ^ (symbol_number * 0xD1B54A32D192ED03ULL))) & ~systematic_flag;
}

inline std::uint32_t repair_degree(std::uint32_t source_count, std::uint32_t seed) {
    if (source_count <= 1) {
        return source_count;
    }

    // Ideal-soliton degree distribution. It is deterministic from the packet seed
    // and intentionally described as experimental rather than standards compliant.
    std::mt19937 random(seed ^ 0x6A09E667U);
    const double u = std::generate_canonical<double, 53>(random);
    double cumulative = 1.0 / static_cast<double>(source_count);
    if (u < cumulative) {
        return 1;
    }
    for (std::uint32_t degree = 2; degree <= source_count; ++degree) {
        cumulative += 1.0 / (static_cast<double>(degree) * static_cast<double>(degree - 1));
        if (u < cumulative || degree == source_count) {
            return degree;
        }
    }
    return source_count;
}

inline std::optional<std::vector<std::uint32_t>> source_indexes(
    std::uint32_t source_count,
    std::uint32_t seed,
    std::uint32_t degree) {
    if (source_count == 0 || degree == 0 || degree > source_count) {
        return std::nullopt;
    }
    if ((seed & systematic_flag) != 0U) {
        const auto index = seed & ~systematic_flag;
        if (degree != 1 || index >= source_count) {
            return std::nullopt;
        }
        return std::vector<std::uint32_t>{index};
    }
    if (degree != repair_degree(source_count, seed)) {
        return std::nullopt;
    }

    std::vector<std::uint32_t> indexes(source_count);
    std::iota(indexes.begin(), indexes.end(), 0U);
    std::mt19937 random(seed);
    for (std::uint32_t i = 0; i < degree; ++i) {
        std::uniform_int_distribution<std::uint32_t> choose(i, source_count - 1);
        const auto selected = choose(random);
        std::swap(indexes[i], indexes[selected]);
    }
    indexes.resize(degree);
    return indexes;
}

inline bool any_nonzero(const std::vector<std::uint8_t>& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value != 0; });
}

} // namespace detail

class Encoder {
public:
    Encoder(const std::vector<std::uint8_t>& bytes, std::size_t symbol_size = 256,
            std::uint64_t generation_seed = 0xC0FFEEBEEFULL)
        : symbol_size_(symbol_size), generation_seed_(generation_seed) {
        if (symbol_size_ == 0) {
            throw std::invalid_argument("LT-like encoder: symbol size must be positive");
        }
        const std::size_t count = (bytes.size() + symbol_size_ - 1) / symbol_size_;
        symbols_.assign(count, std::vector<std::uint8_t>(symbol_size_, 0));
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            symbols_[i / symbol_size_][i % symbol_size_] = bytes[i];
        }
    }

    [[nodiscard]] int N() const {
        return static_cast<int>(symbols_.size());
    }

    Fp emit() {
        const auto source_count = static_cast<std::uint32_t>(symbols_.size());
        if (source_count == 0) {
            throw std::logic_error("LT-like encoder: cannot emit a symbol for an empty generation");
        }

        std::uint32_t seed = 0;
        std::uint32_t degree = 0;
        if (next_symbol_ < source_count) {
            seed = detail::systematic_flag | static_cast<std::uint32_t>(next_symbol_);
            degree = 1;
        } else {
            seed = detail::repair_seed(generation_seed_, next_symbol_);
            degree = detail::repair_degree(source_count, seed);
        }
        ++next_symbol_;

        const auto indexes = detail::source_indexes(source_count, seed, degree);
        if (!indexes) {
            throw std::logic_error("LT-like encoder: failed to reconstruct its own source indexes");
        }
        std::vector<std::uint8_t> mixed(symbol_size_, 0);
        for (const auto index : *indexes) {
            for (std::size_t byte = 0; byte < symbol_size_; ++byte) {
                mixed[byte] ^= symbols_[index][byte];
            }
        }
        return Fp{seed, degree, std::move(mixed)};
    }

private:
    std::vector<std::vector<std::uint8_t>> symbols_;
    std::size_t symbol_size_ = 0;
    std::uint64_t generation_seed_ = 0;
    std::uint64_t next_symbol_ = 0;
};

enum class PushResult : std::uint8_t {
    INNOVATIVE,
    DEPENDENT,
    MALFORMED,
    INCONSISTENT
};

class Decoder {
public:
    Decoder(int source_count, std::size_t symbol_size)
        : source_count_(source_count), symbol_size_(symbol_size), basis_(validated_size(source_count)) {
        if (symbol_size_ == 0) {
            throw std::invalid_argument("LT-like decoder: symbol size must be positive");
        }
    }

    PushResult push(const Fp& packet) {
        if (packet.data.size() != symbol_size_) {
            ++malformed_count_;
            return PushResult::MALFORMED;
        }
        const auto indexes = detail::source_indexes(
            static_cast<std::uint32_t>(source_count_), packet.seed, packet.deg);
        if (!indexes) {
            ++malformed_count_;
            return PushResult::MALFORMED;
        }

        Equation candidate;
        candidate.coefficients.assign(static_cast<std::size_t>(source_count_), 0);
        candidate.payload = packet.data;
        for (const auto index : *indexes) {
            candidate.coefficients[index] = 1;
        }

        for (int pivot = 0; pivot < source_count_; ++pivot) {
            if (candidate.coefficients[static_cast<std::size_t>(pivot)] == 0) {
                continue;
            }
            if (!basis_[static_cast<std::size_t>(pivot)]) {
                basis_[static_cast<std::size_t>(pivot)] = std::move(candidate);
                ++rank_;
                return PushResult::INNOVATIVE;
            }
            xor_equation(candidate, *basis_[static_cast<std::size_t>(pivot)]);
        }

        if (detail::any_nonzero(candidate.payload)) {
            inconsistent_ = true;
            return PushResult::INCONSISTENT;
        }
        return PushResult::DEPENDENT;
    }

    [[nodiscard]] int rank() const {
        return rank_;
    }

    [[nodiscard]] int malformed_count() const {
        return malformed_count_;
    }

    [[nodiscard]] bool inconsistent() const {
        return inconsistent_;
    }

    [[nodiscard]] std::pair<bool, std::vector<std::uint8_t>> solve() const {
        if (rank_ != source_count_ || inconsistent_) {
            return {false, {}};
        }
        std::vector<std::vector<std::uint8_t>> decoded(
            static_cast<std::size_t>(source_count_), std::vector<std::uint8_t>(symbol_size_, 0));
        for (int pivot = source_count_ - 1; pivot >= 0; --pivot) {
            if (!basis_[static_cast<std::size_t>(pivot)]) {
                return {false, {}};
            }
            auto value = basis_[static_cast<std::size_t>(pivot)]->payload;
            const auto& coefficients = basis_[static_cast<std::size_t>(pivot)]->coefficients;
            for (int column = pivot + 1; column < source_count_; ++column) {
                if (coefficients[static_cast<std::size_t>(column)] == 0) {
                    continue;
                }
                for (std::size_t byte = 0; byte < symbol_size_; ++byte) {
                    value[byte] ^= decoded[static_cast<std::size_t>(column)][byte];
                }
            }
            decoded[static_cast<std::size_t>(pivot)] = std::move(value);
        }

        std::vector<std::uint8_t> output;
        output.reserve(static_cast<std::size_t>(source_count_) * symbol_size_);
        for (const auto& symbol : decoded) {
            output.insert(output.end(), symbol.begin(), symbol.end());
        }
        return {true, std::move(output)};
    }

private:
    struct Equation {
        std::vector<std::uint8_t> coefficients;
        std::vector<std::uint8_t> payload;
    };

    static std::size_t validated_size(int source_count) {
        if (source_count <= 0) {
            throw std::invalid_argument("LT-like decoder: source count must be positive");
        }
        return static_cast<std::size_t>(source_count);
    }

    static void xor_equation(Equation& target, const Equation& source) {
        for (std::size_t i = 0; i < target.coefficients.size(); ++i) {
            target.coefficients[i] ^= source.coefficients[i];
        }
        for (std::size_t i = 0; i < target.payload.size(); ++i) {
            target.payload[i] ^= source.payload[i];
        }
    }

    int source_count_ = 0;
    std::size_t symbol_size_ = 0;
    std::vector<std::optional<Equation>> basis_;
    int rank_ = 0;
    int malformed_count_ = 0;
    bool inconsistent_ = false;
};

} // namespace fec

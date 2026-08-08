#pragma once

#include "LtLikeCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aurora::fec {

class SymbolEncoder {
public:
    virtual ~SymbolEncoder() = default;
    [[nodiscard]] virtual int source_symbol_count() const = 0;
    virtual ::fec::Fp emit() = 0;
};

class SymbolDecoder {
public:
    virtual ~SymbolDecoder() = default;
    virtual ::fec::PushResult push(const ::fec::Fp& packet) = 0;
    [[nodiscard]] virtual int rank() const = 0;
    [[nodiscard]] virtual bool inconsistent() const = 0;
    [[nodiscard]] virtual std::pair<bool, std::vector<std::uint8_t>> solve() const = 0;
};

class GenerationCodec {
public:
    virtual ~GenerationCodec() = default;
    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::uint16_t version() const = 0;
    [[nodiscard]] virtual std::unique_ptr<SymbolEncoder> make_encoder(
        const std::vector<std::uint8_t>& bytes,
        std::size_t symbol_size,
        std::uint64_t generation_seed) const = 0;
    [[nodiscard]] virtual std::unique_ptr<SymbolDecoder> make_decoder(
        std::uint32_t source_symbol_count,
        std::size_t symbol_size) const = 0;
};

class LtLikeSymbolEncoder final : public SymbolEncoder {
public:
    LtLikeSymbolEncoder(const std::vector<std::uint8_t>& bytes,
                        std::size_t symbol_size,
                        std::uint64_t generation_seed)
        : encoder_(bytes, symbol_size, generation_seed) {}

    [[nodiscard]] int source_symbol_count() const override { return encoder_.N(); }
    ::fec::Fp emit() override { return encoder_.emit(); }

private:
    ::fec::Encoder encoder_;
};

class LtLikeSymbolDecoder final : public SymbolDecoder {
public:
    LtLikeSymbolDecoder(std::uint32_t source_symbol_count, std::size_t symbol_size)
        : decoder_(static_cast<int>(source_symbol_count), symbol_size) {}

    ::fec::PushResult push(const ::fec::Fp& packet) override { return decoder_.push(packet); }
    [[nodiscard]] int rank() const override { return decoder_.rank(); }
    [[nodiscard]] bool inconsistent() const override { return decoder_.inconsistent(); }
    [[nodiscard]] std::pair<bool, std::vector<std::uint8_t>> solve() const override {
        return decoder_.solve();
    }

private:
    ::fec::Decoder decoder_;
};

class ExperimentalLtLikeCodec final : public GenerationCodec {
public:
    [[nodiscard]] std::string id() const override { return "experimental-lt-like"; }
    [[nodiscard]] std::uint16_t version() const override { return 1; }

    [[nodiscard]] std::unique_ptr<SymbolEncoder> make_encoder(
        const std::vector<std::uint8_t>& bytes,
        std::size_t symbol_size,
        std::uint64_t generation_seed) const override {
        return std::make_unique<LtLikeSymbolEncoder>(bytes, symbol_size, generation_seed);
    }

    [[nodiscard]] std::unique_ptr<SymbolDecoder> make_decoder(
        std::uint32_t source_symbol_count,
        std::size_t symbol_size) const override {
        return std::make_unique<LtLikeSymbolDecoder>(source_symbol_count, symbol_size);
    }
};

} // namespace aurora::fec

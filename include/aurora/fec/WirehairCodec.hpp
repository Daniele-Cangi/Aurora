#pragma once

#include "GenerationCodec.hpp"

namespace aurora::fec {

class WirehairCodec final : public GenerationCodec {
public:
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] std::uint16_t version() const override;
    [[nodiscard]] std::unique_ptr<SymbolEncoder> make_encoder(
        const std::vector<std::uint8_t>& bytes,
        std::size_t symbol_size,
        std::uint64_t generation_seed) const override;
    [[nodiscard]] std::unique_ptr<SymbolDecoder> make_decoder(
        std::uint32_t source_symbol_count,
        std::size_t symbol_size,
        std::size_t source_bytes) const override;
};

} // namespace aurora::fec

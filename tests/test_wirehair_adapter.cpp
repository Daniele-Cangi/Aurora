#include "aurora/fec/WirehairCodec.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    constexpr std::size_t symbol_size = 64;
    std::vector<std::uint8_t> source(symbol_size * 5 + 17);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::uint8_t>((index * 37 + 11) & 0xffU);
    }

    aurora::fec::WirehairCodec codec;
    assert(codec.id() == "wirehair-legacy-fixups-2026-07");
    assert(codec.version() == 1);

    auto encoder = codec.make_encoder(source, symbol_size, 1234);
    assert(encoder->source_symbol_count() == 6);
    auto decoder = codec.make_decoder(6, symbol_size, source.size());

    std::vector<::fec::Fp> packets;
    for (int index = 0; index < 18; ++index) packets.push_back(encoder->emit());
    assert(packets[5].data.size() == symbol_size);

    const std::vector<std::size_t> delivery_order{
        0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    for (const auto index : delivery_order) {
        assert(decoder->push(packets[index]) == ::fec::PushResult::INNOVATIVE);
        if (decoder->rank() == encoder->source_symbol_count()) break;
    }
    assert(decoder->rank() == encoder->source_symbol_count());
    const auto [ok, recovered] = decoder->solve();
    assert(ok);
    assert(recovered == source);

    auto duplicate_decoder = codec.make_decoder(6, symbol_size, source.size());
    assert(duplicate_decoder->push(packets[0]) == ::fec::PushResult::INNOVATIVE);
    assert(duplicate_decoder->push(packets[0]) == ::fec::PushResult::DEPENDENT);
    auto conflict = packets[0];
    conflict.data[0] ^= 0xffU;
    assert(duplicate_decoder->push(conflict) == ::fec::PushResult::INCONSISTENT);
    assert(duplicate_decoder->inconsistent());

    auto malformed_decoder = codec.make_decoder(6, symbol_size, source.size());
    auto malformed = packets[0];
    malformed.deg = 1;
    assert(malformed_decoder->push(malformed) == ::fec::PushResult::MALFORMED);

    std::cout << "Wirehair adapter tests passed\n";
    return 0;
}

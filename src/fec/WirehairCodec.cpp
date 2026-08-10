#include "aurora/fec/WirehairCodec.hpp"

#include <wirehair/wirehair.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace aurora::fec {
namespace {

[[noreturn]] void fail(const char* operation, WirehairResult result) {
    throw std::runtime_error(std::string("Wirehair ") + operation + ": " +
                             wirehair_result_string(result));
}

void require_success(const char* operation, WirehairResult result) {
    if (result != Wirehair_Success) fail(operation, result);
}

WirehairWireProfile frozen_profile() {
    WirehairWireProfile profile{};
    require_success("profile initialization",
                    wirehair_wire_profile_init(
                        WIREHAIR_LEGACY_PROFILE_FIXUPS_2026_07, &profile));
    return profile;
}

void initialize_library() {
    static const WirehairResult result = wirehair_init();
    require_success("library initialization", result);
}

void validate_dimensions(std::uint32_t source_symbol_count,
                         std::size_t symbol_size,
                         std::size_t source_bytes) {
    if (source_symbol_count < 2) {
        throw std::invalid_argument("Wirehair requires at least two source symbols");
    }
    if (symbol_size == 0 ||
        symbol_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Wirehair symbol size is out of range");
    }
    if (source_bytes == 0 ||
        (source_bytes + symbol_size - 1) / symbol_size != source_symbol_count) {
        throw std::invalid_argument(
            "Wirehair source dimensions do not match the generation descriptor");
    }
}

class WirehairSymbolEncoder final : public SymbolEncoder {
public:
    WirehairSymbolEncoder(const std::vector<std::uint8_t>& bytes,
                          std::size_t symbol_size)
        : source_symbol_count_(static_cast<std::uint32_t>(
              (bytes.size() + symbol_size - 1) / symbol_size)),
          symbol_size_(static_cast<std::uint32_t>(symbol_size)) {
        validate_dimensions(source_symbol_count_, symbol_size, bytes.size());
        initialize_library();
        const auto profile = frozen_profile();
        require_success("encoder creation",
                        wirehair_encoder_create_profile_ex(
                            nullptr, bytes.data(), bytes.size(), symbol_size_,
                            &profile, WIREHAIR_ENCODER_OWN_INPUT, &codec_));
    }

    ~WirehairSymbolEncoder() override { wirehair_free(codec_); }

    [[nodiscard]] int source_symbol_count() const override {
        return static_cast<int>(source_symbol_count_);
    }

    ::fec::Fp emit() override {
        if (next_block_id_ == std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("Wirehair block identifier exhausted");
        }
        std::vector<std::uint8_t> data(symbol_size_);
        std::uint32_t bytes_out = 0;
        require_success("encode",
                        wirehair_encode(codec_, next_block_id_, data.data(),
                                        symbol_size_, &bytes_out));
        if (bytes_out == 0 || bytes_out > symbol_size_) {
            throw std::runtime_error("Wirehair encode returned an invalid size");
        }
        return ::fec::Fp{next_block_id_++, 0, std::move(data)};
    }

private:
    ::WirehairCodec codec_ = nullptr;
    std::uint32_t source_symbol_count_ = 0;
    std::uint32_t symbol_size_ = 0;
    std::uint32_t next_block_id_ = 0;
};

class WirehairSymbolDecoder final : public SymbolDecoder {
public:
    WirehairSymbolDecoder(std::uint32_t source_symbol_count,
                          std::size_t symbol_size,
                          std::size_t source_bytes)
        : source_symbol_count_(source_symbol_count),
          symbol_size_(static_cast<std::uint32_t>(symbol_size)),
          source_bytes_(source_bytes) {
        validate_dimensions(source_symbol_count, symbol_size, source_bytes);
        initialize_library();
        const auto profile = frozen_profile();
        require_success("decoder creation",
                        wirehair_decoder_create_profile_ex(
                            nullptr, source_bytes_, symbol_size_, &profile,
                            &codec_));
    }

    ~WirehairSymbolDecoder() override { wirehair_free(codec_); }

    ::fec::PushResult push(const ::fec::Fp& packet) override {
        if (packet.deg != 0 || packet.data.empty() ||
            packet.data.size() > symbol_size_) {
            return ::fec::PushResult::MALFORMED;
        }

        const auto found = accepted_.find(packet.seed);
        if (found != accepted_.end()) {
            if (found->second == packet.data) return ::fec::PushResult::DEPENDENT;
            inconsistent_ = true;
            return ::fec::PushResult::INCONSISTENT;
        }

        const auto result = wirehair_decode(
            codec_, packet.seed, packet.data.data(),
            static_cast<std::uint32_t>(packet.data.size()));
        if (result == Wirehair_InvalidInput ||
            result == Wirehair_BadInput_SmallN ||
            result == Wirehair_BadInput_LargeN) {
            return ::fec::PushResult::MALFORMED;
        }
        if (result != Wirehair_NeedMore && result != Wirehair_Success) {
            fail("decode", result);
        }

        accepted_.emplace(packet.seed, packet.data);
        if (result == Wirehair_Success && !complete_) {
            recovered_.resize(source_bytes_);
            require_success("recover",
                            wirehair_recover(codec_, recovered_.data(),
                                             source_bytes_));
            complete_ = true;
        }
        return ::fec::PushResult::INNOVATIVE;
    }

    [[nodiscard]] int rank() const override {
        if (complete_) return static_cast<int>(source_symbol_count_);
        return static_cast<int>(std::min<std::size_t>(
            accepted_.size(), source_symbol_count_ - 1));
    }

    [[nodiscard]] bool inconsistent() const override { return inconsistent_; }

    [[nodiscard]] std::pair<bool, std::vector<std::uint8_t>> solve() const override {
        if (!complete_ || inconsistent_) return {false, {}};
        return {true, recovered_};
    }

private:
    ::WirehairCodec codec_ = nullptr;
    std::uint32_t source_symbol_count_ = 0;
    std::uint32_t symbol_size_ = 0;
    std::size_t source_bytes_ = 0;
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> accepted_;
    std::vector<std::uint8_t> recovered_;
    bool complete_ = false;
    bool inconsistent_ = false;
};

} // namespace

std::string WirehairCodec::id() const {
    return "wirehair-legacy-fixups-2026-07";
}

std::uint16_t WirehairCodec::version() const { return 1; }

std::unique_ptr<SymbolEncoder> WirehairCodec::make_encoder(
    const std::vector<std::uint8_t>& bytes,
    std::size_t symbol_size,
    std::uint64_t generation_seed) const {
    (void)generation_seed;
    return std::make_unique<WirehairSymbolEncoder>(bytes, symbol_size);
}

std::unique_ptr<SymbolDecoder> WirehairCodec::make_decoder(
    std::uint32_t source_symbol_count,
    std::size_t symbol_size,
    std::size_t source_bytes) const {
    return std::make_unique<WirehairSymbolDecoder>(
        source_symbol_count, symbol_size, source_bytes);
}

} // namespace aurora::fec

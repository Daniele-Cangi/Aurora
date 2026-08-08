#include "../aurora_organism.hpp"
#include "../include/aurora/control/TransportPolicy.hpp"
#include "../include/aurora/fec/GenerationCodec.hpp"
#include "../include/aurora/transport/GenerationManager.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class TaggedCodec final : public aurora::fec::GenerationCodec {
public:
    [[nodiscard]] std::string id() const override { return "test-codec"; }
    [[nodiscard]] std::uint16_t version() const override { return 7; }

    [[nodiscard]] std::unique_ptr<aurora::fec::SymbolEncoder> make_encoder(
        const std::vector<std::uint8_t>& bytes,
        std::size_t symbol_size,
        std::uint64_t generation_seed) const override {
        return delegate_.make_encoder(bytes, symbol_size, generation_seed);
    }

    [[nodiscard]] std::unique_ptr<aurora::fec::SymbolDecoder> make_decoder(
        std::uint32_t source_symbol_count,
        std::size_t symbol_size) const override {
        return delegate_.make_decoder(source_symbol_count, symbol_size);
    }

private:
    aurora::fec::ExperimentalLtLikeCodec delegate_;
};

class RecordingPolicy final : public aurora::control::TransportPolicy {
public:
    [[nodiscard]] std::string id() const override { return "recording-policy"; }
    [[nodiscard]] std::uint16_t version() const override { return 3; }

    [[nodiscard]] aurora::control::FlowProfile profile_for(
        const aurora::transport::TransportContract& contract) const override {
        return aurora::control::transport_profile_for(contract);
    }

    aurora::control::ProtectionPlan plan(
        const aurora::transport::TransportContract& contract) override {
        aurora::control::ProtectionPlan result;
        result.policy_id = id();
        result.policy_version = version();
        result.profile = profile_for(contract);
        result.critical_overhead = 2.0;
        result.important_overhead = 1.25;
        result.elastic_overhead = 1.0;
        return result;
    }

    void observe(const aurora::control::FlowProfile&,
                 const aurora::transport::DecodeReport& report) override {
        ++observations;
        last_status = report.status;
    }

    int observations = 0;
    aurora::transport::DecodeStatus last_status =
        aurora::transport::DecodeStatus::NO_PROGRESS;
};

} // namespace

int main() {
    auto codec = std::make_shared<TaggedCodec>();
    auto policy = std::make_shared<RecordingPolicy>();
    aurora::transport::GenerationManager manager(codec, policy, 2);
    const auto contract = aurora::transport::TransportContract::parse(
        "deadline:100ms;importance:important;seed:92");
    const std::vector<std::uint8_t> bytes(129, 0xA4);

    const auto generated = manager.spawn(contract, "modular", bytes, 32, 0);
    assert(generated.descriptor.codec_id == "test-codec");
    assert(generated.descriptor.codec_version == 7);
    assert(generated.descriptor.policy_id == "recording-policy");
    assert(generated.descriptor.policy_version == 3);
    assert(generated.K == generated.source_symbol_count);

    const auto complete = manager.integrate(
        generated.descriptor.generation_id, generated.packets, 10);
    assert(complete.delivered());
    assert(complete.payload == bytes);
    assert(policy->observations == 1);
    assert(policy->last_status == aurora::transport::DecodeStatus::COMPLETE);

    const auto expiring = manager.spawn(contract, "modular-expired", bytes, 32, 0);
    const auto expired = manager.integrate(
        expiring.descriptor.generation_id, {}, 101);
    assert(expired.status == aurora::transport::DecodeStatus::EXPIRED);
    assert(policy->observations == 2);

    aurora::AlienFountainOrganism compatibility;
    const auto legacy = compatibility.spawn(contract, "compatibility", bytes, 32, 0);
    assert(legacy.descriptor.codec_id == "experimental-lt-like");
    assert(legacy.descriptor.policy_id == "biological-adaptive");

    std::cout << "modular transport tests passed\n";
    return 0;
}

#include "../include/aurora/transport/TransportContract.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using aurora::transport::TransportContract;
using aurora::transport::ContractFieldSemantics;
using aurora::transport::TransportImportance;

namespace {

template <typename Function>
void expect_invalid(Function&& function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

} // namespace

int main() {
    const auto legacy = TransportContract::parse(
        "deadline:600; reliability:0.99; duty:0.01; optical:on; "
        "backscatter:off; ris:8; selector:ucb; seed:0x2A");
    assert(legacy.deadline_ms() == 600'000);
    assert(std::abs(legacy.reliability - 0.99) < 1e-12);
    assert(std::abs(legacy.duty_frac - 0.01) < 1e-12);
    assert(legacy.allow_optical);
    assert(!legacy.allow_backscatter);
    assert(legacy.ris_tiles == 8);
    assert(!legacy.selector_argmax);
    assert(legacy.experiment_seed == 42);

    const auto segmented = TransportContract::parse(
        "deadline:2s;importance:elastic;reserve_floor:0.2;max_observation_age:250ms;"
        "max_repair_amplification:3;min_critical_overhead:2;max_source_symbols:512;"
        "segment:0-191,critical,100ms,0.999;"
        "segment:192-255,important,800ms,0.99");
    assert(segmented.importance == TransportImportance::ELASTIC);
    assert(segmented.minimum_source_reserve == 0.2);
    assert(segmented.maximum_observation_age_ms == 250);
    assert(segmented.maximum_source_symbols == 512);
    assert(segmented.segments.size() == 2);
    assert(segmented.segments[0].offset == 0);
    assert(segmented.segments[0].length == 192);
    assert(segmented.segments[0].importance == TransportImportance::CRITICAL);
    assert(segmented.segments[0].deadline_ms == 100);

    expect_invalid([] { TransportContract::parse("deadline:abc"); });
    expect_invalid([] { TransportContract::parse("reliability:1.5"); });
    expect_invalid([] { TransportContract::parse("rf:off;optical:off;backscatter:off"); });
    expect_invalid([] { TransportContract::parse("semantic_value:secret"); });
    expect_invalid([] {
        TransportContract::parse("segment:0-10,critical;segment:10-20,important");
    });
    expect_invalid([] {
        TransportContract unsupported;
        unsupported.version = 2;
        unsupported.validate();
    });

    const auto audit_for = [](std::string_view field) {
        return std::find_if(
            aurora::transport::transport_contract_semantic_audit.begin(),
            aurora::transport::transport_contract_semantic_audit.end(),
            [&](const auto& item) { return item.field == field; });
    };
    const auto segment_deadline = audit_for("segments.deadline_ms");
    const auto segment_reliability = audit_for("segments.target_reliability");
    const auto global_deadline = audit_for("deadline_s");
    const auto duty = audit_for("duty_frac");
    assert(segment_deadline !=
           aurora::transport::transport_contract_semantic_audit.end());
    assert(segment_reliability !=
           aurora::transport::transport_contract_semantic_audit.end());
    assert(global_deadline !=
           aurora::transport::transport_contract_semantic_audit.end());
    assert(duty != aurora::transport::transport_contract_semantic_audit.end());
    assert(segment_deadline->semantics == ContractFieldSemantics::METADATA_ONLY);
    assert(segment_reliability->semantics == ContractFieldSemantics::METADATA_ONLY);
    assert(global_deadline->semantics == ContractFieldSemantics::ENFORCED);
    assert(duty->semantics == ContractFieldSemantics::ENFORCED);

    std::cout << "transport contract tests passed\n";
    return 0;
}

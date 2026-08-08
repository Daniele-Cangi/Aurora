#include "../include/aurora/simulation/ChannelTrace.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    using namespace aurora::simulation;

    ChannelTraceGenerator generator;
    ChannelScenario iid;
    iid.iid_loss_rate = 0.30;
    const auto first = generator.generate(iid, 77, 0, 128);
    const auto repeated = generator.generate(iid, 77, 0, 128);
    const auto other_seed = generator.generate(iid, 78, 0, 128);
    assert(first == repeated);
    assert(first.outcomes != other_seed.outcomes);

    for (const auto kind : {
             ChannelScenarioKind::IID,
             ChannelScenarioKind::GILBERT_ELLIOTT,
             ChannelScenarioKind::SCHEDULED_OUTAGE,
             ChannelScenarioKind::SLOW_DRIFT,
             ChannelScenarioKind::SHOCK_RECOVERY}) {
        ChannelScenario scenario;
        scenario.kind = kind;
        const auto identifier = channel_scenario_id(scenario);
        assert(channel_scenario_from_id(identifier) == scenario);
    }

    ChannelScenario outage;
    outage.kind = ChannelScenarioKind::SCHEDULED_OUTAGE;
    outage.outage_base_loss_rate = 0.0;
    outage.outage_start_fraction = 0.25;
    outage.outage_duration_fraction = 0.25;
    const auto outage_trace = generator.generate(outage, 9, 0, 20);
    for (std::size_t slot = 0; slot < outage_trace.outcomes.size(); ++slot) {
        const bool should_deliver = slot < 5 || slot >= 10;
        assert(outage_trace.delivered(slot) == should_deliver);
    }

    ChannelTraceCorpus corpus;
    corpus.experiment_seed = 77;
    corpus.scenario_id = channel_scenario_id(iid);
    for (std::size_t trial = 0; trial < 3; ++trial) {
        corpus.traces.push_back(generator.generate(iid, 77, trial, 128));
    }
    const auto encoded = corpus.serialize();
    const auto decoded = ChannelTraceCorpus::deserialize(encoded);
    assert(decoded == corpus);
    assert(decoded.serialize() == encoded);
    assert(decoded.fingerprint() == corpus.fingerprint());

    auto corrupted = encoded;
    const auto trace_record = corrupted.find("T|");
    assert(trace_record != std::string::npos);
    corrupted[trace_record + 2] = '9';
    bool corruption_rejected = false;
    try {
        (void)ChannelTraceCorpus::deserialize(corrupted);
    } catch (const std::invalid_argument&) {
        corruption_rejected = true;
    }
    assert(corruption_rejected);

    auto corrupted_header = encoded;
    const auto seed_position = corrupted_header.find('|') + 1;
    assert(seed_position != 0);
    corrupted_header[seed_position] = corrupted_header[seed_position] == '0' ? '1' : '0';
    bool header_corruption_rejected = false;
    try {
        (void)ChannelTraceCorpus::deserialize(corrupted_header);
    } catch (const std::invalid_argument&) {
        header_corruption_rejected = true;
    }
    assert(header_corruption_rejected);

    const auto footer = encoded.rfind("END|");
    assert(footer != std::string::npos);
    bool truncation_rejected = false;
    try {
        (void)ChannelTraceCorpus::deserialize(encoded.substr(0, footer));
    } catch (const std::invalid_argument&) {
        truncation_rejected = true;
    }
    assert(truncation_rejected);

    std::cout << "channel trace tests passed\n";
    return 0;
}

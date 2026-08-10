#include "aurora/emulation/ImpairmentTrace.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

int main() {
    using namespace aurora::emulation;
    const auto encoded = ImpairmentTrace::make("unit", "PDU");
    const auto trace = ImpairmentTrace::parse(encoded);
    assert(trace.name() == "unit");
    assert(trace.period() == 3);
    assert(trace.action(0) == ImpairmentAction::PASS);
    assert(trace.action(1) == ImpairmentAction::DROP);
    assert(trace.action(2) == ImpairmentAction::DUPLICATE);
    assert(trace.action(3) == ImpairmentAction::PASS);
    std::string crlf;
    for (char value : encoded) {
        if (value == '\n') crlf.push_back('\r');
        crlf.push_back(value);
    }
    assert(ImpairmentTrace::parse(crlf).fingerprint() == trace.fingerprint());
    const auto timed_encoded = ImpairmentTrace::make_timed(
        "timed", "P@40,D@0,U@5,P@0");
    const auto timed = ImpairmentTrace::parse(timed_encoded);
    assert(timed.version() == 2);
    assert(timed.period() == 4);
    assert(timed.directive(0).action == ImpairmentAction::PASS);
    assert(timed.directive(0).delay_ms == 40);
    assert(timed.directive(1).action == ImpairmentAction::DROP);
    assert(timed.directive(2).action == ImpairmentAction::DUPLICATE);
    assert(timed.directive(2).delay_ms == 5);
    assert(timed.directive(4).delay_ms == 40);
    auto corrupted = encoded;
    corrupted[corrupted.find("PDU")] = 'D';
    try { (void)ImpairmentTrace::parse(corrupted); assert(false); }
    catch (const std::invalid_argument&) {}
    try {
        (void)ImpairmentTrace::parse(
            ImpairmentTrace::make_timed("bad", "P@60001"));
        assert(false);
    } catch (const std::invalid_argument&) {}
}

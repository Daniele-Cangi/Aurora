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
    auto corrupted = encoded;
    corrupted[corrupted.find("PDU")] = 'D';
    try { (void)ImpairmentTrace::parse(corrupted); assert(false); }
    catch (const std::invalid_argument&) {}
}

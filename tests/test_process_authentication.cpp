#include "aurora/emulation/ProcessAuthentication.hpp"

#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
    using namespace aurora::emulation;
    ProcessAuthenticator::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    ProcessAuthenticator auth(key, 0x0123456789abcdefULL);
#ifdef AURORA_USE_REAL_CRYPTO
    static_assert(ProcessAuthenticator::secure_backend());
    assert(ProcessAuthenticator::profile() == "hmac-sha256-libsodium");
#else
    static_assert(!ProcessAuthenticator::secure_backend());
    assert(ProcessAuthenticator::profile() ==
           "insecure-test-placeholder-mac");
#endif
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
    const auto sealed = auth.seal(ProcessDirection::FORWARD, 9, payload);
    const auto opened = auth.open(ProcessDirection::FORWARD, sealed);
    assert(opened.sequence == 9);
    assert(opened.payload == payload);

    auto corrupted = sealed;
    corrupted[30] ^= 0x40;
    try { (void)auth.open(ProcessDirection::FORWARD, corrupted); assert(false); }
    catch (const std::invalid_argument&) {}
    try { (void)auth.open(ProcessDirection::REVERSE, sealed); assert(false); }
    catch (const std::invalid_argument&) {}

    ProcessAuthenticator wrong_session(key, 0xfedcba9876543210ULL);
    try { (void)wrong_session.open(ProcessDirection::FORWARD, sealed); assert(false); }
    catch (const std::invalid_argument&) {}
    auto wrong_key = key;
    wrong_key[0] ^= 0xff;
    ProcessAuthenticator wrong_key_auth(
        wrong_key, 0x0123456789abcdefULL);
    try { (void)wrong_key_auth.open(ProcessDirection::FORWARD, sealed); assert(false); }
    catch (const std::invalid_argument&) {}

    ReplayWindow window;
    assert(window.accept(5));
    assert(window.accept(7));
    assert(window.accept(6));
    assert(!window.accept(6));
    assert(!window.accept(5));
    assert(window.accept(80));
    assert(!window.accept(7));
}

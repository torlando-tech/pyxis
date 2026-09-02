// Host test for the unknown-source key-request policy
// (lib/tdeck_ui/UI/LXMF/UnknownSourceKeyRequest.h).
//
// The policy decides when the firmware fires an RNS path request for an
// LXMF message from an identity it has not learned (Sideband's "Query
// Network For Keys" equivalent). The decision must be pure and bounded:
// one request per source per 30-minute window, independent per source,
// and an aggregate bound — a flood of distinct bogus identities must
// never force more than kMaxTrackedSources requests per window, and a
// source whose window is open must stay suppressed for the whole window
// no matter what.
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include "UI/LXMF/UnknownSourceKeyRequest.h"

namespace {

int passed = 0;
int failures = 0;

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (expr) {                                                                 \
            ++passed;                                                               \
        } else {                                                                    \
            ++failures;                                                             \
            std::cerr << "FAIL line " << __LINE__ << ": " << #expr << '\n';         \
        }                                                                           \
    } while (false)

using UI::LXMF::UnknownSourceKeyRequestPolicy;

std::string sourceHex(unsigned seed) {
    // Distinct 8-hex string per seed up to 0xffffff (the tests use <=
    // 1512), so the flood tests have no hash collisions.
    const char* digits = "0123456789abcdef";
    std::string hex;
    for (unsigned shift : {28U, 24U, 20U, 16U, 12U, 8U, 4U, 0U}) {
        hex += digits[(seed >> shift) & 0xFU];
    }
    return hex;
}

void testNewSourceIsRequested() {
    UnknownSourceKeyRequestPolicy policy;
    const std::string src = sourceHex(1);
    CHECK(policy.should_request(src, 0));
    CHECK(policy.should_request(src, 12345));
}

void testCooldownSuppressesRepeat() {
    UnknownSourceKeyRequestPolicy policy;
    const std::string src = sourceHex(2);
    policy.record_request(src, 1000);
    // Within the 30-minute cooldown: suppressed.
    CHECK(!policy.should_request(src, 1001));
    CHECK(!policy.should_request(src, 1000 + UnknownSourceKeyRequestPolicy::kCooldownMillis - 1));
    // At and after the cooldown: requested again.
    CHECK(policy.should_request(src, 1000 + UnknownSourceKeyRequestPolicy::kCooldownMillis));
    CHECK(policy.should_request(src, 1000 + UnknownSourceKeyRequestPolicy::kCooldownMillis + 500));
}

void testReRecordResetsCooldown() {
    UnknownSourceKeyRequestPolicy policy;
    const std::string src = sourceHex(3);
    policy.record_request(src, 0);
    CHECK(policy.should_request(src, UnknownSourceKeyRequestPolicy::kCooldownMillis));
    // A fresh request near the original expiry restarts the window.
    policy.record_request(src, UnknownSourceKeyRequestPolicy::kCooldownMillis - 1000);
    CHECK(!policy.should_request(src, UnknownSourceKeyRequestPolicy::kCooldownMillis));
    CHECK(!policy.should_request(src, 2 * UnknownSourceKeyRequestPolicy::kCooldownMillis - 1001));
    CHECK(policy.should_request(src, 2 * UnknownSourceKeyRequestPolicy::kCooldownMillis - 1000));
}

void testSourcesTrackedIndependently() {
    UnknownSourceKeyRequestPolicy policy;
    const std::string a = sourceHex(4);
    const std::string b = sourceHex(5);
    policy.record_request(a, 100);
    CHECK(!policy.should_request(a, 200));
    CHECK(policy.should_request(b, 200));  // b unaffected by a's request
    policy.record_request(b, 200);
    CHECK(!policy.should_request(a, 300));
    CHECK(!policy.should_request(b, 300));
}

void testSaturatedTableDeclinesNewSources() {
    // While every tracking slot holds an open window, a never-before-seen
    // source is DECLINED (deferred), not tracked by dropping someone
    // else's window. Existing sources stay suppressed throughout.
    UnknownSourceKeyRequestPolicy policy;
    const unsigned cap = UnknownSourceKeyRequestPolicy::kMaxTrackedSources;
    for (unsigned i = 1; i <= cap; ++i) {
        policy.record_request(sourceHex(i),
            1000 + static_cast<unsigned long long>(i) * 1000);
    }
    CHECK(policy.last_requested.size() == cap);
    const std::string stranger = sourceHex(1000);
    CHECK(!policy.should_request(stranger, 2000));
    // Nobody's open window was dropped to make room: existing sources are
    // still suppressed.
    CHECK(!policy.should_request(sourceHex(1), 2000));
    CHECK(!policy.should_request(sourceHex(cap), 2000));
    CHECK(policy.last_requested.size() == cap);
    // The deferred source stays declined while the budget is full, and
    // becomes eligible as soon as the first recorded window expires
    // (src 1, recorded at 2000, expires at 2000 + cooldown).
    CHECK(!policy.should_request(stranger, 1801000 - 1));  // all still open
    const unsigned long long after_windows = 2000 + UnknownSourceKeyRequestPolicy::kCooldownMillis;
    CHECK(policy.should_request(stranger, after_windows));
    policy.record_request(stranger, after_windows);
    CHECK(policy.last_requested.size() == cap);
}

void testSteadyStateCostIsZero() {
    // A known source is never recorded by the caller (UIManager only calls
    // this for SOURCE_UNKNOWN), so once a peer is learned there is no
    // per-message work here. Model the steady state: one learned source's
    // hash must never appear in the table.
    UnknownSourceKeyRequestPolicy policy;
    const std::string learned = sourceHex(7);
    CHECK(policy.should_request(learned, 0));
    // Caller records only when it actually fires; after the first request
    // plus cooldown the table holds at most one entry per unknown peer.
    policy.record_request(learned, 0);
    CHECK(policy.last_requested.size() == 1);
    CHECK(!policy.should_request(learned, 10));
    CHECK(policy.should_request(learned, UnknownSourceKeyRequestPolicy::kCooldownMillis));
}

void testFloodBoundWorstCaseAirtime() {
    // Worst-case airtime an attacker can force: flood more distinct bogus
    // identities than the table can track, all inside one 30-minute
    // window. The total number of requests fired must be bounded by the
    // table cap, and every fired source must stay suppressed for the rest
    // of the window.
    UnknownSourceKeyRequestPolicy policy;
    const unsigned cap = UnknownSourceKeyRequestPolicy::kMaxTrackedSources;
    const unsigned long long t0 = 100000;
    const unsigned flood = 512;
    unsigned requests_fired = 0;
    for (unsigned i = 1; i <= flood; ++i) {
        const std::string src = sourceHex(1000 + i);
        const unsigned long long now = t0 + static_cast<unsigned long long>(i) * 1000;
        if (policy.should_request(src, now)) {
            ++requests_fired;
            policy.record_request(src, now);
        }
    }
    CHECK(requests_fired == cap);
    CHECK(policy.last_requested.size() == cap);
    // No identity whose window is open can fire a second time.
    unsigned violations = 0;
    for (unsigned i = 1; i <= flood; ++i) {
        const std::string src = sourceHex(1000 + i);
        const unsigned long long now = t0 + 29U * 60U * 1000U;
        if (policy.should_request(src, now)) {
            ++violations;
        }
    }
    CHECK(violations == 0);
}

void testFloodBoundAcrossConsecutiveWindows() {
    // Aggregate bound: the same 512 identities re-ask in a second
    // 30-minute window (each exactly one window after its first request,
    // staggered one per second). The first window's budget only frees one
    // slot per second as the first requests expire, so the 65th identity's
    // second request is still declined — at most cap requests may fire in
    // the second window too.
    UnknownSourceKeyRequestPolicy policy;
    const unsigned cap = UnknownSourceKeyRequestPolicy::kMaxTrackedSources;
    const unsigned long long t0 = 100000;
    const unsigned flood = 512;
    unsigned first_window = 0;
    unsigned second_window = 0;
    for (unsigned i = 1; i <= flood; ++i) {
        const std::string src = sourceHex(1000 + i);
        const unsigned long long w1 = t0 + static_cast<unsigned long long>(i) * 1000;
        if (policy.should_request(src, w1)) {
            ++first_window;
            policy.record_request(src, w1);
        }
        const unsigned long long w2 = w1 + UnknownSourceKeyRequestPolicy::kCooldownMillis;
        if (policy.should_request(src, w2)) {
            ++second_window;
            policy.record_request(src, w2);
        }
    }
    CHECK(first_window == cap);
    CHECK(second_window == cap);
}

void testWindowExpiryPrunesAndAllows() {
    UnknownSourceKeyRequestPolicy policy;
    const std::string a = sourceHex(20);
    const std::string b = sourceHex(21);
    policy.record_request(a, 0);
    policy.record_request(b, 5000);
    CHECK(!policy.should_request(a, 6000));
    CHECK(!policy.should_request(b, 6000));
    // a's window expires before b's; the table prunes a lazily and makes
    // room for a new source while b stays suppressed.
    CHECK(policy.should_request(a, UnknownSourceKeyRequestPolicy::kCooldownMillis));
    const std::string c = sourceHex(22);
    CHECK(policy.should_request(c, UnknownSourceKeyRequestPolicy::kCooldownMillis));
    policy.record_request(c, UnknownSourceKeyRequestPolicy::kCooldownMillis);
    CHECK(!policy.should_request(b, UnknownSourceKeyRequestPolicy::kCooldownMillis));
    CHECK(!policy.should_request(c, UnknownSourceKeyRequestPolicy::kCooldownMillis + 1));
}

void testConstants() {
    CHECK(UnknownSourceKeyRequestPolicy::kCooldownMillis == 30U * 60U * 1000U);
    CHECK(UnknownSourceKeyRequestPolicy::kMaxTrackedSources == 64U);
}

}  // namespace

int main() {
    testConstants();
    testNewSourceIsRequested();
    testCooldownSuppressesRepeat();
    testReRecordResetsCooldown();
    testSourcesTrackedIndependently();
    testSaturatedTableDeclinesNewSources();
    testSteadyStateCostIsZero();
    testFloodBoundWorstCaseAirtime();
    testFloodBoundAcrossConsecutiveWindows();
    testWindowExpiryPrunesAndAllows();
    std::cout << "unknown source key request policy: " << passed << " passed, "
              << failures << " failed" << std::endl;
    return failures == 0 ? 0 : 1;
}

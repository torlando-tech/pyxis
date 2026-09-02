// Host test for the unknown-source key-request policy
// (lib/tdeck_ui/UI/LXMF/UnknownSourceKeyRequest.h).
//
// The policy decides when the firmware fires an RNS path request for an
// LXMF message from an identity it has not learned (Sideband's "Query
// Network For Keys" equivalent). The decision must be pure and bounded:
// one request per source per cooldown, independent per source, and a
// capped tracking table so an opportunistic flood of unknown senders can
// neither hammer the transport nor starve the table forever.
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
    // Distinct 8-hex string per seed up to 0xffffff (the tests use <= 65),
    // so the capacity-cap test has no hash collisions.
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
    // Within the 5-minute cooldown: suppressed.
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

void testCapacityCapEvictsOldest() {
    UnknownSourceKeyRequestPolicy policy;
    const unsigned cap = UnknownSourceKeyRequestPolicy::kMaxTrackedSources;
    for (unsigned i = 0; i < cap; ++i) {
        policy.record_request(sourceHex(i + 1), 1000 + static_cast<unsigned long long>(i));
    }
    CHECK(policy.last_requested.size() == cap);
    // New source pushes the oldest (seed 1) out of the table.
    const std::string evicted = sourceHex(1);
    const std::string newest = sourceHex(cap + 1);
    policy.record_request(newest, 5000);
    CHECK(policy.last_requested.size() == cap);
    // The evicted source is no longer rate-limited (its entry is gone).
    CHECK(policy.should_request(evicted, 1010));
    // The most recent source IS rate-limited.
    CHECK(!policy.should_request(newest, 5001));
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

void testConstants() {
    CHECK(UnknownSourceKeyRequestPolicy::kCooldownMillis == 5U * 60U * 1000U);
    CHECK(UnknownSourceKeyRequestPolicy::kMaxTrackedSources == 64U);
}

}  // namespace

int main() {
    testConstants();
    testNewSourceIsRequested();
    testCooldownSuppressesRepeat();
    testReRecordResetsCooldown();
    testSourcesTrackedIndependently();
    testCapacityCapEvictsOldest();
    testSteadyStateCostIsZero();
    std::cout << "unknown source key request policy: " << passed << " passed, "
              << failures << " failed" << std::endl;
    return failures == 0 ? 0 : 1;
}

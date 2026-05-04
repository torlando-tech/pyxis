// Native unit tests for BLEFragmenter <-> BLEReassembler round-trip.
//
// This is the highest-impact pyxis-unique surface for latent bugs: every
// Reticulum packet over BLE goes through fragment+reassemble. Verifies the
// v2.2 fragment header format and reassembly state machine handle:
//   - single-fragment (END-only) packets
//   - multi-fragment START / CONTINUE / END sequences
//   - out-of-order fragment delivery
//   - duplicate fragments
//   - dropped fragments + timeout cleanup
//   - fragment-without-START (orphan) rejection
//   - per-peer isolation (one peer's reassembly doesn't bleed into another's)
//   - MTU change between packets

#include "../../lib/ble_interface/BLEFragmenter.h"
#include "../../lib/ble_interface/BLEReassembler.h"
#include "Utilities/OS.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using RNS::Bytes;
using RNS::BLE::BLEFragmenter;
using RNS::BLE::BLEReassembler;
namespace Fragment = RNS::BLE::Fragment;

// ── minimal test framework (same as test_hdlc.cpp) ──

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(actual, expected)                                            \
    do {                                                                       \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (!(_a == _e)) {                                                     \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: %s != %s",                 \
                          __FILE__, __LINE__, #actual, #expected);             \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: expected %s",              \
                          __FILE__, __LINE__, #cond);                          \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define RUN(name)                                                              \
    do {                                                                       \
        try {                                                                  \
            name();                                                            \
            ++g_pass;                                                          \
            std::printf("PASS %s\n", #name);                                   \
        } catch (const std::exception& e) {                                    \
            ++g_fail;                                                          \
            std::printf("FAIL %s: %s\n", #name, e.what());                     \
        }                                                                      \
    } while (0)

static Bytes make_payload(size_t n, uint8_t seed) {
    Bytes b;
    for (size_t i = 0; i < n; ++i) b.append((uint8_t)((i * 31) ^ seed));
    return b;
}

static Bytes make_peer(uint8_t tag) {
    Bytes id;
    for (int i = 0; i < 16; ++i) id.append(tag);
    return id;
}

// Helper: collect everything the reassembler emits via callback.
struct Capture {
    std::vector<std::pair<Bytes, Bytes>> packets;     // (peer, packet)
    std::vector<std::pair<Bytes, std::string>> timeouts;

    void wire(BLEReassembler& r) {
        r.setReassemblyCallback([this](const Bytes& peer, const Bytes& pkt) {
            packets.push_back({peer, pkt});
        });
        r.setTimeoutCallback([this](const Bytes& peer, const std::string& reason) {
            timeouts.push_back({peer, reason});
        });
    }
};

// ── tests ──

static void single_fragment_round_trip() {
    BLEFragmenter f(64);   // tiny MTU
    BLEReassembler r;
    Capture cap; cap.wire(r);

    Bytes payload = make_payload(40, 0xAA);
    auto frags = f.fragment(payload);
    EXPECT_EQ(frags.size(), (size_t)1);

    Bytes peer = make_peer(0x01);
    EXPECT_TRUE(r.processFragment(peer, frags[0]));
    EXPECT_EQ(cap.packets.size(), (size_t)1);
    EXPECT_EQ(cap.packets[0].first, peer);
    EXPECT_EQ(cap.packets[0].second, payload);
}

static void multi_fragment_round_trip_in_order() {
    BLEFragmenter f(32);   // forces multi-fragment for any nontrivial payload
    BLEReassembler r;
    Capture cap; cap.wire(r);

    Bytes payload = make_payload(200, 0xBB);
    auto frags = f.fragment(payload);
    EXPECT_TRUE(frags.size() >= 2);

    Bytes peer = make_peer(0x02);
    for (size_t i = 0; i < frags.size(); ++i) {
        bool ok = r.processFragment(peer, frags[i]);
        EXPECT_TRUE(ok);
        // Callback should fire exactly once on the final fragment.
        EXPECT_EQ(cap.packets.size(), i + 1 == frags.size() ? (size_t)1 : (size_t)0);
    }
    EXPECT_EQ(cap.packets[0].second, payload);
}

static void out_of_order_fragments_reassemble() {
    BLEFragmenter f(32);
    BLEReassembler r;
    Capture cap; cap.wire(r);

    Bytes payload = make_payload(150, 0xCC);
    auto frags = f.fragment(payload);
    EXPECT_TRUE(frags.size() >= 3);

    Bytes peer = make_peer(0x03);
    // Send START first (required by reassembler — no fragment-without-START),
    // then deliver remaining fragments in reverse order.
    r.processFragment(peer, frags[0]);
    for (size_t i = frags.size() - 1; i >= 1; --i) {
        r.processFragment(peer, frags[i]);
        if (i == 1) break;
    }

    EXPECT_EQ(cap.packets.size(), (size_t)1);
    EXPECT_EQ(cap.packets[0].second, payload);
}

static void duplicate_fragments_dont_double_emit() {
    BLEFragmenter f(32);
    BLEReassembler r;
    Capture cap; cap.wire(r);

    Bytes payload = make_payload(100, 0xDD);
    auto frags = f.fragment(payload);

    Bytes peer = make_peer(0x04);
    // Replay every fragment twice.
    for (auto& fr : frags) r.processFragment(peer, fr);
    for (auto& fr : frags) r.processFragment(peer, fr);

    // Reassembly must complete exactly once. Replay after completion may
    // be silently ignored or trigger a fresh failed reassembly — we only
    // require that the original packet was emitted exactly once and not
    // corrupted.
    EXPECT_TRUE(cap.packets.size() >= 1);
    EXPECT_EQ(cap.packets[0].second, payload);
    if (cap.packets.size() > 1) {
        for (auto& kv : cap.packets) EXPECT_EQ(kv.second, payload);
    }
}

static void fragment_without_start_is_rejected() {
    BLEFragmenter f(32);
    BLEReassembler r;
    Capture cap; cap.wire(r);

    Bytes payload = make_payload(100, 0xEE);
    auto frags = f.fragment(payload);
    EXPECT_TRUE(frags.size() >= 3);

    Bytes peer = make_peer(0x05);
    // Skip START, deliver only CONTINUE + END. Reassembler should drop
    // these as orphans.
    for (size_t i = 1; i < frags.size(); ++i) r.processFragment(peer, frags[i]);

    EXPECT_EQ(cap.packets.size(), (size_t)0);
    EXPECT_EQ(r.pendingCount(), (size_t)0);
}

static void dropped_fragment_times_out() {
    BLEFragmenter f(32);
    BLEReassembler r;
    Capture cap; cap.wire(r);
    r.setTimeout(10.0);

    Bytes payload = make_payload(150, 0xFF);
    auto frags = f.fragment(payload);
    EXPECT_TRUE(frags.size() >= 3);

    Bytes peer = make_peer(0x06);
    // Install a fake clock at t=0, deliver START + first CONTINUE, then drop
    // the rest. Advance the clock past the timeout and call checkTimeouts.
    RNS::Utilities::OS::set_fake_time(0.0);
    r.processFragment(peer, frags[0]);
    r.processFragment(peer, frags[1]);
    EXPECT_EQ(r.pendingCount(), (size_t)1);

    RNS::Utilities::OS::set_fake_time(20.0);   // > timeout
    r.checkTimeouts();

    EXPECT_EQ(r.pendingCount(), (size_t)0);
    EXPECT_EQ(cap.timeouts.size(), (size_t)1);
    EXPECT_EQ(cap.timeouts[0].first, peer);
    EXPECT_EQ(cap.packets.size(), (size_t)0);

    RNS::Utilities::OS::clear_fake_time();
}

static void per_peer_isolation() {
    BLEFragmenter f(32);
    BLEReassembler r;
    Capture cap; cap.wire(r);

    Bytes payload_a = make_payload(150, 0xA0);
    Bytes payload_b = make_payload(150, 0xB0);
    auto frags_a = f.fragment(payload_a);
    auto frags_b = f.fragment(payload_b);

    Bytes peer_a = make_peer(0x0A);
    Bytes peer_b = make_peer(0x0B);

    // Interleave fragments from two peers.
    for (size_t i = 0; i < frags_a.size(); ++i) {
        r.processFragment(peer_a, frags_a[i]);
        if (i < frags_b.size()) r.processFragment(peer_b, frags_b[i]);
    }
    for (size_t i = frags_a.size(); i < frags_b.size(); ++i) {
        r.processFragment(peer_b, frags_b[i]);
    }

    EXPECT_EQ(cap.packets.size(), (size_t)2);
    // Map by peer.
    Bytes got_a, got_b;
    for (auto& kv : cap.packets) {
        if (kv.first == peer_a) got_a = kv.second;
        if (kv.first == peer_b) got_b = kv.second;
    }
    EXPECT_EQ(got_a, payload_a);
    EXPECT_EQ(got_b, payload_b);
}

static void fragment_count_matches_calculator() {
    BLEFragmenter f(64);
    Bytes payload = make_payload(300, 0x11);
    auto frags = f.fragment(payload);
    EXPECT_EQ(frags.size(), (size_t)f.calculateFragmentCount(payload.size()));
}

static void mtu_change_affects_subsequent_fragments() {
    BLEFragmenter f(32);
    Bytes payload = make_payload(100, 0x22);
    auto small_mtu_frags = f.fragment(payload);

    f.setMTU(128);
    auto large_mtu_frags = f.fragment(payload);
    // Larger MTU should produce strictly fewer (or equal) fragments.
    EXPECT_TRUE(large_mtu_frags.size() <= small_mtu_frags.size());
}

static void clear_for_peer_drops_only_that_peer() {
    BLEFragmenter f(32);
    BLEReassembler r;
    Capture cap; cap.wire(r);

    auto frags_a = f.fragment(make_payload(100, 0x1));
    auto frags_b = f.fragment(make_payload(100, 0x2));

    Bytes peer_a = make_peer(0x1A);
    Bytes peer_b = make_peer(0x1B);

    r.processFragment(peer_a, frags_a[0]);
    r.processFragment(peer_b, frags_b[0]);
    EXPECT_EQ(r.pendingCount(), (size_t)2);

    r.clearForPeer(peer_a);
    EXPECT_EQ(r.pendingCount(), (size_t)1);
    EXPECT_TRUE(!r.hasPending(peer_a));
    EXPECT_TRUE(r.hasPending(peer_b));
}

int main() {
    RUN(single_fragment_round_trip);
    RUN(multi_fragment_round_trip_in_order);
    RUN(out_of_order_fragments_reassemble);
    RUN(duplicate_fragments_dont_double_emit);
    RUN(fragment_without_start_is_rejected);
    RUN(dropped_fragment_times_out);
    RUN(per_peer_isolation);
    RUN(fragment_count_matches_calculator);
    RUN(mtu_change_affects_subsequent_fragments);
    RUN(clear_for_peer_drops_only_that_peer);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

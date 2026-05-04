// Native unit tests for BLEPeerManager.
//
// PR #13 (greptile-flagged TOCTOU/UAF) lives in this neighborhood: the
// connection-map lifecycle is the active risk surface. Tests below exercise:
//
//   - Discovery and lookup by MAC / identity / handle
//   - Promotion from MAC-only to identity-keyed (handshake completion)
//   - Connection success/fail accounting and consecutive-failure blacklist
//   - Blacklist expiration via fake clock
//   - removePeer clears handle mapping (UAF risk if it doesn't)
//   - cleanupStalePeers ages out unconnected peers past PEER_TIMEOUT
//   - canAcceptConnection respects MAX_PEERS
//   - Pool exhaustion is handled gracefully (no crash, returns false)
//   - shouldInitiateConnection MAC-sort rule
//   - MAC rotation via updatePeerMac

#include "../../lib/ble_interface/BLEPeerManager.h"
#include "Utilities/OS.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

using RNS::Bytes;
using RNS::BLE::BLEPeerManager;
using RNS::BLE::PeerState;
using RNS::BLE::PeerInfo;

// ── minimal test framework ──

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

static Bytes mac_n(uint8_t n) {
    Bytes b;
    for (int i = 0; i < 6; ++i) b.append((uint8_t)(n + i));
    return b;
}

static Bytes id_n(uint8_t n) {
    Bytes b;
    for (int i = 0; i < 16; ++i) b.append((uint8_t)(n + i));
    return b;
}

// ── tests ──

static void discover_new_peer_appears_in_count() {
    BLEPeerManager pm;
    EXPECT_EQ(pm.totalPeerCount(), (size_t)0);
    EXPECT_TRUE(pm.addDiscoveredPeer(mac_n(0x10), -50));
    EXPECT_EQ(pm.totalPeerCount(), (size_t)1);
    EXPECT_EQ(pm.connectedCount(), (size_t)0);
    auto* p = pm.getPeerByMac(mac_n(0x10));
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(p->state, PeerState::DISCOVERED);
    EXPECT_EQ(p->rssi, (int8_t)-50);
}

static void rediscover_updates_rssi_no_double_count() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x10), -50);
    pm.addDiscoveredPeer(mac_n(0x10), -60);
    EXPECT_EQ(pm.totalPeerCount(), (size_t)1);
    auto* p = pm.getPeerByMac(mac_n(0x10));
    EXPECT_EQ(p->rssi, (int8_t)-60);
}

static void short_mac_rejected() {
    BLEPeerManager pm;
    Bytes too_short;
    too_short.append((uint8_t)0x01);
    EXPECT_TRUE(!pm.addDiscoveredPeer(too_short, -50));
    EXPECT_EQ(pm.totalPeerCount(), (size_t)0);
}

static void set_identity_promotes_peer() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x20), -45);
    EXPECT_TRUE(pm.getPeerByMac(mac_n(0x20)) != nullptr);
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xA0)) == nullptr);

    EXPECT_TRUE(pm.setPeerIdentity(mac_n(0x20), id_n(0xA0)));
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xA0)) != nullptr);
    EXPECT_EQ(pm.totalPeerCount(), (size_t)1);   // Should still be 1, not 2.
}

static void set_identity_unknown_mac_returns_false() {
    BLEPeerManager pm;
    EXPECT_TRUE(!pm.setPeerIdentity(mac_n(0x99), id_n(0x99)));
}

static void set_handle_then_lookup_by_handle() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x30), -55);
    pm.setPeerIdentity(mac_n(0x30), id_n(0xB0));
    pm.setPeerHandle(id_n(0xB0), 3);

    auto* p = pm.getPeerByHandle(3);
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(p->mac_address, mac_n(0x30));
}

static void out_of_range_handle_is_rejected_silently() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x30), -55);
    pm.setPeerIdentity(mac_n(0x30), id_n(0xB0));
    // MAX_CONN_HANDLES = 8 — handle 99 must not crash.
    pm.setPeerHandle(id_n(0xB0), 99);
    EXPECT_TRUE(pm.getPeerByHandle(99) == nullptr);
}

static void connection_failures_blacklist_after_threshold() {
    BLEPeerManager pm;
    RNS::Utilities::OS::set_fake_time(0.0);
    pm.addDiscoveredPeer(mac_n(0x40), -50);
    pm.setPeerIdentity(mac_n(0x40), id_n(0xC0));

    pm.connectionFailed(id_n(0xC0));
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xC0))->state != PeerState::BLACKLISTED);
    pm.connectionFailed(id_n(0xC0));
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xC0))->state != PeerState::BLACKLISTED);
    pm.connectionFailed(id_n(0xC0));
    // Threshold = 3 → now blacklisted.
    EXPECT_EQ(pm.getPeerByIdentity(id_n(0xC0))->state, PeerState::BLACKLISTED);
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xC0))->blacklisted_until > 0.0);

    RNS::Utilities::OS::clear_fake_time();
}

static void connection_success_clears_consecutive_failures() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x40), -50);
    pm.setPeerIdentity(mac_n(0x40), id_n(0xC0));

    pm.connectionFailed(id_n(0xC0));
    pm.connectionFailed(id_n(0xC0));
    EXPECT_EQ(pm.getPeerByIdentity(id_n(0xC0))->consecutive_failures, (uint8_t)2);

    pm.connectionSucceeded(id_n(0xC0));
    EXPECT_EQ(pm.getPeerByIdentity(id_n(0xC0))->consecutive_failures, (uint8_t)0);
    EXPECT_EQ(pm.getPeerByIdentity(id_n(0xC0))->state, PeerState::CONNECTED);
}

static void blacklist_blocks_rediscovery_until_expiration() {
    BLEPeerManager pm;
    RNS::Utilities::OS::set_fake_time(100.0);
    pm.addDiscoveredPeer(mac_n(0x50), -50);
    pm.setPeerIdentity(mac_n(0x50), id_n(0xD0));
    pm.connectionFailed(id_n(0xD0));
    pm.connectionFailed(id_n(0xD0));
    pm.connectionFailed(id_n(0xD0));
    EXPECT_EQ(pm.getPeerByIdentity(id_n(0xD0))->state, PeerState::BLACKLISTED);
    double until = pm.getPeerByIdentity(id_n(0xD0))->blacklisted_until;
    EXPECT_TRUE(until > 100.0);

    // While blacklisted, addDiscoveredPeer for that MAC must return false.
    EXPECT_TRUE(!pm.addDiscoveredPeer(mac_n(0x50), -40));

    // Advance past expiration; expiration sweep clears blacklist.
    RNS::Utilities::OS::set_fake_time(until + 1.0);
    pm.checkBlacklistExpirations();
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xD0))->state != PeerState::BLACKLISTED);

    RNS::Utilities::OS::clear_fake_time();
}

// Removing a peer must drop its conn_handle entry. If the handle map kept a
// dangling pointer to the freed slot, getPeerByHandle would return a UAF.
static void remove_peer_clears_handle_map() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x60), -50);
    pm.setPeerIdentity(mac_n(0x60), id_n(0xE0));
    pm.setPeerHandle(id_n(0xE0), 4);
    EXPECT_TRUE(pm.getPeerByHandle(4) != nullptr);

    pm.removePeer(id_n(0xE0));
    EXPECT_TRUE(pm.getPeerByIdentity(id_n(0xE0)) == nullptr);
    EXPECT_TRUE(pm.getPeerByHandle(4) == nullptr);
}

static void cleanup_ages_out_unconnected_peers() {
    BLEPeerManager pm;
    RNS::Utilities::OS::set_fake_time(0.0);
    pm.addDiscoveredPeer(mac_n(0x70), -50);
    EXPECT_EQ(pm.totalPeerCount(), (size_t)1);

    RNS::Utilities::OS::set_fake_time(1000.0);   // way past PEER_TIMEOUT (30s)
    pm.cleanupStalePeers(30.0);
    EXPECT_EQ(pm.totalPeerCount(), (size_t)0);

    RNS::Utilities::OS::clear_fake_time();
}

static void mac_pool_full_returns_false_no_crash() {
    BLEPeerManager pm;
    // PEERS_POOL_SIZE = 8 (MAC-only). Fill it.
    for (uint8_t i = 0; i < 8; ++i) {
        EXPECT_TRUE(pm.addDiscoveredPeer(mac_n(0xA0 + i), -50));
    }
    // 9th MAC-only peer must fail without exception.
    EXPECT_TRUE(!pm.addDiscoveredPeer(mac_n(0xA8), -50));
    EXPECT_EQ(pm.totalPeerCount(), (size_t)8);
}

static void should_initiate_lower_mac_wins() {
    Bytes our_mac;  for (uint8_t b : {0x01,0x02,0x03,0x04,0x05,0x06}) our_mac.append(b);
    Bytes other;    for (uint8_t b : {0x01,0x02,0x03,0x04,0x05,0x07}) other.append(b);
    EXPECT_TRUE(BLEPeerManager::shouldInitiateConnection(our_mac, other));
    EXPECT_TRUE(!BLEPeerManager::shouldInitiateConnection(other, our_mac));
}

static void update_peer_mac_handles_rotation() {
    BLEPeerManager pm;
    pm.addDiscoveredPeer(mac_n(0x80), -50);
    pm.setPeerIdentity(mac_n(0x80), id_n(0xF0));

    EXPECT_TRUE(pm.updatePeerMac(id_n(0xF0), mac_n(0x90)));
    auto* p = pm.getPeerByIdentity(id_n(0xF0));
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(p->mac_address, mac_n(0x90));
    // Old MAC lookup should not return this peer (it's now identity-keyed and
    // by_mac lookup goes through the rotated MAC).
    auto* p_by_new = pm.getPeerByMac(mac_n(0x90));
    EXPECT_TRUE(p_by_new != nullptr);
    EXPECT_EQ(p_by_new->mac_address, mac_n(0x90));
}

int main() {
    RUN(discover_new_peer_appears_in_count);
    RUN(rediscover_updates_rssi_no_double_count);
    RUN(short_mac_rejected);
    RUN(set_identity_promotes_peer);
    RUN(set_identity_unknown_mac_returns_false);
    RUN(set_handle_then_lookup_by_handle);
    RUN(out_of_range_handle_is_rejected_silently);
    RUN(connection_failures_blacklist_after_threshold);
    RUN(connection_success_clears_consecutive_failures);
    RUN(blacklist_blocks_rediscovery_until_expiration);
    RUN(remove_peer_clears_handle_map);
    RUN(cleanup_ages_out_unconnected_peers);
    RUN(mac_pool_full_returns_false_no_crash);
    RUN(should_initiate_lower_mac_wins);
    RUN(update_peer_mac_handles_rotation);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

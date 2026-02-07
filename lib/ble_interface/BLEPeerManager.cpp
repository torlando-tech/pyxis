/**
 * @file BLEPeerManager.cpp
 * @brief BLE-Reticulum Protocol v2.2 peer management implementation
 *
 * Uses fixed-size pools instead of STL containers to eliminate heap fragmentation.
 */

#include "BLEPeerManager.h"
#include "Log.h"

#include <cmath>
#include <cstring>

namespace RNS { namespace BLE {

BLEPeerManager::BLEPeerManager() {
    _local_mac = Bytes(6);  // Initialize to zeros

    // Initialize all pools to empty state
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        _peers_by_identity_pool[i].clear();
        _peers_by_mac_only_pool[i].clear();
    }
    for (size_t i = 0; i < MAC_IDENTITY_POOL_SIZE; i++) {
        _mac_to_identity_pool[i].clear();
    }
    for (size_t i = 0; i < MAX_CONN_HANDLES; i++) {
        _handle_to_peer[i] = nullptr;
    }
}

void BLEPeerManager::setLocalMac(const Bytes& mac) {
    if (mac.size() >= Limits::MAC_SIZE) {
        _local_mac = Bytes(mac.data(), Limits::MAC_SIZE);
    }
}

//=============================================================================
// Peer Discovery
//=============================================================================

bool BLEPeerManager::addDiscoveredPeer(const Bytes& mac_address, int8_t rssi, uint8_t address_type) {
    if (mac_address.size() < Limits::MAC_SIZE) {
        return false;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);
    double now = Utilities::OS::time();

    // Check if this MAC maps to a known identity
    Bytes identity = getIdentityForMac(mac);
    if (identity.size() == Limits::IDENTITY_SIZE) {
        // Update existing peer with identity
        PeerByIdentitySlot* slot = findPeerByIdentitySlot(identity);
        if (slot) {
            PeerInfo& peer = slot->peer;

            // Check if blacklisted
            if (peer.state == PeerState::BLACKLISTED && now < peer.blacklisted_until) {
                return false;
            }

            peer.last_seen = now;
            peer.rssi = rssi;
            peer.address_type = address_type;  // Update address type
            // Exponential moving average for RSSI
            peer.rssi_avg = static_cast<int8_t>(0.7f * peer.rssi_avg + 0.3f * rssi);

            return true;
        }
    }

    // Check if peer exists in MAC-only storage
    PeerByMacSlot* mac_slot = findPeerByMacSlot(mac);
    if (mac_slot) {
        PeerInfo& peer = mac_slot->peer;

        // Check if blacklisted
        if (peer.state == PeerState::BLACKLISTED && now < peer.blacklisted_until) {
            return false;
        }

        peer.last_seen = now;
        peer.rssi = rssi;
        peer.address_type = address_type;  // Update address type
        peer.rssi_avg = static_cast<int8_t>(0.7f * peer.rssi_avg + 0.3f * rssi);

        return true;
    }

    // New peer - add to MAC-only storage
    PeerByMacSlot* empty_slot = findEmptyPeerByMacSlot();
    if (!empty_slot) {
        WARNING("BLEPeerManager: MAC-only peer pool is full, cannot add new peer");
        return false;
    }

    empty_slot->in_use = true;
    empty_slot->mac_address = mac;
    PeerInfo& peer = empty_slot->peer;
    peer = PeerInfo();  // Reset to defaults
    peer.mac_address = mac;
    peer.address_type = address_type;
    peer.state = PeerState::DISCOVERED;
    peer.discovered_at = now;
    peer.last_seen = now;
    peer.rssi = rssi;
    peer.rssi_avg = rssi;

    char buf[80];
    snprintf(buf, sizeof(buf), "BLEPeerManager: Discovered new peer %s RSSI %d",
             BLEAddress(mac.data()).toString().c_str(), rssi);
    DEBUG(buf);

    return true;
}

bool BLEPeerManager::setPeerIdentity(const Bytes& mac_address, const Bytes& identity) {
    if (mac_address.size() < Limits::MAC_SIZE || identity.size() != Limits::IDENTITY_SIZE) {
        return false;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    // Check if peer exists in MAC-only storage
    PeerByMacSlot* mac_slot = findPeerByMacSlot(mac);
    if (mac_slot) {
        promoteToIdentityKeyed(mac, identity);
        return true;
    }

    // Check if peer already has identity (MAC might have changed)
    PeerByIdentitySlot* identity_slot = findPeerByIdentitySlot(identity);
    if (identity_slot) {
        // Update MAC address mapping
        PeerInfo& peer = identity_slot->peer;

        // Remove old MAC mapping if different
        if (peer.mac_address != mac) {
            removeMacToIdentity(peer.mac_address);
            peer.mac_address = mac;
            setMacToIdentity(mac, identity);
        }

        return true;
    }

    // Peer not found
    WARNING("BLEPeerManager: Cannot set identity for unknown peer");
    return false;
}

bool BLEPeerManager::updatePeerMac(const Bytes& identity, const Bytes& new_mac) {
    if (identity.size() != Limits::IDENTITY_SIZE || new_mac.size() < Limits::MAC_SIZE) {
        return false;
    }

    Bytes mac(new_mac.data(), Limits::MAC_SIZE);

    PeerByIdentitySlot* slot = findPeerByIdentitySlot(identity);
    if (!slot) {
        return false;
    }

    PeerInfo& peer = slot->peer;

    // Remove old MAC mapping
    removeMacToIdentity(peer.mac_address);

    // Update to new MAC
    peer.mac_address = mac;
    setMacToIdentity(mac, identity);

    char buf[80];
    snprintf(buf, sizeof(buf), "BLEPeerManager: Updated MAC for peer to %s",
             BLEAddress(mac.data()).toString().c_str());
    DEBUG(buf);

    return true;
}

//=============================================================================
// Peer Lookup
//=============================================================================

PeerInfo* BLEPeerManager::getPeerByMac(const Bytes& mac_address) {
    if (mac_address.size() < Limits::MAC_SIZE) return nullptr;

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    // Check MAC-to-identity mapping first
    Bytes identity = getIdentityForMac(mac);
    if (identity.size() == Limits::IDENTITY_SIZE) {
        PeerByIdentitySlot* slot = findPeerByIdentitySlot(identity);
        if (slot) {
            return &slot->peer;
        }
    }

    // Check MAC-only storage
    PeerByMacSlot* mac_slot = findPeerByMacSlot(mac);
    if (mac_slot) {
        return &mac_slot->peer;
    }

    return nullptr;
}

const PeerInfo* BLEPeerManager::getPeerByMac(const Bytes& mac_address) const {
    return const_cast<BLEPeerManager*>(this)->getPeerByMac(mac_address);
}

PeerInfo* BLEPeerManager::getPeerByIdentity(const Bytes& identity) {
    if (identity.size() != Limits::IDENTITY_SIZE) return nullptr;

    PeerByIdentitySlot* slot = findPeerByIdentitySlot(identity);
    if (slot) {
        return &slot->peer;
    }

    return nullptr;
}

const PeerInfo* BLEPeerManager::getPeerByIdentity(const Bytes& identity) const {
    return const_cast<BLEPeerManager*>(this)->getPeerByIdentity(identity);
}

PeerInfo* BLEPeerManager::getPeerByHandle(uint16_t conn_handle) {
    // O(1) lookup using handle array
    return getHandleToPeer(conn_handle);
}

const PeerInfo* BLEPeerManager::getPeerByHandle(uint16_t conn_handle) const {
    return getHandleToPeer(conn_handle);
}

std::vector<PeerInfo*> BLEPeerManager::getConnectedPeers() {
    std::vector<PeerInfo*> result;
    result.reserve(PEERS_POOL_SIZE);

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use && _peers_by_identity_pool[i].peer.isConnected()) {
            result.push_back(&_peers_by_identity_pool[i].peer);
        }
    }

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use && _peers_by_mac_only_pool[i].peer.isConnected()) {
            result.push_back(&_peers_by_mac_only_pool[i].peer);
        }
    }

    return result;
}

std::vector<PeerInfo*> BLEPeerManager::getAllPeers() {
    std::vector<PeerInfo*> result;
    result.reserve(PEERS_POOL_SIZE * 2);

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use) {
            result.push_back(&_peers_by_identity_pool[i].peer);
        }
    }

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use) {
            result.push_back(&_peers_by_mac_only_pool[i].peer);
        }
    }

    return result;
}

//=============================================================================
// Connection Management
//=============================================================================

PeerInfo* BLEPeerManager::getBestConnectionCandidate() {
    double now = Utilities::OS::time();
    PeerInfo* best = nullptr;
    float best_score = -1.0f;

    auto checkPeer = [&](PeerInfo& peer) {
        // Skip if already connected or connecting
        if (peer.state != PeerState::DISCOVERED) {
            return;
        }

        // Skip if blacklisted
        if (peer.state == PeerState::BLACKLISTED && now < peer.blacklisted_until) {
            return;
        }

        // Skip if we shouldn't initiate (MAC sorting)
        if (!shouldInitiateConnection(peer.mac_address)) {
            return;
        }

        if (peer.score > best_score) {
            best_score = peer.score;
            best = &peer;
        }
    };

    // Check identity-keyed peers (unlikely to be DISCOVERED state, but possible after disconnect)
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use) {
            checkPeer(_peers_by_identity_pool[i].peer);
        }
    }

    // Check MAC-only peers (more common for connection candidates)
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use) {
            checkPeer(_peers_by_mac_only_pool[i].peer);
        }
    }

    return best;
}

bool BLEPeerManager::shouldInitiateConnection(const Bytes& peer_mac) const {
    return shouldInitiateConnection(_local_mac, peer_mac);
}

bool BLEPeerManager::shouldInitiateConnection(const Bytes& our_mac, const Bytes& peer_mac) {
    if (our_mac.size() < Limits::MAC_SIZE || peer_mac.size() < Limits::MAC_SIZE) {
        return false;
    }

    // Check if our MAC is a random address (first byte >= 0xC0)
    // Random addresses have the two MSBs of the first byte set (0b11xxxxxx)
    // When using random addresses, MAC comparison is unreliable because our
    // random MAC changes between restarts. In this case, always initiate
    // connections and let the identity layer handle duplicate connections.
    if (our_mac.data()[0] >= 0xC0) {
        return true;  // Always initiate with random address
    }

    // Lower MAC initiates connection (standard behavior for public addresses)
    BLEAddress our_addr(our_mac.data());
    BLEAddress peer_addr(peer_mac.data());

    bool result = our_addr.isLowerThan(peer_addr);
    char buf[100];
    snprintf(buf, sizeof(buf), "BLEPeerManager::shouldInitiateConnection: our=%s peer=%s result=%s",
             our_addr.toString().c_str(), peer_addr.toString().c_str(), result ? "yes" : "no");
    DEBUG(buf);
    return result;
}

void BLEPeerManager::connectionSucceeded(const Bytes& identifier) {
    PeerInfo* peer = findPeer(identifier);
    if (!peer) return;

    peer->connection_successes++;
    peer->consecutive_failures = 0;
    peer->connected_at = Utilities::OS::time();
    peer->state = PeerState::CONNECTED;

    DEBUG("BLEPeerManager: Connection succeeded for peer");
}

void BLEPeerManager::connectionFailed(const Bytes& identifier) {
    PeerInfo* peer = findPeer(identifier);
    if (!peer) return;

    // Clear handle mapping on disconnect
    if (peer->conn_handle != 0xFFFF) {
        clearHandleToPeer(peer->conn_handle);
        peer->conn_handle = 0xFFFF;
    }

    peer->connection_failures++;
    peer->consecutive_failures++;
    peer->state = PeerState::DISCOVERED;

    // Check if should blacklist
    if (peer->consecutive_failures >= Limits::BLACKLIST_THRESHOLD) {
        double duration = calculateBlacklistDuration(peer->consecutive_failures);
        peer->blacklisted_until = Utilities::OS::time() + duration;
        peer->state = PeerState::BLACKLISTED;

        char buf[80];
        snprintf(buf, sizeof(buf), "BLEPeerManager: Blacklisted peer for %.0fs after %u failures",
                 duration, peer->consecutive_failures);
        WARNING(buf);
    }
}

void BLEPeerManager::setPeerState(const Bytes& identifier, PeerState state) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        peer->state = state;
    }
}

void BLEPeerManager::setPeerHandle(const Bytes& identifier, uint16_t conn_handle) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        // Remove old handle mapping if exists
        if (peer->conn_handle != 0xFFFF) {
            clearHandleToPeer(peer->conn_handle);
        }
        peer->conn_handle = conn_handle;
        // Add new handle mapping
        if (conn_handle != 0xFFFF) {
            setHandleToPeer(conn_handle, peer);
        }
    }
}

void BLEPeerManager::setPeerMTU(const Bytes& identifier, uint16_t mtu) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        peer->mtu = mtu;
    }
}

void BLEPeerManager::removePeer(const Bytes& identifier) {
    // Try identity first
    if (identifier.size() == Limits::IDENTITY_SIZE) {
        PeerByIdentitySlot* slot = findPeerByIdentitySlot(identifier);
        if (slot) {
            // Remove handle mapping
            if (slot->peer.conn_handle != 0xFFFF) {
                clearHandleToPeer(slot->peer.conn_handle);
            }
            // Remove MAC mapping
            removeMacToIdentity(slot->peer.mac_address);
            slot->clear();
            return;
        }
    }

    // Try MAC
    if (identifier.size() >= Limits::MAC_SIZE) {
        Bytes mac(identifier.data(), Limits::MAC_SIZE);

        // Check if maps to identity
        Bytes identity = getIdentityForMac(mac);
        if (identity.size() == Limits::IDENTITY_SIZE) {
            PeerByIdentitySlot* slot = findPeerByIdentitySlot(identity);
            if (slot) {
                // Remove handle mapping
                if (slot->peer.conn_handle != 0xFFFF) {
                    clearHandleToPeer(slot->peer.conn_handle);
                }
                slot->clear();
            }
            removeMacToIdentity(mac);
            return;
        }

        // Check MAC-only
        PeerByMacSlot* mac_slot = findPeerByMacSlot(mac);
        if (mac_slot) {
            // Remove handle mapping
            if (mac_slot->peer.conn_handle != 0xFFFF) {
                clearHandleToPeer(mac_slot->peer.conn_handle);
            }
            mac_slot->clear();
        }
    }
}

void BLEPeerManager::updateRssi(const Bytes& identifier, int8_t rssi) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        peer->rssi = rssi;
        peer->rssi_avg = static_cast<int8_t>(0.7f * peer->rssi_avg + 0.3f * rssi);
    }
}

//=============================================================================
// Statistics
//=============================================================================

void BLEPeerManager::recordPacketSent(const Bytes& identifier) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        peer->packets_sent++;
        peer->last_activity = Utilities::OS::time();
    }
}

void BLEPeerManager::recordPacketReceived(const Bytes& identifier) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        peer->packets_received++;
        peer->last_activity = Utilities::OS::time();
    }
}

void BLEPeerManager::updateLastActivity(const Bytes& identifier) {
    PeerInfo* peer = findPeer(identifier);
    if (peer) {
        peer->last_activity = Utilities::OS::time();
    }
}

//=============================================================================
// Scoring & Blacklist
//=============================================================================

void BLEPeerManager::recalculateScores() {
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use) {
            _peers_by_identity_pool[i].peer.score = calculateScore(_peers_by_identity_pool[i].peer);
        }
    }

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use) {
            _peers_by_mac_only_pool[i].peer.score = calculateScore(_peers_by_mac_only_pool[i].peer);
        }
    }
}

void BLEPeerManager::checkBlacklistExpirations() {
    double now = Utilities::OS::time();

    auto checkAndClear = [now](PeerInfo& peer) {
        if (peer.state == PeerState::BLACKLISTED && now >= peer.blacklisted_until) {
            peer.state = PeerState::DISCOVERED;
            peer.blacklisted_until = 0;
            DEBUG("BLEPeerManager: Peer blacklist expired, restored to DISCOVERED");
        }
    };

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use) {
            checkAndClear(_peers_by_identity_pool[i].peer);
        }
    }

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use) {
            checkAndClear(_peers_by_mac_only_pool[i].peer);
        }
    }
}

//=============================================================================
// Counts & Limits
//=============================================================================

size_t BLEPeerManager::connectedCount() const {
    size_t count = 0;

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use && _peers_by_identity_pool[i].peer.isConnected()) {
            count++;
        }
    }

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use && _peers_by_mac_only_pool[i].peer.isConnected()) {
            count++;
        }
    }

    return count;
}

void BLEPeerManager::cleanupStalePeers(double max_age) {
    double now = Utilities::OS::time();

    // Check MAC-only peers (identity-keyed peers are more persistent)
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (!_peers_by_mac_only_pool[i].in_use) continue;

        const PeerInfo& peer = _peers_by_mac_only_pool[i].peer;

        // Only clean up DISCOVERED peers (not connected or connecting)
        if (peer.state == PeerState::DISCOVERED) {
            double age = now - peer.last_seen;
            if (age > max_age) {
                Bytes mac = _peers_by_mac_only_pool[i].mac_address;
                _peers_by_mac_only_pool[i].clear();
                char buf[80];
                snprintf(buf, sizeof(buf), "BLEPeerManager: Removed stale peer %s",
                         BLEAddress(mac.data()).toString().c_str());
                TRACE(buf);
            }
        }
    }
}

//=============================================================================
// Private Methods
//=============================================================================

float BLEPeerManager::calculateScore(const PeerInfo& peer) const {
    double now = Utilities::OS::time();

    // RSSI component (60% weight)
    float rssi_norm = normalizeRssi(peer.rssi_avg);
    float rssi_score = Scoring::RSSI_WEIGHT * rssi_norm;

    // History component (30% weight)
    float history_score = 0.0f;
    if (peer.connection_attempts > 0) {
        float success_rate = static_cast<float>(peer.connection_successes) /
                             static_cast<float>(peer.connection_attempts);
        history_score = Scoring::HISTORY_WEIGHT * success_rate;
    } else {
        // New peer: benefit of the doubt (50%)
        history_score = Scoring::HISTORY_WEIGHT * 0.5f;
    }

    // Recency component (10% weight)
    float recency_score = 0.0f;
    double age = now - peer.last_seen;
    if (age < 5.0) {
        recency_score = Scoring::RECENCY_WEIGHT * 1.0f;
    } else if (age < 30.0) {
        // Linear decay from 1.0 to 0.0 over 25 seconds
        recency_score = Scoring::RECENCY_WEIGHT * (1.0f - static_cast<float>((age - 5.0) / 25.0));
    }

    return rssi_score + history_score + recency_score;
}

float BLEPeerManager::normalizeRssi(int8_t rssi) const {
    // Clamp to expected range
    if (rssi < Scoring::RSSI_MIN) rssi = Scoring::RSSI_MIN;
    if (rssi > Scoring::RSSI_MAX) rssi = Scoring::RSSI_MAX;

    // Map to 0.0-1.0
    return static_cast<float>(rssi - Scoring::RSSI_MIN) /
           static_cast<float>(Scoring::RSSI_MAX - Scoring::RSSI_MIN);
}

double BLEPeerManager::calculateBlacklistDuration(uint8_t failures) const {
    // Exponential backoff: 60s × min(2^(failures-3), 8)
    if (failures < Limits::BLACKLIST_THRESHOLD) {
        return 0;
    }

    uint8_t exponent = failures - Limits::BLACKLIST_THRESHOLD;
    uint8_t multiplier = 1 << exponent;  // 2^exponent
    if (multiplier > Limits::BLACKLIST_MAX_MULTIPLIER) {
        multiplier = Limits::BLACKLIST_MAX_MULTIPLIER;
    }

    return Timing::BLACKLIST_BASE_BACKOFF * multiplier;
}

PeerInfo* BLEPeerManager::findPeer(const Bytes& identifier) {
    // Try as identity
    if (identifier.size() == Limits::IDENTITY_SIZE) {
        PeerByIdentitySlot* slot = findPeerByIdentitySlot(identifier);
        if (slot) {
            return &slot->peer;
        }
    }

    // Try as MAC
    if (identifier.size() >= Limits::MAC_SIZE) {
        return getPeerByMac(identifier);
    }

    return nullptr;
}

void BLEPeerManager::promoteToIdentityKeyed(const Bytes& mac_address, const Bytes& identity) {
    PeerByMacSlot* mac_slot = findPeerByMacSlot(mac_address);
    if (!mac_slot) {
        return;
    }

    // Find an empty slot in identity pool
    PeerByIdentitySlot* identity_slot = findEmptyPeerByIdentitySlot();
    if (!identity_slot) {
        WARNING("BLEPeerManager: Identity pool is full, cannot promote peer");
        return;
    }

    // Copy peer info to identity pool
    identity_slot->in_use = true;
    identity_slot->identity_hash = identity;
    identity_slot->peer = mac_slot->peer;
    identity_slot->peer.identity = identity;

    // Update handle mapping to point to new location
    if (identity_slot->peer.conn_handle != 0xFFFF) {
        setHandleToPeer(identity_slot->peer.conn_handle, &identity_slot->peer);
    }

    // Add MAC-to-identity mapping
    setMacToIdentity(mac_address, identity);

    // Clear MAC-only slot
    mac_slot->clear();

    DEBUG("BLEPeerManager: Promoted peer to identity-keyed storage");
}

//=============================================================================
// Pool Helper Methods - Peers by Identity
//=============================================================================

BLEPeerManager::PeerByIdentitySlot* BLEPeerManager::findPeerByIdentitySlot(const Bytes& identity) {
    if (identity.size() != Limits::IDENTITY_SIZE) return nullptr;

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use &&
            _peers_by_identity_pool[i].identity_hash == identity) {
            return &_peers_by_identity_pool[i];
        }
    }
    return nullptr;
}

const BLEPeerManager::PeerByIdentitySlot* BLEPeerManager::findPeerByIdentitySlot(const Bytes& identity) const {
    return const_cast<BLEPeerManager*>(this)->findPeerByIdentitySlot(identity);
}

BLEPeerManager::PeerByIdentitySlot* BLEPeerManager::findEmptyPeerByIdentitySlot() {
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (!_peers_by_identity_pool[i].in_use) {
            return &_peers_by_identity_pool[i];
        }
    }
    return nullptr;
}

size_t BLEPeerManager::peersByIdentityCount() const {
    size_t count = 0;
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_identity_pool[i].in_use) count++;
    }
    return count;
}

//=============================================================================
// Pool Helper Methods - Peers by MAC Only
//=============================================================================

BLEPeerManager::PeerByMacSlot* BLEPeerManager::findPeerByMacSlot(const Bytes& mac) {
    if (mac.size() < Limits::MAC_SIZE) return nullptr;

    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use &&
            _peers_by_mac_only_pool[i].mac_address == mac) {
            return &_peers_by_mac_only_pool[i];
        }
    }
    return nullptr;
}

const BLEPeerManager::PeerByMacSlot* BLEPeerManager::findPeerByMacSlot(const Bytes& mac) const {
    return const_cast<BLEPeerManager*>(this)->findPeerByMacSlot(mac);
}

BLEPeerManager::PeerByMacSlot* BLEPeerManager::findEmptyPeerByMacSlot() {
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (!_peers_by_mac_only_pool[i].in_use) {
            return &_peers_by_mac_only_pool[i];
        }
    }
    return nullptr;
}

size_t BLEPeerManager::peersByMacOnlyCount() const {
    size_t count = 0;
    for (size_t i = 0; i < PEERS_POOL_SIZE; i++) {
        if (_peers_by_mac_only_pool[i].in_use) count++;
    }
    return count;
}

//=============================================================================
// Pool Helper Methods - MAC to Identity Mapping
//=============================================================================

BLEPeerManager::MacToIdentitySlot* BLEPeerManager::findMacToIdentitySlot(const Bytes& mac) {
    if (mac.size() < Limits::MAC_SIZE) return nullptr;

    for (size_t i = 0; i < MAC_IDENTITY_POOL_SIZE; i++) {
        if (_mac_to_identity_pool[i].in_use &&
            _mac_to_identity_pool[i].mac_address == mac) {
            return &_mac_to_identity_pool[i];
        }
    }
    return nullptr;
}

const BLEPeerManager::MacToIdentitySlot* BLEPeerManager::findMacToIdentitySlot(const Bytes& mac) const {
    return const_cast<BLEPeerManager*>(this)->findMacToIdentitySlot(mac);
}

BLEPeerManager::MacToIdentitySlot* BLEPeerManager::findEmptyMacToIdentitySlot() {
    for (size_t i = 0; i < MAC_IDENTITY_POOL_SIZE; i++) {
        if (!_mac_to_identity_pool[i].in_use) {
            return &_mac_to_identity_pool[i];
        }
    }
    return nullptr;
}

bool BLEPeerManager::setMacToIdentity(const Bytes& mac, const Bytes& identity) {
    // Check if already exists
    MacToIdentitySlot* existing = findMacToIdentitySlot(mac);
    if (existing) {
        existing->identity = identity;
        return true;
    }

    // Find empty slot
    MacToIdentitySlot* slot = findEmptyMacToIdentitySlot();
    if (!slot) {
        WARNING("BLEPeerManager: MAC-to-identity pool is full");
        return false;
    }

    slot->in_use = true;
    slot->mac_address = mac;
    slot->identity = identity;
    return true;
}

void BLEPeerManager::removeMacToIdentity(const Bytes& mac) {
    MacToIdentitySlot* slot = findMacToIdentitySlot(mac);
    if (slot) {
        slot->clear();
    }
}

Bytes BLEPeerManager::getIdentityForMac(const Bytes& mac) const {
    const MacToIdentitySlot* slot = findMacToIdentitySlot(mac);
    if (slot) {
        return slot->identity;
    }
    return Bytes();
}

//=============================================================================
// Pool Helper Methods - Handle to Peer Mapping
//=============================================================================

void BLEPeerManager::setHandleToPeer(uint16_t handle, PeerInfo* peer) {
    if (handle < MAX_CONN_HANDLES) {
        _handle_to_peer[handle] = peer;
    }
}

void BLEPeerManager::clearHandleToPeer(uint16_t handle) {
    if (handle < MAX_CONN_HANDLES) {
        _handle_to_peer[handle] = nullptr;
    }
}

PeerInfo* BLEPeerManager::getHandleToPeer(uint16_t handle) {
    if (handle < MAX_CONN_HANDLES) {
        return _handle_to_peer[handle];
    }
    return nullptr;
}

const PeerInfo* BLEPeerManager::getHandleToPeer(uint16_t handle) const {
    if (handle < MAX_CONN_HANDLES) {
        return _handle_to_peer[handle];
    }
    return nullptr;
}

}} // namespace RNS::BLE

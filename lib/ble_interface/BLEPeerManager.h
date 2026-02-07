/**
 * @file BLEPeerManager.h
 * @brief BLE-Reticulum Protocol v2.2 peer management
 *
 * Manages discovered and connected BLE peers with:
 * - Peer scoring for connection prioritization
 * - Blacklisting with exponential backoff
 * - MAC address rotation handling via identity-based keying
 * - Connection direction determination via MAC sorting
 *
 * Uses fixed-size pools instead of STL containers to eliminate heap fragmentation.
 */
#pragma once

#include "BLETypes.h"
#include "Bytes.h"
#include "Utilities/OS.h"

#include <cstdint>

namespace RNS { namespace BLE {

/**
 * @brief Information about a discovered/connected peer
 */
struct PeerInfo {
    // Addressing (both needed for MAC rotation handling)
    Bytes mac_address;              // Current 6-byte MAC address
    Bytes identity;                 // 16-byte identity hash (stable key)
    uint8_t address_type = 0;       // BLE address type (0=public, 1=random)

    // Connection state
    PeerState state = PeerState::DISCOVERED;
    bool is_central = false;        // true if we are central (we initiated)

    // Timing
    double discovered_at = 0.0;
    double last_seen = 0.0;
    double last_activity = 0.0;
    double connected_at = 0.0;

    // Signal quality
    int8_t rssi = Scoring::RSSI_MIN;
    int8_t rssi_avg = Scoring::RSSI_MIN;  // Smoothed average

    // Statistics for scoring
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t connection_attempts = 0;
    uint32_t connection_successes = 0;
    uint32_t connection_failures = 0;

    // Blacklist tracking
    uint8_t consecutive_failures = 0;
    double blacklisted_until = 0.0;

    // BLE connection handle (platform-specific)
    uint16_t conn_handle = 0xFFFF;

    // MTU for this peer
    uint16_t mtu = MTU::MINIMUM;

    // Computed score (cached)
    float score = 0.0f;

    // Check if peer has known identity
    bool hasIdentity() const { return identity.size() == Limits::IDENTITY_SIZE; }

    // Check if connected
    bool isConnected() const {
        return state == PeerState::CONNECTED ||
               state == PeerState::HANDSHAKING;
    }
};

/**
 * @brief Manages BLE peers for the BLEInterface
 *
 * Uses fixed-size pools to eliminate heap fragmentation:
 * - _peers_pool: Stores all peer info (max 8 slots)
 * - _mac_to_identity_pool: Maps MAC addresses to identities (max 8 slots)
 * - _handle_to_peer: Fixed array indexed by connection handle (max 8)
 */
class BLEPeerManager {
public:
    //=========================================================================
    // Pool Configuration
    //=========================================================================
    static constexpr size_t PEERS_POOL_SIZE = 8;
    static constexpr size_t MAC_IDENTITY_POOL_SIZE = 8;
    static constexpr size_t MAX_CONN_HANDLES = 8;

    /**
     * @brief Slot for storing peer info (keyed by identity)
     */
    struct PeerByIdentitySlot {
        bool in_use = false;
        Bytes identity_hash;  // 16-byte identity key
        PeerInfo peer;        // value

        void clear() {
            in_use = false;
            identity_hash.clear();
            peer = PeerInfo();
        }
    };

    /**
     * @brief Slot for storing peer info (keyed by MAC only, no identity yet)
     */
    struct PeerByMacSlot {
        bool in_use = false;
        Bytes mac_address;    // 6-byte MAC key
        PeerInfo peer;        // value

        void clear() {
            in_use = false;
            mac_address.clear();
            peer = PeerInfo();
        }
    };

    /**
     * @brief Slot for MAC to identity mapping
     */
    struct MacToIdentitySlot {
        bool in_use = false;
        Bytes mac_address;    // 6-byte MAC key
        Bytes identity;       // 16-byte identity value

        void clear() {
            in_use = false;
            mac_address.clear();
            identity.clear();
        }
    };

    BLEPeerManager();

    /**
     * @brief Set our local MAC address (for connection direction decisions)
     */
    void setLocalMac(const Bytes& mac);

    /**
     * @brief Get our local MAC address
     */
    const Bytes& getLocalMac() const { return _local_mac; }

    //=========================================================================
    // Peer Discovery
    //=========================================================================

    /**
     * @brief Register a newly discovered peer from BLE scan
     *
     * @param mac_address 6-byte MAC address
     * @param rssi Signal strength
     * @param address_type BLE address type (0=public, 1=random)
     * @return true if peer was added or updated (not blacklisted)
     */
    bool addDiscoveredPeer(const Bytes& mac_address, int8_t rssi, uint8_t address_type = 0);

    /**
     * @brief Update peer identity after handshake completion
     *
     * @param mac_address Current MAC address
     * @param identity 16-byte identity hash
     * @return true if peer was found and updated
     */
    bool setPeerIdentity(const Bytes& mac_address, const Bytes& identity);

    /**
     * @brief Update peer MAC address (when identity already known but MAC rotated)
     *
     * @param identity 16-byte identity hash
     * @param new_mac New 6-byte MAC address
     * @return true if peer was found and updated
     */
    bool updatePeerMac(const Bytes& identity, const Bytes& new_mac);

    //=========================================================================
    // Peer Lookup
    //=========================================================================

    /**
     * @brief Get peer info by MAC address
     * @return Pointer to PeerInfo or nullptr if not found
     */
    PeerInfo* getPeerByMac(const Bytes& mac_address);
    const PeerInfo* getPeerByMac(const Bytes& mac_address) const;

    /**
     * @brief Get peer info by identity
     * @return Pointer to PeerInfo or nullptr if not found
     */
    PeerInfo* getPeerByIdentity(const Bytes& identity);
    const PeerInfo* getPeerByIdentity(const Bytes& identity) const;

    /**
     * @brief Get peer info by connection handle
     * @return Pointer to PeerInfo or nullptr if not found
     */
    PeerInfo* getPeerByHandle(uint16_t conn_handle);
    const PeerInfo* getPeerByHandle(uint16_t conn_handle) const;

    /**
     * @brief Get all connected peers
     */
    std::vector<PeerInfo*> getConnectedPeers();

    /**
     * @brief Get all peers (for iteration)
     */
    std::vector<PeerInfo*> getAllPeers();

    //=========================================================================
    // Connection Management
    //=========================================================================

    /**
     * @brief Get best peer to connect to (highest score, not blacklisted)
     * @return Pointer to best peer or nullptr if none available
     */
    PeerInfo* getBestConnectionCandidate();

    /**
     * @brief Check if we should initiate connection to a peer (MAC sorting rule)
     *
     * Lower MAC address should be the initiator (central).
     * @param peer_mac The peer's MAC address
     * @return true if we should initiate (our MAC < peer MAC)
     */
    bool shouldInitiateConnection(const Bytes& peer_mac) const;

    /**
     * @brief Static version for use without instance
     */
    static bool shouldInitiateConnection(const Bytes& our_mac, const Bytes& peer_mac);

    /**
     * @brief Mark peer connection as successful
     */
    void connectionSucceeded(const Bytes& identifier);

    /**
     * @brief Mark peer connection as failed
     */
    void connectionFailed(const Bytes& identifier);

    /**
     * @brief Update peer state
     */
    void setPeerState(const Bytes& identifier, PeerState state);

    /**
     * @brief Set peer connection handle
     */
    void setPeerHandle(const Bytes& identifier, uint16_t conn_handle);

    /**
     * @brief Set peer MTU
     */
    void setPeerMTU(const Bytes& identifier, uint16_t mtu);

    /**
     * @brief Remove a peer
     */
    void removePeer(const Bytes& identifier);

    /**
     * @brief Update peer RSSI
     */
    void updateRssi(const Bytes& identifier, int8_t rssi);

    //=========================================================================
    // Statistics
    //=========================================================================

    /**
     * @brief Record packet sent to peer
     */
    void recordPacketSent(const Bytes& identifier);

    /**
     * @brief Record packet received from peer
     */
    void recordPacketReceived(const Bytes& identifier);

    /**
     * @brief Update last activity time for peer
     */
    void updateLastActivity(const Bytes& identifier);

    //=========================================================================
    // Scoring & Blacklist
    //=========================================================================

    /**
     * @brief Recalculate scores for all peers
     *
     * Should be called periodically or after significant changes.
     */
    void recalculateScores();

    /**
     * @brief Check blacklist expirations and restore peers
     */
    void checkBlacklistExpirations();

    //=========================================================================
    // Counts & Limits
    //=========================================================================

    /**
     * @brief Get current connected peer count
     */
    size_t connectedCount() const;

    /**
     * @brief Get total peer count
     */
    size_t totalPeerCount() const { return peersByIdentityCount() + peersByMacOnlyCount(); }

    /**
     * @brief Check if we can accept more connections
     */
    bool canAcceptConnection() const { return connectedCount() < Limits::MAX_PEERS; }

    /**
     * @brief Clean up stale discovered peers
     * @param max_age Maximum age in seconds for discovered (unconnected) peers
     */
    void cleanupStalePeers(double max_age = Timing::PEER_TIMEOUT);

private:
    /**
     * @brief Calculate peer score using v2.2 formula
     */
    float calculateScore(const PeerInfo& peer) const;

    /**
     * @brief Normalize RSSI to 0.0-1.0 range
     */
    float normalizeRssi(int8_t rssi) const;

    /**
     * @brief Calculate blacklist duration for given failure count
     */
    double calculateBlacklistDuration(uint8_t failures) const;

    /**
     * @brief Find peer by any identifier (MAC or identity)
     */
    PeerInfo* findPeer(const Bytes& identifier);

    /**
     * @brief Move peer from MAC-only to identity-keyed storage
     */
    void promoteToIdentityKeyed(const Bytes& mac_address, const Bytes& identity);

    //=========================================================================
    // Pool Helper Methods - Peers by Identity
    //=========================================================================

    /**
     * @brief Find slot by identity key
     * @return Pointer to slot or nullptr if not found
     */
    PeerByIdentitySlot* findPeerByIdentitySlot(const Bytes& identity);
    const PeerByIdentitySlot* findPeerByIdentitySlot(const Bytes& identity) const;

    /**
     * @brief Find an empty slot in the identity pool
     * @return Pointer to empty slot or nullptr if pool is full
     */
    PeerByIdentitySlot* findEmptyPeerByIdentitySlot();

    /**
     * @brief Get count of peers by identity
     */
    size_t peersByIdentityCount() const;

    //=========================================================================
    // Pool Helper Methods - Peers by MAC Only
    //=========================================================================

    /**
     * @brief Find slot by MAC key
     * @return Pointer to slot or nullptr if not found
     */
    PeerByMacSlot* findPeerByMacSlot(const Bytes& mac);
    const PeerByMacSlot* findPeerByMacSlot(const Bytes& mac) const;

    /**
     * @brief Find an empty slot in the MAC-only pool
     * @return Pointer to empty slot or nullptr if pool is full
     */
    PeerByMacSlot* findEmptyPeerByMacSlot();

    /**
     * @brief Get count of peers by MAC only
     */
    size_t peersByMacOnlyCount() const;

    //=========================================================================
    // Pool Helper Methods - MAC to Identity Mapping
    //=========================================================================

    /**
     * @brief Find MAC-to-identity mapping slot by MAC
     * @return Pointer to slot or nullptr if not found
     */
    MacToIdentitySlot* findMacToIdentitySlot(const Bytes& mac);
    const MacToIdentitySlot* findMacToIdentitySlot(const Bytes& mac) const;

    /**
     * @brief Find an empty slot in the MAC-to-identity pool
     * @return Pointer to empty slot or nullptr if pool is full
     */
    MacToIdentitySlot* findEmptyMacToIdentitySlot();

    /**
     * @brief Add or update MAC-to-identity mapping
     * @return true if successful, false if pool is full
     */
    bool setMacToIdentity(const Bytes& mac, const Bytes& identity);

    /**
     * @brief Remove MAC-to-identity mapping
     */
    void removeMacToIdentity(const Bytes& mac);

    /**
     * @brief Get identity for a MAC from the mapping pool
     * @return Identity or empty Bytes if not found
     */
    Bytes getIdentityForMac(const Bytes& mac) const;

    //=========================================================================
    // Pool Helper Methods - Handle to Peer Mapping
    //=========================================================================

    /**
     * @brief Set handle-to-peer mapping
     */
    void setHandleToPeer(uint16_t handle, PeerInfo* peer);

    /**
     * @brief Clear handle-to-peer mapping
     */
    void clearHandleToPeer(uint16_t handle);

    /**
     * @brief Get peer for handle
     * @return Pointer to peer or nullptr if not found
     */
    PeerInfo* getHandleToPeer(uint16_t handle);
    const PeerInfo* getHandleToPeer(uint16_t handle) const;

    //=========================================================================
    // Fixed-size Pool Storage
    //=========================================================================

    // Peers with known identity (keyed by identity)
    PeerByIdentitySlot _peers_by_identity_pool[PEERS_POOL_SIZE];

    // Peers without identity yet (keyed by MAC)
    PeerByMacSlot _peers_by_mac_only_pool[PEERS_POOL_SIZE];

    // MAC to identity lookup for peers with identity
    MacToIdentitySlot _mac_to_identity_pool[MAC_IDENTITY_POOL_SIZE];

    // Connection handle to peer pointer for O(1) lookup
    // Index is the connection handle (must be < MAX_CONN_HANDLES)
    // nullptr means no mapping for that handle
    PeerInfo* _handle_to_peer[MAX_CONN_HANDLES];

    // Our own MAC address
    Bytes _local_mac;
};

}} // namespace RNS::BLE

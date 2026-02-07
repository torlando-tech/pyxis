/**
 * @file BLEIdentityManager.h
 * @brief BLE-Reticulum Protocol v2.2 identity handshake manager
 *
 * Manages the identity handshake protocol and address-to-identity mapping.
 *
 * Handshake Protocol (v2.2):
 * 1. Central connects to peripheral
 * 2. Central writes 16-byte identity to RX characteristic
 * 3. Peripheral detects handshake: exactly 16 bytes AND no existing identity for that address
 * 4. Both sides now have bidirectional identity mapping
 *
 * The identity is the first 16 bytes of the Reticulum transport identity hash,
 * which remains stable across MAC address rotations.
 *
 * Uses fixed-size pools instead of STL containers to eliminate heap fragmentation.
 */
#pragma once

#include "BLETypes.h"
#include "Bytes.h"
#include "Utilities/OS.h"

#include <functional>
#include <cstdint>

namespace RNS { namespace BLE {

class BLEIdentityManager {
public:
    //=========================================================================
    // Pool Configuration
    //=========================================================================
    static constexpr size_t ADDRESS_IDENTITY_POOL_SIZE = 16;
    static constexpr size_t HANDSHAKE_POOL_SIZE = 4;
    /**
     * @brief Callback when handshake completes successfully
     *
     * @param mac_address The peer's current MAC address
     * @param peer_identity The peer's 16-byte identity hash
     * @param is_central true if we are the central (we initiated)
     */
    using HandshakeCompleteCallback = std::function<void(
        const Bytes& mac_address,
        const Bytes& peer_identity,
        bool is_central)>;

    /**
     * @brief Callback when handshake fails
     *
     * @param mac_address The peer's MAC address
     * @param reason Description of the failure
     */
    using HandshakeFailedCallback = std::function<void(
        const Bytes& mac_address,
        const std::string& reason)>;

    /**
     * @brief Callback when MAC rotation is detected
     *
     * Called when an identity we already know appears from a different MAC address.
     * This is common on Android which rotates BLE MAC addresses every ~15 minutes.
     *
     * @param old_mac The previous MAC address for this identity
     * @param new_mac The new MAC address
     * @param identity The stable 16-byte identity
     */
    using MacRotationCallback = std::function<void(
        const Bytes& old_mac,
        const Bytes& new_mac,
        const Bytes& identity)>;

public:
    BLEIdentityManager();

    /**
     * @brief Set our local identity (from RNS::Identity)
     *
     * Must be called before any handshakes. The identity should be the
     * first 16 bytes of the transport identity hash.
     *
     * @param identity_hash The 16-byte identity hash
     */
    void setLocalIdentity(const Bytes& identity_hash);

    /**
     * @brief Get our local identity hash
     */
    const Bytes& getLocalIdentity() const { return _local_identity; }

    /**
     * @brief Check if local identity is set
     */
    bool hasLocalIdentity() const { return _local_identity.size() == Limits::IDENTITY_SIZE; }

    /**
     * @brief Set callback for successful handshakes
     */
    void setHandshakeCompleteCallback(HandshakeCompleteCallback callback);

    /**
     * @brief Set callback for failed handshakes
     */
    void setHandshakeFailedCallback(HandshakeFailedCallback callback);

    /**
     * @brief Set callback for MAC rotation detection
     */
    void setMacRotationCallback(MacRotationCallback callback);

    //=========================================================================
    // Handshake Operations
    //=========================================================================

    /**
     * @brief Start handshake as central (initiator)
     *
     * Called after BLE connection is established. Returns the identity
     * bytes that should be written to the peer's RX characteristic.
     *
     * @param mac_address Peer's MAC address
     * @return The 16-byte identity to write to peer's RX characteristic
     */
    Bytes initiateHandshake(const Bytes& mac_address);

    /**
     * @brief Process received data to detect/complete handshake
     *
     * This should be called for all received data. The function detects
     * whether the data is an identity handshake or regular data.
     *
     * @param mac_address Source MAC address
     * @param data Received data (may be identity or regular packet)
     * @param is_central true if we are central role for this connection
     * @return true if this was a handshake message (consumed), false if regular data
     */
    bool processReceivedData(const Bytes& mac_address, const Bytes& data, bool is_central);

    /**
     * @brief Check if data looks like an identity handshake
     *
     * A handshake is detected if:
     * - Data is exactly 16 bytes
     * - No existing identity mapping exists for this MAC address
     *
     * @param data The received data
     * @param mac_address The sender's MAC
     * @return true if this appears to be a handshake
     */
    bool isHandshakeData(const Bytes& data, const Bytes& mac_address) const;

    /**
     * @brief Mark handshake as complete for a peer
     *
     * Called after receiving identity from peer or after writing our identity.
     *
     * @param mac_address The peer's MAC address
     * @param peer_identity The peer's 16-byte identity
     * @param is_central true if we are the central
     */
    void completeHandshake(const Bytes& mac_address, const Bytes& peer_identity, bool is_central);

    /**
     * @brief Check for timed-out handshakes
     */
    void checkTimeouts();

    //=========================================================================
    // Identity Mapping
    //=========================================================================

    /**
     * @brief Get identity for a MAC address
     * @return Identity bytes or empty if not known
     */
    Bytes getIdentityForMac(const Bytes& mac_address) const;

    /**
     * @brief Get MAC address for an identity
     * @return MAC address or empty if not known
     */
    Bytes getMacForIdentity(const Bytes& identity) const;

    /**
     * @brief Check if we have completed handshake with a MAC
     */
    bool hasIdentity(const Bytes& mac_address) const;

    /**
     * @brief Find identity that matches a prefix (for MAC rotation detection)
     *
     * Used when scanning detects a device name with identity prefix (Protocol v2.2).
     * If found, returns the full identity so we can recognize the rotated peer.
     *
     * @param prefix First N bytes of identity (typically 3 bytes from device name)
     * @return Full identity bytes if found, empty otherwise
     */
    Bytes findIdentityByPrefix(const Bytes& prefix) const;

    /**
     * @brief Update MAC address for a known identity (MAC rotation)
     *
     * @param identity The stable identity
     * @param new_mac The new MAC address
     */
    void updateMacForIdentity(const Bytes& identity, const Bytes& new_mac);

    /**
     * @brief Remove identity mapping (on disconnect)
     */
    void removeMapping(const Bytes& mac_address);

    /**
     * @brief Clear all mappings
     */
    void clearAllMappings();

    /**
     * @brief Get count of known peer identities
     */
    size_t knownPeerCount() const;

    /**
     * @brief Check if handshake is in progress for a MAC
     */
    bool isHandshakeInProgress(const Bytes& mac_address) const;

private:
    /**
     * @brief Handshake state tracking
     */
    enum class HandshakeState {
        NONE,               // No handshake in progress
        INITIATED,          // We sent our identity (as central)
        RECEIVED_IDENTITY,  // We received peer's identity
        COMPLETE            // Bidirectional identity exchange done
    };

    /**
     * @brief State for an in-progress handshake
     */
    struct HandshakeSession {
        bool in_use = false;
        Bytes mac_address;
        Bytes peer_identity;
        HandshakeState state = HandshakeState::NONE;
        bool is_central = false;
        double started_at = 0.0;

        void clear() {
            in_use = false;
            mac_address.clear();
            peer_identity.clear();
            state = HandshakeState::NONE;
            is_central = false;
            started_at = 0.0;
        }
    };

    /**
     * @brief Slot for address-to-identity mapping
     */
    struct AddressIdentitySlot {
        bool in_use = false;
        Bytes mac_address;    // 6-byte MAC key
        Bytes identity;       // 16-byte identity value

        void clear() {
            in_use = false;
            mac_address.clear();
            identity.clear();
        }
    };

    //=========================================================================
    // Pool Helper Methods - Address to Identity Mapping
    //=========================================================================

    /**
     * @brief Find slot by MAC address
     */
    AddressIdentitySlot* findAddressToIdentitySlot(const Bytes& mac);
    const AddressIdentitySlot* findAddressToIdentitySlot(const Bytes& mac) const;

    /**
     * @brief Find slot by identity
     */
    AddressIdentitySlot* findIdentityToAddressSlot(const Bytes& identity);
    const AddressIdentitySlot* findIdentityToAddressSlot(const Bytes& identity) const;

    /**
     * @brief Find an empty slot in the address-identity pool
     */
    AddressIdentitySlot* findEmptyAddressIdentitySlot();

    /**
     * @brief Add or update address-identity mapping
     * @return true if successful, false if pool is full
     */
    bool setAddressIdentityMapping(const Bytes& mac, const Bytes& identity);

    /**
     * @brief Remove mapping by MAC address
     */
    void removeAddressIdentityMapping(const Bytes& mac);

    //=========================================================================
    // Pool Helper Methods - Handshake Sessions
    //=========================================================================

    /**
     * @brief Find handshake session by MAC
     */
    HandshakeSession* findHandshakeSession(const Bytes& mac);
    const HandshakeSession* findHandshakeSession(const Bytes& mac) const;

    /**
     * @brief Find an empty handshake session slot
     */
    HandshakeSession* findEmptyHandshakeSlot();

    /**
     * @brief Get or create a handshake session for a MAC
     */
    HandshakeSession* getOrCreateSession(const Bytes& mac_address);

    /**
     * @brief Remove handshake session by MAC
     */
    void removeHandshakeSession(const Bytes& mac);

    //=========================================================================
    // Fixed-size Pool Storage
    //=========================================================================

    // Our local identity hash (16 bytes)
    Bytes _local_identity;

    // Bidirectional mappings (survive MAC rotation via identity)
    // Note: We use the same pool for both directions since they share the same data
    AddressIdentitySlot _address_identity_pool[ADDRESS_IDENTITY_POOL_SIZE];

    // Active handshake sessions (keyed by MAC)
    HandshakeSession _handshakes_pool[HANDSHAKE_POOL_SIZE];

    // Callbacks
    HandshakeCompleteCallback _handshake_complete_callback = nullptr;
    HandshakeFailedCallback _handshake_failed_callback = nullptr;
    MacRotationCallback _mac_rotation_callback = nullptr;
};

}} // namespace RNS::BLE

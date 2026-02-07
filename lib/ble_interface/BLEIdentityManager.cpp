/**
 * @file BLEIdentityManager.cpp
 * @brief BLE-Reticulum Protocol v2.2 identity handshake manager implementation
 *
 * Uses fixed-size pools instead of STL containers to eliminate heap fragmentation.
 */

#include "BLEIdentityManager.h"
#include "Log.h"

namespace RNS { namespace BLE {

BLEIdentityManager::BLEIdentityManager() {
    // Initialize all pools to empty state
    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        _address_identity_pool[i].clear();
    }
    for (size_t i = 0; i < HANDSHAKE_POOL_SIZE; i++) {
        _handshakes_pool[i].clear();
    }
}

void BLEIdentityManager::setLocalIdentity(const Bytes& identity_hash) {
    if (identity_hash.size() >= Limits::IDENTITY_SIZE) {
        _local_identity = Bytes(identity_hash.data(), Limits::IDENTITY_SIZE);
        DEBUG("BLEIdentityManager: Local identity set: " + _local_identity.toHex().substr(0, 8) + "...");
    } else {
        ERROR("BLEIdentityManager: Invalid identity size: " + std::to_string(identity_hash.size()));
    }
}

void BLEIdentityManager::setHandshakeCompleteCallback(HandshakeCompleteCallback callback) {
    _handshake_complete_callback = callback;
}

void BLEIdentityManager::setHandshakeFailedCallback(HandshakeFailedCallback callback) {
    _handshake_failed_callback = callback;
}

void BLEIdentityManager::setMacRotationCallback(MacRotationCallback callback) {
    _mac_rotation_callback = callback;
}

//=============================================================================
// Handshake Operations
//=============================================================================

Bytes BLEIdentityManager::initiateHandshake(const Bytes& mac_address) {
    if (!hasLocalIdentity()) {
        ERROR("BLEIdentityManager: Cannot initiate handshake without local identity");
        return Bytes();
    }

    if (mac_address.size() < Limits::MAC_SIZE) {
        ERROR("BLEIdentityManager: Invalid MAC address size");
        return Bytes();
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    // Create or update handshake session
    HandshakeSession* session = getOrCreateSession(mac);
    if (!session) {
        WARNING("BLEIdentityManager: Handshake pool is full, cannot initiate");
        return Bytes();
    }
    session->is_central = true;
    session->state = HandshakeState::INITIATED;
    session->started_at = Utilities::OS::time();

    DEBUG("BLEIdentityManager: Initiating handshake as central with " +
          BLEAddress(mac.data()).toString());

    // Return our identity to be written to peer
    return _local_identity;
}

bool BLEIdentityManager::processReceivedData(const Bytes& mac_address, const Bytes& data, bool is_central) {
    if (mac_address.size() < Limits::MAC_SIZE) {
        return false;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    // Check if this looks like a handshake
    if (!isHandshakeData(data, mac)) {
        return false;  // Regular data, not consumed
    }

    // This is a handshake - extract peer's identity
    if (data.size() != Limits::IDENTITY_SIZE) {
        // Should not happen given isHandshakeData check, but be safe
        return false;
    }

    Bytes peer_identity(data.data(), Limits::IDENTITY_SIZE);

    DEBUG("BLEIdentityManager: Received identity handshake from " +
          BLEAddress(mac.data()).toString() + ": " +
          peer_identity.toHex().substr(0, 8) + "...");

    // Complete the handshake
    completeHandshake(mac, peer_identity, is_central);

    return true;  // Handshake data consumed
}

bool BLEIdentityManager::isHandshakeData(const Bytes& data, const Bytes& mac_address) const {
    // Handshake is detected if:
    // 1. Data is exactly 16 bytes (identity size)
    // 2. No existing identity mapping for this MAC

    if (data.size() != Limits::IDENTITY_SIZE) {
        return false;
    }

    if (mac_address.size() < Limits::MAC_SIZE) {
        return false;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    // Check if we already have identity for this MAC
    const AddressIdentitySlot* slot = findAddressToIdentitySlot(mac);
    if (slot) {
        // Already have identity - this is regular data, not handshake
        return false;
    }

    // No existing identity + 16 bytes = handshake
    return true;
}

void BLEIdentityManager::completeHandshake(const Bytes& mac_address, const Bytes& peer_identity,
                                            bool is_central) {
    DEBUG("BLEIdentityManager::completeHandshake: Starting");
    if (mac_address.size() < Limits::MAC_SIZE || peer_identity.size() != Limits::IDENTITY_SIZE) {
        DEBUG("BLEIdentityManager::completeHandshake: Invalid sizes, returning");
        return;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);
    Bytes identity(peer_identity.data(), Limits::IDENTITY_SIZE);
    DEBUG("BLEIdentityManager::completeHandshake: Created local copies");

    // Check for MAC rotation: same identity from different MAC address
    Bytes old_mac;
    bool is_rotation = false;
    AddressIdentitySlot* existing_slot = findIdentityToAddressSlot(identity);
    if (existing_slot && existing_slot->mac_address != mac) {
        // MAC rotation detected!
        old_mac = existing_slot->mac_address;
        is_rotation = true;

        INFO("BLEIdentityManager: MAC rotation detected for identity " +
             identity.toHex().substr(0, 8) + "...: " +
             BLEAddress(old_mac.data()).toString() + " -> " +
             BLEAddress(mac.data()).toString());

        // Update the slot with new MAC (same identity)
        existing_slot->mac_address = mac;
    } else if (!existing_slot) {
        // New mapping - add to pool
        if (!setAddressIdentityMapping(mac, identity)) {
            WARNING("BLEIdentityManager: Address-identity pool is full");
            return;
        }
    }
    DEBUG("BLEIdentityManager::completeHandshake: Stored mappings");

    // Remove handshake session
    removeHandshakeSession(mac);
    DEBUG("BLEIdentityManager::completeHandshake: Removed handshake session");

    DEBUG("BLEIdentityManager: Handshake complete with " +
          BLEAddress(mac.data()).toString() +
          " identity: " + identity.toHex().substr(0, 8) + "..." +
          (is_central ? " (we are central)" : " (we are peripheral)"));

    // Invoke MAC rotation callback if this was a rotation
    if (is_rotation && _mac_rotation_callback) {
        DEBUG("BLEIdentityManager::completeHandshake: Calling MAC rotation callback");
        _mac_rotation_callback(old_mac, mac, identity);
        DEBUG("BLEIdentityManager::completeHandshake: MAC rotation callback returned");
    }

    // Invoke handshake complete callback
    if (_handshake_complete_callback) {
        DEBUG("BLEIdentityManager::completeHandshake: Calling handshake complete callback");
        _handshake_complete_callback(mac, identity, is_central);
        DEBUG("BLEIdentityManager::completeHandshake: Callback returned");
    } else {
        DEBUG("BLEIdentityManager::completeHandshake: No callback set");
    }
}

void BLEIdentityManager::checkTimeouts() {
    double now = Utilities::OS::time();

    for (size_t i = 0; i < HANDSHAKE_POOL_SIZE; i++) {
        HandshakeSession& session = _handshakes_pool[i];
        if (!session.in_use) continue;

        if (session.state != HandshakeState::COMPLETE) {
            double age = now - session.started_at;
            if (age > Timing::HANDSHAKE_TIMEOUT) {
                Bytes mac = session.mac_address;

                WARNING("BLEIdentityManager: Handshake timeout for " +
                        BLEAddress(mac.data()).toString());

                if (_handshake_failed_callback) {
                    _handshake_failed_callback(mac, "Handshake timeout");
                }

                session.clear();
            }
        }
    }
}

//=============================================================================
// Identity Mapping
//=============================================================================

Bytes BLEIdentityManager::getIdentityForMac(const Bytes& mac_address) const {
    if (mac_address.size() < Limits::MAC_SIZE) {
        return Bytes();
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    const AddressIdentitySlot* slot = findAddressToIdentitySlot(mac);
    if (slot) {
        return slot->identity;
    }

    return Bytes();
}

Bytes BLEIdentityManager::getMacForIdentity(const Bytes& identity) const {
    if (identity.size() != Limits::IDENTITY_SIZE) {
        return Bytes();
    }

    const AddressIdentitySlot* slot = findIdentityToAddressSlot(identity);
    if (slot) {
        return slot->mac_address;
    }

    return Bytes();
}

bool BLEIdentityManager::hasIdentity(const Bytes& mac_address) const {
    if (mac_address.size() < Limits::MAC_SIZE) {
        return false;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);
    return findAddressToIdentitySlot(mac) != nullptr;
}

Bytes BLEIdentityManager::findIdentityByPrefix(const Bytes& prefix) const {
    if (prefix.size() == 0 || prefix.size() > Limits::IDENTITY_SIZE) {
        return Bytes();
    }

    // Search through all known identities for one that starts with this prefix
    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        if (_address_identity_pool[i].in_use) {
            const Bytes& identity = _address_identity_pool[i].identity;
            if (identity.size() >= prefix.size() &&
                memcmp(identity.data(), prefix.data(), prefix.size()) == 0) {
                return identity;
            }
        }
    }

    return Bytes();
}

void BLEIdentityManager::updateMacForIdentity(const Bytes& identity, const Bytes& new_mac) {
    if (identity.size() != Limits::IDENTITY_SIZE || new_mac.size() < Limits::MAC_SIZE) {
        return;
    }

    Bytes mac(new_mac.data(), Limits::MAC_SIZE);

    AddressIdentitySlot* slot = findIdentityToAddressSlot(identity);
    if (!slot) {
        return;  // Unknown identity
    }

    // Update MAC address in the slot
    slot->mac_address = mac;

    DEBUG("BLEIdentityManager: Updated MAC for identity " +
          identity.toHex().substr(0, 8) + "... to " +
          BLEAddress(mac.data()).toString());
}

void BLEIdentityManager::removeMapping(const Bytes& mac_address) {
    if (mac_address.size() < Limits::MAC_SIZE) {
        return;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    removeAddressIdentityMapping(mac);

    DEBUG("BLEIdentityManager: Removed mapping for " +
          BLEAddress(mac.data()).toString());

    // Also clean up any pending handshake
    removeHandshakeSession(mac);
}

void BLEIdentityManager::clearAllMappings() {
    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        _address_identity_pool[i].clear();
    }
    for (size_t i = 0; i < HANDSHAKE_POOL_SIZE; i++) {
        _handshakes_pool[i].clear();
    }

    DEBUG("BLEIdentityManager: Cleared all identity mappings");
}

size_t BLEIdentityManager::knownPeerCount() const {
    size_t count = 0;
    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        if (_address_identity_pool[i].in_use) count++;
    }
    return count;
}

bool BLEIdentityManager::isHandshakeInProgress(const Bytes& mac_address) const {
    if (mac_address.size() < Limits::MAC_SIZE) {
        return false;
    }

    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    const HandshakeSession* session = findHandshakeSession(mac);
    if (session) {
        return session->state != HandshakeState::NONE &&
               session->state != HandshakeState::COMPLETE;
    }

    return false;
}

//=============================================================================
// Pool Helper Methods - Address to Identity Mapping
//=============================================================================

BLEIdentityManager::AddressIdentitySlot* BLEIdentityManager::findAddressToIdentitySlot(const Bytes& mac) {
    if (mac.size() < Limits::MAC_SIZE) return nullptr;

    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        if (_address_identity_pool[i].in_use &&
            _address_identity_pool[i].mac_address == mac) {
            return &_address_identity_pool[i];
        }
    }
    return nullptr;
}

const BLEIdentityManager::AddressIdentitySlot* BLEIdentityManager::findAddressToIdentitySlot(const Bytes& mac) const {
    return const_cast<BLEIdentityManager*>(this)->findAddressToIdentitySlot(mac);
}

BLEIdentityManager::AddressIdentitySlot* BLEIdentityManager::findIdentityToAddressSlot(const Bytes& identity) {
    if (identity.size() != Limits::IDENTITY_SIZE) return nullptr;

    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        if (_address_identity_pool[i].in_use &&
            _address_identity_pool[i].identity == identity) {
            return &_address_identity_pool[i];
        }
    }
    return nullptr;
}

const BLEIdentityManager::AddressIdentitySlot* BLEIdentityManager::findIdentityToAddressSlot(const Bytes& identity) const {
    return const_cast<BLEIdentityManager*>(this)->findIdentityToAddressSlot(identity);
}

BLEIdentityManager::AddressIdentitySlot* BLEIdentityManager::findEmptyAddressIdentitySlot() {
    for (size_t i = 0; i < ADDRESS_IDENTITY_POOL_SIZE; i++) {
        if (!_address_identity_pool[i].in_use) {
            return &_address_identity_pool[i];
        }
    }
    return nullptr;
}

bool BLEIdentityManager::setAddressIdentityMapping(const Bytes& mac, const Bytes& identity) {
    // Check if already exists for this MAC
    AddressIdentitySlot* existing = findAddressToIdentitySlot(mac);
    if (existing) {
        existing->identity = identity;
        return true;
    }

    // Check if already exists for this identity (MAC rotation case)
    existing = findIdentityToAddressSlot(identity);
    if (existing) {
        existing->mac_address = mac;
        return true;
    }

    // Find empty slot
    AddressIdentitySlot* slot = findEmptyAddressIdentitySlot();
    if (!slot) {
        WARNING("BLEIdentityManager: Address-identity pool is full");
        return false;
    }

    slot->in_use = true;
    slot->mac_address = mac;
    slot->identity = identity;
    return true;
}

void BLEIdentityManager::removeAddressIdentityMapping(const Bytes& mac) {
    AddressIdentitySlot* slot = findAddressToIdentitySlot(mac);
    if (slot) {
        slot->clear();
    }
}

//=============================================================================
// Pool Helper Methods - Handshake Sessions
//=============================================================================

BLEIdentityManager::HandshakeSession* BLEIdentityManager::findHandshakeSession(const Bytes& mac) {
    if (mac.size() < Limits::MAC_SIZE) return nullptr;

    for (size_t i = 0; i < HANDSHAKE_POOL_SIZE; i++) {
        if (_handshakes_pool[i].in_use &&
            _handshakes_pool[i].mac_address == mac) {
            return &_handshakes_pool[i];
        }
    }
    return nullptr;
}

const BLEIdentityManager::HandshakeSession* BLEIdentityManager::findHandshakeSession(const Bytes& mac) const {
    return const_cast<BLEIdentityManager*>(this)->findHandshakeSession(mac);
}

BLEIdentityManager::HandshakeSession* BLEIdentityManager::findEmptyHandshakeSlot() {
    for (size_t i = 0; i < HANDSHAKE_POOL_SIZE; i++) {
        if (!_handshakes_pool[i].in_use) {
            return &_handshakes_pool[i];
        }
    }
    return nullptr;
}

BLEIdentityManager::HandshakeSession* BLEIdentityManager::getOrCreateSession(const Bytes& mac_address) {
    Bytes mac(mac_address.data(), Limits::MAC_SIZE);

    // Check if session already exists
    HandshakeSession* existing = findHandshakeSession(mac);
    if (existing) {
        return existing;
    }

    // Find empty slot
    HandshakeSession* slot = findEmptyHandshakeSlot();
    if (!slot) {
        return nullptr;  // Pool is full
    }

    // Create new session
    slot->in_use = true;
    slot->mac_address = mac;
    slot->state = HandshakeState::NONE;
    slot->started_at = Utilities::OS::time();

    return slot;
}

void BLEIdentityManager::removeHandshakeSession(const Bytes& mac) {
    HandshakeSession* session = findHandshakeSession(mac);
    if (session) {
        session->clear();
    }
}

}} // namespace RNS::BLE

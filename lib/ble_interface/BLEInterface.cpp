/**
 * @file BLEInterface.cpp
 * @brief BLE-Reticulum Protocol v2.2 interface implementation
 */

#include "BLEInterface.h"
#include "Log.h"
#include "Utilities/OS.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#endif

using namespace RNS;
using namespace RNS::BLE;

BLEInterface::BLEInterface(const char* name) : InterfaceImpl(name) {
    _IN = true;
    _OUT = true;
    _bitrate = BITRATE_GUESS;
    _HW_MTU = HW_MTU_DEFAULT;
}

BLEInterface::~BLEInterface() {
    stop();
}

//=============================================================================
// Configuration
//=============================================================================

void BLEInterface::setRole(Role role) {
    _role = role;
}

void BLEInterface::setDeviceName(const std::string& name) {
    _device_name = name;
}

void BLEInterface::setLocalIdentity(const Bytes& identity) {
    if (identity.size() >= Limits::IDENTITY_SIZE) {
        _local_identity = Bytes(identity.data(), Limits::IDENTITY_SIZE);
        _identity_manager.setLocalIdentity(_local_identity);
    }
}

void BLEInterface::setMaxConnections(uint8_t max) {
    _max_connections = (max <= Limits::MAX_PEERS) ? max : Limits::MAX_PEERS;
}

//=============================================================================
// InterfaceImpl Overrides
//=============================================================================

bool BLEInterface::start() {
    if (_platform && _platform->isRunning()) {
        return true;
    }

    // Validate identity
    if (!_identity_manager.hasLocalIdentity()) {
        ERROR("BLEInterface: Local identity not set");
        return false;
    }

    // Create platform
    _platform = BLEPlatformFactory::create();
    if (!_platform) {
        ERROR("BLEInterface: Failed to create BLE platform");
        return false;
    }

    // Configure platform
    PlatformConfig config;
    config.role = _role;
    config.device_name = _device_name;
    config.preferred_mtu = MTU::REQUESTED;
    config.max_connections = _max_connections;

    if (!_platform->initialize(config)) {
        ERROR("BLEInterface: Failed to initialize BLE platform");
        _platform.reset();
        return false;
    }

    // Setup callbacks
    setupCallbacks();

    // Set identity data for peripheral mode
    _platform->setIdentityData(_local_identity);

    // Start platform
    if (!_platform->start()) {
        ERROR("BLEInterface: Failed to start BLE platform");
        _platform.reset();
        return false;
    }

    // Set local MAC in peer manager (must be after start() when NimBLE has a valid address)
    auto local_addr = _platform->getLocalAddress();
    auto local_mac_bytes = local_addr.toBytes();
    INFO("BLEInterface: Local address from platform: " + local_addr.toString() +
         " bytes_size=" + std::to_string(local_mac_bytes.size()) +
         " isZero=" + std::to_string(local_addr.isZero()));
    _peer_manager.setLocalMac(local_mac_bytes);

    _online = true;
    _last_scan = 0;  // Trigger immediate scan
    _last_keepalive = Utilities::OS::time();
    _last_maintenance = Utilities::OS::time();

    INFO("BLEInterface: Started, role: " + std::string(roleToString(_role)) +
         ", identity: " + _local_identity.toHex().substr(0, 8) + "..." +
         ", localMAC: " + _platform->getLocalAddress().toString());

    return true;
}

void BLEInterface::stop() {
    if (_platform) {
        _platform->stop();
        _platform->shutdown();
        _platform.reset();
    }

    for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
        _fragmenter_pool[i].clear();
    }
    _pending_handshake_count = 0;
    _pending_data_count = 0;
    _online = false;

    INFO("BLEInterface: Stopped");
}

void BLEInterface::loop() {
    static double last_loop_log = 0;
    static bool local_mac_set = false;
    double now = Utilities::OS::time();

    // Lazy init: set local MAC once NimBLE has a valid random address
    if (!local_mac_set && _platform) {
        auto addr = _platform->getLocalAddress();
        if (!addr.isZero()) {
            _peer_manager.setLocalMac(addr.toBytes());
            local_mac_set = true;
            INFO("BLEInterface: Local MAC resolved: " + addr.toString());
        }
    }

    // Process any pending handshakes (deferred from callback for stack safety)
    if (_pending_handshake_count > 0) {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        for (size_t i = 0; i < _pending_handshake_count; i++) {
            const PendingHandshake& pending = _pending_handshake_pool[i];
            DEBUG("BLEInterface: Processing deferred handshake for " +
                  pending.identity.toHex().substr(0, 8) + "...");

            // Check for duplicate identity — only keep one connection per identity
            PeerInfo* existing = _peer_manager.getPeerByIdentity(pending.identity);
            if (existing && existing->isConnected() && existing->mac_address != pending.mac) {
                INFO("BLEInterface: Duplicate identity " + pending.identity.toHex().substr(0, 8) +
                     " - disconnecting new connection");
                // Disconnect the new (duplicate) connection
                PeerInfo* new_peer = _peer_manager.getPeerByMac(pending.mac);
                if (new_peer && new_peer->conn_handle != 0xFFFF) {
                    _platform->disconnect(new_peer->conn_handle);
                }
                _peer_manager.connectionFailed(pending.mac);
                continue;
            }

            // Update peer manager with identity
            _peer_manager.setPeerIdentity(pending.mac, pending.identity);
            _peer_manager.connectionSucceeded(pending.identity);

            // Create fragmenter for this peer
            PeerInfo* peer = _peer_manager.getPeerByIdentity(pending.identity);
            uint16_t mtu = peer ? peer->mtu : MTU::MINIMUM;

            // Find or allocate a fragmenter slot
            FragmenterSlot* fslot = nullptr;
            for (size_t j = 0; j < MAX_FRAGMENTERS; j++) {
                if (_fragmenter_pool[j].in_use && _fragmenter_pool[j].identity == pending.identity) {
                    fslot = &_fragmenter_pool[j];
                    break;
                }
            }
            if (!fslot) {
                for (size_t j = 0; j < MAX_FRAGMENTERS; j++) {
                    if (!_fragmenter_pool[j].in_use) {
                        fslot = &_fragmenter_pool[j];
                        break;
                    }
                }
            }
            if (fslot) {
                fslot->in_use = true;
                fslot->identity = pending.identity;
                fslot->fragmenter = BLEFragmenter(mtu);
            }

            INFO("BLEInterface: Handshake complete with " + pending.identity.toHex().substr(0, 8) +
                 "... (we are " + (pending.is_central ? "central" : "peripheral") + ")");
        }
        _pending_handshake_count = 0;
    }

    // Process any pending data fragments (deferred from callback for stack safety)
    if (_pending_data_count > 0) {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        size_t requeue_count = 0;
        for (size_t i = 0; i < _pending_data_count; i++) {
            Bytes& stored_id = _pending_data_pool[i].identity;
            // Check if this entry was MAC-keyed (size 6) and try to resolve to identity
            if (stored_id.size() == Limits::MAC_SIZE) {
                Bytes resolved = _identity_manager.getIdentityForMac(stored_id);
                if (resolved.size() == Limits::IDENTITY_SIZE) {
                    stored_id = resolved;
                } else {
                    // Still no identity — keep for next loop iteration
                    if (requeue_count != i) {
                        _pending_data_pool[requeue_count] = _pending_data_pool[i];
                    }
                    requeue_count++;
                    continue;
                }
            }
            _reassembler.processFragment(stored_id, _pending_data_pool[i].data);
        }
        _pending_data_count = requeue_count;
    }

    // Debug: log loop status every 10 seconds
    if (now - last_loop_log >= 10.0) {
        INFO("BLE: running=" + std::string(_platform && _platform->isRunning() ? "yes" : "no") +
             " scanning=" + std::string(_platform && _platform->isScanning() ? "yes" : "no") +
             " connected=" + std::to_string(_peer_manager.connectedCount()) +
             " peers=" + std::to_string(_peer_manager.getAllPeers().size()) +
             " heap=" + std::to_string(ESP.getFreeHeap()));
        last_loop_log = now;
    }

    if (!_platform || !_platform->isRunning()) {
        return;
    }

    // Platform loop
    _platform->loop();

    // Periodic advertising refresh (combat Android/system silent stops)
    if (_role == Role::PERIPHERAL || _role == Role::DUAL) {
        if (now - _last_advertising_refresh >= Timing::ADVERTISING_REFRESH_INTERVAL) {
            if (_platform && !_platform->isAdvertising()) {
                DEBUG("BLEInterface: Refreshing advertising");
                _platform->startAdvertising();
            }
            _last_advertising_refresh = now;
        }
    }

    // Periodic scanning (central mode)
    if (_role == Role::CENTRAL || _role == Role::DUAL) {
        if (now - _last_scan >= SCAN_INTERVAL) {
            performScan();
            _last_scan = now;
        }
    }

    // Keepalive processing
    if (now - _last_keepalive >= KEEPALIVE_INTERVAL) {
        sendKeepalives();
        _last_keepalive = now;
    }

    // Maintenance (cleanup, scores, timeouts)
    if (now - _last_maintenance >= MAINTENANCE_INTERVAL) {
        performMaintenance();
        _last_maintenance = now;
    }

    // Process discovered peers (connect attempts) — called OUTSIDE performMaintenance()
    // to avoid holding _mutex during the blocking _platform->connect() call.
    // The main loop's send_outgoing() acquires _mutex for packet sends — if we held
    // _mutex during a 3-6s connect, it would starve the main loop and trigger the WDT.
    processDiscoveredPeers();
}

//=============================================================================
// Data Transfer
//=============================================================================

void BLEInterface::send_outgoing(const Bytes& data) {
    if (!_platform || !_platform->isRunning()) {
        return;
    }

    // Non-blocking lock: if BLE task holds _mutex (maintenance, connect, etc.),
    // skip this send rather than blocking the main loop. Reticulum handles
    // retransmission at the transport layer.
    if (!_mutex.try_lock()) {
        TRACE("BLEInterface: send_outgoing skipped - BLE task busy");
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(_mutex, std::adopt_lock);

    // Get all connected peers
    auto connected_peers = _peer_manager.getConnectedPeers();

    if (connected_peers.empty()) {
        TRACE("BLEInterface: No connected peers, dropping packet");
        return;
    }

    // Count peers with identity
    size_t peers_with_identity = 0;
    for (PeerInfo* peer : connected_peers) {
        if (peer->hasIdentity()) {
            peers_with_identity++;
        }
    }
    DEBUG("BLEInterface: Sending to " + std::to_string(peers_with_identity) +
          "/" + std::to_string(connected_peers.size()) + " connected peers");

    // Send to all connected peers with identity
    for (PeerInfo* peer : connected_peers) {
        if (peer->hasIdentity()) {
            sendToPeer(peer->identity, data);
        }
    }

    // Track outgoing stats
    handle_outgoing(data);
}

bool BLEInterface::sendToPeer(const Bytes& peer_identity, const Bytes& data) {
    PeerInfo* peer = _peer_manager.getPeerByIdentity(peer_identity);
    if (!peer || !peer->isConnected()) {
        return false;
    }

    // Get or create fragmenter for this peer (linear search pool)
    FragmenterSlot* fslot = nullptr;
    for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
        if (_fragmenter_pool[i].in_use && _fragmenter_pool[i].identity == peer_identity) {
            fslot = &_fragmenter_pool[i];
            break;
        }
    }
    if (!fslot) {
        for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
            if (!_fragmenter_pool[i].in_use) {
                fslot = &_fragmenter_pool[i];
                fslot->in_use = true;
                fslot->identity = peer_identity;
                fslot->fragmenter = BLEFragmenter(peer->mtu);
                break;
            }
        }
    }
    if (!fslot) {
        WARNING("BLEInterface: Fragmenter pool full, cannot send to peer");
        return false;
    }

    // Update MTU if changed
    fslot->fragmenter.setMTU(peer->mtu);

    // Fragment the data
    std::vector<Bytes> fragments = fslot->fragmenter.fragment(data);

    INFO("BLEInterface: Sending " + std::to_string(fragments.size()) + " frags to " +
         peer_identity.toHex().substr(0, 8) + " via " + (peer->is_central ? "write" : "notify") +
         " conn=" + std::to_string(peer->conn_handle) + " mtu=" + std::to_string(peer->mtu));

    // Send each fragment
    bool all_sent = true;
    for (const Bytes& fragment : fragments) {
        bool sent = false;

        if (peer->is_central) {
            // We are central - write to peripheral (no response = non-blocking)
            // Reticulum handles retransmission, so BLE-level ACK is unnecessary
            // and write-with-response blocks until peer acknowledges or disconnects
            sent = _platform->write(peer->conn_handle, fragment, false);
        } else {
            // We are peripheral - notify central
            sent = _platform->notify(peer->conn_handle, fragment);
        }

        if (!sent) {
            WARNING("BLEInterface: Failed to send fragment to " +
                    peer_identity.toHex().substr(0, 8) + " conn=" +
                    std::to_string(peer->conn_handle));
            all_sent = false;
            break;
        }
    }

    if (!all_sent) {
        return false;
    }

    _peer_manager.recordPacketSent(peer_identity);
    return true;
}

//=============================================================================
// Status
//=============================================================================

size_t BLEInterface::peerCount() const {
    // Non-blocking: return 0 if BLE task is busy (avoids main loop WDT)
    if (!_mutex.try_lock()) return 0;
    size_t count = _peer_manager.connectedCount();
    _mutex.unlock();
    return count;
}

size_t BLEInterface::getConnectedPeerSummaries(PeerSummary* out, size_t max_count) const {
    if (!out || max_count == 0) return 0;

    // Non-blocking: return 0 if BLE task is busy (avoids main loop WDT)
    if (!_mutex.try_lock()) return 0;
    std::lock_guard<std::recursive_mutex> lock(_mutex, std::adopt_lock);

    // Cast away const for read-only access to non-const getConnectedPeers()
    auto& mutable_peer_manager = const_cast<BLE::BLEPeerManager&>(_peer_manager);
    auto connected_peers = mutable_peer_manager.getConnectedPeers();

    size_t count = 0;
    for (const auto* peer : connected_peers) {
        if (!peer || count >= max_count) break;

        PeerSummary& summary = out[count];

        // Format identity (first 12 hex chars) or empty if no identity
        // Look up identity from identity manager (where it's actually stored after handshake)
        Bytes identity = _identity_manager.getIdentityForMac(peer->mac_address);
        if (identity.size() == Limits::IDENTITY_SIZE) {
            std::string hex = identity.toHex();
            size_t len = (hex.length() >= 12) ? 12 : hex.length();
            memcpy(summary.identity, hex.c_str(), len);
            summary.identity[len] = '\0';
        } else {
            summary.identity[0] = '\0';
        }

        // Format MAC as AA:BB:CC:DD:EE:FF
        if (peer->mac_address.size() >= 6) {
            const uint8_t* mac = peer->mac_address.data();
            snprintf(summary.mac, sizeof(summary.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            summary.mac[0] = '\0';
        }

        summary.rssi = peer->rssi;
        count++;
    }

    return count;
}

std::map<std::string, float> BLEInterface::get_stats() const {
    std::map<std::string, float> stats;
    stats["central_connections"] = 0.0f;
    stats["peripheral_connections"] = 0.0f;

    try {
        // Non-blocking: return defaults if BLE task is busy (avoids main loop WDT)
        if (!_mutex.try_lock()) return stats;
        std::lock_guard<std::recursive_mutex> lock(_mutex, std::adopt_lock);

        // Count central vs peripheral connections
        int central_count = 0;
        int peripheral_count = 0;

        // Cast away const for read-only access to non-const getConnectedPeers()
        auto& mutable_peer_manager = const_cast<BLE::BLEPeerManager&>(_peer_manager);
        auto connected_peers = mutable_peer_manager.getConnectedPeers();
        for (const auto* peer : connected_peers) {
            if (peer && peer->is_central) {
                central_count++;
            } else if (peer) {
                peripheral_count++;
            }
        }

        stats["central_connections"] = (float)central_count;
        stats["peripheral_connections"] = (float)peripheral_count;
    } catch (...) {
        // Ignore errors during BLE state changes
    }

    return stats;
}

//=============================================================================
// Platform Callbacks
//=============================================================================

void BLEInterface::setupCallbacks() {
    _platform->setOnScanResult([this](const ScanResult& result) {
        onScanResult(result);
    });

    _platform->setOnConnected([this](const ConnectionHandle& conn) {
        onConnected(conn);
    });

    _platform->setOnDisconnected([this](const ConnectionHandle& conn, uint8_t reason) {
        onDisconnected(conn, reason);
    });

    _platform->setOnMTUChanged([this](const ConnectionHandle& conn, uint16_t mtu) {
        onMTUChanged(conn, mtu);
    });

    _platform->setOnServicesDiscovered([this](const ConnectionHandle& conn, bool success) {
        onServicesDiscovered(conn, success);
    });

    _platform->setOnDataReceived([this](const ConnectionHandle& conn, const Bytes& data) {
        onDataReceived(conn, data);
    });

    _platform->setOnCentralConnected([this](const ConnectionHandle& conn) {
        onCentralConnected(conn);
    });

    _platform->setOnCentralDisconnected([this](const ConnectionHandle& conn) {
        onCentralDisconnected(conn);
    });

    _platform->setOnWriteReceived([this](const ConnectionHandle& conn, const Bytes& data) {
        onWriteReceived(conn, data);
    });

    // Identity manager callbacks
    _identity_manager.setHandshakeCompleteCallback(
        [this](const Bytes& mac, const Bytes& identity, bool is_central) {
            onHandshakeComplete(mac, identity, is_central);
        });

    _identity_manager.setHandshakeFailedCallback(
        [this](const Bytes& mac, const std::string& reason) {
            onHandshakeFailed(mac, reason);
        });

    _identity_manager.setMacRotationCallback(
        [this](const Bytes& old_mac, const Bytes& new_mac, const Bytes& identity) {
            onMacRotation(old_mac, new_mac, identity);
        });

    // Reassembler callbacks
    _reassembler.setReassemblyCallback(
        [this](const Bytes& peer_identity, const Bytes& packet) {
            onPacketReassembled(peer_identity, packet);
        });

    _reassembler.setTimeoutCallback(
        [this](const Bytes& peer_identity, const std::string& reason) {
            onReassemblyTimeout(peer_identity, reason);
        });
}

void BLEInterface::onScanResult(const ScanResult& result) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (!result.has_reticulum_service) {
        return;
    }

    Bytes mac = result.address.toBytes();

    // Check if identity prefix suggests this is a known peer at a new MAC (rotation)
    if (result.identity_prefix.size() >= 3) {
        Bytes known_identity = _identity_manager.findIdentityByPrefix(result.identity_prefix);
        if (known_identity.size() == Limits::IDENTITY_SIZE) {
            Bytes old_mac = _identity_manager.getMacForIdentity(known_identity);
            if (old_mac.size() > 0 && old_mac != mac) {
                // MAC rotation detected! Update mapping
                INFO("BLEInterface: MAC rotation detected for identity " +
                     known_identity.toHex().substr(0, 8) + "...: " +
                     BLEAddress(old_mac.data()).toString() + " -> " +
                     result.address.toString());
                _identity_manager.updateMacForIdentity(known_identity, mac);
            }
        }
    }

    // Add to peer manager with address type
    _peer_manager.addDiscoveredPeer(mac, result.rssi, result.address.type);

    INFO("BLEInterface: Discovered Reticulum peer " + result.address.toString() +
         " type=" + std::to_string(result.address.type) +
         " RSSI=" + std::to_string(result.rssi) + " name=" + result.name);
}

void BLEInterface::onConnected(const ConnectionHandle& conn) {
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);

        Bytes mac = conn.peer_address.toBytes();

        // Update peer state
        _peer_manager.setPeerState(mac, PeerState::HANDSHAKING);
        _peer_manager.setPeerHandle(mac, conn.handle);

        // Set MTU from connection (onMTUChange only fires for peripheral connections)
        if (conn.mtu > 0) {
            _peer_manager.setPeerMTU(mac, conn.mtu);
        }

        // Mark as central connection (we initiated the connection)
        PeerInfo* peer = _peer_manager.getPeerByMac(mac);
        if (peer) {
            peer->is_central = true;  // We ARE central in this connection
        }

        INFO("BLE: Connected to " + conn.peer_address.toString() +
              " handle=" + std::to_string(conn.handle) +
              " mtu=" + std::to_string(conn.mtu) + " (we are central)");
    }  // _mutex released BEFORE blocking GATT service discovery

    // Discover services — this does blocking GATT reads (3-15s) and must NOT
    // hold _mutex, otherwise the NimBLE host task and main loop both block.
    _platform->discoverServices(conn.handle);
}

void BLEInterface::onDisconnected(const ConnectionHandle& conn, uint8_t reason) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    Bytes mac = conn.peer_address.toBytes();
    Bytes identity = _identity_manager.getIdentityForMac(mac);

    if (identity.size() > 0) {
        // Clean up identity-keyed peer
        for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
            if (_fragmenter_pool[i].in_use && _fragmenter_pool[i].identity == identity) {
                _fragmenter_pool[i].clear();
                break;
            }
        }
        _reassembler.clearForPeer(identity);
        _peer_manager.setPeerHandle(identity, 0xFFFF);
        _peer_manager.setPeerState(identity, PeerState::DISCOVERED);
    } else {
        // Peer might still be in CONNECTING state (no identity yet)
        // Reset to DISCOVERED so we can try again
        _peer_manager.connectionFailed(mac);
    }

    _identity_manager.removeMapping(mac);

    INFO("BLE: Disconnected from " + conn.peer_address.toString() +
          " reason: " + std::to_string(reason));
}

void BLEInterface::onMTUChanged(const ConnectionHandle& conn, uint16_t mtu) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    Bytes mac = conn.peer_address.toBytes();
    _peer_manager.setPeerMTU(mac, mtu);

    // Update fragmenter if exists
    Bytes identity = _identity_manager.getIdentityForMac(mac);
    if (identity.size() > 0) {
        for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
            if (_fragmenter_pool[i].in_use && _fragmenter_pool[i].identity == identity) {
                _fragmenter_pool[i].fragmenter.setMTU(mtu);
                break;
            }
        }
    }

    DEBUG("BLEInterface: MTU changed to " + std::to_string(mtu) +
          " for " + conn.peer_address.toString());
}

void BLEInterface::onServicesDiscovered(const ConnectionHandle& conn, bool success) {
    if (!success) {
        WARNING("BLEInterface: Service discovery failed for " + conn.peer_address.toString());

        // Clean up peer state under brief lock
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            Bytes mac = conn.peer_address.toBytes();
            _peer_manager.connectionFailed(mac);
        }

        // Try to disconnect (may be no-op if already disconnected)
        _platform->disconnect(conn.handle);
        return;
    }

    INFO("BLE: Services discovered for " + conn.peer_address.toString());

    // All operations below are blocking GATT ops — do NOT hold _mutex.
    // Holding _mutex during these blocks the NimBLE host task (which needs
    // _mutex for its own callbacks), causing "BLE stack stuck" detection,
    // and blocks the main loop's peer status queries, causing WDT triggers.

    // Enable notifications on TX characteristic
    _platform->enableNotifications(conn.handle, true);

    // Protocol v2.2: Read peer's identity characteristic before sending ours
    // This matches the Kotlin implementation's 4-step handshake
    if (conn.identity_handle != 0) {
        Bytes mac = conn.peer_address.toBytes();
        uint16_t handle = conn.handle;

        _platform->read(conn.handle, conn.identity_handle,
            [this, mac, handle](OperationResult result, const Bytes& identity) {
                if (result == OperationResult::SUCCESS &&
                    identity.size() == Limits::IDENTITY_SIZE) {
                    DEBUG("BLEInterface: Read peer identity: " + identity.toHex().substr(0, 8) + "...");

                    // Store the peer's identity - handshake complete for receiving direction
                    _identity_manager.completeHandshake(mac, identity, true);

                    // Send our identity to the peer's IDENTITY_CHAR.
                    // Use Write Without Response (false) to avoid blocking —
                    // the handshake is already complete from our side.
                    if (_identity_manager.hasLocalIdentity()) {
                        ConnectionHandle c = _platform->getConnection(handle);
                        if (c.identity_handle != 0) {
                            _platform->writeCharacteristic(handle, c.identity_handle,
                                _identity_manager.getLocalIdentity(), false);
                        }
                        DEBUG("BLEInterface: Sent identity to peer");
                    }
                } else {
                    WARNING("BLEInterface: Failed to read peer identity, trying write-based handshake");
                    // Fall back to old behavior - initiate handshake and wait for response
                    ConnectionHandle conn = _platform->getConnection(handle);
                    if (conn.handle != 0) {
                        initiateHandshake(conn);
                    }
                }
            });
    } else {
        // No identity characteristic - fall back to write-only handshake (Protocol v1)
        DEBUG("BLEInterface: No identity characteristic, using v1 fallback");
        initiateHandshake(conn);
    }
}

void BLEInterface::onDataReceived(const ConnectionHandle& conn, const Bytes& data) {
    // Called when we receive notification from peripheral (we are central)
    handleIncomingData(conn, data);
}

void BLEInterface::onCentralConnected(const ConnectionHandle& conn) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    Bytes mac = conn.peer_address.toBytes();

    // Update peer manager
    _peer_manager.addDiscoveredPeer(mac, 0);
    _peer_manager.setPeerState(mac, PeerState::HANDSHAKING);
    _peer_manager.setPeerHandle(mac, conn.handle);

    // Mark as peripheral connection (they are central, we are peripheral)
    PeerInfo* peer = _peer_manager.getPeerByMac(mac);
    if (peer) {
        peer->is_central = false;  // We are NOT central in this connection
    }

    INFO("BLEInterface: Central connected: " + conn.peer_address.toString() +
          " handle=" + std::to_string(conn.handle) + " (we are peripheral)");
}

void BLEInterface::onCentralDisconnected(const ConnectionHandle& conn) {
    onDisconnected(conn, 0);
}

void BLEInterface::onWriteReceived(const ConnectionHandle& conn, const Bytes& data) {
    // Called when central writes to our RX characteristic (we are peripheral)
    handleIncomingData(conn, data);
}

//=============================================================================
// Handshake Callbacks
//=============================================================================

void BLEInterface::onHandshakeComplete(const Bytes& mac, const Bytes& identity, bool is_central) {
    // Lock before modifying queue - protects against race with loop()
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Queue the handshake for processing in loop() to avoid stack overflow in NimBLE callback
    // The NimBLE task has limited stack space, so we defer heavy processing
    if (_pending_handshake_count >= MAX_PENDING_HANDSHAKES) {
        WARNING("BLEInterface: Pending handshake queue full, dropping handshake");
        return;
    }
    PendingHandshake& pending = _pending_handshake_pool[_pending_handshake_count];
    pending.mac = mac;
    pending.identity = identity;
    pending.is_central = is_central;
    _pending_handshake_count++;
    DEBUG("BLEInterface::onHandshakeComplete: Queued handshake for deferred processing");
}

void BLEInterface::onHandshakeFailed(const Bytes& mac, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    WARNING("BLEInterface: Handshake failed with " +
            BLEAddress(mac.data()).toString() + ": " + reason);

    _peer_manager.connectionFailed(mac);
}

void BLEInterface::onMacRotation(const Bytes& old_mac, const Bytes& new_mac, const Bytes& identity) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    INFO("BLEInterface: MAC rotation detected for identity " +
         identity.toHex().substr(0, 8) + "...: " +
         BLEAddress(old_mac.data()).toString() + " -> " +
         BLEAddress(new_mac.data()).toString());

    // Update peer manager with new MAC
    _peer_manager.updatePeerMac(identity, new_mac);

    // Fragmenter is keyed by identity, so it stays valid after MAC rotation
    for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
        if (_fragmenter_pool[i].in_use && _fragmenter_pool[i].identity == identity) {
            DEBUG("BLEInterface: Fragmenter preserved for rotated identity");
            break;
        }
    }
}

//=============================================================================
// Reassembly Callbacks
//=============================================================================

void BLEInterface::onPacketReassembled(const Bytes& peer_identity, const Bytes& packet) {
    // Packet reassembly complete - pass to transport
    _peer_manager.recordPacketReceived(peer_identity);
    handle_incoming(packet);
}

void BLEInterface::onReassemblyTimeout(const Bytes& peer_identity, const std::string& reason) {
    WARNING("BLEInterface: Reassembly timeout for " +
            peer_identity.toHex().substr(0, 8) + ": " + reason);
}

//=============================================================================
// Internal Operations
//=============================================================================

void BLEInterface::performScan() {
    if (!_platform || _platform->isScanning()) {
        return;
    }

    // Only scan if we have room for more connections
    if (_peer_manager.connectedCount() >= _max_connections) {
        return;
    }

    _platform->startScan(5000);  // 5 second scan
}

void BLEInterface::processDiscoveredPeers() {
    // Don't attempt connections when memory is critically low
    // BLE connection setup requires significant heap allocation
#ifdef ARDUINO
    if (ESP.getFreeHeap() < 30000) {
        static uint32_t last_low_mem_warn = 0;
        if (millis() - last_low_mem_warn > 10000) {
            WARNING("BLEInterface: Skipping connection attempts - low memory");
            last_low_mem_warn = millis();
        }
        return;
    }
#endif

    // Don't try to connect while scanning - BLE stack will return "busy"
    if (_platform->isScanning()) {
        return;
    }

    // Cooldown after connection attempts to let BLE stack settle
    double now = Utilities::OS::time();
    if (now - _last_connection_attempt < CONNECTION_COOLDOWN) {
        return;  // Still in cooldown period
    }

    // Prepare connection candidate under short-lived lock — DO NOT hold _mutex
    // during the blocking _platform->connect() call. The main loop calls
    // send_outgoing() which acquires _mutex, and a 3-6s connect would starve it.
    BLEAddress addr;
    Bytes candidate_mac;
    bool should_connect = false;
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);

        PeerInfo* candidate = _peer_manager.getBestConnectionCandidate();

        // Debug: log all peers and why they may not be candidates
        static double last_peer_log = 0;
        if (now - last_peer_log >= 10.0) {
            auto all_peers = _peer_manager.getAllPeers();
            INFO("BLE: Peers=" + std::to_string(all_peers.size()) +
                 " localMAC=" + _peer_manager.getLocalMac().toString());
            for (PeerInfo* peer : all_peers) {
                if (peer->mac_address.size() < Limits::MAC_SIZE) {
                    WARNING("BLE: Peer with empty MAC, state=" +
                            std::to_string(static_cast<int>(peer->state)));
                    continue;
                }
                bool should_initiate = _peer_manager.shouldInitiateConnection(peer->mac_address);
                INFO("BLE: Peer " + BLEAddress(peer->mac_address.data()).toString() +
                     " state=" + std::to_string(static_cast<int>(peer->state)) +
                     " shouldInit=" + std::string(should_initiate ? "yes" : "no") +
                     " score=" + std::to_string(peer->score));
            }
            last_peer_log = now;
        }

        if (candidate && candidate->mac_address.size() >= Limits::MAC_SIZE) {
            INFO("BLE: Connection candidate: " + BLEAddress(candidate->mac_address.data()).toString() +
                  " type=" + std::to_string(candidate->address_type) +
                  " canAccept=" + std::string(_peer_manager.canAcceptConnection() ? "yes" : "no"));
        }

        if (candidate && _peer_manager.canAcceptConnection()) {
            _peer_manager.setPeerState(candidate->mac_address, PeerState::CONNECTING);
            candidate->connection_attempts++;

            // Copy address data before releasing lock
            addr = BLEAddress(candidate->mac_address.data(), candidate->address_type);
            candidate_mac = candidate->mac_address;
            should_connect = true;

            INFO("BLEInterface: Connecting to " + addr.toString() + " type=" + std::to_string(candidate->address_type));
            _last_connection_attempt = now;
        }
    }  // _mutex released here — before the blocking connect

    if (should_connect) {
        // Blocking connect (3-6s) — _mutex NOT held, main loop can send packets
        if (!_platform->connect(addr, 3000)) {
            WARNING("BLEInterface: Connection attempt failed immediately");
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            _peer_manager.connectionFailed(candidate_mac);
        }
    }
}

void BLEInterface::sendKeepalives() {
    // Send empty keepalive to maintain connections
    Bytes keepalive(1);
    keepalive.writable(1)[0] = 0x00;

    auto connected = _peer_manager.getConnectedPeers();
    for (PeerInfo* peer : connected) {
        if (peer->hasIdentity()) {
            // Verify connection handle is valid before sending
            if (peer->conn_handle == 0xFFFF) {
                WARNING("BLEInterface: Peer " + peer->identity.toHex().substr(0, 8) +
                        " state=CONNECTED but conn_handle=INVALID, resetting");
                _peer_manager.setPeerState(peer->identity, PeerState::DISCOVERED);
                continue;
            }

            // Cross-check with platform connection table
            ConnectionHandle platformConn = _platform->getConnection(peer->conn_handle);
            if (!platformConn.isValid()) {
                WARNING("BLEInterface: Peer " + peer->identity.toHex().substr(0, 8) +
                        " has stale conn_handle=" + std::to_string(peer->conn_handle) +
                        ", resetting");
                _peer_manager.setPeerHandle(peer->identity, 0xFFFF);
                _peer_manager.setPeerState(peer->identity, PeerState::DISCOVERED);
                continue;
            }

            bool sent = false;
            if (peer->is_central) {
                sent = _platform->write(peer->conn_handle, keepalive, false);
            } else {
                sent = _platform->notify(peer->conn_handle, keepalive);
            }

            if (sent) {
                peer->consecutive_keepalive_failures = 0;
            } else {
                peer->consecutive_keepalive_failures++;
                if (peer->consecutive_keepalive_failures >= PeerInfo::MAX_KEEPALIVE_FAILURES) {
                    WARNING("BLEInterface: Keepalive failed " +
                            std::to_string(peer->consecutive_keepalive_failures) +
                            " times, disconnecting " + peer->identity.toHex().substr(0, 8));
                    _platform->disconnect(peer->conn_handle);

                    // Force-remove peer if disconnect keeps failing
                    if (peer->consecutive_keepalive_failures >= PeerInfo::MAX_KEEPALIVE_FAILURES * 2) {
                        WARNING("BLEInterface: Force-removing unresponsive peer " +
                                peer->identity.toHex().substr(0, 8));
                        _identity_manager.removeMapping(peer->mac_address);
                        _peer_manager.removePeer(peer->identity);
                    }
                }
            }
        }
    }
}

void BLEInterface::performMaintenance() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Check reassembly timeouts
    _reassembler.checkTimeouts();

    // Check handshake timeouts
    _identity_manager.checkTimeouts();

    // Check blacklist expirations
    _peer_manager.checkBlacklistExpirations();

    // Recalculate peer scores
    _peer_manager.recalculateScores();

    // Clean up stale peers (also marks zombies as DISCONNECTING)
    _peer_manager.cleanupStalePeers();

    // Force-disconnect zombie peers
    {
        auto all = _peer_manager.getAllPeers();
        for (PeerInfo* peer : all) {
            if (peer->state == PeerState::DISCONNECTING && peer->conn_handle != 0xFFFF) {
                WARNING("BLEInterface: Force-disconnecting zombie peer " +
                        peer->identity.toHex().substr(0, 8));
                _platform->disconnect(peer->conn_handle);
            }
        }
    }

    // Clean up fragmenters for peers that no longer exist
    for (size_t i = 0; i < MAX_FRAGMENTERS; i++) {
        if (_fragmenter_pool[i].in_use) {
            if (!_peer_manager.getPeerByIdentity(_fragmenter_pool[i].identity)) {
                TRACE("BLEInterface: Cleaned up orphaned fragmenter for " +
                      _fragmenter_pool[i].identity.toHex().substr(0, 8));
                _reassembler.clearForPeer(_fragmenter_pool[i].identity);
                _fragmenter_pool[i].clear();
            }
        }
    }

    // NOTE: processDiscoveredPeers() is called separately from loop() to avoid
    // holding _mutex during blocking connect operations (3-6 seconds). If held
    // here, the main loop's send_outgoing() would block on _mutex, triggering
    // the Task Watchdog.
}

void BLEInterface::handleIncomingData(const ConnectionHandle& conn, const Bytes& data) {
    // Hot path - no logging to avoid blocking main loop
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    Bytes mac = conn.peer_address.toBytes();
    bool is_central = (conn.local_role == Role::CENTRAL);

    // First check if this is an identity handshake
    if (_identity_manager.processReceivedData(mac, data, is_central)) {
        return;
    }

    // Check for keepalive (1 byte, value 0x00)
    if (data.size() == 1 && data.data()[0] == 0x00) {
        _peer_manager.updateLastActivity(_identity_manager.getIdentityForMac(mac));
        return;
    }

    // Queue data for deferred processing (avoid stack overflow in NimBLE callback)
    Bytes identity = _identity_manager.getIdentityForMac(mac);
    if (identity.size() == 0) {
        // No identity yet - buffer data for replay after handshake completes
        // This handles the race where data arrives before deferred handshake is processed
        TRACE("BLEInterface: Buffering data from peer without identity (handshake pending)");
        if (_pending_data_count < MAX_PENDING_DATA) {
            PendingData& pending = _pending_data_pool[_pending_data_count];
            pending.identity = mac;  // Use MAC as temporary key
            pending.data = data;
            _pending_data_count++;
        }
        return;
    }

    if (_pending_data_count >= MAX_PENDING_DATA) {
        WARNING("BLEInterface: Pending data queue full, dropping data");
        return;
    }

    PendingData& pending = _pending_data_pool[_pending_data_count];
    pending.identity = identity;
    pending.data = data;
    _pending_data_count++;
}

void BLEInterface::initiateHandshake(const ConnectionHandle& conn) {
    Bytes mac = conn.peer_address.toBytes();

    // Get handshake data (our identity)
    Bytes handshake = _identity_manager.initiateHandshake(mac);

    if (handshake.size() > 0) {
        // Write our identity to peer's RX characteristic (no-response to avoid blocking)
        _platform->write(conn.handle, handshake, false);

        DEBUG("BLEInterface: Sent identity handshake to " + conn.peer_address.toString());
    }
}

//=============================================================================
// FreeRTOS Task Support
//=============================================================================

#ifdef ARDUINO

void BLEInterface::ble_task(void* param) {
    BLEInterface* self = static_cast<BLEInterface*>(param);
    Serial.printf("BLE task started on core %d\n", xPortGetCoreID());

    // Subscribe BLE task to Task Watchdog — detects BLE deadlocks
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();

        // Run the BLE loop (already has internal mutex protection)
        self->loop();

        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool BLEInterface::start_task(int priority, int core) {
    if (_task_handle != nullptr) {
        WARNING("BLEInterface: Task already running");
        return true;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        ble_task,
        "ble",
        12288,          // 12KB stack (string ops in debug logs need headroom)
        this,
        priority,
        &_task_handle,
        core
    );

    if (result != pdPASS) {
        ERROR("BLEInterface: Failed to create BLE task");
        return false;
    }

    Serial.printf("BLE task created with priority %d on core %d\n", priority, core);
    return true;
}

#else

// Non-Arduino stub
bool BLEInterface::start_task(int priority, int core) {
    WARNING("BLEInterface: Task mode not supported on this platform");
    return false;
}

#endif

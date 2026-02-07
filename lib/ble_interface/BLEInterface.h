/**
 * @file BLEInterface.h
 * @brief BLE-Reticulum Protocol v2.2 interface for microReticulum
 *
 * Main BLEInterface class that integrates with the Reticulum transport layer.
 * Supports dual-mode operation (central + peripheral) for mesh networking.
 *
 * Usage:
 *   BLEInterface ble("ble0");
 *   ble.setDeviceName("my-node");
 *   ble.setLocalIdentity(identity.hash());
 *
 *   Interface interface(&ble);
 *   interface.start();
 *   Transport::register_interface(interface);
 */
#pragma once

#include "Interface.h"
#include "Bytes.h"
#include "Type.h"
#include "BLE/BLETypes.h"
#include "BLE/BLEPlatform.h"
#include "BLE/BLEFragmenter.h"
#include "BLE/BLEReassembler.h"
#include "BLE/BLEPeerManager.h"
#include "BLE/BLEIdentityManager.h"

#include <map>
#include <mutex>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

/**
 * @brief Reticulum BLE Interface
 *
 * Implements the BLE-Reticulum protocol v2.2 as a microReticulum interface.
 * Manages BLE connections, fragmentation, and peer discovery.
 */
class BLEInterface : public RNS::InterfaceImpl {
public:
    // Protocol constants
    static constexpr uint32_t BITRATE_GUESS = 100000;   // ~100 kbps effective throughput
    static constexpr uint16_t HW_MTU_DEFAULT = 512;     // Default after MTU negotiation

    // Timing constants
    static constexpr double SCAN_INTERVAL = 5.0;        // Seconds between scans
    static constexpr double KEEPALIVE_INTERVAL = 15.0;  // Seconds between keepalives
    static constexpr double MAINTENANCE_INTERVAL = 1.0; // Seconds between maintenance
    static constexpr double CONNECTION_COOLDOWN = 3.0;  // Seconds to wait after connection failure

public:
    /**
     * @brief Construct a BLE interface
     * @param name Interface name (e.g., "ble0")
     */
    explicit BLEInterface(const char* name = "BLEInterface");

    virtual ~BLEInterface();

    //=========================================================================
    // Configuration (call before start())
    //=========================================================================

    /**
     * @brief Set the BLE role
     * @param role CENTRAL, PERIPHERAL, or DUAL (default: DUAL)
     */
    void setRole(RNS::BLE::Role role);

    /**
     * @brief Set the advertised device name
     * @param name Device name (max ~8 characters recommended)
     */
    void setDeviceName(const std::string& name);

    /**
     * @brief Set our local identity hash
     *
     * Required for the identity handshake protocol.
     * Should be the first 16 bytes of the transport identity hash.
     *
     * @param identity 16-byte identity hash
     */
    void setLocalIdentity(const RNS::Bytes& identity);

    /**
     * @brief Set maximum connections
     * @param max Maximum simultaneous connections (default: 7)
     */
    void setMaxConnections(uint8_t max);

    //=========================================================================
    // InterfaceImpl Overrides
    //=========================================================================

    virtual bool start() override;
    virtual void stop() override;
    virtual void loop() override;

    virtual std::string toString() const override {
        return "BLEInterface[" + _name + "/" + _device_name + "]";
    }

    /**
     * @brief Get interface statistics
     * @return Map with central_connections and peripheral_connections counts
     */
    virtual std::map<std::string, float> get_stats() const override;

    //=========================================================================
    // Status
    //=========================================================================

    /**
     * @brief Summary info for a connected peer (fixed-size, no heap allocation)
     */
    struct PeerSummary {
        char identity[14];    // First 12 hex chars + null, or empty if unknown
        char mac[18];         // "AA:BB:CC:DD:EE:FF" format
        int8_t rssi;
    };
    static constexpr size_t MAX_PEER_SUMMARIES = 8;

    /**
     * @brief Get count of connected peers
     */
    size_t peerCount() const;

    /**
     * @brief Get summaries of connected peers (for UI display)
     * @param out Pre-allocated array to fill
     * @param max_count Maximum entries to fill
     * @return Actual number of entries filled
     */
    size_t getConnectedPeerSummaries(PeerSummary* out, size_t max_count) const;

    /**
     * @brief Check if BLE is running
     */
    bool isRunning() const { return _platform && _platform->isRunning(); }

    /**
     * @brief Start BLE on its own FreeRTOS task
     *
     * This allows BLE operations to run independently of the main loop,
     * preventing UI freezes during scans and connections.
     *
     * @param priority Task priority (default 1)
     * @param core Core to pin the task to (default 0, where BT controller runs)
     * @return true if task started successfully
     */
    bool start_task(int priority = 1, int core = 0);

    /**
     * @brief Check if BLE is running on its own task
     */
    bool is_task_running() const { return _task_handle != nullptr; }

protected:
    virtual void send_outgoing(const RNS::Bytes& data) override;

private:
    //=========================================================================
    // Platform Callbacks
    //=========================================================================

    void onScanResult(const RNS::BLE::ScanResult& result);
    void onConnected(const RNS::BLE::ConnectionHandle& conn);
    void onDisconnected(const RNS::BLE::ConnectionHandle& conn, uint8_t reason);
    void onMTUChanged(const RNS::BLE::ConnectionHandle& conn, uint16_t mtu);
    void onServicesDiscovered(const RNS::BLE::ConnectionHandle& conn, bool success);
    void onDataReceived(const RNS::BLE::ConnectionHandle& conn, const RNS::Bytes& data);
    void onCentralConnected(const RNS::BLE::ConnectionHandle& conn);
    void onCentralDisconnected(const RNS::BLE::ConnectionHandle& conn);
    void onWriteReceived(const RNS::BLE::ConnectionHandle& conn, const RNS::Bytes& data);

    //=========================================================================
    // Handshake Callbacks
    //=========================================================================

    void onHandshakeComplete(const RNS::Bytes& mac, const RNS::Bytes& identity, bool is_central);
    void onHandshakeFailed(const RNS::Bytes& mac, const std::string& reason);
    void onMacRotation(const RNS::Bytes& old_mac, const RNS::Bytes& new_mac, const RNS::Bytes& identity);

    //=========================================================================
    // Reassembly Callbacks
    //=========================================================================

    void onPacketReassembled(const RNS::Bytes& peer_identity, const RNS::Bytes& packet);
    void onReassemblyTimeout(const RNS::Bytes& peer_identity, const std::string& reason);

    //=========================================================================
    // Internal Operations
    //=========================================================================

    void setupCallbacks();
    void performScan();
    void processDiscoveredPeers();
    void sendKeepalives();
    void performMaintenance();

    /**
     * @brief Send data to a specific peer (with fragmentation)
     */
    bool sendToPeer(const RNS::Bytes& peer_identity, const RNS::Bytes& data);

    /**
     * @brief Process incoming data from a peer
     */
    void handleIncomingData(const RNS::BLE::ConnectionHandle& conn, const RNS::Bytes& data);

    /**
     * @brief Initiate handshake for a new connection
     */
    void initiateHandshake(const RNS::BLE::ConnectionHandle& conn);

    //=========================================================================
    // Configuration
    //=========================================================================

    RNS::BLE::Role _role = RNS::BLE::Role::DUAL;
    std::string _device_name = "RNS-Node";
    uint8_t _max_connections = RNS::BLE::Limits::MAX_PEERS;
    RNS::Bytes _local_identity;

    //=========================================================================
    // Components
    //=========================================================================

    RNS::BLE::IBLEPlatform::Ptr _platform;
    RNS::BLE::BLEPeerManager _peer_manager;
    RNS::BLE::BLEIdentityManager _identity_manager;
    RNS::BLE::BLEReassembler _reassembler;

    // Per-peer fragmenters (keyed by identity)
    std::map<RNS::Bytes, RNS::BLE::BLEFragmenter> _fragmenters;

    //=========================================================================
    // State
    //=========================================================================

    double _last_scan = 0;
    double _last_keepalive = 0;
    double _last_maintenance = 0;
    double _last_connection_attempt = 0;  // Cooldown after connection failures

    // Pending handshake completions (deferred from callback to loop for stack safety)
    static constexpr size_t MAX_PENDING_HANDSHAKES = 32;
    struct PendingHandshake {
        RNS::Bytes mac;
        RNS::Bytes identity;
        bool is_central;
    };
    std::vector<PendingHandshake> _pending_handshakes;

    // Pending data fragments (deferred from callback to loop for stack safety)
    static constexpr size_t MAX_PENDING_DATA = 64;
    struct PendingData {
        RNS::Bytes identity;
        RNS::Bytes data;
    };
    std::vector<PendingData> _pending_data;

    // Thread safety for callbacks from BLE stack
    // Using recursive_mutex because handleIncomingData holds the lock while
    // calling processReceivedData, which can trigger onHandshakeComplete callback
    // that also needs the lock
    mutable std::recursive_mutex _mutex;

    //=========================================================================
    // FreeRTOS Task Support
    //=========================================================================

    TaskHandle_t _task_handle = nullptr;
    static void ble_task(void* param);
};

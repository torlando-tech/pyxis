/**
 * @file NimBLEPlatform.h
 * @brief NimBLE-Arduino implementation of IBLEPlatform for ESP32
 *
 * This implementation uses the NimBLE-Arduino library to provide BLE
 * functionality on ESP32 devices. It supports both central and peripheral
 * modes simultaneously (dual-mode operation).
 */
#pragma once

#include "../BLEPlatform.h"
#include "../BLEOperationQueue.h"

// Only compile for ESP32 with NimBLE
#if defined(ESP32) && (defined(USE_NIMBLE) || defined(CONFIG_BT_NIMBLE_ENABLED))

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Undefine NimBLE's backward compatibility macros to avoid conflict with our types
#undef BLEAddress

#include <atomic>
#include <map>
#include <vector>

namespace RNS { namespace BLE {

//=============================================================================
// State Machine Enums for Dual-Role BLE Operation
//=============================================================================

/**
 * @brief Master role states (Central - scanning/connecting)
 */
enum class MasterState : uint8_t {
    IDLE,           ///< No master operations
    SCAN_STARTING,  ///< Gap scan start requested
    SCANNING,       ///< Actively scanning
    SCAN_STOPPING,  ///< Gap scan stop requested
    CONN_STARTING,  ///< Connection initiation requested
    CONNECTING,     ///< Connection in progress
    CONN_CANCELING  ///< Connection cancel requested
};

/**
 * @brief Slave role states (Peripheral - advertising)
 */
enum class SlaveState : uint8_t {
    IDLE,           ///< Not advertising
    ADV_STARTING,   ///< Gap adv start requested
    ADVERTISING,    ///< Actively advertising
    ADV_STOPPING    ///< Gap adv stop requested
};

/**
 * @brief GAP coordinator state (overall BLE subsystem)
 */
enum class GAPState : uint8_t {
    UNINITIALIZED,   ///< BLE not started
    INITIALIZING,    ///< NimBLE init in progress
    READY,           ///< Idle, ready for operations
    MASTER_PRIORITY, ///< Master operation in progress, slave paused
    SLAVE_PRIORITY,  ///< Slave operation in progress, master paused
    TRANSITIONING,   ///< State change in progress
    ERROR_RECOVERY   ///< Recovering from error
};

// State name helpers for logging
const char* masterStateName(MasterState state);
const char* slaveStateName(SlaveState state);
const char* gapStateName(GAPState state);

/**
 * @brief NimBLE-Arduino implementation of IBLEPlatform
 */
class NimBLEPlatform : public IBLEPlatform,
                       public BLEOperationQueue,
                       public NimBLEServerCallbacks,
                       public NimBLECharacteristicCallbacks,
                       public NimBLEClientCallbacks,
                       public NimBLEScanCallbacks {
public:
    NimBLEPlatform();
    virtual ~NimBLEPlatform();

    //=========================================================================
    // IBLEPlatform Implementation
    //=========================================================================

    // Lifecycle
    bool initialize(const PlatformConfig& config) override;
    bool start() override;
    void stop() override;
    void loop() override;
    void shutdown() override;
    bool isRunning() const override;

    // Central mode - Scanning
    bool startScan(uint16_t duration_ms = 0) override;
    void stopScan() override;
    bool isScanning() const override;

    // Central mode - Connections
    bool connect(const BLEAddress& address, uint16_t timeout_ms = 10000) override;
    bool disconnect(uint16_t conn_handle) override;
    void disconnectAll() override;
    bool requestMTU(uint16_t conn_handle, uint16_t mtu) override;
    bool discoverServices(uint16_t conn_handle) override;

    // Peripheral mode
    bool startAdvertising() override;
    void stopAdvertising() override;
    bool isAdvertising() const override;
    bool setAdvertisingData(const Bytes& data) override;
    void setIdentityData(const Bytes& identity) override;

    // GATT Operations
    bool write(uint16_t conn_handle, const Bytes& data, bool response = true) override;
    bool read(uint16_t conn_handle, uint16_t char_handle,
              std::function<void(OperationResult, const Bytes&)> callback) override;
    bool enableNotifications(uint16_t conn_handle, bool enable) override;
    bool notify(uint16_t conn_handle, const Bytes& data) override;
    bool notifyAll(const Bytes& data) override;

    // Connection management
    std::vector<ConnectionHandle> getConnections() const override;
    ConnectionHandle getConnection(uint16_t handle) const override;
    size_t getConnectionCount() const override;
    bool isConnectedTo(const BLEAddress& address) const override;

    // Callback registration
    void setOnScanResult(Callbacks::OnScanResult callback) override;
    void setOnScanComplete(Callbacks::OnScanComplete callback) override;
    void setOnConnected(Callbacks::OnConnected callback) override;
    void setOnDisconnected(Callbacks::OnDisconnected callback) override;
    void setOnMTUChanged(Callbacks::OnMTUChanged callback) override;
    void setOnServicesDiscovered(Callbacks::OnServicesDiscovered callback) override;
    void setOnDataReceived(Callbacks::OnDataReceived callback) override;
    void setOnNotifyEnabled(Callbacks::OnNotifyEnabled callback) override;
    void setOnCentralConnected(Callbacks::OnCentralConnected callback) override;
    void setOnCentralDisconnected(Callbacks::OnCentralDisconnected callback) override;
    void setOnWriteReceived(Callbacks::OnWriteReceived callback) override;
    void setOnReadRequested(Callbacks::OnReadRequested callback) override;

    // Platform info
    PlatformType getPlatformType() const override { return PlatformType::NIMBLE_ARDUINO; }
    std::string getPlatformName() const override { return "NimBLE-Arduino"; }
    BLEAddress getLocalAddress() const override;

    //=========================================================================
    // NimBLEServerCallbacks (Peripheral mode)
    //=========================================================================

    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override;

    //=========================================================================
    // NimBLECharacteristicCallbacks
    //=========================================================================

    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;
    void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;
    void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo,
                     uint16_t subValue) override;

    //=========================================================================
    // NimBLEClientCallbacks (Central mode)
    //=========================================================================

    void onConnect(NimBLEClient* pClient) override;
    void onConnectFail(NimBLEClient* pClient, int reason) override;
    void onDisconnect(NimBLEClient* pClient, int reason) override;

    //=========================================================================
    // NimBLEScanCallbacks (Scanning)
    //=========================================================================

    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
    void onScanEnd(const NimBLEScanResults& results, int reason) override;

protected:
    // BLEOperationQueue implementation
    bool executeOperation(const GATTOperation& op) override;

private:
    // Setup methods
    bool setupServer();
    bool setupAdvertising();
    bool setupScan();

    // Address conversion
    static BLEAddress fromNimBLE(const NimBLEAddress& addr);
    static NimBLEAddress toNimBLE(const BLEAddress& addr);

    // Find client by connection handle or address
    NimBLEClient* findClient(uint16_t conn_handle);
    NimBLEClient* findClient(const BLEAddress& address);

    // Connection handle management
    uint16_t allocateConnHandle();
    void freeConnHandle(uint16_t handle);

    // Update connection info
    void updateConnectionMTU(uint16_t conn_handle, uint16_t mtu);

    // Check if a device address is currently connected
    bool isDeviceConnected(const std::string& addrKey) const;

    //=========================================================================
    // State Machine Infrastructure
    //=========================================================================

    // State variables (protected by spinlock)
    mutable portMUX_TYPE _state_mux = portMUX_INITIALIZER_UNLOCKED;
    MasterState _master_state = MasterState::IDLE;
    SlaveState _slave_state = SlaveState::IDLE;
    GAPState _gap_state = GAPState::UNINITIALIZED;

    // Mutex for connection map access (longer operations)
    SemaphoreHandle_t _conn_mutex = nullptr;

    // State transition helpers (atomic compare-and-swap)
    bool transitionMasterState(MasterState expected, MasterState new_state);
    bool transitionSlaveState(SlaveState expected, SlaveState new_state);
    bool transitionGAPState(GAPState expected, GAPState new_state);

    // State verification methods
    bool canStartScan() const;
    bool canStartAdvertising() const;
    bool canConnect() const;

    // Operation coordination
    bool pauseSlaveForMaster();
    void resumeSlave();
    void enterErrorRecovery();

    // Track if slave was paused for a master operation
    bool _slave_paused_for_master = false;

    //=========================================================================
    // Configuration
    //=========================================================================
    PlatformConfig _config;
    bool _initialized = false;
    bool _running = false;
    Bytes _identity_data;
    unsigned long _scan_stop_time = 0;  // millis() when to stop continuous scan

    // BLE stack recovery
    uint8_t _scan_fail_count = 0;
    uint8_t _lightweight_reset_fails = 0;
    uint8_t _conn_establish_fail_count = 0;  // rc=574 connection establishment failures
    unsigned long _last_full_recovery_time = 0;
    static constexpr uint8_t SCAN_FAIL_RECOVERY_THRESHOLD = 5;
    static constexpr uint8_t LIGHTWEIGHT_RESET_MAX_FAILS = 3;
    static constexpr uint8_t CONN_ESTABLISH_FAIL_THRESHOLD = 3;  // Threshold for rc=574
    static constexpr unsigned long FULL_RECOVERY_COOLDOWN_MS = 60000;  // 60 seconds
    bool recoverBLEStack();

    // NimBLE objects
    NimBLEServer* _server = nullptr;
    NimBLEService* _service = nullptr;
    NimBLECharacteristic* _rx_char = nullptr;
    NimBLECharacteristic* _tx_char = nullptr;
    NimBLECharacteristic* _identity_char = nullptr;
    NimBLEScan* _scan = nullptr;
    NimBLEAdvertising* _advertising_obj = nullptr;

    // Client connections (as central)
    std::map<uint16_t, NimBLEClient*> _clients;

    // Connection tracking
    std::map<uint16_t, ConnectionHandle> _connections;

    // Cached scan results for connection (stores full device info from scan)
    // Key: MAC address as string (e.g., "b8:27:eb:43:04:bc")
    std::map<std::string, NimBLEAdvertisedDevice> _discovered_devices;

    // Insertion-order tracking for FIFO eviction of discovered devices
    std::vector<std::string> _discovered_order;

    // Connection handle allocator (NimBLE uses its own, we wrap for consistency)
    uint16_t _next_conn_handle = 1;

    // VOLATILE RATIONALE: NimBLE callback synchronization flags
    //
    // These volatile flags synchronize between:
    // 1. NimBLE host task (callback context - runs asynchronously like ISR)
    // 2. BLE task (loop() context - application thread)
    //
    // Volatile is appropriate because:
    // - Single-word reads/writes are atomic on ESP32 (32-bit aligned)
    // - These are simple status flags, not complex state
    // - Mutex would cause priority inversion in callback context
    // - Memory barriers not needed - flag semantics sufficient
    //
    // Alternative rejected: Mutex acquisition in NimBLE callbacks can cause
    // priority inversion or deadlock since callbacks run in host task context.
    //
    // Reference: ESP32 Technical Reference Manual, Section 5.4 (Memory Consistency)

    // Async connection tracking (NimBLEClientCallbacks)
    volatile bool _async_connect_pending = false;
    volatile bool _async_connect_failed = false;
    volatile int _async_connect_error = 0;

    // VOLATILE RATIONALE: Native GAP handler callback flags
    // Same rationale as above - nativeGapEventHandler runs in NimBLE host task.
    // These track connection state during ble_gap_connect() operations.
    volatile bool _native_connect_pending = false;
    volatile bool _native_connect_success = false;
    volatile int _native_connect_result = 0;
    volatile uint16_t _native_connect_handle = 0;
    BLEAddress _native_connect_address;

    // Native GAP event handler
    static int nativeGapEventHandler(struct ble_gap_event* event, void* arg);
    bool connectNative(const BLEAddress& address, uint16_t timeout_ms);

    // Callbacks
    Callbacks::OnScanResult _on_scan_result;
    Callbacks::OnScanComplete _on_scan_complete;
    Callbacks::OnConnected _on_connected;
    Callbacks::OnDisconnected _on_disconnected;
    Callbacks::OnMTUChanged _on_mtu_changed;
    Callbacks::OnServicesDiscovered _on_services_discovered;
    Callbacks::OnDataReceived _on_data_received;
    Callbacks::OnNotifyEnabled _on_notify_enabled;
    Callbacks::OnCentralConnected _on_central_connected;
    Callbacks::OnCentralDisconnected _on_central_disconnected;
    Callbacks::OnWriteReceived _on_write_received;
    Callbacks::OnReadRequested _on_read_requested;

    //=========================================================================
    // BLE Shutdown Safety (CONC-H4, CONC-M4)
    //=========================================================================

    // Unclean shutdown flag - set if forced shutdown occurred with active operations
    // Uses RTC_NOINIT_ATTR on ESP32 for persistence across soft reboot
    static bool _unclean_shutdown;

    // Active write operation tracking (atomic for callback safety)
    std::atomic<int> _active_write_count{0};

public:
    /**
     * Check if there are active write operations in progress.
     * Write operations are critical - interrupting can corrupt peer state.
     */
    bool hasActiveWriteOperations() const { return _active_write_count.load() > 0; }

    /**
     * Check if last shutdown was clean.
     * Returns false if BLE was force-closed with active operations.
     */
    static bool wasCleanShutdown() { return !_unclean_shutdown; }

    /**
     * Clear unclean shutdown flag (call after boot verification).
     */
    static void clearUncleanShutdownFlag() { _unclean_shutdown = false; }

private:
    /**
     * Mark a write operation as starting (call before characteristic write).
     */
    void beginWriteOperation() { _active_write_count.fetch_add(1); }

    /**
     * Mark a write operation as complete (call after write callback).
     */
    void endWriteOperation() { _active_write_count.fetch_sub(1); }
};

}} // namespace RNS::BLE

#endif // ESP32 && USE_NIMBLE

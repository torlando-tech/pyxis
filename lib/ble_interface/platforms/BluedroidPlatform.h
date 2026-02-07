/**
 * @file BluedroidPlatform.h
 * @brief ESP-IDF Bluedroid implementation of IBLEPlatform for ESP32
 *
 * This implementation uses the ESP-IDF Bluedroid stack to provide BLE
 * functionality on ESP32 devices. It supports both central and peripheral
 * modes simultaneously (dual-mode operation).
 *
 * Alternative to NimBLEPlatform to work around state machine bugs in NimBLE
 * that cause rc=530/rc=21 errors during dual-role operation.
 */
#pragma once

#include "../BLEPlatform.h"
#include "../BLEOperationQueue.h"

// Only compile for ESP32 with Bluedroid
#if defined(ESP32) && defined(USE_BLUEDROID)

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_gattc_api.h>
#include <esp_bt_defs.h>
#include <esp_gatt_common_api.h>

#include <map>
#include <vector>
#include <queue>
#include <functional>

namespace RNS { namespace BLE {

/**
 * @brief Bluedroid (ESP-IDF) implementation of IBLEPlatform
 */
class BluedroidPlatform : public IBLEPlatform, public BLEOperationQueue {
public:
    BluedroidPlatform();
    virtual ~BluedroidPlatform();

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
    PlatformType getPlatformType() const override { return PlatformType::ESP_IDF; }
    std::string getPlatformName() const override { return "Bluedroid"; }
    BLEAddress getLocalAddress() const override;

protected:
    // BLEOperationQueue implementation
    bool executeOperation(const GATTOperation& op) override;

private:
    //=========================================================================
    // Static Callback Handlers (Bluedroid requires static functions)
    //=========================================================================

    static void gapEventHandler(esp_gap_ble_cb_event_t event,
                                esp_ble_gap_cb_param_t* param);
    static void gattsEventHandler(esp_gatts_cb_event_t event,
                                  esp_gatt_if_t gatts_if,
                                  esp_ble_gatts_cb_param_t* param);
    static void gattcEventHandler(esp_gattc_cb_event_t event,
                                  esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t* param);

    // Singleton instance for static callback routing
    static BluedroidPlatform* _instance;

    //=========================================================================
    // State Machines
    //=========================================================================

    enum class InitState {
        UNINITIALIZED,
        CONTROLLER_INIT,
        BLUEDROID_INIT,
        CALLBACKS_REGISTERED,
        GATTS_REGISTERING,
        GATTS_CREATING_SERVICE,
        GATTS_ADDING_CHARS,
        GATTS_STARTING_SERVICE,
        GATTC_REGISTERING,
        READY
    };

    enum class ScanState {
        IDLE,
        SETTING_PARAMS,
        STARTING,
        ACTIVE,
        STOPPING
    };

    enum class AdvState {
        IDLE,
        CONFIGURING_DATA,
        CONFIGURING_SCAN_RSP,
        STARTING,
        ACTIVE,
        STOPPING
    };

    //=========================================================================
    // Internal Event Handlers
    //=========================================================================

    // GAP events
    void handleGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    void handleScanResult(esp_ble_gap_cb_param_t* param);
    void handleScanComplete();
    void handleAdvStart(esp_bt_status_t status);
    void handleAdvStop();

    // GATTS events (peripheral/server)
    void handleGattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                          esp_ble_gatts_cb_param_t* param);
    void handleGattsRegister(esp_gatt_if_t gatts_if, esp_gatt_status_t status);
    void handleGattsServiceCreated(uint16_t service_handle);
    void handleGattsCharAdded(uint16_t attr_handle, esp_bt_uuid_t* char_uuid);
    void handleGattsServiceStarted();
    void handleGattsConnect(esp_ble_gatts_cb_param_t* param);
    void handleGattsDisconnect(esp_ble_gatts_cb_param_t* param);
    void handleGattsWrite(esp_ble_gatts_cb_param_t* param);
    void handleGattsRead(esp_ble_gatts_cb_param_t* param);
    void handleGattsMtuChange(esp_ble_gatts_cb_param_t* param);
    void handleGattsConfirm(esp_ble_gatts_cb_param_t* param);

    // GATTC events (central/client)
    void handleGattcEvent(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                          esp_ble_gattc_cb_param_t* param);
    void handleGattcRegister(esp_gatt_if_t gattc_if, esp_gatt_status_t status);
    void handleGattcConnect(esp_ble_gattc_cb_param_t* param);
    void handleGattcDisconnect(esp_ble_gattc_cb_param_t* param);
    void handleGattcSearchResult(esp_ble_gattc_cb_param_t* param);
    void handleGattcSearchComplete(esp_ble_gattc_cb_param_t* param);
    void handleGattcGetChar(esp_ble_gattc_cb_param_t* param);
    void handleGattcNotify(esp_ble_gattc_cb_param_t* param);
    void handleGattcWrite(esp_ble_gattc_cb_param_t* param);
    void handleGattcRead(esp_ble_gattc_cb_param_t* param);

    //=========================================================================
    // Setup Methods
    //=========================================================================

    bool initBluetooth();
    bool setupGattsService();
    void buildAdvertisingData();
    void buildScanResponseData();

    //=========================================================================
    // Address Conversion
    //=========================================================================

    static BLEAddress fromEspBdAddr(const esp_bd_addr_t addr, esp_ble_addr_type_t type);
    static void toEspBdAddr(const BLEAddress& addr, esp_bd_addr_t out_addr);

    //=========================================================================
    // Connection Management
    //=========================================================================

    struct BluedroidConnection {
        uint16_t conn_id = 0xFFFF;
        esp_bd_addr_t peer_addr = {0};
        esp_ble_addr_type_t addr_type = BLE_ADDR_TYPE_PUBLIC;
        Role local_role = Role::NONE;
        uint16_t mtu = MTU::MINIMUM;
        bool notifications_enabled = false;

        // Client-mode discovery handles (when we connect to a peripheral)
        uint16_t service_start_handle = 0;
        uint16_t service_end_handle = 0;
        uint16_t rx_char_handle = 0;
        uint16_t tx_char_handle = 0;
        uint16_t tx_cccd_handle = 0;
        uint16_t identity_char_handle = 0;

        // Discovery state
        enum class DiscoveryState {
            NONE,
            SEARCHING_SERVICE,
            GETTING_CHARS,
            GETTING_DESCRIPTORS,
            COMPLETE
        } discovery_state = DiscoveryState::NONE;
    };

    uint16_t allocateConnHandle();
    void freeConnHandle(uint16_t handle);
    BluedroidConnection* findConnection(uint16_t conn_id);
    BluedroidConnection* findConnectionByAddress(const esp_bd_addr_t addr);

    //=========================================================================
    // Member Variables
    //=========================================================================

    // Configuration
    PlatformConfig _config;
    bool _initialized = false;
    bool _running = false;
    Bytes _identity_data;
    Bytes _custom_adv_data;

    // State machines
    InitState _init_state = InitState::UNINITIALIZED;
    ScanState _scan_state = ScanState::IDLE;
    AdvState _adv_state = AdvState::IDLE;

    // GATT interfaces (from registration events)
    esp_gatt_if_t _gatts_if = ESP_GATT_IF_NONE;
    esp_gatt_if_t _gattc_if = ESP_GATT_IF_NONE;

    // GATTS handles (server/peripheral mode)
    uint16_t _service_handle = 0;
    uint16_t _rx_char_handle = 0;      // RX characteristic (central writes here)
    uint16_t _tx_char_handle = 0;      // TX characteristic (we notify from here)
    uint16_t _tx_cccd_handle = 0;      // TX CCCD (for notification enable/disable)
    uint16_t _identity_char_handle = 0; // Identity characteristic (central reads)

    // Service creation state tracking
    uint8_t _chars_added = 0;
    static constexpr uint8_t CHARS_EXPECTED = 3;  // RX, TX, Identity

    // Scan timing
    uint16_t _scan_duration_ms = 0;
    unsigned long _scan_start_time = 0;

    // Connection tracking
    std::map<uint16_t, BluedroidConnection> _connections;
    uint16_t _next_conn_handle = 1;

    // Pending connection state
    volatile bool _connect_pending = false;
    volatile bool _connect_success = false;
    volatile int _connect_error = 0;
    BLEAddress _pending_connect_address;
    unsigned long _connect_start_time = 0;
    uint16_t _connect_timeout_ms = 10000;

    // Service discovery serialization (Bluedroid can only handle one at a time)
    uint16_t _discovery_in_progress = 0xFFFF;  // conn_handle of active discovery, or 0xFFFF if none
    std::queue<uint16_t> _pending_discoveries;  // queued conn_handles waiting for discovery

    // Local address cache
    mutable esp_bd_addr_t _local_addr = {0};
    mutable bool _local_addr_valid = false;

    // App IDs for GATT profiles
    static constexpr uint16_t GATTS_APP_ID = 0;
    static constexpr uint16_t GATTC_APP_ID = 1;

    //=========================================================================
    // Callbacks (application-level)
    //=========================================================================

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
};

}} // namespace RNS::BLE

#endif // ESP32 && USE_BLUEDROID

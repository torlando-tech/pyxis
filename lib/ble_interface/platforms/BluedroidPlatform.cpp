/**
 * @file BluedroidPlatform.cpp
 * @brief ESP-IDF Bluedroid implementation of IBLEPlatform for ESP32
 */

#include "BluedroidPlatform.h"

#if defined(ESP32) && defined(USE_BLUEDROID)

#include "Log.h"
#include "../BLETypes.h"

#include <Arduino.h>
#include <cstring>

namespace RNS { namespace BLE {

// Static singleton instance for callback routing
BluedroidPlatform* BluedroidPlatform::_instance = nullptr;

//=============================================================================
// Constructor / Destructor
//=============================================================================

BluedroidPlatform::BluedroidPlatform() {
    DEBUG("BluedroidPlatform: Constructor");
    if (_instance != nullptr) {
        WARNING("BluedroidPlatform: Another instance exists - callbacks may misbehave");
    }
    _instance = this;
}

BluedroidPlatform::~BluedroidPlatform() {
    DEBUG("BluedroidPlatform: Destructor");
    shutdown();
    if (_instance == this) {
        _instance = nullptr;
    }
}

//=============================================================================
// Static Callback Handlers (route to instance methods)
//=============================================================================

void BluedroidPlatform::gapEventHandler(esp_gap_ble_cb_event_t event,
                                         esp_ble_gap_cb_param_t* param) {
    if (_instance) {
        _instance->handleGapEvent(event, param);
    }
}

void BluedroidPlatform::gattsEventHandler(esp_gatts_cb_event_t event,
                                           esp_gatt_if_t gatts_if,
                                           esp_ble_gatts_cb_param_t* param) {
    if (_instance) {
        _instance->handleGattsEvent(event, gatts_if, param);
    }
}

void BluedroidPlatform::gattcEventHandler(esp_gattc_cb_event_t event,
                                           esp_gatt_if_t gattc_if,
                                           esp_ble_gattc_cb_param_t* param) {
    if (_instance) {
        _instance->handleGattcEvent(event, gattc_if, param);
    }
}

//=============================================================================
// Lifecycle
//=============================================================================

bool BluedroidPlatform::initialize(const PlatformConfig& config) {
    if (_initialized) {
        WARNING("BluedroidPlatform: Already initialized");
        return true;
    }

    INFO("BluedroidPlatform: Initializing Bluedroid BLE stack...");
    _config = config;

    if (!initBluetooth()) {
        ERROR("BluedroidPlatform: Failed to initialize Bluetooth");
        return false;
    }

    _initialized = true;
    INFO("BluedroidPlatform: Initialization complete");
    return true;
}

bool BluedroidPlatform::initBluetooth() {
    esp_err_t ret;

    // Release classic BT memory if not needed (saves ~65KB)
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        WARNING("BluedroidPlatform: Could not release classic BT memory: " +
                std::to_string(ret));
    }

    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Controller init failed: " + std::to_string(ret));
        return false;
    }
    _init_state = InitState::CONTROLLER_INIT;

    // Enable BT controller in BLE-only mode
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Controller enable failed: " + std::to_string(ret));
        return false;
    }

    // Initialize Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Bluedroid init failed: " + std::to_string(ret));
        return false;
    }
    _init_state = InitState::BLUEDROID_INIT;

    // Enable Bluedroid
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Bluedroid enable failed: " + std::to_string(ret));
        return false;
    }

    // Register callbacks
    ret = esp_ble_gap_register_callback(gapEventHandler);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: GAP callback register failed: " + std::to_string(ret));
        return false;
    }

    ret = esp_ble_gatts_register_callback(gattsEventHandler);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: GATTS callback register failed: " + std::to_string(ret));
        return false;
    }

    ret = esp_ble_gattc_register_callback(gattcEventHandler);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: GATTC callback register failed: " + std::to_string(ret));
        return false;
    }
    _init_state = InitState::CALLBACKS_REGISTERED;

    // Set device name
    ret = esp_ble_gap_set_device_name(_config.device_name.c_str());
    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Set device name failed: " + std::to_string(ret));
    }

    // Set local MTU
    ret = esp_ble_gatt_set_local_mtu(_config.preferred_mtu);
    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Set local MTU failed: " + std::to_string(ret));
    }

    // Register GATTS app for peripheral mode
    if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
        ret = esp_ble_gatts_app_register(GATTS_APP_ID);
        if (ret != ESP_OK) {
            ERROR("BluedroidPlatform: GATTS app register failed: " + std::to_string(ret));
            return false;
        }
        _init_state = InitState::GATTS_REGISTERING;
        DEBUG("BluedroidPlatform: GATTS app registration pending...");
    }

    // Register GATTC app for central mode
    if (_config.role == Role::CENTRAL || _config.role == Role::DUAL) {
        ret = esp_ble_gattc_app_register(GATTC_APP_ID);
        if (ret != ESP_OK) {
            ERROR("BluedroidPlatform: GATTC app register failed: " + std::to_string(ret));
            return false;
        }
        DEBUG("BluedroidPlatform: GATTC app registration pending...");
    }

    return true;
}

bool BluedroidPlatform::start() {
    if (!_initialized) {
        ERROR("BluedroidPlatform: Cannot start - not initialized");
        return false;
    }

    if (_running) {
        return true;
    }

    INFO("BluedroidPlatform: Starting BLE operations");

    // Reset state to ensure clean startup
    _connect_pending = false;
    _connect_success = false;
    _connect_error = 0;
    _connect_start_time = 0;
    _connect_timeout_ms = 10000;
    _discovery_in_progress = 0xFFFF;
    while (!_pending_discoveries.empty()) {
        _pending_discoveries.pop();
    }

    // Start advertising if in peripheral or dual mode
    if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
        if (_init_state == InitState::READY) {
            startAdvertising();
        } else {
            DEBUG("BluedroidPlatform: Waiting for GATTS service ready before advertising");
        }
    }

    _running = true;
    return true;
}

void BluedroidPlatform::stop() {
    if (!_running) return;

    INFO("BluedroidPlatform: Stopping BLE operations");

    stopScan();
    stopAdvertising();
    disconnectAll();

    _running = false;
}

void BluedroidPlatform::loop() {
    if (!_running) return;

    // Process operation queue
    process();

    // Check scan timeout
    if (_scan_state == ScanState::ACTIVE && _scan_duration_ms > 0) {
        if (millis() - _scan_start_time >= _scan_duration_ms) {
            stopScan();
        }
    }

    // Check connection timeout - allows retrying other peers while BLE stack
    // continues its internal connection attempt (which can take 30+ seconds)
    if (_connect_pending && _connect_timeout_ms > 0) {
        if (millis() - _connect_start_time >= _connect_timeout_ms) {
            WARNING("BluedroidPlatform: Connection timed out to " + _pending_connect_address.toString());
            _connect_pending = false;
            // Note: The BLE stack may still complete the connection later,
            // which will be handled normally in handleGattcConnect()
        }
    }
}

void BluedroidPlatform::shutdown() {
    INFO("BluedroidPlatform: Shutting down");

    stop();

    if (_initialized) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }

    _initialized = false;
    _init_state = InitState::UNINITIALIZED;
    _gatts_if = ESP_GATT_IF_NONE;
    _gattc_if = ESP_GATT_IF_NONE;
}

bool BluedroidPlatform::isRunning() const {
    return _running && _init_state == InitState::READY;
}

//=============================================================================
// Scanning (Central Mode)
//=============================================================================

bool BluedroidPlatform::startScan(uint16_t duration_ms) {
    if (_scan_state != ScanState::IDLE) {
        DEBUG("BluedroidPlatform: Scan already in progress");
        return false;
    }

    // Skip scanning if we've reached connection limit - saves memory from scan results
    if (getConnectionCount() >= _config.max_connections) {
        TRACE("BluedroidPlatform: Skipping scan - at max connections (" +
              std::to_string(getConnectionCount()) + "/" +
              std::to_string(_config.max_connections) + ")");
        return false;
    }

    // In dual mode, stop advertising before scanning
    if (_adv_state == AdvState::ACTIVE) {
        DEBUG("BluedroidPlatform: Stopping advertising for scan");
        stopAdvertising();
        taskYIELD();  // Allow stack to process
    }

    esp_ble_scan_params_t scan_params = {
        .scan_type = (_config.scan_mode == ScanMode::ACTIVE) ?
                     BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,  // Use public address (chip MAC)
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = static_cast<uint16_t>((_config.scan_interval_ms * 1000) / 625),
        .scan_window = static_cast<uint16_t>((_config.scan_window_ms * 1000) / 625),
        .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE
    };

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Set scan params failed: " + std::to_string(ret));
        return false;
    }

    _scan_duration_ms = duration_ms;
    _scan_state = ScanState::SETTING_PARAMS;
    DEBUG("BluedroidPlatform: Scan params set, waiting for confirmation...");

    return true;
}

void BluedroidPlatform::stopScan() {
    if (_scan_state == ScanState::IDLE) return;

    esp_err_t ret = esp_ble_gap_stop_scanning();
    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Stop scan failed: " + std::to_string(ret));
    }
    _scan_state = ScanState::STOPPING;
}

bool BluedroidPlatform::isScanning() const {
    return _scan_state == ScanState::ACTIVE;
}

//=============================================================================
// Advertising (Peripheral Mode)
//=============================================================================

bool BluedroidPlatform::startAdvertising() {
    if (_adv_state != AdvState::IDLE) {
        DEBUG("BluedroidPlatform: Advertising already active or starting");
        return false;
    }

    if (_init_state != InitState::READY) {
        WARNING("BluedroidPlatform: Cannot advertise - GATTS not ready");
        return false;
    }

    // Skip advertising if at max connections - no point accepting more
    if (getConnectionCount() >= _config.max_connections) {
        TRACE("BluedroidPlatform: Skipping advertising - at max connections (" +
              std::to_string(getConnectionCount()) + "/" +
              std::to_string(_config.max_connections) + ")");
        return false;
    }

    DEBUG("BluedroidPlatform: Starting advertising");
    buildAdvertisingData();
    _adv_state = AdvState::CONFIGURING_DATA;

    return true;
}

void BluedroidPlatform::buildAdvertisingData() {
    // Build advertising data with service UUID
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = true,
        .min_interval = static_cast<uint16_t>((_config.adv_interval_min_ms * 1000) / 625),
        .max_interval = static_cast<uint16_t>((_config.adv_interval_max_ms * 1000) / 625),
        .appearance = 0x0000,
        .manufacturer_len = 0,
        .p_manufacturer_data = nullptr,
        .service_data_len = 0,
        .p_service_data = nullptr,
        .service_uuid_len = 16,
        .p_service_uuid = nullptr,  // Will set below
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)
    };

    // Parse service UUID from string to bytes
    static uint8_t service_uuid_bytes[16];
    // UUID::SERVICE = "37145b00-442d-4a94-917f-8f42c5da28e3"
    // UUID is stored in little-endian in advertising data
    const char* uuid_str = UUID::SERVICE;
    int idx = 15;  // Start from end (little-endian)
    for (int i = 0; i < 36 && idx >= 0; i++) {
        if (uuid_str[i] == '-') continue;
        unsigned int byte_val;
        sscanf(&uuid_str[i], "%02x", &byte_val);
        service_uuid_bytes[idx--] = static_cast<uint8_t>(byte_val);
        i++;  // Skip second hex digit
    }
    adv_data.p_service_uuid = service_uuid_bytes;

    esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Config adv data failed: " + std::to_string(ret));
        _adv_state = AdvState::IDLE;
    }
}

void BluedroidPlatform::buildScanResponseData() {
    esp_ble_adv_data_t scan_rsp_data = {
        .set_scan_rsp = true,
        .include_name = true,
        .include_txpower = false,
        .min_interval = 0,
        .max_interval = 0,
        .appearance = 0x0000,
        .manufacturer_len = 0,
        .p_manufacturer_data = nullptr,
        .service_data_len = 0,
        .p_service_data = nullptr,
        .service_uuid_len = 0,
        .p_service_uuid = nullptr,
        .flag = 0
    };

    esp_err_t ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Config scan response failed: " + std::to_string(ret));
        _adv_state = AdvState::IDLE;
    }
}

void BluedroidPlatform::stopAdvertising() {
    if (_adv_state == AdvState::IDLE) return;

    esp_err_t ret = esp_ble_gap_stop_advertising();
    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Stop advertising failed: " + std::to_string(ret));
    }
    _adv_state = AdvState::STOPPING;
}

bool BluedroidPlatform::isAdvertising() const {
    return _adv_state == AdvState::ACTIVE;
}

bool BluedroidPlatform::setAdvertisingData(const Bytes& data) {
    _custom_adv_data = data;
    return true;
}

void BluedroidPlatform::setIdentityData(const Bytes& identity) {
    _identity_data = identity;
    DEBUG("BluedroidPlatform: Identity data set (" + std::to_string(identity.size()) + " bytes)");
}

//=============================================================================
// Connection Management
//=============================================================================

bool BluedroidPlatform::connect(const BLEAddress& address, uint16_t timeout_ms) {
    // Check connection limit first - don't connect if at max
    if (getConnectionCount() >= _config.max_connections) {
        TRACE("BluedroidPlatform: Skipping connect - at max connections (" +
              std::to_string(getConnectionCount()) + "/" +
              std::to_string(_config.max_connections) + ")");
        return false;
    }

    // Protect against connecting when memory is critically low
    if (ESP.getFreeHeap() < 40000) {
        static uint32_t last_low_mem_warn = 0;
        if (millis() - last_low_mem_warn > 10000) {
            WARNING("BluedroidPlatform: Skipping connection - low memory (" +
                    std::to_string(ESP.getFreeHeap()) + " bytes free)");
            last_low_mem_warn = millis();
        }
        return false;
    }

    if (_gattc_if == ESP_GATT_IF_NONE) {
        ERROR("BluedroidPlatform: GATTC not registered");
        return false;
    }

    if (_connect_pending) {
        WARNING("BluedroidPlatform: Connection already pending");
        return false;
    }

    // Bluedroid can't handle connection while service discovery is in progress
    // This causes hash_map_set assertion failures in btu_start_timer
    if (_discovery_in_progress != 0xFFFF) {
        WARNING("BluedroidPlatform: Cannot connect while discovery in progress for " +
                std::to_string(_discovery_in_progress));
        return false;
    }

    esp_bd_addr_t peer_addr;
    toEspBdAddr(address, peer_addr);

    _connect_pending = true;
    _connect_success = false;
    _connect_error = 0;
    _pending_connect_address = address;
    _connect_start_time = millis();
    _connect_timeout_ms = timeout_ms;

    esp_err_t ret = esp_ble_gattc_open(
        _gattc_if,
        peer_addr,
        static_cast<esp_ble_addr_type_t>(address.type),
        true  // Direct connection
    );

    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: GATTC open failed: " + std::to_string(ret));
        _connect_pending = false;
        return false;
    }

    DEBUG("BluedroidPlatform: Connection initiated to " + address.toString());
    return true;
}

bool BluedroidPlatform::disconnect(uint16_t conn_handle) {
    auto* conn = findConnection(conn_handle);
    if (!conn) {
        WARNING("BluedroidPlatform: Connection not found: " + std::to_string(conn_handle));
        return false;
    }

    esp_err_t ret;
    if (conn->local_role == Role::PERIPHERAL) {
        ret = esp_ble_gatts_close(_gatts_if, conn->conn_id);
    } else {
        ret = esp_ble_gattc_close(_gattc_if, conn->conn_id);
    }

    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Disconnect failed: " + std::to_string(ret));
        return false;
    }

    return true;
}

void BluedroidPlatform::disconnectAll() {
    for (auto& pair : _connections) {
        disconnect(pair.first);
    }
}

bool BluedroidPlatform::requestMTU(uint16_t conn_handle, uint16_t mtu) {
    auto* conn = findConnection(conn_handle);
    if (!conn) return false;

    if (conn->local_role == Role::CENTRAL) {
        esp_err_t ret = esp_ble_gattc_send_mtu_req(_gattc_if, conn->conn_id);
        return ret == ESP_OK;
    }

    // Peripheral mode MTU is negotiated by central
    return true;
}

bool BluedroidPlatform::discoverServices(uint16_t conn_handle) {
    DEBUG("BluedroidPlatform: discoverServices called for handle " + std::to_string(conn_handle));

    auto* conn = findConnection(conn_handle);
    if (!conn) {
        ERROR("BluedroidPlatform: discoverServices - connection not found for handle " + std::to_string(conn_handle));
        return false;
    }

    // Bluedroid can only handle one GATT operation at a time
    // Queue if connection is pending or another discovery is in progress
    if (_connect_pending || _discovery_in_progress != 0xFFFF) {
        INFO("BluedroidPlatform: GATT busy (connect_pending=" + std::string(_connect_pending ? "yes" : "no") +
             " discovery=" + std::to_string(_discovery_in_progress) + "), queueing " + std::to_string(conn_handle));
        _pending_discoveries.push(conn_handle);
        return true;  // Queued successfully
    }

    DEBUG("BluedroidPlatform: Found connection, conn_id=" + std::to_string(conn->conn_id));

    // Parse service UUID for filter
    esp_bt_uuid_t service_uuid;
    service_uuid.len = ESP_UUID_LEN_128;
    // Convert UUID string to bytes (little-endian)
    const char* uuid_str = UUID::SERVICE;
    int idx = 15;
    for (int i = 0; i < 36 && idx >= 0; i++) {
        if (uuid_str[i] == '-') continue;
        unsigned int byte_val;
        sscanf(&uuid_str[i], "%02x", &byte_val);
        service_uuid.uuid.uuid128[idx--] = static_cast<uint8_t>(byte_val);
        i++;
    }

    _discovery_in_progress = conn_handle;
    conn->discovery_state = BluedroidConnection::DiscoveryState::SEARCHING_SERVICE;
    esp_err_t ret = esp_ble_gattc_search_service(_gattc_if, conn->conn_id, &service_uuid);

    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Service search failed: " + std::to_string(ret));
        _discovery_in_progress = 0xFFFF;
        return false;
    }

    DEBUG("BluedroidPlatform: Service search started for conn_id=" + std::to_string(conn->conn_id));
    return true;
}

//=============================================================================
// GATT Operations
//=============================================================================

bool BluedroidPlatform::write(uint16_t conn_handle, const Bytes& data, bool response) {
    auto* conn = findConnection(conn_handle);
    if (!conn) {
        WARNING("BluedroidPlatform: write - connection not found for handle " + std::to_string(conn_handle));
        return false;
    }

    if (conn->rx_char_handle == 0) {
        ERROR("BluedroidPlatform: RX characteristic not discovered for conn " + std::to_string(conn_handle) +
              " conn_id=" + std::to_string(conn->conn_id) +
              " discovery_state=" + std::to_string(static_cast<int>(conn->discovery_state)));
        return false;
    }

    // Hot path - no logging to avoid heap allocation

    esp_err_t ret = esp_ble_gattc_write_char(
        _gattc_if,
        conn->conn_id,
        conn->rx_char_handle,
        data.size(),
        const_cast<uint8_t*>(data.data()),
        response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE
    );

    // Yield to let BLE stack process
    taskYIELD();

    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Write failed with err " + std::to_string(ret));
    }
    return ret == ESP_OK;
}

bool BluedroidPlatform::read(uint16_t conn_handle, uint16_t char_handle,
                              std::function<void(OperationResult, const Bytes&)> callback) {
    auto* conn = findConnection(conn_handle);
    if (!conn) {
        if (callback) callback(OperationResult::DISCONNECTED, Bytes());
        return false;
    }

    // Queue the operation
    GATTOperation op;
    op.type = OperationType::READ;
    op.conn_handle = conn_handle;
    op.char_handle = char_handle;
    op.callback = callback;
    enqueue(op);

    return true;
}

bool BluedroidPlatform::enableNotifications(uint16_t conn_handle, bool enable) {
    auto* conn = findConnection(conn_handle);
    if (!conn) {
        WARNING("BluedroidPlatform: enableNotifications - connection not found");
        return false;
    }

    if (conn->tx_char_handle == 0) {
        ERROR("BluedroidPlatform: TX char not discovered for conn " + std::to_string(conn_handle));
        return false;
    }

    INFO("BluedroidPlatform: Enabling notifications for conn " + std::to_string(conn_handle) +
         " conn_id=" + std::to_string(conn->conn_id) + " tx_char=" + std::to_string(conn->tx_char_handle));

    // Step 1: Register for notifications with the Bluedroid stack
    // This tells ESP-IDF to route ESP_GATTC_NOTIFY_EVT to our handler
    esp_err_t ret = esp_ble_gattc_register_for_notify(
        _gattc_if,
        conn->peer_addr,
        conn->tx_char_handle  // The characteristic we want notifications from
    );

    if (ret != ESP_OK) {
        WARNING("BluedroidPlatform: Failed to register for notify: " + std::to_string(ret));
        return false;
    }
    INFO("BluedroidPlatform: Registered for notify OK");

    // Step 2: Write to CCCD to enable notifications on the peripheral
    if (conn->tx_cccd_handle != 0) {
        DEBUG("BluedroidPlatform: Enabling notifications on CCCD handle " +
              std::to_string(conn->tx_cccd_handle) + " for conn " + std::to_string(conn_handle));

        uint16_t cccd_value = enable ? 0x0001 : 0x0000;
        ret = esp_ble_gattc_write_char_descr(
            _gattc_if,
            conn->conn_id,
            conn->tx_cccd_handle,
            sizeof(cccd_value),
            reinterpret_cast<uint8_t*>(&cccd_value),
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE
        );
    }

    DEBUG("BluedroidPlatform: Notifications " + std::string(enable ? "enabled" : "disabled") +
          " for conn " + std::to_string(conn_handle));

    // Yield to let Bluedroid process
    taskYIELD();

    return ret == ESP_OK;
}

bool BluedroidPlatform::notify(uint16_t conn_handle, const Bytes& data) {
    if (_gatts_if == ESP_GATT_IF_NONE || _tx_char_handle == 0) {
        return false;
    }

    auto* conn = findConnection(conn_handle);
    if (!conn) return false;

    esp_err_t ret = esp_ble_gatts_send_indicate(
        _gatts_if,
        conn->conn_id,
        _tx_char_handle,
        data.size(),
        const_cast<uint8_t*>(data.data()),
        false  // false = notification, true = indication
    );

    return ret == ESP_OK;
}

bool BluedroidPlatform::notifyAll(const Bytes& data) {
    if (_gatts_if == ESP_GATT_IF_NONE || _tx_char_handle == 0) {
        return false;
    }

    bool any_sent = false;
    for (auto& pair : _connections) {
        if (pair.second.local_role == Role::PERIPHERAL &&
            pair.second.notifications_enabled) {
            if (notify(pair.first, data)) {
                any_sent = true;
            }
        }
    }

    return any_sent;
}

bool BluedroidPlatform::executeOperation(const GATTOperation& op) {
    auto* conn = findConnection(op.conn_handle);
    if (!conn) {
        if (op.callback) op.callback(OperationResult::DISCONNECTED, Bytes());
        return true;  // Operation complete (failed)
    }

    switch (op.type) {
        case OperationType::READ: {
            esp_err_t ret = esp_ble_gattc_read_char(
                _gattc_if,
                conn->conn_id,
                op.char_handle,
                ESP_GATT_AUTH_REQ_NONE
            );
            if (ret != ESP_OK) {
                WARNING("BluedroidPlatform: Read char failed: " + std::to_string(ret));
                return false;  // Failed to start
            }
            return true;  // Successfully started, will complete async
        }
        default:
            WARNING("BluedroidPlatform: Unknown operation type");
            return false;  // Unknown operation type - failed to start
    }
}

//=============================================================================
// Connection Query
//=============================================================================

std::vector<ConnectionHandle> BluedroidPlatform::getConnections() const {
    std::vector<ConnectionHandle> result;
    for (const auto& pair : _connections) {
        ConnectionHandle handle;
        handle.handle = pair.first;
        handle.peer_address = fromEspBdAddr(pair.second.peer_addr, pair.second.addr_type);
        handle.local_role = pair.second.local_role;
        handle.state = ConnectionState::READY;
        handle.mtu = pair.second.mtu;
        result.push_back(handle);
    }
    return result;
}

ConnectionHandle BluedroidPlatform::getConnection(uint16_t handle) const {
    auto it = _connections.find(handle);
    if (it == _connections.end()) {
        return ConnectionHandle();
    }

    ConnectionHandle result;
    result.handle = handle;
    result.peer_address = fromEspBdAddr(it->second.peer_addr, it->second.addr_type);
    result.local_role = it->second.local_role;
    result.state = ConnectionState::READY;
    result.mtu = it->second.mtu;
    result.rx_char_handle = it->second.rx_char_handle;
    result.tx_char_handle = it->second.tx_char_handle;
    result.tx_cccd_handle = it->second.tx_cccd_handle;
    result.identity_handle = it->second.identity_char_handle;
    return result;
}

size_t BluedroidPlatform::getConnectionCount() const {
    return _connections.size();
}

bool BluedroidPlatform::isConnectedTo(const BLEAddress& address) const {
    esp_bd_addr_t esp_addr;
    toEspBdAddr(address, esp_addr);

    for (const auto& pair : _connections) {
        if (memcmp(pair.second.peer_addr, esp_addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

//=============================================================================
// Callback Registration
//=============================================================================

void BluedroidPlatform::setOnScanResult(Callbacks::OnScanResult callback) {
    _on_scan_result = callback;
}

void BluedroidPlatform::setOnScanComplete(Callbacks::OnScanComplete callback) {
    _on_scan_complete = callback;
}

void BluedroidPlatform::setOnConnected(Callbacks::OnConnected callback) {
    _on_connected = callback;
}

void BluedroidPlatform::setOnDisconnected(Callbacks::OnDisconnected callback) {
    _on_disconnected = callback;
}

void BluedroidPlatform::setOnMTUChanged(Callbacks::OnMTUChanged callback) {
    _on_mtu_changed = callback;
}

void BluedroidPlatform::setOnServicesDiscovered(Callbacks::OnServicesDiscovered callback) {
    _on_services_discovered = callback;
}

void BluedroidPlatform::setOnDataReceived(Callbacks::OnDataReceived callback) {
    _on_data_received = callback;
}

void BluedroidPlatform::setOnNotifyEnabled(Callbacks::OnNotifyEnabled callback) {
    _on_notify_enabled = callback;
}

void BluedroidPlatform::setOnCentralConnected(Callbacks::OnCentralConnected callback) {
    _on_central_connected = callback;
}

void BluedroidPlatform::setOnCentralDisconnected(Callbacks::OnCentralDisconnected callback) {
    _on_central_disconnected = callback;
}

void BluedroidPlatform::setOnWriteReceived(Callbacks::OnWriteReceived callback) {
    _on_write_received = callback;
}

void BluedroidPlatform::setOnReadRequested(Callbacks::OnReadRequested callback) {
    _on_read_requested = callback;
}

//=============================================================================
// Platform Info
//=============================================================================

BLEAddress BluedroidPlatform::getLocalAddress() const {
    if (!_local_addr_valid) {
        // Get local address from GAP
        uint8_t addr[6];
        uint8_t addr_type = 0;
        if (esp_ble_gap_get_local_used_addr(addr, &addr_type) == ESP_OK) {
            memcpy(_local_addr, addr, 6);
            _local_addr_valid = true;
        }
        // If still not valid, address will be retrieved on first advertising start
    }
    return fromEspBdAddr(_local_addr, BLE_ADDR_TYPE_PUBLIC);
}

//=============================================================================
// GAP Event Handler
//=============================================================================

void BluedroidPlatform::handleGapEvent(esp_gap_ble_cb_event_t event,
                                        esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                DEBUG("BluedroidPlatform: Scan params set, starting scan");
                _scan_start_time = millis();
                uint32_t duration_sec = (_scan_duration_ms > 0) ?
                    (_scan_duration_ms / 1000) : 0;  // 0 = continuous
                esp_ble_gap_start_scanning(duration_sec);
                _scan_state = ScanState::STARTING;
            } else {
                ERROR("BluedroidPlatform: Scan param set failed");
                _scan_state = ScanState::IDLE;
            }
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                DEBUG("BluedroidPlatform: Scan started");
                _scan_state = ScanState::ACTIVE;
            } else {
                ERROR("BluedroidPlatform: Scan start failed");
                _scan_state = ScanState::IDLE;
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            handleScanResult(param);
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            DEBUG("BluedroidPlatform: Scan stopped");
            _scan_state = ScanState::IDLE;
            handleScanComplete();
            break;

        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                DEBUG("BluedroidPlatform: Adv data set");
                _adv_state = AdvState::CONFIGURING_SCAN_RSP;
                buildScanResponseData();
            } else {
                ERROR("BluedroidPlatform: Adv data set failed");
                _adv_state = AdvState::IDLE;
            }
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            if (param->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                DEBUG("BluedroidPlatform: Scan response set, starting advertising");
                _adv_state = AdvState::STARTING;

                esp_ble_adv_params_t adv_params = {
                    .adv_int_min = static_cast<uint16_t>((_config.adv_interval_min_ms * 1000) / 625),
                    .adv_int_max = static_cast<uint16_t>((_config.adv_interval_max_ms * 1000) / 625),
                    .adv_type = ADV_TYPE_IND,
                    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,  // Use public address (chip MAC)
                    .peer_addr = {0},
                    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
                    .channel_map = ADV_CHNL_ALL,
                    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY
                };

                esp_ble_gap_start_advertising(&adv_params);
            } else {
                ERROR("BluedroidPlatform: Scan response set failed");
                _adv_state = AdvState::IDLE;
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            handleAdvStart(param->adv_start_cmpl.status);
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            handleAdvStop();
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            DEBUG("BluedroidPlatform: Connection params updated");
            break;

        default:
            break;
    }
}

void BluedroidPlatform::handleScanResult(esp_ble_gap_cb_param_t* param) {
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            // Scan duration complete
            _scan_state = ScanState::IDLE;
            handleScanComplete();
        }
        return;
    }

    // Parse advertising data for service UUID
    uint8_t* adv_data = param->scan_rst.ble_adv;
    uint8_t adv_len = param->scan_rst.adv_data_len;

    // Look for 128-bit service UUID
    bool has_service = false;
    uint8_t service_uuid_bytes[16];
    const char* uuid_str = UUID::SERVICE;
    int idx = 15;
    for (int i = 0; i < 36 && idx >= 0; i++) {
        if (uuid_str[i] == '-') continue;
        unsigned int byte_val;
        sscanf(&uuid_str[i], "%02x", &byte_val);
        service_uuid_bytes[idx--] = static_cast<uint8_t>(byte_val);
        i++;
    }

    // Parse AD structures
    int pos = 0;
    while (pos < adv_len) {
        uint8_t len = adv_data[pos++];
        if (len == 0 || pos + len > adv_len) break;

        uint8_t type = adv_data[pos];
        if (type == ESP_BLE_AD_TYPE_128SRV_CMPL ||
            type == ESP_BLE_AD_TYPE_128SRV_PART) {
            // Check if UUID matches
            if (len >= 17 &&
                memcmp(&adv_data[pos + 1], service_uuid_bytes, 16) == 0) {
                has_service = true;
            }
        }
        pos += len;
    }

    // Build scan result
    ScanResult result;
    result.address = fromEspBdAddr(param->scan_rst.bda, param->scan_rst.ble_addr_type);
    result.rssi = param->scan_rst.rssi;
    result.connectable = (param->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_ADV);
    result.has_reticulum_service = has_service;

    // Get device name if present
    uint8_t* name = esp_ble_resolve_adv_data(adv_data, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_len);
    if (name && adv_len > 0) {
        result.name = std::string(reinterpret_cast<char*>(name), adv_len);
    }

    if (_on_scan_result && result.has_reticulum_service) {
        _on_scan_result(result);
    }
}

void BluedroidPlatform::handleScanComplete() {
    if (_on_scan_complete) {
        _on_scan_complete();
    }

    // In dual mode, restart advertising after scan
    if (_config.role == Role::DUAL && _init_state == InitState::READY) {
        startAdvertising();
    }
}

void BluedroidPlatform::handleAdvStart(esp_bt_status_t status) {
    if (status == ESP_BT_STATUS_SUCCESS) {
        INFO("BluedroidPlatform: Advertising started");
        _adv_state = AdvState::ACTIVE;
    } else {
        ERROR("BluedroidPlatform: Advertising start failed: " + std::to_string(status));
        _adv_state = AdvState::IDLE;
    }
}

void BluedroidPlatform::handleAdvStop() {
    DEBUG("BluedroidPlatform: Advertising stopped");
    _adv_state = AdvState::IDLE;
}

//=============================================================================
// GATTS Event Handler
//=============================================================================

void BluedroidPlatform::handleGattsEvent(esp_gatts_cb_event_t event,
                                          esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t* param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            handleGattsRegister(gatts_if, param->reg.status);
            break;

        case ESP_GATTS_CREATE_EVT:
            handleGattsServiceCreated(param->create.service_handle);
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            handleGattsCharAdded(param->add_char.attr_handle, &param->add_char.char_uuid);
            break;

        case ESP_GATTS_START_EVT:
            handleGattsServiceStarted();
            break;

        case ESP_GATTS_CONNECT_EVT:
            handleGattsConnect(param);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            handleGattsDisconnect(param);
            break;

        case ESP_GATTS_WRITE_EVT:
            handleGattsWrite(param);
            break;

        case ESP_GATTS_READ_EVT:
            handleGattsRead(param);
            break;

        case ESP_GATTS_MTU_EVT:
            handleGattsMtuChange(param);
            break;

        case ESP_GATTS_CONF_EVT:
            handleGattsConfirm(param);
            break;

        default:
            break;
    }
}

void BluedroidPlatform::handleGattsRegister(esp_gatt_if_t gatts_if, esp_gatt_status_t status) {
    if (status != ESP_GATT_OK) {
        ERROR("BluedroidPlatform: GATTS register failed: " + std::to_string(status));
        return;
    }

    _gatts_if = gatts_if;
    DEBUG("BluedroidPlatform: GATTS registered, if=" + std::to_string(gatts_if));

    // Create the Reticulum service
    setupGattsService();
}

bool BluedroidPlatform::setupGattsService() {
    // Parse service UUID
    esp_gatt_srvc_id_t service_id;
    service_id.is_primary = true;
    service_id.id.inst_id = 0;
    service_id.id.uuid.len = ESP_UUID_LEN_128;

    const char* uuid_str = UUID::SERVICE;
    int idx = 15;
    for (int i = 0; i < 36 && idx >= 0; i++) {
        if (uuid_str[i] == '-') continue;
        unsigned int byte_val;
        sscanf(&uuid_str[i], "%02x", &byte_val);
        service_id.id.uuid.uuid.uuid128[idx--] = static_cast<uint8_t>(byte_val);
        i++;
    }

    // Calculate number of handles needed:
    // 1 for service, 2 per characteristic (decl + value), 1 for CCCD on TX
    // Service + RX(2) + TX(2+1 CCCD) + Identity(2) = 8 handles
    esp_err_t ret = esp_ble_gatts_create_service(_gatts_if, &service_id, 10);
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Create service failed: " + std::to_string(ret));
        return false;
    }

    _init_state = InitState::GATTS_CREATING_SERVICE;
    return true;
}

void BluedroidPlatform::handleGattsServiceCreated(uint16_t service_handle) {
    _service_handle = service_handle;
    DEBUG("BluedroidPlatform: Service created, handle=" + std::to_string(service_handle));

    _init_state = InitState::GATTS_ADDING_CHARS;
    _chars_added = 0;

    // Helper to parse UUID string to esp_bt_uuid_t
    auto parseUuid = [](const char* uuid_str, esp_bt_uuid_t& uuid) {
        uuid.len = ESP_UUID_LEN_128;
        int idx = 15;
        for (int i = 0; i < 36 && idx >= 0; i++) {
            if (uuid_str[i] == '-') continue;
            unsigned int byte_val;
            sscanf(&uuid_str[i], "%02x", &byte_val);
            uuid.uuid.uuid128[idx--] = static_cast<uint8_t>(byte_val);
            i++;
        }
    };

    // Add RX characteristic (central writes here)
    esp_bt_uuid_t rx_uuid;
    parseUuid(UUID::RX_CHAR, rx_uuid);
    esp_err_t ret = esp_ble_gatts_add_char(
        _service_handle,
        &rx_uuid,
        ESP_GATT_PERM_WRITE,
        ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
        nullptr,
        nullptr
    );
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Add RX char failed: " + std::to_string(ret));
    }

    // Add TX characteristic (we notify from here)
    esp_bt_uuid_t tx_uuid;
    parseUuid(UUID::TX_CHAR, tx_uuid);
    ret = esp_ble_gatts_add_char(
        _service_handle,
        &tx_uuid,
        ESP_GATT_PERM_READ,
        ESP_GATT_CHAR_PROP_BIT_NOTIFY,
        nullptr,
        nullptr
    );
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Add TX char failed: " + std::to_string(ret));
    }

    // Add Identity characteristic (central reads)
    esp_bt_uuid_t id_uuid;
    parseUuid(UUID::IDENTITY_CHAR, id_uuid);
    ret = esp_ble_gatts_add_char(
        _service_handle,
        &id_uuid,
        ESP_GATT_PERM_READ,
        ESP_GATT_CHAR_PROP_BIT_READ,
        nullptr,
        nullptr
    );
    if (ret != ESP_OK) {
        ERROR("BluedroidPlatform: Add Identity char failed: " + std::to_string(ret));
    }
}

void BluedroidPlatform::handleGattsCharAdded(uint16_t attr_handle, esp_bt_uuid_t* char_uuid) {
    // Determine which characteristic was added by comparing UUID
    // For simplicity, we track by order of addition
    _chars_added++;
    DEBUG("BluedroidPlatform: Char added, handle=" + std::to_string(attr_handle) +
          " (" + std::to_string(_chars_added) + "/" + std::to_string(CHARS_EXPECTED) + ")");

    switch (_chars_added) {
        case 1:
            _rx_char_handle = attr_handle;
            break;
        case 2:
            _tx_char_handle = attr_handle;
            // TX needs CCCD for notifications - will be auto-added by stack
            _tx_cccd_handle = attr_handle + 1;  // CCCD is typically handle+1
            break;
        case 3:
            _identity_char_handle = attr_handle;
            break;
    }

    if (_chars_added >= CHARS_EXPECTED) {
        // All characteristics added, start service
        _init_state = InitState::GATTS_STARTING_SERVICE;
        esp_err_t ret = esp_ble_gatts_start_service(_service_handle);
        if (ret != ESP_OK) {
            ERROR("BluedroidPlatform: Start service failed: " + std::to_string(ret));
        }
    }
}

void BluedroidPlatform::handleGattsServiceStarted() {
    INFO("BluedroidPlatform: GATTS service started");

    // Check if GATTC is also ready (for dual mode)
    if (_config.role == Role::PERIPHERAL ||
        (_config.role == Role::DUAL && _gattc_if != ESP_GATT_IF_NONE)) {
        _init_state = InitState::READY;
        INFO("BluedroidPlatform: Ready for connections");

        // Start advertising if running
        if (_running) {
            startAdvertising();
        }
    }
}

void BluedroidPlatform::handleGattsConnect(esp_ble_gatts_cb_param_t* param) {
    uint16_t conn_handle = allocateConnHandle();

    BluedroidConnection conn;
    conn.conn_id = param->connect.conn_id;
    memcpy(conn.peer_addr, param->connect.remote_bda, 6);
    conn.addr_type = BLE_ADDR_TYPE_PUBLIC;  // Could be determined from GAP
    conn.local_role = Role::PERIPHERAL;
    conn.mtu = MTU::MINIMUM;

    _connections[conn_handle] = conn;

    // Log the mapping for debugging
    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
             param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);
    INFO("BluedroidPlatform: GATTS Connected handle=" + std::to_string(conn_handle) +
         " conn_id=" + std::to_string(param->connect.conn_id) +
         " addr=" + std::string(addr_str));

    // Stop advertising if we've hit max connections
    if (getConnectionCount() >= _config.max_connections) {
        DEBUG("BluedroidPlatform: At max connections, stopping advertising");
        stopAdvertising();
    }

    if (_on_central_connected) {
        ConnectionHandle ch = getConnection(conn_handle);
        _on_central_connected(ch);
    }
}

void BluedroidPlatform::handleGattsDisconnect(esp_ble_gatts_cb_param_t* param) {
    // Find connection by conn_id
    uint16_t conn_handle = 0xFFFF;
    for (auto& pair : _connections) {
        if (pair.second.conn_id == param->disconnect.conn_id) {
            conn_handle = pair.first;
            break;
        }
    }

    if (conn_handle != 0xFFFF) {
        INFO("BluedroidPlatform: Central disconnected, handle=" + std::to_string(conn_handle));

        if (_on_central_disconnected) {
            ConnectionHandle ch = getConnection(conn_handle);
            _on_central_disconnected(ch);
        }

        _connections.erase(conn_handle);
    }

    // In dual mode, restart advertising
    if (_config.role == Role::DUAL || _config.role == Role::PERIPHERAL) {
        if (_running) {
            startAdvertising();
        }
    }
}

void BluedroidPlatform::handleGattsWrite(esp_ble_gatts_cb_param_t* param) {
    // Hot path - no logging here to avoid blocking main loop

    if (!param->write.is_prep) {
        // Regular write (not prepare write)
        Bytes data(param->write.value, param->write.len);

        // Find connection
        uint16_t conn_handle = 0xFFFF;
        for (auto& pair : _connections) {
            if (pair.second.conn_id == param->write.conn_id) {
                conn_handle = pair.first;
                break;
            }
        }

        if (param->write.handle == _rx_char_handle) {
            // Data write to RX characteristic
            if (_on_write_received && conn_handle != 0xFFFF) {
                ConnectionHandle ch = getConnection(conn_handle);
                _on_write_received(ch, data);
            } else {
                WARNING("BluedroidPlatform: No callback or invalid handle for RX write");
            }
        } else if (param->write.handle == _tx_cccd_handle) {
            // Notification enable/disable
            bool enabled = (param->write.len >= 2 && param->write.value[0] != 0);
            if (conn_handle != 0xFFFF) {
                auto* conn = findConnection(conn_handle);
                if (conn) {
                    conn->notifications_enabled = enabled;
                }
                if (_on_notify_enabled) {
                    ConnectionHandle ch = getConnection(conn_handle);
                    _on_notify_enabled(ch, enabled);
                }
            }
        }

        // Send response if needed
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(_gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, nullptr);
        }
    }
}

void BluedroidPlatform::handleGattsRead(esp_ble_gatts_cb_param_t* param) {
    // Hot path - no logging here to avoid blocking main loop

    esp_gatt_rsp_t rsp;
    memset(&rsp, 0, sizeof(rsp));

    if (param->read.handle == _identity_char_handle) {
        // Return identity data
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = _identity_data.size();
        if (rsp.attr_value.len > ESP_GATT_MAX_ATTR_LEN) {
            rsp.attr_value.len = ESP_GATT_MAX_ATTR_LEN;
        }
        memcpy(rsp.attr_value.value, _identity_data.data(), rsp.attr_value.len);

        esp_ble_gatts_send_response(_gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
    } else {
        // Unknown characteristic - send error
        esp_ble_gatts_send_response(_gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_READ_NOT_PERMIT, nullptr);
    }
}

void BluedroidPlatform::handleGattsMtuChange(esp_ble_gatts_cb_param_t* param) {
    uint16_t conn_handle = 0xFFFF;
    for (auto& pair : _connections) {
        if (pair.second.conn_id == param->mtu.conn_id) {
            conn_handle = pair.first;
            pair.second.mtu = param->mtu.mtu;
            break;
        }
    }

    DEBUG("BluedroidPlatform: MTU changed to " + std::to_string(param->mtu.mtu));

    if (_on_mtu_changed && conn_handle != 0xFFFF) {
        ConnectionHandle ch = getConnection(conn_handle);
        _on_mtu_changed(ch, param->mtu.mtu);
    }
}

void BluedroidPlatform::handleGattsConfirm(esp_ble_gatts_cb_param_t* param) {
    // Notification confirmation (indication acknowledgment)
    // For notifications (not indications), this may not be called
}

//=============================================================================
// GATTC Event Handler
//=============================================================================

void BluedroidPlatform::handleGattcEvent(esp_gattc_cb_event_t event,
                                          esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t* param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            handleGattcRegister(gattc_if, param->reg.status);
            break;

        case ESP_GATTC_OPEN_EVT:
            handleGattcConnect(param);
            break;

        case ESP_GATTC_CLOSE_EVT:
        case ESP_GATTC_DISCONNECT_EVT:
            handleGattcDisconnect(param);
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            DEBUG("BluedroidPlatform: SEARCH_RES_EVT received, conn_id=" +
                  std::to_string(param->search_res.conn_id));
            handleGattcSearchResult(param);
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            DEBUG("BluedroidPlatform: SEARCH_CMPL_EVT received, conn_id=" +
                  std::to_string(param->search_cmpl.conn_id) +
                  " status=" + std::to_string(param->search_cmpl.status));
            handleGattcSearchComplete(param);
            break;

        case ESP_GATTC_NOTIFY_EVT:
            handleGattcNotify(param);
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT:
            handleGattcWrite(param);
            break;

        case ESP_GATTC_READ_CHAR_EVT:
            handleGattcRead(param);
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.status == ESP_GATT_OK) {
                DEBUG("BluedroidPlatform: Registered for notifications on handle " +
                      std::to_string(param->reg_for_notify.handle));
            } else {
                WARNING("BluedroidPlatform: Failed to register for notify: " +
                        std::to_string(param->reg_for_notify.status));
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            // MTU exchange complete (when we're central)
            if (param->cfg_mtu.status == ESP_GATT_OK) {
                INFO("BluedroidPlatform: GATTC MTU configured to " + std::to_string(param->cfg_mtu.mtu));
                // Find connection and update MTU
                for (auto& pair : _connections) {
                    if (pair.second.conn_id == param->cfg_mtu.conn_id) {
                        pair.second.mtu = param->cfg_mtu.mtu;
                        if (_on_mtu_changed) {
                            ConnectionHandle ch = getConnection(pair.first);
                            _on_mtu_changed(ch, param->cfg_mtu.mtu);
                        }
                        break;
                    }
                }
            } else {
                WARNING("BluedroidPlatform: MTU config failed: " + std::to_string(param->cfg_mtu.status));
            }
            break;

        default:
            break;
    }
}

void BluedroidPlatform::handleGattcRegister(esp_gatt_if_t gattc_if, esp_gatt_status_t status) {
    if (status != ESP_GATT_OK) {
        ERROR("BluedroidPlatform: GATTC register failed: " + std::to_string(status));
        return;
    }

    _gattc_if = gattc_if;
    DEBUG("BluedroidPlatform: GATTC registered, if=" + std::to_string(gattc_if));

    // If in dual mode and GATTS is ready, mark as fully ready
    if (_config.role == Role::CENTRAL ||
        (_config.role == Role::DUAL && _init_state == InitState::READY)) {
        // Already ready or central-only mode
    } else if (_init_state >= InitState::GATTS_STARTING_SERVICE) {
        // GATTS service started, now we're fully ready
        _init_state = InitState::READY;
        INFO("BluedroidPlatform: Ready for connections (GATTC registered)");
    }
}

void BluedroidPlatform::handleGattcConnect(esp_ble_gattc_cb_param_t* param) {
    _connect_pending = false;

    if (param->open.status != ESP_GATT_OK) {
        ERROR("BluedroidPlatform: Connection failed: " + std::to_string(param->open.status));
        _connect_success = false;
        _connect_error = param->open.status;
        // Try to start queued discoveries since connection finished
        if (!_pending_discoveries.empty() && _discovery_in_progress == 0xFFFF) {
            uint16_t next_handle = _pending_discoveries.front();
            _pending_discoveries.pop();
            DEBUG("BluedroidPlatform: Starting queued discovery for handle " + std::to_string(next_handle));
            discoverServices(next_handle);
        }
        return;
    }

    _connect_success = true;
    uint16_t conn_handle = allocateConnHandle();

    BluedroidConnection conn;
    conn.conn_id = param->open.conn_id;
    memcpy(conn.peer_addr, param->open.remote_bda, 6);
    conn.addr_type = BLE_ADDR_TYPE_PUBLIC;
    conn.local_role = Role::CENTRAL;
    conn.mtu = param->open.mtu;

    _connections[conn_handle] = conn;

    // Log the mapping for debugging
    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             param->open.remote_bda[0], param->open.remote_bda[1], param->open.remote_bda[2],
             param->open.remote_bda[3], param->open.remote_bda[4], param->open.remote_bda[5]);
    INFO("BluedroidPlatform: GATTC Connected handle=" + std::to_string(conn_handle) +
         " conn_id=" + std::to_string(param->open.conn_id) +
         " addr=" + std::string(addr_str));

    if (_on_connected) {
        ConnectionHandle ch = getConnection(conn_handle);
        _on_connected(ch);
    }

    // Request MTU update
    esp_ble_gattc_send_mtu_req(_gattc_if, param->open.conn_id);
}

void BluedroidPlatform::handleGattcDisconnect(esp_ble_gattc_cb_param_t* param) {
    _connect_pending = false;

    uint16_t conn_handle = 0xFFFF;
    for (auto& pair : _connections) {
        if (pair.second.conn_id == param->close.conn_id) {
            conn_handle = pair.first;
            break;
        }
    }

    if (conn_handle != 0xFFFF) {
        INFO("BluedroidPlatform: Peripheral disconnected, handle=" + std::to_string(conn_handle));

        // If this connection was doing discovery, clear the flag and start next
        if (_discovery_in_progress == conn_handle) {
            _discovery_in_progress = 0xFFFF;
            if (!_pending_discoveries.empty()) {
                uint16_t next_handle = _pending_discoveries.front();
                _pending_discoveries.pop();
                DEBUG("BluedroidPlatform: Starting queued discovery for handle " + std::to_string(next_handle));
                discoverServices(next_handle);
            }
        }

        // Remove this connection from pending discoveries queue
        std::queue<uint16_t> temp_queue;
        while (!_pending_discoveries.empty()) {
            uint16_t h = _pending_discoveries.front();
            _pending_discoveries.pop();
            if (h != conn_handle) {
                temp_queue.push(h);
            }
        }
        _pending_discoveries = std::move(temp_queue);

        if (_on_disconnected) {
            ConnectionHandle ch = getConnection(conn_handle);
            _on_disconnected(ch, param->close.reason);
        }

        _connections.erase(conn_handle);
    }
}

void BluedroidPlatform::handleGattcSearchResult(esp_ble_gattc_cb_param_t* param) {
    // Find connection - ONLY search central (GATTC) connections
    BluedroidConnection* conn = nullptr;
    for (auto& pair : _connections) {
        if (pair.second.conn_id == param->search_res.conn_id &&
            pair.second.local_role == Role::CENTRAL) {
            conn = &pair.second;
            break;
        }
    }

    if (!conn) return;

    conn->service_start_handle = param->search_res.start_handle;
    conn->service_end_handle = param->search_res.end_handle;
    DEBUG("BluedroidPlatform: Found service, handles " +
          std::to_string(conn->service_start_handle) + "-" +
          std::to_string(conn->service_end_handle));
}

void BluedroidPlatform::handleGattcSearchComplete(esp_ble_gattc_cb_param_t* param) {
    // Find connection - ONLY search central (GATTC) connections
    BluedroidConnection* conn = nullptr;
    uint16_t conn_handle = 0xFFFF;
    for (auto& pair : _connections) {
        if (pair.second.conn_id == param->search_cmpl.conn_id &&
            pair.second.local_role == Role::CENTRAL) {
            conn = &pair.second;
            conn_handle = pair.first;
            break;
        }
    }

    if (!conn) {
        ERROR("BluedroidPlatform: handleGattcSearchComplete - connection not found for conn_id=" +
              std::to_string(param->search_cmpl.conn_id));
        // Clear discovery state and try next
        _discovery_in_progress = 0xFFFF;
        if (!_pending_discoveries.empty()) {
            uint16_t next_handle = _pending_discoveries.front();
            _pending_discoveries.pop();
            discoverServices(next_handle);
        }
        return;
    }

    DEBUG("BluedroidPlatform: handleGattcSearchComplete for conn_handle=" + std::to_string(conn_handle) +
          " service_start=" + std::to_string(conn->service_start_handle));

    if (conn->service_start_handle == 0) {
        ERROR("BluedroidPlatform: Service not found for conn " + std::to_string(conn_handle));
        // Clear discovery state and try next
        _discovery_in_progress = 0xFFFF;
        if (_on_services_discovered) {
            ConnectionHandle ch = getConnection(conn_handle);
            _on_services_discovered(ch, false);
        }
        if (!_pending_discoveries.empty()) {
            uint16_t next_handle = _pending_discoveries.front();
            _pending_discoveries.pop();
            discoverServices(next_handle);
        }
        return;
    }

    // Get all characteristics in the service
    conn->discovery_state = BluedroidConnection::DiscoveryState::GETTING_CHARS;

    // Get characteristic count first
    uint16_t count = 0;
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
        _gattc_if,
        param->search_cmpl.conn_id,
        ESP_GATT_DB_CHARACTERISTIC,
        conn->service_start_handle,
        conn->service_end_handle,
        0,
        &count
    );

    if (status != ESP_GATT_OK || count == 0) {
        ERROR("BluedroidPlatform: No characteristics found, status=" + std::to_string(status) +
              " count=" + std::to_string(count));
        conn->discovery_state = BluedroidConnection::DiscoveryState::COMPLETE;
        // Clear discovery state and try next
        _discovery_in_progress = 0xFFFF;
        if (_on_services_discovered) {
            ConnectionHandle ch = getConnection(conn_handle);
            _on_services_discovered(ch, false);
        }
        if (!_pending_discoveries.empty()) {
            uint16_t next_handle = _pending_discoveries.front();
            _pending_discoveries.pop();
            discoverServices(next_handle);
        }
        return;
    }

    {
        char buf[80];
        snprintf(buf, sizeof(buf), "BluedroidPlatform: Found %u characteristics for conn_handle=%u", count, conn_handle);
        DEBUG(buf);
    }

    // Get all characteristics - use fixed stack buffer to avoid heap fragmentation
    // We only need RX, TX, Identity chars so 16 is plenty (most services have <10)
    static const uint16_t MAX_CHARS = 16;
    esp_gattc_char_elem_t chars[MAX_CHARS];
    if (count > MAX_CHARS) {
        count = MAX_CHARS;  // Limit to what we can handle
    }
    status = esp_ble_gattc_get_all_char(
        _gattc_if,
        param->search_cmpl.conn_id,
        conn->service_start_handle,
        conn->service_end_handle,
        chars,
        &count,
        0
    );

    if (status == ESP_GATT_OK) {
        // Parse UUID helper (same logic as buildAdvertisingData)
        auto uuidMatches = [](const esp_bt_uuid_t& uuid, const char* uuid_str) -> bool {
            if (uuid.len != ESP_UUID_LEN_128) return false;
            uint8_t expected[16];
            int idx = 15;
            for (int i = 0; i < 36 && idx >= 0; i++) {
                if (uuid_str[i] == '-') continue;
                unsigned int byte_val;
                sscanf(&uuid_str[i], "%02x", &byte_val);
                expected[idx--] = static_cast<uint8_t>(byte_val);
                i++;
            }
            return memcmp(uuid.uuid.uuid128, expected, 16) == 0;
        };

        char buf[64];
        for (uint16_t i = 0; i < count; i++) {
            if (uuidMatches(chars[i].uuid, UUID::RX_CHAR)) {
                conn->rx_char_handle = chars[i].char_handle;
                snprintf(buf, sizeof(buf), "BluedroidPlatform: Found RX char, handle=%u", conn->rx_char_handle);
                DEBUG(buf);
            } else if (uuidMatches(chars[i].uuid, UUID::TX_CHAR)) {
                conn->tx_char_handle = chars[i].char_handle;
                snprintf(buf, sizeof(buf), "BluedroidPlatform: Found TX char, handle=%u", conn->tx_char_handle);
                DEBUG(buf);

                // Get descriptor for TX (CCCD)
                uint16_t desc_count = 1;
                esp_gattc_descr_elem_t descr;
                esp_bt_uuid_t cccd_uuid;
                cccd_uuid.len = ESP_UUID_LEN_16;
                cccd_uuid.uuid.uuid16 = 0x2902;  // CCCD UUID

                if (esp_ble_gattc_get_descr_by_char_handle(
                        _gattc_if, param->search_cmpl.conn_id,
                        chars[i].char_handle, cccd_uuid,
                        &descr, &desc_count) == ESP_GATT_OK && desc_count > 0) {
                    conn->tx_cccd_handle = descr.handle;
                    snprintf(buf, sizeof(buf), "BluedroidPlatform: Found TX CCCD, handle=%u", conn->tx_cccd_handle);
                    DEBUG(buf);
                }
            } else if (uuidMatches(chars[i].uuid, UUID::IDENTITY_CHAR)) {
                conn->identity_char_handle = chars[i].char_handle;
                snprintf(buf, sizeof(buf), "BluedroidPlatform: Found Identity char, handle=%u", conn->identity_char_handle);
                DEBUG(buf);
            }
        }
    }

    // chars is stack-allocated, no delete needed

    conn->discovery_state = BluedroidConnection::DiscoveryState::COMPLETE;

    bool success = (conn->rx_char_handle != 0 && conn->tx_char_handle != 0);
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "BluedroidPlatform: Service discovery complete for conn %u RX=%u TX=%u CCCD=%u Identity=%u",
                 conn_handle, conn->rx_char_handle, conn->tx_char_handle, conn->tx_cccd_handle, conn->identity_char_handle);
        INFO(buf);
    }

    if (_on_services_discovered) {
        ConnectionHandle ch = getConnection(conn_handle);
        ch.rx_char_handle = conn->rx_char_handle;
        ch.tx_char_handle = conn->tx_char_handle;
        ch.tx_cccd_handle = conn->tx_cccd_handle;
        ch.identity_handle = conn->identity_char_handle;
        _on_services_discovered(ch, success);
    }

    // Clear discovery in progress and start next queued discovery
    _discovery_in_progress = 0xFFFF;
    if (!_pending_discoveries.empty()) {
        taskYIELD();  // Let stack process before next discovery
        uint16_t next_handle = _pending_discoveries.front();
        _pending_discoveries.pop();
        DEBUG("BluedroidPlatform: Starting queued discovery for handle " + std::to_string(next_handle));
        discoverServices(next_handle);
    }
}

void BluedroidPlatform::handleGattcNotify(esp_ble_gattc_cb_param_t* param) {
    // Hot path - no logging to avoid heap allocation on every packet

    // Find connection - ONLY search central (GATTC) connections
    uint16_t conn_handle = 0xFFFF;
    for (auto& pair : _connections) {
        if (pair.second.conn_id == param->notify.conn_id &&
            pair.second.local_role == Role::CENTRAL) {
            conn_handle = pair.first;
            break;
        }
    }

    if (conn_handle == 0xFFFF) {
        WARNING("BluedroidPlatform: No CENTRAL connection found for notify conn_id=" +
                std::to_string(param->notify.conn_id));
        return;
    }

    Bytes data(param->notify.value, param->notify.value_len);

    if (_on_data_received) {
        ConnectionHandle ch = getConnection(conn_handle);
        _on_data_received(ch, data);
    }
}

void BluedroidPlatform::handleGattcWrite(esp_ble_gattc_cb_param_t* param) {
    // Only log failures - hot path, avoid string allocation on success
    if (param->write.status != ESP_GATT_OK) {
        WARNING("BluedroidPlatform: Write FAILED conn_id=" + std::to_string(param->write.conn_id) +
                " status=" + std::to_string(param->write.status));
    }
}

void BluedroidPlatform::handleGattcRead(esp_ble_gattc_cb_param_t* param) {
    if (param->read.status != ESP_GATT_OK) {
        WARNING("BluedroidPlatform: Read failed: " + std::to_string(param->read.status));
        complete(OperationResult::ERROR, Bytes());
        return;
    }

    // Extract read data
    Bytes data(param->read.value, param->read.value_len);
    DEBUG("BluedroidPlatform: Read complete, " + std::to_string(data.size()) + " bytes");

    // Complete the pending operation with the data
    complete(OperationResult::SUCCESS, data);
}

//=============================================================================
// Address Conversion
//=============================================================================

BLEAddress BluedroidPlatform::fromEspBdAddr(const esp_bd_addr_t addr,
                                             esp_ble_addr_type_t type) {
    BLEAddress result;
    memcpy(result.addr, addr, 6);
    result.type = type;
    return result;
}

void BluedroidPlatform::toEspBdAddr(const BLEAddress& addr, esp_bd_addr_t out_addr) {
    memcpy(out_addr, addr.addr, 6);
}

//=============================================================================
// Connection Handle Management
//=============================================================================

uint16_t BluedroidPlatform::allocateConnHandle() {
    return _next_conn_handle++;
}

void BluedroidPlatform::freeConnHandle(uint16_t handle) {
    // Handle reuse not implemented - with 16-bit handles, unlikely to overflow
}

BluedroidPlatform::BluedroidConnection* BluedroidPlatform::findConnection(uint16_t conn_handle) {
    auto it = _connections.find(conn_handle);
    if (it != _connections.end()) {
        return &it->second;
    }
    return nullptr;
}

BluedroidPlatform::BluedroidConnection* BluedroidPlatform::findConnectionByAddress(
    const esp_bd_addr_t addr) {
    for (auto& pair : _connections) {
        if (memcmp(pair.second.peer_addr, addr, 6) == 0) {
            return &pair.second;
        }
    }
    return nullptr;
}

}} // namespace RNS::BLE

#endif // ESP32 && USE_BLUEDROID

/**
 * @file NimBLEPlatform.cpp
 * @brief NimBLE-Arduino implementation for ESP32
 */

#include "NimBLEPlatform.h"

#if defined(ESP32) && (defined(USE_NIMBLE) || defined(CONFIG_BT_NIMBLE_ENABLED))

#include "Log.h"
#include <algorithm>

// WiFi coexistence: Check if WiFi is available and connected
// This is used to add extra delays before BLE connection attempts
#if __has_include(<WiFi.h>)
    #include <WiFi.h>
    #define HAS_WIFI_COEX 1
#else
    #define HAS_WIFI_COEX 0
#endif

// NimBLE low-level GAP functions for checking stack state and native connections
extern "C" {
    #include "nimble/nimble/host/include/host/ble_gap.h"
    #include "nimble/nimble/host/include/host/ble_hs.h"

    int ble_gap_adv_active(void);
    int ble_gap_disc_active(void);
    int ble_gap_conn_active(void);
}

namespace RNS { namespace BLE {

//=============================================================================
// Static Member Initialization
//=============================================================================

// Unclean shutdown flag - persists across soft reboot on ESP32
// RTC_NOINIT_ATTR places in RTC slow memory which survives soft reset
#ifdef ESP32
RTC_NOINIT_ATTR
#endif
bool NimBLEPlatform::_unclean_shutdown = false;

//=============================================================================
// State Name Helpers (for logging)
//=============================================================================

const char* masterStateName(MasterState state) {
    switch (state) {
        case MasterState::IDLE: return "IDLE";
        case MasterState::SCAN_STARTING: return "SCAN_STARTING";
        case MasterState::SCANNING: return "SCANNING";
        case MasterState::SCAN_STOPPING: return "SCAN_STOPPING";
        case MasterState::CONN_STARTING: return "CONN_STARTING";
        case MasterState::CONNECTING: return "CONNECTING";
        case MasterState::CONN_CANCELING: return "CONN_CANCELING";
        default: return "UNKNOWN";
    }
}

const char* slaveStateName(SlaveState state) {
    switch (state) {
        case SlaveState::IDLE: return "IDLE";
        case SlaveState::ADV_STARTING: return "ADV_STARTING";
        case SlaveState::ADVERTISING: return "ADVERTISING";
        case SlaveState::ADV_STOPPING: return "ADV_STOPPING";
        default: return "UNKNOWN";
    }
}

const char* gapStateName(GAPState state) {
    switch (state) {
        case GAPState::UNINITIALIZED: return "UNINITIALIZED";
        case GAPState::INITIALIZING: return "INITIALIZING";
        case GAPState::READY: return "READY";
        case GAPState::MASTER_PRIORITY: return "MASTER_PRIORITY";
        case GAPState::SLAVE_PRIORITY: return "SLAVE_PRIORITY";
        case GAPState::TRANSITIONING: return "TRANSITIONING";
        case GAPState::ERROR_RECOVERY: return "ERROR_RECOVERY";
        default: return "UNKNOWN";
    }
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

NimBLEPlatform::NimBLEPlatform() {
    // Initialize connection mutex
    _conn_mutex = xSemaphoreCreateMutex();
}

NimBLEPlatform::~NimBLEPlatform() {
    shutdown();
    if (_conn_mutex) {
        vSemaphoreDelete(_conn_mutex);
        _conn_mutex = nullptr;
    }
}

//=============================================================================
// Lifecycle
//=============================================================================

bool NimBLEPlatform::initialize(const PlatformConfig& config) {
    if (_initialized) {
        WARNING("NimBLEPlatform: Already initialized");
        return true;
    }

    _config = config;

    // Initialize NimBLE
    NimBLEDevice::init(_config.device_name);

    // Address type for ESP32-S3:
    // - BLE_OWN_ADDR_PUBLIC fails with error 13 (ETIMEOUT) for client connections
    // - BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT also fails with error 13
    // - BLE_OWN_ADDR_RANDOM works for client connections
    // Using RANDOM address allows connections to work. Role negotiation is handled
    // by always initiating connections and using identity-based duplicate detection.
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

    // Set power level (ESP32)
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Set MTU
    NimBLEDevice::setMTU(_config.preferred_mtu);

    // Setup server (peripheral mode)
    if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
        if (!setupServer()) {
            ERROR("NimBLEPlatform: Failed to setup server");
            return false;
        }
    }

    // Setup scan (central mode)
    if (_config.role == Role::CENTRAL || _config.role == Role::DUAL) {
        if (!setupScan()) {
            ERROR("NimBLEPlatform: Failed to setup scan");
            return false;
        }
    }

    _initialized = true;

    // Set GAP state to READY
    portENTER_CRITICAL(&_state_mux);
    _gap_state = GAPState::READY;
    portEXIT_CRITICAL(&_state_mux);

    INFO("NimBLEPlatform: Initialized, role: " + std::string(roleToString(_config.role)));

    return true;
}

bool NimBLEPlatform::start() {
    if (!_initialized) {
        ERROR("NimBLEPlatform: Not initialized");
        return false;
    }

    if (_running) {
        return true;
    }

    // Start advertising if peripheral mode
    if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
        if (!startAdvertising()) {
            WARNING("NimBLEPlatform: Failed to start advertising");
        }
    }

    _running = true;
    INFO("NimBLEPlatform: Started");

    return true;
}

void NimBLEPlatform::stop() {
    if (!_running) {
        return;
    }

    stopScan();
    stopAdvertising();
    disconnectAll();

    _running = false;
    INFO("NimBLEPlatform: Stopped");
}

void NimBLEPlatform::loop() {
    if (!_running) {
        return;
    }

    // Check if continuous scan should stop
    portENTER_CRITICAL(&_state_mux);
    MasterState ms = _master_state;
    portEXIT_CRITICAL(&_state_mux);

    if (ms == MasterState::SCANNING && _scan_stop_time > 0 && millis() >= _scan_stop_time) {
        DEBUG("NimBLEPlatform: Stopping scan after timeout");
        stopScan();

        if (_on_scan_complete) {
            _on_scan_complete();
        }
    }

    // Process operation queue
    BLEOperationQueue::process();
}

void NimBLEPlatform::shutdown() {
    INFO("NimBLEPlatform: Beginning graceful shutdown");

    // CONC-H4: Graceful shutdown timeout for active write operations
    const uint32_t SHUTDOWN_TIMEOUT_MS = 10000;
    uint32_t start = millis();

    // Stop accepting new operations by transitioning GAP state
    // This prevents new connections/operations from starting
    portENTER_CRITICAL(&_state_mux);
    GAPState current_gap = _gap_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_gap == GAPState::READY) {
        transitionGAPState(GAPState::READY, GAPState::TRANSITIONING);
    }

    // Wait for active write operations to complete
    while (hasActiveWriteOperations() && (millis() - start) < SHUTDOWN_TIMEOUT_MS) {
        DEBUG("NimBLEPlatform: Waiting for " + std::to_string(_active_write_count.load()) +
              " active write operation(s)");
        // DELAY RATIONALE: Shutdown wait polling - check every 100ms for write completion
        delay(100);
    }

    // Check if we timed out
    if (hasActiveWriteOperations()) {
        WARNING("NimBLEPlatform: Shutdown timeout (" +
                std::to_string(SHUTDOWN_TIMEOUT_MS) + "ms) with " +
                std::to_string(_active_write_count.load()) + " active writes - forcing close");
        _unclean_shutdown = true;
    } else {
        DEBUG("NimBLEPlatform: All operations complete, proceeding with clean shutdown");
    }

    // Stop advertising and scanning
    stop();

    // Disconnect and cleanup clients with mutex protection
    if (xSemaphoreTake(_conn_mutex, pdMS_TO_TICKS(1000))) {
        for (auto& kv : _clients) {
            if (kv.second) {
                NimBLEDevice::deleteClient(kv.second);
            }
        }
        _clients.clear();
        _connections.clear();
        _discovered_devices.clear();
        _discovered_order.clear();
        xSemaphoreGive(_conn_mutex);
    } else {
        WARNING("NimBLEPlatform: Could not acquire mutex for cleanup - forcing cleanup");
        // Force cleanup anyway to prevent leaks
        for (auto& kv : _clients) {
            if (kv.second) {
                NimBLEDevice::deleteClient(kv.second);
            }
        }
        _clients.clear();
        _connections.clear();
        _discovered_devices.clear();
        _discovered_order.clear();
    }

    // Deinit NimBLE stack
    if (_initialized) {
        NimBLEDevice::deinit(true);
        _initialized = false;
    }

    _server = nullptr;
    _service = nullptr;
    _rx_char = nullptr;
    _tx_char = nullptr;
    _identity_char = nullptr;
    _scan = nullptr;
    _advertising_obj = nullptr;

    INFO("NimBLEPlatform: Shutdown complete" +
         std::string(wasCleanShutdown() ? "" : " (unclean - verify on boot)"));
}

bool NimBLEPlatform::isRunning() const {
    return _running;
}

//=============================================================================
// BLE Stack Recovery
//=============================================================================

bool NimBLEPlatform::recoverBLEStack() {
    // CONC-M4: Enhanced soft reset with graceful shutdown
    INFO("NimBLEPlatform: Soft reset requested");

    // Track consecutive recovery attempts using existing member variable
    _lightweight_reset_fails++;
    WARNING("NimBLEPlatform: Performing soft BLE reset (attempt " +
            std::to_string(_lightweight_reset_fails) + ")...");

    // If we've had too many consecutive recovery attempts without success,
    // the BLE stack is truly stuck. Reboot is the only reliable fix.
    if (_lightweight_reset_fails >= 5) {
        ERROR("NimBLEPlatform: BLE stack unrecoverable after " +
              std::to_string(_lightweight_reset_fails) + " attempts - rebooting device");
        // DELAY RATIONALE: Stack init settling - allow log message to flush before reboot
        delay(100);
        ESP.restart();
        return false;  // Won't reach here
    }

    // CONC-M4: Use graceful shutdown to wait for active operations
    // This ensures write operations complete before we reset state
    // Save config before shutdown clears it
    PlatformConfig saved_config = _config;

    // Perform graceful shutdown (waits for writes, cleans up properly)
    shutdown();

    // Brief delay for NimBLE host task to process shutdown
    // DELAY RATIONALE: Soft reset processing - allow stack to fully quiesce after deinit
    delay(100);

    // Reinitialize with saved config
    if (!initialize(saved_config)) {
        WARNING("NimBLEPlatform: Soft reset reinitialization failed");
        // Log detailed state for debugging
        ERROR("NimBLEPlatform: Soft reset failed - stack may need hard recovery (ESP.restart)");
        // Return false - caller can decide to trigger ESP.restart() if needed
        return false;
    }

    // Restart the platform
    if (!start()) {
        WARNING("NimBLEPlatform: Soft reset start failed");
        return false;
    }

    // Reset failure counters on successful reset
    _lightweight_reset_fails = 0;
    _scan_fail_count = 0;

    INFO("NimBLEPlatform: Soft reset complete");
    return true;
}

//=============================================================================
// State Machine Implementation
//=============================================================================

bool NimBLEPlatform::transitionMasterState(MasterState expected, MasterState new_state) {
    bool ok = false;
    portENTER_CRITICAL(&_state_mux);
    if (_master_state == expected) {
        _master_state = new_state;
        ok = true;
    }
    portEXIT_CRITICAL(&_state_mux);
    if (ok) {
        DEBUG("NimBLEPlatform: Master state: " + std::string(masterStateName(expected)) +
              " -> " + std::string(masterStateName(new_state)));
    }
    return ok;
}

bool NimBLEPlatform::transitionSlaveState(SlaveState expected, SlaveState new_state) {
    bool ok = false;
    portENTER_CRITICAL(&_state_mux);
    if (_slave_state == expected) {
        _slave_state = new_state;
        ok = true;
    }
    portEXIT_CRITICAL(&_state_mux);
    if (ok) {
        DEBUG("NimBLEPlatform: Slave state: " + std::string(slaveStateName(expected)) +
              " -> " + std::string(slaveStateName(new_state)));
    }
    return ok;
}

bool NimBLEPlatform::transitionGAPState(GAPState expected, GAPState new_state) {
    bool ok = false;
    portENTER_CRITICAL(&_state_mux);
    if (_gap_state == expected) {
        _gap_state = new_state;
        ok = true;
    }
    portEXIT_CRITICAL(&_state_mux);
    if (ok) {
        DEBUG("NimBLEPlatform: GAP state: " + std::string(gapStateName(expected)) +
              " -> " + std::string(gapStateName(new_state)));
    }
    return ok;
}

bool NimBLEPlatform::canStartScan() const {
    bool ok = false;
    portENTER_CRITICAL(&_state_mux);
    ok = (_gap_state == GAPState::READY || _gap_state == GAPState::MASTER_PRIORITY)
         && _master_state == MasterState::IDLE
         && !ble_gap_disc_active()
         && !ble_gap_conn_active();  // Also check no connection in progress
    portEXIT_CRITICAL(&_state_mux);
    return ok;
}

bool NimBLEPlatform::canStartAdvertising() const {
    bool ok = false;
    portENTER_CRITICAL(&_state_mux);
    ok = (_gap_state == GAPState::READY || _gap_state == GAPState::SLAVE_PRIORITY)
         && _slave_state == SlaveState::IDLE
         && !ble_gap_adv_active();
    portEXIT_CRITICAL(&_state_mux);
    return ok;
}

bool NimBLEPlatform::canConnect() const {
    bool ok = false;
    portENTER_CRITICAL(&_state_mux);
    ok = (_gap_state == GAPState::READY || _gap_state == GAPState::MASTER_PRIORITY)
         && _master_state == MasterState::IDLE
         && !ble_gap_conn_active();
    portEXIT_CRITICAL(&_state_mux);
    return ok;
}

bool NimBLEPlatform::pauseSlaveForMaster() {
    // Check if slave is currently advertising
    portENTER_CRITICAL(&_state_mux);
    SlaveState current_slave = _slave_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_slave == SlaveState::IDLE) {
        DEBUG("NimBLEPlatform: Slave already idle, no pause needed");
        return true;  // Already idle
    }

    if (current_slave == SlaveState::ADVERTISING) {
        // Transition to stopping
        if (!transitionSlaveState(SlaveState::ADVERTISING, SlaveState::ADV_STOPPING)) {
            WARNING("NimBLEPlatform: Failed to transition slave to ADV_STOPPING");
            return false;
        }

        // Stop advertising
        if (_advertising_obj) {
            _advertising_obj->stop();
        }

        // Also stop at low level
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }

        // Wait for advertising to stop
        uint32_t start = millis();
        while (ble_gap_adv_active() && millis() - start < 2000) {
            // DELAY RATIONALE: Advertising stop polling - check completion every NimBLE scheduler tick (~10ms)
            delay(10);
        }

        if (ble_gap_adv_active()) {
            ERROR("NimBLEPlatform: Advertising didn't stop within 2s");
            // Force state to IDLE anyway
            portENTER_CRITICAL(&_state_mux);
            _slave_state = SlaveState::IDLE;
            portEXIT_CRITICAL(&_state_mux);
            return false;
        }

        // Transition to IDLE
        portENTER_CRITICAL(&_state_mux);
        _slave_state = SlaveState::IDLE;
        portEXIT_CRITICAL(&_state_mux);

        _slave_paused_for_master = true;
        DEBUG("NimBLEPlatform: Slave paused for master operation");
        return true;
    }

    // In other states (ADV_STARTING, ADV_STOPPING), wait for completion
    uint32_t start = millis();
    while (millis() - start < 2000) {
        portENTER_CRITICAL(&_state_mux);
        current_slave = _slave_state;
        portEXIT_CRITICAL(&_state_mux);

        if (current_slave == SlaveState::IDLE) {
            _slave_paused_for_master = true;
            return true;
        }
        // DELAY RATIONALE: Slave state polling - check completion every NimBLE scheduler tick (~10ms)
        delay(10);
    }

    WARNING("NimBLEPlatform: Timed out waiting for slave to become idle");
    return false;
}

void NimBLEPlatform::resumeSlave() {
    // Atomically check and clear the paused flag to prevent race conditions
    bool should_resume = false;
    portENTER_CRITICAL(&_state_mux);
    if (_slave_paused_for_master) {
        _slave_paused_for_master = false;
        should_resume = true;
    }
    portEXIT_CRITICAL(&_state_mux);

    if (!should_resume) {
        return;
    }

    // Only restart advertising if in peripheral/dual mode
    if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
        DEBUG("NimBLEPlatform: Resuming slave (restarting advertising)");
        startAdvertising();
    }
}

void NimBLEPlatform::enterErrorRecovery() {
    WARNING("NimBLEPlatform: Entering error recovery");

    // Reset all states atomically
    portENTER_CRITICAL(&_state_mux);
    _gap_state = GAPState::ERROR_RECOVERY;
    _master_state = MasterState::IDLE;
    _slave_state = SlaveState::IDLE;
    portEXIT_CRITICAL(&_state_mux);

    // Force stop all operations at low level first
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    // Stop high level objects
    if (_scan) {
        _scan->stop();
    }
    if (_advertising_obj) {
        _advertising_obj->stop();
    }

    _scan_stop_time = 0;
    _slave_paused_for_master = false;

    // Wait for host to sync after any reset operation
    // This is critical - the host needs time to fully reset and resync with controller
    if (!ble_hs_synced()) {
        DEBUG("NimBLEPlatform: Waiting for host sync during recovery...");
        uint32_t sync_start = millis();
        while (!ble_hs_synced() && (millis() - sync_start) < 3000) {
            // DELAY RATIONALE: Error recovery before retry
            // After BLE operation failure, NimBLE stack needs processing time before retry.
            // Without delay: immediate retry fails, rapid retries can trigger assertions.
            // 50ms = 5 NimBLE scheduler ticks, empirically chosen balance of recovery vs latency.
            delay(50);
        }
        if (ble_hs_synced()) {
            DEBUG("NimBLEPlatform: Host sync restored after " +
                  std::to_string(millis() - sync_start) + "ms");
        } else {
            ERROR("NimBLEPlatform: Host sync failed during recovery");
            // Continue anyway - may recover on next attempt
        }
    }

    // DELAY RATIONALE: Connect attempt recovery - ESP32-S3 settling time after host sync
    delay(50);

    // Re-acquire scan object to reset NimBLE internal state
    // This is necessary because NimBLE scan object can get into stuck state
    _scan = NimBLEDevice::getScan();
    if (_scan) {
        _scan->setScanCallbacks(this, false);
        _scan->setActiveScan(_config.scan_mode == ScanMode::ACTIVE);
        _scan->setInterval(_config.scan_interval_ms);
        _scan->setWindow(_config.scan_window_ms);
        _scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
        _scan->setDuplicateFilter(true);
        _scan->clearResults();
    }

    // Verify GAP is truly idle
    if (!ble_gap_disc_active() && !ble_gap_adv_active() && !ble_gap_conn_active()) {
        portENTER_CRITICAL(&_state_mux);
        _gap_state = GAPState::READY;
        portEXIT_CRITICAL(&_state_mux);
        INFO("NimBLEPlatform: Error recovery complete, GAP ready");
    } else {
        ERROR("NimBLEPlatform: GAP still busy after recovery attempt");
    }

    // Restart advertising if in peripheral/dual mode
    if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
        DEBUG("NimBLEPlatform: Restarting advertising after recovery");
        startAdvertising();
    }
}

//=============================================================================
// Central Mode - Scanning
//=============================================================================

bool NimBLEPlatform::startScan(uint16_t duration_ms) {
    if (!_scan) {
        ERROR("NimBLEPlatform: Scan not initialized");
        return false;
    }

    // Check current master state
    portENTER_CRITICAL(&_state_mux);
    MasterState current_master = _master_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_master == MasterState::SCANNING) {
        _scan_fail_count = 0;  // Reset on successful state
        return true;
    }

    // Log GAP hardware state before checking
    DEBUG("NimBLEPlatform: Pre-scan GAP state: disc=" + std::to_string(ble_gap_disc_active()) +
          " adv=" + std::to_string(ble_gap_adv_active()) +
          " conn=" + std::to_string(ble_gap_conn_active()));

    // Verify we can start scan
    if (!canStartScan()) {
        DEBUG("NimBLEPlatform: Cannot start scan - state check failed" +
              std::string(" master=") + masterStateName(current_master) +
              " gap_disc=" + std::to_string(ble_gap_disc_active()) +
              " gap_conn=" + std::to_string(ble_gap_conn_active()));
        return false;
    }

    // Pause slave (advertising) for master operation
    if (!pauseSlaveForMaster()) {
        WARNING("NimBLEPlatform: Failed to pause slave for scan");
        // Try to restart advertising in case it was stopped but flag wasn't set
        if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
            startAdvertising();
        }
        return false;
    }

    // DELAY RATIONALE: MTU negotiation settling - allow stack to stabilize before scan start
    delay(20);

    // Transition to SCAN_STARTING
    if (!transitionMasterState(MasterState::IDLE, MasterState::SCAN_STARTING)) {
        WARNING("NimBLEPlatform: Failed to transition to SCAN_STARTING");
        resumeSlave();
        return false;
    }

    // Set GAP to master priority
    portENTER_CRITICAL(&_state_mux);
    _gap_state = GAPState::MASTER_PRIORITY;
    portEXIT_CRITICAL(&_state_mux);

    uint32_t duration_sec = (duration_ms == 0) ? 0 : (duration_ms / 1000);
    if (duration_sec < 1) duration_sec = 1;  // Minimum 1 second

    // Clear results and reconfigure scan before starting
    _scan->clearResults();
    _scan->setActiveScan(_config.scan_mode == ScanMode::ACTIVE);
    _scan->setInterval(_config.scan_interval_ms);
    _scan->setWindow(_config.scan_window_ms);

    DEBUG("NimBLEPlatform: Starting scan with duration=" + std::to_string(duration_sec) + "s");

    // NimBLE 2.x: use 0 for continuous scanning (we'll stop it manually in loop())
    bool started = _scan->start(0, false);

    if (started) {
        // Transition to SCANNING
        portENTER_CRITICAL(&_state_mux);
        _master_state = MasterState::SCANNING;
        portEXIT_CRITICAL(&_state_mux);

        _scan_fail_count = 0;
        _lightweight_reset_fails = 0;
        _scan_stop_time = millis() + duration_ms;
        DEBUG("NimBLEPlatform: Scan started, will stop in " + std::to_string(duration_ms) + "ms");
        return true;
    }

    // Scan failed
    ERROR("NimBLEPlatform: Failed to start scan");

    // Reset state
    portENTER_CRITICAL(&_state_mux);
    _master_state = MasterState::IDLE;
    _gap_state = GAPState::READY;
    portEXIT_CRITICAL(&_state_mux);

    _scan_fail_count++;
    if (_scan_fail_count >= SCAN_FAIL_RECOVERY_THRESHOLD) {
        WARNING("NimBLEPlatform: Too many scan failures, entering error recovery");
        enterErrorRecovery();
    }

    resumeSlave();
    return false;
}

void NimBLEPlatform::stopScan() {
    portENTER_CRITICAL(&_state_mux);
    MasterState current_master = _master_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_master != MasterState::SCANNING && current_master != MasterState::SCAN_STARTING) {
        return;
    }

    // Transition to SCAN_STOPPING
    portENTER_CRITICAL(&_state_mux);
    _master_state = MasterState::SCAN_STOPPING;
    portEXIT_CRITICAL(&_state_mux);

    DEBUG("NimBLEPlatform: stopScan() called");

    if (_scan) {
        _scan->stop();
    }

    // Wait for scan to actually stop
    uint32_t start = millis();
    while (ble_gap_disc_active() && millis() - start < 1000) {
        // DELAY RATIONALE: Scan stop polling - check completion every NimBLE scheduler tick (~10ms)
        delay(10);
    }

    // Transition to IDLE
    portENTER_CRITICAL(&_state_mux);
    _master_state = MasterState::IDLE;
    _gap_state = GAPState::READY;
    portEXIT_CRITICAL(&_state_mux);

    _scan_stop_time = 0;
    DEBUG("NimBLEPlatform: Scan stopped");

    // Resume slave if it was paused
    resumeSlave();
}

bool NimBLEPlatform::isScanning() const {
    portENTER_CRITICAL(&_state_mux);
    bool scanning = (_master_state == MasterState::SCANNING ||
                     _master_state == MasterState::SCAN_STARTING);
    portEXIT_CRITICAL(&_state_mux);
    return scanning;
}

//=============================================================================
// Central Mode - Connections
//=============================================================================

bool NimBLEPlatform::connect(const BLEAddress& address, uint16_t timeout_ms) {
    NimBLEAddress nimAddr = toNimBLE(address);

    // Rate limit connections to avoid overwhelming the BLE stack
    // Non-blocking: return false if too soon, caller can retry later
    static unsigned long last_connect_time = 0;
    unsigned long now = millis();
    if (now - last_connect_time < 300) {  // Reduced from 500ms
        DEBUG("NimBLEPlatform: Connection rate limited, try again later");
        return false;  // Non-blocking: fail fast instead of delay
    }
    last_connect_time = millis();

    // Check if already connected
    if (isConnectedTo(address)) {
        WARNING("NimBLEPlatform: Already connected to " + address.toString());
        return false;
    }

    // Check connection limit
    if (getConnectionCount() >= _config.max_connections) {
        WARNING("NimBLEPlatform: Connection limit reached");
        return false;
    }

    // Verify we can connect using state machine
    if (!canConnect()) {
        portENTER_CRITICAL(&_state_mux);
        MasterState ms = _master_state;
        GAPState gs = _gap_state;
        portEXIT_CRITICAL(&_state_mux);
        WARNING("NimBLEPlatform: Cannot connect - state check failed" +
                std::string(" master=") + masterStateName(ms) +
                " gap=" + gapStateName(gs));
        return false;
    }

    // Stop scanning if active
    portENTER_CRITICAL(&_state_mux);
    MasterState current_master = _master_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_master == MasterState::SCANNING) {
        DEBUG("NimBLEPlatform: Stopping scan before connect");
        stopScan();
    }

    // Pause slave (advertising) for master operation
    if (!pauseSlaveForMaster()) {
        WARNING("NimBLEPlatform: Failed to pause slave for connect");
        // Try to restart advertising in case it was stopped but flag wasn't set
        if (_config.role == Role::PERIPHERAL || _config.role == Role::DUAL) {
            startAdvertising();
        }
        return false;
    }

    // Transition to CONN_STARTING
    if (!transitionMasterState(MasterState::IDLE, MasterState::CONN_STARTING)) {
        WARNING("NimBLEPlatform: Failed to transition to CONN_STARTING");
        resumeSlave();
        return false;
    }

    // Set GAP to master priority
    portENTER_CRITICAL(&_state_mux);
    _gap_state = GAPState::MASTER_PRIORITY;
    portEXIT_CRITICAL(&_state_mux);

    // DELAY RATIONALE: Service discovery settling - allow stack to finalize after advertising stop
    delay(20);

    // Verify GAP is truly idle
    if (ble_gap_disc_active() || ble_gap_adv_active()) {
        ERROR("NimBLEPlatform: GAP not idle before connect, entering error recovery");
        enterErrorRecovery();
        resumeSlave();
        return false;
    }

    // Check if there's still a pending connection
    if (ble_gap_conn_active()) {
        WARNING("NimBLEPlatform: Connection still pending in GAP, waiting...");
        uint32_t start = millis();
        while (ble_gap_conn_active() && millis() - start < 1000) {
            // DELAY RATIONALE: Service discovery polling - check completion per scheduler tick
            delay(10);
        }
        if (ble_gap_conn_active()) {
            ERROR("NimBLEPlatform: GAP connection still active after timeout");
            portENTER_CRITICAL(&_state_mux);
            _master_state = MasterState::IDLE;
            _gap_state = GAPState::READY;
            portEXIT_CRITICAL(&_state_mux);
            resumeSlave();
            return false;
        }
    }

    // Delete any existing clients for this address to ensure clean state
    NimBLEClient* existingClient = NimBLEDevice::getClientByPeerAddress(nimAddr);
    while (existingClient) {
        DEBUG("NimBLEPlatform: Deleting existing client for " + address.toString());
        if (existingClient->isConnected()) {
            existingClient->disconnect();
        }
        NimBLEDevice::deleteClient(existingClient);
        existingClient = NimBLEDevice::getClientByPeerAddress(nimAddr);
    }

    DEBUG("NimBLEPlatform: Connecting to " + address.toString() +
          " timeout=" + std::to_string(timeout_ms / 1000) + "s");

    // Transition to CONNECTING
    portENTER_CRITICAL(&_state_mux);
    _master_state = MasterState::CONNECTING;
    portEXIT_CRITICAL(&_state_mux);

    // Use native NimBLE connection
    bool connected = connectNative(address, timeout_ms);

    if (!connected) {
        ERROR("NimBLEPlatform: Native connection failed to " + address.toString());
        portENTER_CRITICAL(&_state_mux);
        _master_state = MasterState::IDLE;
        _gap_state = GAPState::READY;
        portEXIT_CRITICAL(&_state_mux);
        resumeSlave();
        return false;
    }

    // Connection succeeded - transition states
    portENTER_CRITICAL(&_state_mux);
    _master_state = MasterState::IDLE;
    _gap_state = GAPState::READY;
    portEXIT_CRITICAL(&_state_mux);

    // Remove from discovered devices cache
    std::string addrKey = nimAddr.toString().c_str();
    if (xSemaphoreTake(_conn_mutex, pdMS_TO_TICKS(100))) {
        auto cachedIt = _discovered_devices.find(addrKey);
        if (cachedIt != _discovered_devices.end()) {
            // Also remove from order tracking
            auto orderIt = std::find(_discovered_order.begin(),
                                      _discovered_order.end(), addrKey);
            if (orderIt != _discovered_order.end()) {
                _discovered_order.erase(orderIt);
            }
            _discovered_devices.erase(cachedIt);
        }
        xSemaphoreGive(_conn_mutex);
    } else {
        // CONC-M5: Log timeout failures
        WARNING("NimBLEPlatform: conn_mutex timeout (100ms) during cache update");
    }

    DEBUG("NimBLEPlatform: Connection established successfully");

    // Resume slave operations
    resumeSlave();

    return true;
}

//=============================================================================
// Native NimBLE Connection (bypasses NimBLE-Arduino wrapper)
//=============================================================================

int NimBLEPlatform::nativeGapEventHandler(struct ble_gap_event* event, void* arg) {
    NimBLEPlatform* platform = static_cast<NimBLEPlatform*>(arg);

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            DEBUG("NimBLEPlatform::nativeGapEventHandler: BLE_GAP_EVENT_CONNECT status=" +
                  std::to_string(event->connect.status) +
                  " handle=" + std::to_string(event->connect.conn_handle));

            platform->_native_connect_result = event->connect.status;
            if (event->connect.status == 0) {
                platform->_native_connect_success = true;
                platform->_native_connect_handle = event->connect.conn_handle;
                // Reset failure counters on successful connection
                platform->_conn_establish_fail_count = 0;
            } else {
                platform->_native_connect_success = false;
            }
            platform->_native_connect_pending = false;
            break;

        case BLE_GAP_EVENT_DISCONNECT: {
            uint16_t disc_handle = event->disconnect.conn.conn_handle;
            int disc_reason = event->disconnect.reason;

            DEBUG("NimBLEPlatform::nativeGapEventHandler: BLE_GAP_EVENT_DISCONNECT reason=" +
                  std::to_string(disc_reason) +
                  " handle=" + std::to_string(disc_handle));

            // If we were still waiting for connection, this is a failure
            if (platform->_native_connect_pending) {
                platform->_native_connect_result = disc_reason;
                platform->_native_connect_success = false;
                platform->_native_connect_pending = false;

                // Track connection establishment failures (574 = BLE_ERR_CONN_ESTABLISHMENT)
                if (disc_reason == 574) {
                    platform->_conn_establish_fail_count++;
                    WARNING("NimBLEPlatform: Connection establishment failed (574), count=" +
                            std::to_string(platform->_conn_establish_fail_count));

                    // If too many consecutive failures, trigger recovery
                    if (platform->_conn_establish_fail_count >= CONN_ESTABLISH_FAIL_THRESHOLD) {
                        WARNING("NimBLEPlatform: Too many connection establishment failures, entering recovery");
                        platform->_conn_establish_fail_count = 0;
                        platform->enterErrorRecovery();
                    }
                }
            }

            // Clean up established connections (handles MAC rotation, out of range, etc.)
            auto conn_it = platform->_connections.find(disc_handle);
            if (conn_it != platform->_connections.end()) {
                ConnectionHandle conn = conn_it->second;
                platform->_connections.erase(conn_it);

                INFO("NimBLEPlatform: Native connection lost to " + conn.peer_address.toString() +
                     " reason=" + std::to_string(disc_reason));

                // Clean up client object
                auto client_it = platform->_clients.find(disc_handle);
                if (client_it != platform->_clients.end()) {
                    if (client_it->second) {
                        NimBLEDevice::deleteClient(client_it->second);
                    }
                    platform->_clients.erase(client_it);
                }

                // Clear operation queue for this connection
                platform->clearForConnection(disc_handle);

                // Notify higher layers
                if (platform->_on_disconnected) {
                    platform->_on_disconnected(conn, static_cast<uint8_t>(disc_reason));
                }

                // Restart advertising if in peripheral/dual mode and not currently advertising
                if ((platform->_config.role == Role::PERIPHERAL || platform->_config.role == Role::DUAL) &&
                    !platform->isAdvertising()) {
                    platform->startAdvertising();
                }
            }
            break;
        }

        default:
            DEBUG("NimBLEPlatform::nativeGapEventHandler: event type=" + std::to_string(event->type));
            break;
    }

    return 0;
}

bool NimBLEPlatform::connectNative(const BLEAddress& address, uint16_t timeout_ms) {
    DEBUG("NimBLEPlatform::connectNative: Starting native connection to " + address.toString());

    // Verify host-controller sync before connection attempt
    if (!ble_hs_synced()) {
        WARNING("NimBLEPlatform::connectNative: Host not synced, waiting for sync");
        uint32_t sync_start = millis();
        while (!ble_hs_synced() && (millis() - sync_start) < 1000) {
            // DELAY RATIONALE: Notification send retry - respect scheduler tick for queue processing
            delay(10);
        }
        if (!ble_hs_synced()) {
            ERROR("NimBLEPlatform::connectNative: Host sync timeout, entering error recovery");
            enterErrorRecovery();
            return false;
        }
        DEBUG("NimBLEPlatform::connectNative: Host sync restored");
    }

    // Validate address type
    // BLE_ADDR_PUBLIC = 0, BLE_ADDR_RANDOM = 1, BLE_ADDR_PUBLIC_ID = 2, BLE_ADDR_RANDOM_ID = 3
    if (address.type > 3) {
        ERROR("NimBLEPlatform::connectNative: Invalid address type " + std::to_string(address.type));
        return false;
    }

    DEBUG("NimBLEPlatform::connectNative: Connecting to " + address.toString() +
          " type=" + std::to_string(address.type) +
          (address.type == 0 ? " (public)" : address.type == 1 ? " (random)" : " (other)"));

    // Build the peer address structure
    ble_addr_t peer_addr;
    peer_addr.type = address.type;
    // NimBLE stores addresses in little-endian: val[0]=LSB, val[5]=MSB
    // Our BLEAddress stores in big-endian display order: addr[0]=MSB, addr[5]=LSB
    for (int i = 0; i < 6; i++) {
        peer_addr.val[i] = address.addr[5 - i];
    }

    DEBUG("NimBLEPlatform::connectNative: peer_addr type=" + std::to_string(peer_addr.type) +
          " val=" + std::to_string(peer_addr.val[5]) + ":" +
          std::to_string(peer_addr.val[4]) + ":" +
          std::to_string(peer_addr.val[3]) + ":" +
          std::to_string(peer_addr.val[2]) + ":" +
          std::to_string(peer_addr.val[1]) + ":" +
          std::to_string(peer_addr.val[0]));

    // Connection parameters - use reasonable defaults
    struct ble_gap_conn_params conn_params;
    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_itvl = 16;           // 10ms in 0.625ms units
    conn_params.scan_window = 16;         // 10ms in 0.625ms units
    conn_params.itvl_min = 24;            // 30ms in 1.25ms units
    conn_params.itvl_max = 40;            // 50ms in 1.25ms units
    conn_params.latency = 0;
    conn_params.supervision_timeout = 256; // 2560ms in 10ms units
    conn_params.min_ce_len = 0;
    conn_params.max_ce_len = 0;

    // Reset connection state
    _native_connect_pending = true;
    _native_connect_success = false;
    _native_connect_result = 0;
    _native_connect_handle = 0;
    _native_connect_address = address;

    // Use RANDOM own address type (same as what NimBLEDevice is configured with)
    uint8_t own_addr_type = BLE_OWN_ADDR_RANDOM;

    DEBUG("NimBLEPlatform::connectNative: Trying with own_addr_type=" + std::to_string(own_addr_type));

    int rc = ble_gap_connect(own_addr_type,
                              &peer_addr,
                              timeout_ms,
                              &conn_params,
                              nativeGapEventHandler,
                              this);

    DEBUG("NimBLEPlatform::connectNative: ble_gap_connect returned " + std::to_string(rc));

    if (rc != 0) {
        ERROR("NimBLEPlatform::connectNative: ble_gap_connect failed with rc=" + std::to_string(rc));
        _native_connect_pending = false;

        // Special handling for host desync (rc=22 / BLE_HS_ENOTSYNCED)
        if (rc == 22) {  // BLE_HS_ENOTSYNCED
            ERROR("NimBLEPlatform::connectNative: Host desync detected (rc=22), scheduling host reset");
            // Schedule a host reset to resynchronize with controller
            ble_hs_sched_reset(0);
            // DELAY RATIONALE: Soft reset processing - allow stack to process host reset
            delay(50);
            enterErrorRecovery();
        }

        return false;
    }

    // Wait for connection to complete
    unsigned long start = millis();
    while (_native_connect_pending && (millis() - start) < timeout_ms) {
        // DELAY RATIONALE: Reset wait polling - check connection completion per scheduler tick
        delay(10);
    }

    if (_native_connect_pending) {
        // Timeout - try to cancel but only if connection is still pending at GAP level
        WARNING("NimBLEPlatform::connectNative: Connection timed out, cancelling");
        // Check if we're actually connecting before cancelling
        if (ble_gap_conn_active()) {
            int rc = ble_gap_conn_cancel();
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                DEBUG("NimBLEPlatform::connectNative: ble_gap_conn_cancel returned " + std::to_string(rc));
            }
        }
        // DELAY RATIONALE: Stack stabilization after connection cancel
        delay(10);
        _native_connect_pending = false;
        return false;
    }

    if (!_native_connect_success) {
        ERROR("NimBLEPlatform::connectNative: Connection failed with result=" +
              std::to_string(_native_connect_result));
        return false;
    }

    // Copy volatile to local variable
    uint16_t conn_handle = _native_connect_handle;

    INFO("NimBLEPlatform::connectNative: Connection succeeded! handle=" +
         std::to_string(conn_handle));

    // Now create an NimBLEClient for this connection to use for GATT operations
    // The connection already exists, so we just need to wrap it
    NimBLEClient* client = NimBLEDevice::getClientByHandle(conn_handle);
    if (!client) {
        // Create a new client and associate it with this connection
        NimBLEAddress nimAddr(peer_addr);
        client = NimBLEDevice::createClient(nimAddr);
        if (client) {
            client->setClientCallbacks(this, false);
        }
    }

    if (client) {
        // Track the connection
        ConnectionHandle conn;
        conn.handle = conn_handle;
        conn.peer_address = address;
        conn.local_role = Role::CENTRAL;
        conn.state = ConnectionState::CONNECTED;
        conn.mtu = client->getMTU();

        _connections[conn_handle] = conn;
        _clients[conn_handle] = client;

        if (_on_connected) {
            _on_connected(conn);
        }
    }

    return true;
}

bool NimBLEPlatform::disconnect(uint16_t conn_handle) {
    auto conn_it = _connections.find(conn_handle);
    if (conn_it == _connections.end()) {
        return false;
    }

    ConnectionHandle& conn = conn_it->second;

    if (conn.local_role == Role::CENTRAL) {
        // We are central - disconnect client
        auto client_it = _clients.find(conn_handle);
        if (client_it != _clients.end() && client_it->second) {
            client_it->second->disconnect();
            return true;
        }
    } else {
        // We are peripheral - disconnect via server
        if (_server) {
            _server->disconnect(conn_handle);
            return true;
        }
    }

    return false;
}

void NimBLEPlatform::disconnectAll() {
    // Disconnect all clients (central mode)
    for (auto& kv : _clients) {
        if (kv.second && kv.second->isConnected()) {
            kv.second->disconnect();
        }
    }

    // Disconnect all server connections (peripheral mode)
    if (_server) {
        std::vector<uint16_t> handles;
        for (const auto& kv : _connections) {
            if (kv.second.local_role == Role::PERIPHERAL) {
                handles.push_back(kv.first);
            }
        }
        for (uint16_t handle : handles) {
            _server->disconnect(handle);
        }
    }
}

bool NimBLEPlatform::requestMTU(uint16_t conn_handle, uint16_t mtu) {
    auto client_it = _clients.find(conn_handle);
    if (client_it == _clients.end() || !client_it->second) {
        return false;
    }

    // NimBLE handles MTU exchange automatically, but we can try to update
    // The MTU change callback will be invoked
    return true;
}

bool NimBLEPlatform::discoverServices(uint16_t conn_handle) {
    auto client_it = _clients.find(conn_handle);
    if (client_it == _clients.end() || !client_it->second) {
        return false;
    }

    NimBLEClient* client = client_it->second;

    // Get our service
    NimBLERemoteService* service = client->getService(UUID::SERVICE);
    if (!service) {
        ERROR("NimBLEPlatform: Service not found");
        if (_on_services_discovered) {
            ConnectionHandle conn = getConnection(conn_handle);
            _on_services_discovered(conn, false);
        }
        return false;
    }

    // Get characteristics
    NimBLERemoteCharacteristic* rxChar = service->getCharacteristic(UUID::RX_CHAR);
    NimBLERemoteCharacteristic* txChar = service->getCharacteristic(UUID::TX_CHAR);
    NimBLERemoteCharacteristic* idChar = service->getCharacteristic(UUID::IDENTITY_CHAR);

    if (!rxChar || !txChar) {
        ERROR("NimBLEPlatform: Required characteristics not found");
        if (_on_services_discovered) {
            ConnectionHandle conn = getConnection(conn_handle);
            _on_services_discovered(conn, false);
        }
        return false;
    }

    // Update connection with characteristic handles
    auto conn_it = _connections.find(conn_handle);
    if (conn_it != _connections.end()) {
        conn_it->second.rx_char_handle = rxChar->getHandle();
        conn_it->second.tx_char_handle = txChar->getHandle();
        if (idChar) {
            conn_it->second.identity_handle = idChar->getHandle();
        }
        conn_it->second.state = ConnectionState::READY;
    }

    DEBUG("NimBLEPlatform: Services discovered for " + std::to_string(conn_handle));

    if (_on_services_discovered) {
        ConnectionHandle conn = getConnection(conn_handle);
        _on_services_discovered(conn, true);
    }

    return true;
}

//=============================================================================
// Peripheral Mode
//=============================================================================

bool NimBLEPlatform::startAdvertising() {
    if (!_advertising_obj) {
        if (!setupAdvertising()) {
            return false;
        }
    }

    // Check current slave state
    portENTER_CRITICAL(&_state_mux);
    SlaveState current_slave = _slave_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_slave == SlaveState::ADVERTISING) {
        return true;
    }

    // Check if we can start advertising
    if (!canStartAdvertising()) {
        DEBUG("NimBLEPlatform: Cannot start advertising - state check failed" +
              std::string(" slave=") + slaveStateName(current_slave) +
              " gap_adv=" + std::to_string(ble_gap_adv_active()));
        return false;
    }

    // Transition to ADV_STARTING
    if (!transitionSlaveState(SlaveState::IDLE, SlaveState::ADV_STARTING)) {
        WARNING("NimBLEPlatform: Failed to transition to ADV_STARTING");
        return false;
    }

    if (_advertising_obj->start()) {
        // Transition to ADVERTISING
        portENTER_CRITICAL(&_state_mux);
        _slave_state = SlaveState::ADVERTISING;
        portEXIT_CRITICAL(&_state_mux);

        DEBUG("NimBLEPlatform: Advertising started");
        return true;
    }

    // Failed to start
    portENTER_CRITICAL(&_state_mux);
    _slave_state = SlaveState::IDLE;
    portEXIT_CRITICAL(&_state_mux);

    ERROR("NimBLEPlatform: Failed to start advertising");
    return false;
}

void NimBLEPlatform::stopAdvertising() {
    portENTER_CRITICAL(&_state_mux);
    SlaveState current_slave = _slave_state;
    portEXIT_CRITICAL(&_state_mux);

    if (current_slave != SlaveState::ADVERTISING && current_slave != SlaveState::ADV_STARTING) {
        return;
    }

    // Transition to ADV_STOPPING
    portENTER_CRITICAL(&_state_mux);
    _slave_state = SlaveState::ADV_STOPPING;
    portEXIT_CRITICAL(&_state_mux);

    DEBUG("NimBLEPlatform: stopAdvertising() called");

    if (_advertising_obj) {
        _advertising_obj->stop();
    }

    // Also stop at low level
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    // Wait for advertising to actually stop
    uint32_t start = millis();
    while (ble_gap_adv_active() && millis() - start < 1000) {
        // DELAY RATIONALE: Loop iteration throttle - prevent tight loop CPU consumption
        delay(10);
    }

    // Transition to IDLE
    portENTER_CRITICAL(&_state_mux);
    _slave_state = SlaveState::IDLE;
    portEXIT_CRITICAL(&_state_mux);

    DEBUG("NimBLEPlatform: Advertising stopped");
}

bool NimBLEPlatform::isAdvertising() const {
    portENTER_CRITICAL(&_state_mux);
    bool advertising = (_slave_state == SlaveState::ADVERTISING ||
                        _slave_state == SlaveState::ADV_STARTING);
    portEXIT_CRITICAL(&_state_mux);
    return advertising;
}

bool NimBLEPlatform::setAdvertisingData(const Bytes& data) {
    // Custom advertising data not directly supported by high-level API
    // Use the service UUID instead
    return true;
}

void NimBLEPlatform::setIdentityData(const Bytes& identity) {
    _identity_data = identity;

    if (_identity_char && identity.size() > 0) {
        _identity_char->setValue(identity.data(), identity.size());
        DEBUG("NimBLEPlatform: Identity data set");
    }

    // Update device name to include identity prefix (Protocol v2.2)
    // Format: "RNS-" + first 3 bytes of identity as hex (6 chars)
    // This allows peers to recognize us across MAC rotations
    if (identity.size() >= 3 && _advertising_obj) {
        char name[11];  // "RNS-" (4) + 6 hex chars + null
        snprintf(name, sizeof(name), "RNS-%02x%02x%02x",
                 identity.data()[0], identity.data()[1], identity.data()[2]);

        _advertising_obj->setName(name);
        DEBUG("NimBLEPlatform: Updated advertised name to " + std::string(name));

        // Restart advertising if currently active to apply new name
        if (isAdvertising()) {
            stopAdvertising();
            startAdvertising();
        }
    }
}

//=============================================================================
// GATT Operations
//=============================================================================

bool NimBLEPlatform::write(uint16_t conn_handle, const Bytes& data, bool response) {
    auto conn_it = _connections.find(conn_handle);
    if (conn_it == _connections.end()) {
        return false;
    }

    ConnectionHandle& conn = conn_it->second;

    if (conn.local_role == Role::CENTRAL) {
        // We are central - write to peripheral's RX characteristic
        auto client_it = _clients.find(conn_handle);
        if (client_it == _clients.end() || !client_it->second) {
            return false;
        }

        NimBLEClient* client = client_it->second;
        NimBLERemoteService* service = client->getService(UUID::SERVICE);
        if (!service) return false;

        NimBLERemoteCharacteristic* rxChar = service->getCharacteristic(UUID::RX_CHAR);
        if (!rxChar) return false;

        // CONC-H4: Track active write for graceful shutdown
        beginWriteOperation();
        bool result = rxChar->writeValue(data.data(), data.size(), response);
        endWriteOperation();
        return result;
    } else {
        // We are peripheral - this shouldn't be used, use notify instead
        WARNING("NimBLEPlatform: write() called in peripheral mode, use notify()");
        return false;
    }
}

bool NimBLEPlatform::read(uint16_t conn_handle, uint16_t char_handle,
                          std::function<void(OperationResult, const Bytes&)> callback) {
    auto client_it = _clients.find(conn_handle);
    if (client_it == _clients.end() || !client_it->second) {
        if (callback) callback(OperationResult::NOT_FOUND, Bytes());
        return false;
    }

    NimBLEClient* client = client_it->second;
    NimBLERemoteService* service = client->getService(UUID::SERVICE);
    if (!service) {
        if (callback) callback(OperationResult::NOT_FOUND, Bytes());
        return false;
    }

    // Find characteristic by handle
    NimBLERemoteCharacteristic* chr = nullptr;
    if (char_handle == _connections[conn_handle].identity_handle) {
        chr = service->getCharacteristic(UUID::IDENTITY_CHAR);
    }

    if (!chr) {
        if (callback) callback(OperationResult::NOT_FOUND, Bytes());
        return false;
    }

    NimBLEAttValue value = chr->readValue();
    if (callback) {
        Bytes result(value.data(), value.size());
        callback(OperationResult::SUCCESS, result);
    }

    return true;
}

bool NimBLEPlatform::enableNotifications(uint16_t conn_handle, bool enable) {
    auto client_it = _clients.find(conn_handle);
    if (client_it == _clients.end() || !client_it->second) {
        return false;
    }

    NimBLEClient* client = client_it->second;
    NimBLERemoteService* service = client->getService(UUID::SERVICE);
    if (!service) return false;

    NimBLERemoteCharacteristic* txChar = service->getCharacteristic(UUID::TX_CHAR);
    if (!txChar) return false;

    if (enable) {
        // Subscribe to notifications
        auto notifyCb = [this, conn_handle](NimBLERemoteCharacteristic* pChar,
                                             uint8_t* pData, size_t length, bool isNotify) {
            if (_on_data_received) {
                ConnectionHandle conn = getConnection(conn_handle);
                Bytes data(pData, length);
                _on_data_received(conn, data);
            }
        };

        return txChar->subscribe(true, notifyCb);
    } else {
        return txChar->unsubscribe();
    }
}

bool NimBLEPlatform::notify(uint16_t conn_handle, const Bytes& data) {
    if (!_tx_char) {
        return false;
    }

    _tx_char->setValue(data.data(), data.size());
    return _tx_char->notify(true);
}

bool NimBLEPlatform::notifyAll(const Bytes& data) {
    if (!_tx_char) {
        return false;
    }

    _tx_char->setValue(data.data(), data.size());
    return _tx_char->notify(true);  // Notifies all subscribed clients
}

//=============================================================================
// Connection Management
//=============================================================================

std::vector<ConnectionHandle> NimBLEPlatform::getConnections() const {
    std::vector<ConnectionHandle> result;
    for (const auto& kv : _connections) {
        result.push_back(kv.second);
    }
    return result;
}

ConnectionHandle NimBLEPlatform::getConnection(uint16_t handle) const {
    auto it = _connections.find(handle);
    if (it != _connections.end()) {
        return it->second;
    }
    return ConnectionHandle();
}

size_t NimBLEPlatform::getConnectionCount() const {
    return _connections.size();
}

bool NimBLEPlatform::isConnectedTo(const BLEAddress& address) const {
    for (const auto& kv : _connections) {
        if (kv.second.peer_address == address) {
            return true;
        }
    }
    return false;
}

bool NimBLEPlatform::isDeviceConnected(const std::string& addrKey) const {
    for (const auto& [handle, conn] : _connections) {
        if (conn.peer_address.toString() == addrKey) {
            return true;
        }
    }
    return false;
}

//=============================================================================
// Callback Registration
//=============================================================================

void NimBLEPlatform::setOnScanResult(Callbacks::OnScanResult callback) {
    _on_scan_result = callback;
}

void NimBLEPlatform::setOnScanComplete(Callbacks::OnScanComplete callback) {
    _on_scan_complete = callback;
}

void NimBLEPlatform::setOnConnected(Callbacks::OnConnected callback) {
    _on_connected = callback;
}

void NimBLEPlatform::setOnDisconnected(Callbacks::OnDisconnected callback) {
    _on_disconnected = callback;
}

void NimBLEPlatform::setOnMTUChanged(Callbacks::OnMTUChanged callback) {
    _on_mtu_changed = callback;
}

void NimBLEPlatform::setOnServicesDiscovered(Callbacks::OnServicesDiscovered callback) {
    _on_services_discovered = callback;
}

void NimBLEPlatform::setOnDataReceived(Callbacks::OnDataReceived callback) {
    _on_data_received = callback;
}

void NimBLEPlatform::setOnNotifyEnabled(Callbacks::OnNotifyEnabled callback) {
    _on_notify_enabled = callback;
}

void NimBLEPlatform::setOnCentralConnected(Callbacks::OnCentralConnected callback) {
    _on_central_connected = callback;
}

void NimBLEPlatform::setOnCentralDisconnected(Callbacks::OnCentralDisconnected callback) {
    _on_central_disconnected = callback;
}

void NimBLEPlatform::setOnWriteReceived(Callbacks::OnWriteReceived callback) {
    _on_write_received = callback;
}

void NimBLEPlatform::setOnReadRequested(Callbacks::OnReadRequested callback) {
    _on_read_requested = callback;
}

BLEAddress NimBLEPlatform::getLocalAddress() const {
    return fromNimBLE(NimBLEDevice::getAddress());
}

//=============================================================================
// NimBLE Server Callbacks (Peripheral mode)
//=============================================================================

void NimBLEPlatform::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    uint16_t conn_handle = connInfo.getConnHandle();

    ConnectionHandle conn;
    conn.handle = conn_handle;
    conn.peer_address = fromNimBLE(connInfo.getAddress());
    conn.local_role = Role::PERIPHERAL;  // We are peripheral, they are central
    conn.state = ConnectionState::CONNECTED;
    conn.mtu = MTU::MINIMUM;

    _connections[conn_handle] = conn;

    DEBUG("NimBLEPlatform: Central connected: " + conn.peer_address.toString());

    if (_on_central_connected) {
        _on_central_connected(conn);
    }

    // Continue advertising to accept more connections
    if (_config.role == Role::DUAL && getConnectionCount() < _config.max_connections) {
        startAdvertising();
    }
}

void NimBLEPlatform::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    uint16_t conn_handle = connInfo.getConnHandle();

    auto it = _connections.find(conn_handle);
    if (it != _connections.end()) {
        ConnectionHandle conn = it->second;
        _connections.erase(it);

        DEBUG("NimBLEPlatform: Central disconnected: " + conn.peer_address.toString() +
              " reason: " + std::to_string(reason));

        if (_on_central_disconnected) {
            _on_central_disconnected(conn);
        }
    }

    // Clear operation queue for this connection
    BLEOperationQueue::clearForConnection(conn_handle);
}

void NimBLEPlatform::onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) {
    uint16_t conn_handle = connInfo.getConnHandle();
    updateConnectionMTU(conn_handle, MTU);

    DEBUG("NimBLEPlatform: MTU changed to " + std::to_string(MTU) +
          " for connection " + std::to_string(conn_handle));

    if (_on_mtu_changed) {
        ConnectionHandle conn = getConnection(conn_handle);
        _on_mtu_changed(conn, MTU);
    }
}

//=============================================================================
// NimBLE Characteristic Callbacks
//=============================================================================

void NimBLEPlatform::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    uint16_t conn_handle = connInfo.getConnHandle();

    NimBLEAttValue value = pCharacteristic->getValue();
    Bytes data(value.data(), value.size());

    DEBUG("NimBLEPlatform::onWrite: Received " + std::to_string(data.size()) + " bytes from conn " + std::to_string(conn_handle));

    if (_on_write_received) {
        DEBUG("NimBLEPlatform::onWrite: Getting connection handle");
        ConnectionHandle conn = getConnection(conn_handle);
        DEBUG("NimBLEPlatform::onWrite: Calling callback, peer=" + conn.peer_address.toString());
        _on_write_received(conn, data);
        DEBUG("NimBLEPlatform::onWrite: Callback returned");
    } else {
        DEBUG("NimBLEPlatform::onWrite: No callback registered");
    }
}

void NimBLEPlatform::onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Identity characteristic read - return stored identity
    if (pCharacteristic == _identity_char && _identity_data.size() > 0) {
        pCharacteristic->setValue(_identity_data.data(), _identity_data.size());
    }
}

void NimBLEPlatform::onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo,
                                  uint16_t subValue) {
    uint16_t conn_handle = connInfo.getConnHandle();
    bool enabled = (subValue > 0);

    DEBUG("NimBLEPlatform: Notifications " + std::string(enabled ? "enabled" : "disabled") +
          " for connection " + std::to_string(conn_handle));

    if (_on_notify_enabled) {
        ConnectionHandle conn = getConnection(conn_handle);
        _on_notify_enabled(conn, enabled);
    }
}

//=============================================================================
// NimBLE Client Callbacks (Central mode)
//=============================================================================

void NimBLEPlatform::onConnect(NimBLEClient* pClient) {
    uint16_t conn_handle = pClient->getConnHandle();
    BLEAddress peer_addr = fromNimBLE(pClient->getPeerAddress());

    ConnectionHandle conn;
    conn.handle = conn_handle;
    conn.peer_address = peer_addr;
    conn.local_role = Role::CENTRAL;  // We are central
    conn.state = ConnectionState::CONNECTED;
    conn.mtu = pClient->getMTU();

    _connections[conn_handle] = conn;
    _clients[conn_handle] = pClient;

    DEBUG("NimBLEPlatform: Connected to peripheral: " + peer_addr.toString() +
          " handle=" + std::to_string(conn_handle) + " mtu=" + std::to_string(conn.mtu));

    // Signal async connect completion
    _async_connect_pending = false;
    _async_connect_failed = false;

    if (_on_connected) {
        _on_connected(conn);
    }
}

void NimBLEPlatform::onConnectFail(NimBLEClient* pClient, int reason) {
    BLEAddress peer_addr = fromNimBLE(pClient->getPeerAddress());
    ERROR("NimBLEPlatform: onConnectFail to " + peer_addr.toString() +
          " reason=" + std::to_string(reason));

    // Signal async connect failure
    _async_connect_pending = false;
    _async_connect_failed = true;
    _async_connect_error = reason;
}

void NimBLEPlatform::onDisconnect(NimBLEClient* pClient, int reason) {
    uint16_t conn_handle = pClient->getConnHandle();

    auto it = _connections.find(conn_handle);
    if (it != _connections.end()) {
        ConnectionHandle conn = it->second;
        _connections.erase(it);

        DEBUG("NimBLEPlatform: Disconnected from peripheral: " + conn.peer_address.toString() +
              " reason: " + std::to_string(reason));

        if (_on_disconnected) {
            _on_disconnected(conn, static_cast<uint8_t>(reason));
        }
    }

    // Remove client
    _clients.erase(conn_handle);
    NimBLEDevice::deleteClient(pClient);

    // Clear operation queue
    BLEOperationQueue::clearForConnection(conn_handle);
}

//=============================================================================
// NimBLE Scan Callbacks
//=============================================================================

void NimBLEPlatform::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    // Check if device has our service UUID
    bool hasService = advertisedDevice->isAdvertisingService(BLEUUID(UUID::SERVICE));

    // Debug: log RNS device scan results with address type
    if (hasService) {
        DEBUG("NimBLEPlatform: RNS device found: " + std::string(advertisedDevice->getAddress().toString().c_str()) +
              " type=" + std::to_string(advertisedDevice->getAddress().getType()) +
              " RSSI=" + std::to_string(advertisedDevice->getRSSI()) +
              " name=" + advertisedDevice->getName());

        // Cache the full device info for later connection
        // Using string key since NimBLEAdvertisedDevice stores all connection metadata
        std::string addrKey = advertisedDevice->getAddress().toString().c_str();

        // Bounded cache with connected device protection (CONC-M6)
        static constexpr size_t MAX_DISCOVERED_DEVICES = 16;
        while (_discovered_devices.size() >= MAX_DISCOVERED_DEVICES) {
            bool evicted = false;
            // Find oldest non-connected device using insertion order
            for (auto it = _discovered_order.begin(); it != _discovered_order.end(); ++it) {
                if (!isDeviceConnected(*it)) {
                    _discovered_devices.erase(*it);
                    _discovered_order.erase(it);
                    evicted = true;
                    break;
                }
            }
            if (!evicted) {
                // All cached devices are connected - don't cache new one
                WARNING("NimBLEPlatform: Cannot cache device - all slots hold connected devices");
                return;
            }
        }

        // Track insertion order for new devices
        auto existing = _discovered_devices.find(addrKey);
        if (existing == _discovered_devices.end()) {
            // New device - add to order tracking
            _discovered_order.push_back(addrKey);
        }
        _discovered_devices[addrKey] = *advertisedDevice;
        TRACE("NimBLEPlatform: Cached device for connection: " + addrKey +
              " (cache size: " + std::to_string(_discovered_devices.size()) + ")");
    }

    if (hasService && _on_scan_result) {
        ScanResult result;
        result.address = fromNimBLE(advertisedDevice->getAddress());
        result.name = advertisedDevice->getName();
        result.rssi = advertisedDevice->getRSSI();
        result.connectable = advertisedDevice->isConnectable();
        result.has_reticulum_service = true;

        // Extract identity prefix from device name (Protocol v2.2)
        // Format: "RNS-xxxxxx" where xxxxxx is 6 hex chars (3 bytes of identity)
        std::string name = advertisedDevice->getName();
        if (name.size() >= 10 && name.substr(0, 4) == "RNS-") {
            std::string hexPart = name.substr(4, 6);
            if (hexPart.size() == 6) {
                // Parse hex to bytes
                uint8_t prefix[3];
                bool valid = true;
                for (int i = 0; i < 3 && valid; i++) {
                    unsigned int val;
                    if (sscanf(hexPart.c_str() + i*2, "%02x", &val) == 1) {
                        prefix[i] = static_cast<uint8_t>(val);
                    } else {
                        valid = false;
                    }
                }
                if (valid) {
                    result.identity_prefix = Bytes(prefix, 3);
                    DEBUG("NimBLEPlatform: Extracted identity prefix from name: " + hexPart);
                }
            }
        }

        _on_scan_result(result);
    }
}

void NimBLEPlatform::onScanEnd(const NimBLEScanResults& results, int reason) {
    // Check if we were actively scanning
    portENTER_CRITICAL(&_state_mux);
    MasterState prev_master = _master_state;
    bool was_scanning = (prev_master == MasterState::SCANNING ||
                         prev_master == MasterState::SCAN_STARTING ||
                         prev_master == MasterState::SCAN_STOPPING);
    // Transition to IDLE
    if (was_scanning) {
        _master_state = MasterState::IDLE;
        _gap_state = GAPState::READY;
    }
    portEXIT_CRITICAL(&_state_mux);

    _scan_stop_time = 0;

    DEBUG("NimBLEPlatform: onScanEnd callback, reason=" + std::to_string(reason) +
          " found=" + std::to_string(results.getCount()) + " devices" +
          " was_scanning=" + std::string(was_scanning ? "yes" : "no"));

    // Only process if we were actively scanning (not a spurious callback)
    if (!was_scanning) {
        return;
    }

    // Resume slave if it was paused for this scan
    resumeSlave();

    if (_on_scan_complete) {
        _on_scan_complete();
    }
}

//=============================================================================
// BLEOperationQueue Implementation
//=============================================================================

bool NimBLEPlatform::executeOperation(const GATTOperation& op) {
    // Most operations are executed directly in NimBLE
    // This is a placeholder for more complex queued operations
    return true;
}

//=============================================================================
// Private Methods
//=============================================================================

bool NimBLEPlatform::setupServer() {
    _server = NimBLEDevice::createServer();
    if (!_server) {
        ERROR("NimBLEPlatform: Failed to create server");
        return false;
    }

    _server->setCallbacks(this);

    // Create Reticulum service
    _service = _server->createService(UUID::SERVICE);
    if (!_service) {
        ERROR("NimBLEPlatform: Failed to create service");
        return false;
    }

    // Create RX characteristic (write from central)
    _rx_char = _service->createCharacteristic(
        UUID::RX_CHAR,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _rx_char->setValue((uint8_t*)"\x00", 1);  // Initialize to 0x00
    _rx_char->setCallbacks(this);

    // Create TX characteristic (notify/indicate to central)
    // Note: indicate property required for compatibility with ble-reticulum/Columba
    _tx_char = _service->createCharacteristic(
        UUID::TX_CHAR,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::INDICATE
    );
    _tx_char->setValue((uint8_t*)"\x00", 1);  // Initialize to 0x00 (matches Columba)
    _tx_char->setCallbacks(this);

    // Create Identity characteristic (read only)
    _identity_char = _service->createCharacteristic(
        UUID::IDENTITY_CHAR,
        NIMBLE_PROPERTY::READ
    );
    _identity_char->setCallbacks(this);

    // Start service
    _service->start();

    return setupAdvertising();
}

bool NimBLEPlatform::setupAdvertising() {
    _advertising_obj = NimBLEDevice::getAdvertising();
    if (!_advertising_obj) {
        ERROR("NimBLEPlatform: Failed to get advertising");
        return false;
    }

    // CRITICAL: Reset advertising state before configuring
    // Without this, the advertising data may not be properly updated on ESP32-S3
    _advertising_obj->reset();

    _advertising_obj->setMinInterval(_config.adv_interval_min_ms * 1000 / 625);  // Convert to 0.625ms units
    _advertising_obj->setMaxInterval(_config.adv_interval_max_ms * 1000 / 625);

    // NimBLE 2.x: Use addServiceUUID to include service in advertising packet
    // The name goes in scan response automatically when enableScanResponse is used
    _advertising_obj->addServiceUUID(NimBLEUUID(UUID::SERVICE));
    _advertising_obj->setName(_config.device_name);

    DEBUG("NimBLEPlatform: Advertising configured with service UUID: " + std::string(UUID::SERVICE));

    return true;
}

bool NimBLEPlatform::setupScan() {
    _scan = NimBLEDevice::getScan();
    if (!_scan) {
        ERROR("NimBLEPlatform: Failed to get scan");
        return false;
    }

    _scan->setScanCallbacks(this, false);
    _scan->setActiveScan(_config.scan_mode == ScanMode::ACTIVE);
    _scan->setInterval(_config.scan_interval_ms);
    _scan->setWindow(_config.scan_window_ms);
    _scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
    _scan->setDuplicateFilter(true);  // Filter duplicates within a scan window
    // Don't call setMaxResults - let NimBLE use defaults

    DEBUG("NimBLEPlatform: Scan configured - interval=" + std::to_string(_config.scan_interval_ms) +
          " window=" + std::to_string(_config.scan_window_ms));

    return true;
}

BLEAddress NimBLEPlatform::fromNimBLE(const NimBLEAddress& addr) {
    BLEAddress result;
    const ble_addr_t* base = addr.getBase();
    if (base) {
        // NimBLE stores addresses in little-endian: val[0]=LSB, val[5]=MSB
        // Our BLEAddress stores in big-endian display order: addr[0]=MSB, addr[5]=LSB
        // Need to reverse the byte order
        for (int i = 0; i < 6; i++) {
            result.addr[i] = base->val[5 - i];
        }
    }
    result.type = addr.getType();
    return result;
}

NimBLEAddress NimBLEPlatform::toNimBLE(const BLEAddress& addr) {
    // Use NimBLEAddress string constructor - it parses "XX:XX:XX:XX:XX:XX" format
    // and handles the byte order internally
    std::string addrStr = addr.toString();
    NimBLEAddress nimAddr(addrStr.c_str(), addr.type);
    DEBUG("NimBLEPlatform::toNimBLE: input=" + addrStr +
          " type=" + std::to_string(addr.type) +
          " -> nimAddr=" + std::string(nimAddr.toString().c_str()) +
          " nimType=" + std::to_string(nimAddr.getType()));
    return nimAddr;
}

NimBLEClient* NimBLEPlatform::findClient(uint16_t conn_handle) {
    auto it = _clients.find(conn_handle);
    return (it != _clients.end()) ? it->second : nullptr;
}

NimBLEClient* NimBLEPlatform::findClient(const BLEAddress& address) {
    for (const auto& kv : _clients) {
        if (kv.second && fromNimBLE(kv.second->getPeerAddress()) == address) {
            return kv.second;
        }
    }
    return nullptr;
}

uint16_t NimBLEPlatform::allocateConnHandle() {
    return _next_conn_handle++;
}

void NimBLEPlatform::freeConnHandle(uint16_t handle) {
    // No-op for simple allocator
}

void NimBLEPlatform::updateConnectionMTU(uint16_t conn_handle, uint16_t mtu) {
    auto it = _connections.find(conn_handle);
    if (it != _connections.end()) {
        it->second.mtu = mtu;
    }
}

}} // namespace RNS::BLE

#endif // ESP32 && USE_NIMBLE

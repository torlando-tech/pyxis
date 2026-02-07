/**
 * @file BLEPlatform.h
 * @brief BLE Hardware Abstraction Layer (HAL) interface
 *
 * Provides a platform-agnostic interface for BLE operations. Platform-specific
 * implementations (NimBLE, ESP-IDF, Zephyr) implement this interface to enable
 * the BLEInterface to work across different hardware.
 *
 * The HAL abstracts:
 * - BLE stack initialization and lifecycle
 * - Scanning and advertising
 * - Connection management
 * - GATT operations (read, write, notify)
 * - Callback handling
 */
#pragma once

#include "BLETypes.h"
#include "Bytes.h"

#include <memory>
#include <vector>

namespace RNS { namespace BLE {

/**
 * @brief Abstract BLE platform interface
 *
 * Platform-specific implementations should inherit from this class and
 * implement all pure virtual methods. The factory method create() returns
 * the appropriate implementation based on compile-time detection.
 */
class IBLEPlatform {
public:
    using Ptr = std::shared_ptr<IBLEPlatform>;

    virtual ~IBLEPlatform() = default;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    /**
     * @brief Initialize the BLE stack with configuration
     *
     * @param config Platform configuration
     * @return true if initialization successful
     */
    virtual bool initialize(const PlatformConfig& config) = 0;

    /**
     * @brief Start BLE operations (advertising/scanning based on role)
     * @return true if started successfully
     */
    virtual bool start() = 0;

    /**
     * @brief Stop all BLE operations
     */
    virtual void stop() = 0;

    /**
     * @brief Main loop processing - must be called periodically
     *
     * Handles BLE events, processes queued operations, and invokes callbacks.
     */
    virtual void loop() = 0;

    /**
     * @brief Shutdown and cleanup the BLE stack
     */
    virtual void shutdown() = 0;

    /**
     * @brief Check if platform is initialized and running
     */
    virtual bool isRunning() const = 0;

    //=========================================================================
    // Central Mode - Scanning
    //=========================================================================

    /**
     * @brief Start scanning for peripherals
     *
     * @param duration_ms Scan duration in milliseconds (0 = continuous)
     * @return true if scan started successfully
     */
    virtual bool startScan(uint16_t duration_ms = 0) = 0;

    /**
     * @brief Stop scanning
     */
    virtual void stopScan() = 0;

    /**
     * @brief Check if currently scanning
     */
    virtual bool isScanning() const = 0;

    //=========================================================================
    // Central Mode - Connections
    //=========================================================================

    /**
     * @brief Connect to a peripheral
     *
     * @param address Peer's BLE address
     * @param timeout_ms Connection timeout in milliseconds
     * @return true if connection attempt started
     */
    virtual bool connect(const BLEAddress& address, uint16_t timeout_ms = 10000) = 0;

    /**
     * @brief Disconnect from a peer
     *
     * @param conn_handle Connection handle
     * @return true if disconnect initiated
     */
    virtual bool disconnect(uint16_t conn_handle) = 0;

    /**
     * @brief Disconnect all connections
     */
    virtual void disconnectAll() = 0;

    /**
     * @brief Request MTU update for a connection
     *
     * @param conn_handle Connection handle
     * @param mtu Requested MTU
     * @return true if request was sent
     */
    virtual bool requestMTU(uint16_t conn_handle, uint16_t mtu) = 0;

    /**
     * @brief Discover services on connected peripheral
     *
     * @param conn_handle Connection handle
     * @return true if discovery started
     */
    virtual bool discoverServices(uint16_t conn_handle) = 0;

    //=========================================================================
    // Peripheral Mode - Advertising
    //=========================================================================

    /**
     * @brief Start advertising
     * @return true if advertising started
     */
    virtual bool startAdvertising() = 0;

    /**
     * @brief Stop advertising
     */
    virtual void stopAdvertising() = 0;

    /**
     * @brief Check if currently advertising
     */
    virtual bool isAdvertising() const = 0;

    /**
     * @brief Update advertising data
     *
     * @param data Custom advertising data
     * @return true if updated successfully
     */
    virtual bool setAdvertisingData(const Bytes& data) = 0;

    /**
     * @brief Set the identity data for the Identity characteristic
     *
     * @param identity 16-byte identity hash
     */
    virtual void setIdentityData(const Bytes& identity) = 0;

    //=========================================================================
    // GATT Operations
    //=========================================================================

    /**
     * @brief Write data to a connected peripheral's RX characteristic
     *
     * @param conn_handle Connection handle
     * @param data Data to write
     * @param response true for write with response, false for write without response
     * @return true if write was queued/sent
     */
    virtual bool write(uint16_t conn_handle, const Bytes& data, bool response = true) = 0;

    /**
     * @brief Read from a characteristic
     *
     * @param conn_handle Connection handle
     * @param char_handle Characteristic handle
     * @param callback Callback invoked with result
     * @return true if read was queued
     */
    virtual bool read(uint16_t conn_handle, uint16_t char_handle,
                      std::function<void(OperationResult, const Bytes&)> callback) = 0;

    /**
     * @brief Enable/disable notifications on TX characteristic
     *
     * @param conn_handle Connection handle
     * @param enable true to enable, false to disable
     * @return true if operation was queued
     */
    virtual bool enableNotifications(uint16_t conn_handle, bool enable) = 0;

    /**
     * @brief Send notification to a connected central (peripheral mode)
     *
     * @param conn_handle Connection handle
     * @param data Data to send
     * @return true if notification was sent
     */
    virtual bool notify(uint16_t conn_handle, const Bytes& data) = 0;

    /**
     * @brief Send notification to all connected centrals
     *
     * @param data Data to broadcast
     * @return true if at least one notification was sent
     */
    virtual bool notifyAll(const Bytes& data) = 0;

    //=========================================================================
    // Connection Management
    //=========================================================================

    /**
     * @brief Get all active connections
     */
    virtual std::vector<ConnectionHandle> getConnections() const = 0;

    /**
     * @brief Get connection by handle
     */
    virtual ConnectionHandle getConnection(uint16_t handle) const = 0;

    /**
     * @brief Get current connection count
     */
    virtual size_t getConnectionCount() const = 0;

    /**
     * @brief Check if connected to specific address
     */
    virtual bool isConnectedTo(const BLEAddress& address) const = 0;

    //=========================================================================
    // Callback Registration
    //=========================================================================

    /**
     * @brief Set callback for scan results
     */
    virtual void setOnScanResult(Callbacks::OnScanResult callback) = 0;

    /**
     * @brief Set callback for scan completion
     */
    virtual void setOnScanComplete(Callbacks::OnScanComplete callback) = 0;

    /**
     * @brief Set callback for outgoing connection established (central mode)
     */
    virtual void setOnConnected(Callbacks::OnConnected callback) = 0;

    /**
     * @brief Set callback for connection terminated
     */
    virtual void setOnDisconnected(Callbacks::OnDisconnected callback) = 0;

    /**
     * @brief Set callback for MTU change
     */
    virtual void setOnMTUChanged(Callbacks::OnMTUChanged callback) = 0;

    /**
     * @brief Set callback for service discovery completion
     */
    virtual void setOnServicesDiscovered(Callbacks::OnServicesDiscovered callback) = 0;

    /**
     * @brief Set callback for data received via notification (central mode)
     */
    virtual void setOnDataReceived(Callbacks::OnDataReceived callback) = 0;

    /**
     * @brief Set callback for notification enable/disable
     */
    virtual void setOnNotifyEnabled(Callbacks::OnNotifyEnabled callback) = 0;

    /**
     * @brief Set callback for incoming connection (peripheral mode)
     */
    virtual void setOnCentralConnected(Callbacks::OnCentralConnected callback) = 0;

    /**
     * @brief Set callback for incoming connection terminated (peripheral mode)
     */
    virtual void setOnCentralDisconnected(Callbacks::OnCentralDisconnected callback) = 0;

    /**
     * @brief Set callback for data received via write (peripheral mode)
     */
    virtual void setOnWriteReceived(Callbacks::OnWriteReceived callback) = 0;

    /**
     * @brief Set callback for read request (peripheral mode)
     */
    virtual void setOnReadRequested(Callbacks::OnReadRequested callback) = 0;

    //=========================================================================
    // Platform Info
    //=========================================================================

    /**
     * @brief Get the platform type
     */
    virtual PlatformType getPlatformType() const = 0;

    /**
     * @brief Get human-readable platform name
     */
    virtual std::string getPlatformName() const = 0;

    /**
     * @brief Get our local BLE address
     */
    virtual BLEAddress getLocalAddress() const = 0;
};

/**
 * @brief Factory for creating platform-specific BLE implementations
 */
class BLEPlatformFactory {
public:
    /**
     * @brief Create platform instance based on compile-time detection
     *
     * Returns the appropriate IBLEPlatform implementation for the current
     * platform (NimBLE for ESP32, Zephyr for nRF52840, etc.)
     *
     * @return Shared pointer to platform instance, or nullptr if no platform available
     */
    static IBLEPlatform::Ptr create();

    /**
     * @brief Create specific platform (for testing or explicit selection)
     *
     * @param type Platform type to create
     * @return Shared pointer to platform instance, or nullptr if not available
     */
    static IBLEPlatform::Ptr create(PlatformType type);

    /**
     * @brief Get the detected platform type for this build
     */
    static PlatformType getDetectedPlatform();
};

}} // namespace RNS::BLE

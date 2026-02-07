/**
 * @file BLETypes.h
 * @brief BLE-Reticulum Protocol v2.2 types, constants, and common structures
 *
 * This file defines the core types used throughout the BLE interface implementation.
 * It includes GATT service/characteristic UUIDs, protocol constants, enumerations,
 * and data structures used by all BLE components.
 */
#pragma once

#include "Bytes.h"
#include "Log.h"

#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <cstring>

namespace RNS { namespace BLE {

//=============================================================================
// Protocol Version
//=============================================================================

static constexpr uint8_t PROTOCOL_VERSION_MAJOR = 2;
static constexpr uint8_t PROTOCOL_VERSION_MINOR = 2;

//=============================================================================
// GATT Service and Characteristic UUIDs (BLE-Reticulum v2.2)
//=============================================================================

namespace UUID {
    // Reticulum BLE Service UUID
    static constexpr const char* SERVICE = "37145b00-442d-4a94-917f-8f42c5da28e3";

    // TX Characteristic (notify) - Data from peripheral to central
    static constexpr const char* TX_CHAR = "37145b00-442d-4a94-917f-8f42c5da28e4";

    // RX Characteristic (write) - Data from central to peripheral
    static constexpr const char* RX_CHAR = "37145b00-442d-4a94-917f-8f42c5da28e5";

    // Identity Characteristic (read) - 16-byte identity hash
    static constexpr const char* IDENTITY_CHAR = "37145b00-442d-4a94-917f-8f42c5da28e6";

    // Standard CCCD UUID for enabling notifications
    static constexpr const char* CCCD = "00002902-0000-1000-8000-00805f9b34fb";
}

//=============================================================================
// MTU Constants
//=============================================================================

namespace MTU {
    static constexpr uint16_t REQUESTED = 517;      // Request maximum MTU (BLE 5.0)
    static constexpr uint16_t MINIMUM = 23;         // BLE 4.0 minimum MTU
    static constexpr uint16_t INITIAL = 185;        // Conservative default (BLE 4.2)
    static constexpr uint16_t ATT_OVERHEAD = 3;     // ATT protocol header overhead
}

//=============================================================================
// Protocol Timing Constants (v2.2 spec)
//=============================================================================

namespace Timing {
    static constexpr double KEEPALIVE_INTERVAL = 15.0;        // Seconds between keepalives
    static constexpr double REASSEMBLY_TIMEOUT = 30.0;        // Seconds to complete reassembly
    static constexpr double CONNECTION_TIMEOUT = 30.0;        // Seconds to establish connection
    static constexpr double HANDSHAKE_TIMEOUT = 10.0;         // Seconds for identity exchange
    static constexpr double SCAN_INTERVAL = 5.0;              // Seconds between scans
    static constexpr double PEER_TIMEOUT = 30.0;              // Seconds before peer removal
    static constexpr double POST_MTU_DELAY = 0.15;            // Seconds after MTU negotiation
    static constexpr double BLACKLIST_BASE_BACKOFF = 60.0;    // Base backoff seconds
}

//=============================================================================
// Protocol Limits
//=============================================================================

namespace Limits {
#ifdef ARDUINO
    static constexpr size_t MAX_PEERS = 3;                    // Reduced for MCU memory constraints
    static constexpr size_t MAX_DISCOVERED_PEERS = 16;        // Reduced discovery cache for MCU
#else
    static constexpr size_t MAX_PEERS = 7;                    // Maximum simultaneous connections
    static constexpr size_t MAX_DISCOVERED_PEERS = 100;       // Discovery cache limit
#endif
    static constexpr size_t IDENTITY_SIZE = 16;               // Identity hash size (bytes)
    static constexpr size_t MAC_SIZE = 6;                     // BLE MAC address size (bytes)
    static constexpr uint8_t BLACKLIST_THRESHOLD = 3;         // Failures before blacklist
    static constexpr uint8_t BLACKLIST_MAX_MULTIPLIER = 8;    // Max 2^n backoff multiplier
}

//=============================================================================
// Peer Scoring Weights (v2.2 spec)
//=============================================================================

namespace Scoring {
    static constexpr float RSSI_WEIGHT = 0.60f;
    static constexpr float HISTORY_WEIGHT = 0.30f;
    static constexpr float RECENCY_WEIGHT = 0.10f;
    static constexpr int8_t RSSI_MIN = -100;      // dBm
    static constexpr int8_t RSSI_MAX = -30;       // dBm
}

//=============================================================================
// Fragment Header Constants
//=============================================================================

namespace Fragment {
    static constexpr size_t HEADER_SIZE = 5;

    enum Type : uint8_t {
        START    = 0x01,    // First fragment of multi-fragment message
        CONTINUE = 0x02,    // Middle fragment
        END      = 0x03     // Last fragment (or single fragment)
    };
}

//=============================================================================
// Enumerations
//=============================================================================

/**
 * @brief Platform type for compile-time selection
 */
enum class PlatformType {
    NONE,
    NIMBLE_ARDUINO,     // ESP32 with NimBLE-Arduino library
    ESP_IDF,            // ESP32 with ESP-IDF native BLE
    ZEPHYR,             // nRF52840 with Zephyr RTOS
    NORDIC_SDK          // nRF52840 with Nordic SDK (future)
};

/**
 * @brief BLE role in a connection
 */
enum class Role : uint8_t {
    NONE       = 0x00,
    CENTRAL    = 0x01,   // Initiates connections (GATT client)
    PERIPHERAL = 0x02,   // Accepts connections (GATT server)
    DUAL       = 0x03    // Both central and peripheral simultaneously
};

/**
 * @brief Connection state machine states
 */
enum class ConnectionState : uint8_t {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    DISCOVERING_SERVICES,
    READY,               // Fully connected, services discovered, handshake complete
    DISCONNECTING
};

/**
 * @brief Peer state for tracking
 */
enum class PeerState : uint8_t {
    DISCOVERED,          // Seen in scan, not connected
    CONNECTING,          // Connection in progress
    HANDSHAKING,         // Connected, awaiting identity exchange
    CONNECTED,           // Fully connected with identity
    DISCONNECTING,       // Disconnect in progress
    BLACKLISTED          // Temporarily blacklisted
};

/**
 * @brief GATT operation types for queuing
 */
enum class OperationType : uint8_t {
    READ,
    WRITE,
    WRITE_NO_RESPONSE,
    NOTIFY_ENABLE,
    NOTIFY_DISABLE,
    MTU_REQUEST
};

/**
 * @brief GATT operation result codes
 */
enum class OperationResult : uint8_t {
    SUCCESS,
    PENDING,
    TIMEOUT,
    DISCONNECTED,
    NOT_FOUND,
    NOT_SUPPORTED,
    INVALID_HANDLE,
    INSUFFICIENT_AUTH,
    INSUFFICIENT_ENC,
    BUSY,
    ERROR
};

/**
 * @brief Scan mode
 */
enum class ScanMode : uint8_t {
    PASSIVE,
    ACTIVE
};

//=============================================================================
// Data Structures
//=============================================================================

/**
 * @brief BLE address (6 bytes + type)
 */
struct BLEAddress {
    uint8_t addr[6] = {0};
    uint8_t type = 0;  // 0 = public, 1 = random

    BLEAddress() = default;

    BLEAddress(const uint8_t* address, uint8_t addr_type = 0) : type(addr_type) {
        if (address) {
            memcpy(addr, address, 6);
        }
    }

    bool operator==(const BLEAddress& other) const {
        return memcmp(addr, other.addr, 6) == 0 && type == other.type;
    }

    bool operator!=(const BLEAddress& other) const {
        return !(*this == other);
    }

    bool operator<(const BLEAddress& other) const {
        int cmp = memcmp(addr, other.addr, 6);
        if (cmp != 0) return cmp < 0;
        return type < other.type;
    }

    /**
     * @brief Check if this address is "lower" than another (for MAC sorting)
     *
     * The device with the lower MAC address should initiate the connection
     * as the central role. This provides deterministic connection direction.
     */
    bool isLowerThan(const BLEAddress& other) const {
        // Compare as 48-bit integers, MSB first (addr[0] is most significant)
        for (int i = 0; i < 6; i++) {
            if (addr[i] < other.addr[i]) return true;
            if (addr[i] > other.addr[i]) return false;
        }
        return false;  // Equal
    }

    /**
     * @brief Convert to colon-separated hex string (XX:XX:XX:XX:XX:XX)
     * addr[0] is MSB (first displayed), addr[5] is LSB (last displayed)
     */
    std::string toString() const {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return std::string(buf);
    }

    /**
     * @brief Parse from colon-separated hex string
     * First byte in string goes to addr[0] (MSB)
     */
    static BLEAddress fromString(const std::string& str) {
        BLEAddress result;
        if (str.length() >= 17) {
            unsigned int values[6];
            if (sscanf(str.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]) == 6) {
                for (int i = 0; i < 6; i++) {
                    result.addr[i] = static_cast<uint8_t>(values[i]);
                }
            }
        }
        return result;
    }

    /**
     * @brief Convert to Bytes for storage/comparison
     */
    Bytes toBytes() const {
        return Bytes(addr, 6);
    }

    /**
     * @brief Check if address is all zeros (invalid)
     */
    bool isZero() const {
        for (int i = 0; i < 6; i++) {
            if (addr[i] != 0) return false;
        }
        return true;
    }
};

/**
 * @brief Scan result from BLE discovery
 */
struct ScanResult {
    BLEAddress address;
    std::string name;
    int8_t rssi = 0;
    bool connectable = false;
    Bytes advertising_data;
    Bytes scan_response_data;
    bool has_reticulum_service = false;  // Pre-filtered for our service UUID
    Bytes identity_prefix;               // First 3 bytes of identity from "RNS-xxxxxx" name (Protocol v2.2)
};

/**
 * @brief Connection handle with associated state
 */
struct ConnectionHandle {
    uint16_t handle = 0xFFFF;           // Platform-specific connection handle
    BLEAddress peer_address;
    Role local_role = Role::NONE;       // Our role in this connection
    ConnectionState state = ConnectionState::DISCONNECTED;
    uint16_t mtu = MTU::MINIMUM;        // Negotiated MTU

    // Characteristic handles (discovered after connection)
    uint16_t rx_char_handle = 0;        // Handle for RX characteristic
    uint16_t tx_char_handle = 0;        // Handle for TX characteristic
    uint16_t tx_cccd_handle = 0;        // Handle for TX CCCD (notifications)
    uint16_t identity_handle = 0;       // Handle for Identity characteristic

    bool isValid() const { return handle != 0xFFFF; }

    void reset() {
        handle = 0xFFFF;
        peer_address = BLEAddress();
        local_role = Role::NONE;
        state = ConnectionState::DISCONNECTED;
        mtu = MTU::MINIMUM;
        rx_char_handle = 0;
        tx_char_handle = 0;
        tx_cccd_handle = 0;
        identity_handle = 0;
    }
};

/**
 * @brief GATT operation for queuing
 */
struct GATTOperation {
    OperationType type = OperationType::READ;
    uint16_t conn_handle = 0xFFFF;
    uint16_t char_handle = 0;
    Bytes data;                         // For writes
    uint32_t timeout_ms = 5000;

    // Completion callback
    std::function<void(OperationResult, const Bytes&)> callback;

    // Internal tracking
    double queued_at = 0;
    double started_at = 0;
};

/**
 * @brief Platform configuration
 */
struct PlatformConfig {
    Role role = Role::DUAL;

    // Advertising parameters (peripheral mode)
    uint16_t adv_interval_min_ms = 100;
    uint16_t adv_interval_max_ms = 200;
    std::string device_name = "RNS-Node";

    // Scan parameters (central mode)
    // WiFi/BLE Coexistence: With software coexistence, scan interval must not exceed 160ms.
    // Use lower duty cycle (25%) to give WiFi more RF access time.
    // Passive scanning reduces TX interference with WiFi.
    uint16_t scan_interval_ms = 120;  // 120ms interval (within 160ms coex limit)
    uint16_t scan_window_ms = 30;     // 30ms window (25% duty cycle for WiFi breathing room)
    ScanMode scan_mode = ScanMode::PASSIVE;  // Passive scan reduces RF contention
    uint16_t scan_duration_ms = 10000;  // 0 = continuous

    // Connection parameters
    uint16_t conn_interval_min_ms = 15;
    uint16_t conn_interval_max_ms = 30;
    uint16_t conn_latency = 0;
    uint16_t supervision_timeout_ms = 4000;

    // MTU
    uint16_t preferred_mtu = MTU::REQUESTED;

    // Limits
    uint8_t max_connections = Limits::MAX_PEERS;
};

//=============================================================================
// Callback Type Definitions
//=============================================================================

namespace Callbacks {
    // Scan callbacks
    using OnScanResult = std::function<void(const ScanResult& result)>;
    using OnScanComplete = std::function<void()>;

    // Connection callbacks (central mode - we initiated)
    using OnConnected = std::function<void(const ConnectionHandle& conn)>;
    using OnDisconnected = std::function<void(const ConnectionHandle& conn, uint8_t reason)>;
    using OnMTUChanged = std::function<void(const ConnectionHandle& conn, uint16_t mtu)>;
    using OnServicesDiscovered = std::function<void(const ConnectionHandle& conn, bool success)>;

    // Data callbacks
    using OnDataReceived = std::function<void(const ConnectionHandle& conn, const Bytes& data)>;
    using OnNotifyEnabled = std::function<void(const ConnectionHandle& conn, bool enabled)>;

    // Peripheral-mode callbacks (they connected to us)
    using OnCentralConnected = std::function<void(const ConnectionHandle& conn)>;
    using OnCentralDisconnected = std::function<void(const ConnectionHandle& conn)>;
    using OnWriteReceived = std::function<void(const ConnectionHandle& conn, const Bytes& data)>;
    using OnReadRequested = std::function<Bytes(const ConnectionHandle& conn)>;
}

//=============================================================================
// Utility Functions
//=============================================================================

/**
 * @brief Get the payload size for a given MTU
 * @param mtu The negotiated MTU
 * @return Maximum payload size per fragment
 */
inline size_t getPayloadSize(uint16_t mtu) {
    return (mtu > Fragment::HEADER_SIZE) ? (mtu - Fragment::HEADER_SIZE) : 0;
}

/**
 * @brief Check if a packet needs fragmentation
 * @param data_size Size of the data to send
 * @param mtu The negotiated MTU
 * @return true if fragmentation is required
 */
inline bool needsFragmentation(size_t data_size, uint16_t mtu) {
    return data_size > getPayloadSize(mtu);
}

/**
 * @brief Calculate number of fragments needed
 * @param data_size Size of the data to fragment
 * @param mtu The negotiated MTU
 * @return Number of fragments needed (minimum 1)
 */
inline uint16_t calculateFragmentCount(size_t data_size, uint16_t mtu) {
    size_t payload_size = getPayloadSize(mtu);
    if (payload_size == 0) return 0;
    return static_cast<uint16_t>((data_size + payload_size - 1) / payload_size);
}

/**
 * @brief Convert Role to string for logging
 */
inline const char* roleToString(Role role) {
    switch (role) {
        case Role::NONE:       return "NONE";
        case Role::CENTRAL:    return "CENTRAL";
        case Role::PERIPHERAL: return "PERIPHERAL";
        case Role::DUAL:       return "DUAL";
        default:               return "UNKNOWN";
    }
}

/**
 * @brief Convert ConnectionState to string for logging
 */
inline const char* stateToString(ConnectionState state) {
    switch (state) {
        case ConnectionState::DISCONNECTED:        return "DISCONNECTED";
        case ConnectionState::CONNECTING:          return "CONNECTING";
        case ConnectionState::CONNECTED:           return "CONNECTED";
        case ConnectionState::DISCOVERING_SERVICES: return "DISCOVERING_SERVICES";
        case ConnectionState::READY:               return "READY";
        case ConnectionState::DISCONNECTING:       return "DISCONNECTING";
        default:                                   return "UNKNOWN";
    }
}

/**
 * @brief Convert PeerState to string for logging
 */
inline const char* peerStateToString(PeerState state) {
    switch (state) {
        case PeerState::DISCOVERED:    return "DISCOVERED";
        case PeerState::CONNECTING:    return "CONNECTING";
        case PeerState::HANDSHAKING:   return "HANDSHAKING";
        case PeerState::CONNECTED:     return "CONNECTED";
        case PeerState::DISCONNECTING: return "DISCONNECTING";
        case PeerState::BLACKLISTED:   return "BLACKLISTED";
        default:                       return "UNKNOWN";
    }
}

}} // namespace RNS::BLE

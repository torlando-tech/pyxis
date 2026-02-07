#pragma once

#include "Interface.h"
#include "Bytes.h"
#include "Type.h"

#ifdef ARDUINO
#include <WiFi.h>
#include <WiFiClient.h>
#else
#include <netinet/in.h>
#endif

#include <stdint.h>
#include <string>

#define DEFAULT_TCP_PORT 4965

/**
 * TCPClientInterface - TCP client interface for microReticulum.
 *
 * Connects to a Python RNS TCPServerInterface or another TCP server.
 * Uses HDLC framing for wire protocol compatibility with Python RNS.
 *
 * Features:
 * - Automatic reconnection with configurable retry interval
 * - TCP keepalive for connection health monitoring
 * - HDLC framing (0x7E flags with byte stuffing)
 *
 * Usage:
 *   TCPClientInterface* tcp = new TCPClientInterface("tcp0");
 *   tcp->set_target_host("192.168.1.100");
 *   tcp->set_target_port(4965);
 *   Interface interface(tcp);
 *   interface.start();
 *   Transport::register_interface(interface);
 */
class TCPClientInterface : public RNS::InterfaceImpl {

public:
    // Match Python RNS constants
    static const uint32_t BITRATE_GUESS = 10 * 1000 * 1000;  // 10 Mbps
    static const uint32_t HW_MTU = 1064;  // Match UDPInterface

    // Reconnection parameters (match Python RNS)
    static const uint32_t RECONNECT_WAIT_MS = 5000;  // 5 seconds between reconnect attempts
    static const uint32_t CONNECT_TIMEOUT_MS = 5000; // 5 second connection timeout

    // TCP keepalive parameters (match Python RNS)
    static const int TCP_KEEPIDLE_SEC = 5;
    static const int TCP_KEEPINTVL_SEC = 2;
    static const int TCP_KEEPCNT_PROBES = 12;

public:
    TCPClientInterface(const char* name = "TCPClient");
    virtual ~TCPClientInterface();

    // Configuration (call before start())
    void set_target_host(const std::string& host) { _target_host = host; }
    void set_target_port(int port) { _target_port = port; }

    // InterfaceImpl overrides
    virtual bool start();
    virtual void stop();
    virtual void loop();

    virtual inline std::string toString() const {
        return "TCPClientInterface[" + _name + "/" + _target_host + ":" + std::to_string(_target_port) + "]";
    }

protected:
    virtual void send_outgoing(const RNS::Bytes& data);

private:
    // Connection management
    bool connect();
    void disconnect();
    void configure_socket();
    void handle_disconnect();

    // HDLC frame processing
    void process_incoming();
    void extract_and_process_frames();

    // Target server
    std::string _target_host;
    int _target_port = DEFAULT_TCP_PORT;

    // Connection state
    bool _initiator = true;
    uint32_t _last_connect_attempt = 0;
    bool _reconnected = false;  // Set when connection re-established after being offline
    uint32_t _last_data_received = 0;  // Track last data receipt for stale connection detection
    static const uint32_t STALE_CONNECTION_MS = 120000;  // Consider connection stale after 2 min no data

public:
    // Check and clear reconnection flag (for announcing after reconnect)
    bool check_reconnected() {
        if (_reconnected) {
            _reconnected = false;
            return true;
        }
        return false;
    }

private:

    // HDLC frame buffer for partial frame reassembly
    RNS::Bytes _frame_buffer;

    // Read buffer for incoming data
    RNS::Bytes _read_buffer;

    // Platform-specific socket
#ifdef ARDUINO
    WiFiClient _client;
    // WiFi credentials (for ESP32 WiFi connection)
    std::string _wifi_ssid;
    std::string _wifi_password;
#else
    int _socket = -1;
    in_addr_t _target_address = INADDR_NONE;
#endif
};

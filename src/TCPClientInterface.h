#pragma once

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#ifdef ARDUINO
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
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

    // Reconnection parameters. connect() runs on tcp_task (off the main loop),
    // but still bound the timeout and back off so a dead host doesn't busy-retry.
    static const uint32_t RECONNECT_WAIT_MS = 15000; // 15s between reconnect attempts
    static const uint32_t CONNECT_TIMEOUT_MS = 3000;  // 3s connect timeout (task-side)

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
    virtual bool send_outgoing(const RNS::Bytes& data);

private:
    // Connection management
    bool connect();
    void disconnect();
    void configure_socket();
    void handle_disconnect();

#ifdef ARDUINO
    // Only the blocking connect()/DNS runs on tcp_task so it can't stall the
    // main loop (which froze the UI). read/write/frame all stay on the main loop
    // (low latency, unchanged data path). _conn_state hands _client ownership
    // between the two so they never touch the socket concurrently:
    //   DISCONNECTED -> CONNECTING (task owns _client) -> CONNECTED (main loop
    //   owns _client); main loop flips CONNECTED -> DISCONNECTED on a drop.
    enum ConnState : uint8_t { DISCONNECTED = 0, CONNECTING = 1, CONNECTED = 2 };
    static void tcp_task(void* arg);
    void task_loop();
    TaskHandle_t _task_handle = nullptr;
    volatile bool _task_running = false;
    volatile bool _task_done = false;   // task sets this right before exit; stop() joins on it
    std::atomic<uint8_t> _conn_state{DISCONNECTED};
#endif

    // HDLC frame processing
    void process_incoming();
    void extract_and_process_frames();

    // Target server
    std::string _target_host;
    int _target_port = DEFAULT_TCP_PORT;

    // Connection state
    bool _initiator = true;
#ifdef ARDUINO
    // Touched by both task_loop() (core 0) and handle_disconnect() (core 1).
    std::atomic<uint32_t> _last_connect_attempt{0};
#else
    uint32_t _last_connect_attempt = 0;
#endif
#ifdef ARDUINO
    std::atomic<bool> _reconnected{false};  // re-established after offline (task-set)
#else
    bool _reconnected = false;  // Set when connection re-established after being offline
#endif
    uint32_t _last_data_received = 0;  // Track last data receipt for stale connection detection
    static const uint32_t STALE_CONNECTION_MS = 120000;  // Consider connection stale after 2 min no data

public:
    // Check and clear reconnection flag (for announcing after reconnect)
    bool check_reconnected() {
#ifdef ARDUINO
        return _reconnected.exchange(false);
#else
        if (_reconnected) {
            _reconnected = false;
            return true;
        }
        return false;
#endif
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

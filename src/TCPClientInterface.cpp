#include "TCPClientInterface.h"
#include "HDLC.h"

#include <microReticulum/Transport.h>
#include <microReticulum/Log.h>

#include <memory>

#ifdef ARDUINO
// ESP32 lwIP socket headers
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

using namespace RNS;

TCPClientInterface::TCPClientInterface(const char* name /*= "TCPClientInterface"*/)
    : RNS::InterfaceImpl(name) {

    _IN = true;
    _OUT = true;
    _bitrate = BITRATE_GUESS;
    _HW_MTU = HW_MTU;
}

/*virtual*/ TCPClientInterface::~TCPClientInterface() {
    stop();
}

/*virtual*/ bool TCPClientInterface::start() {
    _online = false;

    TRACE("TCPClientInterface: target host: " + _target_host);
    TRACE("TCPClientInterface: target port: " + std::to_string(_target_port));

    if (_target_host.empty()) {
        ERROR("TCPClientInterface: No target host configured");
        return false;
    }

#ifdef ARDUINO
    // The blocking connect() runs on its own task so it never stalls the main
    // loop. read/write/frame stay on the main loop (see loop()).
    _task_running = true;
    BaseType_t r = xTaskCreatePinnedToCore(tcp_task, "tcp", 6144, this, 1, &_task_handle, 0);
    if (r != pdPASS) {
        ERROR("TCPClientInterface: Failed to create connect task");
        _task_running = false;
        return false;
    }
    INFO("TCPClientInterface: connect worker running");
    return true;
#else
    // WiFi connection is handled externally (in main.cpp)
    // Attempt initial connection
    if (!connect()) {
        INFO("TCPClientInterface: Initial connection failed, will retry in background");
        // Don't return false - we'll reconnect in loop()
    }

    return true;
#endif
}

bool TCPClientInterface::connect() {
    TRACE("TCPClientInterface: Connecting to " + _target_host + ":" + std::to_string(_target_port));

#ifdef ARDUINO
    _client.setTimeout(CONNECT_TIMEOUT_MS);

    // 3-arg connect bounds the blocking time (the 2-arg form ignores it and can
    // block ~18.5s on an unreachable host). Runs on tcp_task, off the main loop.
    if (!_client.connect(_target_host.c_str(), _target_port, CONNECT_TIMEOUT_MS)) {
        DEBUG("TCPClientInterface: Connection failed");
        return false;
    }

    // Configure socket options
    configure_socket();

    INFO("TCPClientInterface: Connected to " + _target_host + ":" + std::to_string(_target_port));
    // task_loop() publishes the link state (_conn_state / _online / _reconnected)
    // after this returns; nothing else is touched here.
    return true;

#else
    // Resolve target host
    struct in_addr target_addr;
    if (inet_aton(_target_host.c_str(), &target_addr) == 0) {
        struct hostent* host_ent = gethostbyname(_target_host.c_str());
        if (host_ent == nullptr || host_ent->h_addr_list[0] == nullptr) {
            ERROR("TCPClientInterface: Unable to resolve host " + _target_host);
            return false;
        }
        _target_address = *((in_addr_t*)(host_ent->h_addr_list[0]));
    } else {
        _target_address = target_addr.s_addr;
    }

    // Create TCP socket
    _socket = socket(PF_INET, SOCK_STREAM, 0);
    if (_socket < 0) {
        ERROR("TCPClientInterface: Unable to create socket, error " + std::to_string(errno));
        return false;
    }

    // Set non-blocking for connect timeout
    int flags = fcntl(_socket, F_GETFL, 0);
    fcntl(_socket, F_SETFL, flags | O_NONBLOCK);

    // Connect to server
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = _target_address;
    server_addr.sin_port = htons(_target_port);

    int result = ::connect(_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(_socket);
        _socket = -1;
        ERROR("TCPClientInterface: Connect failed, error " + std::to_string(errno));
        return false;
    }

    // Wait for connection with timeout
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(_socket, &write_fds);
    struct timeval timeout;
    timeout.tv_sec = CONNECT_TIMEOUT_MS / 1000;
    timeout.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;

    result = select(_socket + 1, nullptr, &write_fds, nullptr, &timeout);
    if (result <= 0) {
        close(_socket);
        _socket = -1;
        DEBUG("TCPClientInterface: Connection timeout");
        return false;
    }

    // Check if connection succeeded
    int sock_error = 0;
    socklen_t len = sizeof(sock_error);
    getsockopt(_socket, SOL_SOCKET, SO_ERROR, &sock_error, &len);
    if (sock_error != 0) {
        close(_socket);
        _socket = -1;
        DEBUG("TCPClientInterface: Connection failed, error " + std::to_string(sock_error));
        return false;
    }

    // Restore blocking mode for normal operation
    fcntl(_socket, F_SETFL, flags);

    // Configure socket options
    configure_socket();

    INFO("TCPClientInterface: Connected to " + _target_host + ":" + std::to_string(_target_port));
    _online = true;
    _frame_buffer.clear();
    return true;
#endif
}

void TCPClientInterface::configure_socket() {
#ifdef ARDUINO
    // Get underlying socket fd for setsockopt
    int fd = _client.fd();
    if (fd < 0) {
        DEBUG("TCPClientInterface: Could not get socket fd for configuration");
        return;
    }

    // TCP_NODELAY - disable Nagle's algorithm
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Enable TCP keepalive
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

    // Keepalive parameters (may not all be available on ESP32 lwIP)
#ifdef TCP_KEEPIDLE
    int keepidle = TCP_KEEPIDLE_SEC;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
#endif
#ifdef TCP_KEEPINTVL
    int keepintvl = TCP_KEEPINTVL_SEC;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
    int keepcnt = TCP_KEEPCNT_PROBES;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif

    TRACE("TCPClientInterface: Socket configured with TCP_NODELAY and keepalive");

#else
    // TCP_NODELAY
    int flag = 1;
    setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Enable TCP keepalive
    setsockopt(_socket, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

    // Keepalive parameters
    int keepidle = TCP_KEEPIDLE_SEC;
    int keepintvl = TCP_KEEPINTVL_SEC;
    int keepcnt = TCP_KEEPCNT_PROBES;
    setsockopt(_socket, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(_socket, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(_socket, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    // TCP_USER_TIMEOUT (Linux 2.6.37+)
#ifdef TCP_USER_TIMEOUT
    int user_timeout = 24000;  // 24 seconds, matches Python RNS
    setsockopt(_socket, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout));
#endif

    TRACE("TCPClientInterface: Socket configured with TCP_NODELAY, keepalive, and timeouts");
#endif
}

void TCPClientInterface::disconnect() {
    DEBUG("TCPClientInterface: Disconnecting");

#ifdef ARDUINO
    _client.stop();
#else
    if (_socket >= 0) {
        close(_socket);
        _socket = -1;
    }
#endif

    _online = false;
    _frame_buffer.clear();
}

void TCPClientInterface::handle_disconnect() {
#ifdef ARDUINO
    // Called on the main loop while CONNECTED. Close the socket and hand it back
    // to tcp_task (DISCONNECTED) for a fresh connect.
    INFO("TCPClientInterface: Connection lost, will attempt reconnection");
    disconnect();                     // _client.stop(), _online=false, clear buffer
    _last_connect_attempt = millis();
    _conn_state.store(DISCONNECTED);
#else
    if (_online) {
        INFO("TCPClientInterface: Connection lost, will attempt reconnection");
        disconnect();
        // Reset connect attempt timer to enforce wait before reconnection
        _last_connect_attempt = millis();
    }
#endif
}

#ifdef ARDUINO
/*static*/ void TCPClientInterface::tcp_task(void* arg) {
    auto* self = static_cast<TCPClientInterface*>(arg);
    self->task_loop();
    self->_task_done = true;   // let stop() join before the object is freed
    vTaskDelete(nullptr);
}

// Owns _client ONLY while connecting. When the link is down it runs the blocking
// connect() here (off the main loop); on success it publishes CONNECTED and the
// main loop takes over all socket I/O. It never touches _client while CONNECTED.
void TCPClientInterface::task_loop() {
    while (_task_running) {
        if (_conn_state.load() == DISCONNECTED) {
            uint32_t now = millis();
            if (now - _last_connect_attempt >= RECONNECT_WAIT_MS) {
                _last_connect_attempt = now;
                if (ESP.getMaxAllocHeap() >= 20000) {  // skip under heap pressure
                    _conn_state.store(CONNECTING);      // claim _client
                    if (connect()) {
                        _frame_buffer.clear();
                        _last_data_received = millis();
                        _online = true;
                        _reconnected.store(true);       // main loop announces
                        _conn_state.store(CONNECTED);   // hand _client to main loop
                    } else {
                        _conn_state.store(DISCONNECTED);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

/*virtual*/ void TCPClientInterface::stop() {
#ifdef ARDUINO
    // Join the task: signal it, then wait until it has actually left task_loop()
    // before tearing anything down. An in-flight connect() can overrun
    // CONNECT_TIMEOUT_MS on a slow DNS server, and ~TCPClientInterface() calls
    // stop() — returning early would risk a use-after-free on `this`.
    _task_running = false;
    if (_task_handle != nullptr) {
        uint32_t deadline = millis() + CONNECT_TIMEOUT_MS + 2000;
        while (!_task_done && (int32_t)(millis() - deadline) < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        _task_handle = nullptr;
    }
    _conn_state.store(DISCONNECTED);
#endif
    disconnect();
}

/*virtual*/ void TCPClientInterface::loop() {
#ifdef ARDUINO
    // tcp_task owns _client while (re)connecting; the main loop only touches the
    // socket once CONNECTED. read/write/frame all happen here (same low-latency
    // path as before the task split). The legacy body below is unreachable on
    // ARDUINO.
    if (_conn_state.load() != CONNECTED) {
        _online = false;
        return;
    }
    _online = true;
    // ESP32 WiFiClient.connected() can momentarily read false; only treat it as a
    // drop when there is also no buffered data.
    if (!_client.connected() && _client.available() == 0) {
        handle_disconnect();
        return;
    }
    if (_client.available() > 0) {
        _last_data_received = millis();
        while (_client.available() > 0) {
            uint8_t byte = _client.read();
            _frame_buffer.append(byte);
        }
    }
    extract_and_process_frames();
    return;
#endif
    // Periodic status logging
    static uint32_t last_status_log = 0;
    static uint32_t loop_count = 0;
    static uint32_t total_rx = 0;
    loop_count++;
    uint32_t now = millis();
    // [TCP] connection-status heartbeat — protocol-debug only. Was at
    // INFO level firing every 5s; combined with the per-frame [TCP] /
    // [HDLC] / [ustore] prints below, this saturated USB CDC during
    // active LXST calls and starved T:CALL_QOS responses (#75).
    if (now - last_status_log >= 5000) {
        last_status_log = now;
        if (RNS::loglevel() >= RNS::LOG_DEBUG) {
            int avail = _client.available();
            Serial.printf("[TCP] connected=%d online=%d avail=%d loops=%u rx=%u buf=%d\n",
                          _client.connected(), _online, avail, loop_count, total_rx, (int)_frame_buffer.size());
        }
        loop_count = 0;
    }

    // Handle reconnection if not connected
    if (!_online) {
        if (_initiator) {
#ifdef ARDUINO
            uint32_t now = millis();
#else
            uint32_t now = static_cast<uint32_t>(Utilities::OS::time() * 1000);
#endif
            if (now - _last_connect_attempt >= RECONNECT_WAIT_MS) {
                _last_connect_attempt = now;
                // Skip reconnection if memory is too low - prevents fragmentation
                uint32_t max_block = ESP.getMaxAllocHeap();
                if (max_block < 20000) {
                    Serial.printf("[TCP] Skipping reconnect - low memory (max_block=%u)\n", max_block);
                } else {
                    DEBUG("TCPClientInterface: Attempting reconnection...");
                    connect();
                }
            }
        }
        return;
    }

    // Check connection status
    // Note: ESP32 WiFiClient.connected() has known bugs where it returns false incorrectly
    // See: https://github.com/espressif/arduino-esp32/issues/1714
    // Workaround: only disconnect if connected() is false AND no data available
#ifdef ARDUINO
    if (!_client.connected() && _client.available() == 0) {
        Serial.printf("[TCP] Connection closed (connected=false, available=0)\n");
        handle_disconnect();
        return;
    }

    // Stale connection detection disabled - was causing frequent reconnects
    // TODO: investigate why this triggers even when receiving data
    // if (_last_data_received > 0 && (now - _last_data_received) > STALE_CONNECTION_MS) {
    //     WARNING("TCPClientInterface: Connection appears stale, forcing reconnection");
    //     handle_disconnect();
    //     return;
    // }

    // Read available data
    int avail = _client.available();
    if (avail > 0) {
        bool dbg = RNS::loglevel() >= RNS::LOG_DEBUG;
        if (dbg) Serial.printf("[TCP] Reading %d bytes\n", avail);
        total_rx += avail;
        _last_data_received = now;  // Update stale timer on any data receipt
        size_t start_pos = _frame_buffer.size();
        while (_client.available() > 0) {
            uint8_t byte = _client.read();
            _frame_buffer.append(byte);
        }
        if (dbg) {
            Serial.printf("[TCP] First bytes: ");
            size_t dump_len = (_frame_buffer.size() - start_pos);
            if (dump_len > 20) dump_len = 20;
            for (size_t i = 0; i < dump_len; ++i) {
                Serial.printf("%02x ", _frame_buffer.data()[start_pos + i]);
            }
            Serial.printf("\n");
        }
    }
#else
    // Non-blocking read
    uint8_t buf[4096];
    ssize_t len = recv(_socket, buf, sizeof(buf), MSG_DONTWAIT);
    if (len > 0) {
        DEBUG("TCPClientInterface: Received " + std::to_string(len) + " bytes");
        _frame_buffer.append(buf, len);
    } else if (len == 0) {
        // Connection closed by peer
        DEBUG("TCPClientInterface: recv returned 0 - connection closed");
        handle_disconnect();
        return;
    } else {
        int err = errno;
        if (err != EAGAIN && err != EWOULDBLOCK) {
            // Socket error
            ERROR("TCPClientInterface: recv error " + std::to_string(err));
            handle_disconnect();
            return;
        }
        // EAGAIN/EWOULDBLOCK - normal for non-blocking, just no data yet
    }
#endif

    // Process any complete frames
    extract_and_process_frames();
}

void TCPClientInterface::extract_and_process_frames() {
    // Find and process complete HDLC frames: [FLAG][data][FLAG]
    static uint32_t frame_count = 0;

    while (true) {
        if (_frame_buffer.size() == 0) break;

        // Find first FLAG byte
        int start = -1;
        for (size_t i = 0; i < _frame_buffer.size(); ++i) {
            if (_frame_buffer.data()[i] == HDLC::FLAG) {
                start = static_cast<int>(i);
                break;
            }
        }

        if (start < 0) {
            // No FLAG found, discard buffer (garbage data before any frame)
            Serial.printf("[HDLC] No FLAG in %d bytes, clearing\n", (int)_frame_buffer.size());
            _frame_buffer.clear();
            break;
        }

        // Discard data before first FLAG
        if (start > 0) {
            Serial.printf("[HDLC] Discarding %d bytes before FLAG\n", start);
            _frame_buffer = _frame_buffer.mid(start);
        }

        // Find end FLAG (skip the start FLAG at position 0)
        int end = -1;
        for (size_t i = 1; i < _frame_buffer.size(); ++i) {
            if (_frame_buffer.data()[i] == HDLC::FLAG) {
                end = static_cast<int>(i);
                break;
            }
        }

        if (end < 0) {
            // Incomplete frame, wait for more data
            break;
        }

        // Extract frame content between FLAGS (excluding the FLAGS)
        Bytes frame_content = _frame_buffer.mid(1, end - 1);
        frame_count++;
        if (RNS::loglevel() >= RNS::LOG_DEBUG) {
            Serial.printf("[HDLC] Frame #%u: %d escaped bytes\n", frame_count, (int)frame_content.size());
        }

        // Remove processed frame from buffer (keep data after end FLAG)
        _frame_buffer = _frame_buffer.mid(end);

        // Skip empty frames (consecutive FLAGs)
        if (frame_content.size() == 0) {
            if (RNS::loglevel() >= RNS::LOG_DEBUG) Serial.printf("[HDLC] Empty frame, skipping\n");
            continue;
        }

        // Unescape frame
        Bytes unescaped = HDLC::unescape(frame_content);
        if (unescaped.size() == 0) {
            if (RNS::loglevel() >= RNS::LOG_DEBUG) Serial.printf("[HDLC] Unescape failed!\n");
            DEBUG("TCPClientInterface: HDLC unescape error, discarding frame");
            continue;
        }

        // Validate minimum frame size (matches Python RNS HEADER_MINSIZE check)
        if (unescaped.size() < Type::Reticulum::HEADER_MINSIZE) {
            TRACE("TCPClientInterface: Frame too small (" + std::to_string(unescaped.size()) + " bytes), discarding");
            continue;
        }

        // Pass to transport layer
        if (RNS::loglevel() >= RNS::LOG_DEBUG) {
            Serial.printf("[TCP] Processing frame: %d bytes\n", (int)unescaped.size());
        }
        DEBUG(toString() + ": Received frame, " + std::to_string(unescaped.size()) + " bytes");
        InterfaceImpl::handle_incoming(unescaped);
    }
}

/*virtual*/ bool TCPClientInterface::send_outgoing(const Bytes& data) {
    DEBUG(toString() + ".send_outgoing: data: " + std::to_string(data.size()) + " bytes");

    if (!_online) {
        DEBUG("TCPClientInterface: Not connected, cannot send");
        return false;
    }

    try {
        // Frame with HDLC
        Bytes framed = HDLC::frame(data);

        // Wire-format dumps are protocol-debug only — re-enable by
        // raising RNS log level to DEBUG. At INFO they fired ~10×/s
        // during voice calls (pre + post HDLC, per packet) and
        // saturated USB CDC, starving T:CALL_QOS responses.
        if (RNS::loglevel() >= RNS::LOG_DEBUG) {
            std::string hex_preview;
            size_t preview_len = (data.size() < 50) ? data.size() : 50;
            for (size_t i = 0; i < preview_len; ++i) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x", data.data()[i]);
                hex_preview += buf;
            }
            if (data.size() > 50) hex_preview += "...";
            DEBUG("WIRE TX raw (" + std::to_string(data.size()) + " bytes): " + hex_preview);

            std::string framed_hex;
            size_t flen = (framed.size() < 30) ? framed.size() : 30;
            for (size_t i = 0; i < flen; ++i) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x", framed.data()[i]);
                framed_hex += buf;
            }
            if (framed.size() > 30) framed_hex += "...";
            DEBUG("WIRE TX framed (" + std::to_string(framed.size()) + " bytes): " + framed_hex);
        }

#ifdef ARDUINO
        // Only write when CONNECTED — while (re)connecting, _client belongs to
        // tcp_task. send_outgoing() runs on the main loop (same thread as loop()),
        // so no lock is needed once CONNECTED.
        if (_conn_state.load() != CONNECTED) {
            return false;  // not connected; Reticulum will retry/route
        }
        size_t written = _client.write(framed.data(), framed.size());
        if (written != framed.size()) {
            ERROR("TCPClientInterface: Write incomplete, " + std::to_string(written) +
                  " of " + std::to_string(framed.size()) + " bytes");
            handle_disconnect();
            return false;
        }
        _client.flush();
#else
        ssize_t written = send(_socket, framed.data(), framed.size(), MSG_NOSIGNAL);
        if (written < 0) {
            ERROR("TCPClientInterface: send error " + std::to_string(errno));
            handle_disconnect();
            return false;
        }
        if (static_cast<size_t>(written) != framed.size()) {
            ERROR("TCPClientInterface: Write incomplete, " + std::to_string(written) +
                  " of " + std::to_string(framed.size()) + " bytes");
            handle_disconnect();
            return false;
        }
#endif

        // Perform post-send housekeeping
        InterfaceImpl::handle_outgoing(data);
        return true;

    } catch (std::exception& e) {
        ERROR("TCPClientInterface: Exception during send: " + std::string(e.what()));
        handle_disconnect();
    }
    return false;
}

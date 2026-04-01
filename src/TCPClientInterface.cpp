#include "TCPClientInterface.h"
#include "HDLC.h"
#include <tdeck_ui/Hardware/TDeck/Display.h>

#include <Transport.h>
#include <Log.h>

#include <algorithm>
#include <cstring>
#include <memory>

#ifdef ARDUINO
// ESP32 lwIP socket headers
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_task_wdt.h>
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
    reserve_buffers();
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

    // WiFi connection is handled externally (in main.cpp)
    // Attempt initial connection
    if (!connect()) {
        INFO("TCPClientInterface: Initial connection failed, will retry in background");
        // Don't return false - we'll reconnect in loop()
    }

    return true;
}

bool TCPClientInterface::connect() {
    TRACE("TCPClientInterface: Connecting to " + _target_host + ":" + std::to_string(_target_port));

#ifdef ARDUINO
    // Set connection timeout
    _client.setTimeout(CONNECT_TIMEOUT_MS);

    if (!_client.connect(_target_host.c_str(), _target_port)) {
        DEBUG("TCPClientInterface: Connection failed");
        return false;
    }

    // Configure socket options
    configure_socket();

    INFO("TCPClientInterface: Connected to " + _target_host + ":" + std::to_string(_target_port));
    _online = true;
    _reconnected = true;  // Signal that we (re)connected - main loop should announce
    _last_data_received = millis();  // Reset stale timer
    reset_buffers();
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
    reset_buffers();
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
    reset_buffers();
}

void TCPClientInterface::handle_disconnect() {
    if (_online) {
        INFO("TCPClientInterface: Connection lost, will attempt reconnection");
        disconnect();
        // Reset connect attempt timer to enforce wait before reconnection
        _last_connect_attempt = millis();
    }
}

void TCPClientInterface::reserve_buffers() {
    const size_t rx_capacity = FRAME_BUFFER_HARD_LIMIT + READ_BUDGET_BYTES;
    if (_frame_buffer.capacity() < rx_capacity) {
        _frame_buffer.reserve(rx_capacity);
    }

    const size_t scratch_capacity = HW_MTU * 2;
    if (_frame_scratch.capacity() < scratch_capacity) {
        _frame_scratch.reserve(scratch_capacity);
    }
}

void TCPClientInterface::reset_buffers() {
    _frame_buffer.clear();
    _frame_start = 0;
    _frame_scratch.clear();
    reserve_buffers();
}

size_t TCPClientInterface::active_buffered_bytes() const {
    if (_frame_buffer.size() <= _frame_start) {
        return 0;
    }
    return _frame_buffer.size() - _frame_start;
}

void TCPClientInterface::compact_rx_buffer() {
    if (_frame_start == 0) {
        return;
    }

    const size_t active = active_buffered_bytes();
    if (active == 0) {
        _frame_buffer.clear();
        _frame_start = 0;
        return;
    }

    std::memmove(_frame_buffer.data(), _frame_buffer.data() + _frame_start, active);
    _frame_buffer.resize(active);
    _frame_start = 0;
}

bool TCPClientInterface::append_rx_bytes(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return true;
    }

    const size_t active = active_buffered_bytes();
    if (_frame_start > 0 && (_frame_buffer.size() + len > FRAME_BUFFER_HARD_LIMIT || active + len > FRAME_BUFFER_HARD_LIMIT)) {
        compact_rx_buffer();
    }

    if (active_buffered_bytes() + len > FRAME_BUFFER_HARD_LIMIT) {
        return false;
    }

    reserve_buffers();

    try {
        _frame_buffer.insert(_frame_buffer.end(), data, data + len);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool TCPClientInterface::decode_hdlc_payload(size_t start, size_t end) {
    if (end <= start) {
        _frame_scratch.clear();
        return true;
    }

    const size_t escaped_len = end - start;
    _frame_scratch.clear();
    if (_frame_scratch.capacity() < escaped_len) {
        _frame_scratch.reserve(escaped_len);
    }

    bool in_escape = false;
    const uint8_t* payload = _frame_buffer.data() + start;
    for (size_t i = 0; i < escaped_len; ++i) {
        uint8_t byte = payload[i];
        if (in_escape) {
            _frame_scratch.push_back(static_cast<uint8_t>(byte ^ HDLC::ESC_MASK));
            in_escape = false;
        } else if (byte == HDLC::ESC) {
            in_escape = true;
        } else {
            _frame_scratch.push_back(byte);
        }
    }

    if (in_escape) {
        _frame_scratch.clear();
        return false;
    }

    return true;
}

/*virtual*/ void TCPClientInterface::stop() {
    disconnect();
}

/*virtual*/ void TCPClientInterface::loop() {
    // Periodic status logging
    static uint32_t last_status_log = 0;
    static uint32_t loop_count = 0;
    static uint32_t total_rx = 0;
    loop_count++;
    uint32_t now = millis();
    if (now - last_status_log >= 5000) {  // Every 5 seconds
        last_status_log = now;
        int avail = _client.available();
        Serial.printf("[TCP] connected=%d online=%d avail=%d loops=%u rx=%u buf=%d\n",
                      _client.connected(), _online, avail, loop_count, total_rx, (int)active_buffered_bytes());
        loop_count = 0;
    }

    if (active_buffered_bytes() >= FRAME_BUFFER_HIGH_WATER) {
        _read_throttled = true;
    } else if (_read_throttled && active_buffered_bytes() <= FRAME_BUFFER_LOW_WATER) {
        _read_throttled = false;
    }

    if (active_buffered_bytes() > FRAME_BUFFER_HARD_LIMIT) {
        Serial.printf("[TCP] Frame buffer exceeded hard limit (%d), reconnecting\n", (int)active_buffered_bytes());
        handle_disconnect();
        return;
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
    if (_read_throttled && avail > 0) {
        if (now - _last_backpressure_log_ms >= PRESSURE_LOG_INTERVAL_MS) {
            _last_backpressure_log_ms = now;
            Serial.printf("[TCP] Backpressure active, skipping read with %d buffered bytes and %d pending bytes\n",
                          (int)active_buffered_bytes(), avail);
        }
    } else if (avail > 0) {
        size_t bytes_read = 0;
        while (_client.available() > 0 && bytes_read < READ_BUDGET_BYTES) {
            size_t budget_left = READ_BUDGET_BYTES - bytes_read;
            size_t chunk = std::min<size_t>(budget_left, static_cast<size_t>(_client.available()));
            chunk = std::min<size_t>(chunk, _read_buffer.size());
            if (chunk == 0) {
                break;
            }

            int read_len = _client.read(_read_buffer.data(), chunk);
            if (read_len <= 0) {
                break;
            }

            if (!append_rx_bytes(_read_buffer.data(), static_cast<size_t>(read_len))) {
                Serial.printf("[TCP] Frame buffer overflow while appending %d bytes, reconnecting\n", read_len);
                handle_disconnect();
                return;
            }

            total_rx += static_cast<uint32_t>(read_len);
            bytes_read += static_cast<size_t>(read_len);
            _last_data_received = now;  // Update stale timer on any data receipt

            if (static_cast<size_t>(read_len) < chunk) {
                break;
            }
        }

        if (_client.available() > 0) {
            if (now - _last_read_budget_log_ms >= PRESSURE_LOG_INTERVAL_MS) {
                _last_read_budget_log_ms = now;
                Serial.printf("[TCP] Read budget exhausted, deferring %d bytes\n", _client.available());
            }
        }
#ifdef TCPCLIENT_VERBOSE_LOGGING
        if (now - _last_read_detail_log_ms >= READ_DETAIL_LOG_INTERVAL_MS) {
            _last_read_detail_log_ms = now;
            Serial.printf("[TCP] Reading %u bytes\n", (unsigned)bytes_read);
            Serial.printf("[TCP] First bytes: ");
            size_t dump_len = bytes_read;
            if (dump_len > 20) dump_len = 20;
            for (size_t i = 0; i < dump_len; ++i) {
                Serial.printf("%02x ", _read_buffer[i]);
            }
            Serial.printf("\n");
        }
#endif
    }
#else
    // Non-blocking read
    ssize_t len = recv(_socket, _read_buffer.data(), _read_buffer.size(), MSG_DONTWAIT);
    if (len > 0) {
        if (!append_rx_bytes(_read_buffer.data(), static_cast<size_t>(len))) {
            ERROR("TCPClientInterface: Frame buffer overflow, reconnecting");
            handle_disconnect();
            return;
        }
        total_rx += static_cast<uint32_t>(len);
        _last_data_received = now;
        DEBUG("TCPClientInterface: Received " + std::to_string(len) + " bytes");
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
    size_t frame_budget = FRAME_BUDGET_PER_LOOP;
    uint32_t process_budget = PROCESS_BUDGET_MS;
    uint32_t last_flush_ms = Hardware::TDeck::Display::last_flush_ms();
    bool display_stalled = (last_flush_ms > 0 && (now - last_flush_ms) > 1000);
    if (_read_throttled) {
        frame_budget = FRAME_BUDGET_PER_LOOP * 4;
        process_budget = PROCESS_BUDGET_MS * 3;
    }
    if (display_stalled) {
        frame_budget = std::min<size_t>(frame_budget, FRAME_BUDGET_PER_LOOP * 2);
        process_budget = std::min<uint32_t>(process_budget, PROCESS_BUDGET_MS);
    }
    extract_and_process_frames(frame_budget, now, process_budget);

#ifdef ARDUINO
    if (_read_throttled || display_stalled) {
        taskYIELD();
    }
#endif
}

void TCPClientInterface::extract_and_process_frames(size_t max_frames, uint32_t start_ms, uint32_t budget_ms) {
    // Find and process complete HDLC frames: [FLAG][data][FLAG]
    static uint32_t frame_count = 0;
    size_t frames_processed = 0;

    while (true) {
        if (frames_processed >= max_frames) {
            uint32_t now = millis();
            if (now - _last_process_budget_log_ms >= PRESSURE_LOG_INTERVAL_MS) {
                _last_process_budget_log_ms = now;
                Serial.printf("[HDLC] Frame budget exhausted with %d buffered bytes remaining\n", (int)active_buffered_bytes());
            }
            break;
        }
        if (millis() - start_ms >= budget_ms) {
            uint32_t now = millis();
            if (now - _last_process_budget_log_ms >= PRESSURE_LOG_INTERVAL_MS) {
                _last_process_budget_log_ms = now;
                Serial.printf("[HDLC] Time budget exhausted with %d buffered bytes remaining\n", (int)active_buffered_bytes());
            }
            break;
        }

        const size_t active = active_buffered_bytes();
        if (active == 0) {
            break;
        }

        const uint8_t* base = _frame_buffer.data();
        const uint8_t* start_ptr = static_cast<const uint8_t*>(
            std::memchr(base + _frame_start, HDLC::FLAG, active));
        if (!start_ptr) {
            // No FLAG found, discard stale garbage so the buffer cannot grow forever.
            Serial.printf("[HDLC] No FLAG in %d buffered bytes, clearing\n", (int)active);
            reset_buffers();
            break;
        }

        size_t start = static_cast<size_t>(start_ptr - base);
        if (start > _frame_start) {
#ifdef TCPCLIENT_VERBOSE_LOGGING
            Serial.printf("[HDLC] Discarding %d bytes before FLAG\n", (int)(start - _frame_start));
#endif
            _frame_start = start;
        }

        const size_t remaining = _frame_buffer.size() - _frame_start;
        if (remaining <= 1) {
            break;
        }

        const uint8_t* end_ptr = static_cast<const uint8_t*>(
            std::memchr(base + _frame_start + 1, HDLC::FLAG, remaining - 1));
        if (!end_ptr) {
            // Incomplete frame, wait for more data
            break;
        }

        const size_t end = static_cast<size_t>(end_ptr - base);
        const size_t escaped_start = _frame_start + 1;

        if (!decode_hdlc_payload(escaped_start, end)) {
            Serial.printf("[HDLC] Unescape failed!\n");
            DEBUG("TCPClientInterface: HDLC unescape error, discarding frame");
            _frame_start = end + 1;
            continue;
        }

        Bytes frame_content;
        if (_frame_scratch.empty()) {
            frame_content = Bytes();
        } else {
            frame_content = Bytes(_frame_scratch.data(), _frame_scratch.size());
        }
        frame_count++;
        if (_frame_scratch.size() > 0) {
#ifdef TCPCLIENT_VERBOSE_LOGGING
            Serial.printf("[HDLC] Frame #%u: %d escaped bytes\n", frame_count, (int)_frame_scratch.size());
#endif
        }

        // Skip empty frames (consecutive FLAGs)
        if (_frame_scratch.size() == 0) {
#ifdef TCPCLIENT_VERBOSE_LOGGING
            Serial.printf("[HDLC] Empty frame, skipping\n");
#endif
            _frame_start = end + 1;
            continue;
        }

        // Validate minimum frame size (matches Python RNS HEADER_MINSIZE check)
        if (frame_content.size() < Type::Reticulum::HEADER_MINSIZE) {
            TRACE("TCPClientInterface: Frame too small (" + std::to_string(frame_content.size()) + " bytes), discarding");
            _frame_start = end + 1;
            continue;
        }

        // Pass to transport layer
        #ifdef TCPCLIENT_VERBOSE_LOGGING
        Serial.printf("[TCP] Processing frame: %d bytes\n", (int)frame_content.size());
        #endif
        DEBUG(toString() + ": Received frame, " + std::to_string(frame_content.size()) + " bytes");
        InterfaceImpl::handle_incoming(frame_content);
        frames_processed++;
#ifdef ARDUINO
        esp_task_wdt_reset();
#endif

        _frame_start = end + 1;
        if (_frame_start >= _frame_buffer.size()) {
            reset_buffers();
            break;
        }

        if (_frame_start > FRAME_BUFFER_LOW_WATER && _frame_start > (_frame_buffer.size() / 2)) {
            compact_rx_buffer();
        }
    }
}

/*virtual*/ void TCPClientInterface::send_outgoing(const Bytes& data) {
    DEBUG(toString() + ".send_outgoing: data: " + std::to_string(data.size()) + " bytes");

#ifdef TCPCLIENT_VERBOSE_LOGGING
    // Log first 50 bytes of raw packet (before HDLC framing)
    std::string hex_preview;
    size_t preview_len = (data.size() < 50) ? data.size() : 50;
    for (size_t i = 0; i < preview_len; ++i) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x", data.data()[i]);
        hex_preview += buf;
    }
    if (data.size() > 50) hex_preview += "...";
    INFO("WIRE TX raw (" + std::to_string(data.size()) + " bytes): " + hex_preview);
#endif

    if (!_online) {
        DEBUG("TCPClientInterface: Not connected, cannot send");
        return;
    }

    try {
        // Frame with HDLC
        Bytes framed = HDLC::frame(data);

#ifdef TCPCLIENT_VERBOSE_LOGGING
        // Log HDLC framed output for debugging
        std::string framed_hex;
        size_t flen = (framed.size() < 30) ? framed.size() : 30;
        for (size_t i = 0; i < flen; ++i) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x", framed.data()[i]);
            framed_hex += buf;
        }
        if (framed.size() > 30) framed_hex += "...";
        INFO("WIRE TX framed (" + std::to_string(framed.size()) + " bytes): " + framed_hex);
#endif

#ifdef ARDUINO
        size_t written = _client.write(framed.data(), framed.size());
        if (written != framed.size()) {
            ERROR("TCPClientInterface: Write incomplete, " + std::to_string(written) +
                  " of " + std::to_string(framed.size()) + " bytes");
            handle_disconnect();
            return;
        }
        _client.flush();
#else
        ssize_t written = send(_socket, framed.data(), framed.size(), MSG_NOSIGNAL);
        if (written < 0) {
            ERROR("TCPClientInterface: send error " + std::to_string(errno));
            handle_disconnect();
            return;
        }
        if (static_cast<size_t>(written) != framed.size()) {
            ERROR("TCPClientInterface: Write incomplete, " + std::to_string(written) +
                  " of " + std::to_string(framed.size()) + " bytes");
            handle_disconnect();
            return;
        }
#endif

        // Perform post-send housekeeping
        InterfaceImpl::handle_outgoing(data);

    } catch (std::exception& e) {
        ERROR("TCPClientInterface: Exception during send: " + std::string(e.what()));
        handle_disconnect();
    }
}

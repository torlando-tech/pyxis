#include "TCPClientInterface.h"
#include "HDLC.h"

#include <microReticulum/Transport.h>
#include <microReticulum/Log.h>

#include <memory>

#ifdef ARDUINO
// ESP32 lwIP socket headers
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <esp_heap_caps.h>
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

// Keep TCP work per main-loop pass bounded. A peer that continuously fills the
// receive socket must not prevent loopTask from returning to its watchdog feed.
static constexpr size_t MAX_TCP_BYTES_PER_LOOP = 4096;
static constexpr size_t MAX_TCP_FRAMES_PER_LOOP = 32;
static constexpr size_t MAX_TCP_FRAME_BUFFER = 16384;

#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
static const char* tcp_diag_state(uint8_t state) {
    switch (state) {
        case 0: return "DISCONNECTED";
        case 1: return "CONNECTING";
        case 2: return "CONNECTED";
        default: return "UNKNOWN";
    }
}

static void tcp_diag_socket_option(int fd, int level, int option, const char* name,
                                   int configured, int set_result) {
    int actual = -1;
    socklen_t actual_size = sizeof(actual);
    errno = 0;
    int get_result = getsockopt(fd, level, option, &actual, &actual_size);
    int get_errno = errno;
    Serial.printf("T:TCP_SOCKET fd=%d option=%s wanted=%d set_rc=%d get_rc=%d actual=%d errno=%d\n",
                  fd, name, configured, set_result, get_result, actual, get_errno);
}

#define TCP_DIAG_WORKER() do { \
    static uint32_t last_log = 0; \
    uint32_t now = millis(); \
    if (now - last_log >= 5000) { \
        last_log = now; \
        Serial.printf("T:TCP_WORKER now=%lu state=%s running=%d done=%d since_attempt=%lu " \
                      "free=%u largest=%u minimum=%u stack=%u\n", \
                      static_cast<unsigned long>(now), tcp_diag_state(_conn_state.load()), \
                      _task_running.load(), _task_done.load(), \
                      static_cast<unsigned long>(now - _last_connect_attempt.load()), \
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), \
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)), \
                      static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)), \
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr))); \
    } \
} while (0)

#define TCP_DIAG_MAIN() do { \
    static uint32_t last_log = 0; \
    uint32_t now = millis(); \
    if (now - last_log >= 5000) { \
        last_log = now; \
        Serial.printf("T:TCP_MAIN now=%lu state=%s online=%d fd=%d connected=%d avail=%d " \
                      "running=%d done=%d wifi=%d free=%u largest=%u minimum=%u\n", \
                      static_cast<unsigned long>(now), tcp_diag_state(_conn_state.load()), \
                      static_cast<int>(_online), _client.fd(), static_cast<int>(_client.connected()), \
                      _client.available(), _task_running.load(), _task_done.load(), \
                      static_cast<int>(WiFi.status()), \
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), \
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)), \
                      static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL))); \
    } \
} while (0)
#else
#define TCP_DIAG_WORKER() do { } while (0)
#define TCP_DIAG_MAIN() do { } while (0)
#endif

#if defined(ARDUINO) && defined(PYXIS_NOMAD_LINK_DIAGNOSTIC)
static const char* nomad_link_packet_kind(const RNS::Bytes& packet) {
    if (packet.size() < Type::Reticulum::HEADER_MINSIZE) return nullptr;
    if ((packet.data()[0] & 0x03) == Type::Packet::LINKREQUEST) return "LINKREQUEST";
    const bool header_2 = (packet.data()[0] & 0x40) != 0;
    const size_t context_offset = header_2 ? 34 : 18;
    if (packet.size() > context_offset && packet.data()[context_offset] == Type::Packet::LRPROOF) {
        return "LRPROOF";
    }
    return nullptr;
}

static void nomad_trace_packet(const char* direction, const RNS::Bytes& packet,
                               size_t framed_size, size_t io_size) {
    const char* kind = nomad_link_packet_kind(packet);
    if (!kind) return;
    Serial.printf("T:WIRE %s kind=%s raw=%u framed=%u io=%u hex=",
                  direction, kind, static_cast<unsigned>(packet.size()),
                  static_cast<unsigned>(framed_size), static_cast<unsigned>(io_size));
    for (size_t i = 0; i < packet.size(); ++i) Serial.printf("%02x", packet.data()[i]);
    Serial.println();
}
#endif

static bool contains_complete_hdlc_frame(const RNS::Bytes& buffer) {
    bool saw_start = false;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer.data()[i] != HDLC::FLAG) continue;
        if (saw_start) return true;
        saw_start = true;
    }
    return false;
}

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
    // Seed _last_connect_attempt so the task's first reconnect-wait check passes
    // immediately; otherwise the initial connect could be delayed up to
    // RECONNECT_WAIT_MS. Unsigned wraparound keeps this correct when
    // millis() < RECONNECT_WAIT_MS.
    _last_connect_attempt = millis() - RECONNECT_WAIT_MS;
    // stop() leaves the prior worker's completion latched. Runtime settings
    // reuse this object, so re-arm the latch before publishing/running the next
    // worker; otherwise a later stop can skip joining the replacement task.
    _task_done = false;
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
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    Serial.printf("T:TCP_ATTEMPT phase=start now=%lu state=%s running=%d done=%d wifi=%d "
                  "free=%u largest=%u minimum=%u stack=%u\n",
                  static_cast<unsigned long>(millis()),
                  tcp_diag_state(_conn_state.load()), _task_running.load(), _task_done.load(),
                  static_cast<int>(WiFi.status()),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    IPAddress target;
    if (!WiFi.hostByName(_target_host.c_str(), target)) {
        record_connect_failure(TcpConnectStage::DNS, 0, false);
        return false;
    }

    errno = 0;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        record_connect_failure(TcpConnectStage::SOCKET, errno, true);
        return false;
    }
    auto fail_socket = [&](TcpConnectStage stage, int error, bool local) {
        close(sockfd);
        record_connect_failure(stage, error, local);
        return false;
    };

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return fail_socket(TcpConnectStage::SOCKET_OPTIONS, errno, true);
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    uint32_t address = target;
    std::memcpy(&server.sin_addr.s_addr, &address, sizeof(address));
    server.sin_port = htons(_target_port);

    errno = 0;
    int result = ::connect(sockfd, reinterpret_cast<sockaddr*>(&server), sizeof(server));
    if (result < 0 && errno != EINPROGRESS) {
        int error = errno;
        return fail_socket(TcpConnectStage::CONNECT, error, local_lwip_failure(error));
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);
    timeval timeout{static_cast<long>(CONNECT_TIMEOUT_MS / 1000),
                    static_cast<long>((CONNECT_TIMEOUT_MS % 1000) * 1000)};
    errno = 0;
    result = select(sockfd + 1, nullptr, &write_fds, nullptr, &timeout);
    if (result == 0) return fail_socket(TcpConnectStage::SELECT_TIMEOUT, ETIMEDOUT, false);
    if (result < 0) {
        int error = errno;
        return fail_socket(TcpConnectStage::SELECT_ERROR, error, local_lwip_failure(error));
    }

    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    errno = 0;
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) < 0) {
        int error = errno;
        return fail_socket(TcpConnectStage::SO_ERROR_READ, error, local_lwip_failure(error));
    }
    if (socket_error != 0) {
        return fail_socket(TcpConnectStage::SOCKET_ERROR, socket_error,
                           local_lwip_failure(socket_error));
    }

    timeval io_timeout{static_cast<long>(CONNECT_TIMEOUT_MS / 1000),
                       static_cast<long>((CONNECT_TIMEOUT_MS % 1000) * 1000)};
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) < 0 ||
        fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return fail_socket(TcpConnectStage::SOCKET_OPTIONS, errno, true);
    }

    _client = WiFiClient(sockfd);
    _client.setTimeout(CONNECT_TIMEOUT_MS);
    if (!configure_socket()) {
        int error = errno;
        _client.stop();
        record_connect_failure(TcpConnectStage::SOCKET_OPTIONS, error, true);
        return false;
    }
    _consecutive_local_failures = 0;

#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    Serial.printf("T:TCP_ATTEMPT phase=connected now=%lu fd=%d connected=%d\n",
                  static_cast<unsigned long>(millis()), _client.fd(),
                  static_cast<int>(_client.connected()));
#endif
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

bool TCPClientInterface::configure_socket() {
#ifdef ARDUINO
    // Get underlying socket fd for setsockopt
    int fd = _client.fd();
    if (fd < 0) {
        DEBUG("TCPClientInterface: Could not get socket fd for configuration");
        errno = EBADF;
        return false;
    }

    // TCP_NODELAY - disable Nagle's algorithm
    int flag = 1;
    int first_error = 0;
    int nodelay_result = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    if (nodelay_result < 0) first_error = errno;

    // Enable TCP keepalive
    int keepalive_result = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
    if (keepalive_result < 0 && first_error == 0) first_error = errno;
    bool configured = nodelay_result == 0 && keepalive_result == 0;

#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    tcp_diag_socket_option(fd, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY", flag, nodelay_result);
    tcp_diag_socket_option(fd, SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE", flag, keepalive_result);
#endif

    // Keepalive parameters (may not all be available on ESP32 lwIP)
#ifdef TCP_KEEPIDLE
    int keepidle = TCP_KEEPIDLE_SEC;
    int keepidle_result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    if (keepidle_result < 0 && first_error == 0) first_error = errno;
    configured = configured && keepidle_result == 0;
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    tcp_diag_socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE, "TCP_KEEPIDLE", keepidle, keepidle_result);
#endif
#endif
#ifdef TCP_KEEPINTVL
    int keepintvl = TCP_KEEPINTVL_SEC;
    int keepintvl_result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    if (keepintvl_result < 0 && first_error == 0) first_error = errno;
    configured = configured && keepintvl_result == 0;
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    tcp_diag_socket_option(fd, IPPROTO_TCP, TCP_KEEPINTVL, "TCP_KEEPINTVL", keepintvl, keepintvl_result);
#endif
#endif
#ifdef TCP_KEEPCNT
    int keepcnt = TCP_KEEPCNT_PROBES;
    int keepcnt_result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
    if (keepcnt_result < 0 && first_error == 0) first_error = errno;
    configured = configured && keepcnt_result == 0;
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    tcp_diag_socket_option(fd, IPPROTO_TCP, TCP_KEEPCNT, "TCP_KEEPCNT", keepcnt, keepcnt_result);
#endif
#endif

    TRACE("TCPClientInterface: Socket configured with TCP_NODELAY and keepalive");
    if (!configured) errno = first_error;
    return configured;

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
    return true;
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
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    Serial.printf("T:TCP_DROP now=%lu state=%s fd=%d connected=%d avail=%d running=%d done=%d wifi=%d "
                  "free=%u largest=%u minimum=%u\n",
                  static_cast<unsigned long>(millis()), tcp_diag_state(_conn_state.load()),
                  _client.fd(), static_cast<int>(_client.connected()), _client.available(),
                  _task_running.load(), _task_done.load(), static_cast<int>(WiFi.status()),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
#endif
    INFO("TCPClientInterface: Connection lost, will attempt reconnection");
    disconnect();                     // _client.stop(), _online=false, clear buffer
    _last_connect_attempt = millis();
    _conn_state.store(DISCONNECTED);
#else
    if (_online) {
        INFO("TCPClientInterface: Connection lost, will attempt reconnection");
        disconnect();
        // Reset connect attempt timer to enforce wait before reconnection
        _last_connect_attempt = static_cast<uint32_t>(Utilities::OS::time() * 1000);
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

/*static*/ const char* TCPClientInterface::connect_stage_name(TcpConnectStage stage) {
    switch (stage) {
        case TcpConnectStage::DNS: return "DNS";
        case TcpConnectStage::SOCKET: return "SOCKET";
        case TcpConnectStage::CONNECT: return "CONNECT";
        case TcpConnectStage::SELECT_TIMEOUT: return "SELECT_TIMEOUT";
        case TcpConnectStage::SELECT_ERROR: return "SELECT_ERROR";
        case TcpConnectStage::SO_ERROR_READ: return "SO_ERROR_READ";
        case TcpConnectStage::SOCKET_ERROR: return "SO_ERROR";
        case TcpConnectStage::SOCKET_OPTIONS: return "SOCKET_OPTIONS";
        case TcpConnectStage::NONE: return "NONE";
    }
    return "UNKNOWN";
}

/*static*/ bool TCPClientInterface::local_lwip_failure(int error) {
    switch (error) {
        case ENOMEM:
        case ENOBUFS:
        case EMFILE:
        case ENFILE:
        case EADDRNOTAVAIL:
        case ENETDOWN:
        case ENETUNREACH:
            return true;
        default:
            return false;
    }
}

void TCPClientInterface::record_connect_failure(TcpConnectStage stage, int error,
                                                bool local_setup_failure) {
    if (local_setup_failure) {
        if (_consecutive_local_failures < UINT8_MAX) ++_consecutive_local_failures;
    } else {
        _consecutive_local_failures = 0;
    }
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    Serial.printf("T:TCP_FAILURE stage=%s error=%d local=%d count=%u wifi=%d free=%u largest=%u\n",
                  connect_stage_name(stage), error, static_cast<int>(local_setup_failure),
                  static_cast<unsigned>(_consecutive_local_failures), static_cast<int>(WiFi.status()),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
#endif
    WARNING("TCPClientInterface: connect failed at " + std::string(connect_stage_name(stage)) +
            ", error " + std::to_string(error));
}

void TCPClientInterface::maybe_reassociate_wifi() {
    if (_consecutive_local_failures < LOCAL_FAILURES_BEFORE_REASSOCIATE) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (_operation_active && _operation_active()) return;
    const uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (free_internal < REASSOCIATE_MIN_INTERNAL_FREE ||
        largest_internal < REASSOCIATE_MIN_LARGEST_BLOCK) return;
    const uint32_t now = millis();
    if (_last_reassociate_ms != 0 && now - _last_reassociate_ms < REASSOCIATE_COOLDOWN_MS) return;

    _last_reassociate_ms = now;
    _consecutive_local_failures = 0;
    WARNING("TCPClientInterface: repeated local setup failures; requesting bounded WiFi reassociation");
#ifdef PYXIS_TCP_LIVENESS_DIAGNOSTIC
    Serial.printf("T:TCP_REASSOCIATE now=%lu free=%u largest=%u\n",
                  static_cast<unsigned long>(now), static_cast<unsigned>(free_internal),
                  static_cast<unsigned>(largest_internal));
#endif
    WiFi.reconnect();
}

// Owns _client ONLY while connecting. When the link is down it runs the blocking
// connect() here (off the main loop); on success it publishes CONNECTED and the
// main loop takes over all socket I/O. It never touches _client while CONNECTED.
void TCPClientInterface::task_loop() {
    while (_task_running) {
        TCP_DIAG_WORKER();
        if (_conn_state.load() == DISCONNECTED) {
            uint32_t now = millis();
            if (now - _last_connect_attempt >= RECONNECT_WAIT_MS) {
                _last_connect_attempt = now;
                // connect() is bounded and runs off loopTask. Do not gate it on
                // a fixed largest-free-block threshold: the complete UI remains
                // healthy below 20 KiB, and such a gate can suppress every TCP
                // attempt forever while persisted paths still target tcp0.
                _conn_state.store(CONNECTING);      // claim _client
                if (connect()) {
                    _frame_buffer.clear();
                    _last_data_received = millis();
                    // _online is owned by the main loop (it sets it on
                    // observing CONNECTED); writing it here would race with
                    // loop()'s `_online = false` during the CONNECTING window.
                    // Publish CONNECTED BEFORE _reconnected: seq-cst then
                    // guarantees that whenever the main loop observes
                    // _reconnected==true the interface is already CONNECTED,
                    // so check_reconnected() can't fire the announce on an
                    // offline interface (which would drop it).
                    _conn_state.store(CONNECTED);   // hand _client to main loop
                    _reconnected.store(true);       // main loop announces
                } else {
                    maybe_reassociate_wifi();
                    _conn_state.store(DISCONNECTED);
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
        // Wait for the task to leave task_loop() and set _task_done — after that
        // it only calls vTaskDelete(nullptr) and never touches `this` again, so
        // it's safe to free the object. The deadline is far longer than any
        // connect()+DNS (incl. lwIP DNS retries) can take.
        uint32_t deadline = millis() + 30000;
        while (!_task_done && (int32_t)(millis() - deadline) < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!_task_done) {
            // Pathological: the task is still inside a hung connect() past the
            // deadline. Force-delete it so it cannot reference `this` after we
            // return. Safe against its own self-delete: that path sets _task_done
            // first, so reaching here means it has not self-deleted.
            vTaskDelete(_task_handle);
        }
        _task_handle = nullptr;
    }
    _conn_state.store(DISCONNECTED);
#endif
    disconnect();
}

/*virtual*/ void TCPClientInterface::loop() {
    // Drain existing complete frames before accepting more bytes. If 32 frames
    // remain after one pass, defer socket reads until later passes rather than
    // dropping valid backlog. An over-limit buffer with no complete frame is an
    // invalid/hostile partial HDLC frame and can be discarded safely.
    extract_and_process_frames();
    if (_frame_buffer.size() >= MAX_TCP_FRAME_BUFFER) {
        if (!contains_complete_hdlc_frame(_frame_buffer)) {
            WARNING("TCPClientInterface: oversized incomplete HDLC frame; discarding");
            _frame_buffer.clear();
        }
        return;
    }

#ifdef ARDUINO
    // tcp_task owns _client while (re)connecting; the main loop only touches the
    // socket once CONNECTED. read/write/frame all happen here (same low-latency
    // path as before the task split). The legacy body below is unreachable on
    // ARDUINO.
    TCP_DIAG_MAIN();
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
        size_t room = MAX_TCP_FRAME_BUFFER - _frame_buffer.size();
        size_t read_budget = std::min(MAX_TCP_BYTES_PER_LOOP, room);
        size_t bytes_read = 0;
        while (bytes_read < read_budget && _client.available() > 0) {
            int byte = _client.read();
            if (byte < 0) break;
            _frame_buffer.append(static_cast<uint8_t>(byte));
            bytes_read++;
        }
    }
    extract_and_process_frames();
    return;
#endif
    // This legacy diagnostic/reconnect body is unreachable on Arduino because
    // the Arduino path returns above, but retain it there byte-for-byte so host
    // portability does not alter the embedded image.
#ifdef ARDUINO
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
#endif
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
#ifdef ARDUINO
                // Skip reconnection if memory is too low - prevents fragmentation
                uint32_t max_block = ESP.getMaxAllocHeap();
                if (max_block < 20000) {
                    Serial.printf("[TCP] Skipping reconnect - low memory (max_block=%u)\n", max_block);
                } else {
                    DEBUG("TCPClientInterface: Attempting reconnection...");
                    connect();
                }
#else
                DEBUG("TCPClientInterface: Attempting reconnection...");
                connect();
#endif
            }
        }
        return;
    }

    // Check connection status
    // Note: ESP32 WiFiClient.connected() has known bugs where it returns false incorrectly
    // See: https://github.com/espressif/arduino-esp32/issues/1714
    // Workaround: only disconnect if connected() is false AND no data available
#ifndef ARDUINO
    // Non-blocking read
    uint8_t buf[4096];
    size_t room = MAX_TCP_FRAME_BUFFER - _frame_buffer.size();
    size_t read_budget = std::min(sizeof(buf), room);
    ssize_t len = recv(_socket, buf, read_budget, MSG_DONTWAIT);
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

    for (size_t frame_budget = 0; frame_budget < MAX_TCP_FRAMES_PER_LOOP; ++frame_budget) {
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
#ifdef ARDUINO
            Serial.printf("[HDLC] No FLAG in %d bytes, clearing\n", (int)_frame_buffer.size());
#else
            DEBUG("TCPClientInterface: No HDLC flag in buffered data; clearing");
#endif
            _frame_buffer.clear();
            break;
        }

        // Discard data before first FLAG
        if (start > 0) {
#ifdef ARDUINO
            Serial.printf("[HDLC] Discarding %d bytes before FLAG\n", start);
#else
            DEBUG("TCPClientInterface: Discarding bytes before HDLC flag");
#endif
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
#ifdef ARDUINO
            Serial.printf("[HDLC] Frame #%u: %d escaped bytes\n", frame_count, (int)frame_content.size());
#else
            DEBUG("TCPClientInterface: Received escaped HDLC frame");
#endif
        }

        // Remove processed frame from buffer (keep data after end FLAG)
        _frame_buffer = _frame_buffer.mid(end);

        // Skip empty frames (consecutive FLAGs)
        if (frame_content.size() == 0) {
#ifdef ARDUINO
            if (RNS::loglevel() >= RNS::LOG_DEBUG) Serial.printf("[HDLC] Empty frame, skipping\n");
#else
            DEBUG("TCPClientInterface: Empty HDLC frame, skipping");
#endif
            continue;
        }

        // Unescape frame
        Bytes unescaped = HDLC::unescape(frame_content);
        if (unescaped.size() == 0) {
#ifdef ARDUINO
            if (RNS::loglevel() >= RNS::LOG_DEBUG) Serial.printf("[HDLC] Unescape failed!\n");
#endif
            DEBUG("TCPClientInterface: HDLC unescape error, discarding frame");
            continue;
        }

        // Validate minimum frame size (matches Python RNS HEADER_MINSIZE check)
        if (unescaped.size() < Type::Reticulum::HEADER_MINSIZE) {
            TRACE("TCPClientInterface: Frame too small (" + std::to_string(unescaped.size()) + " bytes), discarding");
            continue;
        }

#if defined(ARDUINO) && defined(PYXIS_NOMAD_LINK_DIAGNOSTIC)
        nomad_trace_packet("RX", unescaped, static_cast<size_t>(end + 1), frame_content.size());
#endif

        // Pass to transport layer
        if (RNS::loglevel() >= RNS::LOG_DEBUG) {
#ifdef ARDUINO
            Serial.printf("[TCP] Processing frame: %d bytes\n", (int)unescaped.size());
#else
            DEBUG("TCPClientInterface: Processing unescaped HDLC frame");
#endif
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
#ifdef PYXIS_NOMAD_LINK_DIAGNOSTIC
        nomad_trace_packet("TX", data, framed.size(), written);
#endif
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

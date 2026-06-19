// LXMF Messenger for LilyGO T-Deck Plus
// Complete LXMF messaging application with LVGL UI

#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <new>  // placement new
#include <soc/rtc_cntl_reg.h>

// Reticulum
#include <microReticulum/Reticulum.h>
#include <microReticulum/Utilities/OS.h>

// Filesystem
// Was: <UniversalFileSystem.h> (pyxis-provided RNS::FileSystem wrapper).
// Post-graft: microStore ships filesystem adapters; we use LittleFS via
// -DUSTORE_USE_LITTLEFS. LittleFS replaced SPIFFS for the persistent
// path table because SPIFFS chokes on sustained writes (its GC stalls
// the FS for hundreds of ms during block erase) — under live announce
// flood, SPIFFS's flush_buffer() returns false from FileStore::put,
// surfacing as "Failed to add destination to path table" spam.
// LittleFS reuses the same partition (label "spiffs") and reformats it
// on first boot.
#include <microStore/Adapters/LittleFSFileSystem.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Interface.h>

// TCP Client Interface
#include "TCPClientInterface.h"

// LoRa Interface
#include "SX1262Interface.h"

// Auto Interface (IPv6 peer discovery)
#include "AutoInterface.h"

// BLE Mesh Interface
#include "BLEInterface.h"

// LXMF
#include <LXMF/LXMRouter.h>
#include <LXMF/MessageStore.h>
#include <LXMF/PropagationNodeManager.h>

#ifdef PYXIS_TEST_HOOKS
#include "pyxis_test_hooks.h"
#endif

// Hardware drivers
#include <Hardware/TDeck/Config.h>
#include <Hardware/TDeck/Display.h>
#include <Hardware/TDeck/Keyboard.h>
#include <Hardware/TDeck/Touch.h>
#include <Hardware/TDeck/Trackball.h>

// GPS
#include <TinyGPSPlus.h>

// UI
#include <UI/LVGL/LVGLInit.h>
#include <UI/LVGL/LVGLLock.h>
#include <UI/LXMF/UIManager.h>
#include <UI/LXMF/SettingsScreen.h>

// Audio notifications
#include "Tone.h"

// Logging
#include <microReticulum/Log.h>

// SD Card access and logging
#include <Hardware/TDeck/SDAccess.h>
#include <Hardware/TDeck/SDArchiveFileSystem.h>
#include <Hardware/TDeck/SDLogger.h>

// OTA flashing
#include <ArduinoOTA.h>

// UDP log broadcasting (POSIX socket — avoids WiFiUDP's per-packet heap allocation)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

// Memory instrumentation
#ifdef MEMORY_INSTRUMENTATION_ENABLED
#include <Instrumentation/MemoryMonitor.h>
#endif

// Boot profiling
#ifdef BOOT_PROFILING_ENABLED
#include <Instrumentation/BootProfiler.h>
#endif

// Firmware version for web flasher detection
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif
#define FIRMWARE_NAME "Pyxis"

using namespace RNS;
using namespace LXMF;
using namespace Hardware::TDeck;

// Application settings (loaded from NVS)
UI::LXMF::AppSettings app_settings;

// Global instances
Reticulum* reticulum = nullptr;
Identity* identity = nullptr;
LXMRouter* router = nullptr;
MessageStore* message_store = nullptr;
PropagationNodeManager* propagation_manager = nullptr;
UI::LXMF::UIManager* ui_manager = nullptr;
TCPClientInterface* tcp_interface_impl = nullptr;
Interface* tcp_interface = nullptr;
SX1262Interface* lora_interface_impl = nullptr;
Interface* lora_interface = nullptr;
AutoInterface* auto_interface_impl = nullptr;
Interface* auto_interface = nullptr;
BLEInterface* ble_interface_impl = nullptr;
Interface* ble_interface = nullptr;

// Timing
uint32_t last_ui_update = 0;
uint32_t last_announce = 0;
uint32_t last_sync = 0;
uint32_t last_status_check = 0;
const uint32_t STATUS_CHECK_INTERVAL = 1000;  // 1 second
const uint32_t INITIAL_SYNC_DELAY = 45000;    // 45 seconds after boot before first sync
bool initial_sync_done = false;

// Connection tracking
bool last_tcp_online = false;
bool last_lora_online = false;
bool last_wifi_connected = false;

// Pending WiFi reconnect (deferred from LVGL task to main loop)
volatile bool wifi_reconnect_pending = false;
String pending_wifi_ssid;
String pending_wifi_password;

// UDP log broadcasting (POSIX socket — no per-packet heap allocation)
// WiFiUDP::beginPacket() does new char[1460] on every call, causing severe
// heap fragmentation over time. A raw POSIX socket with sendto() avoids this.
static int udp_log_sock = -1;
static struct sockaddr_in udp_log_dest;
static bool udp_log_ready = false;

static void udp_log_init() {
    if (udp_log_sock >= 0) close(udp_log_sock);  // Re-init safe
    udp_log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_log_sock < 0) return;

    // Non-blocking so sendto() never stalls the log path
    int flags = fcntl(udp_log_sock, F_GETFL, 0);
    fcntl(udp_log_sock, F_SETFL, flags | O_NONBLOCK);

    // Multicast TTL = 1 (local network only)
    uint8_t ttl = 1;
    setsockopt(udp_log_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Bind multicast output to the WiFi station interface — without this,
    // lwIP doesn't know which interface to send multicast packets on.
    struct in_addr iface;
    iface.s_addr = (uint32_t)WiFi.localIP();
    setsockopt(udp_log_sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

    memset(&udp_log_dest, 0, sizeof(udp_log_dest));
    udp_log_dest.sin_family = AF_INET;
    udp_log_dest.sin_port = htons(9999);
    udp_log_dest.sin_addr.s_addr = inet_addr("239.0.99.99");
}

// UDP send — no locking needed.  sendto() is non-blocking (O_NONBLOCK) and
// lwIP's internal TCPIP core lock serializes concurrent calls.  Worst case
// on contention: EAGAIN/ENOMEM and the packet is dropped (acceptable for logs).
static void udp_send(const char* msg, size_t len) {
    if (udp_log_sock < 0 || !udp_log_ready || WiFi.status() != WL_CONNECTED) return;
    sendto(udp_log_sock, msg, len, 0,
           (struct sockaddr*)&udp_log_dest, sizeof(udp_log_dest));
}

// Global log function callable from any module (sends to UDP + Serial)
extern "C" void pyxis_log(const char* msg) {
    Serial.println(msg);
    if (udp_log_ready) {
        udp_send(msg, strlen(msg));
    }
}

// Forward declarations
void start_tcp_interface();
void start_auto_interface();
void on_wifi_connected();

// Screen timeout
bool screen_off = false;
uint8_t saved_brightness = 180;  // Save brightness before turning off
uint32_t screen_off_time = 0;    // millis() when screen was turned off

// Keyboard backlight timeout (5 seconds)
static const uint32_t KB_LIGHT_TIMEOUT_MS = 5000;
static uint32_t last_keypress_time = 0;
static bool kb_light_on = false;

// GPS
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);  // UART1 for GPS
bool gps_time_synced = false;

/**
 * Calculate timezone offset from longitude
 * Each 15 degrees of longitude = 1 hour offset from UTC
 * Positive = East of Greenwich (ahead of UTC)
 * Negative = West of Greenwich (behind UTC)
 */
int calculate_timezone_offset_hours(double longitude) {
    // Simple calculation: divide longitude by 15, round to nearest hour
    int offset = (int)round(longitude / 15.0);
    // Clamp to valid range (-12 to +14)
    if (offset < -12) offset = -12;
    if (offset > 14) offset = 14;
    return offset;
}

/**
 * Try to sync time from GPS
 * Returns true if successful, false if no valid fix
 */
bool sync_time_from_gps(uint32_t timeout_ms = 30000) {
    INFO("Attempting GPS time sync...");

    uint32_t start = millis();
    bool got_time = false;
    bool got_location = false;

    while (millis() - start < timeout_ms) {
        while (GPSSerial.available() > 0) {
            if (gps.encode(GPSSerial.read())) {
                // Check if we have valid date/time
                if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
                    got_time = true;
                }
                // Check if we have valid location (for timezone)
                if (gps.location.isValid()) {
                    got_location = true;
                }
                // If we have both, we can sync
                if (got_time && got_location) {
                    break;
                }
            }
        }
        if (got_time && got_location) break;
        delay(10);
    }

    if (!got_time) {
        WARNING("GPS time not available");
        return false;
    }

    // Build UTC time from GPS
    struct tm gps_time;
    gps_time.tm_year = gps.date.year() - 1900;
    gps_time.tm_mon = gps.date.month() - 1;
    gps_time.tm_mday = gps.date.day();
    gps_time.tm_hour = gps.time.hour();
    gps_time.tm_min = gps.time.minute();
    gps_time.tm_sec = gps.time.second();
    gps_time.tm_isdst = 0;  // GPS time is UTC, no DST

    // Convert to Unix timestamp (UTC)
    time_t gps_unix = mktime(&gps_time);
    // mktime assumes local time, adjust back to UTC
    // Actually, we'll set TZ to UTC first
    setenv("TZ", "UTC0", 1);
    tzset();
    gps_unix = mktime(&gps_time);

    // Set the system time
    struct timeval tv;
    tv.tv_sec = gps_unix;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    // Set timezone based on location if available
    if (got_location) {
        double longitude = gps.location.lng();
        int tz_offset = calculate_timezone_offset_hours(longitude);

        // Build POSIX TZ string (e.g., "EST5" for UTC-5)
        // Note: POSIX uses opposite sign convention!
        char tz_str[32];
        if (tz_offset >= 0) {
            snprintf(tz_str, sizeof(tz_str), "GPS%d", -tz_offset);
        } else {
            snprintf(tz_str, sizeof(tz_str), "GPS+%d", -tz_offset);
        }
        setenv("TZ", tz_str, 1);
        tzset();

        String msg = "  GPS location: " + String(gps.location.lat(), 4) + ", " + String(longitude, 4);
        INFO(msg.c_str());
        msg = "  Timezone offset: UTC" + String(tz_offset >= 0 ? "+" : "") + String(tz_offset);
        INFO(msg.c_str());
    } else {
        // No location, use default Eastern Time
        WARNING("GPS location not available, using Eastern Time");
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
        tzset();
    }

    // Set the time offset for Utilities::OS::time()
    time_t now = time(nullptr);
    uint64_t uptime_ms = millis();
    uint64_t unix_ms = (uint64_t)now * 1000;
    RNS::Utilities::OS::setTimeOffset(unix_ms - uptime_ms);

    // Display synced time
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
    String msg = "  GPS time synced: " + String(time_str);
    INFO(msg.c_str());

    gps_time_synced = true;
    return true;
}

bool try_l76k_init() {
    // Try to initialize L76K GPS module (matches LilyGo example)
    for (int attempt = 0; attempt < 3; attempt++) {
        // Stop NMEA output temporarily
        GPSSerial.write("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02\r\n");
        delay(50);

        // Drain buffer with timeout
        uint32_t timeout = millis() + 500;
        while (GPSSerial.available() && millis() < timeout) {
            GPSSerial.read();
        }
        // Note: Avoid flush() - blocks indefinitely in Arduino Core 3.x if TX can't complete
        delay(100);

        // Request version
        GPSSerial.write("$PCAS06,0*1B\r\n");
        timeout = millis() + 500;
        while (!GPSSerial.available() && millis() < timeout) {
            delay(10);
        }

        if (GPSSerial.available()) {
            String response = GPSSerial.readStringUntil('\n');
            if (response.startsWith("$GPTXT,01,01,02")) {
                INFO("  L76K GPS detected!");
                return true;
            }
        }
        delay(200);
    }
    return false;
}

void setup_gps() {
    INFO("Initializing GPS...");

    String gps_msg = "  GPS UART: ESP32 RX=" + String(Pin::GPS_RX) + ", TX=" + String(Pin::GPS_TX);
    INFO(gps_msg.c_str());

    bool gps_found = false;

    // Try u-blox at 38400 baud FIRST (T-Deck Plus default)
    // This avoids sending L76K commands that could confuse u-blox
    GPSSerial.begin(38400, SERIAL_8N1, Pin::GPS_RX, Pin::GPS_TX);
    GPSSerial.setTimeout(500);
    delay(500);  // Give GPS time to start up

    uint32_t timeout = millis() + 1000;
    while (!GPSSerial.available() && millis() < timeout) {
        delay(10);
    }

    if (GPSSerial.available()) {
        INFO("  u-blox GPS detected at 38400 baud");
        gps_found = true;
    } else {
        // Try L76K at 9600 baud
        INFO("  No data at 38400, trying L76K at 9600...");
        GPSSerial.end();
        GPSSerial.begin(9600, SERIAL_8N1, Pin::GPS_RX, Pin::GPS_TX);
        GPSSerial.setTimeout(500);
        delay(200);

        if (try_l76k_init()) {
            // L76K initialization commands
            GPSSerial.write("$PCAS04,5*1C\r\n");    // GPS + GLONASS mode
            delay(100);
            GPSSerial.write("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*02\r\n");  // Enable all NMEA
            delay(100);
            GPSSerial.write("$PCAS11,3*1E\r\n");    // Vehicle mode
            delay(100);
            gps_found = true;
            INFO("  L76K GPS initialized (GPS+GLONASS, Vehicle mode)");
        } else {
            // Last try: check for any GPS at 9600
            timeout = millis() + 1000;
            while (!GPSSerial.available() && millis() < timeout) {
                delay(10);
            }
            if (GPSSerial.available()) {
                INFO("  GPS detected at 9600 baud");
                gps_found = true;
            }
        }
    }

    // Drain buffer
    while (GPSSerial.available()) {
        GPSSerial.read();
    }

    if (!gps_found) {
        WARNING("  No GPS module detected!");
    }
}

void load_app_settings() {
    INFO("Loading application settings from NVS...");

    Preferences prefs;
    prefs.begin("settings", true);  // Read-only

    // Network
    app_settings.wifi_ssid = prefs.getString("wifi_ssid", "");
    app_settings.wifi_password = prefs.getString("wifi_pass", "");
    app_settings.tcp_host = prefs.getString("tcp_host", "sideband.connect.reticulum.network");
    app_settings.tcp_port = prefs.getUShort("tcp_port", 4965);

#ifdef PYXIS_TEST_HOOKS
    // Test mode: hard-override the TCP server so the harness on the Mac
    // can reach this T-Deck regardless of what's persisted in NVS. The
    // harness runs an rnsd with TCPServerInterface on the configured
    // address; pyxis dials it as a TCP CLIENT.
    //
    // Host:port come from env vars at build time (PYXIS_TEST_TCP_HOST,
    // PYXIS_TEST_TCP_PORT — see platformio.ini + .env.example). If
    // unset, the macros expand to empty/zero; fall back to NVS so a
    // missing env var doesn't silently brick test mode.
    {
        const char* test_host = PYXIS_TEST_TCP_HOST;
        if (test_host && test_host[0] != '\0') {
            app_settings.tcp_host = String(test_host);
        }
        const char* test_port_str = PYXIS_TEST_TCP_PORT;
        if (test_port_str && test_port_str[0] != '\0') {
            int test_port = atoi(test_port_str);
            if (test_port > 0) app_settings.tcp_port = test_port;
        }
    }
#endif

    // Identity
    app_settings.display_name = prefs.getString("disp_name", "");

    // Display
    app_settings.brightness = prefs.getUChar("brightness", 180);
    app_settings.keyboard_light = prefs.getBool("kb_light", false);
    app_settings.screen_timeout = prefs.getUShort("timeout", 60);

    // Notifications
    app_settings.notification_sound = prefs.getBool("notif_snd", true);
    app_settings.notification_volume = prefs.getUChar("notif_vol", 10);

    // Interfaces
    app_settings.tcp_enabled = prefs.getBool("tcp_en", true);
    app_settings.lora_enabled = prefs.getBool("lora_en", false);
    app_settings.lora_frequency = prefs.getFloat("lora_freq", 927.25f);
    app_settings.lora_bandwidth = prefs.getFloat("lora_bw", 50.0f);
    app_settings.lora_sf = prefs.getUChar("lora_sf", 7);
    app_settings.lora_cr = prefs.getUChar("lora_cr", 5);
    app_settings.lora_power = prefs.getChar("lora_pwr", 17);
    app_settings.auto_enabled = prefs.getBool("auto_en", false);
    app_settings.ble_enabled = prefs.getBool("ble_en", false);

    // Advanced
    app_settings.announce_interval = prefs.getULong("announce", 3600);
    app_settings.sync_interval = prefs.getULong("sync_int", 3600);  // Default 60 minutes
    app_settings.gps_time_sync = prefs.getBool("gps_sync", true);

    // Propagation
    app_settings.prop_auto_select = prefs.getBool("prop_auto", true);
    app_settings.prop_selected_node = prefs.getString("prop_node", "");
    app_settings.prop_fallback_enabled = prefs.getBool("prop_fall", true);

    prefs.end();

    // Log loaded settings (hide password)
    String msg = "  WiFi SSID: " + (app_settings.wifi_ssid.length() > 0 ? app_settings.wifi_ssid : "(not set)");
    INFO(msg.c_str());
    msg = "  TCP Server: " + app_settings.tcp_host + ":" + String(app_settings.tcp_port);
    INFO(msg.c_str());
    msg = "  Brightness: " + String(app_settings.brightness);
    INFO(msg.c_str());
}

void setup_wifi() {
    // Check if WiFi credentials are configured
    if (app_settings.wifi_ssid.length() == 0) {
        WARNING("WiFi not configured - skipping WiFi setup");
        return;
    }

    String msg = "Connecting to WiFi: " + app_settings.wifi_ssid;
    INFO(msg.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(app_settings.wifi_ssid.c_str(), app_settings.wifi_password.c_str());

    // Don't block boot waiting for WiFi association — the main loop
    // already sets up TCP and does NTP sync once WL_CONNECTED is
    // observed (search for `last_wifi_connected`). Give association
    // ~1s in case it lands fast (so we can do NTP+TCP synchronously
    // when possible), then continue.
    BOOT_PROFILE_WAIT_START("wifi_connect");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 1000) {
        delay(50);
        Serial.print(".");
    }
    Serial.println();
    BOOT_PROFILE_WAIT_END("wifi_connect");

    if (WiFi.status() == WL_CONNECTED) {
        on_wifi_connected();
    } else {
        INFO("WiFi association deferred — main-loop event handler will "
             "do NTP/OTA/UDP setup when the connect lands");
    }
}

// One-shot post-WiFi-connect setup. Runs the first time WL_CONNECTED is
// observed — either at boot (via setup_wifi) or after async association
// completes (via the periodic-status-check branch in loop()). Pulls
// NTP, OTA, and UDP logging out of setup_wifi so the boot fast-path
// (no synchronous WiFi wait) doesn't lose them.
//
// NTP sync is kicked off here (configTzTime is non-blocking on ESP32 —
// it just stores the server list and starts the SNTP task) but the
// "did it land?" polling happens incrementally in pump_ntp_sync_if_pending()
// from loop(). The earlier in-place `getLocalTime` retry loop blocked
// loopTask for up to 10 s on first WiFi-associate, stalling RNS packet
// ingestion / LXMF delivery / SX1262 RX FIFO drain — a real one-shot
// dead zone for radio traffic. Splitting kick-off from polling fixes that.
static bool _wifi_post_connect_done = false;
static bool _ntp_pending = false;
static uint32_t _ntp_start_ms = 0;
static const uint32_t NTP_TIMEOUT_MS = 10000;  // matches former retry budget

void on_wifi_connected() {
    if (_wifi_post_connect_done) return;
    _wifi_post_connect_done = true;

    INFO("WiFi connected!");
    String msg = "  IP address: " + WiFi.localIP().toString();
    INFO(msg.c_str());
    msg = "  RSSI: " + String(WiFi.RSSI()) + " dBm";
    INFO(msg.c_str());

    // Try GPS time sync first (if GPS is initialized and we haven't synced already)
    if (!gps_time_synced) {
        // Fallback to NTP if GPS didn't work
        INFO("Syncing time via NTP (GPS not available)...");

        // Use configTzTime for proper timezone handling on ESP32
        // Eastern Time: EST5EDT = UTC-5, DST starts 2nd Sunday March, ends 1st Sunday Nov
        // This call is non-blocking — ESP32's lwIP SNTP task does the
        // actual network exchange. Polling for completion happens in
        // pump_ntp_sync_if_pending() (called from loop()).
        configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
        _ntp_pending = true;
        _ntp_start_ms = millis();
    } else {
        INFO("Time already synced via GPS");
    }

    {
        // Initialize ArduinoOTA for wireless flashing
        ArduinoOTA.setHostname("pyxis-tdeck");
        ArduinoOTA.onStart([]() {
            INFO("OTA: Update starting — suspending BLE for clean WiFi");
            // Pause BLE to free the shared 2.4GHz radio for WiFi transfer
            if (ble_interface_impl) {
                ble_interface_impl->stop();
                INFO("OTA: BLE stopped");
            }
            // Disconnect TCP to reduce WiFi contention
            if (tcp_interface_impl) {
                tcp_interface_impl->stop();
                INFO("OTA: TCP stopped");
            }
        });
        ArduinoOTA.onEnd([]() { INFO("OTA: Update complete, rebooting"); });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            // Tight OTA service loop: feed WDT and yield to OTA networking
            // without returning to the heavy main loop
            esp_task_wdt_reset();
            static unsigned int last_pct = 999;
            unsigned int pct = progress * 100 / total;
            if (pct != last_pct && pct % 10 == 0) {
                last_pct = pct;
                Serial.printf("OTA: %u%%\n", pct);
            }
        });
        ArduinoOTA.onError([](ota_error_t error) { ERROR("OTA: Error"); });
        ArduinoOTA.begin();
        INFO("OTA: Ready");

        // Initialize UDP log broadcasting (multicast group 239.0.99.99:9999)
        udp_log_init();
        udp_log_ready = true;
        // Renamed upstream (microReticulum @ 0.3.0): setLogCallback -> set_log_callback.
        RNS::set_log_callback([](const char* msg, RNS::LogLevel level) {
            // Suppress noisy per-packet LoRa/transport trace lines on UDP
            // (still send to Serial for wired debugging)
            bool suppress_udp = false;
            if (level <= RNS::LOG_DEBUG) {
                // Quick prefix checks for the noisiest log sources
                if (strncmp(msg, "SX1262", 6) == 0 ||
                    strncmp(msg, "Transport::inbound", 18) == 0 ||
                    strncmp(msg, "AutoInterface:", 14) == 0 ||
                    strncmp(msg, "Packet::", 8) == 0 ||
                    strncmp(msg, "Creating packet", 15) == 0 ||
                    strncmp(msg, "Checking to see", 15) == 0 ||
                    strncmp(msg, "Caching packet", 14) == 0 ||
                    strncmp(msg, "Adding destination", 18) == 0 ||
                    strncmp(msg, "InterfaceImpl", 13) == 0 ||
                    strncmp(msg, "Identity::", 10) == 0 ||
                    strncmp(msg, "Dropped", 7) == 0) {
                    suppress_udp = true;
                }
            }
            // Serial (preserve wired debugging)
            Serial.print(RNS::getTimeString());
            Serial.print(" [");
            Serial.print(RNS::getLevelName(level));
            Serial.print("] ");
            Serial.println(msg);
            Serial.flush();
            // UDP broadcast (filtered)
            if (udp_log_ready && !suppress_udp) {
                char buf[512];
                int len = snprintf(buf, sizeof(buf), "%s [%s] %s",
                    RNS::getTimeString(), RNS::getLevelName(level), msg);
                if (len > 0) {
                    udp_send(buf, (size_t)len);
                }
            }
        });
        INFO("UDP log broadcasting on port 9999");
    }
}

// Incremental NTP-completion poller. Called once per loop() pass. Single
// non-blocking getLocalTime probe (ms=0 → tries once, returns immediately).
// When the SNTP task lands the time, snapshots it into
// RNS::Utilities::OS::setTimeOffset so microReticulum sees real wall-clock.
// Bounded by NTP_TIMEOUT_MS — after that we log + give up, matching the
// 10 s budget of the previous synchronous retry loop.
static void pump_ntp_sync_if_pending() {
    if (!_ntp_pending) return;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        // Set the time offset for Utilities::OS::time()
        time_t now = time(nullptr);
        uint64_t uptime_ms = millis();
        uint64_t unix_ms = (uint64_t)now * 1000;
        RNS::Utilities::OS::setTimeOffset(unix_ms - uptime_ms);

        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
        String msg = "  NTP time synced: " + String(time_str);
        INFO(msg.c_str());
        _ntp_pending = false;
        return;
    }

    if (millis() - _ntp_start_ms >= NTP_TIMEOUT_MS) {
        WARNING("NTP time sync failed!");
        _ntp_pending = false;
    }
}

void setup_hardware() {
    INFO("\n=== Hardware Initialization ===");

    // Initialize LittleFS for persistence.
    //
    // Pre-graft: pyxis used its own RNS::FileSystem(new UniversalFileSystem())
    // wrapper. Vanilla upstream microReticulum @ 0.3.0 deleted RNS::FileSystem
    // entirely and replaced it with microStore (an out-of-tree dep). microStore
    // ships filesystem adapters activated by build flags. We use LittleFS
    // (-DUSTORE_USE_LITTLEFS) because it tolerates sustained writes much
    // better than SPIFFS — the path table backend (microStore::BasicFileStore)
    // does several puts/sec under network load, and SPIFFS's GC stalls
    // were causing flush failures.
    //
    // Pyxis's lib/universal_filesystem/ is now dead code on this build path and
    // can be deleted once the graft lands.
    static microStore::Adapters::LittleFSFileSystem fs;
    if (!fs.init()) {
        ERROR("FileSystem mount failed!");
    } else {
        INFO("FileSystem mounted");
        RNS::Utilities::OS::register_filesystem(fs);
        INFO("Filesystem registered");
#ifdef BOOT_PROFILING_ENABLED
        RNS::Instrumentation::BootProfiler::setFilesystemReady(true);
#endif
    }

    // Initialize I2C for keyboard and touch
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);
    Wire.setClock(I2C::FREQUENCY);
    INFO("I2C initialized");

    // Note: POWER_EN already set HIGH in setup() before display splash
    INFO("Power enabled (early init)");
}

void setup_lvgl_and_ui() {
    INFO("\n=== LVGL & UI Initialization ===");

    // Initialize LVGL with all hardware drivers
    if (!UI::LVGL::LVGLInit::init()) {
        ERROR("LVGL initialization failed!");
        while (1) delay(1000);
    }

    // Match LVGL default screen background to splash color (#1D1A1E)
    // so LVGL's first render doesn't flash over the boot splash
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x1D1A1E), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    INFO("LVGL initialized");
    INFO("LVGL task start deferred until UI manager is ready "
         "(splash stays visible until first real frame)");

    // Initialize memory monitoring (if enabled)
#ifdef MEMORY_INSTRUMENTATION_ENABLED
    INFO("Initializing memory monitor...");
    if (RNS::Instrumentation::MemoryMonitor::init(30000)) {
        INFO("Memory monitor started (30s interval)");

        // Register LVGL task for stack monitoring
        TaskHandle_t lvgl_task = UI::LVGL::LVGLInit::get_task_handle();
        if (lvgl_task != nullptr) {
            RNS::Instrumentation::MemoryMonitor::registerTask(lvgl_task, "lvgl");
            INFO("  Registered LVGL task");
        }
    } else {
        WARNING("Failed to start memory monitor");
    }
#endif
}

void setup_reticulum() {
    INFO("\n=== Reticulum Initialization ===");

    // Create Reticulum instance (no auto-init)
    reticulum = new Reticulum();

    // Enable transport mode so Transport::start() initializes the path
    // store. Without this, the entire `_path_store.init()` block at
    // Transport.cpp:244 is gated out, _new_path_table.put() always
    // returns false at TypedStore::isValid(), and every announce
    // surfaces as "Failed to add destination to path table". The UI's
    // announce list reads from the path table, so on a busy network
    // (TLAN) nothing ever appears.
    //
    // Transport mode also enables relaying packets for other nodes —
    // typically a desktop-class node behavior, but acceptable on a
    // T-Deck Plus with PSRAM and LittleFS-backed path persistence.
    Reticulum::transport_enabled(true);

    // Reduce transport log verbosity — LOG_TRACE floods serial with
    // token/link/announce details that drown out audio diagnostics.
    RNS::loglevel(RNS::LOG_INFO);

    // Load or create identity using NVS (Non-Volatile Storage)
    // NVS is preserved across flashes unlike SPIFFS
    Preferences prefs;
    prefs.begin("reticulum", false);  // namespace "reticulum", read-write

    size_t key_len = prefs.getBytesLength("identity");
    INFO("Checking for identity in NVS...");
    Serial.printf("NVS identity key length: %u\n", key_len);

    if (key_len == 64) {  // Private key is 64 bytes
        INFO("Identity found in NVS, loading...");
        uint8_t key_data[64];
        prefs.getBytes("identity", key_data, 64);
        Bytes private_key(key_data, 64);
        identity = new Identity(false);  // Create without generating keys
        if (identity->load_private_key(private_key)) {
            INFO("  Identity loaded successfully from NVS");
        } else {
            ERROR("  Failed to load identity from NVS, creating new");
            identity = new Identity();
            Bytes priv_key = identity->get_private_key();
            prefs.putBytes("identity", priv_key.data(), priv_key.size());
            INFO("  New identity saved to NVS");
        }
    } else {
        INFO("No identity in NVS, creating new identity");
        identity = new Identity();
        Bytes priv_key = identity->get_private_key();
        size_t written = prefs.putBytes("identity", priv_key.data(), priv_key.size());
        Serial.printf("  Wrote %u bytes to NVS\n", written);
        INFO("  Identity saved to NVS");
    }
    prefs.end();

    std::string identity_hex = identity->get_public_key().toHex().substr(0, 16);
    std::string msg = "  Identity: " + identity_hex + "...";
    INFO(msg.c_str());

    // Add TCP client interface (if enabled and WiFi connected)
    start_tcp_interface();
    if (!tcp_interface_impl && app_settings.tcp_enabled) {
        INFO("WiFi not connected yet - TCP will start when WiFi connects");
    }

    // Add LoRa interface (if enabled)
    if (app_settings.lora_enabled) {
        INFO("Initializing LoRa interface...");

        lora_interface_impl = new SX1262Interface("LoRa");

        // Apply configuration from settings
        SX1262Config lora_config;
        lora_config.frequency = app_settings.lora_frequency;
        lora_config.bandwidth = app_settings.lora_bandwidth;
        lora_config.spreading_factor = app_settings.lora_sf;
        lora_config.coding_rate = app_settings.lora_cr;
        lora_config.tx_power = app_settings.lora_power;
        lora_interface_impl->set_config(lora_config);

        lora_interface = new Interface(lora_interface_impl);

        if (!lora_interface->start()) {
            ERROR("Failed to initialize LoRa interface!");
        } else {
            INFO("LoRa interface started");
            Transport::register_interface(*lora_interface);
        }
    } else {
        INFO("LoRa interface disabled in settings");
    }

    // Add Auto interface (if enabled). Use the idempotent helper so
    // on_wifi_connected can re-attempt if WiFi associates after this
    // boot block runs (it usually does — wifi_connect typically
    // takes 2-5s, well past where we are in boot).
    if (app_settings.auto_enabled) {
        if (WiFi.status() == WL_CONNECTED) {
            start_auto_interface();
        } else {
            WARNING("AutoInterface enabled but WiFi not connected - "
                    "will retry from on_wifi_connected");
        }
    } else {
        INFO("AutoInterface disabled in settings");
    }

    // Add BLE Mesh interface (if enabled)
    // Uses NimBLE stack by default (env:tdeck), Bluedroid available via env:tdeck-bluedroid
    // NimBLE uses ~100KB less internal RAM than Bluedroid
    if (app_settings.ble_enabled) {
        INFO("Initializing BLE Mesh interface...");

        // Allocate BLEInterface in PSRAM to save ~22KB internal heap
        // Use calloc to zero-initialize — prevents stale PSRAM data from appearing as valid
        void* ble_mem = heap_caps_calloc(1, sizeof(BLEInterface), MALLOC_CAP_SPIRAM);
        ble_interface_impl = new (ble_mem) BLEInterface("BLE");
        // Testing: DUAL mode with WiFi radio completely disabled
        ble_interface_impl->setRole(RNS::BLE::Role::DUAL);
        ble_interface_impl->setLocalIdentity(identity->get_public_key().left(16));
        // Set device name to TD-XXXXXX format (last 6 hex chars of identity) for T-Deck
        std::string ble_name = "TD-" + identity->get_public_key().toHex().substr(26, 6);
        ble_interface_impl->setDeviceName(ble_name);
        ble_interface = new Interface(ble_interface_impl);

        if (!ble_interface->start()) {
            ERROR("Failed to initialize BLE interface!");
        } else {
            INFO("BLE Mesh interface started");
            Transport::register_interface(*ble_interface);

            // Start BLE on its own FreeRTOS task (core 0, priority 1)
            // This prevents BLE operations from blocking the main loop
            if (ble_interface_impl->start_task(1, 0)) {
                INFO("BLE task started on core 0");
            } else {
                WARNING("Failed to start BLE task, will run in main loop");
            }
        }
    } else {
        INFO("BLE Mesh interface disabled in settings");
    }

    // Start Transport (initializes Transport identity and enables packet processing)
    reticulum->start();
}

void setup_lxmf() {
    INFO("\n=== LXMF Initialization ===");

    // Create message store
    message_store = new MessageStore("/lxmf");
    INFO("Message store ready");

    // Wire up the SD card as the archive tier so messages older than
    // HOT_MESSAGES_PER_CONVERSATION (50) get moved off LittleFS. This
    // is critical for sustained operation: the LittleFS partition is
    // 1.875MB and a sustained-receive soak fills it in ~30min. With
    // SD archive enabled, hot stays bounded indefinitely.
    if (Hardware::TDeck::SDAccess::is_ready()) {
        // Path on the SD card. We use "/lxmf-archive" (rather than the
        // hot path "/lxmf") so the archive is clearly distinct from
        // anything else the SD card might hold.
        static Hardware::TDeck::SDArchiveFileSystem sd_archive_fs;
        message_store->set_archive_filesystem(sd_archive_fs, "/lxmf-archive");
        INFO("Message store: SD archive enabled at /lxmf-archive");
    } else {
        WARNING("Message store: SD card not ready, archive disabled — "
                "older messages will be deleted instead of archived");
    }

    // Create LXMF router
    router = new LXMRouter(*identity, "/lxmf");
    INFO("LXMF router created");

    // Create and register propagation node manager
    propagation_manager = new PropagationNodeManager();
    Transport::register_announce_handler(HAnnounceHandler(propagation_manager));
    INFO("Propagation node manager registered");

    // Configure propagation settings
    router->set_fallback_to_propagation(app_settings.prop_fallback_enabled);
    router->set_propagation_only(app_settings.prop_only);
    if (!app_settings.prop_selected_node.isEmpty()) {
        // Use stored node (works for both manual and auto-select as initial/fallback)
        Bytes selected_node;
        selected_node.assignHex(app_settings.prop_selected_node.c_str());
        router->set_outbound_propagation_node(selected_node);
        if (app_settings.prop_auto_select) {
            INFO(("  Propagation node: auto-select (using last known: " + app_settings.prop_selected_node.substring(0, 16) + "...)").c_str());
        } else {
            INFO(("  Selected propagation node: " + app_settings.prop_selected_node.substring(0, 16) + "...").c_str());
        }
    } else {
        INFO("  Propagation node: auto-select (no cached node)");
    }
    INFO(("  Fallback to propagation: " + String(app_settings.prop_fallback_enabled ? "enabled" : "disabled")).c_str());
    INFO(("  Propagation only: " + String(app_settings.prop_only ? "enabled" : "disabled")).c_str());

    // Set display name from settings for announces
    if (!app_settings.display_name.isEmpty()) {
        router->set_display_name(app_settings.display_name.c_str());
    }

    // Only do network stuff if TCP interface exists
    if (tcp_interface) {
        // Wait for TCP connection to stabilize before announcing
        INFO("Waiting 3 seconds for TCP connection to stabilize...");
        BOOT_PROFILE_WAIT_START("tcp_stabilize");
        delay(3000);
        BOOT_PROFILE_WAIT_END("tcp_stabilize");

        // Check TCP status before announcing
        if (tcp_interface->online()) {
            INFO("TCP interface online: YES");
            // Announce delivery destination
            INFO("Sending LXMF announce...");
            router->announce();
            last_announce = millis();
        } else {
            INFO("TCP interface online: NO");
        }
    } else {
        WARNING("No TCP interface - network features disabled until WiFi configured");
    }

    std::string dest_hash = router->delivery_destination().hash().toHex();
    std::string msg = "  Delivery destination: " + dest_hash;
    INFO(msg.c_str());
}

void setup_ui_manager() {
    INFO("\n=== UI Manager Initialization ===");

    // Create UI manager
    ui_manager = new UI::LXMF::UIManager(*reticulum, *router, *message_store);

    if (!ui_manager->init()) {
        ERROR("UI manager initialization failed!");
        while (1) delay(1000);
    }

    // Set initial RNS connection status (check all interfaces)
    {
        bool tcp_online = tcp_interface && tcp_interface->online();
        bool lora_online = lora_interface && lora_interface->online();
        last_tcp_online = tcp_online;
        last_lora_online = lora_online;

        String status_str;
        if (tcp_online && lora_online) {
            status_str = "TCP+LoRa";
        } else if (tcp_online) {
            status_str = "TCP: " + app_settings.tcp_host;
        } else if (lora_online) {
            status_str = "LoRa";
        }
        ui_manager->set_rns_status(tcp_online || lora_online, status_str);
    }

    // Set propagation node manager
    if (propagation_manager) {
        ui_manager->set_propagation_node_manager(propagation_manager);
    }

    // Set LoRa interface for RSSI display
    if (lora_interface) {
        ui_manager->set_lora_interface(lora_interface);
    }

    // Set BLE interface for connection count display
    if (ble_interface) {
        ui_manager->set_ble_interface(ble_interface);
    }

    // Set GPS for satellite count display
    ui_manager->set_gps(&gps);

    // Configure settings screen
    UI::LXMF::SettingsScreen* settings = ui_manager->get_settings_screen();
    if (settings) {
        // Pass GPS for status display
        settings->set_gps(&gps);

        // Set brightness change callback (immediate)
        settings->set_brightness_change_callback([](uint8_t brightness) {
            // Apply brightness immediately via display backlight
            ledcWrite(0, brightness);  // Channel 0 is backlight on T-Deck
            INFO(("Brightness changed to " + String(brightness)).c_str());
        });

        // Set WiFi reconnect callback (deferred to main loop to avoid blocking LVGL task)
        settings->set_wifi_reconnect_callback([](const String& ssid, const String& password) {
            if (ssid.isEmpty()) {
                WARNING("WiFi reconnect skipped: SSID is empty");
                return;
            }
            pending_wifi_ssid = ssid;
            pending_wifi_password = password;
            wifi_reconnect_pending = true;
            INFO(("WiFi reconnect queued for: " + ssid).c_str());
        });

        // Set save callback (update app_settings and apply)
        settings->set_save_callback([](const UI::LXMF::AppSettings& new_settings) {
            // Check what changed
            bool wifi_settings_changed = (new_settings.wifi_ssid != app_settings.wifi_ssid) ||
                                        (new_settings.wifi_password != app_settings.wifi_password);
            bool tcp_settings_changed = (new_settings.tcp_enabled != app_settings.tcp_enabled) ||
                                       (new_settings.tcp_host != app_settings.tcp_host) ||
                                       (new_settings.tcp_port != app_settings.tcp_port);
            bool lora_settings_changed = (new_settings.lora_enabled != app_settings.lora_enabled) ||
                                        (new_settings.lora_frequency != app_settings.lora_frequency) ||
                                        (new_settings.lora_bandwidth != app_settings.lora_bandwidth) ||
                                        (new_settings.lora_sf != app_settings.lora_sf) ||
                                        (new_settings.lora_cr != app_settings.lora_cr) ||
                                        (new_settings.lora_power != app_settings.lora_power);
            bool auto_settings_changed = (new_settings.auto_enabled != app_settings.auto_enabled);
            bool ble_settings_changed = (new_settings.ble_enabled != app_settings.ble_enabled);

            app_settings = new_settings;

            // Handle WiFi credential changes - auto reconnect
            if (wifi_settings_changed && new_settings.wifi_ssid.length() > 0) {
                INFO(("WiFi credentials changed, reconnecting to: " + new_settings.wifi_ssid).c_str());
                udp_log_ready = false;  // Suspend UDP logging during WiFi transition
                WiFi.disconnect();
                delay(100);
                WiFi.begin(new_settings.wifi_ssid.c_str(), new_settings.wifi_password.c_str());

                // Wait for connection (with timeout)
                uint32_t start = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                    delay(100);
                }

                if (WiFi.status() == WL_CONNECTED) {
                    udp_log_init();  // Rebind to new WiFi interface IP
                    udp_log_ready = true;  // Resume UDP logging
                    INFO(("WiFi connected! IP: " + WiFi.localIP().toString()).c_str());
                } else {
                    WARNING("WiFi connection failed");
                }
            }

            // Update router display name
            if (router && !new_settings.display_name.isEmpty()) {
                router->set_display_name(new_settings.display_name.c_str());
            }

            // Handle TCP interface changes at runtime
            if (tcp_settings_changed) {
                if (tcp_interface_impl) {
                    INFO("Stopping TCP interface...");
                    tcp_interface_impl->stop();
                }

                if (new_settings.tcp_enabled) {
                    start_tcp_interface();
                } else {
                    INFO("TCP interface disabled");
                }
            }

            // Handle LoRa interface changes at runtime
            if (lora_settings_changed) {
                if (lora_interface_impl) {
                    INFO("Stopping LoRa interface...");
                    lora_interface_impl->stop();
                }

                if (new_settings.lora_enabled) {
                    INFO("Starting LoRa interface with new settings...");

                    // Create interface if it doesn't exist yet
                    if (!lora_interface_impl) {
                        INFO("Creating new LoRa interface...");
                        lora_interface_impl = new SX1262Interface("LoRa");
                        lora_interface = new Interface(lora_interface_impl);
                    }

                    SX1262Config lora_config;
                    lora_config.frequency = new_settings.lora_frequency;
                    lora_config.bandwidth = new_settings.lora_bandwidth;
                    lora_config.spreading_factor = new_settings.lora_sf;
                    lora_config.coding_rate = new_settings.lora_cr;
                    lora_config.tx_power = new_settings.lora_power;
                    lora_interface_impl->set_config(lora_config);

                    if (lora_interface->start()) {
                        INFO("LoRa interface started");
                        // Register with transport if not already registered
                        Transport::register_interface(*lora_interface);
                    } else {
                        ERROR("Failed to start LoRa interface!");
                    }
                } else {
                    INFO("LoRa interface disabled");
                }
            }

            // Handle Auto interface changes at runtime
            if (auto_settings_changed) {
                if (auto_interface_impl) {
                    INFO("Stopping AutoInterface...");
                    auto_interface_impl->stop();
                }

                if (new_settings.auto_enabled && WiFi.status() == WL_CONNECTED) {
                    INFO("Starting AutoInterface...");

                    // Create interface if it doesn't exist yet
                    if (!auto_interface_impl) {
                        INFO("Creating new AutoInterface...");
                        auto_interface_impl = new AutoInterface("Auto");
                        auto_interface = new Interface(auto_interface_impl);
                    }

                    if (auto_interface->start()) {
                        INFO("AutoInterface started");
                        // Register with transport if not already registered
                        Transport::register_interface(*auto_interface);
                    } else {
                        ERROR("Failed to start AutoInterface!");
                    }
                } else if (new_settings.auto_enabled) {
                    WARNING("AutoInterface enabled but WiFi not connected");
                } else {
                    INFO("AutoInterface disabled");
                }
            }

            // Handle BLE interface changes at runtime
            if (ble_settings_changed) {
                if (ble_interface_impl) {
                    INFO("Stopping BLE interface...");
                    ble_interface_impl->stop();
                }

                if (new_settings.ble_enabled) {
                    INFO("Starting BLE interface...");

                    // Create interface if it doesn't exist yet
                    if (!ble_interface_impl) {
                        INFO("Creating new BLE interface...");
                        void* ble_mem = heap_caps_calloc(1, sizeof(BLEInterface), MALLOC_CAP_SPIRAM);
                        ble_interface_impl = new (ble_mem) BLEInterface("BLE");
                        // Testing: DUAL mode with WiFi radio completely disabled
                        ble_interface_impl->setRole(RNS::BLE::Role::DUAL);
                        ble_interface_impl->setLocalIdentity(identity->get_public_key().left(16));
                        std::string ble_name = "TD-" + identity->get_public_key().toHex().substr(26, 6);
                        ble_interface_impl->setDeviceName(ble_name);
                        ble_interface = new Interface(ble_interface_impl);
                    }

                    if (ble_interface->start()) {
                        INFO("BLE interface started");
                        // Register with transport if not already registered
                        Transport::register_interface(*ble_interface);
                    } else {
                        ERROR("Failed to start BLE interface!");
                    }
                } else {
                    INFO("BLE interface disabled");
                }
            }

            // Apply propagation settings to router
            if (router) {
                router->set_fallback_to_propagation(new_settings.prop_fallback_enabled);
                router->set_propagation_only(new_settings.prop_only);

                // When auto-select is enabled, save the current effective node for next boot
                if (new_settings.prop_auto_select && propagation_manager) {
                    Bytes effective = propagation_manager->get_effective_node();
                    if (effective.size() > 0) {
                        app_settings.prop_selected_node = String(effective.toHex().c_str());
                        // Also persist to NVS
                        Preferences prefs;
                        prefs.begin("lxmf", false);
                        prefs.putString("prop_node", app_settings.prop_selected_node);
                        prefs.end();
                        INFO(("  Cached effective propagation node: " + app_settings.prop_selected_node.substring(0, 16) + "...").c_str());
                    }
                }
            }

            INFO("Settings saved");
        });
    }

    // Apply initial brightness from settings
    ledcWrite(0, app_settings.brightness);

    INFO("UI manager ready");
}

// Create and start TCP interface, register with Transport.
// Safe to call multiple times - no-op if interface already exists.
// Idempotent AutoInterface starter. Call from both boot and the
// post-WiFi handler — the boot path fires before WiFi finishes
// associating, so the boot-time gate fails and AutoInterface
// silently never starts. Mirroring start_tcp_interface()'s pattern
// makes "UI shows AutoInterface enabled" actually take effect once
// WiFi lands a few seconds later.
void start_auto_interface() {
    if (!app_settings.auto_enabled || WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!auto_interface_impl) {
        INFO("Initializing AutoInterface (IPv6 peer discovery)...");
        auto_interface_impl = new AutoInterface("Auto");
        auto_interface = new Interface(auto_interface_impl);
        if (!auto_interface->start()) {
            ERROR("Failed to initialize AutoInterface!");
        } else {
            INFO("AutoInterface started");
            Transport::register_interface(*auto_interface);
        }
    } else if (!auto_interface->online()) {
        // Interface exists but stopped (post-disconnect): restart.
        INFO("Restarting AutoInterface (was stopped)...");
        if (auto_interface->start()) {
            INFO("AutoInterface restarted");
        } else {
            ERROR("AutoInterface restart failed!");
        }
    }
}

void start_tcp_interface() {
    if (!app_settings.tcp_enabled || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!tcp_interface_impl) {
        String server_addr = app_settings.tcp_host + ":" + String(app_settings.tcp_port);
        INFO(("Creating TCP interface to " + std::string(server_addr.c_str())).c_str());

        tcp_interface_impl = new TCPClientInterface("tcp0");
        tcp_interface_impl->set_target_host(app_settings.tcp_host.c_str());
        tcp_interface_impl->set_target_port(app_settings.tcp_port);
        tcp_interface = new Interface(tcp_interface_impl);

        if (!tcp_interface->start()) {
            INFO("TCP initial connection failed, will retry in background");
        }
        Transport::register_interface(*tcp_interface);
    } else {
        // Interface exists, just update settings and restart
        INFO("Starting TCP interface...");
        tcp_interface_impl->set_target_host(app_settings.tcp_host.c_str());
        tcp_interface_impl->set_target_port(app_settings.tcp_port);
        tcp_interface_impl->start();
    }
}

void setup() {
    // Initialize serial
    Serial.begin(115200);
    delay(100);

    INFO("\n");
    INFO("╔══════════════════════════════════════╗");
    INFO("║   LXMF Messenger for T-Deck Plus    ║");
    INFO("║   Pyxis + LVGL UI                   ║");
    INFO("╚══════════════════════════════════════╝");
    INFO("");

    // Enable peripheral power rail before display init.
    // Display needs ~120ms after power-on before accepting SPI commands
    // (ST7789V power-on reset time). Without this delay, SWRESET is sent
    // to an unpowered chip and silently lost.
    pinMode(Pin::POWER_EN, OUTPUT);
    digitalWrite(Pin::POWER_EN, HIGH);
    delay(150);

    // Show boot splash ASAP — before any slow init (GPS, WiFi, SD, Reticulum).
    Hardware::TDeck::Display::init_hardware_only();

    // Capture ESP reset reason early (before WiFi) — logged after WiFi init for UDP visibility
    esp_reset_reason_t _boot_reset_reason = esp_reset_reason();

    // Check for LXST crash breadcrumb from previous boot
    {
        Preferences _dbg;
        _dbg.begin("lxst_dbg", true);
        uint8_t step = _dbg.getUChar("step", 0);
        if (step > 0) {
            uint32_t heap = _dbg.getUInt("heap", 0);
            uint32_t stack = _dbg.getUInt("stack", 0);
            char buf[80];
            snprintf(buf, sizeof(buf), "LXST CRASH: last step=%u heap=%u stack=%u", step, heap, stack);
            WARNING(buf);
        }
        _dbg.end();
        // Clear breadcrumb
        Preferences _dbg2;
        _dbg2.begin("lxst_dbg", false);
        _dbg2.putUChar("step", 0);
        _dbg2.end();
    }

    // Initialize hardware
    BOOT_PROFILE_START("hardware");
    setup_hardware();
    BOOT_PROFILE_END("hardware");

    // Initialize audio for notifications
    BOOT_PROFILE_START("audio");
    Notification::tone_init();
    BOOT_PROFILE_END("audio");

    // Load application settings from NVS (before WiFi/GPS)
    BOOT_PROFILE_START("settings");
    load_app_settings();
    BOOT_PROFILE_END("settings");

    // Initialize GPS but DON'T block boot waiting for a fix. The 15s
    // synchronous wait was 31% of boot on this hardware; GPS rarely
    // cold-starts in 15s anyway, so we'd usually just eat the full
    // timeout. Keep a small (500ms) opportunistic check in case GPS
    // already has a fix from before the boot (warm restart). After
    // boot, the main loop's per-tick gps.encode + a periodic
    // try_gps_sync retry will pick up the time the moment it lands.
    BOOT_PROFILE_START("gps");
    setup_gps();
    if (app_settings.gps_time_sync) {
        INFO("\n=== Time Synchronization ===");
        BOOT_PROFILE_WAIT_START("gps_sync");
        if (!sync_time_from_gps(500)) {  // brief warm-restart check only
            INFO("GPS time sync deferred (will retry async)");
        }
        BOOT_PROFILE_WAIT_END("gps_sync");
    } else {
        INFO("GPS time sync disabled in settings");
    }
    BOOT_PROFILE_END("gps");

    // Initialize WiFi non-blocking. Previously we'd block boot up to
    // 30s waiting for association; with a wrong password the device
    // ate the full 30s and booted broken anyway. The main loop
    // already handles "WiFi just associated" via the
    // last_wifi_connected -> wifi_connected transition (sets up TCP
    // interface and does NTP sync at that point), so blocking here
    // adds nothing except boot latency.
    BOOT_PROFILE_START("wifi");
    setup_wifi();
    BOOT_PROFILE_END("wifi");

    // Log ESP reset reason after WiFi so it reaches UDP logs
    {
        const char* reason_str = "UNKNOWN";
        switch (_boot_reset_reason) {
            case ESP_RST_POWERON:  reason_str = "POWERON"; break;
            case ESP_RST_SW:       reason_str = "SOFTWARE"; break;
            case ESP_RST_PANIC:    reason_str = "PANIC"; break;
            case ESP_RST_INT_WDT:  reason_str = "INT_WDT"; break;
            case ESP_RST_TASK_WDT: reason_str = "TASK_WDT"; break;
            case ESP_RST_WDT:      reason_str = "WDT"; break;
            case ESP_RST_DEEPSLEEP: reason_str = "DEEPSLEEP"; break;
            case ESP_RST_BROWNOUT: reason_str = "BROWNOUT"; break;
            case ESP_RST_SDIO:     reason_str = "SDIO"; break;
            default: break;
        }
        if (_boot_reset_reason != ESP_RST_POWERON) {
            WARNING("Reset reason: " + std::string(reason_str) + " (" + std::to_string((int)_boot_reset_reason) + ")");
        } else {
            INFO("Reset reason: " + std::string(reason_str));
        }
    }

    // Create shared SPI bus mutex (display, LoRa, SD card all share SPI)
    SemaphoreHandle_t spi_mutex = xSemaphoreCreateMutex();
    if (!spi_mutex) {
        ERROR("Failed to create SPI bus mutex!");
    }

    // Try SD card FIRST, before display claims pins — matches LilyGo init order.
    // Uses global SPI (FSPI) with no competing peripheral on the bus.
    BOOT_PROFILE_START("sd_card");
    if (spi_mutex) {
        if (Hardware::TDeck::SDAccess::init(spi_mutex)) {
            INFO("SD card initialized on shared SPI bus");
        } else {
            INFO("SD card not available (no card inserted?)");
        }
    }
    BOOT_PROFILE_END("sd_card");

    // Set SPI mutex on Display before init (null-safe if mutex creation failed)
    Hardware::TDeck::Display::set_spi_mutex(spi_mutex);

    // Initialize LVGL and hardware drivers
    BOOT_PROFILE_START("lvgl");
    setup_lvgl_and_ui();
    BOOT_PROFILE_END("lvgl");

    // Set SPI mutex on LoRa interface (before setup_reticulum creates it)
    if (spi_mutex) {
        SX1262Interface::set_spi_mutex(spi_mutex);
    }

    // Initialize Reticulum (includes LoRa on shared SPI bus)
    BOOT_PROFILE_START("reticulum");
    setup_reticulum();
    BOOT_PROFILE_END("reticulum");

    // Initialize SD logging (SDAccess already initialized above)
    if (Hardware::TDeck::SDAccess::is_ready()) {
        if (Hardware::TDeck::SDLogger::init()) {
            INFO("SD card logging active");
        }
    }

    // Initialize LXMF
    BOOT_PROFILE_START("lxmf");
    setup_lxmf();
    BOOT_PROFILE_END("lxmf");

    // Initialize UI manager
    BOOT_PROFILE_START("ui_manager");
    setup_ui_manager();
    BOOT_PROFILE_END("ui_manager");

    // Now that UIManager has built screens and configured the active
    // one, start the LVGL render task. Doing this any earlier means
    // the LVGL task refreshes its empty default screen on top of the
    // boot splash (visible flash to black), then later refreshes the
    // real UI. Deferring keeps the splash on-screen until the first
    // real frame.
    //
    // Core 1, priority 1 (same as loopTask — round-robin scheduling).
    // Previously priority 2, but that starved loopTask of CPU time
    // during heavy rendering, causing 30s WDT timeouts on loopTask.
    if (!UI::LVGL::LVGLInit::start_task(1, 1)) {
        ERROR("Failed to start LVGL task!");
        while (1) delay(1000);
    }
    INFO("LVGL task started on core 1");

    // Send initial LXST voice destination announce
    if (ui_manager) {
        ui_manager->announce_lxst();
    }

    // Register delivered callback to update message status in storage and UI
    router->register_delivered_callback([](LXMF::LXMessage& msg) {
        INFO(">>> APP DELIVERED CALLBACK ENTRY");
        Serial.flush();

        INFO(">>> Getting message hash");
        Serial.flush();
        RNS::Bytes msg_hash = msg.hash();
        INFO("Delivery confirmed for message: " + msg_hash.toHex().substr(0, 16) + "...");
        Serial.flush();

        // Update message state in storage
        if (message_store) {
            INFO(">>> Updating message state in store");
            Serial.flush();
            message_store->update_message_state(msg_hash, LXMF::Type::Message::DELIVERED);
            INFO(">>> State updated, loading full message");
            Serial.flush();

            // Load full message for UI update (need destination_hash)
            LXMF::LXMessage full_msg = message_store->load_message(msg_hash);
            INFO(">>> Message loaded, checking hash");
            Serial.flush();
            if (full_msg.hash()) {
                INFO(">>> Setting state on full message");
                Serial.flush();
                full_msg.state(LXMF::Type::Message::DELIVERED);
                if (ui_manager) {
                    INFO(">>> Calling UI manager on_message_delivered");
                    Serial.flush();
                    ui_manager->on_message_delivered(full_msg);
                    INFO(">>> UI manager returned");
                    Serial.flush();
                }
            }
        }
        INFO(">>> APP DELIVERED CALLBACK EXIT");
        Serial.flush();
    });

    // Boot profiling complete
    BOOT_PROFILE_COMPLETE();
#ifdef BOOT_PROFILING_ENABLED
    BOOT_PROFILE_SAVE();
#endif

    INFO("\n");
    INFO("╔══════════════════════════════════════╗");
    INFO("║     System Ready - Enjoy!            ║");
    INFO("╚══════════════════════════════════════╝");
    INFO("");

    // Reconfigure Task Watchdog with 30s timeout (default 10s is too tight
    // for SPIFFS flash I/O — identity persistence writes 40-50 entries and
    // can take 5-15s with sector erases and garbage collection)
    // Task Watchdog config:
    // - 60s timeout (was 30s; bumped to tolerate WiFi-stack busy windows
    //   on CPU0 — `pm_tx_data_done_process` in ESP-IDF's `ppTask` can
    //   starve CPU0 idle for >30s under heavy multicast/mDNS traffic)
    // - panic=false: log warnings, don't reset. The reset behavior was
    //   blocking pyxis from running long enough to debug anything else
    //   on the graft. Will revisit panic=true once the WDT culprit is
    //   tracked down — see pyxis_microReticulum_graft_spike_findings.md
    //
    // We tried `esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0))` to
    // unsubscribe just CPU0 idle, but Arduino-ESP32's prebuilt framework
    // re-adds it on the next loop iteration (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
    // is baked in and sdkconfig.defaults can't override without a framework
    // rebuild from source).
    esp_task_wdt_init(60, false);
    esp_task_wdt_add(NULL);       // Subscribe loopTask
    INFO("Task Watchdog: loopTask subscribed (60s timeout, log-only)");

    // Feed WDT during long persistence + clean_cache operations (71+ entries
    // to SPIFFS can take >30s). Upstream microReticulum @ 0.3.0 moved the
    // per-Identity yield hook to a global RNS::Utilities::OS::_on_loop
    // callback (set via set_loop_callback), invoked during long operations
    // like clean_caches, identity persistence, and the path-table flush.
    // Was: Identity::set_persist_yield_callback (fork-only).
    RNS::Utilities::OS::set_loop_callback([]() { esp_task_wdt_reset(); });

    // Show startup message
    INFO("Press any key to start messaging");
}

// Serial command buffer for web flasher detection
static String serial_cmd_buffer = "";

// Loop step tracker — helps identify which call blocks when device hangs
// Written every loop iteration, printed in 5s heap diagnostic
static volatile uint8_t loop_step = 0;

// Feed WDT and advance loop step tracker
#define LOOP_STEP(n) do { loop_step = (n); esp_task_wdt_reset(); } while(0)

#ifdef PYXIS_TEST_HOOKS
// Test-hook serial command interface for the Mac-side harness. All
// outputs are prefixed `T:OK` or `T:ERR` so the harness can parse them
// out of the regular log stream.
//
// Commands:
//   T:DEST                       — print our delivery dest hash
//   T:ID                         — print our identity hash
//   T:ANN                        — force an announce
//   T:PATHS                      — print known path destination hashes
//   T:HASPATH <hex_dest>         — query path-table membership
//   T:RECALL <hex_dest>          — print app_data hex for that dest
//   T:SEND <hex_dest> <text...>  — queue an outbound DIRECT LXMessage,
//                                  print the message hash on success
//   T:STATE <hex_msg_hash>       — print the LXMessage state if known
//   T:RX                         — print received-message count then a
//                                  one-line summary per message
//   T:SETPROP <hex> <stamp_cost> — configure outbound propagation node
//   T:SENDPROP <hex> <text>      — queue an outbound PROPAGATED message
//   T:SYNCPROP                   — request_messages_from_propagation_node
//   T:SYNCSTATE                  — print current PR_* sync state
static String hex_byte_to_string(const RNS::Bytes& b) { return String(b.toHex().c_str()); }

static RNS::Bytes parse_hex_arg(const String& hex) {
    std::string s = std::string(hex.c_str());
    RNS::Bytes b;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        char buf[3] = {s[i], s[i+1], 0};
        b << (uint8_t)strtoul(buf, nullptr, 16);
    }
    return b;
}

// Track sent messages so T:STATE can look them up. Capped circular
// buffer; oldest entries drop on overflow. Index 0 = most recent.
struct TestSentEntry { RNS::Bytes hash; LXMF::LXMessage msg; bool in_use = false; };
static const size_t TEST_SENT_RING = 16;
static TestSentEntry test_sent_ring[TEST_SENT_RING];
static size_t test_sent_head = 0;
static void test_sent_record(const LXMF::LXMessage& msg) {
    test_sent_ring[test_sent_head].hash = msg.hash();
    test_sent_ring[test_sent_head].msg = msg;
    test_sent_ring[test_sent_head].in_use = true;
    test_sent_head = (test_sent_head + 1) % TEST_SENT_RING;
}
static LXMF::LXMessage* test_sent_find(const RNS::Bytes& hash) {
    for (size_t i = 0; i < TEST_SENT_RING; ++i) {
        if (test_sent_ring[i].in_use && test_sent_ring[i].hash == hash) {
            return &test_sent_ring[i].msg;
        }
    }
    return nullptr;
}

// Track received messages so T:RX can summarize.
// test_rx_total: monotonic count of all received messages (what the
// harness reads as `count=`). test_rx_count: number of entries
// currently held in the ring (≤ TEST_RX_RING). Splitting these two
// fixes T:RX silently capping at 32 during long soak runs — the
// detailed-entry dump is still bounded by ring size, but the count
// the harness sees keeps climbing.
struct TestRxEntry { RNS::Bytes source; RNS::Bytes content; bool in_use = false; };
static const size_t TEST_RX_RING = 32;
static TestRxEntry test_rx_ring[TEST_RX_RING];
static size_t test_rx_count = 0;
static size_t test_rx_total = 0;

// Public wrapper exposed via pyxis_test_hooks.h (global scope, no
// namespace) so other TUs (eg UIManager.cpp) can record received
// messages without ADL gymnastics.
void pyxis_test_hook_record_rx(const ::LXMF::LXMessage& msg) {
    test_rx_total++;
    if (test_rx_count >= TEST_RX_RING) return;
    test_rx_ring[test_rx_count].source = msg.source_hash();
    test_rx_ring[test_rx_count].content = msg.content();
    test_rx_ring[test_rx_count].in_use = true;
    test_rx_count++;
}

static const char* test_state_name(LXMF::Type::Message::State s) {
    switch (s) {
        case LXMF::Type::Message::GENERATING: return "GENERATING";
        case LXMF::Type::Message::OUTBOUND:   return "OUTBOUND";
        case LXMF::Type::Message::SENDING:    return "SENDING";
        case LXMF::Type::Message::SENT:       return "SENT";
        case LXMF::Type::Message::DELIVERED:  return "DELIVERED";
        case LXMF::Type::Message::REJECTED:   return "REJECTED";
        case LXMF::Type::Message::CANCELLED:  return "CANCELLED";
        case LXMF::Type::Message::FAILED:     return "FAILED";
        default:                              return "UNKNOWN";
    }
}

static void handle_test_hook_command(const String& line) {
    int sep = line.indexOf(' ');
    String cmd = (sep < 0) ? line : line.substring(0, sep);
    String args = (sep < 0) ? "" : line.substring(sep + 1);

    if (cmd == "T:DEST") {
        if (!router) { Serial.println("T:ERR no router"); return; }
        Serial.println(String("T:OK ") + router->delivery_destination().hash().toHex().c_str());
    }
    else if (cmd == "T:ID") {
        Serial.println(String("T:OK ") + identity->hash().toHex().c_str());
    }
    else if (cmd == "T:ANN") {
        if (!router) { Serial.println("T:ERR no router"); return; }
        router->announce();
        Serial.println("T:OK announced");
    }
    else if (cmd == "T:ANNLXST") {
        // T:ANNLXST — force a fresh announce of the lxst.telephony
        // destination. Required before pyxis-as-callee tests because
        // the TCP-reconnect path at main.cpp:963 only announces LXMF;
        // a brand-new boot ends up with the LXST destination absent
        // from rnsd's cache, so the bot can resolve a path but the
        // path doesn't actually route to pyxis.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        ui_manager->announce_lxst();
        Serial.println("T:OK announced");
    }
    else if (cmd == "T:PATHS") {
        const auto& path_table = RNS::Transport::path_table();
        Serial.print("T:OK count=");
        Serial.println(String((unsigned)path_table.size()));
        for (const auto& kv : path_table) {
            Serial.print("T:PATH ");
            Serial.println(kv.first.toHex().c_str());
        }
    }
    else if (cmd == "T:HASPATH") {
        RNS::Bytes dest = parse_hex_arg(args);
        if (dest.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        bool has = RNS::Transport::has_path(dest);
        // Diagnostic: also dump whether the in-memory _path_table has it,
        // and the size of each store. They should match when the dual-
        // write fix is working.
        const auto& mem_table = RNS::Transport::path_table();
        bool mem_has = (mem_table.find(dest) != mem_table.end());
        Serial.print("T:OK ");
        Serial.print(has ? "1" : "0");
        Serial.print(" mem=");
        Serial.print(mem_has ? "1" : "0");
        Serial.print(" mem_count=");
        Serial.println(String((unsigned)mem_table.size()));
    }
    else if (cmd == "T:RECALL") {
        RNS::Bytes dest = parse_hex_arg(args);
        if (dest.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        RNS::Bytes app = RNS::Identity::recall_app_data(dest);
        Serial.println(String("T:OK size=") + String((unsigned)app.size())
                       + " hex=" + app.toHex().c_str());
    }
    else if (cmd == "T:HASIDENTITY") {
        // T:HASIDENTITY <hex_dest> — boolean check whether pyxis has
        // an identity cached for this destination hash. Distinct from
        // T:RECALL which only inspects app_data (and "size=0" is
        // ambiguous between "unknown" and "known with empty app_data").
        // Required for harness pre-call wait — the announce_handler
        // populates _known_destinations slightly after path_store, and
        // T:HASPATH succeeding doesn't imply Identity::recall will.
        RNS::Bytes dest = parse_hex_arg(args);
        if (dest.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        RNS::Identity ident = RNS::Identity::recall(dest);
        Serial.println(String("T:OK ") + (ident ? "1" : "0"));
    }
    else if (cmd == "T:SEND" || cmd == "T:SENDOPP") {
        if (!router) { Serial.println("T:ERR no router"); return; }
        int sp = args.indexOf(' ');
        if (sp < 0) { Serial.println("T:ERR usage <cmd> <hex> <text>"); return; }
        String hex = args.substring(0, sp);
        String text = args.substring(sp + 1);
        RNS::Bytes dest_hash = parse_hex_arg(hex);
        if (dest_hash.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        RNS::Identity dest_identity = RNS::Identity::recall(dest_hash);
        RNS::Destination destination(RNS::Type::NONE);
        if (dest_identity) {
            destination = RNS::Destination(dest_identity, RNS::Type::Destination::OUT,
                                           RNS::Type::Destination::SINGLE,
                                           "lxmf", "delivery");
        }
        RNS::Bytes content_b((const uint8_t*)text.c_str(), text.length());
        RNS::Bytes title_b;
        LXMF::Type::Message::Method method = (cmd == "T:SENDOPP")
            ? LXMF::Type::Message::OPPORTUNISTIC
            : LXMF::Type::Message::DIRECT;
        LXMF::LXMessage msg(destination, router->delivery_destination(),
                            content_b, title_b, method);
        if (!dest_identity) msg.destination_hash(dest_hash);
        msg.pack();
        router->handle_outbound(msg);
        test_sent_record(msg);
        Serial.println(String("T:OK hash=") + msg.hash().toHex().c_str()
                       + " state=" + test_state_name(msg.state())
                       + " method=" + (method == LXMF::Type::Message::OPPORTUNISTIC
                                       ? "OPPORTUNISTIC" : "DIRECT"));
    }
    else if (cmd == "T:STATE") {
        RNS::Bytes hash = parse_hex_arg(args);
        LXMF::LXMessage* m = test_sent_find(hash);
        if (!m) { Serial.println("T:ERR not found"); return; }
        Serial.println(String("T:OK state=") + test_state_name(m->state()));
    }
    else if (cmd == "T:RX") {
        // count=<total received since boot/clear> — keeps climbing past
        // TEST_RX_RING. T:RXMSG dump is still capped to ring contents.
        Serial.print("T:OK count=");
        Serial.println(String((unsigned)test_rx_total));
        for (size_t i = 0; i < test_rx_count; ++i) {
            const auto& e = test_rx_ring[i];
            std::string c((const char*)e.content.data(), e.content.size());
            Serial.print("T:RXMSG src=");
            Serial.print(e.source.toHex().c_str());
            Serial.print(" content=");
            Serial.println(c.c_str());
        }
    }
    else if (cmd == "T:RXCLR") {
        test_rx_count = 0;
        test_rx_total = 0;
        Serial.println("T:OK cleared");
    }
    else if (cmd == "T:SETPROP") {
        // T:SETPROP <hex_dest> <stamp_cost> — configure outbound propagation node.
        if (!router) { Serial.println("T:ERR no router"); return; }
        int sp = args.indexOf(' ');
        String hex = (sp < 0) ? args : args.substring(0, sp);
        int stamp_cost = (sp < 0) ? 0 : args.substring(sp + 1).toInt();
        RNS::Bytes node_hash = parse_hex_arg(hex);
        if (node_hash.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        router->set_outbound_propagation_node(node_hash);
        router->set_outbound_propagation_stamp_cost((uint8_t)stamp_cost);
        Serial.println(String("T:OK pn=") + hex + " cost=" + String(stamp_cost));
    }
    else if (cmd == "T:SENDPROP") {
        // T:SENDPROP <hex_dest> <text> — send PROPAGATED via the
        // currently-configured outbound propagation node.
        if (!router) { Serial.println("T:ERR no router"); return; }
        int sp = args.indexOf(' ');
        if (sp < 0) { Serial.println("T:ERR usage T:SENDPROP <hex> <text>"); return; }
        String hex = args.substring(0, sp);
        String text = args.substring(sp + 1);
        RNS::Bytes dest_hash = parse_hex_arg(hex);
        if (dest_hash.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        RNS::Identity dest_identity = RNS::Identity::recall(dest_hash);
        RNS::Destination destination(RNS::Type::NONE);
        if (dest_identity) {
            destination = RNS::Destination(dest_identity, RNS::Type::Destination::OUT,
                                           RNS::Type::Destination::SINGLE,
                                           "lxmf", "delivery");
        }
        RNS::Bytes content_b((const uint8_t*)text.c_str(), text.length());
        RNS::Bytes title_b;
        LXMF::LXMessage msg(destination, router->delivery_destination(),
                            content_b, title_b, LXMF::Type::Message::PROPAGATED);
        if (!dest_identity) msg.destination_hash(dest_hash);
        msg.pack();
        router->handle_outbound(msg);
        test_sent_record(msg);
        Serial.println(String("T:OK hash=") + msg.hash().toHex().c_str()
                       + " state=" + test_state_name(msg.state())
                       + " method=PROPAGATED");
    }
    else if (cmd == "T:SYNCPROP") {
        // T:SYNCPROP — kick off a sync from the configured propagation
        // node. State machine progresses asynchronously; the harness
        // can poll T:SYNCSTATE to track progress.
        if (!router) { Serial.println("T:ERR no router"); return; }
        router->request_messages_from_propagation_node();
        Serial.println("T:OK sync_requested");
    }
    else if (cmd == "T:SYNCSTATE") {
        // T:SYNCSTATE — return the current PR_* state of the prop sync FSM.
        if (!router) { Serial.println("T:ERR no router"); return; }
        Serial.print("T:OK state=");
        Serial.println(String((unsigned)router->get_sync_state()));
    }
    else if (cmd == "T:CALL") {
        // T:CALL <hex_dest> — initiate an outgoing LXST voice call.
        // The state machine progresses asynchronously; harness should
        // poll T:CALL_STATE for IDLE → ... → ACTIVE transitions.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        RNS::Bytes dest_hash = parse_hex_arg(args);
        if (dest_hash.size() != 16) { Serial.println("T:ERR bad hex"); return; }
        ui_manager->test_call_initiate(dest_hash);
        Serial.println(String("T:OK calling=") + args);
    }
    else if (cmd == "T:CALL_STATE") {
        // T:CALL_STATE — print the current call FSM state name.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        Serial.print("T:OK state=");
        Serial.println(ui_manager->test_call_state_name());
    }
    else if (cmd == "T:CALL_HANGUP") {
        // T:CALL_HANGUP — tear down the active call.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        ui_manager->test_call_hangup();
        Serial.println("T:OK hung_up");
    }
    else if (cmd == "T:CALL_ANSWER") {
        // T:CALL_ANSWER — accept an incoming ring. Only valid when state
        // is INCOMING_RINGING. Used by the harness for pyxis-as-callee
        // interop tests against real LXST.Telephony.Telephone clients.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        if (!ui_manager->test_call_answer()) {
            Serial.println("T:ERR not_ringing");
            return;
        }
        Serial.println("T:OK answered");
    }
    else if (cmd == "T:BLE") {
        // T:BLE on|off — toggle the BLE Mesh interface at runtime + persist
        // to NVS. Used by the harness to bring BLE up for cross-device
        // tests against Android Columba. Mirrors the SettingsScreen save
        // path so the change survives a reboot.
        bool want_on = (args == "on" || args == "1" || args == "true");
        bool want_off = (args == "off" || args == "0" || args == "false");
        if (!want_on && !want_off) {
            // No arg → query current state
            Serial.println(String("T:OK ble_enabled=") + (app_settings.ble_enabled ? "1" : "0"));
            return;
        }
        Preferences prefs;
        // Boot loads ble_en from NVS namespace "settings" — must match here
        // or the change won't survive a reboot.
        prefs.begin("settings", false);
        prefs.putBool("ble_en", want_on);
        prefs.end();
        app_settings.ble_enabled = want_on;
        if (want_on && !ble_interface_impl) {
            INFO("T:BLE on — creating BLE interface");
            void* ble_mem = heap_caps_calloc(1, sizeof(BLEInterface), MALLOC_CAP_SPIRAM);
            ble_interface_impl = new (ble_mem) BLEInterface("BLE");
            ble_interface_impl->setRole(RNS::BLE::Role::DUAL);
            ble_interface_impl->setLocalIdentity(identity->get_public_key().left(16));
            std::string ble_name = "TD-" + identity->get_public_key().toHex().substr(26, 6);
            ble_interface_impl->setDeviceName(ble_name);
            ble_interface = new Interface(ble_interface_impl);
            if (ble_interface->start()) {
                Transport::register_interface(*ble_interface);
                ble_interface_impl->start_task(1, 0);
                Serial.println("T:OK ble_enabled=1 started");
            } else {
                Serial.println("T:ERR ble_start_failed");
            }
        } else if (want_on) {
            // Already exists, just restart
            if (ble_interface->start()) {
                Serial.println("T:OK ble_enabled=1 restarted");
            } else {
                Serial.println("T:ERR ble_restart_failed");
            }
        } else if (ble_interface_impl) {
            // want_off
            ble_interface_impl->stop();
            Serial.println("T:OK ble_enabled=0 stopped");
        } else {
            Serial.println("T:OK ble_enabled=0");
        }
    }
    else if (cmd == "T:LXSTDEST") {
        // T:LXSTDEST — pyxis's lxst.telephony destination hash. Used
        // by the harness to set up pyxis-as-callee tests (the bot
        // dials this hash). Returns "T:ERR not_ready" if the
        // destination hasn't been registered yet (early boot).
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        std::string h = ui_manager->test_lxst_dest_hex();
        if (h.empty()) { Serial.println("T:ERR not_ready"); return; }
        Serial.println(String("T:OK ") + h.c_str());
    }
    else if (cmd == "T:CALL_STATS") {
        // T:CALL_STATS — return audio frame counters for the most recent
        // call. tx = frames sent over the wire (encoded by capture path),
        // rx = frames received and queued for playback (decoded). Both
        // are reset on call_initiate.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        Serial.print("T:OK tx=");
        Serial.print((unsigned long)ui_manager->test_call_audio_tx_count());
        Serial.print(" rx=");
        Serial.print((unsigned long)ui_manager->test_call_audio_rx_count());
        Serial.print(" state=");
        Serial.println(ui_manager->test_call_state_name());
    }
    else if (cmd == "T:CALL_QOS") {
        // T:CALL_QOS — wire-level audio fidelity counters from the
        // playback decode path. decode_ok = frames Codec2 successfully
        // decoded into PCM; decode_fail = frames it rejected (bad mode
        // header, corrupt subframe, internal codec error). pcm_n /
        // pcm_ss = sample count + cumulative sum-of-squares the harness
        // divides into RMS for content-level validation. With a peer
        // injecting a 1kHz sine at peak P the expected RMS = P/√2.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        Serial.print("T:OK decode_ok=");
        Serial.print((unsigned long)ui_manager->test_call_decode_ok());
        Serial.print(" decode_fail=");
        Serial.print((unsigned long)ui_manager->test_call_decode_fail());
        Serial.print(" pcm_n=");
        Serial.print((unsigned long)ui_manager->test_call_pcm_sample_count());
        Serial.print(" pcm_ss=");
        Serial.print((unsigned long long)ui_manager->test_call_pcm_sum_squares());
        Serial.print(" state=");
        Serial.println(ui_manager->test_call_state_name());
    }
    else if (cmd == "T:CALL_PROFILE") {
        // T:CALL_PROFILE [hex] — get/set pyxis's preferred Codec2 profile.
        // No arg: print current. With arg: set.
        // Valid: 0x10 (ULBW/700C), 0x20 (VLBW/1600), 0x30 (LBW/3200).
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        if (args.length() == 0) {
            int p = ui_manager->test_call_get_profile();
            Serial.print("T:OK profile=0x");
            if (p < 16) Serial.print("0");
            Serial.println(String(p, HEX));
            return;
        }
        int profile = (int)strtol(args.c_str(), nullptr, 0);
        if (!ui_manager->test_call_set_profile(profile)) {
            Serial.println("T:ERR unknown profile");
            return;
        }
        Serial.print("T:OK profile=0x");
        if (profile < 16) Serial.print("0");
        Serial.println(String(profile, HEX));
    }
    else if (cmd == "T:SHOW") {
        // T:SHOW <name> — switch the UI to a named screen. Used by
        // scripts/screenshot.py --all to drive a full doc capture.
        // Names match UIManager's show_* methods; chat/qr/call need
        // additional state (peer hash / identity / active call) and
        // are not exposed here.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        if (args == "conversation_list" || args == "home") {
            ui_manager->show_conversation_list();
        } else if (args == "compose") {
            ui_manager->show_compose();
        } else if (args == "announces") {
            ui_manager->show_announces();
        } else if (args == "status") {
            ui_manager->show_status();
        } else if (args == "settings") {
            ui_manager->show_settings();
        } else if (args == "propagation_nodes") {
            ui_manager->show_propagation_nodes();
        } else {
            Serial.print("T:ERR unknown screen ");
            Serial.println(args);
            return;
        }
        Serial.print("T:OK shown ");
        Serial.println(args);
    }
    else if (cmd == "T:SCREENSHOT") {
        // T:SCREENSHOT — capture the active LVGL screen as RGB565 and
        // dump base64 over USB-CDC. Decoder is scripts/screenshot.py.
        //
        // Wire format:
        //   T:SCREENSHOT BEGIN W=<w> H=<h> FMT=rgb565<be|le> BYTES=<n>
        //   <base64 line, 76 chars max>
        //   <base64 line>
        //   ...
        //   T:SCREENSHOT END
        //
        // The host script reads until "T:SCREENSHOT END", concatenates
        // all lines between the markers, base64-decodes, and converts
        // to PNG. FMT carries the byte order (`be` when LV_COLOR_16_SWAP
        // is on, `le` otherwise) so the decoder doesn't have to guess.
        // Hold the LVGL lock ONLY for the snapshot: lv_snapshot_take() copies
        // the screen pixels into a freshly-allocated buffer, so live LVGL state
        // isn't touched during the ~18s base64 serial dump below. Holding the
        // recursive mutex across the whole dump blocks the LVGL render task and
        // trips the 5s LVGLLock timeout assert (LVGLLock.h) in debug builds.
        lv_img_dsc_t* snap = nullptr;
        {
            LVGL_LOCK();
            lv_obj_t* scr = lv_scr_act();
            if (!scr) {
                Serial.println("T:ERR no active screen");
                return;
            }
            snap = lv_snapshot_take(scr, LV_IMG_CF_TRUE_COLOR);
        }
        if (!snap || !snap->data) {
            if (snap) { LVGL_LOCK(); lv_snapshot_free(snap); }
            Serial.println("T:ERR snapshot failed (PSRAM exhausted?)");
            return;
        }
        const uint16_t w = snap->header.w;
        const uint16_t h = snap->header.h;
        const uint32_t bytes = snap->data_size;
#if LV_COLOR_16_SWAP
        const char* fmt = "rgb565be";
#else
        const char* fmt = "rgb565le";
#endif
        Serial.print("T:SCREENSHOT BEGIN W=");
        Serial.print(w);
        Serial.print(" H=");
        Serial.print(h);
        Serial.print(" FMT=");
        Serial.print(fmt);
        Serial.print(" BYTES=");
        Serial.println(bytes);

        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint8_t* p = snap->data;
        uint32_t remaining = bytes;
        char line[80];
        size_t line_len = 0;
        // Encode in 3-byte → 4-char groups; flush at 76-char boundary
        // so the host script can read line-by-line.
        while (remaining >= 3) {
            uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            line[line_len++] = b64[(v >> 18) & 0x3f];
            line[line_len++] = b64[(v >> 12) & 0x3f];
            line[line_len++] = b64[(v >> 6) & 0x3f];
            line[line_len++] = b64[v & 0x3f];
            p += 3;
            remaining -= 3;
            if (line_len >= 76) {
                line[line_len] = '\0';
                Serial.println(line);
                line_len = 0;
            }
        }
        if (remaining > 0) {
            uint32_t v = (uint32_t)p[0] << 16;
            if (remaining == 2) v |= (uint32_t)p[1] << 8;
            line[line_len++] = b64[(v >> 18) & 0x3f];
            line[line_len++] = b64[(v >> 12) & 0x3f];
            line[line_len++] = (remaining == 2) ? b64[(v >> 6) & 0x3f] : '=';
            line[line_len++] = '=';
        }
        if (line_len > 0) {
            line[line_len] = '\0';
            Serial.println(line);
        }
        Serial.println("T:SCREENSHOT END");
        { LVGL_LOCK(); lv_snapshot_free(snap); }
    }
    else if (cmd == "T:CALL_INJECT") {
        // T:CALL_INJECT <on|off> [freq_hz] [amp_pct]
        // Replace mic capture with a synthesized sine wave for the
        // active call (bypasses ES7210 + voice filters). The bot
        // decodes pyxis's audio packets and computes RMS over the
        // decoded PCM — should match the expected sine energy.
        if (!ui_manager) { Serial.println("T:ERR no ui_manager"); return; }
        int sp = args.indexOf(' ');
        String on_off = (sp < 0) ? args : args.substring(0, sp);
        bool enabled = (on_off == "on" || on_off == "1" || on_off == "true");
        int freq = 1000;
        float amp = 0.5f;
        if (sp >= 0) {
            String rest = args.substring(sp + 1);
            int sp2 = rest.indexOf(' ');
            if (sp2 < 0) {
                freq = rest.toInt();
            } else {
                freq = rest.substring(0, sp2).toInt();
                amp = rest.substring(sp2 + 1).toFloat();
            }
            if (freq <= 0) freq = 1000;
            if (amp <= 0.f || amp > 1.f) amp = 0.5f;
        }
        ui_manager->test_call_set_inject_sine(enabled, freq, amp);
        Serial.print("T:OK inject=");
        Serial.print(enabled ? "on" : "off");
        Serial.print(" freq=");
        Serial.print(freq);
        Serial.print(" amp=");
        Serial.println(amp, 3);
    }
    else {
        Serial.print("T:ERR unknown cmd ");
        Serial.println(cmd);
    }
}
#endif // PYXIS_TEST_HOOKS

void loop() {
    esp_task_wdt_reset();

    // Handle OTA updates (must be called frequently)
    ArduinoOTA.handle();

    // Handle serial commands for web flasher detection + (under
    // PYXIS_TEST_HOOKS) the harness command interface. Buffer up to
    // 1024 chars so long T:SEND payloads work.
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            serial_cmd_buffer.trim();
            if (serial_cmd_buffer == "VERSION") {
                Serial.println(String(FIRMWARE_NAME) + " v" + FIRMWARE_VERSION);
            } else if (serial_cmd_buffer == "BOOTLOADER") {
                Serial.println("ENTERING_BOOTLOADER");
                Serial.flush();
                delay(100);
                // Set RTC flag to force download mode on next reset
                REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_restart();
            }
#ifdef PYXIS_TEST_HOOKS
            else if (serial_cmd_buffer.startsWith("T:")) {
                handle_test_hook_command(serial_cmd_buffer);
            }
#endif
            serial_cmd_buffer = "";
        } else if (serial_cmd_buffer.length() < 1024) {
            serial_cmd_buffer += c;
        }
    }

    LOOP_STEP(1);  // LVGL task_handler
    // Handle LVGL rendering (must be called frequently for smooth UI)
    UI::LVGL::LVGLInit::task_handler();

    LOOP_STEP(2);  // Display health
    // Monitor display health
    Hardware::TDeck::Display::log_health();

    // Handle deferred WiFi reconnect (from LVGL task)
    LOOP_STEP(3);  // WiFi reconnect check
    if (wifi_reconnect_pending) {
        wifi_reconnect_pending = false;
        INFO(("Reconnecting WiFi to: " + pending_wifi_ssid).c_str());
        udp_log_ready = false;  // Suspend UDP logging during WiFi transition
        WiFi.disconnect();
        delay(100);
        WiFi.begin(pending_wifi_ssid.c_str(), pending_wifi_password.c_str());

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            esp_task_wdt_reset();
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            udp_log_init();  // Rebind to new WiFi interface IP
            udp_log_ready = true;  // Resume UDP logging
            INFO(("WiFi connected! IP: " + WiFi.localIP().toString()).c_str());
        } else {
            WARNING("WiFi reconnection failed");
        }
        pending_wifi_ssid = "";
        pending_wifi_password = "";
    }

    // Process Reticulum
    LOOP_STEP(4);  // reticulum->loop()
    reticulum->loop();

    // Pump TX audio immediately after Reticulum — low-latency path that
    // bypasses LVGL lock and all other loop steps.  No-ops when not in a call.
    if (ui_manager) {
        ui_manager->pump_call_tx();
    }

    // Periodically persist identity/transport data (display names, paths, etc.)
    // NOTE: Persistence writes 40-50 entries via microStore (which routes
    // through the new microStore::FileSystem to SPIFFS or whichever backend
    // is configured). Sector erases (100ms each) can stretch the call to
    // 5-15s; the OS::set_loop_callback above feeds the WDT between entries.
    //
    // Upstream microReticulum @ 0.3.0 unified persistence into a single
    // Reticulum::should_persist_data() entry point — the fork had a
    // separate Identity::should_persist_data() for a 5s fast-flush of known
    // destinations. That fast cadence is folded into microStore's dirty-
    // tracking; the explicit Identity::should_persist_data() call has been
    // dropped here. (If we observe excessive lost-known-destinations after
    // crashes, revisit microStore's flush cadence rather than re-adding
    // the fork-only Identity API.)
    LOOP_STEP(5);  // persist data
    reticulum->should_persist_data();
    esp_task_wdt_reset();

    // Process TCP interface
    LOOP_STEP(6);  // TCP loop
    if (tcp_interface) {
        tcp_interface->loop();
    }

    // Process LoRa interface
    LOOP_STEP(7);  // LoRa loop
    if (lora_interface) {
        lora_interface->loop();
    }

    // Process BLE interface (skip if running on its own task)
    LOOP_STEP(8);  // BLE loop
    if (ble_interface && ble_interface_impl && !ble_interface_impl->is_task_running()) {
        ble_interface->loop();
    }

    // Process LXMF router queues
    LOOP_STEP(9);  // Router processing
    if (router) {
        router->process_outbound();
        router->process_inbound();
        router->process_sync();
    }

    // Update UI manager (processes LXMF messages)
    LOOP_STEP(10);  // UI manager update
    if (ui_manager) {
        ui_manager->update();
    }

    LOOP_STEP(11);  // Memory monitor
    // Process deferred memory monitor logging (flag set by timer callback)
    MEMORY_MONITOR_POLL();

    // Periodic announce (using interval from settings)
    LOOP_STEP(12);  // Periodic tasks
    if (app_settings.announce_interval > 0) {  // 0 = disabled
        uint32_t announce_interval_ms = app_settings.announce_interval * 1000;
        if (millis() - last_announce > announce_interval_ms) {
            // Announce if any interface is online
            bool has_online_interface = (tcp_interface && tcp_interface->online()) ||
                                        (lora_interface && lora_interface->online()) ||
                                        (ble_interface && ble_interface->online());
            if (router && has_online_interface) {
                router->announce();
                if (ui_manager) {
                    ui_manager->announce_lxst();
                }
                last_announce = millis();
                INFO("Periodic announce sent (interval: " + std::to_string(app_settings.announce_interval) + "s)");
            }
        }
    }

    // Periodic propagation sync (fetch messages from prop node)
    if (app_settings.sync_interval > 0 && router) {  // 0 = disabled
        bool should_sync = false;
        uint32_t now = millis();

        // Initial sync after boot delay
        if (!initial_sync_done && now > INITIAL_SYNC_DELAY) {
            should_sync = true;
            initial_sync_done = true;
            INFO("Initial propagation sync after boot delay");
        }
        // Periodic sync
        else if (initial_sync_done) {
            uint32_t sync_interval_ms = app_settings.sync_interval * 1000;
            if (now - last_sync > sync_interval_ms) {
                should_sync = true;
            }
        }

        if (should_sync) {
            // Only sync if TCP is online (propagation nodes need network)
            bool tcp_online = tcp_interface && tcp_interface->online();
            if (tcp_online) {
                router->request_messages_from_propagation_node();
                last_sync = now;
                INFO("Periodic propagation sync (interval: " + std::to_string(app_settings.sync_interval / 60) + " min)");
            }
        }
    }

    // Check for TCP reconnection (handles rapid disconnect/reconnect)
    if (tcp_interface_impl && tcp_interface_impl->check_reconnected()) {
        INFO("TCP interface reconnected - sending announce");
        if (router) {
            delay(500);  // Brief stabilization delay
            router->announce();
            last_announce = millis();
        }
        last_tcp_online = true;
    }

    // Drain any pending NTP probe (non-blocking, no-op when not pending).
    // on_wifi_connected kicks NTP off without blocking; this polls each
    // pass until the SNTP task lands the time or NTP_TIMEOUT_MS elapses.
    pump_ntp_sync_if_pending();

    // Periodic RNS status check (check all interfaces)
    if (millis() - last_status_check > STATUS_CHECK_INTERVAL) {
        last_status_check = millis();

        // Start TCP interface + run one-shot NTP/OTA/UDP setup when
        // WiFi becomes available. on_wifi_connected is idempotent
        // (guarded by _wifi_post_connect_done) so it's safe whether
        // the connect happened during boot or here later.
        bool wifi_connected = (WiFi.status() == WL_CONNECTED);
        if (wifi_connected && !last_wifi_connected) {
            INFO("WiFi connected (post-boot) — running on_wifi_connected");
            on_wifi_connected();
            if (!tcp_interface_impl && app_settings.tcp_enabled) {
                start_tcp_interface();
            }
            // AutoInterface init at boot fails its WiFi-connected gate
            // because WiFi typically associates 2-5s after the boot
            // block runs. Retry here once WiFi actually lands. Also handles
            // reconnect: the old `!auto_interface_impl` guard meant this only
            // ran on the first connect, so after a WiFi drop AutoInterface kept
            // stale multicast sockets and peers never rediscovered until reboot.
            // start_auto_interface() is idempotent — its else-if(!online())
            // branch rebinds the sockets on a reconnect.
            if (app_settings.auto_enabled) {
                start_auto_interface();
            }
        }
        last_wifi_connected = wifi_connected;

        bool tcp_online = tcp_interface && tcp_interface->online();
        bool lora_online = lora_interface && lora_interface->online();

        // Check if status changed
        if (tcp_online != last_tcp_online || lora_online != last_lora_online) {
            last_tcp_online = tcp_online;
            last_lora_online = lora_online;

            String status_str;
            if (tcp_online && lora_online) {
                status_str = "TCP+LoRa";
            } else if (tcp_online) {
                status_str = "TCP: " + app_settings.tcp_host;
            } else if (lora_online) {
                status_str = "LoRa";
            }

            if (ui_manager) {
                ui_manager->set_rns_status(tcp_online || lora_online, status_str);
            }

            if (!tcp_online && !lora_online) {
                WARNING("All RNS interfaces offline");
            }
        }

        // Update BLE peer info on status screen (every 3 seconds)
        static uint32_t last_ble_update = 0;
        if (millis() - last_ble_update > 3000) {
            last_ble_update = millis();
            if (ble_interface_impl && ui_manager && ui_manager->get_status_screen()) {
                BLEInterface::PeerSummary peers[BLEInterface::MAX_PEER_SUMMARIES];
                size_t count = ble_interface_impl->getConnectedPeerSummaries(peers, BLEInterface::MAX_PEER_SUMMARIES);

                // Cast is safe - both structs have identical memory layout
                ui_manager->get_status_screen()->set_ble_info(
                    reinterpret_cast<UI::LXMF::StatusScreen::BLEPeerInfo*>(peers), count);
            }
        }
    }

    // Read GPS data continuously (TinyGPSPlus needs constant feeding)
    while (GPSSerial.available() > 0) {
        gps.encode(GPSSerial.read());
    }

    // Async GPS time sync retry. We dropped the synchronous 15s wait
    // from boot — instead, retry every 30s here until sync succeeds.
    // This also gives a path to catch fixes that arrive after boot
    // (warm starts, stationary cold starts, etc).
    static uint32_t _gps_sync_last_attempt = 0;
    if (!gps_time_synced
            && app_settings.gps_time_sync
            && millis() - _gps_sync_last_attempt > 30000) {
        _gps_sync_last_attempt = millis();
        if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
            // Cheap path: TinyGPSPlus already has a valid timestamp,
            // so sync_time_from_gps will return immediately.
            sync_time_from_gps(0);
        }
    }

    // Screen timeout handling
    LOOP_STEP(13);  // Screen timeout
    if (app_settings.screen_timeout > 0) {  // 0 = never timeout
        uint32_t inactive_ms;
        {
            LVGL_LOCK();
            inactive_ms = lv_disp_get_inactive_time(NULL);
        }
        uint32_t timeout_ms = app_settings.screen_timeout * 1000;

        if (!screen_off && inactive_ms > timeout_ms) {
            // Screen has been inactive long enough - turn off backlight
            saved_brightness = app_settings.brightness;
            ledcWrite(0, 0);  // Turn off backlight (channel 0)
            screen_off = true;
            screen_off_time = millis();
            DEBUG("Screen timeout - backlight off");
        }
        else if (screen_off && inactive_ms < (millis() - screen_off_time)) {
            // Activity detected since screen turned off - wake immediately
            ledcWrite(0, saved_brightness);
            screen_off = false;
            DEBUG("Activity detected - backlight on");
        }
    }

    // Keyboard backlight timeout handling
    if (app_settings.keyboard_light) {
        uint32_t key_time = Keyboard::get_last_key_time();
        if (key_time > 0 && key_time > last_keypress_time) {
            // New key detected
            last_keypress_time = key_time;
            if (!kb_light_on) {
                Keyboard::backlight_on();
                kb_light_on = true;
            }
        }

        // Check for timeout
        if (kb_light_on && (millis() - last_keypress_time > KB_LIGHT_TIMEOUT_MS)) {
            Keyboard::backlight_off();
            kb_light_on = false;
        }
    } else {
        // Ensure light is off when setting disabled
        if (kb_light_on) {
            Keyboard::backlight_off();
            kb_light_on = false;
        }
    }

    // Periodic heap monitoring (every 5 seconds)
    static uint32_t last_heap_check = 0;
    static uint32_t last_free_heap = 0;
    static uint32_t last_table_check = 0;
    if (millis() - last_heap_check > 5000) {
        last_heap_check = millis();
        uint32_t free_heap = ESP.getFreeHeap();
        uint32_t min_heap = ESP.getMinFreeHeap();
        uint32_t max_block = ESP.getMaxAllocHeap();
        int32_t delta = (last_free_heap > 0) ? ((int32_t)free_heap - (int32_t)last_free_heap) : 0;
        UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);

        {
            char diag[192];
            int n = snprintf(diag, sizeof(diag),
                "[HEAP] free=%u min=%u max_block=%u delta=%+d stack_hwm=%u step=%u",
                free_heap, min_heap, max_block, delta, stack_hwm, (unsigned)loop_step);
            Serial.println(diag);
            udp_send(diag, n);
        }
        // PSRAM diagnostics
        uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t psram_total = ESP.getPsramSize();
        uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t internal_max_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        {
            char diag[128];
            int n = snprintf(diag, sizeof(diag),
                "[PSRAM] free=%u/%u  [INTERNAL] free=%u max_block=%u",
                psram_free, psram_total, internal_free, internal_max_block);
            Serial.println(diag);
            udp_send(diag, n);
        }
        Serial.flush();

        // Threshold warnings
        if (free_heap < 20000) {
            {
                const char* crit = "[HEAP] CRITICAL: Free heap below 20KB!";
                Serial.println(crit);
                udp_send(crit, strlen(crit));
            }
            // Print Transport table sizes for debugging.
            //
            // Vanilla upstream microReticulum @ 0.3.0 doesn't expose the
            // *_count() getter family the fork added. The fork's commit
            // 4d6f0b9 (PSRAM/TLSF allocator) replaced these with allocator-
            // stats getters, but pyxis hasn't been ported to those yet.
            //
            // Drop the diagnostic for now — it's a developer-debugging tool,
            // not load-bearing for runtime behavior. To restore: either (a)
            // upstream PR adding the *_count getters back to Transport, or
            // (b) port the diagnostic to use Reticulum::get_path_table().size()
            // and friends, plus heap-stats from the new allocator.
            //
            // Tracked in pyxis_microReticulum_graft_spike_findings.md.
            {
                const char* note = "[TABLES] (size diagnostics disabled — see graft notes)";
                Serial.println(note);
                udp_send(note, strlen(note));
            }
        } else if (free_heap < 50000) {
            const char* warn = "[HEAP] WARNING: Free heap below 50KB";
            Serial.println(warn);
            udp_send(warn, strlen(warn));
        }

        // Fragmentation warning (large gap between free heap and max allocatable block)
        if (max_block < free_heap / 2) {
            char frag[96];
            int n = snprintf(frag, sizeof(frag),
                "[HEAP] WARNING: Fragmentation detected (max_block=%u, free=%u)",
                max_block, free_heap);
            Serial.println(frag);
            udp_send(frag, n);
        }

        // Periodic table diagnostics — disabled post-graft. Same reason as
        // the in-CRITICAL-heap [TABLES] block above: vanilla upstream
        // microReticulum @ 0.3.0 doesn't expose Identity::*_count or
        // Transport::*_count getters. Restore by porting to upstream's
        // get_path_table().size() etc., or PR the getters back upstream.
        (void)last_table_check;

        last_free_heap = free_heap;
    }

    // Small delay to prevent tight loop
    delay(5);
}

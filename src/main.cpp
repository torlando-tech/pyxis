// LXMF Messenger for LilyGO T-Deck Plus
// Complete LXMF messaging application with LVGL UI

#include <Arduino.h>
#include <Wire.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <new>  // placement new
#include <soc/rtc_cntl_reg.h>

// Reticulum
#include <Reticulum.h>
#include <Utilities/OS.h>

// Filesystem
#include <UniversalFileSystem.h>
#include <Identity.h>
#include <Destination.h>
#include <Transport.h>
#include <Interface.h>

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
#include <Log.h>

// SD Card access and logging
#include <Hardware/TDeck/SDAccess.h>
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

static const char* SETTINGS_NAMESPACE = "settings";
static const char* KEY_PROP_AUTO = "prop_auto";
static const char* KEY_PROP_NODE = "prop_node";
static const char* KEY_PROP_FALLBACK = "prop_fall";
static const char* KEY_PROP_ONLY = "prop_only";
static const char* KEY_PROP_PIN_MIGRATION = "prop_pin_v1";
static const char* DEFAULT_PROP_NODE = "93df1c70a148cd93af0053cc781ef11a";

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

static void refresh_outbound_propagation_node() {
    if (!router || !propagation_manager) {
        return;
    }

    Bytes effective_node = propagation_manager->get_effective_node();

    router->set_outbound_propagation_node(effective_node);

    uint8_t stamp_cost = 0;
    if (effective_node.size() > 0) {
        PropagationNodeInfo node_info = propagation_manager->get_node(effective_node);
        stamp_cost = node_info.stamp_cost;
    }
    router->set_outbound_propagation_stamp_cost(stamp_cost);
}

static void migrate_default_propagation_settings(Preferences& prefs) {
    if (prefs.getBool(KEY_PROP_PIN_MIGRATION, false)) {
        return;
    }

    prefs.putBool(KEY_PROP_AUTO, false);
    prefs.putString(KEY_PROP_NODE, DEFAULT_PROP_NODE);
    prefs.putBool(KEY_PROP_PIN_MIGRATION, true);
    INFO(("Applied propagation default migration: pinned node " + String(DEFAULT_PROP_NODE).substring(0, 16) + "...").c_str());
}

// Timing
uint32_t last_ui_update = 0;
uint32_t last_announce = 0;
uint32_t last_sync = 0;
uint32_t last_status_check = 0;
uint32_t last_persist_run = 0;
uint32_t last_persist_defer_log = 0;
const uint32_t STATUS_CHECK_INTERVAL = 1000;  // 1 second
const uint32_t PERSIST_FORCE_INTERVAL_MS = 60000;
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

    // Check if gps object already has valid data (from main loop feeding)
    bool got_time = (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024);
    bool got_location = gps.location.isValid();

    // If not ready yet, wait and read serial data
    uint32_t start = millis();
    while (!(got_time && got_location) && (millis() - start < timeout_ms)) {
        while (GPSSerial.available() > 0) {
            gps.encode(GPSSerial.read());
        }
        got_time = (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024);
        got_location = gps.location.isValid();
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
    // Set TZ to UTC BEFORE mktime — mktime interprets its argument as local
    // time and mutates the struct, so TZ must be correct on the first call.
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t gps_unix = mktime(&gps_time);

    // Set the system time
    struct timeval tv;
    tv.tv_sec = gps_unix;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    // Set timezone based on location if available
    // Use US timezone with DST rules when in continental US longitude range,
    // otherwise fall back to a simple offset without DST.
    const char* tz_str = "EST5EDT,M3.2.0,M11.1.0";  // default
    if (got_location) {
        double longitude = gps.location.lng();
        if (longitude >= -67.0 && longitude < -67.0)       // Atlantic: not continental US
            tz_str = "AST4ADT,M3.2.0,M11.1.0";
        else if (longitude >= -82.5 && longitude < -67.0)   // Eastern
            tz_str = "EST5EDT,M3.2.0,M11.1.0";
        else if (longitude >= -97.5 && longitude < -82.5)   // Central
            tz_str = "CST6CDT,M3.2.0,M11.1.0";
        else if (longitude >= -112.5 && longitude < -97.5)  // Mountain
            tz_str = "MST7MDT,M3.2.0,M11.1.0";
        else if (longitude >= -127.5 && longitude < -112.5) // Pacific
            tz_str = "PST8PDT,M3.2.0,M11.1.0";
        else {
            // Outside US — use simple offset from longitude (no DST)
            int tz_offset = calculate_timezone_offset_hours(longitude);
            static char tz_buf[32];
            if (tz_offset >= 0)
                snprintf(tz_buf, sizeof(tz_buf), "GPS%d", -tz_offset);
            else
                snprintf(tz_buf, sizeof(tz_buf), "GPS+%d", -tz_offset);
            tz_str = tz_buf;
        }

        String msg = "  GPS location: " + String(gps.location.lat(), 4) + ", " + String(gps.location.lng(), 4);
        INFO(msg.c_str());
        msg = "  Timezone: " + String(tz_str);
        INFO(msg.c_str());
    } else {
        WARNING("GPS location not available, using Eastern Time");
    }
    setenv("TZ", tz_str, 1);
    tzset();

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
    prefs.begin(SETTINGS_NAMESPACE, false);
    migrate_default_propagation_settings(prefs);

    // Network
    app_settings.wifi_ssid = prefs.getString("wifi_ssid", "");
    app_settings.wifi_password = prefs.getString("wifi_pass", "");
    app_settings.tcp_host = prefs.getString("tcp_host", "sideband.connect.reticulum.network");
    app_settings.tcp_port = prefs.getUShort("tcp_port", 4965);

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
    app_settings.prop_auto_select = prefs.getBool(KEY_PROP_AUTO, false);
    app_settings.prop_selected_node = prefs.getString(KEY_PROP_NODE, DEFAULT_PROP_NODE);
    app_settings.prop_fallback_enabled = prefs.getBool(KEY_PROP_FALLBACK, true);
    app_settings.prop_only = prefs.getBool(KEY_PROP_ONLY, false);

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

    BOOT_PROFILE_WAIT_START("wifi_connect");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    BOOT_PROFILE_WAIT_END("wifi_connect");

    if (WiFi.status() == WL_CONNECTED) {
        INFO("WiFi connected!");
        msg = "  IP address: " + WiFi.localIP().toString();
        INFO(msg.c_str());
        msg = "  RSSI: " + String(WiFi.RSSI()) + " dBm";
        INFO(msg.c_str());

        // Try GPS time sync first (if GPS is initialized and we haven't synced already)
        if (!gps_time_synced) {
            // Fallback to NTP if GPS didn't work
            INFO("Syncing time via NTP (GPS not available)...");

            // Use configTzTime for proper timezone handling on ESP32
            // Eastern Time: EST5EDT = UTC-5, DST starts 2nd Sunday March, ends 1st Sunday Nov
            configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");

            // Wait for time to be set (max 10 seconds)
            struct tm timeinfo;
            int retry = 0;
            while (!getLocalTime(&timeinfo) && retry < 20) {
                delay(500);
                retry++;
            }

            if (retry < 20) {
                // Set the time offset for Utilities::OS::time()
                time_t now = time(nullptr);
                uint64_t uptime_ms = millis();
                uint64_t unix_ms = (uint64_t)now * 1000;
                RNS::Utilities::OS::setTimeOffset(unix_ms - uptime_ms);

                char time_str[64];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
                msg = "  NTP time synced: " + String(time_str);
                INFO(msg.c_str());
            } else {
                WARNING("NTP time sync failed!");
            }
        } else {
            INFO("Time already synced via GPS");
        }

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
        RNS::setLogCallback([](const char* msg, RNS::LogLevel level) {
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
    } else {
        ERROR("WiFi connection failed!");
    }
}

void setup_hardware() {
    INFO("\n=== Hardware Initialization ===");

    // Initialize SPIFFS for persistence via UniversalFileSystem
    // NOTE: Do NOT call SPIFFS.begin() here - UniversalFileSystem::init() handles it
    static RNS::FileSystem fs = new UniversalFileSystem();
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

    // Start LVGL on its own FreeRTOS task for responsive UI
    // Core 1, priority 1 (same as loopTask — round-robin scheduling).
    // Previously priority 2, but this starved loopTask of CPU time during
    // heavy rendering, causing 30s WDT timeouts on loopTask.
    if (!UI::LVGL::LVGLInit::start_task(1, 1)) {
        ERROR("Failed to start LVGL task!");
        while (1) delay(1000);
    }

    INFO("LVGL task started on core 1");

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

    // Add Auto interface (if enabled and WiFi connected)
    if (app_settings.auto_enabled && WiFi.status() == WL_CONNECTED) {
        INFO("Initializing AutoInterface (IPv6 peer discovery)...");

        auto_interface_impl = new AutoInterface("Auto");
        auto_interface = new Interface(auto_interface_impl);

        if (!auto_interface->start()) {
            ERROR("Failed to initialize AutoInterface!");
        } else {
            INFO("AutoInterface started");
            Transport::register_interface(*auto_interface);
        }
    } else if (app_settings.auto_enabled) {
        WARNING("AutoInterface enabled but WiFi not connected - skipping");
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
    if (!app_settings.prop_auto_select && !app_settings.prop_selected_node.isEmpty()) {
        Bytes selected_node;
        selected_node.assignHex(app_settings.prop_selected_node.c_str());
        propagation_manager->set_selected_node(selected_node);
        INFO(("  Selected propagation node: " + app_settings.prop_selected_node.substring(0, 16) + "...").c_str());
    } else {
        propagation_manager->set_selected_node(Bytes());
        INFO("  Propagation node: auto-select");
    }
    propagation_manager->set_update_callback([]() {
        refresh_outbound_propagation_node();
    });
    refresh_outbound_propagation_node();
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

                if (propagation_manager) {
                    if (new_settings.prop_auto_select) {
                        propagation_manager->set_selected_node(Bytes());
                    } else if (!new_settings.prop_selected_node.isEmpty()) {
                        Bytes selected_node;
                        selected_node.assignHex(new_settings.prop_selected_node.c_str());
                        propagation_manager->set_selected_node(selected_node);
                    }
                }

                refresh_outbound_propagation_node();

                // When auto-select is enabled, save the current effective node for next boot
                if (new_settings.prop_auto_select && propagation_manager) {
                    Bytes effective = propagation_manager->get_effective_node();
                    if (effective.size() > 0) {
                        app_settings.prop_selected_node = String(effective.toHex().c_str());
                        // Also persist to NVS
                        Preferences prefs;
                        prefs.begin(SETTINGS_NAMESPACE, false);
                        prefs.putString(KEY_PROP_NODE, app_settings.prop_selected_node);
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

    // Initialize GPS and try to sync time (before WiFi)
    BOOT_PROFILE_START("gps");
    setup_gps();
    if (app_settings.gps_time_sync) {
        INFO("\n=== Time Synchronization ===");
        BOOT_PROFILE_WAIT_START("gps_sync");
        if (!sync_time_from_gps(15000)) {  // 15 second timeout for GPS
            INFO("GPS time sync not available, will try NTP after WiFi");
        }
        BOOT_PROFILE_WAIT_END("gps_sync");
    } else {
        INFO("GPS time sync disabled in settings");
    }
    BOOT_PROFILE_END("gps");

    // Initialize WiFi
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

    // Send initial LXST voice destination announce
    if (ui_manager) {
        ui_manager->announce_lxst();
    }

    // Register delivered callback to update message status in storage and UI
    router->register_sent_callback([](LXMF::LXMessage& msg) {
        RNS::Bytes msg_hash = msg.hash();
        bool propagated = (msg.method() == LXMF::Type::Message::PROPAGATED);
        INFO("Message sent for message: " + msg_hash.toHex().substr(0, 16) + "...");
        Serial.flush();

        if (message_store) {
            message_store->update_message_state(msg_hash, LXMF::Type::Message::SENT);
            message_store->update_message_propagated(msg_hash, propagated);

            LXMF::LXMessage full_msg = message_store->load_message(msg_hash);
            if (full_msg.hash()) {
                full_msg.state(LXMF::Type::Message::SENT);
                full_msg.set_method(msg.method());
                if (ui_manager) {
                    ui_manager->on_message_sent(full_msg);
                }
            } else if (ui_manager) {
                ui_manager->on_message_sent(msg);
            }
        } else if (ui_manager) {
            ui_manager->on_message_sent(msg);
        }
    });

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
                  full_msg.set_method(msg.method());
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

    router->register_failed_callback([](LXMF::LXMessage& msg) {
        RNS::Bytes msg_hash = msg.hash();
        WARNING("Message delivery failed for message: " + msg_hash.toHex().substr(0, 16) + "...");
        Serial.flush();

        if (message_store) {
            message_store->update_message_state(msg_hash, LXMF::Type::Message::FAILED);

            LXMF::LXMessage full_msg = message_store->load_message(msg_hash);
            if (full_msg.hash()) {
                full_msg.state(LXMF::Type::Message::FAILED);
                full_msg.set_method(msg.method());
                if (ui_manager) {
                    ui_manager->on_message_failed(full_msg);
                }
            } else if (ui_manager) {
                ui_manager->on_message_failed(msg);
            }
        } else if (ui_manager) {
            ui_manager->on_message_failed(msg);
        }
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
    esp_task_wdt_init(30, true);  // 30s timeout, panic on trigger
    esp_task_wdt_add(NULL);       // Subscribe loopTask
    INFO("Task Watchdog: loopTask subscribed (30s timeout)");

    // Feed WDT during long Identity persistence (71+ entries to SPIFFS can take >30s)
    Identity::set_persist_yield_callback([]() { esp_task_wdt_reset(); });

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

void loop() {
    esp_task_wdt_reset();

    // Handle OTA updates (must be called frequently)
    ArduinoOTA.handle();

    // Handle serial commands for web flasher detection
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
            serial_cmd_buffer = "";
        } else if (serial_cmd_buffer.length() < 32) {
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
    // NOTE: Identity persistence writes 40-50 entries to SPIFFS flash, which
    // involves sector erases (100ms each) and can take 5-15s total.
    // WDT feeds between calls prevent timeout during heavy flash I/O.
    LOOP_STEP(5);  // persist data
    {
        uint32_t now = millis();
        uint32_t last_flush_ms = Hardware::TDeck::Display::last_flush_ms();
        bool ui_under_pressure = (last_flush_ms > 0 && (now - last_flush_ms) > 1000);
        bool tcp_under_pressure = tcp_interface_impl && tcp_interface_impl->under_backpressure();
        bool allow_persist = (!ui_under_pressure && !tcp_under_pressure) ||
                             (now - last_persist_run >= PERSIST_FORCE_INTERVAL_MS);
        if (allow_persist) {
            reticulum->should_persist_data();
            esp_task_wdt_reset();
            // Fast-persist known destinations (5s after dirty) to survive crashes
            Identity::should_persist_data();
            esp_task_wdt_reset();
            last_persist_run = now;
        } else if (now - last_persist_defer_log >= 5000) {
            last_persist_defer_log = now;
            INFO("Deferring persistence while UI/TCP are under pressure");
        }
    }

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

    delay(1);
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

    // Periodic RNS status check (check all interfaces)
    if (millis() - last_status_check > STATUS_CHECK_INTERVAL) {
        last_status_check = millis();

        // Start TCP interface when WiFi becomes available
        bool wifi_connected = (WiFi.status() == WL_CONNECTED);
        if (wifi_connected && !last_wifi_connected && !tcp_interface_impl && app_settings.tcp_enabled) {
            INFO("WiFi connected - starting TCP interface");
            start_tcp_interface();
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

    // Retry GPS time sync once we get a fix (boot timeout often misses cold start)
    if (!gps_time_synced && gps.date.isValid() && gps.time.isValid()
        && gps.date.year() >= 2024 && gps.location.isValid()) {
        // Data is already in the gps object from the feed loop above —
        // sync_time_from_gps will find it on the first iteration
        sync_time_from_gps(1000);
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
            // Print Transport table sizes for debugging
            {
                char tbl[192];
                int n = snprintf(tbl, sizeof(tbl),
                    "[TABLES] ann=%zu dest=%zu rev=%zu link=%zu held=%zu rate=%zu path=%zu",
                    RNS::Transport::announce_table_count(),
                    RNS::Transport::destination_table_count(),
                    RNS::Transport::reverse_table_count(),
                    RNS::Transport::link_table_count(),
                    RNS::Transport::held_announces_count(),
                    RNS::Transport::announce_rate_table_count(),
                    RNS::Transport::path_requests_count());
                Serial.println(tbl);
                udp_send(tbl, n);
                n = snprintf(tbl, sizeof(tbl),
                    "[TABLES] pend_link=%zu act_link=%zu rcpt=%zu pkt_hash=%zu iface=%zu dest_pool=%zu",
                    RNS::Transport::pending_links_count(),
                    RNS::Transport::active_links_count(),
                    RNS::Transport::receipts_count(),
                    RNS::Transport::packet_hashlist_count(),
                    RNS::Transport::interfaces_count(),
                    RNS::Transport::destinations_count());
                Serial.println(tbl);
                udp_send(tbl, n);
                n = snprintf(tbl, sizeof(tbl), "[IDENTITY] known_dest=%zu known_ratch=%zu",
                    RNS::Identity::known_destinations_count(),
                    RNS::Identity::known_ratchets_count());
                Serial.println(tbl);
                udp_send(tbl, n);
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

        // Periodic table diagnostics (every 30 seconds)
        if (millis() - last_table_check > 30000) {
            last_table_check = millis();
            char diag[192];
            int n = snprintf(diag, sizeof(diag),
                "[DIAG] ikd=%zu ikr=%zu ann=%zu dest=%zu pkt=%zu held=%zu rev=%zu link=%zu",
                RNS::Identity::known_destinations_count(),
                RNS::Identity::known_ratchets_count(),
                RNS::Transport::announce_table_count(),
                RNS::Transport::destination_table_count(),
                RNS::Transport::packet_hashlist_count(),
                RNS::Transport::held_announces_count(),
                RNS::Transport::reverse_table_count(),
                RNS::Transport::link_table_count());
            Serial.println(diag);
            udp_send(diag, n);
        }

        last_free_heap = free_heap;
    }

    // Small delay to prevent tight loop
    delay(5);
}

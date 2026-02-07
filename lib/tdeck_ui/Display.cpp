/*
 * Display.cpp - OLED display implementation for microReticulum
 */

#include "Display.h"

#ifdef HAS_DISPLAY

#include "DisplayGraphics.h"
#include "Identity.h"
#include "Interface.h"
#include "Reticulum.h"
#include "Transport.h"
#include "Log.h"
#include "Utilities/OS.h"

// Display library includes (board-specific)
#ifdef ARDUINO
    #include <Wire.h>
    #ifdef DISPLAY_TYPE_SH1106
        #include <Adafruit_SH110X.h>
    #elif defined(DISPLAY_TYPE_SSD1306)
        #include <Adafruit_SSD1306.h>
    #endif
    #include <Adafruit_GFX.h>
#endif

namespace RNS {

// Display dimensions
static const int16_t DISPLAY_WIDTH = 128;
static const int16_t DISPLAY_HEIGHT = 64;

// Layout constants
static const int16_t HEADER_HEIGHT = 17;
static const int16_t CONTENT_Y = 21;
static const int16_t LINE_HEIGHT = 10;
static const int16_t LEFT_MARGIN = 2;

// Static member initialization
bool Display::_ready = false;
bool Display::_blanked = false;
uint8_t Display::_current_page = 0;
uint32_t Display::_last_page_flip = 0;
uint32_t Display::_last_update = 0;
uint32_t Display::_start_time = 0;
Bytes Display::_identity_hash;
Interface* Display::_interface = nullptr;
Reticulum* Display::_reticulum = nullptr;
float Display::_rssi = -120.0f;

// Display object (board-specific)
#ifdef ARDUINO
    #ifdef DISPLAY_TYPE_SH1106
        static Adafruit_SH1106G* display = nullptr;
    #elif defined(DISPLAY_TYPE_SSD1306)
        static Adafruit_SSD1306* display = nullptr;
    #endif
#endif

bool Display::init() {
#ifdef ARDUINO
    TRACE("Display::init: Initializing display...");

    // Initialize I2C
    #if defined(DISPLAY_SDA) && defined(DISPLAY_SCL)
        Wire.begin(DISPLAY_SDA, DISPLAY_SCL);
    #else
        Wire.begin();
    #endif

    // Create display object
    #ifdef DISPLAY_TYPE_SH1106
        display = new Adafruit_SH1106G(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, -1);
        #ifndef DISPLAY_ADDR
            #define DISPLAY_ADDR 0x3C
        #endif
        if (!display->begin(DISPLAY_ADDR, true)) {
            ERROR("Display::init: SH1106 display not found");
            delete display;
            display = nullptr;
            return false;
        }
    #elif defined(DISPLAY_TYPE_SSD1306)
        display = new Adafruit_SSD1306(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, -1);
        #ifndef DISPLAY_ADDR
            #define DISPLAY_ADDR 0x3C
        #endif
        if (!display->begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDR)) {
            ERROR("Display::init: SSD1306 display not found");
            delete display;
            display = nullptr;
            return false;
        }
    #else
        ERROR("Display::init: No display type defined");
        return false;
    #endif

    // Configure display
    display->setRotation(0);  // Portrait mode
    display->clearDisplay();
    display->setTextSize(1);
    display->setTextColor(1);  // White
    display->cp437(true);      // Enable extended characters
    display->display();

    _ready = true;
    _start_time = (uint32_t)Utilities::OS::ltime();
    _last_page_flip = _start_time;
    _last_update = _start_time;

    INFO("Display::init: Display initialized successfully");
    return true;
#else
    // Native build - no display support
    return false;
#endif
}

void Display::update() {
    if (!_ready || _blanked) return;

#ifdef ARDUINO
    uint32_t now = (uint32_t)Utilities::OS::ltime();

    // Check for page rotation
    if (now - _last_page_flip >= PAGE_INTERVAL) {
        _current_page = (_current_page + 1) % NUM_PAGES;
        _last_page_flip = now;
    }

    // Throttle display updates
    if (now - _last_update < UPDATE_INTERVAL) return;
    _last_update = now;

    // Clear and redraw
    display->clearDisplay();

    // Draw header (common to all pages)
    draw_header();

    // Draw page-specific content
    switch (_current_page) {
        case 0:
            draw_page_main();
            break;
        case 1:
            draw_page_interface();
            break;
        case 2:
            draw_page_network();
            break;
    }

    // Send to display
    display->display();
#endif
}

void Display::set_identity(const Identity& identity) {
    if (identity) {
        _identity_hash = identity.hash();
    }
}

void Display::set_interface(Interface* iface) {
    _interface = iface;
}

void Display::set_reticulum(Reticulum* rns) {
    _reticulum = rns;
}

void Display::blank(bool blank) {
    _blanked = blank;
#ifdef ARDUINO
    if (_ready && display) {
        if (blank) {
            display->clearDisplay();
            display->display();
        }
    }
#endif
}

void Display::set_page(uint8_t page) {
    if (page < NUM_PAGES) {
        _current_page = page;
        _last_page_flip = (uint32_t)Utilities::OS::ltime();
    }
}

void Display::next_page() {
    _current_page = (_current_page + 1) % NUM_PAGES;
    _last_page_flip = (uint32_t)Utilities::OS::ltime();
}

uint8_t Display::current_page() {
    return _current_page;
}

bool Display::ready() {
    return _ready;
}

void Display::set_rssi(float rssi) {
    _rssi = rssi;
}

// Private implementation

void Display::draw_header() {
#ifdef ARDUINO
    // Draw "μRNS" text logo
    display->setTextSize(2);  // 2x size for visibility
    display->setCursor(3, 0);
    // Use CP437 code 230 (0xE6) for μ character
    display->write(0xE6);  // μ
    display->print("RNS");
    display->setTextSize(1);  // Reset to normal size

    // Draw signal bars on the right
    draw_signal_bars(DISPLAY_WIDTH - Graphics::SIGNAL_WIDTH - 2, 4);

    // Draw separator line
    display->drawLine(0, HEADER_HEIGHT, DISPLAY_WIDTH - 1, HEADER_HEIGHT, 1);
#endif
}

void Display::draw_signal_bars(int16_t x, int16_t y) {
#ifdef ARDUINO
    // Temporarily disabled to debug stray pixel
    // uint8_t level = 0;
    // if (_interface && _interface->online()) {
    //     level = Graphics::rssi_to_level(_rssi);
    // }
    // const uint8_t* bitmap = Graphics::get_signal_bitmap(level);
    // display->drawBitmap(x, y, bitmap, Graphics::SIGNAL_WIDTH, Graphics::SIGNAL_HEIGHT, 1);
#endif
}

void Display::draw_page_main() {
#ifdef ARDUINO
    int16_t y = CONTENT_Y;

    // Identity hash
    display->setCursor(LEFT_MARGIN, y);
    display->print("ID: ");
    if (_identity_hash.size() > 0) {
        // Show first 12 hex chars
        std::string hex = _identity_hash.toHex();
        if (hex.length() > 12) hex = hex.substr(0, 12);
        display->print(hex.c_str());
    } else {
        display->print("(none)");
    }
    y += LINE_HEIGHT + 4;

    // Interface status
    display->setCursor(LEFT_MARGIN, y);
    if (_interface) {
        display->print(_interface->name().c_str());
        display->print(": ");
        display->print(_interface->online() ? "ONLINE" : "OFFLINE");
    } else {
        display->print("No interface");
    }
    y += LINE_HEIGHT;

    // Link count
    display->setCursor(LEFT_MARGIN, y);
    display->print("Links: ");
    if (_reticulum) {
        display->print((int)_reticulum->get_link_count());
    } else {
        display->print("0");
    }
#endif
}

void Display::draw_page_interface() {
#ifdef ARDUINO
    int16_t y = CONTENT_Y;

    if (!_interface) {
        display->setCursor(LEFT_MARGIN, y);
        display->print("No interface");
        return;
    }

    // Interface name
    display->setCursor(LEFT_MARGIN, y);
    display->print("Interface: ");
    display->print(_interface->name().c_str());
    y += LINE_HEIGHT;

    // Mode
    display->setCursor(LEFT_MARGIN, y);
    display->print("Mode: ");
    switch (_interface->mode()) {
        case Type::Interface::MODE_GATEWAY:
            display->print("Gateway");
            break;
        case Type::Interface::MODE_ACCESS_POINT:
            display->print("Access Point");
            break;
        case Type::Interface::MODE_POINT_TO_POINT:
            display->print("Point-to-Point");
            break;
        case Type::Interface::MODE_ROAMING:
            display->print("Roaming");
            break;
        case Type::Interface::MODE_BOUNDARY:
            display->print("Boundary");
            break;
        default:
            display->print("Unknown");
            break;
    }
    y += LINE_HEIGHT;

    // Bitrate
    display->setCursor(LEFT_MARGIN, y);
    display->print("Bitrate: ");
    display->print(format_bitrate(_interface->bitrate()).c_str());
    y += LINE_HEIGHT;

    // Status
    display->setCursor(LEFT_MARGIN, y);
    display->print("Status: ");
    display->print(_interface->online() ? "Online" : "Offline");
#endif
}

void Display::draw_page_network() {
#ifdef ARDUINO
    int16_t y = CONTENT_Y;

    // Links and paths
    display->setCursor(LEFT_MARGIN, y);
    display->print("Links: ");
    size_t link_count = _reticulum ? _reticulum->get_link_count() : 0;
    display->print((int)link_count);

    display->print("  Paths: ");
    size_t path_count = _reticulum ? _reticulum->get_path_table().size() : 0;
    display->print((int)path_count);
    y += LINE_HEIGHT;

    // RTT placeholder (would need link reference to get actual RTT)
    display->setCursor(LEFT_MARGIN, y);
    display->print("RTT: --");
    y += LINE_HEIGHT;

    // TX/RX bytes (if interface available)
    display->setCursor(LEFT_MARGIN, y);
    display->print("TX/RX: --");
    y += LINE_HEIGHT;

    // Uptime
    display->setCursor(LEFT_MARGIN, y);
    display->print("Uptime: ");
    uint32_t uptime_sec = ((uint32_t)Utilities::OS::ltime() - _start_time) / 1000;
    display->print(format_time(uptime_sec).c_str());
#endif
}

std::string Display::format_bytes(size_t bytes) {
    char buf[16];
    if (bytes >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1fM", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.1fK", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%zuB", bytes);
    }
    return std::string(buf);
}

std::string Display::format_time(uint32_t seconds) {
    char buf[16];
    if (seconds >= 3600) {
        uint32_t hours = seconds / 3600;
        uint32_t mins = (seconds % 3600) / 60;
        snprintf(buf, sizeof(buf), "%luh %lum", (unsigned long)hours, (unsigned long)mins);
    } else if (seconds >= 60) {
        uint32_t mins = seconds / 60;
        uint32_t secs = seconds % 60;
        snprintf(buf, sizeof(buf), "%lum %lus", (unsigned long)mins, (unsigned long)secs);
    } else {
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)seconds);
    }
    return std::string(buf);
}

std::string Display::format_bitrate(uint32_t bps) {
    char buf[16];
    if (bps >= 1000000) {
        snprintf(buf, sizeof(buf), "%.1f Mbps", bps / 1000000.0);
    } else if (bps >= 1000) {
        snprintf(buf, sizeof(buf), "%.1f kbps", bps / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "%lu bps", (unsigned long)bps);
    }
    return std::string(buf);
}

} // namespace RNS

#endif // HAS_DISPLAY

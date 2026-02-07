// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_SDLOGGER_H
#define HARDWARE_TDECK_SDLOGGER_H

#include "Config.h"
#include <Log.h>

#ifdef ARDUINO
#include <SD.h>
#include <SPI.h>
#endif

namespace Hardware {
namespace TDeck {

/**
 * SD Card Logger for crash debugging
 *
 * Writes logs to SD card with frequent flushing to capture
 * context before crashes. Uses a ring buffer approach with
 * two log files to prevent unbounded growth.
 *
 * Usage:
 *   SDLogger::init();  // Call after SPI is initialized
 *   // Logs are automatically written via RNS::setLogCallback
 */
class SDLogger {
public:
    /**
     * Initialize SD card and set up logging callback.
     * Must be called after SPI bus is initialized (after display init).
     *
     * @return true if SD card mounted and logging active
     */
    static bool init();

    /**
     * Check if SD logging is active
     */
    static bool isActive() { return _active; }

    /**
     * Force flush any buffered log data to SD card.
     * Call periodically or before expected operations.
     */
    static void flush();

    /**
     * Write a marker to help identify crash points.
     * Use before operations that might crash.
     */
    static void marker(const char* msg);

    /**
     * Close log file cleanly (call before SD card removal)
     */
    static void close();

private:
    static void logCallback(const char* msg, RNS::LogLevel level);
    static void rotateIfNeeded();
    static void writeToFile(const char* msg, RNS::LogLevel level);

    static bool _active;
    static bool _initialized;

#ifdef ARDUINO
    static File _logFile;
#endif

    static uint32_t _bytes_written;
    static uint32_t _last_flush;
    static uint32_t _line_count;

    // Configuration
    static constexpr uint32_t MAX_LOG_SIZE = 1024 * 1024;  // 1MB per log file
    static constexpr uint32_t FLUSH_INTERVAL_MS = 1000;    // Flush every second
    static constexpr uint32_t FLUSH_AFTER_LINES = 10;      // Or every 10 lines
    static constexpr const char* LOG_FILE_A = "/crash_log_a.txt";
    static constexpr const char* LOG_FILE_B = "/crash_log_b.txt";
    static constexpr const char* CURRENT_LOG = "/crash_log_current.txt";
};

} // namespace TDeck
} // namespace Hardware

#endif // HARDWARE_TDECK_SDLOGGER_H

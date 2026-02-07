// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "SDLogger.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

using namespace Hardware::TDeck;

// Static member initialization
bool SDLogger::_active = false;
bool SDLogger::_initialized = false;
uint32_t SDLogger::_bytes_written = 0;
uint32_t SDLogger::_last_flush = 0;
uint32_t SDLogger::_line_count = 0;

#ifdef ARDUINO
File SDLogger::_logFile;

bool SDLogger::init() {
    if (_initialized) {
        return _active;
    }
    _initialized = true;

    // Initialize SD card on shared SPI bus
    // Note: SPI should already be initialized by display
    if (!SD.begin(SDCard::CS)) {
        Serial.println("[SDLogger] SD card mount failed");
        return false;
    }

    // Check card type
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SDLogger] No SD card detected");
        return false;
    }

    // Open or create log file
    // Use append mode to preserve history across reboots
    _logFile = SD.open(CURRENT_LOG, FILE_APPEND);
    if (!_logFile) {
        Serial.println("[SDLogger] Failed to open log file");
        return false;
    }

    // Write boot marker
    _logFile.println("\n========================================");
    _logFile.println("=== BOOT - SD LOGGING STARTED ===");
    _logFile.printf("=== Free heap: %lu bytes ===\n", ESP.getFreeHeap());
    _logFile.printf("=== Min free heap: %lu bytes ===\n", ESP.getMinFreeHeap());
    _logFile.println("========================================\n");
    _logFile.flush();

    _bytes_written = 0;
    _last_flush = millis();
    _line_count = 0;
    _active = true;

    // Set log callback to capture all logs
    RNS::setLogCallback(logCallback);

    Serial.println("[SDLogger] SD card logging active");
    Serial.printf("[SDLogger] Card size: %lluMB\n", SD.cardSize() / (1024 * 1024));

    return true;
}

void SDLogger::logCallback(const char* msg, RNS::LogLevel level) {
    // Always print to serial as well
    Serial.print(RNS::getTimeString());
    Serial.print(" [");
    Serial.print(RNS::getLevelName(level));
    Serial.print("] ");
    Serial.println(msg);
    Serial.flush();

    // Write to SD if active
    if (_active && _logFile) {
        writeToFile(msg, level);
    }
}

void SDLogger::writeToFile(const char* msg, RNS::LogLevel level) {
    // Format: timestamp [LEVEL] message
    int written = _logFile.printf("%s [%s] %s\n",
                                  RNS::getTimeString(),
                                  RNS::getLevelName(level),
                                  msg);
    if (written > 0) {
        _bytes_written += written;
        _line_count++;
    }

    // Flush periodically or after critical messages
    uint32_t now = millis();
    bool should_flush = false;

    // Always flush errors and warnings immediately
    if (level <= RNS::LOG_WARNING) {
        should_flush = true;
    }
    // Flush every N lines
    else if (_line_count >= FLUSH_AFTER_LINES) {
        should_flush = true;
    }
    // Flush every N milliseconds
    else if (now - _last_flush >= FLUSH_INTERVAL_MS) {
        should_flush = true;
    }

    if (should_flush) {
        _logFile.flush();
        _last_flush = now;
        _line_count = 0;
    }

    // Check if we need to rotate
    if (_bytes_written >= MAX_LOG_SIZE) {
        rotateIfNeeded();
    }
}

void SDLogger::flush() {
    if (_active && _logFile) {
        _logFile.flush();
        _last_flush = millis();
        _line_count = 0;
    }
}

void SDLogger::marker(const char* msg) {
    if (_active && _logFile) {
        _logFile.println("----------------------------------------");
        _logFile.printf(">>> MARKER: %s <<<\n", msg);
        _logFile.printf(">>> Heap: %lu / Min: %lu <<<\n",
                       ESP.getFreeHeap(), ESP.getMinFreeHeap());
        _logFile.println("----------------------------------------");
        _logFile.flush();
    }
}

void SDLogger::rotateIfNeeded() {
    if (!_active || !_logFile) return;

    // Close current log
    _logFile.close();

    // Rotate: delete old B, rename A to B, rename current to A
    if (SD.exists(LOG_FILE_B)) {
        SD.remove(LOG_FILE_B);
    }
    if (SD.exists(LOG_FILE_A)) {
        SD.rename(LOG_FILE_A, LOG_FILE_B);
    }
    if (SD.exists(CURRENT_LOG)) {
        SD.rename(CURRENT_LOG, LOG_FILE_A);
    }

    // Open new current log
    _logFile = SD.open(CURRENT_LOG, FILE_WRITE);
    if (!_logFile) {
        _active = false;
        Serial.println("[SDLogger] Failed to create new log file after rotation");
        return;
    }

    _logFile.println("=== LOG ROTATED ===\n");
    _logFile.flush();
    _bytes_written = 0;
}

void SDLogger::close() {
    if (_active && _logFile) {
        _logFile.println("\n=== LOG CLOSED CLEANLY ===");
        _logFile.flush();
        _logFile.close();
        _active = false;
    }
    // Restore default logging
    RNS::setLogCallback(nullptr);
}

#else
// Native build stubs
bool SDLogger::init() { return false; }
void SDLogger::flush() {}
void SDLogger::marker(const char*) {}
void SDLogger::close() {}
#endif

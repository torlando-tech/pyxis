// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_SDACCESS_H
#define HARDWARE_TDECK_SDACCESS_H

#include "Config.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Hardware {
namespace TDeck {

/**
 * Mutex-protected SD card access for shared SPI bus.
 *
 * The T-Deck Plus shares HSPI (SCK=40, MOSI=41, MISO=38) across
 * display (CS=12), LoRa (CS=9), and SD card (CS=39). All SPI
 * operations must be serialized via a shared FreeRTOS mutex.
 *
 * SDAccess wraps SD.begin() and all file operations in the mutex
 * so callers don't need to manage bus arbitration themselves.
 */
class SDAccess {
public:
    /**
     * Initialize SD card on the shared HSPI bus.
     * Creates its own SPIClass(HSPI) attached to the same hardware
     * peripheral, then calls SD.begin() with that instance.
     *
     * @param mutex Shared SPI bus mutex (created in main.cpp)
     * @return true if SD card mounted successfully
     */
    static bool init(SemaphoreHandle_t mutex);

    /** Check if SD card is mounted and ready */
    static bool is_ready() { return _ready; }

    /** Get the shared SPI mutex (for SDLogger to wrap its own operations) */
    static SemaphoreHandle_t get_mutex() { return _spi_mutex; }

    /**
     * Read an entire file into a buffer with mutex protection.
     * @return bytes read, or -1 on error
     */
    static int read_file(const char* path, uint8_t* buf, size_t max_len);

    /** Check if a file exists (with mutex) */
    static bool file_exists(const char* path);

    /**
     * Acquire the SPI bus mutex for streaming operations.
     * Caller MUST call release_bus() when done.
     * @param timeout_ms Max wait time
     * @return true if mutex acquired
     */
    static bool acquire_bus(uint32_t timeout_ms = 500);

    /** Release the SPI bus mutex after streaming operations */
    static void release_bus();

private:
    static SemaphoreHandle_t _spi_mutex;
    static SPIClass* _sd_spi;
    static bool _ready;

    static constexpr uint32_t SD_SPI_FREQ = 20000000;  // 20MHz for SD card
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_SDACCESS_H

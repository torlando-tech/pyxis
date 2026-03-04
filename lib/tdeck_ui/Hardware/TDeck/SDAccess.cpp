// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "SDAccess.h"

#ifdef ARDUINO

#include <Log.h>

using namespace RNS;

namespace Hardware {
namespace TDeck {

SemaphoreHandle_t SDAccess::_spi_mutex = nullptr;
SPIClass* SDAccess::_sd_spi = nullptr;
bool SDAccess::_ready = false;

bool SDAccess::init(SemaphoreHandle_t mutex) {
    if (_ready) return true;
    if (mutex == nullptr) {
        Serial.println("[SDAccess] ERROR: null mutex");
        return false;
    }

    _spi_mutex = mutex;

    // Drive SD CS high before init to avoid bus contention
    pinMode(SDCard::CS, OUTPUT);
    digitalWrite(SDCard::CS, HIGH);

    // Acquire mutex for SD.begin() since it reconfigures SPI
    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        Serial.println("[SDAccess] Failed to acquire SPI mutex for init");
        return false;
    }

    // Create SPIClass on HSPI — attaches to same hardware peripheral
    // as display and LoRa, but SD.begin() needs its own instance
    _sd_spi = new SPIClass(HSPI);
    _sd_spi->begin(Pin::DISPLAY_SCK, Radio::SPI_MISO, Pin::DISPLAY_MOSI, SDCard::CS);

    bool ok = SD.begin(SDCard::CS, *_sd_spi, SD_SPI_FREQ);

    xSemaphoreGive(_spi_mutex);

    if (!ok) {
        Serial.println("[SDAccess] SD card mount failed (no card?)");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SDAccess] No SD card detected");
        return false;
    }

    _ready = true;

    const char* type_str = "UNKNOWN";
    switch (cardType) {
        case CARD_MMC:  type_str = "MMC"; break;
        case CARD_SD:   type_str = "SD"; break;
        case CARD_SDHC: type_str = "SDHC"; break;
        default: break;
    }
    Serial.printf("[SDAccess] Card type: %s, size: %lluMB\n",
                  type_str, SD.cardSize() / (1024 * 1024));

    return true;
}

int SDAccess::read_file(const char* path, uint8_t* buf, size_t max_len) {
    if (!_ready || !_spi_mutex) return -1;

    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return -1;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        xSemaphoreGive(_spi_mutex);
        return -1;
    }

    int bytes_read = f.read(buf, max_len);
    f.close();

    xSemaphoreGive(_spi_mutex);
    return bytes_read;
}

bool SDAccess::file_exists(const char* path) {
    if (!_ready || !_spi_mutex) return false;

    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    bool exists = SD.exists(path);

    xSemaphoreGive(_spi_mutex);
    return exists;
}

bool SDAccess::acquire_bus(uint32_t timeout_ms) {
    if (!_spi_mutex) return false;
    return xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void SDAccess::release_bus() {
    if (_spi_mutex) {
        xSemaphoreGive(_spi_mutex);
    }
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO

// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "SX1262Interface.h"
#include "Log.h"
#include "Utilities/OS.h"

#ifdef ARDUINO
#include <SPI.h>
#endif

using namespace RNS;

#ifdef ARDUINO
// Static members for SPI mutex (shared with display)
SemaphoreHandle_t SX1262Interface::_spi_mutex = nullptr;
bool SX1262Interface::_mutex_initialized = false;
#endif

SX1262Interface::SX1262Interface(const char* name) : InterfaceImpl(name) {
    _IN = true;
    _OUT = true;
    _HW_MTU = HW_MTU;

    // Calculate bitrate from modulation parameters (matching Python RNS formula)
    // bitrate = sf * ((4.0/cr) / (2^sf / (bw/1000))) * 1000
    _bitrate = (double)_config.spreading_factor *
               ((4.0 / _config.coding_rate) /
                (pow(2, _config.spreading_factor) / (_config.bandwidth / 1000.0))) * 1000.0;
}

SX1262Interface::~SX1262Interface() {
    stop();
}

void SX1262Interface::set_config(const SX1262Config& config) {
    _config = config;

    // Recalculate bitrate
    _bitrate = (double)_config.spreading_factor *
               ((4.0 / _config.coding_rate) /
                (pow(2, _config.spreading_factor) / (_config.bandwidth / 1000.0))) * 1000.0;
}

std::string SX1262Interface::toString() const {
    return "SX1262Interface[" + _name + "]";
}

bool SX1262Interface::start() {
    _online = false;

#ifdef ARDUINO
    INFO("SX1262Interface: Initializing...");
    INFO("  Frequency: " + std::to_string(_config.frequency) + " MHz");
    INFO("  Bandwidth: " + std::to_string(_config.bandwidth) + " kHz");
    INFO("  SF: " + std::to_string(_config.spreading_factor));
    INFO("  CR: 4/" + std::to_string(_config.coding_rate));
    INFO("  TX Power: " + std::to_string(_config.tx_power) + " dBm");

    // Initialize SPI mutex if not already done
    if (!_mutex_initialized) {
        _spi_mutex = xSemaphoreCreateMutex();
        if (_spi_mutex == nullptr) {
            ERROR("SX1262Interface: Failed to create SPI mutex");
            return false;
        }
        _mutex_initialized = true;
        DEBUG("SX1262Interface: SPI mutex created");
    }

    // Acquire SPI mutex
    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ERROR("SX1262Interface: Failed to acquire SPI mutex for init");
        return false;
    }

    // Set radio CS high to avoid conflicts
    pinMode(SX1262Pins::CS, OUTPUT);
    digitalWrite(SX1262Pins::CS, HIGH);

    // Create SPI instance for LoRa using HSPI (same bus as display)
    // Display uses HSPI without MISO (write-only), we add MISO for radio reads
    // SCK=40, MISO=38, MOSI=41
    _lora_spi = new SPIClass(HSPI);
    _lora_spi->begin(40, SX1262Pins::SPI_MISO, 41, SX1262Pins::CS);
    DEBUG("SX1262Interface: HSPI initialized (SCK=40, MISO=38, MOSI=41, CS=9)");

    // Create RadioLib module and radio with our SPI instance
    _module = new Module(SX1262Pins::CS, SX1262Pins::DIO1, SX1262Pins::RST, SX1262Pins::BUSY, *_lora_spi);
    _radio = new SX1262(_module);

    // Initialize radio with configuration
    int16_t state = _radio->begin(
        _config.frequency,
        _config.bandwidth,
        _config.spreading_factor,
        _config.coding_rate,
        _config.sync_word,
        _config.tx_power,
        _config.preamble_length
    );

    if (state != RADIOLIB_ERR_NONE) {
        ERROR("SX1262Interface: Radio init failed, code " + std::to_string(state));
        xSemaphoreGive(_spi_mutex);
        delete _radio;
        delete _module;
        _radio = nullptr;
        _module = nullptr;
        return false;
    }

    // Enable CRC for error detection
    state = _radio->setCRC(true);
    if (state != RADIOLIB_ERR_NONE) {
        WARNING("SX1262Interface: Failed to enable CRC, code " + std::to_string(state));
    }

    // Use explicit header mode (includes length in LoRa header)
    state = _radio->explicitHeader();
    if (state != RADIOLIB_ERR_NONE) {
        WARNING("SX1262Interface: Failed to set explicit header, code " + std::to_string(state));
    }

    xSemaphoreGive(_spi_mutex);

    // Start listening for packets
    start_receive();

    _online = true;
    INFO("SX1262Interface: Initialized successfully");
    INFO("  Bitrate: " + std::to_string(Utilities::OS::round(_bitrate / 1000.0, 2)) + " kbps");

    return true;
#else
    ERROR("SX1262Interface: Not supported on this platform");
    return false;
#endif
}

void SX1262Interface::stop() {
#ifdef ARDUINO
    if (_radio != nullptr) {
        if (_spi_mutex != nullptr && xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            _radio->standby();
            xSemaphoreGive(_spi_mutex);
        }

        delete _radio;
        delete _module;
        _radio = nullptr;
        _module = nullptr;
    }
#endif

    _online = false;
    INFO("SX1262Interface: Stopped");
}

void SX1262Interface::start_receive() {
#ifdef ARDUINO
    if (_radio == nullptr) return;

    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int16_t state = _radio->startReceive();
        xSemaphoreGive(_spi_mutex);

        if (state != RADIOLIB_ERR_NONE) {
            ERROR("SX1262Interface: Failed to start receive, code " + std::to_string(state));
        }
    }
#endif
}

void SX1262Interface::loop() {
    if (!_online) return;

#ifdef ARDUINO
    if (_radio == nullptr) return;

    // Try to acquire SPI mutex (non-blocking to avoid stalling display)
    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;  // Display is using SPI, try again later
    }

    // Check IRQ status to see if a packet was actually received
    uint16_t irqStatus = _radio->getIrqStatus();

    // Only process if RX_DONE flag is set (0x0002 for SX126x)
    if (!(irqStatus & 0x0002)) {
        xSemaphoreGive(_spi_mutex);
        return;  // No new packet
    }

    // Read the received packet (this also clears IRQ internally)
    int16_t state = _radio->readData(_rx_buffer.writable(HW_MTU), HW_MTU);

    // Immediately restart receive to clear IRQ flags and prepare for next packet
    _radio->startReceive();

    if (state == RADIOLIB_ERR_NONE) {
        // Got a packet
        size_t len = _radio->getPacketLength();
        if (len > 1) {  // Must have at least header + data
            _rx_buffer.resize(len);

            // Get signal quality
            _last_rssi = _radio->getRSSI();
            _last_snr = _radio->getSNR();

            xSemaphoreGive(_spi_mutex);

            // RNode packet format: [1-byte random header][payload]
            // Skip header byte, pass payload to transport
            Bytes payload = _rx_buffer.mid(1);

            DEBUG("SX1262Interface: Received " + std::to_string(len) + " bytes, " +
                  "RSSI=" + std::to_string((int)_last_rssi) + " dBm, " +
                  "SNR=" + std::to_string((int)_last_snr) + " dB");

            on_incoming(payload);
            return;
        }
    } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
        // An error occurred (not just timeout)
        ERROR("SX1262Interface: Receive error, code " + std::to_string(state));
    }

    xSemaphoreGive(_spi_mutex);
#endif
}

void SX1262Interface::send_outgoing(const Bytes& data) {
    if (!_online) return;

#ifdef ARDUINO
    if (_radio == nullptr) return;

    DEBUG(toString() + ": Sending " + std::to_string(data.size()) + " bytes");

    // Build packet with random header (RNode-compatible format)
    // Header: upper 4 bits random, lower 4 bits reserved
    uint8_t header = Cryptography::randomnum(256) & 0xF0;

    size_t len = 1 + data.size();
    if (len > HW_MTU) {
        ERROR("SX1262Interface: Packet too large (" + std::to_string(len) + " > " + std::to_string(HW_MTU) + ")");
        return;
    }

    uint8_t* buf = new uint8_t[len];
    buf[0] = header;
    memcpy(buf + 1, data.data(), data.size());

    // Acquire SPI mutex
    if (xSemaphoreTake(_spi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ERROR("SX1262Interface: Failed to acquire SPI mutex for TX");
        delete[] buf;
        return;
    }

    _transmitting = true;

    // Transmit (blocking)
    int16_t state = _radio->transmit(buf, len);

    _transmitting = false;
    xSemaphoreGive(_spi_mutex);
    delete[] buf;

    if (state == RADIOLIB_ERR_NONE) {
        DEBUG("SX1262Interface: Sent " + std::to_string(len) + " bytes");
        // Perform post-send housekeeping
        InterfaceImpl::handle_outgoing(data);
    } else {
        ERROR("SX1262Interface: Transmit failed, code " + std::to_string(state));
    }

    // Return to receive mode
    start_receive();
#endif
}

void SX1262Interface::on_incoming(const Bytes& data) {
    DEBUG(toString() + ": Incoming " + std::to_string(data.size()) + " bytes");
    // Pass received data to transport
    InterfaceImpl::handle_incoming(data);
}

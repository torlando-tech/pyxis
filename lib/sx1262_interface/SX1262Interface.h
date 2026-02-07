// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Interface.h"
#include "Bytes.h"
#include "Type.h"
#include "Cryptography/Random.h"

#ifdef ARDUINO
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

/**
 * SX1262 LoRa Interface for T-Deck Plus
 *
 * Air-compatible with RNode devices using same modulation and packet framing.
 * Uses RadioLib for SX1262 support with DIO1+BUSY pin model.
 */

// T-Deck Plus SX1262 pins (from Hardware::TDeck::Radio)
namespace SX1262Pins {
    constexpr uint8_t CS = 9;
    constexpr uint8_t BUSY = 13;
    constexpr uint8_t RST = 17;
    constexpr uint8_t DIO1 = 45;
    constexpr uint8_t SPI_MISO = 38;
    // Shared with display: MOSI=41, SCK=40
}

/**
 * LoRa configuration parameters.
 * Defaults match RNode for interoperability.
 */
struct SX1262Config {
    float frequency = 927.25f;        // MHz
    float bandwidth = 62.5f;          // kHz (valid: 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500)
    uint8_t spreading_factor = 7;     // SF7-SF12
    uint8_t coding_rate = 5;          // 5=4/5, 6=4/6, 7=4/7, 8=4/8
    int8_t tx_power = 17;             // dBm (2-22)
    uint8_t sync_word = 0x12;         // Standard LoRa sync word
    uint16_t preamble_length = 20;    // symbols
};

class SX1262Interface : public RNS::InterfaceImpl {
public:
    SX1262Interface(const char* name = "LoRa");
    virtual ~SX1262Interface();

    /**
     * Set configuration before calling start().
     * Changes take effect on next start().
     */
    void set_config(const SX1262Config& config);
    const SX1262Config& get_config() const { return _config; }

    // InterfaceImpl interface
    virtual bool start() override;
    virtual void stop() override;
    virtual void loop() override;

    // Status getters (override virtual from InterfaceImpl)
    float get_rssi() const override { return _last_rssi; }
    float get_snr() const override { return _last_snr; }
    bool is_transmitting() const { return _transmitting; }

    virtual std::string toString() const override;

protected:
    virtual void send_outgoing(const RNS::Bytes& data) override;

private:
    void on_incoming(const RNS::Bytes& data);
    void start_receive();

#ifdef ARDUINO
    // RadioLib objects
    SX1262* _radio = nullptr;
    Module* _module = nullptr;

    // SPI for LoRa (shared HSPI bus with display, but with MISO enabled)
    SPIClass* _lora_spi = nullptr;

    // SPI mutex for shared bus with display
    static SemaphoreHandle_t _spi_mutex;
    static bool _mutex_initialized;
#endif

    // Configuration
    SX1262Config _config;

    // State
    bool _transmitting = false;
    float _last_rssi = 0.0f;
    float _last_snr = 0.0f;

    // Receive buffer
    RNS::Bytes _rx_buffer;

    // Hardware MTU (matches existing LoRaInterface)
    static constexpr uint16_t HW_MTU = 508;
};

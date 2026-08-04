#pragma once

#include <cmath>
#include <cstdint>

// Match Python RNS RNodeInterface's bitrate formula while accepting the
// bandwidth unit used by SX1262Config and RadioLib (kHz, not Hz).
inline uint32_t calculate_lora_bitrate_bps(float bandwidth_khz, uint8_t spreading_factor, uint8_t coding_rate) {
    if (bandwidth_khz <= 0.0f || spreading_factor == 0 || coding_rate == 0) {
        return 0;
    }

    const double bandwidth_hz = static_cast<double>(bandwidth_khz) * 1000.0;
    const double symbol_rate = bandwidth_hz / std::pow(2.0, spreading_factor);
    const double coding_efficiency = 4.0 / static_cast<double>(coding_rate);
    return static_cast<uint32_t>(spreading_factor * coding_efficiency * symbol_rate);
}

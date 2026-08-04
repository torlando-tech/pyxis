#include <cstdint>
#include <iostream>

#include "SX1262Bitrate.h"

int main() {
    int passed = 0;
    int failed = 0;

    auto check = [&](const char* name, uint32_t actual, uint32_t expected) {
        if (actual == expected) {
            ++passed;
        } else {
            ++failed;
            std::cerr << name << ": expected " << expected << ", got " << actual << "\n";
        }
    };

    check("k3s RNode settings", calculate_lora_bitrate_bps(500.0f, 7, 5), 21875);
    check("Pyxis default settings", calculate_lora_bitrate_bps(62.5f, 7, 5), 2734);
    check("SF5 settings", calculate_lora_bitrate_bps(500.0f, 5, 5), 62500);
    check("zero bandwidth", calculate_lora_bitrate_bps(0.0f, 7, 5), 0);
    check("zero spreading factor", calculate_lora_bitrate_bps(500.0f, 0, 5), 0);
    check("zero coding rate", calculate_lora_bitrate_bps(500.0f, 7, 0), 0);

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

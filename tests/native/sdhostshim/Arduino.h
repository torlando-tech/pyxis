// Host shim for <Arduino.h> — compiles the real T-Deck SD storage sources
// (MapTileStoreSD.cpp, SDAccess.cpp) unmodified on x86.
//
// MapTileStoreSD.cpp uses Arduino.h only for fs::File (pulled via SD.h);
// SDAccess.cpp uses pinMode/digitalWrite/Serial, modelled here as no-ops.
// The ARDUINO macro itself is supplied by the test driver (-DARDUINO=100).
#ifndef SDHOSTSHIM_ARDUINO_H
#define SDHOSTSHIM_ARDUINO_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1

namespace {

struct HostSerial {
    template <typename... Args>
    void println(const char* const format, Args&&...) {
        std::fputs(format ? format : "\n", stderr);
        std::fputc('\n', stderr);
    }
    void println() { std::fputc('\n', stderr); }
    void printf(const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
    }
};

HostSerial __attribute__((unused)) host_serial;

} // namespace

#define Serial host_serial

inline void pinMode(std::uint8_t, std::uint8_t) {}
inline void digitalWrite(std::uint8_t, std::uint8_t) {}
inline void delay(std::uint32_t) {}

#endif

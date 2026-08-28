// Host shim for <SPI.h> — compiles the real T-Deck SD sources (SDAccess.cpp)
// unmodified on x86. Only the call sites used by SDAccess::init are modelled.
#ifndef SDHOSTSHIM_SPI_H
#define SDHOSTSHIM_SPI_H

#include <cstdint>

class HostSPI {
public:
    void begin(std::uint8_t, std::uint8_t, std::uint8_t) {}
};

extern HostSPI SPI;

#endif

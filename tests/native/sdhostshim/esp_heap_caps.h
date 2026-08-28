// Host shim for <esp_heap_caps.h> — MapTilePack.cpp selects PSRAM-backed
// buffers only on ARDUINO_ARCH_ESP32; on the host the static fallback
// members are used and these are never called. Provided for completeness.
#ifndef SDHOSTSHIM_ESP_HEAP_CAPS_H
#define SDHOSTSHIM_ESP_HEAP_CAPS_H

#include <cstdlib>
#include <cstdint>

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2

inline void* heap_caps_malloc(std::size_t size, std::uint32_t) { return std::malloc(size); }
inline void heap_caps_free(void* pointer) { std::free(pointer); }

#endif

// Host shim for <freertos/FreeRTOS.h> — compiles the real T-Deck SD sources
// (SDAccess.cpp) unmodified on x86. Models the shared SPI bus mutex as a
// plain always-acquirable semaphore; the tests exercise storage logic, not
// bus arbitration.
#ifndef SDHOSTSHIM_FREERTOS_FREERTOS_H
#define SDHOSTSHIM_FREERTOS_FREERTOS_H

#include <cstddef>
#include <cstdint>

typedef void* SemaphoreHandle_t;
typedef long BaseType_t;
typedef unsigned long TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFU

extern SemaphoreHandle_t hostsd_create_mutex(void);

#endif

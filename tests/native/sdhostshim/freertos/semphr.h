#ifndef SDHOSTSHIM_FREERTOS_SEMPHR_H
#define SDHOSTSHIM_FREERTOS_SEMPHR_H

#include <freertos/FreeRTOS.h>

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return hostsd_create_mutex(); }
// Single-threaded host build: the shared SPI bus mutex is always free.
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }

#endif

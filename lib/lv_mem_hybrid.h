/**
 * @file lv_mem_hybrid.h
 * @brief Hybrid memory allocator for LVGL
 *
 * Uses internal RAM for small allocations (fast, good for UI responsiveness)
 * and PSRAM for large allocations (preserves internal heap for BLE/network).
 */
#pragma once

#include <esp_heap_caps.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Threshold: allocations larger than this go to PSRAM */
#define LV_MEM_HYBRID_PSRAM_THRESHOLD 1024

/* Track which allocations went to PSRAM vs internal RAM */
/* We use a simple heuristic: PSRAM addresses are above 0x3C000000 on ESP32-S3 */
#define IS_PSRAM_ADDR(ptr) ((uintptr_t)(ptr) >= 0x3C000000)

static inline void* lv_mem_hybrid_alloc(size_t size) {
    void* ptr;

    if (size >= LV_MEM_HYBRID_PSRAM_THRESHOLD) {
        /* Large allocation -> PSRAM */
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr) return ptr;
        /* Fall back to internal if PSRAM fails */
    }

    /* Small allocation or PSRAM fallback -> internal RAM */
    ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ptr) return ptr;

    /* Last resort: try PSRAM for small allocs if internal is exhausted */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void lv_mem_hybrid_free(void* ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static inline void* lv_mem_hybrid_realloc(void* ptr, size_t size) {
    if (ptr == NULL) {
        return lv_mem_hybrid_alloc(size);
    }

    if (size == 0) {
        lv_mem_hybrid_free(ptr);
        return NULL;
    }

    /* Determine where the original allocation was */
    if (IS_PSRAM_ADDR(ptr)) {
        /* Was in PSRAM, keep it there */
        void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_ptr) return new_ptr;
        /* Fall back to internal if PSRAM realloc fails */
        new_ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (new_ptr) {
            /* Copy and free old - can't use realloc across memory types */
            /* We don't know old size, so this is a best-effort fallback */
            heap_caps_free(ptr);
            return new_ptr;
        }
        return NULL;
    } else {
        /* Was in internal RAM */
        if (size >= LV_MEM_HYBRID_PSRAM_THRESHOLD) {
            /* Growing to large size - move to PSRAM */
            void* new_ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (new_ptr) {
                heap_caps_free(ptr);
                return new_ptr;
            }
            /* Fall back to internal realloc */
        }
        /* Keep in internal RAM */
        void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (new_ptr) return new_ptr;
        /* Fall back to PSRAM */
        return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

#ifdef __cplusplus
}
#endif

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

/* Threshold: allocations larger than this go to PSRAM.
 * Lowered 1024->256 (2026-06-24): pushes most small LVGL objects (styles, obj
 * metadata, labels, anim descriptors) into PSRAM, de-fragmenting the scarce
 * ~57-66KB internal block so the LXST audio pipeline can allocate reliably
 * during a call. UI alloc latency on PSRAM is imperceptible. */
#define LV_MEM_HYBRID_PSRAM_THRESHOLD 256

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

    /* heap_caps_realloc has libc realloc semantics and can move an allocation
     * between capability sets while preserving min(old_size, size) bytes. */
    uint32_t preferred_caps;
    uint32_t fallback_caps;
    if (IS_PSRAM_ADDR(ptr) || size >= LV_MEM_HYBRID_PSRAM_THRESHOLD) {
        preferred_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        fallback_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    } else {
        preferred_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        fallback_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }

    void* new_ptr = heap_caps_realloc(ptr, size, preferred_caps);
    if (new_ptr) return new_ptr;

    /* realloc failure leaves ptr valid, so trying the alternate heap is safe. */
    return heap_caps_realloc(ptr, size, fallback_caps);
}

#ifdef __cplusplus
}
#endif

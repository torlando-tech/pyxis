/*
 * MemoryMonitor - Runtime memory instrumentation for ESP32-S3
 *
 * Implementation uses ESP-IDF heap_caps APIs for heap monitoring
 * and FreeRTOS APIs for task stack monitoring.
 *
 * Key design decisions:
 *   - Static buffers only (no dynamic allocation in instrumentation)
 *   - FreeRTOS software timer for periodic logging (runs in daemon task)
 *   - Explicit task registry (uxTaskGetSystemState unavailable in Arduino)
 *   - Separate internal RAM and PSRAM metrics
 */

#include "MemoryMonitor.h"

#ifdef MEMORY_INSTRUMENTATION_ENABLED

#include <Log.h>
#include <BytesPool.h>

#include <cstring>

namespace RNS { namespace Instrumentation {

// Maximum number of tasks that can be registered for stack monitoring
static constexpr size_t MAX_MONITORED_TASKS = 16;

// Task registry entry
struct TaskEntry {
    TaskHandle_t handle;
    char name[configMAX_TASK_NAME_LEN];
};

// Static task registry (no dynamic allocation)
static TaskEntry _task_registry[MAX_MONITORED_TASKS];
static size_t _task_count = 0;

// Static member initialization
TimerHandle_t MemoryMonitor::_timer = nullptr;
bool MemoryMonitor::_verbose = false;

// Static buffer for log formatting (avoid stack allocation in callbacks)
static char _log_buffer[256];
static char _stack_buffer[256];


bool MemoryMonitor::init(uint32_t interval_ms) {
    // Don't reinitialize if already running
    if (_timer != nullptr) {
        WARNING("[MEM_MON] Already initialized, stop() first to reinit");
        return false;
    }

    // Create FreeRTOS software timer
    _timer = xTimerCreate(
        "mem_mon",                          // Timer name (for debugging)
        pdMS_TO_TICKS(interval_ms),         // Period in ticks
        pdTRUE,                             // Auto-reload (repeat)
        nullptr,                            // Timer ID (unused)
        timerCallback                       // Callback function
    );

    if (_timer == nullptr) {
        ERROR("[MEM_MON] Failed to create timer");
        return false;
    }

    // Start the timer
    if (xTimerStart(_timer, 0) != pdPASS) {
        ERROR("[MEM_MON] Failed to start timer");
        xTimerDelete(_timer, 0);
        _timer = nullptr;
        return false;
    }

    NOTICEF("[MEM_MON] Started (interval=%ums)", interval_ms);

    // Log initial state immediately
    logHeapStats();

    return true;
}


void MemoryMonitor::stop() {
    if (_timer != nullptr) {
        xTimerStop(_timer, 0);
        xTimerDelete(_timer, 0);
        _timer = nullptr;
        NOTICE("[MEM_MON] Stopped");
    }
}


void MemoryMonitor::registerTask(TaskHandle_t handle, const char* name) {
    if (handle == nullptr) {
        WARNING("[MEM_MON] Cannot register null task handle");
        return;
    }

    if (_task_count >= MAX_MONITORED_TASKS) {
        WARNINGF("[MEM_MON] Task registry full, cannot register '%s'", name);
        return;
    }

    // Check for duplicate
    for (size_t i = 0; i < _task_count; i++) {
        if (_task_registry[i].handle == handle) {
            VERBOSEF("[MEM_MON] Task '%s' already registered", name);
            return;
        }
    }

    // Add to registry
    _task_registry[_task_count].handle = handle;
    strncpy(_task_registry[_task_count].name, name, configMAX_TASK_NAME_LEN - 1);
    _task_registry[_task_count].name[configMAX_TASK_NAME_LEN - 1] = '\0';
    _task_count++;

    VERBOSEF("[MEM_MON] Registered task '%s' (%u/%u)",
             name, _task_count, MAX_MONITORED_TASKS);
}


void MemoryMonitor::unregisterTask(TaskHandle_t handle) {
    for (size_t i = 0; i < _task_count; i++) {
        if (_task_registry[i].handle == handle) {
            VERBOSEF("[MEM_MON] Unregistered task '%s'", _task_registry[i].name);

            // Shift remaining entries to fill gap
            for (size_t j = i; j < _task_count - 1; j++) {
                _task_registry[j] = _task_registry[j + 1];
            }
            _task_count--;
            return;
        }
    }
}


void MemoryMonitor::setVerbose(bool verbose) {
    _verbose = verbose;
    VERBOSEF("[MEM_MON] Verbose mode %s", verbose ? "enabled" : "disabled");
}


void MemoryMonitor::logNow() {
    logHeapStats();
    if (_task_count > 0) {
        logTaskStacks();
    }
}


void MemoryMonitor::timerCallback(TimerHandle_t timer) {
    (void)timer;  // Unused parameter

    logHeapStats();
    if (_task_count > 0) {
        logTaskStacks();
    }
}


void MemoryMonitor::logHeapStats() {
    // Internal RAM statistics (critical for stability)
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    // Calculate internal RAM fragmentation percentage
    // Fragmentation = 100 - (largest_block / total_free * 100)
    // Higher fragmentation = smaller largest block relative to free space
    uint8_t internal_frag = 0;
    if (internal_free > 0) {
        internal_frag = 100 - (uint8_t)((internal_largest * 100) / internal_free);
    }

    // PSRAM statistics
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    // Calculate PSRAM fragmentation
    uint8_t psram_frag = 0;
    if (psram_free > 0) {
        psram_frag = 100 - (uint8_t)((psram_largest * 100) / psram_free);
    }

    if (_verbose) {
        // Verbose format: separate lines with labels
        NOTICEF("[HEAP] Internal: free=%u largest=%u min=%u frag=%u%%",
                internal_free, internal_largest, internal_min, internal_frag);
        NOTICEF("[HEAP] PSRAM:    free=%u largest=%u min=%u frag=%u%%",
                psram_free, psram_largest, psram_min, psram_frag);
    } else {
        // Compact format: single line for parsing/graphing
        snprintf(_log_buffer, sizeof(_log_buffer),
                 "[HEAP] int_free=%u int_largest=%u int_min=%u int_frag=%u%% "
                 "psram_free=%u psram_largest=%u",
                 internal_free, internal_largest, internal_min, internal_frag,
                 psram_free, psram_largest);
        NOTICE(_log_buffer);
    }

    // Warn if fragmentation is problematic (>50% is concerning)
    if (internal_frag > 50) {
        WARNINGF("[HEAP] Internal RAM fragmentation high: %u%%", internal_frag);
    }

    // Warn if minimum free dropped significantly (memory pressure)
    if (internal_min < 10000) {
        WARNINGF("[HEAP] Internal RAM watermark low: %u bytes", internal_min);
    }

    // BytesPool stats - shows actual pool usage
    auto& pool = BytesPool::instance();
    NOTICEF("[POOL] tiny=%zu/%zu small=%zu/%zu med=%zu/%zu large=%zu/%zu "
            "hits=%zu misses=%zu fallbacks=%zu",
            pool.tiny_in_use(), BytesPoolConfig::TINY_SLOTS,
            pool.small_in_use(), BytesPoolConfig::SMALL_SLOTS,
            pool.medium_in_use(), BytesPoolConfig::MEDIUM_SLOTS,
            pool.large_in_use(), BytesPoolConfig::LARGE_SLOTS,
            pool.pool_hits(), pool.pool_misses(), pool.fallback_count());
}


void MemoryMonitor::logTaskStacks() {
    if (_task_count == 0) {
        return;
    }

    size_t offset = 0;

    // Build compact stack report: "task1=Nbytes task2=Nbytes ..."
    offset = snprintf(_stack_buffer, sizeof(_stack_buffer), "[STACK] ");

    for (size_t i = 0; i < _task_count && offset < sizeof(_stack_buffer) - 40; i++) {
        // Get stack high water mark (minimum free stack since task start)
        // Returns value in words (4 bytes on ESP32)
        UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(_task_registry[i].handle);
        uint32_t hwm_bytes = hwm_words * 4;

        offset += snprintf(_stack_buffer + offset, sizeof(_stack_buffer) - offset,
                           "%s=%u ", _task_registry[i].name, hwm_bytes);
    }

    NOTICE(_stack_buffer);

    // Warn about low stack (less than 256 bytes remaining is dangerous)
    for (size_t i = 0; i < _task_count; i++) {
        UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(_task_registry[i].handle);
        uint32_t hwm_bytes = hwm_words * 4;
        if (hwm_bytes < 256) {
            WARNINGF("[STACK] Task '%s' stack low: %u bytes remaining",
                     _task_registry[i].name, hwm_bytes);
        }
    }
}

}} // namespace RNS::Instrumentation

#endif // MEMORY_INSTRUMENTATION_ENABLED

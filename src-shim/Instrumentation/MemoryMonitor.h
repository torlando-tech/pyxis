#pragma once

/*
 * MemoryMonitor - Runtime memory instrumentation for ESP32-S3
 *
 * Provides heap/stack monitoring capabilities with periodic logging.
 * All functionality is guarded by MEMORY_INSTRUMENTATION_ENABLED build flag.
 *
 * Usage:
 *   1. Define MEMORY_INSTRUMENTATION_ENABLED in build flags
 *   2. Call MemoryMonitor::init() at startup
 *   3. Optionally register tasks for stack monitoring
 *   4. Monitor logs for heap fragmentation and stack usage
 *
 * When disabled, all API calls compile to no-ops via stub macros.
 */

#ifdef MEMORY_INSTRUMENTATION_ENABLED

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#include <cstdint>
#include <cstddef>

namespace RNS { namespace Instrumentation {

/**
 * MemoryMonitor - Static class for runtime memory monitoring
 *
 * Monitors:
 *   - Internal RAM: free, largest block, minimum free (watermark), fragmentation %
 *   - PSRAM: free, largest block
 *   - Task stack high water marks for registered tasks
 *
 * All methods are static - no instantiation required.
 */
class MemoryMonitor {
public:
    /**
     * Initialize and start periodic monitoring
     *
     * @param interval_ms Logging interval in milliseconds (default 30 seconds)
     * @return true if timer created and started successfully
     */
    static bool init(uint32_t interval_ms = 30000);

    /**
     * Stop the monitoring timer
     *
     * Safe to call even if not initialized.
     */
    static void stop();

    /**
     * Register a task for stack monitoring
     *
     * @param handle FreeRTOS task handle
     * @param name Human-readable name for logging
     *
     * Note: Maximum 16 tasks can be registered. Additional registrations
     * are silently ignored.
     */
    static void registerTask(TaskHandle_t handle, const char* name);

    /**
     * Unregister a task from stack monitoring
     *
     * @param handle FreeRTOS task handle to remove
     */
    static void unregisterTask(TaskHandle_t handle);

    /**
     * Toggle verbose output mode
     *
     * When verbose:
     *   - Additional metrics may be logged
     *   - Format may be more detailed
     *
     * @param verbose true for verbose output
     */
    static void setVerbose(bool verbose);

    /**
     * Trigger immediate log output
     *
     * Useful for debugging or capturing state at specific points.
     * Does not affect the periodic timer.
     */
    static void logNow();

private:
    /**
     * FreeRTOS timer callback
     *
     * Called by the timer daemon task at each interval.
     */
    static void timerCallback(TimerHandle_t timer);

    /**
     * Log heap statistics for internal RAM and PSRAM
     */
    static void logHeapStats();

    /**
     * Log stack high water marks for all registered tasks
     */
    static void logTaskStacks();

    // Static members
    static TimerHandle_t _timer;
    static bool _verbose;
};

}} // namespace RNS::Instrumentation

// Convenience macros for cleaner integration
#define MEMORY_MONITOR_INIT(interval) RNS::Instrumentation::MemoryMonitor::init(interval)
#define MEMORY_MONITOR_REGISTER_TASK(handle, name) RNS::Instrumentation::MemoryMonitor::registerTask(handle, name)
#define MEMORY_MONITOR_UNREGISTER_TASK(handle) RNS::Instrumentation::MemoryMonitor::unregisterTask(handle)
#define MEMORY_MONITOR_LOG_NOW() RNS::Instrumentation::MemoryMonitor::logNow()
#define MEMORY_MONITOR_STOP() RNS::Instrumentation::MemoryMonitor::stop()

#else // MEMORY_INSTRUMENTATION_ENABLED not defined

// Stub macros - compile to no-ops when instrumentation disabled
#define MEMORY_MONITOR_INIT(interval) ((void)0)
#define MEMORY_MONITOR_REGISTER_TASK(handle, name) ((void)0)
#define MEMORY_MONITOR_UNREGISTER_TASK(handle) ((void)0)
#define MEMORY_MONITOR_LOG_NOW() ((void)0)
#define MEMORY_MONITOR_STOP() ((void)0)

#endif // MEMORY_INSTRUMENTATION_ENABLED

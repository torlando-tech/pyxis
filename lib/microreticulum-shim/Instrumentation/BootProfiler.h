#pragma once

/*
 * BootProfiler - Boot sequence timing instrumentation for ESP32
 *
 * Provides phase-level timing for boot sequence analysis, distinguishing
 * between initialization time (CPU work) and blocking wait time (I/O waits).
 *
 * Usage:
 *   1. Define BOOT_PROFILING_ENABLED in build flags
 *   2. Use BOOT_PROFILE_START("phase") at the beginning of each init phase
 *   3. Use BOOT_PROFILE_END("phase") when phase completes
 *   4. Use BOOT_PROFILE_WAIT_START/END for blocking waits within phases
 *   5. Call BOOT_PROFILE_COMPLETE() when boot is finished
 *
 * Output includes:
 *   - Per-phase duration and cumulative time
 *   - Separate tracking of init vs wait time
 *   - Final summary with breakdown
 *
 * When disabled, all API calls compile to no-ops via stub macros.
 */

#ifdef BOOT_PROFILING_ENABLED

#include <cstdint>
#include <cstddef>

namespace RNS { namespace Instrumentation {

/**
 * BootProfiler - Static class for boot sequence timing
 *
 * Tracks:
 *   - Boot start time (first markStart call)
 *   - Per-phase duration with cumulative totals
 *   - Blocking wait time separate from init time
 *   - Total boot duration on bootComplete()
 *
 * All methods are static - no instantiation required.
 */
class BootProfiler {
public:
    /**
     * Mark the start of a boot phase
     *
     * First call also records boot start time.
     *
     * @param phase Human-readable phase name (e.g., "LVGL", "WiFi", "Reticulum")
     */
    static void markStart(const char* phase);

    /**
     * Mark the end of a boot phase
     *
     * Logs phase duration and cumulative boot time.
     *
     * @param phase Phase name (should match corresponding markStart)
     */
    static void markEnd(const char* phase);

    /**
     * Mark the start of a blocking wait within a phase
     *
     * Use for I/O waits, network connection, etc. that are not CPU work.
     *
     * @param phase Phase/operation name for the wait
     */
    static void markWaitStart(const char* phase);

    /**
     * Mark the end of a blocking wait
     *
     * Wait time is tracked separately from init time.
     *
     * @param phase Phase/operation name (should match corresponding markWaitStart)
     */
    static void markWaitEnd(const char* phase);

    /**
     * Mark boot sequence as complete
     *
     * Logs summary with total time, init time, and wait time breakdown.
     * Call when UI is responsive (LVGL interactive).
     */
    static void bootComplete();

    /**
     * Get total milliseconds since boot started
     *
     * @return Cumulative boot time in milliseconds, 0 if not started
     */
    static uint32_t getTotalMs();

    /**
     * Get total init time (excluding waits)
     *
     * @return Init time in milliseconds
     */
    static uint32_t getInitMs();

    /**
     * Get total wait time
     *
     * @return Accumulated wait time in milliseconds
     */
    static uint32_t getWaitMs();

    /**
     * Set filesystem ready state
     *
     * Must be called after SPIFFS is mounted before saveToFile() can work.
     *
     * @param ready True if filesystem is ready for writes
     */
    static void setFilesystemReady(bool ready);

    /**
     * Save boot profile to SPIFFS file
     *
     * Writes boot timing data to /boot_1.log, rotating existing files
     * (max 5 boot logs retained). Requires filesystem to be ready.
     *
     * @return True if file was saved successfully
     */
    static bool saveToFile();

private:
    // Maximum number of boot log files to retain
    static const uint8_t MAX_BOOT_LOGS = 5;
    // Boot timing
    static uint32_t _boot_start_ms;
    static uint32_t _cumulative_ms;

    // Current phase tracking
    static char _current_phase[32];
    static uint32_t _phase_start_ms;

    // Wait time tracking
    static char _current_wait[32];
    static uint32_t _wait_start_ms;
    static uint32_t _total_wait_ms;

    // Static buffer for log formatting (avoid stack allocation)
    static char _log_buffer[256];

    // Filesystem ready flag
    static bool _fs_ready;
};

}} // namespace RNS::Instrumentation

// Convenience macros for cleaner integration
#define BOOT_PROFILE_START(phase) RNS::Instrumentation::BootProfiler::markStart(phase)
#define BOOT_PROFILE_END(phase) RNS::Instrumentation::BootProfiler::markEnd(phase)
#define BOOT_PROFILE_WAIT_START(phase) RNS::Instrumentation::BootProfiler::markWaitStart(phase)
#define BOOT_PROFILE_WAIT_END(phase) RNS::Instrumentation::BootProfiler::markWaitEnd(phase)
#define BOOT_PROFILE_COMPLETE() RNS::Instrumentation::BootProfiler::bootComplete()
#define BOOT_PROFILE_SAVE() RNS::Instrumentation::BootProfiler::saveToFile()

#else // BOOT_PROFILING_ENABLED not defined

// Stub macros - compile to no-ops when profiling disabled
#define BOOT_PROFILE_START(phase) ((void)0)
#define BOOT_PROFILE_END(phase) ((void)0)
#define BOOT_PROFILE_WAIT_START(phase) ((void)0)
#define BOOT_PROFILE_WAIT_END(phase) ((void)0)
#define BOOT_PROFILE_COMPLETE() ((void)0)
#define BOOT_PROFILE_SAVE() ((void)0)

#endif // BOOT_PROFILING_ENABLED

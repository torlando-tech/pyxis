// Native-test shim for microReticulum's Utilities/OS.h.
//
// BLEReassembler uses RNS::Utilities::OS::time() to get a wall-clock seconds
// value for reassembly timeout tracking. The real impl pulls in Arduino /
// FreeRTOS APIs we don't have natively.
//
// For tests we expose a controllable clock so timeout behavior can be
// exercised deterministically without sleep().
#pragma once

#include <chrono>

namespace RNS {
namespace Utilities {

class OS {
public:
    // If a fake clock has been installed via set_fake_time, return that;
    // otherwise return monotonic seconds since the program started.
    static double time() {
        if (_fake_time_set) return _fake_time;
        using clk = std::chrono::steady_clock;
        static const auto t0 = clk::now();
        auto now = clk::now();
        return std::chrono::duration<double>(now - t0).count();
    }

    static void set_fake_time(double seconds) {
        _fake_time = seconds;
        _fake_time_set = true;
    }

    static void clear_fake_time() {
        _fake_time_set = false;
    }

private:
    inline static double _fake_time = 0.0;
    inline static bool _fake_time_set = false;
};

}  // namespace Utilities
}  // namespace RNS

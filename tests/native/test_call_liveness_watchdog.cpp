#include <cstdint>
#include <iostream>

#include "../../lib/tdeck_ui/UI/LXMF/CallLivenessWatchdog.h"

namespace {
int passed = 0;
int failed = 0;

void expect(bool condition, const char* name) {
    if (condition) {
        ++passed;
    } else {
        ++failed;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    constexpr uint32_t timeout = 90000;
    CallLivenessWatchdog watchdog;

    expect(!watchdog.expired(100000, timeout), "disarmed watchdog does not expire");

    watchdog.arm(1000);
    expect(!watchdog.expired(90999, timeout), "active just below timeout");
    expect(watchdog.expired(91000, timeout), "active expires at timeout");

    watchdog.observe(50000);
    expect(!watchdog.expired(100000, timeout), "received media resets deadline");
    expect(watchdog.expired(140000, timeout), "reset deadline eventually expires");

    watchdog.disarm();
    expect(!watchdog.expired(UINT32_MAX, timeout), "disarm suppresses expiry");

    watchdog.arm(UINT32_MAX - 1000);
    expect(!watchdog.expired(1000, timeout), "millis wrap below timeout");
    expect(watchdog.expired(90000, timeout), "millis wrap reaches timeout");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

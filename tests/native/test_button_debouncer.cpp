#include <cstdint>
#include <iostream>
#include "ButtonDebouncer.h"

using Hardware::TDeck::ButtonDebouncer;

static int passed = 0;
static int failed = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failed; std::cerr << "FAIL line " << __LINE__ << ": " #expr "\n"; } } while (0)

int main() {
    ButtonDebouncer d(50);
    CHECK(!d.update(false, 0));
    CHECK(!d.changed());

    // A 30 ms active-low noise pulse must never become a press or release.
    CHECK(!d.update(true, 100));
    CHECK(!d.update(true, 129));
    CHECK(!d.update(false, 130));
    CHECK(!d.changed());

    // Both edges must remain stable for the full debounce interval.
    CHECK(!d.update(true, 200));
    CHECK(!d.update(true, 249));
    CHECK(d.update(true, 250));
    CHECK(d.changed());
    CHECK(d.update(false, 260));
    CHECK(d.update(false, 309));
    CHECK(!d.update(false, 310));
    CHECK(d.changed());

    // Unsigned subtraction keeps the interval correct across millis() wrap.
    ButtonDebouncer wrap(50);
    CHECK(!wrap.update(false, 0xFFFFFFE0u));
    CHECK(!wrap.update(true, 0xFFFFFFF0u));
    CHECK(!wrap.update(true, 0x00000020u));
    CHECK(wrap.update(true, 0x00000022u));
    CHECK(wrap.changed());

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

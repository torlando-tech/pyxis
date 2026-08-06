#include <cassert>

#include "lib/tdeck_ui/Hardware/TDeck/DisplayRefreshRetry.h"

int main() {
    Hardware::TDeck::DisplayRefreshRetry retry;

    assert(!retry.consume());

    retry.mark_failed();
    assert(retry.consume());
    assert(!retry.consume());

    // Multiple dropped regions before the refresh cycle completes coalesce
    // into one full-screen retry on the next LVGL cycle.
    retry.mark_failed();
    retry.mark_failed();
    assert(retry.consume());
    assert(!retry.consume());

    return 0;
}

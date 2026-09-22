// Host test: NomadNet page-image loader (slice 3).
// Verifies the reference behavior (Browser.update_images / __load_image,
// image_loading policy, parse_url destination resolution) from the
// RNS-synced NomadNet at e1e8ab8 / v1.4.3: sequential one-at-a-time load,
// policy gating, link-reuse destination resolution, per-image terminal
// failure, manual reload, and generation cancellation.
#include <cstdint>
#include <cstdio>
#include <string>

#include "NomadNetImageLoader.h"

using namespace UI::LXMF::NomadNet;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); ++failures; } \
} while (0)

static const char* PAGE_HEX = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static ImageLoadEntry entry(uint16_t index, const std::string& path, bool same) {
    ImageLoadEntry e;
    e.image_index = index;
    e.same_destination = same;
    e.path = path;
    if (!same) e.destination_hex = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    return e;
}

static void test_policy_gate() {
    // AUTO + loopback -> always admitted.
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        CHECK(l.configure(1, e, PAGE_HEX, 1, ImagePolicy::AUTO, true, 0, 0), "configure ok");
        CHECK(l.state_of(0) == ImageState::PENDING, "auto+loopback admitted");
    }
    // AUTO + low RTT + high EDR -> admitted.
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::AUTO, false, 100, 20000);
        CHECK(l.state_of(0) == ImageState::PENDING, "auto good-link admitted");
    }
    // AUTO + high RTT -> skipped (reference: images don't auto-load on
    // low-bandwidth carriers).
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::AUTO, false, 2000, 20000);
        CHECK(l.state_of(0) == ImageState::SKIPPED, "auto high-rtt skipped");
    }
    // AUTO + low EDR -> skipped.
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::AUTO, false, 100, 5000);
        CHECK(l.state_of(0) == ImageState::SKIPPED, "auto low-edr skipped");
    }
    // ALWAYS -> admitted regardless of link health.
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 99999, 1);
        CHECK(l.state_of(0) == ImageState::PENDING, "always admitted");
    }
    // NEVER -> never admitted.
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::NEVER, true, 0, 0);
        CHECK(l.state_of(0) == ImageState::SKIPPED, "never skipped");
    }
    // MANUAL -> skipped at configure, admitted by request_images().
    {
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::MANUAL, true, 0, 0);
        CHECK(l.state_of(0) == ImageState::SKIPPED, "manual skipped at configure");
        CHECK(l.request_images() == 1, "manual re-admits");
        CHECK(l.state_of(0) == ImageState::PENDING, "manual now pending");
    }
}

static void test_sequential_and_link_resolution() {
    ImageLoader l;
    ImageLoadEntry e[2] = {
        entry(0, "/media/a.webp", true),
        entry(1, "/media/b.webp", true),
    };
    CHECK(l.configure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0), "configure ok");
    // Same-destination entries resolve to the page destination (for Link
    // reuse); cache key is derived from it.
    {
        ImageAction a = l.poll();
        CHECK(a == ImageAction::SEND_REQUEST, "first send request");
        const auto* active = l.active_entry();
        CHECK(active && active->image_index == 0, "active is image 0");
        CHECK(l.active_destination() == PAGE_HEX, "active resolves to page dest");
        CHECK(active->cache_key == image_cache_key(PAGE_HEX, "/media/a.webp"),
              "cache key from resolved dest");
    }
    // While the first is REQUESTING, poll() must NOT start the second
    // (strictly sequential).
    CHECK(l.poll() == ImageAction::NONE, "no second request while first in flight");
    // Finish first -> LOADED; poll starts second.
    CHECK(l.finish_active(true), "finish first");
    CHECK(l.state_of(0) == ImageState::LOADED, "first loaded");
    CHECK(l.poll() == ImageAction::SEND_REQUEST, "second send request");
    CHECK(l.active_entry()->image_index == 1, "active is image 1");
    // Remote entry keeps its own destination (no page reuse).
    {
        ImageLoader r;
        ImageLoadEntry re[1] = {entry(0, "/media/x.webp", false)};
        re[0].destination_hex = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        r.configure(1, re, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
        r.poll();
        CHECK(r.active_destination() == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
              "remote entry keeps own dest");
    }
    CHECK(l.finish_active(true), "finish second");
    CHECK(l.all_done(), "all done");
    CHECK(l.loaded_count() == 2 && l.failed_count() == 0, "counts");
    CHECK(l.poll() == ImageAction::NONE, "no more requests");
}

static void test_failure_is_per_image() {
    ImageLoader l;
    ImageLoadEntry e[2] = {entry(0, "/media/a.webp", true), entry(1, "/media/b.webp", true)};
    l.configure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
    l.poll(); // start image 0
    // Image 0 fails (e.g. its link closed); it is terminal for that image
    // only and the page continues to image 1.
    l.fail_image(0, true);
    CHECK(l.state_of(0) == ImageState::FAILED, "image 0 failed");
    CHECK(l.poll() == ImageAction::SEND_REQUEST, "advance to image 1 after failure");
    CHECK(l.active_entry()->image_index == 1, "now active is image 1");
    l.finish_active(true);
    CHECK(l.all_done(), "all done");
    CHECK(l.failed_count() == 1 && l.loaded_count() == 1, "mixed counts");
}

static void test_cancellation_and_reconfigure() {
    ImageLoader l;
    ImageLoadEntry e[1] = {entry(0, "/media/a.webp", true)};
    l.configure(1, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
    l.poll();
    l.cancel_all();
    CHECK(l.empty() && l.all_done(), "cancel clears");
    // Reconfigure for a newer page generation.
    ImageLoadEntry e2[1] = {entry(0, "/media/c.webp", true)};
    l.configure(1, e2, PAGE_HEX, 2, ImagePolicy::ALWAYS, false, 0, 0);
    CHECK(l.count() == 1 && l.state_of(0) == ImageState::PENDING, "reconfigured");
}

int main() {
    test_policy_gate();
    test_sequential_and_link_resolution();
    test_failure_is_per_image();
    test_cancellation_and_reconfigure();
    if (failures == 0) {
        std::printf("ALL IMAGE LOADER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}

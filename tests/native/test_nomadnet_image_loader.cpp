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

static void test_partial_refresh_reconfigure() {
    // Scenario: base page has a retained image (index 0) and a region
    // image (index 1). Partial refreshes grow/shrink the region; the
    // loader must pick up new images, preserve loaded ones, and never
    // refetch unchanged ones.
    {
        // Full page: base + region image, both loaded.
        ImageLoader l;
        ImageLoadEntry e[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/r1.webp", true)};
        l.configure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
        l.poll(); l.finish_active(true);   // image 0 loaded
        l.poll(); l.finish_active(true);   // image 1 loaded
        CHECK(l.loaded_count() == 2, "both loaded before refresh");

        // Refresh 1: region shrinks to nothing (only base remains).
        ImageLoadEntry s[1] = {entry(0, "/media/base.webp", true)};
        CHECK(l.reconfigure(1, s, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0),
              "shrink: no orphan (nothing in flight)");
        CHECK(l.count() == 1 && l.state_of(0) == ImageState::LOADED,
              "shrink: base stays LOADED");
        CHECK(l.poll() == ImageAction::NONE, "shrink: nothing to fetch");

        // Refresh 2: region grows to two NEW images (indices 1, 2).
        ImageLoadEntry g[3] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/g1.webp", true),
                              entry(2, "/media/g2.webp", true)};
        CHECK(l.reconfigure(3, g, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0),
              "grow: no orphan");
        CHECK(l.count() == 3, "grow: three entries");
        CHECK(l.state_of(0) == ImageState::LOADED, "grow: base still LOADED");
        CHECK(l.state_of(1) == ImageState::PENDING &&
              l.state_of(2) == ImageState::PENDING, "grow: new images PENDING");
        l.poll();
        CHECK(l.active_entry() && l.active_entry()->image_index == 1,
              "grow: fetch starts at first new image");
        l.finish_active(true);
        l.poll();
        CHECK(l.active_entry()->image_index == 2, "grow: second new image next");
        l.finish_active(true);
        CHECK(l.loaded_count() == 3 && l.all_done(), "grow: all loaded");
    }
    {
        // In-place replacement: index 1 keeps its slot but the region now
        // points at a different file. The old LOADED state must NOT carry
        // over (stale pixels); the new URL must be fetched.
        ImageLoader l;
        ImageLoadEntry e[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/old.webp", true)};
        l.configure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
        l.poll(); l.finish_active(true);
        l.poll(); l.finish_active(true);
        ImageLoadEntry r[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/new.webp", true)};
        CHECK(l.reconfigure(2, r, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0),
              "replace: no orphan");
        CHECK(l.state_of(0) == ImageState::LOADED, "replace: base preserved");
        CHECK(l.state_of(1) == ImageState::PENDING,
              "replace: replaced url re-admitted");
        l.poll();
        CHECK(l.active_entry()->cache_key == image_cache_key(PAGE_HEX, "/media/new.webp"),
              "replace: fetch targets the new url");
    }
    {
        // In-flight orphan: image 1 is REQUESTING when the fragment
        // replaces its url. reconfigure must report the orphan so the
        // caller releases the outstanding receipt.
        ImageLoader l;
        ImageLoadEntry e[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/old.webp", true)};
        l.configure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
        l.poll(); l.finish_active(true);   // base loaded
        l.poll();                          // image 1 REQUESTING
        CHECK(l.state_of(1) == ImageState::REQUESTING, "image 1 in flight");
        ImageLoadEntry r[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/new.webp", true)};
        CHECK(!l.reconfigure(2, r, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0),
              "orphan reported when in-flight url replaced");
        CHECK(l.state_of(1) == ImageState::PENDING,
              "orphaned slot re-admitted with new url");
        CHECK(l.active_position() == 0, "no in-flight after orphan");
    }
    {
        // In-flight preserved: the fragment does not touch the in-flight
        // image. reconfigure must keep it REQUESTING (no orphan, no
        // double request).
        ImageLoader l;
        ImageLoadEntry e[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/r1.webp", true)};
        l.configure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0);
        l.poll(); l.finish_active(true);
        l.poll();                          // image 1 REQUESTING
        CHECK(l.reconfigure(2, e, PAGE_HEX, 1, ImagePolicy::ALWAYS, false, 0, 0),
              "unchanged in-flight: no orphan");
        CHECK(l.state_of(1) == ImageState::REQUESTING, "in-flight preserved");
        CHECK(l.active_position() == 2, "active slot preserved");
        CHECK(l.poll() == ImageAction::NONE, "no second request");
        l.finish_active(true);
        CHECK(l.loaded_count() == 2, "in-flight completes normally");
    }
    {
        // Manual policy: a refresh adds an image; it must wait for the
        // user trigger, and already-loaded images stay loaded.
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/base.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::MANUAL, false, 0, 0);
        CHECK(l.request_images() == 1, "manual trigger admits base");
        l.poll(); l.finish_active(true);
        ImageLoadEntry g[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/new.webp", true)};
        CHECK(l.reconfigure(2, g, PAGE_HEX, 1, ImagePolicy::MANUAL, false, 0, 0),
              "manual reconfigure: no orphan");
        CHECK(l.state_of(0) == ImageState::LOADED, "manual: base stays loaded");
        CHECK(l.state_of(1) == ImageState::SKIPPED,
              "manual: new image waits for trigger");
        CHECK(l.manual_pending(), "manual: pending trigger surfaced");
        CHECK(l.request_images() == 1, "manual: trigger admits new image");
        CHECK(l.state_of(1) == ImageState::PENDING, "manual: new image pending");
    }
    {
        // NEVER policy: refresh adds images but nothing is admitted.
        ImageLoader l;
        ImageLoadEntry e[1] = {entry(0, "/media/base.webp", true)};
        l.configure(1, e, PAGE_HEX, 1, ImagePolicy::NEVER, true, 0, 0);
        ImageLoadEntry g[2] = {entry(0, "/media/base.webp", true),
                              entry(1, "/media/new.webp", true)};
        CHECK(l.reconfigure(2, g, PAGE_HEX, 1, ImagePolicy::NEVER, true, 0, 0),
              "never: no orphan");
        CHECK(l.state_of(1) == ImageState::SKIPPED, "never: new image skipped");
        CHECK(l.poll() == ImageAction::NONE, "never: nothing fetched");
    }
}

int main() {
    test_policy_gate();
    test_sequential_and_link_resolution();
    test_failure_is_per_image();
    test_cancellation_and_reconfigure();
    test_partial_refresh_reconfigure();
    if (failures == 0) {
        std::printf("ALL IMAGE LOADER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}

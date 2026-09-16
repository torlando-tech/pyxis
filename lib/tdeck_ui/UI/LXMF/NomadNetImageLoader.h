// NomadNet page-image loader: bounded core state machine for fetching page
// images (upstream e1e8ab8 / v1.4.3 behavior).
//
// Reference facts driving the design:
//  - Browser.update_images runs a sequential updater: ONE image at a time,
//    in document order, in one background thread.
//  - image_loading policy: never | manual | auto | always. Auto gate:
//    loopback always; otherwise link RTT < 1.5 s AND expected data rate
//    > 10000 bits/s. (On low-bandwidth carriers images effectively never
//    auto-load: that is reference behavior.)
//  - Link handling: reuse the page's ACTIVE Link when the image destination
//    matches; else reuse another image's Link; else a fresh Link (with a
//    15 s path-request throttle). Page images resolve to the page
//    destination (leading ":" in the url), so in practice the page Link is
//    reused.
//  - One request at a time; a failed image is terminal for THAT image only
//    and never fails the page.
//  - Responses are bounded and validated (WebP magic + size cap) before any
//    decode; see NomadNetImageProtocol.h.
//
// The transport (Link::request("/media", ...)) and rendering are supplied by
// UIManager/NomadNetScreen; this type owns the decisions, ordering, policy
// gate, cancellation, and per-image terminal state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "NomadNetImageProtocol.h"

namespace UI::LXMF::NomadNet {

enum class ImagePolicy : uint8_t {
    NEVER = 0,
    MANUAL,   // only when the user explicitly requests a reload of images
    AUTO,     // reference default
    ALWAYS,
};

// One image to load, in document order.
struct ImageLoadEntry {
    uint16_t image_index = 0;      // index into the compact page images()
    bool same_destination = false; // url had a leading ":"
    std::string destination_hex;   // 32 hex chars; empty when same_destination
    std::string path;              // "/media/x.webp"
    std::string cache_key;         // image_cache_key(resolved_dest, path)
};

enum class ImageState : uint8_t {
    SKIPPED = 0,  // policy rejected or url invalid at configure time
    PENDING,      // admitted, waiting for its turn
    REQUESTING,   // the single outstanding request is this image
    LOADED,       // response validated (and, where wired, decoded)
    FAILED,       // terminal per-image failure; the page is unaffected
};

enum class ImageAction : uint8_t {
    NONE = 0,
    SEND_REQUEST,  // transport should issue /media for the active entry
};

class ImageLoader {
public:
    static constexpr std::size_t CAPACITY = 16;
    // Reference path-request throttle: 15 seconds.
    static constexpr uint32_t PATH_THROTTLE_MS = 15000;
    // Reference auto-load gate: RTT < 1.5 s.
    static constexpr uint32_t AUTO_MAX_RTT_MS = 1500;
    // Reference auto-load gate: expected data rate > 10000 bits/s.
    static constexpr uint32_t AUTO_MIN_EDR_BPS = 10000;

    // Decide the reference auto gate. loopback (own node) always loads.
    static bool auto_gate_allows(bool loopback, uint32_t rtt_ms, uint32_t edr_bps) {
        if (loopback) return true;
        return rtt_ms < AUTO_MAX_RTT_MS && edr_bps > AUTO_MIN_EDR_BPS;
    }

    // Decide whether the active page Link can carry this image's request.
    // Mirrors OwnerController::retain_active_link semantics.
    static bool can_use_page_link(const std::string& image_destination,
                                  const std::string& page_destination,
                                  bool page_link_active) {
        return page_link_active && !image_destination.empty() &&
               image_destination == page_destination;
    }

    ImageLoader() = default;
    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // Admit the page's images after a successful page application.
    //  - page_generation: used for cancellation (a newer generation
    //    invalidates everything here).
    //  - entries: this page's images in document order (bounded by CAPACITY
    //    at the parser; anything beyond is not passed in).
    //  - resolved_destination: the page destination hex; same-destination
    //    entries (leading ":") resolve to it.
    //  - policy/loopback/rtt/edr: the load gate. MANUAL means the user
    //    explicitly asked, so it passes the gate.
    // Returns false on bad_alloc (caller must treat as "images not loaded",
    // never as a page failure).
    bool configure(std::size_t image_count, const ImageLoadEntry* entries,
                   const std::string& resolved_destination,
                   std::uint32_t page_generation, ImagePolicy policy,
                   bool loopback, std::uint32_t rtt_ms, std::uint32_t edr_bps);

    // Advance the sequential loader. Exactly one entry may be REQUESTING.
    // Returns the action for the transport: SEND_REQUEST for the active
    // entry (whose path is active_path() / active_destination()) or NONE.
    // Manual reload: MANUAL policy images start SKIPPED; request_images()
    // re-admits them.
    ImageAction poll();

    // User-initiated image reload (reference "manual" mode): re-admit every
    // SKIPPED entry that is not terminal. Returns the number re-admitted.
    std::size_t request_images();

    // Terminal outcomes for the active entry. Response bytes must have
    // already passed NomadNetImageProtocol::normalize_image_response (or be
    // being streamed); this records the outcome. Any image can be reported
    // failed individually (e.g. its Link closed) without affecting the page.
    bool finish_active(bool success);
    void fail_image(uint16_t image_index, bool terminal = true);

    // Cancellation: navigation to another page / generation change.
    void cancel_all();

    // --- State queries (for the screen) ---
    std::size_t count() const { return _count; }
    std::uint32_t generation() const { return _generation; }
    ImageState state_of(uint16_t image_index) const;
    // The entry currently REQUESTING (valid when state_of says REQUESTING).
    const ImageLoadEntry* active_entry() const;
    std::size_t loaded_count() const;
    std::size_t failed_count() const;
    bool all_done() const;
    bool empty() const { return _count == 0; }
    // The active entry's resolved destination for Link-reuse decisions.
    std::string active_destination() const;

private:
    struct Entry {
        ImageLoadEntry data;
        ImageState state = ImageState::SKIPPED;
        bool terminal_failure = false;
    };
    void reset_entries();
    Entry* find(std::size_t index) const;
    std::size_t _count = 0;
    std::uint32_t _generation = 0;
    std::uint32_t _active = 0; // index into _entries, or _count when none
    std::string _resolved_destination;
    Entry _entries[CAPACITY];
};

} // namespace UI::LXMF::NomadNet

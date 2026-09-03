// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "ConversationListScreen.h"

#ifdef ARDUINO

#include "Theme.h"
#include <microReticulum/Log.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Utilities/OS.h>
#include "../../Hardware/TDeck/Config.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"
#include <WiFi.h>
#include <MsgPack.h>
#include <TinyGPSPlus.h>

// SX1262Interface owns the last-RX RSSI/SNR snapshot that the top-bar
// LoRa indicator reads. Only the .cpp needs the full type — the header
// keeps `_lora_interface` typed as RNS::Interface* so set_lora_interface
// callers don't need to include the concrete class.
#include "SX1262Interface.h"

using namespace RNS;
using namespace Hardware::TDeck;

namespace UI {
namespace LXMF {

ConversationListScreen::ConversationListScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _list(nullptr), _bottom_nav(nullptr),
      _btn_new(nullptr), _btn_home(nullptr), _btn_compose(nullptr), _btn_peers(nullptr), _label_wifi(nullptr), _label_lora(nullptr),
      _label_gps(nullptr), _label_ble(nullptr), _battery_container(nullptr),
      _label_battery_icon(nullptr), _label_battery_pct(nullptr),
      _lora_interface(nullptr), _ble_interface(nullptr), _gps(nullptr),
      _message_store(nullptr) {
    LVGL_LOCK();

    // Create screen object
    if (parent) {
        _screen = lv_obj_create(parent);
    } else {
        _screen = lv_obj_create(lv_scr_act());
    }

    lv_obj_set_size(_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_screen, Theme::surface(), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_radius(_screen, 0, 0);

    // Create UI components
    create_header();
    create_list();
    create_bottom_nav();

    TRACE("ConversationListScreen created");
}

ConversationListScreen::~ConversationListScreen() {
    LVGL_LOCK();
    // Pool handles cleanup automatically when vector destructs
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void ConversationListScreen::create_header() {
    _header = lv_obj_create(_screen);
    lv_obj_set_size(_header, LV_PCT(100), 36);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);

    // Title
    lv_obj_t* title = lv_label_create(_header);
    lv_label_set_text(title, "PYXIS");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Status indicators - compact layout: WiFi, LoRa, GPS, BLE, Battery(vertical)
    _label_wifi = lv_label_create(_header);
    lv_label_set_text(_label_wifi, LV_SYMBOL_WIFI " --");
    lv_obj_align(_label_wifi, LV_ALIGN_LEFT_MID, 54, 0);
    lv_obj_set_style_text_color(_label_wifi, Theme::textMuted(), 0);

    _label_lora = lv_label_create(_header);
    lv_label_set_text(_label_lora, LV_SYMBOL_CALL"--");  // Antenna-like symbol
    lv_obj_align(_label_lora, LV_ALIGN_LEFT_MID, 101, 0);
    lv_obj_set_style_text_color(_label_lora, Theme::textMuted(), 0);

    _label_gps = lv_label_create(_header);
    lv_label_set_text(_label_gps, LV_SYMBOL_GPS " --");
    lv_obj_align(_label_gps, LV_ALIGN_LEFT_MID, 142, 0);
    lv_obj_set_style_text_color(_label_gps, Theme::textMuted(), 0);

    // BLE status: Bluetooth icon with central|peripheral counts
    _label_ble = lv_label_create(_header);
    lv_label_set_text(_label_ble, LV_SYMBOL_BLUETOOTH " -|-");
    lv_obj_align(_label_ble, LV_ALIGN_LEFT_MID, 179, 0);
    lv_obj_set_style_text_color(_label_ble, Theme::textMuted(), 0);

    // Battery: vertical layout (icon on top, percentage below) to save horizontal space
    _battery_container = lv_obj_create(_header);
    lv_obj_set_size(_battery_container, 30, 34);
    lv_obj_align(_battery_container, LV_ALIGN_LEFT_MID, 219, 0);
    lv_obj_set_style_bg_opa(_battery_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_battery_container, 0, 0);
    lv_obj_set_style_pad_all(_battery_container, 0, 0);
    lv_obj_clear_flag(_battery_container, LV_OBJ_FLAG_SCROLLABLE);

    _label_battery_icon = lv_label_create(_battery_container);
    lv_label_set_text(_label_battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(_label_battery_icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(_label_battery_icon, Theme::textMuted(), 0);

    _label_battery_pct = lv_label_create(_battery_container);
    lv_label_set_text(_label_battery_pct, "--%");
    lv_obj_align(_label_battery_pct, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_color(_label_battery_pct, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(_label_battery_pct, &lv_font_montserrat_12, 0);

    // Sync button (right corner) - syncs messages from propagation node
    _btn_new = lv_btn_create(_header);
    lv_obj_set_size(_btn_new, 55, 28);
    lv_obj_align(_btn_new, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(_btn_new, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_new, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_new, on_sync_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_sync = lv_label_create(_btn_new);
    lv_label_set_text(label_sync, LV_SYMBOL_REFRESH);
    lv_obj_center(label_sync);
    lv_obj_set_style_text_color(label_sync, Theme::textPrimary(), 0);
}

void ConversationListScreen::create_list() {
    _list = lv_obj_create(_screen);
    lv_obj_set_size(_list, LV_PCT(100), 168);  // 240 - 36 (header) - 36 (bottom nav)
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_list, 2, 0);
    lv_obj_set_style_pad_gap(_list, 2, 0);
    lv_obj_set_style_bg_color(_list, Theme::surface(), 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void ConversationListScreen::create_bottom_nav() {
    _bottom_nav = lv_obj_create(_screen);
    lv_obj_set_size(_bottom_nav, LV_PCT(100), 36);
    lv_obj_align(_bottom_nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_bottom_nav, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_bottom_nav, 0, 0);
    lv_obj_set_style_radius(_bottom_nav, 0, 0);
    lv_obj_set_style_pad_all(_bottom_nav, 0, 0);
    lv_obj_set_flex_flow(_bottom_nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_bottom_nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Application-local actions: LXMF peers stay in Messages rather than the
    // Network or NomadNet announce directories.
    const char* labels[] = {LV_SYMBOL_HOME " Home", LV_SYMBOL_BELL " Peers", LV_SYMBOL_EDIT " Compose"};

    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_btn_create(_bottom_nav);
        lv_obj_set_size(btn, 98, 30);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_set_style_bg_color(btn, Theme::surfaceInput(), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, on_bottom_nav_clicked, LV_EVENT_CLICKED, this);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, labels[i]);
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, Theme::textTertiary(), 0);
        if (i == 0) _btn_home = btn;
        else if (i == 1) _btn_peers = btn;
        else _btn_compose = btn;
    }
}

void ConversationListScreen::load_conversations(::LXMF::MessageStore& store) {
    LVGL_LOCK();
    _message_store = &store;
    refresh();
}

void ConversationListScreen::refresh() {
    LVGL_LOCK();
    if (!_message_store) {
        return;
    }

    // [PERF] temporary instrumentation — removable. Stage timings for the
    // Messages tap path. Kept as INFO so it survives a DEBUG-off build and
    // lands in the serial capture.
    const uint32_t p_t0 = millis();
    uint32_t p_t_gather = 0, p_t_diff = 0, p_t_rebuild = 0;
    size_t p_fallbacks = 0;

    // Revalidate-then-render: gather the row data first (now O(1) per
    // conversation — index preview + hash + unread, no message-file I/O)
    // and compare it against what is already on screen. The old path
    // unconditionally ran lv_obj_clean(_list) + recreated 5-7 LVGL objects
    // per row on EVERY refresh (navigation back to Messages, every
    // 750ms coalesced inbound batch, name-resolution sweep) — that
    // widget churn was what still made Messages feel slow vs NomadNet /
    // Network / Maps, which build once and just unhide.
    std::vector<ConversationItem> gathered;

    _has_unresolved_names = false;

    // Load conversations from store
    std::vector<Bytes> peer_hashes = _message_store->get_conversations();
    gathered.reserve(peer_hashes.size());

    {
        char log_buf[48];
        snprintf(log_buf, sizeof(log_buf), "  Found %zu conversations", peer_hashes.size());
        // DEBUG, not INFO: refresh() now runs on every navigation and
        // periodic sweep, and serial output happens under the LVGL lock
        // (the render task waits on the same lock the serial flush holds).
        DEBUG(log_buf);
    }

    for (const auto& peer_hash : peer_hashes) {
        // Last-message hash comes from the conversation index cache
        // (O(1) in-memory lookup, no 8KB hash-array copy and no I/O).
        Bytes last_msg_hash = _message_store->get_last_message_hash(peer_hash);
        if (!last_msg_hash) {
            continue;
        }

        // Preview + timestamp come from the conversation index cache when
        // available: O(1), zero I/O. Only fall back to load_message_metadata()
        // when the cache is empty — the first refresh after a firmware
        // upgrade (index generation predates the field) or after a
        // corrupt-message drop. The write-through below re-pops the cache,
        // so the fallback happens at most once per conversation per
        // firmware generation.
        //
        // The metadata path does one open + one filtered parse per
        // conversation (the old unconditional path opened + parsed the
        // newest message file on EVERY refresh, which dominated list-load
        // time on SPI LittleFS).
        std::string preview;
        double preview_ts = 0.0;
        bool have_preview =
            _message_store->get_last_message_preview(peer_hash, preview, preview_ts);
        bool need_metadata = !have_preview;
        bool queued_drops = false;
        if (need_metadata) p_fallbacks++;

        ::LXMF::MessageStore::MessageMetadata last_meta;
        last_meta.valid = false;
        if (need_metadata) {
            last_meta = _message_store->load_message_metadata(last_msg_hash);
        }

        if (need_metadata && !last_meta.valid) {
            // The newest message is corrupt/unreadable. Don't hide the
            // conversation over one bad message: drop the unreadable
            // message(s) and preview the newest readable one instead.
            //
            // This only costs the get_messages_for_conversation() array
            // copy and the extra metadata reads on the corrupt path — the
            // common (cached) path above stays O(1) + no I/O.
            //
            // delete_message() commits the index and removes files, so it
            // must not run under this LVGL lock: we only queue the hashes
            // here, and UIManager::update() drains them (before taking the
            // lock) — same deferred pattern as mark-read / name writes.
            // The store's index updates last_message_hash on delete, so
            // the next refresh converges to the same preview.
            std::vector<Bytes> hashes =
                _message_store->get_messages_for_conversation(peer_hash);
            request_drop_message(last_msg_hash);  // tail is the known-bad one
            queued_drops = true;
            // Walk newest→oldest over all but the tail (array is
            // chronological; hashes.back() is the tail we just queued) to
            // find the newest readable preview, queuing any unreadable
            // messages along the way.
            for (int i = (int)hashes.size() - 2; i >= 0; --i) {
                ::LXMF::MessageStore::MessageMetadata m =
                    _message_store->load_message_metadata(hashes[i]);
                if (m.valid) {
                    last_meta = m;  // newest readable preview
                    break;
                }
                request_drop_message(hashes[i]);
                queued_drops = true;
            }
            if (!last_meta.valid) {
                // Every message was unreadable; all queued for drop. Skip
                // the row this refresh — the store converges to none.
                continue;
            }
        }

        // Re-pop the preview cache from the metadata we just read (the
        // next refresh becomes O(1) with no I/O). Only meaningful on the
        // fallback path — when the cache was already serving us there is
        // nothing new to write. An EMPTY content re-pops as a valid
        // cached empty preview (the store marks it populated), so
        // empty-content tails — location shares, blank pings — also stop
        // falling back. The one-shot index commit below persists the
        // repop so a reboot does not re-pay every fallback. Skipped
        // when drops were queued: the drained deletes update
        // last_message_hash, so a preview written for the old tail would
        // be stale for one refresh — let the next fallback re-read the
        // converged tail instead.
        if (need_metadata && last_meta.valid && !queued_drops) {
            _message_store->set_last_message_preview(peer_hash,
                last_meta.content.substr(0, 47));
            _index_commit_pending = true;
        }

        // Create conversation item
        ConversationItem item;
        item.peer_hash = peer_hash;

        // Resolve display name in three tiers:
        //   1. Live announce cache (Identity::recall_app_data) — fresh
        //   2. MessageStore-persisted name — survives reboots
        //   3. Truncated hash — last resort
        // When (1) succeeds, also write through to the persisted cache
        // so future cold boots get (2) immediately.
        bool resolved = false;
        Bytes app_data = Identity::recall_app_data(peer_hash);
        if (app_data && app_data.size() > 0) {
            String display_name = parse_display_name(app_data);
            if (display_name.length() > 0) {
                item.peer_name = display_name;
                resolved = true;
                // Defer the persisted write-through — set_display_name() hits
                // microStore/LittleFS, and refresh() runs under the LVGL lock
                // (held by UIManager::update()). UIManager::update() flushes
                // these before it takes the lock so the I/O never stalls the
                // render task. (Same rationale as on_message_received.)
                _pending_name_writes.emplace_back(
                    peer_hash, std::string(display_name.c_str()));
            }
        }
        if (!resolved && _message_store) {
            std::string cached = _message_store->get_display_name(peer_hash);
            if (!cached.empty()) {
                item.peer_name = String(cached.c_str());
                resolved = true;
            }
        }
        if (!resolved) {
            item.peer_name = truncate_hash(peer_hash);
            _has_unresolved_names = true;
        }

        // Get message content for preview (index cache, or pre-extracted
        // metadata on the fallback path — no msgpack unpacking)
        String content(have_preview ? preview.c_str() : last_meta.content.c_str());
        item.last_message = content.substring(0, 30);  // Truncate to 30 chars
        if (content.length() > 30) {
            item.last_message += "...";
        }

        item.timestamp = (uint32_t)(have_preview ? preview_ts : last_meta.timestamp);
        item.timestamp_str = format_timestamp(item.timestamp);
        // Unread count straight from the conversation index (persisted,
        // incremented on incoming save, cleared when the user opens the
        // chat via request_mark_read()).
        item.unread_count =
            (uint16_t)_message_store->get_conversation_unread_count(peer_hash);

        gathered.push_back(item);
    }

    // Revalidate: skip the LVGL rebuild entirely when nothing changed.
    // Rows keep their focus-group membership and the screen keeps its
    // scroll position, and the render task costs only the O(1) gather
    // above. This is the common case: navigation back to Messages with
    // no new traffic, 750ms coalesced refreshes on an idle link, and
    // the update_status() name-resolution sweep.
    //
    // (Badge edge case: a clicked row has its badge widget deleted and a
    // mark-read queued; the mark-read flush runs in UIManager::update()
    // BEFORE any later refresh takes the lock, so a refresh in that
    // window sees the data unchanged (badge stays off) and the first
    // refresh after the flush sees unread==0 and rebuilds correctly.)
    p_t_gather = millis() - p_t0;
    bool changed = (gathered.size() != _conversations.size());
    if (!changed) {
        for (size_t i = 0; i < gathered.size(); ++i) {
            const ConversationItem& g = gathered[i];
            const ConversationItem& c = _conversations[i];
            if (g.peer_hash != c.peer_hash ||
                g.peer_name != c.peer_name ||
                g.last_message != c.last_message ||
                g.timestamp != c.timestamp ||
                g.unread_count != c.unread_count) {
                changed = true;
                break;
            }
        }
    }
    p_t_diff = millis() - p_t0;
    if (!changed) {
        // [PERF] temporary instrumentation — removable.
        char perf_buf[128];
        snprintf(perf_buf, sizeof(perf_buf),
            "[PERF] convlist skip rows=%u fallbacks=%u gather=%ums diff=%ums total=%ums",
            (unsigned)gathered.size(), (unsigned)p_fallbacks,
            (unsigned)p_t_gather, (unsigned)p_t_diff, (unsigned)p_t_diff);
        INFO(perf_buf);
        return;  // rows on screen are current
    }

    // Data changed: rebuild the list (full rebuild — a partial per-row
    // diff is not worth the extra bookkeeping for a list this size).
    lv_obj_clean(_list);
    _conversations.clear();
    _conversation_containers.clear();
    _badge_pool.clear();
    _peer_hash_pool.clear();
    _peer_hash_pool.reserve(gathered.size());
    _conversations.reserve(gathered.size());
    _conversation_containers.reserve(gathered.size());
    _badge_pool.reserve(gathered.size());

    for (const ConversationItem& item : gathered) {
        _conversations.push_back(item);
        create_conversation_item(item);
    }
    p_t_rebuild = millis() - p_t0;

    // refresh() may run while Messages remains active. Newly created rows are
    // not automatically restored to the encoder group after lv_obj_clean().
    if (_visible) {
        lv_group_t* group = LVGL::LVGLInit::get_default_group();
        if (group) {
            for (lv_obj_t* container : _conversation_containers) {
                lv_group_add_obj(group, container);
            }
        }
    }

    // [PERF] temporary instrumentation — removable.
    {
        char perf_buf[128];
        snprintf(perf_buf, sizeof(perf_buf),
            "[PERF] convlist build rows=%u fallbacks=%u gather=%ums rebuild=%ums total=%ums",
            (unsigned)gathered.size(), (unsigned)p_fallbacks,
            (unsigned)p_t_gather, (unsigned)p_t_rebuild, (unsigned)(millis() - p_t0));
        INFO(perf_buf);
    }
}

void ConversationListScreen::flush_pending_name_writes() {
    // Called from UIManager::update() BEFORE it takes the LVGL lock.
    // set_display_name() hits microStore/LittleFS; running it inside refresh()
    // (under the lock) serially stalls the LVGL render task on a cold-boot
    // announce burst. Same fix as UIManager::on_message_received.
    if (!_message_store || _pending_name_writes.empty()) {
        return;
    }
    for (const auto& w : _pending_name_writes) {
        _message_store->set_display_name(w.first, w.second);
    }
    _pending_name_writes.clear();
}

void ConversationListScreen::create_conversation_item(const ConversationItem& item) {
    // Create container for conversation item - compact 2-row layout
    lv_obj_t* container = lv_obj_create(_list);
    lv_obj_set_size(container, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(container, Theme::surfaceContainer(), 0);
    lv_obj_set_style_bg_color(container, Theme::surfaceElevated(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(container, 1, 0);
    lv_obj_set_style_border_color(container, Theme::border(), 0);
    lv_obj_set_style_radius(container, 6, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Focus style for trackball navigation
    lv_obj_set_style_border_color(container, Theme::info(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(container, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(container, Theme::surfaceElevated(), LV_STATE_FOCUSED);

    // Store peer hash in user data using pool (avoids per-item heap allocations)
    _peer_hash_pool.push_back(item.peer_hash);
    lv_obj_set_user_data(container, &_peer_hash_pool.back());
    lv_obj_add_event_cb(container, on_conversation_clicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(container, on_conversation_long_pressed, LV_EVENT_LONG_PRESSED, this);

    // Track container for focus group management
    _conversation_containers.push_back(container);

    // Row 1: Peer hash
    lv_obj_t* label_peer = lv_label_create(container);
    lv_label_set_text(label_peer, item.peer_name.c_str());
    lv_obj_align(label_peer, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_text_color(label_peer, Theme::info(), 0);
    lv_obj_set_style_text_font(label_peer, &lv_font_montserrat_14, 0);

    // Row 2: Message preview (left) + Timestamp (right)
    lv_obj_t* label_preview = lv_label_create(container);
    lv_label_set_text(label_preview, item.last_message.c_str());
    lv_obj_align(label_preview, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_text_color(label_preview, Theme::textTertiary(), 0);
    lv_obj_set_width(label_preview, 220);  // Limit width to leave room for timestamp
    lv_label_set_long_mode(label_preview, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_height(label_preview, 16, 0);  // Force single line

    lv_obj_t* label_time = lv_label_create(container);
    lv_label_set_text(label_time, item.timestamp_str.c_str());
    // Leave room for the unread badge (20px + 6px pad) when one is
    // present; the badge occupies the bottom-right corner.
    lv_obj_align(label_time, LV_ALIGN_BOTTOM_RIGHT,
                 item.unread_count > 0 ? -30 : -6, -4);
    lv_obj_set_style_text_color(label_time, Theme::textMuted(), 0);

    // Unread count badge. Always append to _badge_pool (nullptr when no
    // badge) so the pool stays index-aligned with _conversation_containers.
    lv_obj_t* badge = nullptr;
    if (item.unread_count > 0) {
        badge = lv_obj_create(container);
        lv_obj_set_size(badge, 20, 20);
        lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
        lv_obj_set_style_bg_color(badge, Theme::error(), 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);

        lv_obj_t* label_count = lv_label_create(badge);
        // Badge is a 20px circle; cap the digit count so it always fits.
        int shown = item.unread_count;
        if (shown > 9) shown = 9;
        lv_label_set_text_fmt(label_count, "%d", shown);
        lv_obj_center(label_count);
        lv_obj_set_style_text_color(label_count, lv_color_white(), 0);
    }
    _badge_pool.push_back(badge);
}

void ConversationListScreen::request_mark_read(const Bytes& peer_hash) {
    // Defers the actual store mutation. mark_conversation_read() commits
    // the index to LittleFS, so it runs from UIManager::update() BEFORE
    // the LVGL lock (same pattern as flush_pending_name_writes) — not
    // from this LVGL event callback.
    for (const auto& h : _pending_mark_reads) {
        if (h == peer_hash) return;
    }
    _pending_mark_reads.push_back(peer_hash);
}

void ConversationListScreen::flush_pending_mark_reads() {
    // Called from UIManager::update() BEFORE it takes the LVGL lock.
    // mark_conversation_read() hits microStore/LittleFS; the LVGL event
    // that requested the mark-read only set the flag.
    if (!_message_store || _pending_mark_reads.empty()) {
        return;
    }
    for (const auto& h : _pending_mark_reads) {
        _message_store->mark_conversation_read(h);
    }
    _pending_mark_reads.clear();
}

void ConversationListScreen::request_drop_message(const Bytes& message_hash) {
    // Queues a corrupt/unreadable message for deletion. The actual
    // delete_message() (index commit + payload file removal, and the
    // last_message_hash update) runs from flush_pending_drops() OUTSIDE
    // the LVGL lock. A bounded number of unreadable messages can be
    // queued per refresh (one per conversation, walking the tail), so the
    // queue cannot grow without bound.
    for (const auto& h : _pending_drops) {
        if (h == message_hash) return;
    }
    _pending_drops.push_back(message_hash);
}

void ConversationListScreen::flush_pending_drops() {
    // Called from UIManager::update() BEFORE it takes the LVGL lock.
    // Drop the queued unreadable messages so the conversation index
    // converges (deletion updates last_message_hash to the new tail); the
    // next list refresh then previews the newest readable message. A
    // delete that fails (store not ready) is left for a later drain.
    if (!_message_store || _pending_drops.empty()) {
        return;
    }
    std::vector<Bytes> remaining;
    for (const auto& h : _pending_drops) {
        if (!_message_store->delete_message(h)) {
            remaining.push_back(h);
        }
    }
    _pending_drops.swap(remaining);
}

void ConversationListScreen::flush_pending_index_commit() {
    // Called from UIManager::update() BEFORE it takes the LVGL lock,
    // right after draining drops. Commits the persisted conversation
    // index once per boot when refresh() had to fall back to
    // load_message_metadata() (unpopulated preview cache): the in-memory
    // repop would otherwise be lost on reboot and the next cold boot
    // would re-read every newest message file again. save_index()
    // rewrites the whole index, so it also persists any preview cache
    // writes made by save_message() since the last store-originated
    // commit — one-shot by design; the flag clears even if the commit
    // fails and a later fallback re-arms it.
    if (!_message_store || !_index_commit_pending) {
        return;
    }
    _message_store->commit_index();
    _index_commit_pending = false;
}

void ConversationListScreen::clear_unread_badge(lv_obj_t* container) {
    // Remove the unread badge from a rendered conversation row. The badge
    // is tracked in _badge_pool in lockstep with _conversation_containers,
    // so locate the row by container and delete its stored badge object.
    // (LVGL 8.4's lv_obj_t is opaque — no public child iteration — so a
    // side table is the portable way to reach the badge on click.)
    for (size_t i = 0; i < _conversation_containers.size(); ++i) {
        if (_conversation_containers[i] == container) {
            if (i < _badge_pool.size() && _badge_pool[i]) {
                lv_obj_del(_badge_pool[i]);
                _badge_pool[i] = nullptr;
            }
            return;
        }
    }
}

void ConversationListScreen::set_conversation_selected_callback(ConversationSelectedCallback callback) {
    _conversation_selected_callback = callback;
}

void ConversationListScreen::set_compose_callback(ComposeCallback callback) {
    _compose_callback = callback;
}

void ConversationListScreen::set_sync_callback(SyncCallback callback) {
    _sync_callback = callback;
}

void ConversationListScreen::set_home_callback(HomeCallback callback) {
    _home_callback = callback;
}

void ConversationListScreen::show() {
    LVGL_LOCK();
    // [PERF] temporary instrumentation — removable.
    const uint32_t p_show_start = millis();
    _visible = true;
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);  // Bring to front for touch events

    // Add widgets to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Add conversation containers first (so they come before New button in nav order)
        for (lv_obj_t* container : _conversation_containers) {
            lv_group_add_obj(group, container);
        }

        // Add New button last
        if (_btn_new) {
            lv_group_add_obj(group, _btn_new);
        }
        if (_btn_home) lv_group_add_obj(group, _btn_home);
        if (_btn_peers) lv_group_add_obj(group, _btn_peers);
        if (_btn_compose) lv_group_add_obj(group, _btn_compose);

        // Focus first conversation if available, otherwise New button
        if (!_conversation_containers.empty()) {
            lv_group_focus_obj(_conversation_containers[0]);
        } else if (_btn_new) {
            lv_group_focus_obj(_btn_new);
        }
    }

    // [PERF] temporary instrumentation — removable.
    {
        char perf_buf[64];
        snprintf(perf_buf, sizeof(perf_buf), "[PERF] convlist show=%ums",
            (unsigned)(millis() - p_show_start));
        INFO(perf_buf);
    }
}

void ConversationListScreen::hide() {
    LVGL_LOCK();
    _visible = false;
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Remove conversation containers
        for (lv_obj_t* container : _conversation_containers) {
            lv_group_remove_obj(container);
        }

        if (_btn_new) {
            lv_group_remove_obj(_btn_new);
        }
        if (_btn_home) lv_group_remove_obj(_btn_home);
        if (_btn_peers) lv_group_remove_obj(_btn_peers);
        if (_btn_compose) lv_group_remove_obj(_btn_compose);
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* ConversationListScreen::get_object() {
    return _screen;
}

void ConversationListScreen::update_status() {
    LVGL_LOCK();
    // Update WiFi RSSI
    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        char wifi_text[32];
        snprintf(wifi_text, sizeof(wifi_text), "%s %d", LV_SYMBOL_WIFI, rssi);
        lv_label_set_text(_label_wifi, wifi_text);

        // Color based on signal strength
        if (rssi > -50) {
            lv_obj_set_style_text_color(_label_wifi, Theme::success(), 0);  // Green
        } else if (rssi > -70) {
            lv_obj_set_style_text_color(_label_wifi, Theme::warning(), 0);  // Yellow
        } else {
            lv_obj_set_style_text_color(_label_wifi, Theme::error(), 0);  // Red
        }
    } else {
        lv_label_set_text(_label_wifi, LV_SYMBOL_WIFI " --");
        lv_obj_set_style_text_color(_label_wifi, Theme::textMuted(), 0);
    }

    // Update LoRa RSSI. SX1262Interface::get_rssi() is non-virtual on
    // the concrete impl class, and `_lora_interface` is a RNS::Interface
    // wrapper — we have to drop down to its InterfaceImpl* via .get()
    // and static_cast to the concrete subclass. Pyxis only ever passes
    // a SX1262Interface here (set_lora_interface in main.cpp wraps
    // `lora_interface_impl`, which IS SX1262Interface*).
    if (_lora_interface && _lora_interface->get()) {
        SX1262Interface* lora = static_cast<SX1262Interface*>(_lora_interface->get());
        float rssi_f = lora->get_rssi();
        int rssi = (int)rssi_f;

        // Only show RSSI if we've received at least one packet (RSSI != 0)
        if (rssi_f != 0.0f) {
            char lora_text[32];
            snprintf(lora_text, sizeof(lora_text), "%s%d", LV_SYMBOL_CALL, rssi);
            lv_label_set_text(_label_lora, lora_text);

            // Color based on signal strength (LoRa typically has weaker signals)
            if (rssi > -80) {
                lv_obj_set_style_text_color(_label_lora, Theme::success(), 0);  // Green
            } else if (rssi > -100) {
                lv_obj_set_style_text_color(_label_lora, Theme::warning(), 0);  // Yellow
            } else {
                lv_obj_set_style_text_color(_label_lora, Theme::error(), 0);  // Red
            }
        } else {
            // RSSI of 0 means no recent packet
            lv_label_set_text(_label_lora, LV_SYMBOL_CALL"--");
            lv_obj_set_style_text_color(_label_lora, Theme::textMuted(), 0);
        }
    } else {
        lv_label_set_text(_label_lora, LV_SYMBOL_CALL"--");
        lv_obj_set_style_text_color(_label_lora, Theme::textMuted(), 0);
    }

    // Update GPS state — four tiers:
    //   "--"   muted    no GPS handle, or no NMEA bytes parsed yet
    //   "?"    yellow   NMEA flowing, no fix, no in-view info either
    //   "?N"   yellow   N satellites in view but no fix yet
    //   "N"    colored  N satellites in fix; color by count
    //
    // `_gps->satellites` only updates from $GPGGA (sats USED in fix). A
    // module that's seeing the sky but hasn't acquired a fix outputs 0
    // for GGA-sats while $GPGSV reports N visible. We bind a
    // TinyGPSCustom to GPGSV field 3 in set_gps to surface that count
    // — the user sees "?12" ("12 visible, no fix") and watches it flip
    // to a colored "N" once the lock comes in.
    int sats_in_fix = (_gps && _gps->satellites.isValid())
                      ? (int)_gps->satellites.value() : 0;
    int sats_in_view = (_gps_in_view.isValid())
                       ? atoi(_gps_in_view.value()) : 0;

    if (_gps && sats_in_fix > 0) {
        char gps_text[32];
        snprintf(gps_text, sizeof(gps_text), "%s %d", LV_SYMBOL_GPS, sats_in_fix);
        lv_label_set_text(_label_gps, gps_text);

        if (sats_in_fix >= 6) {
            lv_obj_set_style_text_color(_label_gps, Theme::success(), 0);
        } else if (sats_in_fix >= 3) {
            lv_obj_set_style_text_color(_label_gps, Theme::warning(), 0);
        } else {
            lv_obj_set_style_text_color(_label_gps, Theme::error(), 0);
        }
    } else if (_gps && sats_in_view > 0) {
        char gps_text[32];
        snprintf(gps_text, sizeof(gps_text), "%s ?%d", LV_SYMBOL_GPS, sats_in_view);
        lv_label_set_text(_label_gps, gps_text);
        lv_obj_set_style_text_color(_label_gps, Theme::warning(), 0);
    } else if (_gps && _gps->charsProcessed() > 0) {
        lv_label_set_text(_label_gps, LV_SYMBOL_GPS " ?");
        lv_obj_set_style_text_color(_label_gps, Theme::warning(), 0);
    } else {
        lv_label_set_text(_label_gps, LV_SYMBOL_GPS " --");
        lv_obj_set_style_text_color(_label_gps, Theme::textMuted(), 0);
    }

    // Update BLE connection counts (central|peripheral)
    if (_ble_interface) {
        int central_count = 0;
        int peripheral_count = 0;

        // Pre-graft: BLEInterface::get_stats() was a virtual override on
        // RNS::Interface. Vanilla upstream doesn't expose get_stats on the
        // Interface base; the method is still defined on BLEInterface as
        // non-virtual. To restore: change _ble_interface to BLEInterface*.
        // Disabled for the spike.
        // auto stats = _ble_interface->get_stats();
        (void)central_count;
        (void)peripheral_count;

        char ble_text[32];
        snprintf(ble_text, sizeof(ble_text), "%s %d|%d", LV_SYMBOL_BLUETOOTH, central_count, peripheral_count);
        lv_label_set_text(_label_ble, ble_text);

        // Color based on connection status
        int total = central_count + peripheral_count;
        if (total > 0) {
            lv_obj_set_style_text_color(_label_ble, Theme::bluetooth(), 0);  // Blue - connected
        } else {
            lv_obj_set_style_text_color(_label_ble, Theme::textMuted(), 0);  // Gray - no connections
        }
    } else {
        lv_label_set_text(_label_ble, LV_SYMBOL_BLUETOOTH " -|-");
        lv_obj_set_style_text_color(_label_ble, Theme::textMuted(), 0);
    }

    // Update battery level (read from ADC) - vertical layout
    // ESP32 ADC has linearity/offset issues - add 0.32V calibration per LilyGo community
    int raw_adc = analogRead(Pin::BATTERY_ADC);
    float voltage = (raw_adc / 4095.0) * 3.3 * Power::BATTERY_VOLTAGE_DIVIDER + 0.32;
    int percent = (int)((voltage - Power::BATTERY_EMPTY) / (Power::BATTERY_FULL - Power::BATTERY_EMPTY) * 100);
    percent = constrain(percent, 0, 100);

    // Detect charging: voltage > 4.4V indicates USB power connected
    // (calibrated voltage reads ~5V+ when charging, ~4.2V max on battery)
    bool charging = (voltage > 4.4);

    // Update icon and percentage display
    if (charging) {
        // When charging: show charge icon centered, hide percentage (voltage doesn't reflect battery state)
        lv_label_set_text(_label_battery_icon, LV_SYMBOL_CHARGE);
        lv_obj_align(_label_battery_icon, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(_label_battery_pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(_label_battery_icon, Theme::charging(), 0);  // Cyan
    } else {
        // When on battery: show icon at top with percentage below
        lv_label_set_text(_label_battery_icon, LV_SYMBOL_BATTERY_FULL);
        lv_obj_align(_label_battery_icon, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(_label_battery_pct, LV_OBJ_FLAG_HIDDEN);
        char pct_text[16];
        snprintf(pct_text, sizeof(pct_text), "%d%%", percent);
        lv_label_set_text(_label_battery_pct, pct_text);

        // Color based on battery level
        lv_color_t battery_color;
        if (percent > 50) {
            battery_color = Theme::success();  // Green
        } else if (percent > 20) {
            battery_color = Theme::warning();  // Yellow
        } else {
            battery_color = Theme::error();  // Red
        }
        lv_obj_set_style_text_color(_label_battery_icon, battery_color, 0);
        lv_obj_set_style_text_color(_label_battery_pct, battery_color, 0);
    }

    // Check if any unresolved display names can now be resolved
    // (announces may have arrived since the list was last refreshed)
    if (_has_unresolved_names && _message_store) {
        for (const auto& item : _conversations) {
            Bytes app_data = Identity::recall_app_data(item.peer_hash);
            if (app_data && app_data.size() > 0) {
                String name = parse_display_name(app_data);
                if (name.length() > 0 && item.peer_name != name) {
                    // A name has become available — refresh the whole list
                    DEBUG("Display name resolved, refreshing conversation list");
                    refresh();
                    return;
                }
            }
        }
    }
}

void ConversationListScreen::on_conversation_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);

    Bytes* peer_hash = (Bytes*)lv_obj_get_user_data(target);

    if (peer_hash && screen->_conversation_selected_callback) {
        // Clear the badge in the UI now; the store commit is deferred to
        // UIManager::update() (mark_conversation_read() writes the
        // LittleFS index and must not run under the LVGL lock).
        screen->clear_unread_badge(target);
        screen->request_mark_read(*peer_hash);
        screen->_conversation_selected_callback(*peer_hash);
    }
}

void ConversationListScreen::on_sync_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);

    if (screen->_sync_callback) {
        screen->_sync_callback();
    }
}

void ConversationListScreen::on_bottom_nav_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);
    int btn_index = (int)(intptr_t)lv_obj_get_user_data(target);

    switch (btn_index) {
        case 0: // Launcher home
            if (screen->_home_callback) screen->_home_callback();
            break;
        case 1: // Heard LXMF peers
            if (screen->_peers_callback) screen->_peers_callback();
            break;
        case 2: // Compose new message
            if (screen->_compose_callback) {
                screen->_compose_callback();
            }
            break;

        default:
            break;
    }
}

void ConversationListScreen::msgbox_close_cb(lv_event_t* event) {
    lv_obj_t* mbox = lv_event_get_current_target(event);
    lv_msgbox_close(mbox);
}

void ConversationListScreen::on_conversation_long_pressed(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);

    Bytes* peer_hash = (Bytes*)lv_obj_get_user_data(target);
    if (!peer_hash) {
        return;
    }

    // Store the hash we want to delete
    screen->_pending_delete_hash = *peer_hash;

    // Show confirmation dialog
    static const char* btns[] = {"Delete", "Cancel", ""};
    lv_obj_t* mbox = lv_msgbox_create(NULL, "Delete Conversation",
        "Delete this conversation and all messages?", btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, on_delete_confirmed, LV_EVENT_VALUE_CHANGED, screen);
}

void ConversationListScreen::on_delete_confirmed(lv_event_t* event) {
    lv_obj_t* mbox = lv_event_get_current_target(event);
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);

    if (btn_id == 0 && screen->_message_store) {  // "Delete" button
        // Delete the conversation
        screen->_message_store->delete_conversation(screen->_pending_delete_hash);
        INFO("Deleted conversation");

        // Refresh the list
        screen->refresh();
    }

    lv_msgbox_close(mbox);
}

String ConversationListScreen::format_timestamp(uint32_t timestamp) {
    double now = Utilities::OS::time();

    // If our local clock hasn't been set (no GPS lock yet, no NTP) it's
    // running off uptime in seconds — way smaller than any real
    // unix-epoch timestamp from a properly-clocked peer. The legacy
    // "diff < 0 → Future" branch then mis-fires on every message,
    // because every message looks like it's from the future.
    //
    // Threshold: 2024-01-01 UTC = 1704067200. Anything less than that
    // means we don't have real time. Show "?" rather than confusing the
    // user with "Future" on every row.
    constexpr double SANE_EPOCH = 1704067200.0;
    if (now < SANE_EPOCH) {
        return "?";  // Local clock not yet synced
    }

    double diff = now - (double)timestamp;

    if (diff < 0) {
        return "Future";  // Genuine future timestamp (peer's clock is off)
    } else if (diff < 60) {
        return "Just now";
    } else if (diff < 3600) {
        int mins = (int)(diff / 60);
        return String(mins) + "m ago";
    } else if (diff < 86400) {
        int hours = (int)(diff / 3600);
        return String(hours) + "h ago";
    } else if (diff < 604800) {
        int days = (int)(diff / 86400);
        return String(days) + "d ago";
    } else {
        int weeks = (int)(diff / 604800);
        return String(weeks) + "w ago";
    }
}

String ConversationListScreen::truncate_hash(const Bytes& hash) {
    return String(hash.toHex().c_str());
}

String ConversationListScreen::parse_display_name(const Bytes& app_data) {
    if (app_data.size() == 0) {
        return String();
    }

    uint8_t first_byte = app_data.data()[0];

    // Check for msgpack array format (LXMF 0.5.0+)
    // fixarray: 0x90-0x9f (array with 0-15 elements)
    // array16: 0xdc
    if ((first_byte >= 0x90 && first_byte <= 0x9f) || first_byte == 0xdc) {
        // Msgpack encoded: [display_name, stamp_cost, ...]
        MsgPack::Unpacker unpacker;
        unpacker.feed(app_data.data(), app_data.size());

        // Get array size
        MsgPack::arr_size_t arr_size;
        if (!unpacker.deserialize(arr_size)) {
            return String();
        }

        if (arr_size.size() < 1) {
            return String();
        }

        // First element is display_name (can be nil or bytes)
        if (unpacker.isNil()) {
            unpacker.unpackNil();
            return String();
        }

        MsgPack::bin_t<uint8_t> name_bin;
        if (unpacker.deserialize(name_bin)) {
            // Convert bytes to string
            return String((const char*)name_bin.data(), name_bin.size());
        }

        return String();
    } else {
        // Original format: raw UTF-8 string
        return String(app_data.toString().c_str());
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO

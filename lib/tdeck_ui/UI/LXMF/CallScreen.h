// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CALLSCREEN_H
#define UI_LXMF_CALLSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <functional>
#include "Bytes.h"

namespace UI {
namespace LXMF {

/**
 * Call Screen
 *
 * Shows voice call status and controls:
 * - Peer name/hash
 * - Call state (connecting, ringing, active, ended)
 * - Call duration timer
 * - Mute toggle
 * - Hangup button
 *
 * Layout:
 * ┌─────────────────────────────────────┐
 * │          Calling...                 │
 * │                                     │
 * │        Alice (a1b2c3d4...)          │
 * │                                     │
 * │           00:00:32                  │
 * │                                     │
 * │      [Mute]        [Hangup]         │
 * └─────────────────────────────────────┘
 */
class CallScreen {
public:
    enum class CallState {
        CONNECTING,        // Link being established (outgoing)
        RINGING,           // Waiting for remote to answer (outgoing)
        INCOMING_RINGING,  // Incoming call, waiting for user to answer
        ACTIVE,            // Voice flowing
        ENDED              // Call ended (brief display before returning)
    };

    using HangupCallback = std::function<void()>;
    using MuteCallback = std::function<void(bool muted)>;
    using AnswerCallback = std::function<void()>;

    CallScreen(lv_obj_t* parent = nullptr);
    ~CallScreen();

    /** Set peer info for display */
    void set_peer(const RNS::Bytes& peer_hash, const String& display_name = "");

    /** Update call state display */
    void set_state(CallState state);

    /** Update call duration display (seconds) */
    void set_duration(uint32_t seconds);

    /** Update mute button state */
    void set_muted(bool muted);

    void set_hangup_callback(HangupCallback callback);
    void set_mute_callback(MuteCallback callback);
    void set_answer_callback(AnswerCallback callback);

    void show();
    void hide();
    lv_obj_t* get_object();

private:
    lv_obj_t* _screen;
    lv_obj_t* _label_state;
    lv_obj_t* _label_peer;
    lv_obj_t* _label_duration;
    lv_obj_t* _btn_mute;
    lv_obj_t* _label_mute;
    lv_obj_t* _btn_hangup;

    bool _muted;
    CallState _state;

    HangupCallback _hangup_callback;
    MuteCallback _mute_callback;
    AnswerCallback _answer_callback;

    void create_ui();

    static void on_hangup_clicked(lv_event_t* event);
    static void on_mute_clicked(lv_event_t* event);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_CALLSCREEN_H

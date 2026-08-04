#pragma once

#ifdef ARDUINO

#include <Arduino.h>
#include <functional>
#include <lvgl.h>
#include "radio_activity/RadioActivityHistory.h"

namespace UI {
namespace LXMF {

class RadioActivityScreen {
public:
    struct RadioConfig {
        float frequency_mhz = 0.0f;
        float bandwidth_khz = 0.0f;
        uint8_t spreading_factor = 0;
        uint8_t coding_rate = 0;
        int8_t tx_power_dbm = 0;
        bool available = false;
    };

    using BackCallback = std::function<void()>;

    explicit RadioActivityScreen(lv_obj_t* parent = nullptr);
    ~RadioActivityScreen();

    void set_back_callback(BackCallback callback);
    void render(const RadioActivity::Snapshot& snapshot,
                const RadioConfig& config,
                uint32_t now_ms);
    void show();
    void hide();
    bool visible() const { return _visible; }
    bool render_due(uint32_t now_ms) const;

private:
    lv_obj_t* _screen = nullptr;
    lv_obj_t* _btn_back = nullptr;
    lv_obj_t* _chart = nullptr;
    lv_obj_t* _label_current = nullptr;
    lv_obj_t* _label_noise = nullptr;
    lv_obj_t* _label_load = nullptr;
    lv_obj_t* _label_footer = nullptr;
    RadioActivity::Snapshot _draw_snapshot{};
    BackCallback _back_callback;
    uint32_t _last_render_ms = 0;
    bool _visible = false;

    void create_header();
    void create_metrics();
    void create_graph();
    void create_footer();
    static void on_back_clicked(lv_event_t* event);
    static void on_chart_draw(lv_event_t* event);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO

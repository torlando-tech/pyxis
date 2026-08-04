#include "RadioActivityScreen.h"

#ifdef ARDUINO

#include "Theme.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"

namespace UI {
namespace LXMF {
namespace {

constexpr uint32_t RENDER_INTERVAL_MS = 143; // Seven frames per second.
constexpr int16_t GRAPH_MIN_DBM = -135;
constexpr int16_t GRAPH_MAX_DBM = -60;

void style_panel(lv_obj_t* panel) {
    lv_obj_set_style_bg_color(panel, Theme::surfaceInput(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, Theme::border(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 3, 0);
}

lv_obj_t* metric_panel(lv_obj_t* parent, int x, int width,
                       const char* title, lv_obj_t** value) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, 40);
    lv_obj_set_size(panel, width, 34);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    style_panel(panel);

    lv_obj_t* heading = lv_label_create(panel);
    lv_label_set_text(heading, title);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, -1);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(heading, Theme::textMuted(), 0);

    *value = lv_label_create(panel);
    lv_label_set_text(*value, "--");
    lv_obj_align(*value, LV_ALIGN_BOTTOM_LEFT, 0, 1);
    lv_obj_set_style_text_font(*value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(*value, Theme::textPrimary(), 0);
    return panel;
}

} // namespace

RadioActivityScreen::RadioActivityScreen(lv_obj_t* parent) {
    _screen = lv_obj_create(parent ? parent : lv_scr_act());
    lv_obj_set_size(_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_screen, Theme::surface(), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_radius(_screen, 0, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);

    create_header();
    create_metrics();
    create_graph();
    create_footer();
    hide();
}

RadioActivityScreen::~RadioActivityScreen() {
    LVGL_LOCK();
    if (_screen) lv_obj_del(_screen);
}

void RadioActivityScreen::create_header() {
    lv_obj_t* header = lv_obj_create(_screen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 320, 36);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    _btn_back = lv_btn_create(header);
    lv_obj_set_size(_btn_back, 44, 28);
    lv_obj_align(_btn_back, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_set_style_bg_color(_btn_back, Theme::btnSecondary(), 0);
    lv_obj_set_style_border_color(_btn_back, Theme::border(), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_add_event_cb(_btn_back, on_back_clicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back = lv_label_create(_btn_back);
    lv_label_set_text(back, LV_SYMBOL_LEFT);
    lv_obj_center(back);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Radio Activity");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);

    lv_obj_t* live = lv_label_create(header);
    lv_label_set_text(live, LV_SYMBOL_BULLET " LIVE | 7 FPS");
    lv_obj_align(live, LV_ALIGN_RIGHT_MID, -7, 0);
    lv_obj_set_style_text_font(live, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(live, Theme::success(), 0);
}

void RadioActivityScreen::create_metrics() {
    metric_panel(_screen, 7, 100, "CURRENT", &_label_current);
    metric_panel(_screen, 111, 100, "NOISE FLOOR", &_label_noise);
    metric_panel(_screen, 215, 98, "CHANNEL LOAD", &_label_load);
    lv_obj_set_style_text_color(_label_load, Theme::success(), 0);
}

void RadioActivityScreen::create_graph() {
    lv_obj_t* graph_panel = lv_obj_create(_screen);
    lv_obj_set_pos(graph_panel, 7, 78);
    lv_obj_set_size(graph_panel, 306, 116);
    lv_obj_clear_flag(graph_panel, LV_OBJ_FLAG_SCROLLABLE);
    style_panel(graph_panel);
    lv_obj_set_style_pad_all(graph_panel, 0, 0);

    const char* scales[] = {"-60", "-100", "-135"};
    const int ys[] = {3, 49, 98};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* label = lv_label_create(graph_panel);
        lv_label_set_text(label, scales[i]);
        lv_obj_set_pos(label, 3, ys[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_color(label, Theme::textMuted(), 0);
    }

    // A plain draw object avoids both a full-screen canvas and LVGL's disabled
    // chart widget. The callback emits only bounded line primitives.
    _chart = lv_obj_create(graph_panel);
    lv_obj_set_pos(_chart, 27, 3);
    lv_obj_set_size(_chart, 276, 108);
    lv_obj_clear_flag(_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_chart, Theme::surface(), 0);
    lv_obj_set_style_bg_opa(_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chart, 0, 0);
    lv_obj_set_style_radius(_chart, 0, 0);
    lv_obj_set_style_pad_all(_chart, 0, 0);
    lv_obj_add_event_cb(_chart, on_chart_draw, LV_EVENT_DRAW_POST_END, this);
}

void RadioActivityScreen::create_footer() {
    lv_obj_t* legend = lv_label_create(_screen);
    lv_label_set_text(legend, LV_SYMBOL_BULLET " Noise   " LV_SYMBOL_BULLET
                      " LoRa RX   " LV_SYMBOL_BULLET " Other RF   " LV_SYMBOL_BULLET " TX");
    lv_obj_set_pos(legend, 9, 198);
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(legend, Theme::textTertiary(), 0);

    _label_footer = lv_label_create(_screen);
    lv_label_set_text(_label_footer, "Radio unavailable");
    lv_obj_set_pos(_label_footer, 0, 220);
    lv_obj_set_width(_label_footer, 320);
    lv_obj_set_style_text_align(_label_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_label_footer, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(_label_footer, Theme::textTertiary(), 0);
}

void RadioActivityScreen::render(const RadioActivity::Snapshot& snapshot,
                                 const RadioConfig& config,
                                 uint32_t now_ms) {
    if (!_visible || now_ms - _last_render_ms < RENDER_INTERVAL_MS) return;
    _last_render_ms = now_ms;

    LVGL_LOCK();
    _draw_snapshot = snapshot;

    char text[64];
    if (snapshot.count > 0) {
        snprintf(text, sizeof(text), "%d dBm", snapshot.current_rssi);
    } else {
        snprintf(text, sizeof(text), "Warming up");
    }
    lv_label_set_text(_label_current, text);

    if (snapshot.noise_floor_ready) {
        snprintf(text, sizeof(text), "%d dBm", snapshot.noise_floor);
    } else {
        snprintf(text, sizeof(text), "Learning...");
    }
    lv_label_set_text(_label_noise, text);
    snprintf(text, sizeof(text), "%u%%", snapshot.channel_load_percent);
    lv_label_set_text(_label_load, text);

    if (config.available) {
        snprintf(text, sizeof(text), "%.3f MHz | BW %.1f kHz | SF%u | CR4/%u | %d dBm",
                 config.frequency_mhz, config.bandwidth_khz,
                 config.spreading_factor, config.coding_rate, config.tx_power_dbm);
    } else {
        snprintf(text, sizeof(text), "Radio unavailable");
    }
    lv_label_set_text(_label_footer, text);
    lv_obj_invalidate(_chart);
}

void RadioActivityScreen::show() {
    LVGL_LOCK();
    _visible = true;
    _last_render_ms = millis() - RENDER_INTERVAL_MS;
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group && _btn_back) {
        lv_group_add_obj(group, _btn_back);
        lv_group_focus_obj(_btn_back);
    }
}

void RadioActivityScreen::hide() {
    LVGL_LOCK();
    _visible = false;
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group && _btn_back) lv_group_remove_obj(_btn_back);
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

void RadioActivityScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void RadioActivityScreen::on_back_clicked(lv_event_t* event) {
    auto* screen = static_cast<RadioActivityScreen*>(lv_event_get_user_data(event));
    if (screen && screen->_back_callback) screen->_back_callback();
}

void RadioActivityScreen::on_chart_draw(lv_event_t* event) {
    auto* screen = static_cast<RadioActivityScreen*>(lv_event_get_user_data(event));
    if (!screen || screen->_draw_snapshot.count == 0) return;

    lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t area;
    lv_obj_get_content_coords(screen->_chart, &area);
    const int32_t width = lv_area_get_width(&area) - 1;
    const int32_t height = lv_area_get_height(&area) - 1;
    const auto& snapshot = screen->_draw_snapshot;

    auto x_for = [&](std::size_t index) -> lv_coord_t {
        const std::size_t right_aligned = RadioActivity::History::CAPACITY - snapshot.count + index;
        return static_cast<lv_coord_t>(area.x1 + static_cast<int32_t>(
            right_aligned * width / (RadioActivity::History::CAPACITY - 1)));
    };
    auto y_for = [&](int16_t rssi) -> lv_coord_t {
        if (rssi < GRAPH_MIN_DBM) rssi = GRAPH_MIN_DBM;
        if (rssi > GRAPH_MAX_DBM) rssi = GRAPH_MAX_DBM;
        return static_cast<lv_coord_t>(area.y1 + static_cast<int32_t>(
            (GRAPH_MAX_DBM - rssi) * height / (GRAPH_MAX_DBM - GRAPH_MIN_DBM)));
    };

    lv_draw_line_dsc_t grid_dsc;
    lv_draw_line_dsc_init(&grid_dsc);
    grid_dsc.color = Theme::border();
    grid_dsc.width = 1;
    grid_dsc.opa = LV_OPA_30;
    for (const int16_t dbm : {static_cast<int16_t>(-60), static_cast<int16_t>(-100),
                              static_cast<int16_t>(-135)}) {
        lv_point_t p1 = {area.x1, y_for(dbm)};
        lv_point_t p2 = {area.x2, y_for(dbm)};
        lv_draw_line(draw_ctx, &grid_dsc, &p1, &p2);
    }
    for (int division = 1; division < 4; ++division) {
        const lv_coord_t x = static_cast<lv_coord_t>(area.x1 + width * division / 4);
        lv_point_t p1 = {x, area.y1};
        lv_point_t p2 = {x, area.y2};
        lv_draw_line(draw_ctx, &grid_dsc, &p1, &p2);
    }

    if (snapshot.noise_floor_ready) {
        lv_draw_line_dsc_t noise_dsc;
        lv_draw_line_dsc_init(&noise_dsc);
        noise_dsc.color = lv_color_hex(0xA98BAE);
        noise_dsc.width = 1;
        noise_dsc.opa = LV_OPA_60;
        const lv_coord_t y = y_for(snapshot.noise_floor);
        lv_point_t p1 = {area.x1, y};
        lv_point_t p2 = {area.x2, y};
        lv_draw_line(draw_ctx, &noise_dsc, &p1, &p2);
    }

    if (snapshot.count > 1) {
        lv_draw_line_dsc_t trace_dsc;
        lv_draw_line_dsc_init(&trace_dsc);
        trace_dsc.color = lv_color_hex(0x675A70);
        trace_dsc.width = 2;
        trace_dsc.opa = LV_OPA_COVER;
        for (std::size_t i = 1; i < snapshot.count; ++i) {
            lv_point_t p1 = {x_for(i - 1), y_for(snapshot.samples[i - 1].rssi_dbm)};
            lv_point_t p2 = {x_for(i), y_for(snapshot.samples[i].rssi_dbm)};
            lv_draw_line(draw_ctx, &trace_dsc, &p1, &p2);
        }
    }

    for (std::size_t i = 0; i < snapshot.count; ++i) {
        const auto& sample = snapshot.samples[i];
        auto draw_marker = [&](RadioActivity::Event event_type, int32_t x_offset,
                               lv_color_t color, uint8_t line_width,
                               bool start_at_rssi) {
            if (!RadioActivity::has_event(sample, event_type)) return;
            int32_t marker_x = static_cast<int32_t>(x_for(i)) + x_offset;
            if (marker_x < area.x1) marker_x = area.x1;
            if (marker_x > area.x2) marker_x = area.x2;
            const lv_coord_t y1 = start_at_rssi ? y_for(sample.rssi_dbm) : area.y1;

            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.color = color;
            dsc.width = line_width;
            dsc.opa = LV_OPA_80;
            lv_point_t points[2] = {
                {static_cast<lv_coord_t>(marker_x), y1},
                {static_cast<lv_coord_t>(marker_x), area.y2},
            };
            lv_draw_line(draw_ctx, &dsc, &points[0], &points[1]);
        };

        draw_marker(RadioActivity::Event::Rx, -1, lv_color_hex(0x55966B), 2, true);
        draw_marker(RadioActivity::Event::Interference, 0, lv_color_hex(0xA98BAE), 2, true);
        draw_marker(RadioActivity::Event::Tx, 1, Theme::primaryLight(), 3, false);
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO

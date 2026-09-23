#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <cstdio>

using namespace baji::ui;

namespace {
void DrawStatusWifi(lv_event_t* event) {
    auto* object = lv_event_get_target_obj(event);
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    lv_obj_init_draw_arc_dsc(object, LV_PART_MAIN, &arc);
    arc.center = {bounds.x1 + 8, bounds.y1 + 12};
    arc.start_angle = 225;
    arc.end_angle = 315;
    // A compact quarter-circle silhouette with a half-size inner arc.
    // The smaller inner arc leaves 3px between strokes without widening the icon.
    for (const uint16_t radius : {10, 5}) {
        arc.radius = radius;
        lv_draw_arc(lv_event_get_layer(event), &arc);
    }
}
}  // namespace

void WatchUi::RenderStatus() {
    if (status_)
        lv_obj_delete(status_);
    // Tabler-inspired rounded signals and compact battery, drawn at native
    // pixel sizes inside BAJI's light status group.
    status_ = Box(root_, 0, 16, 128, 26, 0, LV_RADIUS_CIRCLE, 64);
    const bool cell = device_.network.find("4G") != std::string::npos;
    const bool off = device_.network.empty() || device_.network == "无网络" ||
                     device_.network.find("未连接") != std::string::npos;
    auto* network = Box(status_, 0, 0, 16, 16, 0, 0, 0);
    lv_obj_align(network, LV_ALIGN_LEFT_MID, 11, 0);
    const lv_opa_t signal_opa = off ? 100 : LV_OPA_COVER;
    if (cell) {
        for (int i = 0; i < 3; ++i)
            Box(network, 2 + i * 5, 10 - i * 4, 3, 4 + i * 4,
                0xffffff, 1, signal_opa);
    } else {
        lv_obj_set_style_arc_color(network, lv_color_white(), 0);
        lv_obj_set_style_arc_width(network, 2, 0);
        lv_obj_set_style_arc_rounded(network, true, 0);
        lv_obj_set_style_arc_opa(network, signal_opa, 0);
        lv_obj_add_event_cb(network, DrawStatusWifi, LV_EVENT_DRAW_MAIN, nullptr);
        Box(network, 7, 11, 2, 2, 0xffffff, LV_RADIUS_CIRCLE, signal_opa);
    }
    if (off) {
        // This indicates connectivity, not RSSI. Keep the same three marks
        // when offline, dimmed behind a clear disconnection slash.
        static const lv_point_precise_t slash_points[] = {{2, 2}, {13, 13}};
        auto* slash = lv_line_create(network);
        lv_obj_remove_style_all(slash);
        lv_line_set_points(slash, slash_points, 2);
        lv_obj_set_size(slash, 16, 16);
        lv_obj_set_style_line_width(slash, 2, 0);
        lv_obj_set_style_line_color(slash, lv_color_white(), 0);
        lv_obj_set_style_line_rounded(slash, true, 0);
        lv_obj_set_pos(slash, 0, 0);
        lv_obj_remove_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    }

    const bool battery_known = device_.battery >= 0;
    const int level = std::clamp(device_.battery, 0, 100);
    const bool charging = battery_known && device_.charging;
    int x = 33;
    if (charging) {
        auto* charge = Icon(status_, "zap", 0, 0, 11, 0x34d399);
        lv_obj_align(charge, LV_ALIGN_LEFT_MID, x, 0);
        x += 16;
    }
    auto* battery = Box(status_, x, 0, 23, 12, 0, 3, 0);
    Border(battery, 0xffffff, 210);
    lv_obj_align(battery, LV_ALIGN_LEFT_MID, x, 0);
    auto* terminal = Box(status_, x + 23, 0, 2, 4, 0xffffff, 1, 210);
    lv_obj_align(terminal, LV_ALIGN_LEFT_MID, x + 23, 0);
    const uint32_t color = charging ? 0x34d399 : level <= 10 ? 0xef4444
                                               : level <= 30 ? 0xfbbf24 : 0xffffff;
    // A single continuous fill follows the percentage at native pixel size.
    // Including the 1px shell border, the 19x8 interior has a 2px outer inset.
    constexpr int kFillWidth = 19;
    auto* track = Box(battery, 0, 0, kFillWidth, 8, 0xffffff, 2, 28);
    lv_obj_align(track, LV_ALIGN_LEFT_MID, 1, 0);
    const int fill_width = battery_known && level > 0
                               ? std::max(1, (level * kFillWidth + 50) / 100) : 0;
    if (fill_width > 0) {
        auto* fill = Box(battery, 0, 0, fill_width, 8, color,
                         fill_width > 2 ? 2 : 0, LV_OPA_COVER);
        lv_obj_align(fill, LV_ALIGN_LEFT_MID, 1, 0);
    }
    x += 31;
    char text[12];
    std::snprintf(text, sizeof(text), "%d%%", level);
    auto* percentage = Text(status_, battery_known ? text : "--%", 0, 0, 32, 12,
                            battery_known && level <= 10 && !charging ? 0xef4444 : 0xffffff,
                            600, false, 18);
    SingleLine(percentage);
    lv_obj_set_style_text_align(percentage, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(percentage, LV_ALIGN_LEFT_MID, x, 0);
    const int width = x + 32 + 12;
    lv_obj_set_size(status_, width, 26);
    lv_obj_set_x(status_, (360 - width) / 2);
    lv_obj_remove_flag(status_, LV_OBJ_FLAG_CLICKABLE);
    if (reminder_)
        lv_obj_move_foreground(reminder_);
    if (modal_)
        lv_obj_move_foreground(modal_);
    if (power_overlay_)
        lv_obj_move_foreground(power_overlay_);
}

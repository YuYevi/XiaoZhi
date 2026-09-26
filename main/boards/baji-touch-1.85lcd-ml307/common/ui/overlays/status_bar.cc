#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <src/draw/lv_draw_triangle.h>
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
    arc.center = {bounds.x1 + 9, bounds.y1 + 14};
    arc.start_angle = 225;
    arc.end_angle = 315;
    // Concentric, round-ended arcs share the signal columns' stroke weight.
    // Leave 3px between the 2px strokes and space above the separate round dot.
    for (const uint16_t radius : {12, 7}) {
        arc.radius = radius;
        lv_draw_arc(lv_event_get_layer(event), &arc);
    }
}

void DrawChargingBolt(lv_event_t* event) {
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    // Overlap the two solid tips so the tiny bolt has no hollow center.
    static constexpr lv_point_precise_t tips[][3] = {
        {{6, 0}, {1, 6}, {6, 6}},
        {{3, 5}, {8, 5}, {3, 11}},
    };
    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.color = lv_color_white();
    triangle.opa = LV_OPA_COVER;
    for (const auto& points : tips) {
        for (int i = 0; i < 3; ++i)
            triangle.p[i] = {bounds.x1 + points[i].x, bounds.y1 + points[i].y};
        lv_draw_triangle(lv_event_get_layer(event), &triangle);
    }
}
}  // namespace

void WatchUi::RenderStatus() {
    if (status_)
        lv_obj_delete(status_);
    // Rounded signals and a compact battery, drawn at native pixel sizes.
    status_ = Box(root_, 0, 16, 128, 26, 0, LV_RADIUS_CIRCLE, 64);
    const bool cell = device_.network.find("4G") != std::string::npos;
    const bool off = device_.network.empty() || device_.network == "无网络" ||
                     device_.network.find("未连接") != std::string::npos;
    auto* network = Box(status_, 0, 0, 20, 16, 0, 0, 0);
    lv_obj_align(network, LV_ALIGN_LEFT_MID, 11, 0);
    const lv_opa_t signal_opa = off ? 100 : LV_OPA_COVER;
    if (cell) {
        // Four compact, bottom-aligned signal columns match the reference
        // glyph while keeping a clear gap between each rounded column.
        constexpr int heights[] = {5, 8, 11, 14};
        for (int i = 0; i < 4; ++i) {
            const int height = heights[i];
            Box(network, 1 + i * 5, 15 - height, 3, height,
                0xffffff, LV_RADIUS_CIRCLE, signal_opa);
        }
    } else {
        lv_obj_set_style_arc_color(network, lv_color_white(), 0);
        lv_obj_set_style_arc_width(network, 2, 0);
        lv_obj_set_style_arc_rounded(network, true, 0);
        lv_obj_set_style_arc_opa(network, signal_opa, 0);
        lv_obj_add_event_cb(network, DrawStatusWifi, LV_EVENT_DRAW_MAIN, nullptr);
        Box(network, 8, 13, 3, 3, 0xffffff, LV_RADIUS_CIRCLE, signal_opa);
    }
    if (off) {
        // This indicates connectivity, not RSSI. Keep the same compact marks
        // when offline, dimmed behind a clear disconnection slash.
        static const lv_point_precise_t slash_points[] = {{3, 2}, {16, 14}};
        auto* slash = lv_line_create(network);
        lv_obj_remove_style_all(slash);
        lv_line_set_points(slash, slash_points, 2);
        lv_obj_set_size(slash, 20, 16);
        lv_obj_set_style_line_width(slash, 2, 0);
        lv_obj_set_style_line_color(slash, lv_color_white(), 0);
        lv_obj_set_style_line_opa(slash, signal_opa, 0);
        lv_obj_set_style_line_rounded(slash, true, 0);
        lv_obj_set_pos(slash, 0, 0);
        lv_obj_remove_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    }

    const bool battery_known = device_.battery >= 0;
    const int level = std::clamp(device_.battery, 0, 100);
    const bool charging = battery_known && device_.charging;
    int x = 37;
    // Keep the battery as one compact glyph: a fine white shell, a continuous
    // fill, and (when charging) the bolt inside the shell as in the reference.
    constexpr int kBatteryWidth = 28;
    constexpr int kBatteryHeight = 14;
    auto* battery = Box(status_, x, 0, kBatteryWidth, kBatteryHeight, 0, 3, 0);
    Border(battery, 0xffffff);
    lv_obj_align(battery, LV_ALIGN_LEFT_MID, x, 0);
    auto* terminal = Box(status_, x + kBatteryWidth, 0, 2, 6, 0xffffff, 1);
    lv_obj_align(terminal, LV_ALIGN_LEFT_MID, x + kBatteryWidth, 0);
    const uint32_t color = charging ? 0x4cdb64
                          : level <= 10 ? 0xef4444
                          : level <= 30 ? 0xfbbf24 : 0xffffff;
    // A 1px border and 1px clear gap enclose the continuous fill.
    constexpr int kFillWidth = kBatteryWidth - 4;
    constexpr int kFillHeight = kBatteryHeight - 4;
    const int fill_width = battery_known && level > 0
                               ? std::max(1, (level * kFillWidth + 50) / 100) : 0;
    if (fill_width > 0) {
        auto* fill = Box(battery, 0, 0, fill_width, kFillHeight, color,
                         fill_width > 2 ? 2 : 0, LV_OPA_COVER);
        lv_obj_align(fill, LV_ALIGN_LEFT_MID, 1, 0);
    }
    if (charging) {
        auto* charge = Box(battery, 0, 0, 10, 12, 0, 0, LV_OPA_TRANSP);
        lv_obj_add_event_cb(charge, DrawChargingBolt, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_center(charge);
    }
    x += kBatteryWidth + 6;
    char text[12];
    std::snprintf(text, sizeof(text), "%d%%", level);
    auto* percentage = Text(status_, battery_known ? text : "--%", 0, 0, 32, 12,
                            battery_known && level <= 10 && !charging ? 0xef4444 : 0xffffff,
                            600, false, 18);
    SingleLine(percentage);
    lv_obj_set_style_text_align(percentage, LV_TEXT_ALIGN_LEFT, 0);
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

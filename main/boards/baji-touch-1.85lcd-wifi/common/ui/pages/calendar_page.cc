#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <cstdio>

using namespace baji::ui;

void WatchUi::RenderCalendar() {
    PageTitle("日历", kGreen);
    if (!snapshot_.time_valid) {
        EmptyState(content_, "calendar-days", "时间待同步", "联网后自动校准时间", kGreen);
        return;
    }
    const int64_t today = snapshot_.now - snapshot_.now % 86400;
    const int64_t chosen = today + selected_date_offset_ * 86400;
    auto date = Date(chosen);
    char heading[48];
    if (snapshot_.settings.language)
        std::snprintf(heading, sizeof(heading), "%04d / %02d / %02d", date.tm_year + 1900,
                      date.tm_mon + 1, date.tm_mday);
    else
        std::snprintf(heading, sizeof(heading), "%d月%d日 · 星期%s", date.tm_mon + 1,
                      date.tm_mday, kDays[date.tm_wday]);
    Text(content_, heading, 72, 91, 216, 14, kMuted, 400, true, 22);
    calendar_strip_ = Box(content_, 36, 119, 288, 64, kBg);
    lv_obj_add_flag(calendar_strip_, LV_OBJ_FLAG_CLICKABLE);
    auto* selected = Box(calendar_strip_, 126, 1, 36, 61, 0x17332c, 13);
    Border(selected, kGreen, 80);
    calendar_days_ = Box(calendar_strip_, 0, 0, 288, 64, 0, 0, 0);
    lv_obj_add_flag(calendar_days_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    constexpr const char* english_days[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int d = -10; d <= 10; ++d) {
        const int64_t day_start = chosen + d * 86400;
        const auto day = Date(day_start);
        auto* cell = Button(calendar_days_, 123 + d * 41, 0, 41, 64, 0, 0, [this, d] {
            if (d) page_scroll_ = nullptr;
            selected_date_offset_ += d;
            Render();
        });
        lv_obj_set_style_bg_opa(cell, 0, 0);
        Text(cell, snapshot_.settings.language ? english_days[day.tm_wday] : kDays[day.tm_wday],
             0, 7, 41, 12, d ? kMuted : kGreen, 400, true, 18);
        Text(cell, std::to_string(day.tm_mday).c_str(), 0, 26, 41, 20,
             d ? 0xc4c4ce : kText, 400, true, 26);
        const bool events = std::any_of(snapshot_.events.begin(), snapshot_.events.end(),
            [day_start](const auto& e) {
                return !e.completed && e.due >= day_start && e.due < day_start + 86400;
            });
        if (events) Box(cell, 19, 55, 3, 3, kGreen, 2);
    }
    // The strip may slide, but its sides fade into a static, flat background.
    Fade(calendar_strip_, 0, 0, 22, 64, kBg, true, false);
    Fade(calendar_strip_, 266, 0, 22, 64, kBg, true, true);
    auto* list = page_scroll_ = ScrollList(content_, 60, 195, 240, 109);
    int y = 0;
    for (const auto& event : snapshot_.events) {
        if (event.due < chosen || event.due >= chosen + 86400 || event.completed) continue;
        auto time = Date(event.due);
        auto* row = Box(list, 0, y, 240, 50, 0x16231f, 15);
        Border(row, kGreen, 30);
        Text(row, (Number(time.tm_hour) + ":" + Number(time.tm_min)).c_str(),
             12, 14, 48, 14, kGreen, 400, false, 22);
        Box(row, 69, 13, 2, 24, kGreen, 1, 90);
        auto* label = Text(row, event.title.c_str(), 82, 5, 145, 14, kText, 400, false, 20);
        SetLabelText(label, event.title.c_str());
        lv_obj_set_height(label, 42);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        y += 58;
    }
    if (!y) {
        Icon(list, "calendar-days", 110, 14, 20, kGreen);
        Text(list, selected_date_offset_ ? "这天没有日程" : "今天没有日程",
             8, 49, 224, 14, kMuted, 400, true, 22);
    }
    Text(content_, "通过 AI 添加日程", 92, 313, 176, 12, kMuted, 400, true, 18);
}

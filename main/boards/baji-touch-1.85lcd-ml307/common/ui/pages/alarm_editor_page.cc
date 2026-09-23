#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

using namespace baji::ui;

void WatchUi::RenderAlarmEditor() {
    PageTitle(alarm_draft_.id ? "编辑闹钟" : "新建闹钟", 0xfcd34d);
    Text(content_, "时", 94, 87, 80, 12, kMuted, 400, true, 18);
    Text(content_, "分", 186, 87, 80, 12, kMuted, 400, true, 18);
    hour_ = Roller(content_, 94, 106, 23, alarm_draft_.hour, 0xffdf9a);
    minute_ = Roller(content_, 186, 106, 59, alarm_draft_.minute, 0xffdf9a);
    Text(content_, ":", 174, 150, 12, 32, 0xffdf9a, 400, true, 44);
    Text(content_, AlarmRepeat(alarm_draft_.weekdays, snapshot_.settings.language).c_str(),
         70, 240, 220, 12, kMuted, 400, true, 18);
    constexpr const char* english_days[] = {"M", "T", "W", "T", "F", "S", "S"};
    for (int d = 0; d < 7; ++d) {
        bool selected = alarm_draft_.weekdays & (1 << d);
        auto* day = Button(content_, 63 + d * 34, 263, 30, 30,
                            selected ? 0x624924 : kSurface, 15, [this, d] {
            alarm_draft_.hour = lv_roller_get_selected(hour_);
            alarm_draft_.minute = lv_roller_get_selected(minute_);
            alarm_draft_.weekdays ^= 1 << d;
            Render();
        });
        Text(day, snapshot_.settings.language ? english_days[d] : kDays[(d + 1) % 7],
             0, 6, 30, 12, selected ? 0xffdf9a : kMuted, 400, true, 18);
    }
    auto* save = Button(content_, 122, 306, 116, 32, 0x624924, 16, [this] {
        auto alarm = alarm_draft_;
        alarm.hour = lv_roller_get_selected(hour_);
        alarm.minute = lv_roller_get_selected(minute_);
        auto* service = &services_;
        Submit([service, alarm](std::string* error) { return service->SaveAlarm(alarm, nullptr, error); },
               Page::Alarms);
    }, "保存", 14);
    Border(save, 0xfcd34d, 85);
}

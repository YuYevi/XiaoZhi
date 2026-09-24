#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

using namespace baji::ui;

void WatchUi::RenderAlarms() {
    DrawTabs(false);
    auto* list = page_scroll_ = ScrollList(content_, 60, 98, 240, 182);
    int y = 0;
    for (auto alarm : snapshot_.alarms) {
        auto* holder = Box(list, 0, y, 240, 64, kBg);
        auto* del = Button(holder, 188, 7, 52, 50, 0x58272c, 16, [this, alarm] {
            auto* service = &services_;
            Submit([service, alarm](std::string* error) { return service->DeleteAlarm(alarm.id, error); },
                   Page::Alarms);
        });
        CenteredIcon(del, "trash-2", 18, 0xfca5a5);
        auto* card = Button(holder, 0, 0, 240, 64, alarm.enabled ? 0x32291d : kSurface, 17,
                             [this, alarm] {
            alarm_draft_ = alarm;
            Navigate(Page::AlarmEditor);
        });
        lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<uintptr_t>(0xa11a)));
        Border(card, alarm.enabled ? 0xfcd34d : 0xffffff, alarm.enabled ? 55 : 20);
        auto* time = Text(card, (Number(alarm.hour) + ":" + Number(alarm.minute)).c_str(),
                          16, 4, 151, 32, alarm.enabled ? 0xffdf9a : 0xb0adb5, 400, false, 38);
        lv_obj_set_style_text_letter_space(time, -1, 0);
        auto repeat = AlarmRepeat(alarm.weekdays, snapshot_.settings.language);
        if (!alarm.title.empty() && alarm.title != "闹钟") repeat = alarm.title + " · " + repeat;
        auto* detail = Text(card, repeat.c_str(), 17, 42, 157, 12, kMuted, 400, false, 18);
        SingleLine(detail);
        Switch(card, alarm.enabled, 0xb88b36, [this, alarm] {
            auto value = alarm;
            value.enabled = !value.enabled;
            auto* service = &services_;
            Submit([service, value](std::string* error) { return service->SaveAlarm(value, nullptr, error); },
                   Page::Alarms);
        });
        y += 72;
    }
    if (!y)
        EmptyState(content_, "alarm-clock", "还没有闹钟", "为下一件期待的事设个提醒", 0xfcd34d, 120);
    auto* add = Button(content_, 118, 295, 124, 40, 0x624924, 20, [this] {
        alarm_draft_ = WatchAlarm{};
        alarm_draft_.weekdays = 0x1f;
        Navigate(Page::AlarmEditor);
    });
    Border(add, 0xfcd34d, 85);
    lv_obj_align(Icon(add, "plus", 0, 0, 18, 0xffdf9a), LV_ALIGN_LEFT_MID, 15, 0);
    Text(add, "添加闹钟", 43, 9, 74, 14, 0xffdf9a, 400, false, 22);
}

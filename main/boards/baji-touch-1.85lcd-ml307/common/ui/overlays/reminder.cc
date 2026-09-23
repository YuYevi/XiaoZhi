#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

using namespace baji::ui;

void WatchUi::RefreshReminder() {
    if (snapshot_.reminders.empty()) {
        if (reminder_) {
            lv_obj_delete(reminder_);
            reminder_ = nullptr;
            UpdateAnimationTimer();
        }
        reminder_token_ = 0;
        return;
    }
    const uint32_t key = snapshot_.reminders.front().token ^
                         static_cast<uint32_t>(snapshot_.reminders.size() << 24);
    if (reminder_ && key == reminder_token_) return;
    ResetMenuInteraction();
    if (reminder_) lv_obj_delete(reminder_);
    reminder_token_ = key;
    reminder_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(reminder_, LV_OBJ_FLAG_CLICKABLE);
    auto* tile = Box(reminder_, 152, 67, 56, 56, 0x392c1a, 19);
    Border(tile, 0xfcd34d, 60);
    CenteredIcon(tile, "alarm-clock", 20, 0xffdf9a);
    Text(reminder_, "时间到了", 70, 139, 220, 16, 0xffdf9a, 400, true, 24);
    std::string titles;
    for (const auto& reminder : snapshot_.reminders) {
        if (!titles.empty()) titles += '\n';
        titles += reminder.title;
    }
    auto* area = ScrollList(reminder_, 72, 177, 216, 66);
    Text(area, titles.c_str(), 0, 0, 216, 14, kText, 400, true, 22);
    auto* stop = Button(reminder_, 76, 270, 100, 40, 0x624924, 20, [this] {
        auto* service = &services_;
        Submit([service](std::string*) { service->DismissReminders(); return true; }, page_);
    }, "停止", 14);
    Border(stop, 0xfcd34d, 70);
    auto* snooze = Button(reminder_, 184, 270, 100, 40, kSurface, 20, [this] {
        auto* service = &services_;
        Submit([service](std::string* error) { return service->SnoozeReminders(300, error); }, page_);
    }, "稍后5分", 14);
    Border(snooze, 0xffffff, 24);
    if (power_overlay_) lv_obj_move_foreground(power_overlay_);
    UpdateAnimationTimer();
}

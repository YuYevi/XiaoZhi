#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

using namespace baji::ui;

void WatchUi::RenderDisplaySettings() {
    PageTitle("显示设置", kPurple);
    auto* list = page_scroll_ = ScrollList(content_, 60, 96, 240, 210);
    const bool english = snapshot_.settings.language != 0;
    auto detail = [&](lv_obj_t* item, const char* text, int width = 148) {
        SingleLine(Text(item, text, 54, 33, width, 12, kMuted, 400, false, 18));
    };
    auto* item = SettingsRow(list, 0, "moon", "自动熄屏", kBlue,
                             [this] { ShowScreenTimeoutPicker(); });
    const int timeout = snapshot_.settings.screen_timeout_seconds;
    std::string timeout_text;
    if (!timeout) timeout_text = english ? "Never" : "永不";
    else if (timeout % 60 == 0)
        timeout_text = std::to_string(timeout / 60) + (english ? " min" : " 分钟");
    else timeout_text = std::to_string(timeout) + (english ? " sec" : " 秒");
    detail(item, timeout_text.c_str());
    lv_obj_align(Icon(item, "chevron-right", 0, 0, 18, kMuted), LV_ALIGN_RIGHT_MID, -10, 0);

    const bool show_clock = snapshot_.settings.show_clock;
    auto toggle_clock = [this, show_clock] {
        auto settings = snapshot_.settings;
        settings.show_clock = !show_clock;
        SaveSettings(settings);
    };
    item = SettingsRow(list, 68, "eye", "时钟与日期", kPurple, toggle_clock);
    detail(item, "待机页面", 120);
    Switch(item, show_clock, 0x8271b3, toggle_clock);

    const bool always_on = timeout == 0;
    auto toggle_always_on = [this, always_on] {
        auto settings = snapshot_.settings;
        settings.screen_timeout_seconds = always_on ? 15 : 0;
        if (!always_on) settings.power_save = false;
        SaveSettings(settings);
    };
    item = SettingsRow(list, 136, "sun", "常亮显示", always_on ? kAmber : kMuted, toggle_always_on);
    detail(item, always_on ? (english ? "Screen stays on" : "屏幕保持亮起") :
                            (english ? "Automatic screen off" : "按设定时间熄屏"), 120);
    Switch(item, always_on, 0xa8792d, toggle_always_on);
}

void WatchUi::ShowScreenTimeoutPicker() {
    if (power_overlay_) return;
    ResetMenuInteraction();
    CloseModal();
    modal_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(modal_, LV_OBJ_FLAG_CLICKABLE);
    Text(modal_, "自动熄屏", 80, 60, 200, 16, kText, 400, true, 24);
    Box(modal_, 168, 92, 24, 2, kBlue, 1, 180);
    constexpr int seconds[] = {15, 30, 60, 120, 300, 0};
    constexpr const char* names[] = {"15 秒", "30 秒", "1 分钟", "2 分钟", "5 分钟", "永不"};
    for (int i = 0; i < 6; ++i) {
        const bool selected = snapshot_.settings.screen_timeout_seconds == seconds[i];
        auto* option = Button(modal_, 76 + i % 2 * 108, 114 + i / 2 * 50,
                              100, 42, selected ? 0x263447 : kSurface, 13,
                              [this, timeout = seconds[i]] {
            auto settings = snapshot_.settings;
            settings.screen_timeout_seconds = timeout;
            if (!timeout) settings.power_save = false;
            CloseModal();
            SaveSettings(settings);
        }, names[i], 14, 400);
        Border(option, selected ? kBlue : 0xffffff, selected ? 150 : 24);
    }
    auto* cancel = Button(modal_, 124, 277, 112, 38, kSurface, 19,
                          [this] { CloseModal(); }, "取消", 14, 400);
    Border(cancel, 0xffffff, 24);
    UpdateAnimationTimer();
}

#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <utility>

using namespace baji::ui;

void WatchUi::RenderSettings() {
    PageTitle("设置", kPurple);
    auto* list = page_scroll_ = ScrollList(content_, 60, 96, 240, 210);
    const bool english = snapshot_.settings.language != 0;
    int y = 0;
    auto row = [&](const char* icon, const char* title, uint32_t accent,
                   std::function<void()> action) {
        auto* item = SettingsRow(list, y, icon, title, accent, std::move(action));
        y += 68;
        return item;
    };
    auto detail = [&](lv_obj_t* item, const char* text, uint32_t color = kMuted, int width = 148) {
        auto* label = Text(item, text, 54, 33, width, 12, color, 400, false, 18);
        SingleLine(label);
        return label;
    };
    auto arrow = [&](lv_obj_t* item) {
        lv_obj_align(Icon(item, "chevron-right", 0, 0, 18, kMuted), LV_ALIGN_RIGHT_MID, -10, 0);
    };

    auto* item = row("sun", "显示设置", kPurple, [this] { Navigate(Page::DisplaySettings); });
    detail(item, "熄屏与待机显示");
    arrow(item);

    item = row("languages", "语言", kPurple, [this] { Navigate(Page::LanguageSettings); });
    detail(item, english ? "English" : "简体中文");
    arrow(item);

    item = row("download", "系统更新", kBlue, [this] {
        Confirm("系统更新", "重新启动并检查固件更新？", [this] { Emit(Action::CheckUpdate); });
    });
    detail(item, english ? "Check for updates" : "点击检查更新");
    arrow(item);

    item = row("info", "设备信息", kBlue, [this] { Navigate(Page::DeviceInfo); });
    detail(item, device_.firmware.c_str());
    arrow(item);

    item = row("trash-2", "恢复出厂设置", kBlue, [this] {
        Confirm("恢复出厂设置", snapshot_.settings.language ?
                "Erase all user data, including Wi-Fi, alarms, events and settings? The watch will restart and need Wi-Fi setup again. This cannot be undone." :
                "清除全部用户数据（包括 Wi-Fi、闹钟、日程和设置）并重启？之后需重新配网，此操作不可撤销。",
                [this] { Emit(Action::FactoryReset); });
    });
    detail(item, english ? "Erase all data" : "清除所有内容和设置");
    arrow(item);
    if (!snapshot_.storage_error.empty())
        Text(list, snapshot_.storage_error.c_str(), 10, y + 4, 220, 12, kRose, 400, false, 18, 14);
}

void WatchUi::RenderLanguageSettings() {
    PageTitle("语言", kPurple);
    auto* list = page_scroll_ = ScrollList(content_, 60, 96, 240, 210);
    constexpr const char* names[] = {"简体中文", "English"};
    for (int language = 0; language < 2; ++language) {
        const bool selected = snapshot_.settings.language == language;
        auto* item = Button(list, 0, language * 68, 240, 60, kSurface, 14, [this, language] {
            if (snapshot_.settings.language == language) return;
            auto settings = snapshot_.settings;
            settings.language = language;
            SaveSettings(settings);
        });
        Border(item, selected ? kBlue : 0xffffff, selected ? 150 : 22);
        SingleLine(Text(item, names[language], 20, 19, 174, 14, kText, 400, false, 22));
        if (selected) Icon(item, "check", 204, 21, 18, kBlue);
    }
}

void WatchUi::RenderDeviceInfo() {
    PageTitle("设备信息", kBlue);
    auto* list = page_scroll_ = ScrollList(content_, 60, 96, 240, 210);
    struct InfoRow {
        const char* label;
        const char* value;
    };
    // These board parameters match config.json/config.h.
    const InfoRow rows[] = {
        {"系统版本：", device_.firmware.empty() ? "--" : device_.firmware.c_str()},
        {"主控芯片：", "ESP32-S3"},
        {"屏幕：", "2.1寸屏幕"},
        {"唤醒词：", "你好小智"},
        {"MAC地址：", device_.mac_address.empty() ? "--" : device_.mac_address.c_str()},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const int y = static_cast<int>(i) * 42;
        SingleLine(Text(list, rows[i].label, 0, y + 10, 92, 14, kMuted, 400, false, 22));
        auto* value = Text(list, rows[i].value, 98, y + 10, 142, 14, kText, 400, false, 22);
        SingleLine(value);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        if (i + 1 < sizeof(rows) / sizeof(rows[0]))
            Box(list, 0, y + 41, 240, 1, 0xffffff, 0, 22);
    }
}

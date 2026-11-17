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

    item = row("languages", "语言", kPurple, [this] {
        auto settings = snapshot_.settings;
        settings.language = settings.language ? 0 : 1;
        SaveSettings(settings);
    });
    detail(item, english ? "English" : "简体中文");
    arrow(item);

    item = row("download", "系统更新", kBlue, [this] {
        Confirm("系统更新", "重新启动并检查固件更新？", [this] { Emit(Action::CheckUpdate); });
    });
    detail(item, english ? "Check for updates" : "点击检查更新");
    arrow(item);

    item = row("info", "关于手表", kMuted, [this] {
        const std::string details = "BAJI Watch\n\n" + device_.firmware + "\n" +
            LocalizedText(device_.network, snapshot_.settings.language != 0);
        SetSystemMessage(details.c_str(), 0);
    });
    detail(item, device_.firmware.c_str());

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

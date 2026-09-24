#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <utility>

using namespace baji::ui;

void WatchUi::RenderSettings() {
    PageTitle("设置", kPurple);
    auto* list = page_scroll_ = ScrollList(content_, 60, 96, 240, 210);
    const bool english = snapshot_.settings.language != 0;
    const bool wifi = device_.network.find("Wi-Fi") != std::string::npos;
    const bool cell = device_.network.find("4G") != std::string::npos;
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

    auto* item = row("wifi", "WLAN", wifi ? kBlue : kMuted, [this] {
        wifi_phase_ = "list";
        wifi_error_.clear();
        Navigate(Page::Network);
        if (wifi_scan_) wifi_scan_();
    });
    settings_wifi_label_ = detail(item, device_.wifi_connected ? device_.wifi_ssid.c_str() :
                                  english ? "Offline" : "未连接", kMuted, 120);
    if (device_.wifi_connected) SetLabelText(settings_wifi_label_, device_.wifi_ssid.c_str());
    Switch(item, wifi, 0x3b6ea8, [this, wifi] {
        Confirm("WLAN", wifi ? "设备将重启并关闭网络。" : "设备将重启以切换网络。",
                [this, wifi] { Emit(Action::SetNetwork, wifi ? -1 : 0); });
    });

    auto toggle_cell = [this, cell] {
        Confirm("移动数据", cell ? "设备将重启并关闭网络。" : "设备将重启以切换网络。",
                [this, cell] { Emit(Action::SetNetwork, cell ? -1 : 1); });
    };
    item = row("signal", "移动数据", cell ? kBlue : kMuted, toggle_cell);
    detail(item, cell ? (english ? "4G enabled" : "4G 已开启") :
                       (english ? "4G disabled" : "4G 已关闭"), kMuted, 120);
    Switch(item, cell, 0x3b6ea8, toggle_cell);

    item = row("sun", "显示设置", kPurple, [this] { Navigate(Page::DisplaySettings); });
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
        const std::string details = "BAJI Watch\n\n" + device_.firmware + "\n" + device_.network;
        SetSystemMessage(details.c_str(), 0);
    });
    detail(item, device_.firmware.c_str());

    item = row("rotate-ccw", "重置手表数据", kRose, [this] {
        Confirm("重置手表数据", snapshot_.settings.language ?
                "Clear alarms, events and watch settings? This cannot be undone." :
                "清除闹钟、日程和手表设置？此操作不可撤销。",
                [this] { Emit(Action::FactoryReset); });
    });
    lv_obj_set_style_bg_color(item, lv_color_hex(0x281b24), 0);
    Border(item, kRose, 48);
    detail(item, english ? "Wi-Fi will be kept" : "不可撤销，Wi-Fi 保留", kRose);
    arrow(item);
    if (!snapshot_.storage_error.empty())
        Text(list, snapshot_.storage_error.c_str(), 10, y + 4, 220, 12, kRose, 400, false, 18, 14);
}

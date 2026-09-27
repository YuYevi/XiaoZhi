#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

#include <algorithm>
#include <string>
#include <string_view>
#include "application.h"
#include "assets/lang_config.h"

using namespace baji::ui;

namespace {
const char* BootStateText(DeviceState state, bool english) {
    switch (state) {
        case kDeviceStateWifiConfiguring: return english ? "Set up Wi-Fi" : "等待配网";
        case kDeviceStateConnecting: return english ? "Connecting to network" : "正在连接网络";
        case kDeviceStateActivating: return english ? "Activating device" : "正在激活设备";
        case kDeviceStateUpgrading: return english ? "Updating system" : "正在更新系统";
        case kDeviceStateFatalError: return english ? "Startup error" : "启动遇到问题";
        case kDeviceStateIdle: return english ? "Ready" : "启动完成";
        case kDeviceStateStarting: return english ? "Starting system" : "正在启动系统";
        default: return english ? "Starting system" : "正在启动系统";
    }
}

std::string BootMessage(const std::string& text, bool english) {
    using namespace Lang::Strings;
    struct Translation {
        const char* native;
        const char* chinese;
        const char* english;
    };
    static constexpr Translation messages[] = {
        {INITIALIZING, "正在初始化...", "Initializing..."},
        {LOADING_PROTOCOL, "登录服务器...", "Logging in..."},
        {DETECTING_MODULE, "检测模组...", "Detecting modem..."},
        {REGISTERING_NETWORK, "等待网络...", "Waiting for network..."},
        {CHECKING_NEW_VERSION, "检查新版本...", "Checking for updates..."},
        {SCANNING_WIFI, "扫描 Wi-Fi...", "Scanning Wi-Fi..."},
        {ENTERING_WIFI_CONFIG_MODE, "进入配网模式...", "Opening Wi-Fi setup..."},
        {WIFI_CONFIG_MODE, "配网模式", "Wi-Fi setup"},
        {ACTIVATION, "激活设备", "Activate device"},
        {CONNECTING, "连接中...", "Connecting..."},
        {STANDBY, "待命", "Ready"},
        {ERROR, "错误", "Error"},
        {WARNING, "警告", "Warning"},
        {INFO, "信息", "Information"},
        {PIN_ERROR, "请插入 SIM 卡", "Please insert a SIM card"},
        {REG_ERROR, "无法接入网络，请检查流量卡状态", "Cannot connect. Check your SIM card"},
        {MODEM_INIT_ERROR, "模组初始化失败", "Modem initialization failed"},
        {SERVER_NOT_FOUND, "正在寻找可用服务", "Looking for available service"},
        {SERVER_NOT_CONNECTED, "无法连接服务，请稍后再试", "Cannot connect to service. Try again later"},
        {SERVER_TIMEOUT, "等待响应超时", "Response timed out"},
        {SERVER_ERROR, "发送失败，请检查网络", "Sending failed. Check your network"},
        {OTA_UPGRADE, "OTA 升级", "Firmware update"},
        {UPGRADING, "正在升级系统...", "Updating system..."},
        {UPGRADE_FAILED, "升级失败", "Update failed"},
        {LOADING_ASSETS, "加载资源...", "Loading resources..."},
        {DOWNLOAD_ASSETS_FAILED, "下载资源失败", "Resource download failed"},
        {PLEASE_WAIT, "请稍候...", "Please wait..."},
        {SWITCH_TO_WIFI_NETWORK, "切换到 Wi-Fi...", "Switching to Wi-Fi..."},
        {SWITCH_TO_4G_NETWORK, "切换到 4G...", "Switching to mobile data..."},
        {CONNECTION_SUCCESSFUL, "连接成功", "Connected"},
    };
    for (const auto& message : messages) {
        if (text == message.native)
            return english ? message.english : message.chinese;
    }

    // Translate only known framing. SSIDs, URLs, codes and server payloads
    // must survive the runtime language change without modification.
    const std::string_view source(text);
    const auto starts_with = [source](std::string_view prefix) {
        return !prefix.empty() && source.substr(0, prefix.size()) == prefix;
    };
    const std::string_view hotspot = CONNECT_TO_HOTSPOT;
    if (starts_with(hotspot)) {
        const std::string_view browser = ACCESS_VIA_BROWSER;
        const auto split = source.find(browser, hotspot.size());
        if (split != std::string_view::npos) {
            return std::string(english ? "Connect your phone to " : "手机连接热点 ") +
                   std::string(source.substr(hotspot.size(), split - hotspot.size())) +
                   (english ? "\nOpen " : "\n浏览器访问 ") +
                   std::string(source.substr(split + browser.size()));
        }
    }
    static constexpr Translation prefixes[] = {
        {CONNECT_TO, "连接 ", "Connecting to "},
        {CONNECTED_TO, "已连接 ", "Connected to "},
        {VERSION, "版本 ", "Version "},
        {NEW_VERSION, "新版本 ", "New version "},
    };
    for (const auto& prefix : prefixes) {
        const std::string_view native = prefix.native;
        if (starts_with(native))
            return std::string(english ? prefix.english : prefix.chinese) +
                   std::string(source.substr(native.size()));
    }
    const std::string_view retry_format = CHECK_NEW_VERSION_FAILED;
    const auto delay_offset = retry_format.find("%d");
    const auto detail_offset = retry_format.find("%s");
    if (delay_offset != std::string_view::npos && detail_offset != std::string_view::npos &&
        detail_offset > delay_offset + 2 && starts_with(retry_format.substr(0, delay_offset))) {
        const auto separator = retry_format.substr(delay_offset + 2, detail_offset - delay_offset - 2);
        const auto split = source.find(separator, delay_offset);
        if (split != std::string_view::npos) {
            const auto seconds = source.substr(delay_offset, split - delay_offset);
            if (!seconds.empty() && seconds.find_first_not_of("0123456789") == std::string_view::npos) {
                return std::string(english ? "Update check failed; retry in " : "检查新版本失败，将在 ") +
                       std::string(seconds) + (english ? " seconds:\n" : " 秒后重试：\n") +
                       std::string(source.substr(split + separator.size()));
            }
        }
    }
    const std::string_view assets_format = FOUND_NEW_ASSETS;
    const auto assets_offset = assets_format.find("%s");
    if (assets_offset != std::string_view::npos && starts_with(assets_format.substr(0, assets_offset)))
        return std::string(english ? "Found new resources: " : "发现新资源: ") +
               std::string(source.substr(assets_offset));
    return text;
}
}

void WatchUi::RenderBoot() {
    const bool english = snapshot_.settings.language != 0;
    auto* mark = Box(content_, 145, 50, 70, 70, BlendColor(kBg, kRose, .16f), 24);
    Border(mark, kRose, 75);
    CenteredIcon(mark, "assistant", 36, kRose);
    Text(content_, "BAJI", 60, 132, 240, 27, kText, 700, true, 34);
    Text(content_, english ? "AI Smartwatch" : "AI 智能手表", 60, 167, 240, 14, kMuted, 400, true, 22);

    auto* track = Box(content_, 92, 202, 176, 4, 0x2a2934, 2);
    boot_progress_ = Box(track, 0, 0, 18, 4, kRose, 2);
    boot_status_label_ = Text(content_, BootStateText(boot_state_, english),
                              60, 217, 240, 14, kText, 400, true, 22);
    // Long pairing messages and activation codes remain available by scrolling.
    // Do not truncate them with the normal transient-toast label mode.
    auto* details = ScrollList(content_, 64, 246, 232, 52);
    boot_message_ = Text(details, "", 0, 0, 232, 14, kMuted, 400, true, 20);
    boot_later_ = Button(content_, 122, 310, 116, 28, kSurface, 14,
                         [this] {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateUpgrading)
            Navigate(Page::Standby);
    },
                         english ? "Set up later" : "稍后设置", 12, 400);
    Border(boot_later_, kPurple, 50);
    boot_later_visible_ = false;
    lv_obj_add_flag(boot_later_, LV_OBJ_FLAG_HIDDEN);
    RefreshBoot();
}

void WatchUi::RefreshBoot() {
    if (!IsBooting()) return;
    const bool english = snapshot_.settings.language != 0;
    const uint32_t elapsed = lv_tick_get() - boot_started_at_;
    const bool upgrading = boot_state_ == kDeviceStateUpgrading;
    const bool can_later = elapsed >= 15000 && !upgrading && boot_state_ != kDeviceStateIdle;
    if (can_later != boot_later_visible_) {
        boot_later_visible_ = can_later;
        if (boot_later_) {
            if (can_later) lv_obj_remove_flag(boot_later_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(boot_later_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (boot_status_label_) {
        const std::string text = boot_status_.empty() ? BootStateText(boot_state_, english)
                                                     : BootMessage(boot_status_, english);
        if (std::string(lv_label_get_text(boot_status_label_)) != text)
            SetLabelText(boot_status_label_, text.c_str());
    }
    if (boot_message_) {
        const std::string text = BootMessage(boot_message_text_, english);
        if (std::string(lv_label_get_text(boot_message_)) != text) {
            SetLabelText(boot_message_, text.c_str());
            lv_obj_scroll_to_y(lv_obj_get_parent(boot_message_), 0, LV_ANIM_OFF);
        }
        if (boot_message_text_.empty()) lv_obj_add_flag(boot_message_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(boot_message_, LV_OBJ_FLAG_HIDDEN);
    }
    if (boot_progress_) {
        // This is a startup-stage cue; avoid a fake percentage while network
        // provisioning can take an arbitrary amount of time.
        const int target = boot_state_ == kDeviceStateIdle ? 176 :
                           boot_state_ == kDeviceStateUpgrading ? 152 :
                           boot_state_ == kDeviceStateActivating ? 128 :
                           boot_state_ == kDeviceStateConnecting ? 100 :
                           boot_state_ == kDeviceStateWifiConfiguring ? 72 : 36;
        lv_obj_set_width(boot_progress_, target);
    }
}

void WatchUi::UpdateBootState(DeviceState state) {
    if (!root_ || !IsBooting() || power_transition_) return;
    if (state != boot_state_) boot_status_.clear();
    boot_state_ = state;
    RefreshBoot();
    const uint32_t elapsed = lv_tick_get() - boot_started_at_;
    if (state == kDeviceStateIdle && elapsed >= 1500) {
        Navigate(Page::Standby);
    }
}

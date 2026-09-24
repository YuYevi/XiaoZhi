#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include "ui/watch_resources.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <utility>

using namespace baji::ui;

namespace {
void RemoveCharacter(std::string& s) {
    if (s.empty())
        return;
    size_t at = s.size() - 1;
    while (at && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80)
        --at;
    s.resize(at);
}
}  // namespace

void WatchUi::RenderNetwork() {
    const bool english = snapshot_.settings.language != 0;
    // This back action retains password/list cleanup; the opposite close
    // action always returns to Settings, including while connecting.
    Back(content_, [this] {
        if (wifi_phase_ == "list") Navigate(Page::Settings);
        else {
            wifi_phase_ = "list";
            wifi_password_.clear();
            wifi_error_.clear();
            Render();
        }
    });
    auto* title = Text(content_, wifi_phase_ == "list" ? "WLAN" : wifi_ssid_.c_str(),
                       104, 50, 152, 16, kText, 400, true, 24);
    if (wifi_phase_ != "list") SetLabelText(title, wifi_ssid_.c_str());
    SingleLine(title);
    Box(content_, 168, 80, 24, 2, kBlue, 1, 180);
    auto* close = Button(content_, 264, 44, 36, 36, kSurface, 18,
                         [this] { Navigate(Page::Settings); });
    Border(close, 0xffffff, 24);
    CenteredIcon(close, "x", 18, kText);

    if (wifi_phase_ == "connecting") {
        wifi_loader_ = Icon(content_, "loader", 168, 132, 24, kBlue);
        lv_image_set_pivot(wifi_loader_, 12, 12);
        Text(content_, english ? "Connecting…" : "正在连接…", 66, 181, 228,
             14, kText, 400, true, 22);
        Text(content_, english ? "Please wait" : "请稍候", 72, 211, 216,
             12, kMuted, 400, true, 18);
        return;
    }

    if (wifi_phase_ == "password") {
        auto* input = wifi_input_ = Box(content_, 55, 92, 250, 44, kSurface, 13);
        lv_obj_align(Icon(input, "lock", 0, 0, 16, kMuted), LV_ALIGN_LEFT_MID, 11, 0);
        auto* value = wifi_password_label_ = Text(input, "", 35, 11, 170, 14,
                                                  kText, 400, false, 22);
        lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
        lv_obj_set_height(value, lv_font_get_line_height(lv_obj_get_style_text_font(value, LV_PART_MAIN)));
        auto* eye = Button(input, 208, 4, 36, 36, kSurface, 10, [this] {
            password_visible_ = !password_visible_;
            RefreshWifiPassword();
        });
        wifi_eye_icon_ = CenteredIcon(eye, "eye", 18, kMuted);
        wifi_hint_ = Text(content_, "", 60, 144, 240, 12, kMuted, 400, true, 18);
        SingleLine(wifi_hint_);
        DrawKeyboard();
        RefreshWifiPassword();
        return;
    }

    const bool on = device_.network.find("Wi-Fi") != std::string::npos;
    auto* master = Box(content_, 60, 96, 240, 44, kSurface, 13);
    Border(master, 0xffffff, 22);
    auto* tile = Box(master, 12, 8, 28, 28, BlendColor(kSurface, kBlue, .14f), 9);
    CenteredIcon(tile, "wifi", 18, kBlue);
    Text(master, "WLAN", 52, 11, 122, 14, kText, 400, false, 22);
    Switch(master, on, 0x3b6ea8, [this, on] {
        Confirm("WLAN", on ? "设备将重启并关闭网络。" : "设备将重启以切换网络。",
                [this, on] { Emit(Action::SetNetwork, on ? -1 : 0); });
    });

    auto* networks = page_scroll_ = ScrollList(content_, 60, 151, 240, 124);
    int y = 0;
    for (const auto& network : wifi_networks_) {
        auto* row = Button(networks, 0, y, 240, 54,
                            network.connected ? 0x202e40 : kSurface, 13, [this, network] {
            wifi_ssid_ = network.ssid;
            wifi_password_.clear();
            wifi_error_.clear();
            password_visible_ = false;
            keyboard_layer_ = 0;
            if (network.secure) {
                wifi_phase_ = "password";
                Render();
            } else ConnectWifi();
        });
        Border(row, network.connected ? kBlue : 0xffffff, network.connected ? 110 : 22);
        const int bars = network.rssi >= -60 ? 3 : network.rssi >= -75 ? 2 : 1;
        for (int bar = 0; bar < 3; ++bar)
            Box(row, 14 + bar * 5, 34 - (6 + bar * 4), 3, 6 + bar * 4,
                bar < bars ? kGreen : 0x3b3b45, 1);
        auto* name = Text(row, network.ssid.c_str(), 44, 7, 154, 14,
                           network.connected ? kBlue : kText, 400, false, 22);
        SetLabelText(name, network.ssid.c_str());
        SingleLine(name);
        const char* state = network.connected ? (english ? "Connected" : "已连接") :
                            network.secure ? (english ? "Password required" : "需要密码") :
                                             (english ? "Open network" : "开放网络");
        auto* subtitle = Text(row, state, 44, 30, 154, 12, kMuted, 400, false, 18);
        SingleLine(subtitle);
        if (network.connected) Icon(row, "check", 212, 8, 16, kBlue);
        if (network.secure) Icon(row, "lock", 211, network.connected ? 30 : 18, 18, kMuted);
        y += 62;
    }
    if (wifi_networks_.empty())
        EmptyState(content_, "wifi", wifi_scanning_ ? (english ? "Scanning WLAN…" : "正在扫描 WLAN…") :
                   (english ? "No networks found" : "未发现网络"),
                   english ? "Use Scan again to refresh" : "点击下方按钮重新扫描", kBlue, 154);
    const std::string status = wifi_scanning_ ? (english ? "Scanning WLAN…" : "正在扫描 WLAN…") :
                               !wifi_error_.empty() ? wifi_error_ :
                               wifi_networks_.empty() ? "" :
                               (english ? "Tap a network to connect" : "点击网络可连接");
    auto* hint = Text(content_, status.c_str(), 66, 279, 228, 12,
                      wifi_error_.empty() ? kMuted : kRose, 400, true, 18);
    SingleLine(hint);
    auto* retry = Button(content_, 112, 304, 136, 34, kSurface, 17, [this] {
        if (wifi_scan_) wifi_scan_();
    });
    Border(retry, kBlue, 42);
    lv_obj_align(Icon(retry, "refresh-cw", 0, 0, 18, kBlue), LV_ALIGN_LEFT_MID, 15, 0);
    Text(retry, english ? "Scan again" : "重新扫描", 43, 6, 79, 14, kText, 400, false, 22);
}

void WatchUi::DrawKeyboard() {
    const char* lower[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const char* symbols[] = {"1234567890", "-/:;()¥@\"*", ".,?!'#+="};
    for (int row = 0; row < 3; ++row) {
        const std::string chars = keyboard_layer_ == 2 ? symbols[row] : lower[row];
        std::vector<std::string> keys;
        for (size_t position = 0; position < chars.size();) {
            const size_t end = NextCharacter(chars, position);
            auto key = chars.substr(position, end - position);
            if (keyboard_layer_ == 1) key[0] = std::toupper(static_cast<unsigned char>(key[0]));
            keys.push_back(std::move(key));
            position = end;
        }
        const int y = 170 + row * 37;
        const int row_width = row == 2 ? 234 : 250;
        const int extras = row == 2 ? (keyboard_layer_ == 2 ? 1 : 2) : 0;
        const int count = keys.size() + extras;
        const float unit = (row_width - (count - 1) * 3) / (keys.size() + extras * 1.4f);
        float position = (360 - row_width) / 2;
        auto slot = [&position, unit](float factor) {
            const int left = std::lround(position);
            position += unit * factor + 3;
            return std::pair<int, int>{left, static_cast<int>(std::lround(position - 3)) - left};
        };
        if (row == 2 && keyboard_layer_ != 2) {
            const auto [x, width] = slot(1.4f);
            auto* shift = Button(content_, x, y, width, 32,
                                  keyboard_layer_ == 1 ? 0x3b304b : kSurface, 8, [this] {
                keyboard_layer_ = keyboard_layer_ == 1 ? 0 : 1;
                Render();
            }, "⇧", 14, 400);
            Border(shift, keyboard_layer_ == 1 ? kPurple : 0xffffff,
                   keyboard_layer_ == 1 ? 110 : 28);
        }
        for (const auto& key : keys) {
            const auto [x, width] = slot(1);
            auto* button = Button(content_, x, y, width, 32, kSurface, 8, [this, key] {
                if (wifi_password_.size() + key.size() <= 63) wifi_password_ += key;
                wifi_error_.clear();
                RefreshWifiPassword();
            }, key.c_str(), 14, 400);
            Border(button, 0xffffff, 28);
        }
        if (row == 2) {
            const auto [x, width] = slot(1.4f);
            auto* erase = Button(content_, x, y, width, 32, 0x282831, 8, [this] {
                RemoveCharacter(wifi_password_);
                wifi_error_.clear();
                RefreshWifiPassword();
            });
            Border(erase, 0xffffff, 28);
            CenteredIcon(erase, "delete", 16, kText);
        }
    }
    auto* layer = Button(content_, 81, 285, 46, 34, 0x282831, 9, [this] {
        keyboard_layer_ = keyboard_layer_ == 2 ? 0 : 2;
        Render();
    }, keyboard_layer_ == 2 ? "abc" : "123", 14, 400);
    Border(layer, 0xffffff, 28);
    auto* space = Button(content_, 131, 285, 98, 34, kSurface, 9, [this] {
        if (wifi_password_.size() < 63) wifi_password_ += ' ';
        wifi_error_.clear();
        RefreshWifiPassword();
    }, "空格", 14, 400);
    Border(space, 0xffffff, 28);
    auto* join = wifi_join_ = Button(content_, 233, 285, 46, 34, kSurface, 9,
                        [this] { ConnectWifi(); });
    wifi_join_icon_ = CenteredIcon(join, "check", 18, kMuted);
}

void WatchUi::RefreshWifiPassword() {
    if (!wifi_password_label_ || wifi_phase_ != "password") return;
    const bool english = snapshot_.settings.language != 0;
    std::string shown = wifi_password_;
    if (!password_visible_) {
        shown.clear();
        for (size_t n = 0; n < std::min<size_t>(18, CharacterCount(wifi_password_)); ++n)
            shown += "•";
    }
    if (shown.empty()) shown = english ? "Wi-Fi password" : "输入 WLAN 密码";
    // Keep the newly typed end visible without scrolling animations or
    // rebuilding the keyboard. Measure with the label's actual font/tracking.
    SetLabelText(wifi_password_label_, shown.c_str());
    if (!wifi_password_.empty()) {
        const auto* font = lv_obj_get_style_text_font(wifi_password_label_, LV_PART_MAIN);
        const int spacing = lv_obj_get_style_text_letter_space(wifi_password_label_, LV_PART_MAIN);
        size_t start = 0;
        lv_point_t size;
        do {
            lv_text_get_size(&size, shown.c_str() + start, font, spacing, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
            if (size.x <= 170 || start == shown.size()) break;
            start = NextCharacter(shown, start);
        } while (true);
        if (start) SetLabelText(wifi_password_label_, shown.c_str() + start);
    }
    lv_obj_set_style_text_color(wifi_password_label_, lv_color_hex(wifi_password_.empty() ? kMuted : kText), 0);
    const bool error = !wifi_error_.empty();
    lv_obj_set_style_bg_color(wifi_input_, lv_color_hex(error ? 0x281b24 : kSurface), 0);
    Border(wifi_input_, error ? kRose : kBlue, error ? 110 : 66);
    const std::string hint = error ? wifi_error_ :
        (english ? "At least 8 characters · " : "至少 8 位 · ") +
            std::to_string(wifi_password_.size()) + "/63";
    SetLabelText(wifi_hint_, hint.c_str());
    lv_obj_set_style_text_color(wifi_hint_, lv_color_hex(error ? kRose : kMuted), 0);
    if (const auto* icon = WatchResources::Icon(password_visible_ ? "eye-off" : "eye", 18, kMuted))
        lv_image_set_src(wifi_eye_icon_, icon);
    const bool valid = wifi_password_.size() >= 8;
    const uint32_t color = valid ? 0x203c33 : kSurface;
    lv_obj_set_style_bg_color(wifi_join_, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(wifi_join_, lv_color_hex(BlendColor(color, 0xffffff, .10f)), LV_STATE_PRESSED);
    Border(wifi_join_, valid ? kGreen : 0xffffff, valid ? 120 : 28);
    lv_obj_set_style_image_recolor(wifi_join_icon_, lv_color_hex(valid ? kGreen : kMuted), 0);
}

void WatchUi::SetWifiCallbacks(std::function<void()> scan,
                               std::function<void(const std::string&, const std::string&)> connect) {
    wifi_scan_ = std::move(scan);
    wifi_connect_ = std::move(connect);
}

void WatchUi::SetWifiNetworks(const std::vector<WifiNetwork>& n, bool scanning, const char* e) {
    wifi_networks_ = n;
    wifi_scanning_ = scanning;
    wifi_error_ = e ? e : "";
    if (page_ == Page::Network && wifi_phase_ == "list")
        Render();
}

void WatchUi::SetWifiConnectionState(const char* state, const char* e) {
    std::string s = state ? state : "";
    wifi_error_ = e ? e : "";
    if (s == "connected" || s == "success") {
        wifi_phase_ = "list";
        wifi_password_.clear();
    } else if (s == "failed" || s == "error") {
        wifi_phase_ = "password";
    } else if (s == "connecting")
        wifi_phase_ = "connecting";
    if (page_ == Page::Network)
        Render();
}

void WatchUi::ConnectWifi() {
    if (!wifi_connect_) {
        SetSystemMessage("WLAN 服务暂不可用");
        return;
    }
    if (wifi_phase_ == "password" && wifi_password_.size() < 8) {
        wifi_error_ = "密码至少 8 位";
        RefreshWifiPassword();
        return;
    }
    wifi_phase_ = "connecting";
    wifi_error_.clear();
    Render();
    wifi_connect_(wifi_ssid_, wifi_password_);
}

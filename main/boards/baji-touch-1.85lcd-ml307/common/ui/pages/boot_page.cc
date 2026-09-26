#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

#include <algorithm>
#include <string>
#include "application.h"

using namespace baji::ui;

namespace {
const char* BootStateText(DeviceState state) {
    switch (state) {
        case kDeviceStateWifiConfiguring: return "等待配网";
        case kDeviceStateConnecting: return "正在连接网络";
        case kDeviceStateActivating: return "正在激活设备";
        case kDeviceStateUpgrading: return "正在更新系统";
        case kDeviceStateFatalError: return "启动遇到问题";
        case kDeviceStateIdle: return "启动完成";
        case kDeviceStateStarting: return "正在启动系统";
        default: return "正在启动系统";
    }
}
}

void WatchUi::RenderBoot() {
    auto* mark = Box(content_, 145, 50, 70, 70, BlendColor(kBg, kRose, .16f), 24);
    Border(mark, kRose, 75);
    CenteredIcon(mark, "assistant", 36, kRose);
    Text(content_, "BAJI", 60, 132, 240, 27, kText, 700, true, 34);
    Text(content_, "AI 智能手表", 60, 167, 240, 14, kMuted, 400, true, 22);

    auto* track = Box(content_, 92, 202, 176, 4, 0x2a2934, 2);
    boot_progress_ = Box(track, 0, 0, 18, 4, kRose, 2);
    boot_status_label_ = Text(content_, "正在启动系统", 60, 217, 240, 14, kText, 400, true, 22);
    // Long pairing messages and activation codes remain available by scrolling.
    // Do not truncate them with the normal transient-toast label mode.
    auto* details = ScrollList(content_, 64, 246, 232, 52);
    boot_message_ = Text(details, "", 0, 0, 232, 14, kMuted, 400, true, 20);
    boot_later_ = Button(content_, 122, 310, 116, 28, kSurface, 14,
                         [this] {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateUpgrading)
            Navigate(Page::Standby);
    },
                         "稍后设置", 12, 400);
    Border(boot_later_, kPurple, 50);
    boot_later_visible_ = false;
    lv_obj_add_flag(boot_later_, LV_OBJ_FLAG_HIDDEN);
    RefreshBoot();
}

void WatchUi::RefreshBoot() {
    if (!IsBooting()) return;
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
        const char* text = boot_status_.empty() ? BootStateText(boot_state_) : boot_status_.c_str();
        if (std::string(lv_label_get_text(boot_status_label_)) != text)
            SetLabelText(boot_status_label_, text);
    }
    if (boot_message_) {
        if (std::string(lv_label_get_text(boot_message_)) != boot_message_text_) {
            SetLabelText(boot_message_, boot_message_text_.c_str());
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

#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <cstdio>
#include <utility>

using namespace baji::ui;

void WatchUi::ConfirmFirmwareUpdate(const std::string& version, const std::string& notes,
                                    std::function<void(bool)> decision) {
    if (!root_ || firmware_overlay_ || power_transition_) {
        decision(false);
        return;
    }
    ResetMenuInteraction();
    ToggleControl(false);
    ShowPowerMenu(false);
    CloseModal();
    if (toast_) {
        lv_obj_delete(toast_);
        toast_ = nullptr;
    }
    firmware_version_ = version;
    firmware_notes_ = notes;
    firmware_decision_ = std::move(decision);
    firmware_stage_ = FirmwareUpdateStage::Confirm;
    firmware_progress_value_ = 0;
    RenderFirmwareUpdate();
}

void WatchUi::CloseFirmwareUpdate() {
    if (firmware_overlay_) lv_obj_delete(firmware_overlay_);
    firmware_overlay_ = firmware_progress_ = firmware_percent_ = nullptr;
    firmware_version_.clear();
    firmware_notes_.clear();
    auto decision = std::move(firmware_decision_);
    firmware_decision_ = {};
    UpdateAnimationTimer();
    if (decision) decision(false);
}

void WatchUi::RenderFirmwareUpdate() {
    swiped_ = false;
    if (firmware_overlay_) lv_obj_delete(firmware_overlay_);
    firmware_progress_ = firmware_percent_ = nullptr;
    firmware_overlay_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(firmware_overlay_, LV_OBJ_FLAG_CLICKABLE);
    const bool english = snapshot_.settings.language != 0;
    const bool confirm = firmware_stage_ == FirmwareUpdateStage::Confirm;
    const bool failed = firmware_stage_ == FirmwareUpdateStage::Failed;
    constexpr uint32_t accent = 0xffa52a;
    auto* panel = Box(firmware_overlay_, 48, 60, 264, 240, 0x151519, 8);
    Border(panel, accent, 150);
    Text(panel, confirm ? (english ? "Firmware update" : "固件升级") :
                        failed ? (english ? "Update failed" : "升级失败") :
                                 (english ? "Updating" : "正在升级"),
         24, 18, 216, 16, kText, 400, true, 24);
    if (confirm) {
        const std::string version = (english ? "New version: " : "发现新版本：") + firmware_version_;
        auto* label = Text(panel, version.c_str(), 24, 57, 216, 14, kText, 400, true, 22);
        SingleLine(label);
        auto* details = ScrollList(panel, 24, 88, 216, 55);
        Text(details, firmware_notes_.c_str(), 0, 0, 216, 14, kText, 400, true, 22);
        Text(panel, english ? "Update now?" : "是否立即升级？", 24, 150, 216,
             14, kText, 400, true, 22);
        Button(panel, 24, 184, 100, 38, 0x36363c, 6,
               [this] { CloseFirmwareUpdate(); }, english ? "Cancel" : "取消", 14);
        Button(panel, 136, 184, 104, 38, accent, 6, [this] {
            auto decision = std::move(firmware_decision_);
            firmware_decision_ = {};
            UpdateFirmwareUpdate(FirmwareUpdateStage::Downloading, 0);
            if (decision) decision(true);
        }, english ? "Update" : "确认升级", 14);
    } else {
        const char* message = firmware_notes_.empty() ?
            (english ? "Downloading firmware..." : "正在下载固件...") : firmware_notes_.c_str();
        if (firmware_stage_ == FirmwareUpdateStage::Verifying)
            message = english ? "Verifying firmware..." : "正在校验固件...";
        else if (firmware_stage_ == FirmwareUpdateStage::Complete)
            message = english ? "Update complete. Restarting..." : "升级完成，正在重启...";
        else if (failed)
            message = english ? "Could not update. Current firmware retained." : "升级失败，已保留当前固件";
        auto* details = ScrollList(panel, 24, 78, 216, 72);
        Text(details, message, 0, 0, 216, 14, kText, 400, true, 22);
        if (failed) {
            Button(panel, 82, 184, 100, 38, 0x36363c, 6,
                   [this] { CloseFirmwareUpdate(); }, english ? "OK" : "知道了", 14);
        } else {
            auto* track = Box(panel, 24, 164, 216, 8, 0x36363c, 4);
            firmware_progress_ = Box(track, 0, 0, 216, 8, accent, 4);
            firmware_percent_ = Text(panel, "", 24, 188, 216, 16, kText, 400, true, 24);
        }
    }
    UpdateAnimationTimer();
}

void WatchUi::UpdateFirmwareUpdate(FirmwareUpdateStage stage, int progress) {
    if (!root_) return;
    const bool rebuild = !firmware_overlay_ || firmware_stage_ != stage;
    firmware_stage_ = stage;
    firmware_progress_value_ = (stage == FirmwareUpdateStage::Verifying ||
                                stage == FirmwareUpdateStage::Complete) ? 100 : std::clamp(progress, 0, 100);
    if (rebuild) RenderFirmwareUpdate();
    if (firmware_progress_) {
        lv_obj_set_width(firmware_progress_, std::max(1, 216 * firmware_progress_value_ / 100));
        if (firmware_progress_value_ == 0) lv_obj_add_flag(firmware_progress_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(firmware_progress_, LV_OBJ_FLAG_HIDDEN);
    }
    if (firmware_percent_) {
        char text[8];
        snprintf(text, sizeof(text), "%d%%", firmware_progress_value_);
        SetLabelText(firmware_percent_, text);
    }
}

void WatchUi::CloseModal() {
    if (modal_) {
        lv_obj_delete(modal_);
        modal_ = nullptr;
    }
    if (torch_ || light_applied_) {
        torch_ = light_applied_ = false;
        Emit(Action::SetFlashlight, 0);
    }
    UpdateAnimationTimer();
}

void WatchUi::Confirm(const char* title, const char* text, std::function<void()> callback) {
    ResetMenuInteraction();
    CloseModal();
    modal_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(modal_, LV_OBJ_FLAG_CLICKABLE);
    auto* tile = Box(modal_, 156, 65, 48, 48, 0x292339, 16);
    Border(tile, kPurple, 48);
    CenteredIcon(tile, "info", 20, kPurple);
    Text(modal_, title, 70, 127, 220, 16, kText, 400, true, 24);
    auto* message = ScrollList(modal_, 72, 164, 216, 85);
    Text(message, text, 0, 0, 216, 14, kMuted, 400, true, 22);
    auto* cancel = Button(modal_, 76, 273, 100, 40, kSurface, 20,
                          [this] { CloseModal(); }, "取消", 14);
    Border(cancel, 0xffffff, 24);
    auto* accept = Button(modal_, 184, 273, 100, 40, 0x413656, 20,
                          [this, callback = std::move(callback)] {
        CloseModal();
        callback();
    }, "确认", 14);
    Border(accept, kPurple, 70);
    UpdateAnimationTimer();
}

void WatchUi::SetSystemMessage(const char* text, uint32_t milliseconds) {
    if (!root_ || !text) return;
    if (firmware_overlay_ && !IsBooting()) return;
    if (IsBooting()) {
        // Activation codes and provisioning instructions must remain readable
        // throughout startup instead of expiring as a five-second toast.
        boot_message_text_ = text;
        RefreshBoot();
        return;
    }
    if (!*text) return;
    if (!milliseconds) {
        ResetMenuInteraction();
        CloseModal();
        modal_ = Box(root_, 0, 0, 360, 360, kBg);
        lv_obj_add_flag(modal_, LV_OBJ_FLAG_CLICKABLE);
        Text(modal_, "设备提示", 80, 55, 200, 16, kText, 400, true, 24);
        auto* panel = Box(modal_, 60, 101, 240, 166, kSurface, 18);
        Border(panel, 0xffffff, 24);
        auto* area = ScrollList(panel, 16, 14, 208, 138);
        lv_obj_set_style_bg_opa(area, 0, 0);
        Text(area, text, 0, 0, 208, 14, kText, 400, false, 22);
        auto* close = Button(modal_, 120, 290, 120, 40, 0x413656, 20,
                              [this] { CloseModal(); }, "知道了", 14);
        Border(close, kPurple, 70);
        if (power_overlay_) lv_obj_move_foreground(power_overlay_);
        UpdateAnimationTimer();
        return;
    }
    if (toast_) lv_obj_delete(toast_);
    toast_ = Box(root_, 60, 142, 240, 76, 0x27232f, 20);
    lv_obj_add_flag(toast_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(toast_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(toast_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(toast_, [](lv_event_t* event) {
        auto* self = static_cast<WatchUi*>(lv_event_get_user_data(event));
        self->Emit(Action::Activity);
        const uint32_t now = lv_tick_get();
        if (static_cast<int32_t>(self->toast_deadline_ - now) < 10000)
            self->toast_deadline_ = now + 10000;
    }, LV_EVENT_PRESSED, this);
    Border(toast_, kPurple, 60);
    auto* area = ScrollList(toast_, 16, 16, 208, 44);
    lv_obj_set_style_bg_opa(area, 0, 0);
    auto* label = Text(area, text, 0, 0, 208, 14, kText, 400, true, 22);
    lv_obj_update_layout(label);
    // Network names, URLs and error codes can exceed the two-line toast.
    // Keep short notices at their usual size and let long details wrap/scroll.
    const int text_height = lv_obj_get_y(label) + lv_obj_get_height(label);
    const int body_height = std::clamp(text_height, 44, 176);
    lv_obj_set_height(area, body_height);
    lv_obj_set_height(toast_, body_height + 32);
    lv_obj_set_y(toast_, (360 - body_height - 32) / 2);
    if (text_height > body_height)
        lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_AUTO);
    else
        lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    if (text_height > 44) milliseconds = std::max(milliseconds, uint32_t{10000});
    toast_deadline_ = lv_tick_get() + milliseconds;
    if (power_overlay_) lv_obj_move_foreground(power_overlay_);
}

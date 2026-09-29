#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <utility>

using namespace baji::ui;

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

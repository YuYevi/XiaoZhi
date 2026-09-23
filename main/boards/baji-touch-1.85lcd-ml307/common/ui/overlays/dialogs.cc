#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
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
    if (!root_ || !text || !*text) return;
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
    Border(toast_, kPurple, 60);
    auto* label = Text(toast_, text, 16, 16, 208, 14, kText, 400, true, 22);
    lv_obj_set_height(label, 44);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    toast_deadline_ = lv_tick_get() + milliseconds;
    if (power_overlay_) lv_obj_move_foreground(power_overlay_);
}

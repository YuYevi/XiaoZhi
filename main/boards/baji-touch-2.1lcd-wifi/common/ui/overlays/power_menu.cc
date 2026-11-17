#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include "ui/watch_resources.h"

using namespace baji::ui;

void WatchUi::ShowPowerMenu(bool show) {
    if (show) ResetMenuInteraction();
    if (!root_ || power_transition_)
        return;
    if (power_overlay_) {
        lv_obj_delete(power_overlay_);
        power_overlay_ = power_loader_ = nullptr;
    }
    if (!show) {
        UpdateAnimationTimer();
        return;
    }
    if (!awake_)
        return;
    swiped_ = blocked_ = control_drag_ = date_drag_ = alarm_drag_ = false;
    WatchResources::SetVideoPlaying(false);
    // An opaque dark overlay avoids the prototype's expensive backdrop blur
    // and keeps bright underlying cards from competing with power actions.
    power_overlay_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(power_overlay_, LV_OBJ_FLAG_CLICKABLE);
    Text(power_overlay_, "电源", 90, 86, 180, 16, kText, 400, true, 24);
    struct Item { const char* text; const char* icon; uint32_t ink, tint; Action action; };
    constexpr Item items[] = {{"关机", "power", 0xf87171, 0xef4444, Action::PowerOff},
                              {"重启", "rotate-cw", 0xfbbf24, 0xfbbf24, Action::Reboot},
                              {"息屏", "moon", 0x93c5fd, 0x93c5fd, Action::Sleep}};
    for (int i = 0; i < 3; ++i) {
        const auto item = items[i];
        auto* hit = Button(power_overlay_, 63 + i * 84, 126, 66, 88, 0, 0, [this, item] {
            if (item.action == Action::Sleep) {
                if (torch_ || light_applied_)
                    CloseModal();
                ShowPowerMenu(false);
            }
            // Runtime accepts or rejects the request before starting a
            // non-dismissable transition (for example while updating).
            Emit(item.action);
        });
        lv_obj_set_style_bg_opa(hit, 0, 0);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        auto* circle = Box(hit, 4, 0, 58, 58, BlendColor(kBg, item.tint, .16f), 29);
        Border(circle, item.tint, i ? 102 : 107, 1);
        Glow(circle, item.tint, 5, 48);
        lv_obj_set_style_transform_pivot_x(circle, 29, 0);
        lv_obj_set_style_transform_pivot_y(circle, 29, 0);
        Icon(hit, item.icon, 23, 19, 20, item.ink);
        Bind(hit, [circle](lv_event_t* event) {
            const auto code = lv_event_get_code(event);
            if (code == LV_EVENT_PRESSED) AnimateMenuPlate(circle, 248, 90);
            if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
                AnimateMenuPlate(circle, 256, 120);
        });
        Text(hit, item.text, 0, 65, 66, 14, item.ink, 400, true, 22);
    }
    auto* cancel = Button(power_overlay_, 128, 240, 104, 38, kSurface, 19,
                          [this] { ShowPowerMenu(false); }, "取消", 14);
    Border(cancel, 0xffffff, 36);
    auto* hint = Text(power_overlay_, "继续按住 8 秒可强制关机", 55, 294, 250, 12,
                       kMuted, 400, true, 18);
    SingleLine(hint);
    UpdateAnimationTimer();
}

void WatchUi::ShowPowerTransition(bool reboot) {
    if (!root_)
        return;
    if (power_overlay_)
        lv_obj_delete(power_overlay_);
    power_overlay_ = power_loader_ = nullptr;
    power_transition_ = true;
    WatchResources::SetVideoPlaying(false);
    power_overlay_ = Box(root_, 0, 0, 360, 360, 0x000000);
    lv_obj_add_flag(power_overlay_, LV_OBJ_FLAG_CLICKABLE);
    power_loader_ = Icon(power_overlay_, "loader", 162, 145, 36, reboot ? 0xfbbf24 : 0xe6e6e6);
    lv_image_set_pivot(power_loader_, 18, 18);
    auto* label = Text(power_overlay_, reboot ? "正在重启…" : "正在关机…",
                        60, 197, 240, 12, reboot ? 0xbc8f1b : 0x9e9e9e, 400, true);
    lv_obj_set_style_text_letter_space(label, 3, 0);
    UpdateAnimationTimer();
}

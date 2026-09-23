#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <cmath>

using namespace baji::ui;

lv_obj_t* WatchUi::MenuCard(int x, int y, int width, int height, uint32_t accent,
                                  uint32_t shadow, Page destination) {
    const bool hero = destination == Page::Chat;
    const int radius = hero ? 22 : 15;
    auto* hit = Box(content_, x, y, width, height, 0, radius, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    // Reserve drawing room for the child's small shadow without enlarging
    // the touch target or clipping the shadow to the transparent hit object.
    lv_obj_add_event_cb(hit, [](lv_event_t* event) {
        lv_event_set_ext_draw_size(event, 6);
    }, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    lv_obj_refresh_ext_draw_size(hit);
    auto* plate = Box(hit, 0, 0, width, height,
                      BlendColor(kBg, accent, hero ? .94f : .28f), radius);
    if (menu_plate_count_ < menu_plates_.size()) menu_plates_[menu_plate_count_++] = plate;
    Border(plate, hero ? 0xffffff : accent, hero ? 30 : 46, 2);
    Glow(plate, shadow, 5, hero ? 51 : 61);
    lv_obj_set_style_shadow_ofs_y(plate, hero ? 2 : 1, 0);
    lv_obj_set_style_transform_pivot_x(plate, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(plate, height / 2, 0);
    // The hit area, text and icons remain at native resolution. Only this
    // empty background plate animates, so small lettering never resamples.
    struct Press { lv_point_t origin{}; bool dragged = false; uint32_t generation = 0; };
    // Dispatch copies callbacks to survive object deletion; shared gesture
    // state must therefore live outside the callback value itself.
    auto press = std::make_shared<Press>();
    Bind(hit, [this, plate, destination, press](lv_event_t* e) {
        const auto code = lv_event_get_code(e);
        auto* input = lv_indev_active();
        const bool interactive = awake_ && !power_overlay_ && !control_visible_ && !modal_ && !reminder_;
        if (code == LV_EVENT_PRESSED) {
            press->dragged = !interactive;
            press->generation = menu_input_generation_;
            if (input) lv_indev_get_point(input, &press->origin);
            if (interactive) AnimateMenuPlate(plate, 248, 90);
        } else if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED) {
            if (input) {
                lv_point_t point;
                lv_indev_get_point(input, &point);
                if (!press->dragged && (std::abs(point.x - press->origin.x) > 8 ||
                                       std::abs(point.y - press->origin.y) > 8)) {
                    press->dragged = true;
                    AnimateMenuPlate(plate, 256, 120);
                }
            }
            if (code == LV_EVENT_RELEASED) AnimateMenuPlate(plate, 256, 120);
        } else if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL) {
            press->dragged = true;
            AnimateMenuPlate(plate, 256, 120);
        } else if (code == LV_EVENT_CLICKED && interactive && !press->dragged && !swiped_ &&
                   press->generation == menu_input_generation_) {
            Emit(Action::Activity);
            QueueMenuNavigation(destination);
        }
    });
    return hit;
}

void WatchUi::ResetMenuInteraction() {
    // An overlay can open and close while LVGL still locks the old touch
    // target. Invalidate that press so its later release cannot open a page.
    ++menu_input_generation_;
    if (menu_navigation_timer_) {
        lv_timer_delete(menu_navigation_timer_);
        menu_navigation_timer_ = nullptr;
    }
    for (auto* plate : menu_plates_) {
        if (!plate) continue;
        lv_anim_delete(plate, SetMenuPlateScale);
        SetMenuPlateScale(plate, 256);
    }
}

void WatchUi::QueueMenuNavigation(Page destination) {
    if (menu_navigation_timer_) return;
    menu_destination_ = destination;
    // Leave the current input dispatch before destroying the menu's objects.
    // The owned one-shot is cancelled on redraw/destruction; no callback can
    // retain a pointer to a deleted page or interrupt a later modal.
    menu_navigation_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<WatchUi*>(lv_timer_get_user_data(timer));
        self->menu_navigation_timer_ = nullptr;
        if (!self->awake_ || self->page_ != Page::Menu || self->power_overlay_ ||
            self->control_visible_ || self->modal_ || self->reminder_) return;
        if (self->menu_destination_ == Page::Chat) self->ShowChat(true);
        else self->Navigate(self->menu_destination_);
    }, 1, this);
    if (menu_navigation_timer_) lv_timer_set_repeat_count(menu_navigation_timer_, 1);
}

void WatchUi::RenderMenu() {
    // The visual reference uses a quiet, solid canvas and a narrow centered
    // column. All coordinates are native 360px display pixels, never scaled.
    clock_ = Text(content_, "--", 100, 44, 80, 44, 0xffffff, 400, false, 49);
    clock_colon_ = Text(content_, ":", 170, 50, 20, 32, 0xffffff, 400, false, 37);
    clock_minute_ = Text(content_, "--", 190, 44, 80, 44, 0xffffff, 400, false, 49);
    for (auto* number : {clock_, clock_minute_}) {
        lv_obj_set_width(number, LV_SIZE_CONTENT);
        lv_obj_set_style_text_letter_space(number, -2, 0);
    }
    lv_obj_set_width(clock_colon_, LV_SIZE_CONTENT);
    date_ = Text(content_, "时间待同步", 70, 96, 220, 14, 0x848487, 400, true, 16);

    auto* companion = MenuCard(81, 121, 198, 66, 0xb24d72, 0x681f42, Page::Chat);
    auto* avatar = Box(companion, 20, 13, 40, 40, 0xfbcfe8, 12, 51);
    Border(avatar, 0xffffff, 41);
    CenteredIcon(avatar, "message-circle-heart", 20);
    Text(companion, "专属陪伴", 76, 16, 108, 14, 0xffffff, 400, false, 18);
    auto* subtitle = Text(companion, "随时陪着你", 76, 36, 108,
                           snapshot_.settings.language ? 12 : 14, 0xffffff, 400, false, 18);
    SingleLine(subtitle);

    struct Entry {
        int x, y, width;
        const char* label;
        const char* icon;
        uint32_t accent, shadow, icon_start, icon_end;
        Page destination;
    };
    // Keep the existing watch functions reachable with the reference's
    // alternating wide/narrow rhythm; styling and routing share this table.
    constexpr Entry entries[] = {
        {81, 197, 118, "应援灯", "lightbulb", 0x5eead4, 0x2dd4bf, 0x0f766e, 0x2dd4bf, Page::Lightstick},
        {205, 197, 74, "日历", "calendar", 0x6ee7b7, 0x34d399, 0x065f46, 0x34d399, Page::Calendar},
        {81, 266, 74, "闹钟", "alarm-clock", 0xfcd34d, 0xfbbf24, 0x92400e, 0xfbbf24, Page::Alarms},
        {161, 266, 118, "设置", "settings", 0xe879f9, 0xf0abfc, 0x86198f, 0xf0abfc, Page::Settings},
    };
    for (const auto& entry : entries) {
        auto* card = MenuCard(entry.x, entry.y, entry.width, 63,
                              entry.accent, entry.shadow, entry.destination);
        auto* tile = Box(card, 12, 8, 28, 28, entry.icon_start, 9);
        Gradient(tile, entry.icon_start, entry.icon_end);
        Border(tile, entry.accent, 77);
        CenteredIcon(tile, entry.icon, 16);
        Text(card, entry.label, 12, 41, entry.width - 24, snapshot_.settings.language ? 12 : 14,
              BlendColor(BlendColor(kBg, entry.accent, .28f), entry.accent, .78f),
              400, false, 16);
    }
}

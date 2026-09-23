#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include "ui/watch_resources.h"

using namespace baji::ui;

void WatchUi::ToggleControl(bool show) {
    if (show) ResetMenuInteraction();
    control_drag_ = false;
    if (control_) {
        lv_obj_delete(control_);
        control_ = nullptr;
    }
    brightness_ = volume_ = brightness_value_ = volume_value_ = nullptr;
    control_visible_ = show;
    if (!show) {
        UpdateAnimationTimer();
        return;
    }
    control_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(control_, LV_OBJ_FLAG_CLICKABLE);
    Text(control_, "控制中心", 90, 47, 180, 16, kText, 400, true, 24);
    const bool wifi = device_.network.find("Wi-Fi") != std::string::npos;
    const bool cell = device_.network.find("4G") != std::string::npos;
    struct Item { const char* icon; const char* label; bool active; uint32_t accent; };
    const Item items[] = {{"wifi", "WLAN", wifi, kBlue},
                          {"signal", "移动数据", cell, kBlue},
                          {"volume-x", "静音", device_.volume == 0, kRose},
                          {"moon", "息屏", false, kPurple},
                          {"battery", "省电", power_save_, kGreen},
                          {"flashlight", "手电筒", torch_, 0xfcd34d}};
    for (int i = 0; i < 6; ++i) {
        const auto item = items[i];
        auto* hit = Button(control_, 74 + (i % 3) * 74, 82 + (i / 3) * 74, 64, 68,
                            kBg, 16, [this, i, wifi, cell] {
            switch (i) {
                case 0:
                    Confirm("WLAN", wifi ? "设备将重启并关闭网络。" : "切换网络需要重新启动。",
                            [this, wifi] { Emit(Action::SetNetwork, wifi ? -1 : 0); });
                    break;
                case 1:
                    Confirm("移动数据", cell ? "设备将重启并关闭网络。" : "切换网络需要重新启动。",
                            [this, cell] { Emit(Action::SetNetwork, cell ? -1 : 1); });
                    break;
                case 2:
                    if (device_.volume) {
                        remembered_volume_ = device_.volume;
                        device_.volume = 0;
                    } else device_.volume = remembered_volume_;
                    Emit(Action::SetVolume, device_.volume);
                    ToggleControl(true);
                    break;
                case 3:
                    ToggleControl(false);
                    Emit(Action::Sleep);
                    break;
                case 4:
                    power_save_ = !power_save_;
                    Emit(Action::SetPowerSave, power_save_);
                    ToggleControl(true);
                    break;
                case 5:
                    CloseModal();
                    ResetMenuInteraction();
                    torch_ = true;
                    Emit(Action::SetFlashlight, 1);
                    modal_ = Button(root_, 0, 0, 360, 360, 0xffffff, 0, [this] {
                        torch_ = false;
                        CloseModal();
                        Emit(Action::SetFlashlight, 0);
                    });
                    break;
            }
            UpdateAnimationTimer();
        });
        auto* tile = Box(hit, 6, 0, 52, 48,
                         BlendColor(kBg, item.accent, item.active ? .35f : .10f), 16);
        Border(tile, item.accent, item.active ? 100 : 30);
        // Foreground stays stationary while the tile gently compresses.
        Icon(hit, item.icon, 22, 14, 20, item.active ? kText : item.accent);
        Bind(hit, [tile](lv_event_t* event) {
            auto code = lv_event_get_code(event);
            if (code == LV_EVENT_PRESSED) AnimateMenuPlate(tile, 248, 90);
            if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
                AnimateMenuPlate(tile, 256, 120);
        });
        lv_obj_set_style_transform_pivot_x(tile, 26, 0);
        lv_obj_set_style_transform_pivot_y(tile, 24, 0);
        auto* caption = Text(hit, item.label, -3, 51, 70, 12,
                              item.active ? kText : kMuted, 400, true, 18);
        SingleLine(caption);
    }
    auto slider = [this](int y, bool sound) {
        const uint32_t accent = sound ? kPurple : 0xfcd34d;
        auto* card = Box(control_, 72, y, 216, 40, kSurface, 16);
        Border(card, 0xffffff, 24);
        auto* icon_button = Button(card, 6, 4, 32, 32, kSurface, 12, [this, sound] {
            if (sound) {
                device_.volume = device_.volume ? 0 : remembered_volume_;
                Emit(Action::SetVolume, device_.volume);
                ToggleControl(true);
            }
        });
        auto* icon = CenteredIcon(icon_button, sound ? "volume-2" : "sun", 20, accent);
        auto* track = lv_slider_create(card);
        lv_obj_remove_style_all(track);
        lv_obj_set_pos(track, 47, 17);
        lv_obj_set_size(track, 112, 6);
        lv_slider_set_range(track, sound ? 0 : 5, 100);
        lv_slider_set_value(track, sound ? device_.volume : device_.brightness, LV_ANIM_OFF);
        lv_obj_set_ext_click_area(track, 10);
        lv_obj_set_style_radius(track, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(track, 255, LV_PART_MAIN);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x363640), LV_PART_MAIN);
        lv_obj_set_style_radius(track, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(track, 255, LV_PART_INDICATOR);
        lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(track, 255, LV_PART_KNOB);
        lv_obj_set_style_bg_color(track, lv_color_hex(kText), LV_PART_KNOB);
        lv_obj_set_style_pad_all(track, 6, LV_PART_KNOB);
        auto* value = Text(card, "", 172, 9, 34, 14, kText, 400, true, 22);
        auto refresh = [this, track, sound, accent, value, icon](int level) {
            const bool muted = sound && level == 0;
            const std::string caption = muted ? (snapshot_.settings.language ? "Mute" : "静音")
                                               : std::to_string(level);
            SetLabelText(value, caption.c_str());
            const auto* font = Font(muted && snapshot_.settings.language ? 12 : 14);
            lv_obj_set_style_text_font(value, font, 0);
            lv_obj_set_y(value, 9 + (22 - font->line_height) / 2);
            const auto color = muted ? kMuted : accent;
            if (auto* image = WatchResources::Icon(sound ? (muted ? "volume-x" : "volume-2") : "sun", 20, color))
                lv_image_set_src(icon, image);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(color), 0);
            lv_obj_set_style_bg_color(track, lv_color_hex(color), LV_PART_INDICATOR);
        };
        refresh(lv_slider_get_value(track));
        if (sound) { volume_ = track; volume_value_ = value; }
        else { brightness_ = track; brightness_value_ = value; }
        Bind(track, [this, track, sound, refresh](lv_event_t* event) {
            const auto code = lv_event_get_code(event);
            if (code == LV_EVENT_VALUE_CHANGED) refresh(lv_slider_get_value(track));
            if (code == LV_EVENT_RELEASED) {
                if (control_drag_ || swiped_) {
                    int saved = sound ? device_.volume : device_.brightness;
                    lv_slider_set_value(track, saved, LV_ANIM_OFF);
                    refresh(saved);
                    return;
                }
                int level = lv_slider_get_value(track);
                if (sound) device_.volume = level;
                else device_.brightness = level;
                Emit(sound ? Action::SetVolume : Action::SetBrightness, level);
                if (sound) ToggleControl(true);
            }
        });
    };
    slider(233, false);
    slider(281, true);
    Box(control_, 160, 337, 40, 3, kMuted, 2, 90);
    if (status_) lv_obj_move_foreground(status_);
    if (reminder_) lv_obj_move_foreground(reminder_);
    UpdateAnimationTimer();
}

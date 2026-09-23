#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include "application.h"
#include "assets.h"

using namespace baji::ui;

void WatchUi::LoadWallpapers() {
    if (wallpaper_data_[0] && wallpaper_data_[1] && wallpaper_data_[2])
        return;
    if (Application::GetInstance().GetDeviceState() == kDeviceStateUpgrading)
        return;
    if (asset_check_ && lv_tick_get() - asset_check_ < 5000)
        return;
    asset_check_ = lv_tick_get();
    for (int i = 0; i < 3; ++i) {
        if (wallpaper_data_[i])
            continue;
        char n[48];
        std::snprintf(n, sizeof(n), "watch_wallpaper_%d.rgb565", i);
        void* p = nullptr;
        size_t s = 0;
        if (!Assets::GetInstance().GetAssetData(n, p, s) || s < 12)
            continue;
        lv_image_header_t h{};
        std::memcpy(&h, p, sizeof(h));
        if (h.magic != LV_IMAGE_HEADER_MAGIC || h.w != 360 || h.h != 360 ||
            h.cf != LV_COLOR_FORMAT_RGB565 || s != 259212)
            continue;
        auto data = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[s - sizeof(h)]);
        if (!data)
            continue;
        std::memcpy(data.get(), static_cast<uint8_t*>(p) + sizeof(h), s - sizeof(h));
        wallpapers_[i].header = h;
        wallpapers_[i].data_size = s - sizeof(h);
        wallpapers_[i].data = data.get();
        wallpaper_data_[i] = std::move(data);
    }
    if (page_ == Page::Standby && wallpaper_obj_ && wallpaper_data_[wallpaper_])
        lv_image_set_src(wallpaper_obj_, &wallpapers_[wallpaper_]);
}

void WatchUi::RenderStandby() {
    wallpaper_obj_ = lv_image_create(content_);
    lv_obj_set_size(wallpaper_obj_, 360, 360);
    LoadWallpapers();
    if (wallpaper_data_[wallpaper_])
        lv_image_set_src(wallpaper_obj_, &wallpapers_[wallpaper_]);
    lv_obj_remove_flag(wallpaper_obj_, LV_OBJ_FLAG_CLICKABLE);
    // Linear shades leave the portrait clear without concentric dark bands.
    Fade(content_, 0, 0, 360, 155, kBg, false, false);
    Fade(content_, 0, 252, 360, 108, kBg, false, true);
    if (snapshot_.settings.show_clock) {
        clock_ = Text(content_, "--:--", 80, 60, 200, 44, kText, 400, true, 52);
        date_ = Text(content_, "", 74, 116, 212, 14, 0xd8d5df, 400, true, 22);
    }
    // Restore BAJI's top-centered carousel markers. Solid 6px shapes and a
    // narrow dark outline keep them readable on both light and dark photos.
    int x = 160;
    for (int i = 0; i < 3; ++i) {
        int width = i == wallpaper_ ? 16 : 6;
        auto* dot = Box(content_, x, 56, width, 6, 0xffffff, LV_RADIUS_CIRCLE,
                        i == wallpaper_ ? LV_OPA_COVER : 160);
        lv_obj_set_style_outline_width(dot, 1, 0);
        lv_obj_set_style_outline_color(dot, lv_color_black(), 0);
        lv_obj_set_style_outline_opa(dot, 160, 0);
        x += width + 6;
    }
}

void WatchUi::NextWallpaper(int d) {
    const int previous = wallpaper_;
    wallpaper_ = (wallpaper_ + d + 3) % 3;
    wallpaper_tick_ = lv_tick_get();
    if (page_ == Page::Standby) {
        Render();
        if (wallpaper_data_[previous] && awake_) {
            auto* fade = lv_image_create(content_);
            lv_image_set_src(fade, &wallpapers_[previous]);
            lv_obj_set_size(fade, 360, 360);
            lv_obj_remove_flag(fade, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_to_index(fade, 1);
            lv_anim_t anim;
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, fade);
            lv_anim_set_values(&anim, 255, 0);
            lv_anim_set_duration(&anim, 500);
            lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, 0);
            });
            lv_anim_set_completed_cb(
                &anim, [](lv_anim_t* value) { lv_obj_delete(static_cast<lv_obj_t*>(value->var)); });
            lv_anim_start(&anim);
        }
    }
}

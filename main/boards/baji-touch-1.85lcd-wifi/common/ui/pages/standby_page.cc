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
    bool current_loaded = false;
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
            h.cf != LV_COLOR_FORMAT_RGB565 || h.stride != 720 || s != 259212)
            continue;
        auto data = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[s - sizeof(h)]);
        if (!data)
            continue;
        std::memcpy(data.get(), static_cast<uint8_t*>(p) + sizeof(h), s - sizeof(h));
        // Preserve the native 360x360 RGB565 pixels, including highlights and
        // dark detail. Readability belongs to the individual text controls.
        wallpapers_[i].header = h;
        wallpapers_[i].data_size = s - sizeof(h);
        wallpapers_[i].data = data.get();
        wallpaper_data_[i] = std::move(data);
        current_loaded |= i == wallpaper_transition_.To();
    }
    if (page_ == Page::Standby && wallpaper_obj_) {
        if (current_loaded)
            lv_image_set_src(wallpaper_obj_, &wallpapers_[wallpaper_transition_.To()]);
        StartWallpaperTransition();
    }
}

void WatchUi::RenderStandby() {
    wallpaper_obj_ = lv_image_create(content_);
    lv_obj_set_size(wallpaper_obj_, 360, 360);
    LoadWallpapers();
    if (wallpaper_data_[wallpaper_transition_.To()])
        lv_image_set_src(wallpaper_obj_, &wallpapers_[wallpaper_transition_.To()]);
    lv_obj_remove_flag(wallpaper_obj_, LV_OBJ_FLAG_CLICKABLE);
    // Reuse a single overlay beneath the labels for every transition. Image
    // opacity blends directly instead of allocating an object-opacity layer.
    wallpaper_fade_ = lv_image_create(content_);
    lv_obj_set_size(wallpaper_fade_, 360, 360);
    lv_obj_remove_flag(wallpaper_fade_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wallpaper_fade_, LV_OBJ_FLAG_HIDDEN);
    if (snapshot_.settings.show_clock) {
        clock_ = Text(content_, "--:--", 80, 60, 200, 44, kText, 400, true, 52);
        date_ = Text(content_, "", 74, 116, 212, 14, 0xd8d5df, 400, true, 22);
    }
    // Restore BAJI's top-centered carousel markers. Solid 6px shapes and a
    // narrow dark outline keep them readable on both light and dark photos.
    for (int i = 0; i < 3; ++i) {
        auto* dot = Box(content_, 160, 56, 6, 6, 0xffffff, LV_RADIUS_CIRCLE, 160);
        wallpaper_dots_[i] = dot;
        lv_obj_set_style_outline_width(dot, 1, 0);
        lv_obj_set_style_outline_color(dot, lv_color_black(), 0);
        lv_obj_set_style_outline_opa(dot, 160, 0);
    }
    RefreshWallpaperMarkers();
}

void WatchUi::NextWallpaper(int d) {
    wallpaper_transition_.Step(d);
    wallpaper_tick_ = lv_tick_get();
    StartWallpaperTransition();
}

void WatchUi::RefreshWallpaperMarkers() {
    int x = 160;
    for (int i = 0; i < 3; ++i) {
        const bool selected = i == wallpaper_transition_.To();
        const int width = selected ? 16 : 6;
        if (wallpaper_dots_[i]) {
            lv_obj_set_x(wallpaper_dots_[i], x);
            lv_obj_set_width(wallpaper_dots_[i], width);
            lv_obj_set_style_bg_opa(wallpaper_dots_[i], selected ? LV_OPA_COVER : 160, 0);
        }
        x += width + 6;
    }
}

void WatchUi::StartWallpaperTransition() {
    if (page_ != Page::Standby || !wallpaper_obj_ || !wallpaper_fade_ ||
        wallpaper_transition_.Active() || !wallpaper_transition_.Pending() ||
        !wallpaper_data_[wallpaper_transition_.Requested()])
        return;
    if (!awake_ || !wallpaper_data_[wallpaper_transition_.To()]) {
        wallpaper_transition_.Settle();
        lv_image_set_src(wallpaper_obj_, &wallpapers_[wallpaper_transition_.To()]);
        RefreshWallpaperMarkers();
        return;
    }

    wallpaper_transition_.Begin(lv_tick_get());
    lv_image_set_src(wallpaper_fade_, &wallpapers_[wallpaper_transition_.From()]);
    lv_obj_set_style_image_opa(wallpaper_fade_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(wallpaper_fade_, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(wallpaper_obj_, &wallpapers_[wallpaper_transition_.To()]);
    RefreshWallpaperMarkers();
    UpdateAnimationTimer();
}

void WatchUi::AnimateWallpaper(uint32_t now) {
    if (page_ != Page::Standby || !wallpaper_fade_ ||
        !wallpaper_transition_.Active() || wallpaper_transition_.Paused())
        return;
    lv_obj_set_style_image_opa(wallpaper_fade_, wallpaper_transition_.Opacity(now), 0);
    if (wallpaper_transition_.Complete(now)) {
        lv_obj_add_flag(wallpaper_fade_, LV_OBJ_FLAG_HIDDEN);
        StartWallpaperTransition();
        UpdateAnimationTimer();
    }
}

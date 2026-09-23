#pragma once

#include <lvgl.h>
#include <cstdint>

// All calls run under the LVGL display lock. Returned fonts and icons live for
// the firmware lifetime. The pack is generated from the supplied prototype.
class WatchResources {
public:
    // Latin keeps CSS half-pixel sizes and weights 400/600/700/800. CJK uses
    // regular Noto at integer pixels; the UI selects readable 12/14px sizes.
    // Legacy callers passing false/true still select 400/700 respectively.
    static const lv_font_t* Font(float px, int weight = 400);
    // White alpha bitmap: apply the desired color with LVGL image recolor.
    static const lv_image_dsc_t* Icon(const char* name, int px, uint32_t color = 0xffffff);
    // One silent video player for the standby/speaking clips on the AI page.
    // Deleting the target image automatically stops playback and frees buffers.
    // Reads one compressed frame under the display lock. If assets change or
    // upgrading begins, the last frame remains visible until a new AttachVideo.
    static bool AttachVideo(lv_obj_t* image, bool speaking = false);
    // Version 1 packs need UI overlays; version 2+ bakes them into video.
    // Version 3 also retains fractional CSS font sizes and numeric weights.
    // Version 4 stores separate standby and speaking frame ranges.
    static bool VideoIncludesShading();
    // Reuses the decoder and buffers; repeating the same state keeps its phase.
    // A change while paused is presented when playback resumes.
    static void SetVideoSpeaking(bool speaking);
    static void SetVideoPlaying(bool enabled);
    static void StopVideo();
};

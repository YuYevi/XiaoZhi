#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

using namespace baji::ui;

void WatchUi::RenderCountdown() {
    DrawTabs(true);
    const auto countdown = snapshot_.countdown;
    if (!countdown.active) {
        Text(content_, "分", 94, 97, 80, 12, kMuted, 400, true, 18);
        Text(content_, "秒", 186, 97, 80, 12, kMuted, 400, true, 18);
        hour_ = Roller(content_, 94, 119, 99, 5, kGreen);
        minute_ = Roller(content_, 186, 119, 59, 0, kGreen);
        Text(content_, ":", 174, 163, 12, 32, kGreen, 400, true, 44);
        Text(content_, "留一点时间给自己", 80, 264, 200, 12, kMuted, 400, true, 18);
        auto* start = Button(content_, 114, 298, 132, 40, 0x20564d, 20, [this] {
            const uint32_t seconds = lv_roller_get_selected(hour_) * 60 + lv_roller_get_selected(minute_);
            auto* service = &services_;
            Submit([service, seconds](std::string* error) { return service->StartCountdown(seconds, error); },
                   Page::Countdown);
        });
        Border(start, kTeal, 85);
        lv_obj_align(Icon(start, "play", 0, 0, 18, kGreen), LV_ALIGN_LEFT_MID, 27, 0);
        Text(start, "开始", 57, 9, 58, 14, kGreen, 400, false, 22);
        return;
    }
    // A single progress arc expresses remaining time. It redraws only when
    // the seconds change; no liquid texture or continuous wave animation.
    timer_progress_ = lv_arc_create(content_);
    lv_obj_remove_style_all(timer_progress_);
    lv_obj_set_pos(timer_progress_, 80, 88);
    lv_obj_set_size(timer_progress_, 200, 200);
    lv_arc_set_rotation(timer_progress_, 270);
    lv_arc_set_bg_angles(timer_progress_, 0, 360);
    lv_arc_set_range(timer_progress_, 0, 1000);
    lv_obj_set_style_arc_color(timer_progress_, lv_color_hex(0x1a302b), LV_PART_MAIN);
    lv_obj_set_style_arc_width(timer_progress_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(timer_progress_, 255, LV_PART_MAIN);
    lv_obj_set_style_arc_color(timer_progress_, lv_color_hex(countdown.paused ? 0x66998c : kTeal), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(timer_progress_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(timer_progress_, 255, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(timer_progress_, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(timer_progress_, LV_OBJ_FLAG_CLICKABLE);
    Icon(content_, "timer", 170, 119, 20, kTeal);
    timer_label_ = Text(content_, "", 85, 150, 190, countdown.duration_seconds >= 3600 ? 32 : 44,
                        kText, 400, true, 52);
    Text(content_, countdown.paused ? "已暂停" : "正在倒计时", 100, 207, 160, 14,
         countdown.paused ? kMuted : kGreen, 400, true, 22);
    std::string total = (snapshot_.settings.language ? "Total " : "共 ") +
        CountdownText(countdown.duration_seconds, countdown.duration_seconds >= 3600);
    Text(content_, total.c_str(), 112, 237, 136, 12, kMuted, 400, true, 18);
    auto* reset = Button(content_, 113, 300, 44, 38, kSurface, 19, [this] {
        auto* service = &services_;
        Submit([service](std::string*) { service->CancelCountdown(); return true; }, Page::Countdown);
    });
    Border(reset, 0xffffff, 25);
    CenteredIcon(reset, "rotate-ccw", 18, kMuted);
    auto* pause = Button(content_, 169, 300, 78, 38, 0x20564d, 19, [this, countdown] {
        auto* service = &services_;
        Submit([service, countdown](std::string* error) {
            return countdown.paused ? service->ResumeCountdown(error) : service->PauseCountdown(error);
        }, Page::Countdown);
    }, countdown.paused ? "继续" : "暂停", 14);
    Border(pause, kTeal, 75);
}

void WatchUi::RefreshCountdown() {
    if (timer_label_) {
        const auto seconds = snapshot_.countdown.remaining_seconds;
        const auto time = CountdownText(seconds, snapshot_.countdown.duration_seconds >= 3600);
        if (time != lv_label_get_text(timer_label_)) lv_label_set_text(timer_label_, time.c_str());
    }
    if (timer_progress_) {
        const auto& countdown = snapshot_.countdown;
        const int progress = countdown.duration_seconds ?
            static_cast<uint64_t>(countdown.remaining_seconds) * 1000 / countdown.duration_seconds : 0;
        lv_arc_set_value(timer_progress_, progress);
    }
}

#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
#include "application.h"
#include "ui/watch_resources.h"

using namespace baji::ui;

struct WatchUi::AsyncState {
    std::mutex mutex;
    bool busy = false, done = false, ok = false;
    Page destination = Page::Standby;
    uint32_t navigation_generation = 0;
    std::string text;
};

WatchUi::WatchUi(lv_display_t* d, const lv_font_t* f, const lv_font_t* i, WatchServices& s,
                 ActionCallback a)
    : display_(d),
      fallback_font_(f),
      fallback_icon_(i),
      services_(s),
      action_(std::move(a)),
      async_(std::make_shared<AsyncState>()) {}

WatchUi::~WatchUi() {
    if (menu_navigation_timer_) lv_timer_delete(menu_navigation_timer_);
    if (completion_timer_) lv_timer_delete(completion_timer_);
    if (animation_timer_)
        lv_timer_delete(animation_timer_);
    if (root_)
        lv_obj_delete(root_);
}

void WatchUi::Emit(Action a, int v) {
    if (action_)
        action_(a, v);
}

void WatchUi::Create() {
    if (root_)
        return;
    snapshot_ = services_.Snapshot();
    root_ = Box(lv_display_get_screen_active(display_), 0, 0, 360, 360, kBg);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(
        root_, [](lv_event_t* e) { static_cast<WatchUi*>(lv_event_get_user_data(e))->Gesture(e); },
        LV_EVENT_ALL, this);
    animation_timer_ = lv_timer_create(
        [](lv_timer_t* timer) { static_cast<WatchUi*>(lv_timer_get_user_data(timer))->Animate(); }, 33,
        this);
    // Storage completes on Application's task. Poll only during a submitted
    // operation, and apply its result on LVGL without waiting for the 1 s
    // device-status tick or retaining this object in the storage callback.
    completion_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<WatchUi*>(lv_timer_get_user_data(timer));
        bool done;
        {
            std::lock_guard<std::mutex> lock(self->async_->mutex);
            done = self->async_->done;
        }
        if (done) self->Tick();
    }, 20, this);
    if (completion_timer_) lv_timer_pause(completion_timer_);
    Render();
}

void WatchUi::SetFonts(const lv_font_t* f, const lv_font_t* i) {
    fallback_font_ = f;
    fallback_icon_ = i;
    if (root_) {
        bool c = control_visible_;
        ToggleControl(false);
        CloseModal();
        if (toast_) {
            lv_obj_delete(toast_);
            toast_ = nullptr;
        }
        if (reminder_) {
            lv_obj_delete(reminder_);
            reminder_ = nullptr;
            reminder_token_ = 0;
        }
        Render();
        if (c)
            ToggleControl(true);
        RefreshReminder();
    }
}

void WatchUi::Navigate(Page p) {
    ++navigation_generation_;
    if (page_ == Page::Chat && p != page_)
        Emit(Action::StopChat);
    ToggleControl(false);
    CloseModal();
    page_ = p;
    // A deliberate navigation starts at the new page's beginning. Local
    // rebuilds preserve the current list position below.
    page_scroll_ = nullptr;
    snapshot_ = services_.Snapshot();
    Render();
}

void WatchUi::Render() {
    ResetMenuInteraction();
    if (!root_)
        return;
    const int32_t scroll_y = page_scroll_ ? lv_obj_get_scroll_y(page_scroll_) : 0;
    page_scroll_ = nullptr;
    if (status_) {
        lv_obj_delete(status_);
        status_ = nullptr;
    }
    if (content_)
        lv_obj_delete(content_);
    menu_plates_.fill(nullptr);
    menu_plate_count_ = 0;
    content_ = Box(root_, 0, 0, 360, 360, kBg);
    lv_obj_add_flag(content_, LV_OBJ_FLAG_CLICKABLE);
    status_ = clock_ = date_ = user_label_ = answer_label_ = chat_header_ = chat_mic_ = chat_timer_ =
        wallpaper_obj_ = hour_ = minute_ = timer_label_ = timer_progress_ = calendar_strip_ =
            alarm_drag_row_ = nullptr;
    chat_header_text_.clear();
    wifi_loader_ = thinking_ = answer_cursor_ = nullptr;
    wifi_input_ = wifi_password_label_ = wifi_hint_ = wifi_eye_icon_ = wifi_join_ = wifi_join_icon_ = nullptr;
    calendar_days_ = clock_minute_ = clock_colon_ = chat_timer_dot_ = settings_wifi_label_ =
        nullptr;
    voice_bars_.fill(nullptr);
    thinking_dots_.fill(nullptr);
    // Central page routing; page implementations live in pages/.
    switch (page_) {
        case Page::Standby:
            RenderStandby();
            break;
        case Page::Menu:
            RenderMenu();
            break;
        case Page::Chat:
            RenderChat();
            break;
        case Page::Calendar:
            RenderCalendar();
            break;
        case Page::Alarms:
            RenderAlarms();
            break;
        case Page::Countdown:
            RenderCountdown();
            break;
        case Page::Settings:
            RenderSettings();
            break;
        case Page::DisplaySettings:
            RenderDisplaySettings();
            break;
        case Page::AlarmEditor:
            RenderAlarmEditor();
            break;
        case Page::Network:
            RenderNetwork();
            break;
        case Page::Lightstick:
            RenderLightstick();
            break;
    }
    RenderStatus();
    if (page_scroll_ && scroll_y) {
        lv_obj_update_layout(page_scroll_);
        // LVGL clamps the saved offset if deleting a row shortened the list.
        lv_obj_scroll_to_y(page_scroll_, scroll_y, LV_ANIM_OFF);
    }
    RefreshDynamic();
    UpdateAnimationTimer();
    revision_ = snapshot_.revision;
    if (control_)
        lv_obj_move_foreground(control_);
    if (status_)
        lv_obj_move_foreground(status_);
    if (reminder_)
        lv_obj_move_foreground(reminder_);
    if (toast_)
        lv_obj_move_foreground(toast_);
    if (modal_)
        lv_obj_move_foreground(modal_);
    if (power_overlay_)
        lv_obj_move_foreground(power_overlay_);
}

void WatchUi::UpdateAnimationTimer() {
    const bool covered = power_overlay_ || control_visible_ || modal_ || reminder_;
    const bool play_video = awake_ && page_ == Page::Chat && !covered;
    if (!play_video) WatchResources::SetVideoPlaying(false);
    if (page_ == Page::Chat)
        WatchResources::SetVideoSpeaking(Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking);
    if (play_video) WatchResources::SetVideoPlaying(true);
    if (!animation_timer_) return;
    if (!awake_ || (covered && !power_loader_) ||
        (page_ != Page::Chat && page_ != Page::Menu && !wifi_loader_ && !power_loader_)) {
        lv_timer_pause(animation_timer_);
        return;
    }
    lv_timer_set_period(animation_timer_, power_loader_ || wifi_loader_ ? 33 : page_ == Page::Menu ? 1000 : 50);
    lv_timer_resume(animation_timer_);
}

void WatchUi::Animate() {
    if (!awake_)
        return;
    const uint32_t now = lv_tick_get();
    if (power_overlay_) {
        if (power_loader_)
            lv_image_set_rotation(power_loader_, (now % 900) * 3600 / 900);
        return;
    }
    if (clock_colon_) {
        const lv_opa_t opacity = (now / 1000) % 2 ? 89 : 255;
        if (lv_obj_get_style_opa(clock_colon_, LV_PART_MAIN) != opacity)
            lv_obj_set_style_opa(clock_colon_, opacity, 0);
    }
    if (wifi_loader_)
        lv_image_set_rotation(wifi_loader_, (now % 900) * 3600 / 900);
    AnimateChat(now);
}

void WatchUi::RefreshDynamic() {
    std::string time = "--:--";
    if (clock_ || chat_header_) {
        if (snapshot_.time_valid) {
            auto date = Date(snapshot_.now);
            time = Number(date.tm_hour) + ":" + Number(date.tm_min);
        }
    }
    RefreshClock(time);
    RefreshChat(time);
    RefreshCountdown();
}

void WatchUi::UpdateDeviceSnapshot(const DeviceSnapshot& d) {
    bool changed =
        d.network != device_.network || d.battery != device_.battery || d.charging != device_.charging;
    bool wifi_changed = d.wifi_connected != device_.wifi_connected || d.wifi_ssid != device_.wifi_ssid;
    const bool chat_changed = d.chat_active != device_.chat_active;
    device_ = d;
    if (wifi_changed && settings_wifi_label_) {
        const char* text = device_.wifi_connected        ? device_.wifi_ssid.c_str()
                           : snapshot_.settings.language ? "Offline"
                                                         : "未连接";
        const auto* font = Font(12);
        SetLabelText(settings_wifi_label_, text);
        lv_obj_set_style_text_font(settings_wifi_label_, font, 0);
        lv_obj_set_style_text_line_space(settings_wifi_label_,
                                        18 - font->line_height, 0);
        SingleLine(settings_wifi_label_);
        lv_obj_set_pos(settings_wifi_label_, 54, 33 + std::lround((18 - font->line_height) * .5f));
        lv_obj_set_width(settings_wifi_label_, 120);
    }
    if (device_.volume > 0)
        remembered_volume_ = device_.volume;
    if (changed && content_)
        RenderStatus();
    if (chat_changed) RefreshMicrophone();
}

void WatchUi::Submit(std::function<bool(std::string*)> work, Page destination, const char* success) {
    auto state = async_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->busy)
            return;
        state->busy = true;
        state->navigation_generation = navigation_generation_;
    }
    std::string text = success;
    Application::GetInstance().Schedule([state, work = std::move(work), destination, text] {
        std::string e;
        bool ok = work(&e);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ok = ok;
        state->destination = destination;
        state->text = ok ? text : e;
        state->done = true;
    });
    if (completion_timer_) {
        lv_timer_reset(completion_timer_);
        lv_timer_resume(completion_timer_);
    }
}

void WatchUi::SaveSettings(const WatchSettings& settings) {
    auto* s = &services_;
    Submit([s, settings](std::string* e) { return s->SaveSettings(settings, e); }, page_);
}

void WatchUi::SetAwake(bool awake) {
    if (!awake) ResetMenuInteraction();
    awake_ = awake;
    if (!awake && power_overlay_) {
        lv_obj_delete(power_overlay_);
        power_overlay_ = power_loader_ = nullptr;
        power_transition_ = false;
    }
    wallpaper_tick_ = lv_tick_get();
    UpdateAnimationTimer();
    if (awake)
        Tick();
}

void WatchUi::GoBack() {
    if (power_transition_)
        return;
    if (power_overlay_) {
        ShowPowerMenu(false);
        return;
    }
    if (modal_) {
        CloseModal();
        return;
    }
    if (control_visible_) {
        ToggleControl(false);
        return;
    }
    switch (page_) {
        case Page::Standby:
            break;
        case Page::Menu:
            Navigate(Page::Standby);
            break;
        case Page::Chat:
            Navigate(chat_return_);
            break;
        case Page::AlarmEditor:
            Navigate(Page::Alarms);
            break;
        case Page::DisplaySettings:
            Navigate(Page::Settings);
            break;
        case Page::Network:
            if (wifi_phase_ != "list") {
                wifi_phase_ = "list";
                wifi_password_.clear();
                Render();
            } else
                Navigate(Page::Settings);
            break;
        default:
            Navigate(Page::Menu);
            break;
    }
}

void WatchUi::Gesture(lv_event_t* e) {
    if (!awake_ || power_overlay_)
        return;
    auto code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_GESTURE)
        return;
    auto* indev = lv_indev_active();
    if (!indev)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        press_ = point;
        // The prototype's WLAN panel owns all touch gestures, including the
        // keyboard and network-list scrolling (data-swipe-block).
        blocked_ = page_ == Page::Network && !control_visible_;
        swiped_ = false;
        control_drag_ = date_drag_ = alarm_drag_ = false;
        alarm_drag_row_ = nullptr;
        for (auto* o = static_cast<lv_obj_t*>(lv_event_get_target(e)); o && o != root_;
             o = lv_obj_get_parent(o)) {
            if (lv_obj_check_type(o, &lv_roller_class) || lv_obj_check_type(o, &lv_slider_class))
                blocked_ = true;
            if (o == calendar_strip_)
                date_drag_ = true;
            if (lv_obj_get_user_data(o) == reinterpret_cast<void*>(static_cast<uintptr_t>(0xa11a))) {
                alarm_drag_row_ = o;
                alarm_drag_start_x_ = lv_obj_get_x(o);
            }
        }
        control_start_y_ = control_visible_ ? 0 : -360;
        if (date_drag_ && calendar_days_) {
            lv_anim_delete(calendar_days_, nullptr);
            date_drag_start_x_ = lv_obj_get_x(calendar_days_);
        }
        Emit(Action::Activity);
        return;
    }
    int dx = point.x - press_.x, dy = point.y - press_.y;
    if (modal_ || reminder_)
        return;
    if (code == LV_EVENT_PRESSING) {
        if (page_ == Page::Network && !control_visible_) {
            if (std::abs(dx) > 8 || std::abs(dy) > 8)
                swiped_ = true;
            return;
        }
        // LVGL can still emit CLICKED after a press-locked custom drag. Even a
        // short/rejected page gesture must not activate the button underneath.
        // Native rollers and horizontal sliders keep their own value handling.
        if (!blocked_ && (std::abs(dx) > 8 || std::abs(dy) > 8))
            swiped_ = true;
        if (control_drag_ && control_) {
            lv_obj_set_y(control_, std::clamp(control_start_y_ + dy, -360, 0));
            return;
        }
        if (!blocked_ && page_ == Page::Standby && !control_visible_ && dy < -8 &&
            std::abs(dy) > std::abs(dx)) {
            swiped_ = true;
            return;
        }
        if (date_drag_ && std::abs(dx) > 8) {
            swiped_ = true;
            if (calendar_days_)
                lv_obj_set_x(calendar_days_, std::clamp(date_drag_start_x_ + dx, -369, 369));
            return;
        }
        if (alarm_drag_row_ && std::abs(dx) > 8 && std::abs(dx) > std::abs(dy) * 1.4) {
            alarm_drag_ = true;
            swiped_ = true;
            lv_obj_set_x(alarm_drag_row_, std::clamp(alarm_drag_start_x_ + dx, -62, 0));
            return;
        }
        if ((!blocked_ || control_visible_) && !date_drag_ && std::abs(dy) > 8 &&
            std::abs(dy) > std::abs(dx) * 1.2 &&
            ((!control_visible_ && press_.y <= 115 && dy > 0) || (control_visible_ && dy < 0))) {
            if (!control_visible_)
                ToggleControl(true);
            control_drag_ = true;
            swiped_ = true;
            lv_obj_set_y(control_, std::clamp(control_start_y_ + dy, -360, 0));
            return;
        }
        if (!blocked_ && !date_drag_ && !control_visible_ && page_ != Page::Standby &&
            page_ != Page::Network && dx > 60 && dx > std::abs(dy) * 1.5) {
            swiped_ = true;
            lv_indev_wait_release(indev);
            GoBack();
            return;
        }
    }
    if (code == LV_EVENT_RELEASED) {
        // A swipe can start on the row's nested switch. Finish at the shared
        // gesture owner so both the row and its descendants settle identically.
        if (alarm_drag_ && alarm_drag_row_) {
            auto* row = alarm_drag_row_;
            lv_obj_set_x(row, lv_obj_get_x(row) < -26 ? -62 : 0);
            alarm_drag_ = false;
            alarm_drag_row_ = nullptr;
            return;
        }
        if (control_drag_ && control_) {
            int y = lv_obj_get_y(control_);
            bool open = control_start_y_ == -360 ? y > -267 : y > -94;
            if (open)
                AnimateY(control_, y, 0, 340);
            else
                ToggleControl(false);
            control_drag_ = false;
            return;
        }
        if (date_drag_ && swiped_) {
            int shift = calendar_days_ ? lv_obj_get_x(calendar_days_) : dx;
            int delta = std::clamp(static_cast<int>(std::round(-shift / 41.0)), -9, 9);
            if (delta) page_scroll_ = nullptr;
            selected_date_offset_ += delta;
            Render();
            if (calendar_days_) {
                lv_anim_t snap;
                lv_anim_init(&snap);
                lv_anim_set_var(&snap, calendar_days_);
                lv_anim_set_values(&snap, shift + delta * 41, 0);
                lv_anim_set_duration(&snap, 200);
                lv_anim_set_path_cb(&snap, lv_anim_path_ease_out);
                lv_anim_set_exec_cb(
                    &snap, [](void* p, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t*>(p), x); });
                lv_anim_start(&snap);
            }
            return;
        }
        if (!blocked_ && page_ == Page::Standby && !control_visible_) {
            if (dy < -50 && std::abs(dy) > std::abs(dx)) {
                swiped_ = true;
                Navigate(Page::Menu);
            } else if (std::abs(dx) > 30 && std::abs(dx) > std::abs(dy)) {
                swiped_ = true;
                NextWallpaper(dx < 0 ? 1 : -1);
            }
        } else if (!blocked_ && page_ == Page::Chat && !control_visible_ && dy > 65 &&
                   dy > std::abs(dx)) {
            swiped_ = true;
            GoBack();
        }
    }
}

void WatchUi::Tick() {
    if (!root_)
        return;
    bool done = false, ok = false;
    bool same_navigation = false;
    Page dest = page_;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(async_->mutex);
        if (async_->done) {
            done = true;
            ok = async_->ok;
            dest = async_->destination;
            text = async_->text;
            same_navigation = async_->navigation_generation == navigation_generation_;
            async_->busy = async_->done = false;
        }
    }
    if (done && completion_timer_) lv_timer_pause(completion_timer_);
    auto old = snapshot_;
    snapshot_ = services_.Snapshot();
    power_save_ = snapshot_.settings.power_save;
    bool rebuilt = false;
    if (done) {
        if (ok) {
            if (same_navigation && dest != page_ && page_ == Page::AlarmEditor) {
                Navigate(dest);
                rebuilt = true;
            } else if (same_navigation && page_ != Page::Network) {
                Render();
                rebuilt = true;
            }
            Emit(Action::SettingsChanged);
        }
        if (!text.empty())
            SetSystemMessage(text.c_str());
    }
    // A setting can finish saving after the user has already left its page.
    // Refresh the current page without undoing that navigation.
    // Clock synchronization and midnight do not change the saved-data
    // revision. Refresh the calendar when its displayed day becomes stale.
    const bool calendar_day_changed = page_ == Page::Calendar &&
        (old.time_valid != snapshot_.time_valid ||
         (snapshot_.time_valid && old.now / 86400 != snapshot_.now / 86400));
    if (!rebuilt && (calendar_day_changed || (snapshot_.revision != revision_ &&
        (page_ == Page::Calendar || page_ == Page::Alarms || page_ == Page::Settings ||
         page_ == Page::DisplaySettings ||
         (page_ == Page::Standby && old.settings.show_clock != snapshot_.settings.show_clock))))) {
        if (calendar_day_changed) page_scroll_ = nullptr;
        Render();
        rebuilt = true;
    }
    if (!rebuilt && page_ == Page::Countdown && (old.countdown.active != snapshot_.countdown.active ||
                                     old.countdown.paused != snapshot_.countdown.paused))
        Render();
    RefreshReminder();
    if (!awake_ || power_overlay_)
        return;
    RefreshDynamic();
    LoadWallpapers();
    uint32_t now = lv_tick_get();
    if (toast_ && static_cast<int32_t>(now - toast_deadline_) >= 0) {
        lv_obj_delete(toast_);
        toast_ = nullptr;
    }
    if (page_ == Page::Standby && !control_visible_ && !modal_ && !reminder_ &&
        now - wallpaper_tick_ >= 3000)
        NextWallpaper(1);
}

#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "application.h"
#include "assets/lang_config.h"
#include "ui/watch_resources.h"

using namespace baji::ui;

void WatchUi::ShowChat(bool start) {
    if (power_overlay_)
        return;
    if (page_ != Page::Chat) {
        chat_return_ = page_ == Page::Standby ? Page::Standby : Page::Menu;
        Navigate(Page::Chat);
    }
    // Runtime also calls this on listening/speaking transitions. The existing
    // page receives those changes through SetStatus; keep its video buffers
    // and subtitle objects instead of tearing them down for every transition.
    if (start)
        Emit(Action::StartChat);
}

void WatchUi::RenderChat() {
    wallpaper_obj_ = lv_image_create(content_);
    lv_obj_set_size(wallpaper_obj_, 360, 360);
    WatchResources::AttachVideo(wallpaper_obj_,
                                Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking);
    UpdateAnimationTimer();
    lv_obj_remove_flag(wallpaper_obj_, LV_OBJ_FLAG_CLICKABLE);
    if (!WatchResources::VideoIncludesShading()) {
        Fade(content_, 0, 0, 360, 100, kBg, false, false);
        Fade(content_, 0, 220, 360, 140, kBg, false, true);
    }
    Back(content_);
    // The native status line shows the clock while idle and conversation
    // status in the same position after wake-up. No separate status surface.
    chat_header_ = Text(content_, "", 100, 56, 160, 14, 0xffffff, 400, true, 22);
    SingleLine(chat_header_);

    auto* countdown = Box(content_, 106, 120, 148, 26, 0x102a27, 13, 180);
    Border(countdown, kTeal, 65);
    chat_timer_dot_ = Box(countdown, 12, 11, 4, 4, kTeal, 2);
    chat_timer_ = Text(countdown, "", 25, 4, 62, 12, kGreen, 400, false, 18);
    Text(countdown, snapshot_.settings.language ? "left" : "后提醒", 93, 4, 44, 12, kGreen, 400, false, 18);

    // BAJI uses a neutral gray capsule at 40% opacity. Keep this board's
    // native 14px, three-line subtitles within that lighter surface.
    auto* user = Box(content_, 62, 240, 236, 38, 0x6b7280, LV_RADIUS_CIRCLE, 102);
    Border(user, kRose, 40);
    auto* user_view = Box(user, 13, 7, 208, 22, 0, 0, 0);
    user_label_ = Text(user_view, user_text_.c_str(), 0, 0, 208, 14, 0xfce7f3, 400, true, 22);
    auto* answer = Box(content_, 62, 240, 236, 38, 0x6b7280, LV_RADIUS_CIRCLE, 102);
    Border(answer, 0xffffff, 15);
    auto* answer_view = Box(answer, 13, 7, 208, 22, 0, 0, 0);
    answer_label_ = Text(answer_view, answer_text_.c_str(), 2, 0, 204, 14, 0xffffff, 400, true, 22);
    answer_cursor_ = Box(answer_view, 1, 5, 2, 12, kRose, 1);
    thinking_ = Box(content_, 145, 240, 70, 32, 0x6b7280, LV_RADIUS_CIRCLE, 102);
    Border(thinking_, 0xffffff, 15);
    for (int i = 0; i < 3; ++i) {
        thinking_dots_[i] = Box(thinking_, 0, 0, 5, 5, kRose, LV_RADIUS_CIRCLE);
        lv_obj_align(thinking_dots_[i], LV_ALIGN_CENTER, (i - 1) * 11, 0);
    }
    chat_mic_ = Button(content_, 148, 294, 64, 40, 0x34212f, 20,
                        [this] { Emit(device_.chat_active ? Action::StopChat : Action::StartChat); });
    RefreshMicrophone();
}

void WatchUi::SetStatus(const char* s) {
    status_text_ = s ? s : "";
    RefreshMicrophone();
    RefreshDynamic();
    UpdateAnimationTimer();
}

void WatchUi::RefreshMicrophone() {
    if (!chat_mic_) return;
    lv_obj_clean(chat_mic_);
    voice_bars_.fill(nullptr);
    const bool listening = status_text_.find("聆听") != std::string::npos ||
                           status_text_.find("Listening") != std::string::npos;
    const uint32_t color = listening ? 0x573047 : 0x34212f;
    lv_obj_set_style_bg_color(chat_mic_, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(chat_mic_, lv_color_hex(BlendColor(color, 0xffffff, .12f)), LV_STATE_PRESSED);
    Border(chat_mic_, kRose, listening ? 150 : 65);
    if (listening) {
        for (int i = 0; i < 12; ++i) {
            voice_bars_[i] = Box(chat_mic_, 14 + i * 3, 17, 2, 6, kRose, 1);
            lv_obj_align(voice_bars_[i], LV_ALIGN_LEFT_MID, 13 + i * 3, 0);
        }
    } else
        CenteredIcon(chat_mic_, device_.chat_active ? "pause" : "mic", 20, kRose);
}

void WatchUi::SetChatMessage(const char* role, const char* text) {
    if (!role || !text)
        return;
    if (!std::strcmp(role, "user")) {
        user_text_ = text;
        answer_text_.clear();
        waiting_for_answer_ = true;
        user_tick_ = lv_tick_get();
        answer_shown_ = 0;
    } else if (!std::strcmp(role, "assistant")) {
        if (answer_text_ != text) {
            // Preserve visible characters when streamed content extends the same sentence.
            if (std::string(text).compare(0, answer_text_.size(), answer_text_) != 0)
                answer_shown_ = 0;
            answer_text_ = text;
            answer_tick_ = lv_tick_get();
        }
        user_text_.clear();
        waiting_for_answer_ = false;
    } else if (!std::strcmp(role, "system")) {
        SetSystemMessage(text);
        return;
    }
    RefreshDynamic();
    UpdateAnimationTimer();
}

void WatchUi::ClearChatMessages() {
    user_text_.clear();
    answer_text_.clear();
    answer_shown_ = 0;
    waiting_for_answer_ = false;
    RefreshDynamic();
}

void WatchUi::SetEmotion(const char* s) {
    emotion_ = s ? s : "";
}

void WatchUi::RefreshChat(const std::string& time) {
    const bool thinking =
        (waiting_for_answer_ && lv_tick_get() - user_tick_ >= 2800 && answer_text_.empty()) ||
        status_text_.find("思考") != std::string::npos || status_text_.find("Thinking") != std::string::npos;
    if (chat_header_) {
        // SetStatus runs on native state changes, before the next board tick.
        // Read that state directly so an old snapshot cannot restore the clock
        // over a new listening/speaking status (or leave a stale status at idle).
        const auto state = Application::GetInstance().GetDeviceState();
        WatchResources::SetVideoSpeaking(state == kDeviceStateSpeaking);
        const bool idle = state == kDeviceStateIdle;
        const char* header = idle ? time.c_str() : status_text_.c_str();
        if (!idle && (status_text_.empty() || status_text_ == Lang::Strings::STANDBY)) {
            // The state can change just before its SetStatus event arrives.
            if (state == kDeviceStateConnecting) header = Lang::Strings::CONNECTING;
            else if (state == kDeviceStateListening) header = Lang::Strings::LISTENING;
            else if (state == kDeviceStateSpeaking || state == kDeviceStateNotifying)
                header = Lang::Strings::SPEAKING;
        }
        // DOT rewrites the label's tail. Cache the original text so long status
        // messages are not restored and laid out on every animation frame.
        if (chat_header_text_ != header) {
            SetLabelText(chat_header_, header);
            chat_header_text_ = header;
        }
        const lv_opa_t opacity = idle ? 191 : LV_OPA_COVER;
        if (lv_obj_get_style_text_opa(chat_header_, LV_PART_MAIN) != opacity)
            lv_obj_set_style_text_opa(chat_header_, opacity, 0);
    }
    if (thinking_) {
        if (thinking) lv_obj_remove_flag(thinking_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(thinking_, LV_OBJ_FLAG_HIDDEN);
    }
    auto subtitle = [this](lv_obj_t* label, const std::string& text, bool visible, bool answer) {
        if (!label) return;
        auto* viewport = lv_obj_get_parent(label);
        auto* panel = lv_obj_get_parent(viewport);
        if (!visible) { lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN); return; }
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        if (text != lv_label_get_text(label)) SetLabelText(label, text.c_str());
        lv_obj_update_layout(label);
        const int height = lv_obj_get_height(label);
        const int visible_height = std::min(height, 66);
        const int hidden_height = std::max(0, height - visible_height);
        lv_obj_set_size(panel, 236, std::max(38, visible_height + 16));
        lv_obj_set_y(panel, 278 - std::max(38, visible_height + 16));
        lv_obj_set_size(viewport, 208, visible_height);
        lv_obj_set_y(label, -hidden_height);
        if (answer && answer_cursor_) {
            lv_point_t cursor;
            lv_label_get_letter_pos(label, CharacterCount(text), &cursor);
            lv_obj_set_pos(answer_cursor_, lv_obj_get_x(label) + cursor.x + 1, cursor.y + 5 - hidden_height);
        }
    };
    subtitle(user_label_, user_text_, !user_text_.empty() && !thinking, false);
    subtitle(answer_label_, answer_text_.substr(0, answer_shown_), !answer_text_.empty() && !thinking, true);
    if (chat_timer_) {
        auto* capsule = lv_obj_get_parent(chat_timer_);
        if (snapshot_.countdown.active) {
            const auto seconds = snapshot_.countdown.remaining_seconds;
            const auto time = CountdownText(seconds, snapshot_.countdown.duration_seconds >= 3600);
            if (time != lv_label_get_text(chat_timer_)) lv_label_set_text(chat_timer_, time.c_str());
            lv_obj_remove_flag(capsule, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(capsule, LV_OBJ_FLAG_HIDDEN);
    }
}

void WatchUi::AnimateChat(uint32_t now) {
    constexpr float heights[] = {3.5f, 7, 11, 7.5f, 13, 6, 10, 8.5f, 5, 12, 7, 9.5f};
    for (int i = 0; i < 12; ++i)
        if (voice_bars_[i]) {
            float phase = (static_cast<float>(now) - i * 28) / (250 + i * 30) * 6.283185f;
            int h = std::max(2, static_cast<int>(heights[i] * (.35f + .325f * (1 - std::cos(phase)))));
            lv_obj_set_height(voice_bars_[i], h);
        }
    for (int i = 0; i < 3; ++i)
        if (thinking_dots_[i]) {
            float a =
                .25f + .375f * (1 - std::cos((static_cast<float>(now) - i * 190) * 6.283185f / 800));
            lv_obj_set_style_opa(thinking_dots_[i], static_cast<int>(a * 255), 0);
        }
    bool text_changed = false;
    while (answer_shown_ < answer_text_.size() && now - answer_tick_ >= 46) {
        answer_shown_ = NextCharacter(answer_text_, answer_shown_);
        answer_tick_ += 46;
        text_changed = true;
    }
    if (text_changed || (waiting_for_answer_ && thinking_))
        RefreshDynamic();
    if (answer_cursor_) {
        if (answer_shown_ < answer_text_.size()) {
            lv_obj_remove_flag(answer_cursor_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(answer_cursor_, now % 800 < 400 ? 255 : 0, 0);
        } else
            lv_obj_add_flag(answer_cursor_, LV_OBJ_FLAG_HIDDEN);
    }
}

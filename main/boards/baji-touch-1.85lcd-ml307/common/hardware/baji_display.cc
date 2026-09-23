#include "baji_display.h"

#include "assets/lang_config.h"
#include "assets.h"
#include "config.h"
#include "lvgl_theme.h"
#include "system_info.h"

#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <algorithm>
#include <cstring>
#include <utility>

extern "C" esp_err_t __real_lvgl_port_init(const lvgl_port_cfg_t* cfg);

extern "C" esp_err_t __wrap_lvgl_port_init(const lvgl_port_cfg_t* cfg) {
    // The watch rebuilds nested pages from LVGL input callbacks. Reserve room
    // for that event/layout stack without changing the native display class.
    auto watch_cfg = *cfg;
    watch_cfg.task_stack = std::max(watch_cfg.task_stack, 16 * 1024);
    ESP_LOGI("BajiDisplay", "Watch LVGL task stack: %d bytes", watch_cfg.task_stack);
    return __real_lvgl_port_init(&watch_cfg);
}

BajiDisplay::BajiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y,
                         bool mirror_x, bool mirror_y, bool swap_xy, bool quiet_boot)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy), quiet_boot_(quiet_boot) {
    if (quiet_boot) {
        DisplayLockGuard lock(this);
        auto screen = lv_display_get_screen_active(display_);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    }
}

BajiDisplay::~BajiDisplay() {
    DisplayLockGuard lock(this);
    watch_ui_.reset();
    if (touch_indev_) lvgl_port_remove_touch(touch_indev_);
    if (touch_) esp_lcd_touch_del(touch_);
    if (touch_io_) esp_lcd_panel_io_del(touch_io_);
}

void BajiDisplay::InitializeTouch(i2c_master_bus_handle_t i2c_bus) {
    DisplayLockGuard lock(this);
    if (touch_indev_) return;

    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
    io_config.scl_speed_hz = 100000;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &touch_io_));
    const esp_lcd_touch_config_t touch_config = {
        .x_max = DISPLAY_WIDTH - 1,
        .y_max = DISPLAY_HEIGHT - 1,
        // The shared reset was already pulsed before LCD initialization.
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(touch_io_, &touch_config, &touch_));
    ESP_ERROR_CHECK(gpio_set_pull_mode(TOUCH_INT_GPIO, GPIO_PULLUP_ONLY));
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display_,
        .handle = touch_,
    };
    touch_indev_ = lvgl_port_add_touch(&touch_cfg);
    ESP_ERROR_CHECK(touch_indev_ ? ESP_OK : ESP_FAIL);
    lv_indev_add_event_cb(touch_indev_, [](lv_event_t* event) {
        static_cast<BajiDisplay*>(lv_event_get_user_data(event))->OnTouchPressed();
    }, LV_EVENT_PRESSED, this);
    ESP_LOGI("BajiDisplay", "CST836U touch registered: I2C 0x15, INT GPIO%d", TOUCH_INT_GPIO);
}

void BajiDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (quiet_boot_ || setup_ui_called_) return;

    // The native application applies assets after networking. A watch must
    // display its Chinese menus offline too. Reuse the already packaged font,
    // without starting the audio models before AudioService::Initialize().
    void* asset_data = nullptr;
    size_t asset_size = 0;
    auto& assets = Assets::GetInstance();
    if (assets.GetAssetData("index.json", asset_data, asset_size)) {
        auto index = cJSON_ParseWithLength(static_cast<const char*>(asset_data), asset_size);
        auto font = cJSON_GetObjectItem(index, "text_font");
        if (cJSON_IsString(font) && assets.GetAssetData(font->valuestring, asset_data, asset_size)) {
            auto text_font = std::make_shared<LvglCBinFont>(asset_data);
            if (text_font->font() && SetTextFont(text_font))
                ESP_LOGI("BajiDisplay", "Loaded native font for offline watch UI");
        }
        cJSON_Delete(index);
    }

    // Native theme/font updates still reference these objects. Keep them alive,
    // but let the board's watch root own the visible screen and touch handling.
    SpiLcdDisplay::SetupUI();
    auto screen = lv_display_get_screen_active(display_);
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_add_flag(lv_obj_get_child(screen, i), LV_OBJ_FLAG_HIDDEN);
    }
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    // Native scrolling labels remain hidden and do not need animation timers.
    if (status_label_) lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    if (chat_message_label_) lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_CLIP);

    auto theme = static_cast<LvglTheme*>(current_theme_);
    watch_ui_ = std::make_unique<WatchUi>(display_, theme->text_font()->font(),
        theme->large_icon_font()->font(), WatchServices::GetInstance(),
        [this](WatchUi::Action action, int value) {
            if (watch_action_) watch_action_(action, value);
        });
    watch_ui_->Create();
    watch_ui_->SetWifiCallbacks(watch_wifi_scan_, watch_wifi_connect_);
    watch_ui_->SetAwake(watch_awake_);
}

void BajiDisplay::SetTheme(Theme* theme) {
    if (!theme) return;
    DisplayLockGuard lock(this);
    if (setup_ui_called_) {
        SpiLcdDisplay::SetTheme(theme);
    } else {
        Display::SetTheme(theme);
    }
    if (watch_ui_) {
        auto lvgl_theme = static_cast<LvglTheme*>(theme);
        // SetTextFont retains the previous font owner until this override returns.
        watch_ui_->SetFonts(lvgl_theme->text_font()->font(), lvgl_theme->large_icon_font()->font());
    }
    RefreshChargingStyle();
}

void BajiDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetStatus(status ? status : "");
}

void BajiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!watch_ui_) return;
    if (role && std::strcmp(role, "system") == 0) {
        if (content && SystemInfo::GetUserAgent() == content) return;
        // Activation codes, provisioning instructions and errors must also be
        // visible outside the conversation page. An empty native idle message
        // must not dismiss a notification that is still being read.
        if (content && content[0]) watch_ui_->SetSystemMessage(content, 15000);
    } else {
        watch_ui_->SetChatMessage(role ? role : "assistant", content ? content : "");
    }
}

void BajiDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (!watch_ui_) return;
    watch_ui_->ClearChatMessages();
}

void BajiDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    // The watch owns its conversation animation. Keep the native GIF stopped.
    if (watch_ui_) watch_ui_->SetEmotion(emotion ? emotion : "neutral");
}

void BajiDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void BajiDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (watch_ui_ && notification && notification[0]) {
        watch_ui_->SetSystemMessage(notification, std::max(duration_ms, 0));
    }
}

void BajiDisplay::UpdateStatusBar(bool update_all) {
    DisplayLockGuard lock(this);
    // The board supplies a cached DeviceSnapshot to TickWatch. In particular,
    // do not query the cellular modem again from the native display clock tick.
    // Charging-only startup also reaches this method without Board/Application.
    (void)update_all;
}

void BajiDisplay::SetWatchActionCallback(WatchUi::ActionCallback callback) {
    DisplayLockGuard lock(this);
    watch_action_ = std::move(callback);
}

void BajiDisplay::SetWatchAwake(bool awake) {
    DisplayLockGuard lock(this);
    if (watch_awake_ != awake && touch_indev_) lv_indev_wait_release(touch_indev_);
    watch_awake_ = awake;
    if (watch_ui_) watch_ui_->SetAwake(awake);
}

void BajiDisplay::TickWatch(const WatchUi::DeviceSnapshot& snapshot) {
    DisplayLockGuard lock(this);
    if (!watch_ui_) return;
    watch_ui_->UpdateDeviceSnapshot(snapshot);
    watch_ui_->Tick();
}

void BajiDisplay::ShowWatchChat() {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->ShowChat(false);
}

void BajiDisplay::WatchBack() {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->GoBack();
}

bool BajiDisplay::IsWatchChat() {
    DisplayLockGuard lock(this);
    return watch_ui_ && watch_ui_->IsChat();
}

void BajiDisplay::ShowPowerMenu(bool show) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->ShowPowerMenu(show);
}

bool BajiDisplay::IsPowerMenuOpen() {
    DisplayLockGuard lock(this);
    return watch_ui_ && watch_ui_->IsPowerMenuOpen();
}

void BajiDisplay::ShowPowerTransition(bool reboot) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->ShowPowerTransition(reboot);
}

void BajiDisplay::SetWatchWifiCallbacks(std::function<void()> scan,
    std::function<void(std::string, std::string)> connect) {
    DisplayLockGuard lock(this);
    watch_wifi_scan_ = std::move(scan);
    watch_wifi_connect_ = std::move(connect);
    if (watch_ui_) watch_ui_->SetWifiCallbacks(watch_wifi_scan_, watch_wifi_connect_);
}

void BajiDisplay::SetWatchWifiNetworks(const std::vector<WatchUi::WifiNetwork>& networks,
    bool scanning, const char* error) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetWifiNetworks(networks, scanning, error ? error : "");
}

void BajiDisplay::SetWatchWifiConnectionState(const char* state, const char* error) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetWifiConnectionState(state ? state : "", error ? error : "");
}

void BajiDisplay::OnTouchPressed() {
    DisplayLockGuard lock(this);
    if (quiet_boot_) return;
    if (!watch_awake_) {
        // Sleeping touch never reaches an object or emits an activity action.
        // Keep suppressing until release, including a power-key wake mid-touch.
        lv_indev_wait_release(touch_indev_);
        lv_indev_stop_processing(touch_indev_);
        return;
    }
    if (watch_action_) watch_action_(WatchUi::Action::Activity, 0);
}

void BajiDisplay::RefreshChargingStyle() {
    if (!charging_fullscreen_) return;
    const lv_font_t* text_font = LV_FONT_DEFAULT;
    const lv_font_t* icon_font = LV_FONT_DEFAULT;
    if (auto theme = static_cast<LvglTheme*>(current_theme_)) {
        text_font = theme->text_font()->font();
        icon_font = theme->large_icon_font()->font();
    }
    // Rebind on theme changes before a replaced runtime font is released.
    lv_obj_set_style_text_font(charging_fullscreen_, text_font, 0);
    lv_obj_set_style_text_font(charging_icon_, icon_font, 0);
    lv_obj_align(charging_icon_, LV_ALIGN_CENTER, 0, -lv_font_get_line_height(text_font));
    lv_obj_align_to(charging_caption_, charging_icon_, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}

void BajiDisplay::ShowChargingFullscreen(bool show) {
    DisplayLockGuard lock(this);
    if (!show) {
        if (charging_fullscreen_) lv_obj_add_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!charging_fullscreen_) {
        auto screen = lv_display_get_screen_active(display_);
        charging_fullscreen_ = lv_obj_create(screen);
        lv_obj_set_size(charging_fullscreen_, width_, height_);
        lv_obj_set_style_radius(charging_fullscreen_, 0, 0);
        lv_obj_set_style_bg_color(charging_fullscreen_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(charging_fullscreen_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(charging_fullscreen_, lv_color_white(), 0);
        lv_obj_set_style_border_width(charging_fullscreen_, 0, 0);
        lv_obj_set_style_pad_all(charging_fullscreen_, 0, 0);
        lv_obj_center(charging_fullscreen_);
        lv_obj_remove_flag(charging_fullscreen_, LV_OBJ_FLAG_SCROLLABLE);

        charging_icon_ = lv_label_create(charging_fullscreen_);
        lv_label_set_text(charging_icon_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT);
        charging_caption_ = lv_label_create(charging_fullscreen_);
        lv_label_set_text(charging_caption_, Lang::Strings::BATTERY_CHARGING);
        lv_obj_set_width(charging_caption_, width_ * 85 / 100);
        lv_label_set_long_mode(charging_caption_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(charging_caption_, LV_TEXT_ALIGN_CENTER, 0);
    }
    RefreshChargingStyle();
    lv_obj_remove_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(charging_fullscreen_);
}

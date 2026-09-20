#include "baji_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "../config.h"
#include "lvgl_theme.h"

#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <algorithm>
#include <cstring>

extern "C" {
LV_IMAGE_DECLARE(baji185_wifi_weak);
LV_IMAGE_DECLARE(baji185_wifi_fair);
LV_IMAGE_DECLARE(baji185_wifi_strong);
LV_IMAGE_DECLARE(baji185_wifi_slash);
LV_IMAGE_DECLARE(baji185_bat_0);
LV_IMAGE_DECLARE(baji185_bat_1);
LV_IMAGE_DECLARE(baji185_bat_2);
LV_IMAGE_DECLARE(baji185_bat_3);
LV_IMAGE_DECLARE(baji185_bat_4);
LV_IMAGE_DECLARE(baji185_bat_charge);
}

namespace {

bool IconEquals(const char* icon, const char* expected) {
    return icon != nullptr && std::strcmp(icon, expected) == 0;
}

bool IsCellularIcon(const char* icon) {
    return IconEquals(icon, MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF) ||
           IconEquals(icon, MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT_1_BAR) ||
           IconEquals(icon, MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT_2_BAR) ||
           IconEquals(icon, MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT) ||
           IconEquals(icon, MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR);
}

void ConfigureRow(lv_obj_t* row, int gap) {
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
}

void AddSpacer(lv_obj_t* parent) {
    auto spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
}

}  // namespace

BajiDisplay::BajiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y,
                         bool mirror_x, bool mirror_y, bool swap_xy, bool quiet_boot)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
    if (quiet_boot) {
        DisplayLockGuard lock(this);
        auto screen = lv_display_get_screen_active(display_);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    }
}

BajiDisplay::~BajiDisplay() {
    if (touch_indev_) lvgl_port_remove_touch(touch_indev_);
    if (touch_) esp_lcd_touch_del(touch_);
    if (touch_io_) esp_lcd_panel_io_del(touch_io_);
}

void BajiDisplay::InitializeTouch(i2c_master_bus_handle_t i2c_bus) {
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
    ESP_LOGI("BajiDisplay", "CST836U touch registered: I2C 0x15, INT GPIO%d", TOUCH_INT_GPIO);
}

void BajiDisplay::SetupUI() {
    if (setup_ui_called_) return;
    SpiLcdDisplay::SetupUI();
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    DisplayLockGuard lock(this);
    auto theme = static_cast<LvglTheme*>(current_theme_);
    auto old_right_icons = lv_obj_get_parent(mute_label_);
    lv_obj_set_style_pad_column(top_bar_, theme->spacing(3), 0);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE);

    AddSpacer(top_bar_);
    center_status_row_ = lv_obj_create(top_bar_);
    ConfigureRow(center_status_row_, theme->spacing(4));
    lv_obj_set_style_min_width(center_status_row_, 88, 0);
    network_image_ = lv_image_create(center_status_row_);
    lv_image_set_src(network_image_, &baji185_wifi_slash);
    lv_obj_set_parent(network_label_, center_status_row_);
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    auto battery_row = lv_obj_create(center_status_row_);
    ConfigureRow(battery_row, theme->spacing(2));
    lv_obj_set_width(battery_row, LV_SIZE_CONTENT);
    battery_image_ = lv_image_create(battery_row);
    lv_image_set_src(battery_image_, &baji185_bat_2);
    battery_percent_label_ = lv_label_create(battery_row);
    lv_label_set_text(battery_percent_label_, "--%");

    AddSpacer(top_bar_);
    lv_obj_set_parent(mute_label_, top_bar_);
    // Native SetTheme and UpdateStatusBar retain ownership of these labels.
    lv_obj_set_parent(battery_label_, top_bar_);
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete(old_right_icons);
    RefreshBoardLayout();
#endif
}

void BajiDisplay::SetTheme(Theme* theme) {
    if (!theme) return;
    if (setup_ui_called_) {
        SpiLcdDisplay::SetTheme(theme);
    } else {
        Display::SetTheme(theme);
    }
    DisplayLockGuard lock(this);
    RefreshBoardLayout();
    RefreshChargingStyle();
}

void BajiDisplay::RefreshBoardLayout() {
    if (!center_status_row_) return;
    auto theme = static_cast<LvglTheme*>(current_theme_);
    // Inherit the screen's current text font instead of retaining a resource pointer.
    lv_obj_set_style_text_color(battery_percent_label_, theme->text_color(), 0);
    lv_obj_set_style_translate_y(bottom_bar_,
        -2 * lv_font_get_line_height(theme->text_font()->font()), 0);
    lv_obj_update_layout(top_bar_);
    lv_obj_align_to(status_bar_, top_bar_, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(top_bar_);
    if (charging_fullscreen_ && !lv_obj_has_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(charging_fullscreen_);
    }
}

void BajiDisplay::UpdateStatusBar(bool update_all) {
    // The charging-only path must not construct Board or Application.
    if (!setup_ui_called_) return;
    bool low_battery_was_visible = false;
    if (center_status_row_ && low_battery_popup_ && !update_all) {
        DisplayLockGuard lock(this);
        low_battery_was_visible = !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    }
    SpiLcdDisplay::UpdateStatusBar(update_all);
    if (!center_status_row_) return;

    auto& board = Board::GetInstance();
    int battery_level = 0;
    bool charging = false, discharging = false;
    const bool has_battery = board.GetBatteryLevel(battery_level, charging, discharging);
    const char* network = nullptr;
    if (Application::GetInstance().GetDeviceState() != kDeviceStateUpgrading) {
        // Refresh promptly when changing Wi-Fi/4G, independently of the native timer.
        network = board.GetNetworkStateIcon();
    }

    DisplayLockGuard lock(this);
    if (has_battery) {
        battery_level = std::clamp(battery_level, 0, 100);
        static const lv_image_dsc_t* levels[] = {
            &baji185_bat_0, &baji185_bat_1, &baji185_bat_2, &baji185_bat_3, &baji185_bat_4,
        };
        lv_image_set_src(battery_image_, charging ? &baji185_bat_charge
            : levels[std::min(battery_level / 20, 4)]);
        lv_label_set_text_fmt(battery_percent_label_, "%d%%", battery_level);
        if (low_battery_popup_ && !update_all) {
            if (battery_level < 20 && discharging) {
                // Native code may hide the popup at 1-19%; preserve BAJI's
                // threshold without replaying its alert on every status tick.
                if (!low_battery_was_visible &&
                    lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    Application::GetInstance().Schedule([]() {
                        Application::GetInstance().PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                    });
                }
                lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (network) {
        network_icon_ = network;
        if (IsCellularIcon(network)) {
            lv_label_set_text(network_label_, network);
            lv_obj_remove_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(network_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            const lv_image_dsc_t* image = &baji185_wifi_strong;
            if (IconEquals(network, MATERIAL_SYMBOLS_WIFI_OFF)) image = &baji185_wifi_slash;
            else if (IconEquals(network, MATERIAL_SYMBOLS_WIFI_1_BAR)) image = &baji185_wifi_weak;
            else if (IconEquals(network, MATERIAL_SYMBOLS_WIFI_2_BAR)) image = &baji185_wifi_fair;
            lv_image_set_src(network_image_, image);
            lv_obj_remove_flag(network_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    RefreshBoardLayout();
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

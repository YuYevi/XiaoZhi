#pragma once

#include "display/lcd_display.h"
#include <driver/i2c_master.h>
#include <esp_lcd_touch.h>

// Reuse XiaoZhi's renderer, with only BAJI status layout and charging additions.
class BajiDisplay : public SpiLcdDisplay {
public:
    BajiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                int width, int height, int offset_x, int offset_y,
                bool mirror_x, bool mirror_y, bool swap_xy, bool quiet_boot = false);
    ~BajiDisplay() override;
    void InitializeTouch(i2c_master_bus_handle_t i2c_bus);
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ShowChargingFullscreen(bool show);

private:
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;

    lv_obj_t* center_status_row_ = nullptr;
    lv_obj_t* network_image_ = nullptr;
    lv_obj_t* battery_image_ = nullptr;
    lv_obj_t* battery_percent_label_ = nullptr;
    lv_obj_t* charging_fullscreen_ = nullptr;
    lv_obj_t* charging_icon_ = nullptr;
    lv_obj_t* charging_caption_ = nullptr;

    void RefreshBoardLayout();
    void RefreshChargingStyle();
};

// Shared hardware setup for normal startup and the charging-only entry.
BajiDisplay* baji_185_create_lcd_display(bool quiet_boot = false);

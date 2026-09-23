#pragma once

#include "display/lcd_display.h"
#include "ui/watch_ui.h"
#include <driver/i2c_master.h>
#include <esp_lcd_touch.h>

// Keep the native display/font lifetime, with the watch UI owned by this board.
class BajiDisplay : public SpiLcdDisplay {
public:
    BajiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                int width, int height, int offset_x, int offset_y,
                bool mirror_x, bool mirror_y, bool swap_xy, bool quiet_boot = false);
    ~BajiDisplay() override;
    void InitializeTouch(i2c_master_bus_handle_t i2c_bus);
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmotion(const char* emotion) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ShowChargingFullscreen(bool show);

    void SetWatchActionCallback(WatchUi::ActionCallback callback);
    void SetWatchAwake(bool awake);
    void TickWatch(const WatchUi::DeviceSnapshot& snapshot);
    void ShowWatchChat();
    void WatchBack();
    bool IsWatchChat();
    void ShowPowerMenu(bool show);
    bool IsPowerMenuOpen();
    void ShowPowerTransition(bool reboot);
    void SetWatchWifiCallbacks(std::function<void()> scan,
        std::function<void(std::string, std::string)> connect);
    void SetWatchWifiNetworks(const std::vector<WatchUi::WifiNetwork>& networks,
        bool scanning, const char* error = "");
    void SetWatchWifiConnectionState(const char* state, const char* error = "");

private:
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;

    const bool quiet_boot_;
    bool watch_awake_ = true;
    std::unique_ptr<WatchUi> watch_ui_;
    WatchUi::ActionCallback watch_action_;
    std::function<void()> watch_wifi_scan_;
    std::function<void(std::string, std::string)> watch_wifi_connect_;
    lv_obj_t* charging_fullscreen_ = nullptr;
    lv_obj_t* charging_icon_ = nullptr;
    lv_obj_t* charging_caption_ = nullptr;

    void RefreshChargingStyle();
    void OnTouchPressed();
};

// Shared hardware setup for normal startup and the charging-only entry.
BajiDisplay* baji_185_create_lcd_display(bool quiet_boot = false);

#pragma once
#include <lvgl.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "watch/watch_services.h"
// Owns navigation, page objects, overlays and asynchronous results.
// Page implementations live in ui/pages; shared widgets live in watch_ui_widgets.cc.
// All UI entry points require the display lock. Storage runs on Application.
class WatchUi {
   public:
    enum class Page {
        Standby,
        Menu,
        Chat,
        Calendar,
        Alarms,
        Countdown,
        Settings,
        DisplaySettings,
        AlarmEditor,
        Network,
        Lightstick
    };
    enum class Action {
        Activity,
        StartChat,
        StopChat,
        SetBrightness,
        SetVolume,
        SetNetwork,
        StartWifiConfig,
        Sleep,
        PowerOff,
        Reboot,
        CheckUpdate,
        FactoryReset,
        SettingsChanged,
        SetPowerSave,
        SetFlashlight,
        SetAlwaysOn,
        SetLanguage
    };
    struct DeviceSnapshot {
        std::string network;
        std::string wifi_ssid;
        bool wifi_connected = false;
        int battery = -1, volume = 50, brightness = 50;
        bool charging = false, chat_active = false;
        std::string firmware;
    };
    struct WifiNetwork {
        std::string ssid;
        int rssi = -100;
        bool secure = true, connected = false;
    };
    using ActionCallback = std::function<void(Action, int)>;
    WatchUi(lv_display_t*, const lv_font_t*, const lv_font_t*, WatchServices&, ActionCallback);
    ~WatchUi();
    void Create();
    void SetFonts(const lv_font_t*, const lv_font_t*);
    void UpdateDeviceSnapshot(const DeviceSnapshot&);
    void SetStatus(const char*);
    void SetChatMessage(const char* role, const char* text);
    void ClearChatMessages();
    void SetEmotion(const char*);
    void SetSystemMessage(const char*, uint32_t duration_ms = 5000);
    void SetAwake(bool);
    void ShowPowerMenu(bool show);
    bool IsPowerMenuOpen() const { return power_overlay_ && !power_transition_; }
    void ShowPowerTransition(bool reboot);
    void ShowChat(bool start = false);
    void GoBack();
    void Tick();
    void SetWifiCallbacks(std::function<void()> scan,
                          std::function<void(const std::string&, const std::string&)> connect);
    void SetWifiNetworks(const std::vector<WifiNetwork>&, bool scanning, const char* error = nullptr);
    void SetWifiConnectionState(const char* state, const char* error = nullptr);
    Page GetPage() const {
        return page_;
    }
    bool IsChat() const {
        return page_ == Page::Chat;
    }

   private:
    struct AsyncState;
    lv_display_t* display_;
    const lv_font_t *fallback_font_, *fallback_icon_;
    WatchServices& services_;
    ActionCallback action_;
    std::shared_ptr<AsyncState> async_;
    WatchSnapshot snapshot_;
    DeviceSnapshot device_;
    Page page_ = Page::Standby, chat_return_ = Page::Standby, menu_destination_ = Page::Menu;
    lv_obj_t *root_ = nullptr, *content_ = nullptr, *control_ = nullptr, *modal_ = nullptr,
             *reminder_ = nullptr, *toast_ = nullptr, *status_ = nullptr;
    lv_obj_t* page_scroll_ = nullptr;
    lv_obj_t *power_overlay_ = nullptr, *power_loader_ = nullptr;
    lv_obj_t *clock_ = nullptr, *date_ = nullptr, *user_label_ = nullptr, *answer_label_ = nullptr,
             *chat_header_ = nullptr, *chat_mic_ = nullptr, *chat_timer_ = nullptr;
    lv_obj_t *wallpaper_obj_ = nullptr, *hour_ = nullptr, *minute_ = nullptr, *timer_label_ = nullptr,
             *timer_progress_ = nullptr, *brightness_ = nullptr, *volume_ = nullptr,
             *brightness_value_ = nullptr, *volume_value_ = nullptr;
    lv_obj_t *calendar_strip_ = nullptr, *alarm_drag_row_ = nullptr;
    lv_obj_t *calendar_days_ = nullptr, *clock_minute_ = nullptr, *clock_colon_ = nullptr,
             *chat_timer_dot_ = nullptr, *settings_wifi_label_ = nullptr;
    lv_obj_t *wifi_loader_ = nullptr, *thinking_ = nullptr, *answer_cursor_ = nullptr;
    lv_obj_t *wifi_input_ = nullptr, *wifi_password_label_ = nullptr, *wifi_hint_ = nullptr,
             *wifi_eye_icon_ = nullptr, *wifi_join_ = nullptr, *wifi_join_icon_ = nullptr;
    lv_timer_t* animation_timer_ = nullptr;
    lv_timer_t* completion_timer_ = nullptr;
    lv_timer_t* menu_navigation_timer_ = nullptr;
    std::array<lv_obj_t*, 5> menu_plates_{};
    size_t menu_plate_count_ = 0;
    uint32_t menu_input_generation_ = 0;
    std::array<lv_obj_t*, 12> voice_bars_{};
    std::array<lv_obj_t*, 3> thinking_dots_{};
    size_t answer_shown_ = 0;
    uint32_t answer_tick_ = 0, user_tick_ = 0;
    uint32_t navigation_generation_ = 0;
    bool waiting_for_answer_ = false;
    std::array<lv_image_dsc_t, 3> wallpapers_{};
    std::array<std::unique_ptr<uint8_t[]>, 3> wallpaper_data_;
    uint32_t asset_check_ = 0, wallpaper_tick_ = 0, toast_deadline_ = 0, reminder_token_ = 0,
             revision_ = 0, light_color_ = 0x9a91f2;
    int wallpaper_ = 0, remembered_volume_ = 50, remembered_brightness_ = 50;
    bool awake_ = true, blocked_ = false, swiped_ = false, control_visible_ = false, power_save_ = false,
         light_applied_ = false, torch_ = false;
    bool control_drag_ = false, date_drag_ = false, alarm_drag_ = false;
    bool power_transition_ = false;
    int control_start_y_ = 0, selected_date_offset_ = 0, alarm_drag_start_x_ = 0, date_drag_start_x_ = 0;
    lv_point_t press_{};
    WatchAlarm alarm_draft_;
    std::string status_text_, chat_header_text_, user_text_, answer_text_, emotion_;
    std::function<void()> wifi_scan_;
    std::function<void(const std::string&, const std::string&)> wifi_connect_;
    std::vector<WifiNetwork> wifi_networks_;
    std::string wifi_phase_ = "list", wifi_error_, wifi_ssid_, wifi_password_;
    bool wifi_scanning_ = false, password_visible_ = false;
    int keyboard_layer_ = 0;
    void Emit(Action, int value = 0);
    void Navigate(Page);
    void Render();
    void RenderStandby();
    void RenderMenu();
    lv_obj_t* MenuCard(int x, int y, int width, int height, uint32_t accent,
                      uint32_t shadow, Page destination);
    void QueueMenuNavigation(Page destination);
    void ResetMenuInteraction();
    void RenderChat();
    void RenderCalendar();
    void RenderAlarms();
    void RenderCountdown();
    void RenderAlarmEditor();
    void RenderSettings();
    void RenderDisplaySettings();
    lv_obj_t* SettingsRow(lv_obj_t* parent, int y, const char* icon, const char* title,
                          uint32_t accent, std::function<void()> action);
    void ShowScreenTimeoutPicker();
    void RenderNetwork();
    void RenderLightstick();
    void RenderStatus();
    void ToggleControl(bool);
    void RefreshDynamic();
    void RefreshClock(const std::string& time);
    void RefreshChat(const std::string& time);
    void RefreshCountdown();
    void AnimateChat(uint32_t now);
    void RefreshMicrophone();
    void Animate();
    void UpdateAnimationTimer();
    void Fade(lv_obj_t*, int x, int y, int width, int height, uint32_t color, bool horizontal,
              bool reverse);
    void RefreshReminder();
    void LoadWallpapers();
    void NextWallpaper(int);
    void Gesture(lv_event_t*);
    void SaveSettings(const WatchSettings&);
    void Submit(std::function<bool(std::string*)>, Page, const char* success = "");
    void Confirm(const char*, const char*, std::function<void()>);
    void CloseModal();
    void DrawTabs(bool);
    void DrawKeyboard();
    void RefreshWifiPassword();
    void ConnectWifi();
    const lv_font_t* Font(float, int weight = 400);
    lv_obj_t* Icon(lv_obj_t*, const char*, int, int, int, uint32_t color = 0xffffff);
    lv_obj_t* CenteredIcon(lv_obj_t*, const char*, int size, uint32_t color = 0xffffff);
    lv_obj_t* Box(lv_obj_t*, int, int, int, int, uint32_t, int radius = 0, lv_opa_t opa = LV_OPA_COVER);
    lv_obj_t* Text(lv_obj_t*, const char*, int, int, int, float size = 12, uint32_t color = 0xffffff,
                   int weight = 400, bool center = false, float line_height = 0, float cjk_size = 12);
    lv_obj_t* Button(lv_obj_t*, int, int, int, int, uint32_t, int, std::function<void()>,
                     const char* text = nullptr, float font_size = 12, int weight = 400);
    lv_obj_t* Roller(lv_obj_t*, int, int, int, int, uint32_t);
    void Back(lv_obj_t*, std::function<void()> callback = {});
    void PageTitle(const char*, uint32_t accent);
    lv_obj_t* ScrollList(lv_obj_t*, int x, int y, int width, int height);
    void EmptyState(lv_obj_t*, const char* icon, const char* title, const char* hint,
                    uint32_t accent, int y = 120);
    void Border(lv_obj_t*, uint32_t, lv_opa_t opa = LV_OPA_COVER, int width = 1);
    void Gradient(lv_obj_t*, uint32_t, uint32_t);
    void Glow(lv_obj_t*, uint32_t, int, lv_opa_t);
    void Bind(lv_obj_t*, std::function<void(lv_event_t*)>);
    void Switch(lv_obj_t*, bool, uint32_t, std::function<void()>);
};

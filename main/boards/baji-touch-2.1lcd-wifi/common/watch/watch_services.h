#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct WatchSettings {
    uint16_t screen_timeout_seconds = 15;  // 0: always on
    uint16_t wallpaper_interval_seconds = 10;  // 0: no rotation
    bool do_not_disturb = false;
    bool show_clock = true;
    bool power_save = false;
    uint8_t language = 0;  // 0: Chinese, 1: English (watch UI only)
    uint8_t alarm_volume = 70;
};

struct WatchAlarm {
    uint32_t id = 0;  // 0 when creating
    uint8_t hour = 7;
    uint8_t minute = 0;
    uint8_t weekdays = 0;  // bit 0 Monday ... bit 6 Sunday; 0: next occurrence only
    bool enabled = true;
    std::string title = "闹钟";
    int64_t next_due = 0;  // local wall-clock epoch, assigned by the service
    int64_t last_fired = 0;  // service-owned occurrence watermark; also excludes time before creation
};

struct WatchEvent {
    uint32_t id = 0;
    int64_t due = 0;  // same local wall-clock epoch as the native server time
    std::string title;
    bool completed = false;
    bool notified = false;
};

enum class WatchReminderKind { Alarm, Event, Countdown };

struct WatchReminder {
    uint32_t token = 0;
    WatchReminderKind kind = WatchReminderKind::Alarm;
    uint32_t source_id = 0;
    std::string title;
    bool audible = true;  // calendar events honor DND; alarms/countdowns do not
};

struct WatchCountdown {
    bool active = false;
    bool paused = false;
    uint32_t remaining_seconds = 0;
    uint32_t duration_seconds = 0;
};

struct WatchSnapshot {
    WatchSettings settings;
    std::vector<WatchAlarm> alarms;
    std::vector<WatchEvent> events;
    std::vector<WatchReminder> reminders;
    WatchCountdown countdown;
    bool time_valid = false;
    int64_t now = 0;
    uint32_t revision = 0;
    std::string storage_error;
};

// All methods are task-safe. Persistence methods may block: invoke them through
// Application::Schedule() from LVGL callbacks, never from an ISR/esp_timer.
// The native OTA clock already includes timezone_offset: do not add UTC+8 again.
class WatchServices {
public:
    static constexpr size_t kMaxAlarms = 8;
    static constexpr size_t kMaxEvents = 16;
    static constexpr size_t kMaxTitleBytes = 60;
    static WatchServices& GetInstance();

    bool Initialize(std::string* error = nullptr);
    bool Start(std::string* error = nullptr);
    WatchSnapshot Snapshot() const;
    bool SaveSettings(const WatchSettings& settings, std::string* error = nullptr);
    bool ResetWatchData(std::string* error = nullptr);  // Keeps Wi-Fi and native application settings
    bool SaveAlarm(const WatchAlarm& alarm, uint32_t* assigned_id = nullptr,
                   std::string* error = nullptr);
    bool DeleteAlarm(uint32_t id, std::string* error = nullptr);
    bool SaveEvent(const WatchEvent& event, uint32_t* assigned_id = nullptr,
                   std::string* error = nullptr);
    bool DeleteEvent(uint32_t id, std::string* error = nullptr);
    bool SetLocalTime(int year, int month, int day, int hour, int minute,
                      std::string* error = nullptr);
    bool StartCountdown(uint32_t seconds, std::string* error = nullptr);
    bool PauseCountdown(std::string* error = nullptr);
    bool ResumeCountdown(std::string* error = nullptr);
    void CancelCountdown();
    void DismissReminders();
    bool SnoozeReminders(uint32_t seconds = 300, std::string* error = nullptr);
    void SetReminderCallback(std::function<void(const std::vector<WatchReminder>&)> callback);
    void RegisterMcpTools();

    // Strict date validation, also shared by UI date/time editors.
    static bool MakeLocalTime(int year, int month, int day, int hour, int minute,
                              int64_t* result);
    static bool IsTimeValid(int64_t value);

private:
    WatchServices();
    ~WatchServices();
    WatchServices(const WatchServices&) = delete;
    WatchServices& operator=(const WatchServices&) = delete;
    void Tick();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

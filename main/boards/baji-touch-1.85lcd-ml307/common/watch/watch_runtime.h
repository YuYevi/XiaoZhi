#pragma once

#include "ui/watch_ui.h"
#include "board.h"
#include <atomic>
#include <esp_timer.h>

class BajiDisplay;

// Coordinates the board's screen and reminders on the application task.
// The UI and service layers do not own hardware or another audio pipeline.
class WatchRuntime {
public:
    WatchRuntime(Board& board, BajiDisplay& display);
    void Initialize();
    void Start();
    void Wake();
    bool IsAwake() const { return awake_ && !power_transition_; }
    void PowerTap();
    void PowerLongPress();
    void SetPowerOffAction(std::function<void()> action) { power_off_ = std::move(action); }
    void Back();
    void Chat();
    void OnNetworkEvent(NetworkEvent event);
    void OnPerformanceRequested();
    void SetNetworkActions(std::function<void(int)> change, std::function<void()> configure);
    void SetNetworkEnabled(bool enabled) { network_enabled_ = enabled; }

private:
    Board& board_;
    BajiDisplay& display_;
    WatchServices& services_;
    esp_timer_handle_t timer_ = nullptr;
    esp_timer_handle_t power_tap_timer_ = nullptr;
    std::atomic<bool> tick_pending_{false};
    bool started_ = false;
    bool booting_ = true;
    bool awake_ = true;
    bool sleep_requested_ = false;
    bool power_transition_ = false;
    bool power_reboot_ = false;
    int64_t power_reboot_at_ = 0;
    int64_t power_tap_at_ = 0;
    std::atomic<uint32_t> power_tap_generation_{0};
    std::function<void()> power_off_;
    bool ringing_ = false;
    bool stop_chat_requested_ = false;
    bool chat_start_pending_ = false;
    bool chat_start_submitted_ = false;
    bool chat_stop_submitted_ = false;
    bool chat_abort_submitted_ = false;
    uint32_t chat_request_generation_ = 0;
    bool connected_ = false;
    bool network_enabled_ = true;
    bool charging_ = false;
    bool flashlight_ = false;
    bool applied_power_save_ = false;
    std::atomic<bool> wifi_worker_busy_{false};
    bool wifi_connecting_ = false;
    int64_t wifi_connect_deadline_ = 0;
    std::string wifi_target_;
    std::vector<WatchUi::WifiNetwork> wifi_networks_;
    int64_t last_activity_ = 0;
    int64_t last_ui_tick_ = 0;
    int64_t ring_started_ = 0;
    int64_t next_tone_ = 0;
    uint32_t reminder_token_ = 0;
    bool reminder_timed_out_ = false;
    int last_state_ = -1;
    std::function<void(int)> change_network_;
    std::function<void()> configure_network_;

    void Tick();
    void Sleep();
    void RequestSleep();
    void CancelPowerTap();
    void RequestPowerChange(bool reboot);
    void Action(WatchUi::Action action, int value);
    void StopChat();
    void ContinueStopChat();
    void UpdateReminder(const WatchSnapshot& snapshot, int64_t now);
    void StopRing();
    void ApplyPowerSettings();
    void RequestWifiScan();
    void RequestWifiConnect(std::string ssid, std::string password);
    bool LaunchWifiWorker(std::function<void()> work);
    void UpdateWifiState();
};

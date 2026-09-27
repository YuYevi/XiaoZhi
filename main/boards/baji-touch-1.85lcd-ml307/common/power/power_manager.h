#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include "battery_monitor.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/task.h>

// Shared RTC state for runtime shutdown and the next boot.
extern "C" void charging_rtc_set_usb_shutdown_flag(void);
extern "C" void charging_rtc_clear_boot_flags(void);
extern "C" bool charging_rtc_usb_shutdown_next_boot(void);

enum class PowerUiHint {
    ShuttingDown,
};

enum class PowerKeyEvent { Tap, LongPress };

class PowerManager {
private:
    TaskHandle_t battery_task_ = nullptr;
    esp_timer_handle_t power_timer_handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::unique_ptr<BajiBatteryMonitor> battery_;
    bool usb_present_ = false;
    bool is_low_battery_ = false;

    bool key_raw_pressed_ = false;
    bool key_pressed_ = false;
    bool key_wait_release_ = true;
    bool key_menu_sent_ = false;
    int64_t key_raw_since_ = 0;
    int64_t key_pressed_since_ = 0;
    int64_t shutdown_released_since_ = 0;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> emergency_shutdown_{false};

    std::function<void(PowerUiHint)> on_power_ui_;
    std::function<void(PowerKeyEvent)> on_power_key_;

    void PowrSwitch();
    void PollPowerKey(bool pressed, int64_t now);
    void CheckBatteryStatus();
    static void BatteryTask(void* arg);
    static void ShutdownTask(void* arg);
    void RememberUsbShutdown();
    void BeginEmergencyShutdown();
    void RunShutdownSequence();

public:
    PowerManager(gpio_num_t pin);
    void Start();
    ~PowerManager();
    bool IsCharging();
    bool IsDischarging();
    BajiBatteryMonitor::Snapshot GetBatterySnapshot();
    void SaveBatteryState();
    void OnLowBatteryStatusChanged(std::function<void(bool)> callback);
    void OnChargingStatusChanged(std::function<void(bool)> callback);
    void OnPowerUi(std::function<void(PowerUiHint)> callback);
    void OnPowerKey(std::function<void(PowerKeyEvent)> callback);
    void shutdown();
};

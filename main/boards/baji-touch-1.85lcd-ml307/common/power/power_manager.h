#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <vector>

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>

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
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_timer_handle_t power_timer_handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    uint32_t battery_level_ = 30;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    int ticks_ = 0;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 20;

    bool key_raw_pressed_ = false;
    bool key_pressed_ = false;
    bool key_wait_release_ = true;
    bool key_menu_sent_ = false;
    int64_t key_raw_since_ = 0;
    int64_t key_pressed_since_ = 0;
    int64_t shutdown_released_since_ = 0;
    bool new_charging_status = false;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> emergency_shutdown_{false};

    std::function<void(PowerUiHint)> on_power_ui_;
    std::function<void(PowerKeyEvent)> on_power_key_;

    void PowrSwitch();
    void PollPowerKey(bool pressed, int64_t now);
    void CheckBatteryStatus();
    void ReadBatteryAdcData();
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
    uint8_t GetBatteryLevel();
    void OnLowBatteryStatusChanged(std::function<void(bool)> callback);
    void OnChargingStatusChanged(std::function<void(bool)> callback);
    void OnPowerUi(std::function<void(PowerUiHint)> callback);
    void OnPowerKey(std::function<void(PowerKeyEvent)> callback);
    void shutdown();
};

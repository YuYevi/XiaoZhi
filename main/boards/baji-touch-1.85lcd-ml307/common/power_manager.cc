#include "power_manager.h"
#include "../config.h"
#include "board.h"

#include <utility>
#include <driver/ledc.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define CHARGING_RTC_MAGIC 0x43485247u
#define FLAG_USB_SHUTDOWN_NEXT 0x01u

RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint32_t s_rtc_flags;

extern "C" void charging_rtc_set_usb_shutdown_flag(void)
{
    s_rtc_magic = CHARGING_RTC_MAGIC;
    s_rtc_flags |= FLAG_USB_SHUTDOWN_NEXT;
}

extern "C" void charging_rtc_clear_boot_flags(void)
{
    s_rtc_magic = 0;
    s_rtc_flags = 0;
}

extern "C" bool charging_rtc_usb_shutdown_next_boot(void)
{
    return (s_rtc_magic == CHARGING_RTC_MAGIC) && (s_rtc_flags & FLAG_USB_SHUTDOWN_NEXT);
}

void PowerManager::PowrSwitch() {
    if (emergency_shutdown_) {
        // A failed task allocation must not block the shared ESP timer
        // task while the user still holds the power button.
        if (POWER_KEY_PRESSED()) {
            shutdown_release_debounce_ticks_ = 0;
            return;
        }
        if (++shutdown_release_debounce_ticks_ < POWER_SHUTDOWN_RELEASE_DEBOUNCE_TICKS) {
            return;
        }
        esp_timer_stop(power_timer_handle_);
        const auto err = esp_sleep_enable_ext1_wakeup(1ULL << Power_Dec, ESP_EXT1_WAKEUP_ANY_LOW);
        if (err != ESP_OK) {
            esp_sleep_enable_ext0_wakeup(Power_Dec, 0);
        }
        esp_deep_sleep_start();
    }
    if (shutdown_requested_) {
        return;
    }

    if (POWER_KEY_PRESSED()) {
        shutdown_release_debounce_ticks_ = 0;
        hold_shutdown_ticks_++;
        if (hold_shutdown_ticks_ >= POWER_SHUTDOWN_HOLD_TICKS) {
            if (timer_handle_) {
                esp_timer_stop(timer_handle_);
                esp_timer_delete(timer_handle_);
                timer_handle_ = nullptr;
            }
            shutdown_requested_ = true;
            shutdown();
        }
    } else {
        shutdown_release_debounce_ticks_++;
        if (shutdown_release_debounce_ticks_ >= POWER_SHUTDOWN_RELEASE_DEBOUNCE_TICKS) {
            hold_shutdown_ticks_ = 0;
        }
    }
}

void PowerManager::CheckBatteryStatus() {
#if POWER_CHARGE_DETECT_USE_GPIO
    new_charging_status = (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL);
#else
    int usb_usb_adc_value0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, POWER_USBIN_ADC_CHANNEL, &usb_usb_adc_value0));
    new_charging_status = (1500 < usb_usb_adc_value0 && usb_usb_adc_value0 < 4000);
#endif

    if (new_charging_status != is_charging_) {
        ReadBatteryAdcData();
        is_charging_ = new_charging_status;
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
        return;
    }

    if (adc_values_.size() < kBatteryAdcDataCount) {
        ReadBatteryAdcData();
        return;
    }

    ticks_++;
    if (ticks_ % kBatteryAdcInterval == 0) {
        ReadBatteryAdcData();
    }
}

void PowerManager::ReadBatteryAdcData() {
    int adc_value;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, POWER_BATTERY_ADC_CHANNEL, &adc_value));

    adc_values_.push_back(adc_value);
    if (adc_values_.size() > kBatteryAdcDataCount) {
        adc_values_.erase(adc_values_.begin());
    }
    uint32_t average_adc = 0;
    for (auto value : adc_values_) {
        average_adc += value;
    }
    average_adc /= adc_values_.size();

    const struct {
        uint16_t adc;
        uint8_t level;
    } levels[] = {
        {1970, 0},
        {2062, 20},
        {2154, 40},
        {2246, 60},
        {2338, 80},
        {2430, 100}
    };

    if (average_adc < levels[0].adc) {
        battery_level_ = 0;
    }

    else if (average_adc >= levels[5].adc) {
			battery_level_ = 100;
    } else {

        for (int i = 0; i < 5; i++) {
            if (average_adc >= levels[i].adc && average_adc < levels[i+1].adc) {
                float ratio = static_cast<float>(average_adc - levels[i].adc) / (levels[i+1].adc - levels[i].adc);
                battery_level_ = levels[i].level + ratio * (levels[i+1].level - levels[i].level);
                break;
            }
        }
    }

    if (adc_values_.size() >= kBatteryAdcDataCount) {
        bool new_low_battery_status = battery_level_ <= kLowBatteryLevel;
        if (new_low_battery_status != is_low_battery_) {
            is_low_battery_ = new_low_battery_status;
            if (on_low_battery_status_changed_) {
                on_low_battery_status_changed_(is_low_battery_);
            }
        }
    }

}

void PowerManager::ShutdownTask(void* arg) {
    auto* self = static_cast<PowerManager*>(arg);
    self->RunShutdownSequence();
    vTaskDelete(nullptr);
}

void PowerManager::RememberUsbShutdown() {
#if POWER_CHARGE_DETECT_USE_GPIO
    if (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL) {
#else
    if (new_charging_status) {
#endif
        charging_rtc_set_usb_shutdown_flag();
    }
}

void PowerManager::BeginEmergencyShutdown() {
    // All resources already exist. Avoid Board access, UI locks, task
    // allocation and delays when the shared timer task is the caller.
    emergency_shutdown_ = true;
    shutdown_requested_ = true;
    shutdown_release_debounce_ticks_ = 0;
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
    }
    RememberUsbShutdown();
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
    gpio_set_level(Power_Control, 0);
    if (!esp_timer_is_active(power_timer_handle_)) {
        esp_timer_start_periodic(power_timer_handle_, 200000);
    }
}

void PowerManager::RunShutdownSequence() {

    if (on_power_ui_) {
        on_power_ui_(PowerUiHint::ShuttingDown);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    if (power_timer_handle_) {
        esp_timer_stop(power_timer_handle_);
    }
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
    }

    RememberUsbShutdown();

    if (auto* backlight = Board::GetInstance().GetBacklight()) {
        backlight->SetBrightness(0);
    }

    gpio_config_t wake_in = {};
    wake_in.intr_type = GPIO_INTR_DISABLE;
    wake_in.mode = GPIO_MODE_INPUT;
    wake_in.pin_bit_mask = (1ULL << Power_Dec);
    wake_in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    wake_in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&wake_in);

    gpio_set_level(Power_Control, 0);

    vTaskDelay(pdMS_TO_TICKS(200));

    while (POWER_KEY_PRESSED()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = esp_sleep_enable_ext1_wakeup((1ULL << Power_Dec), ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {

        esp_sleep_enable_ext0_wakeup(Power_Dec, 0);
    }

    esp_deep_sleep_start();
}

PowerManager::PowerManager(gpio_num_t pin) : charging_pin_(pin) {

    gpio_config_t powerdecgpio_conf = {};
    powerdecgpio_conf.intr_type = GPIO_INTR_DISABLE;
    powerdecgpio_conf.mode = GPIO_MODE_INPUT;
    powerdecgpio_conf.pin_bit_mask = (1ULL << Power_Dec);
    powerdecgpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    powerdecgpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&powerdecgpio_conf);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << charging_pin_);
#if POWER_CHARGE_DETECT_USE_GPIO
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#else
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#endif
    gpio_config(&io_conf);

    const bool v5m_present = (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL);
    const esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();

    gpio_config_t powercontgpio_conf = {};
    powercontgpio_conf.intr_type = GPIO_INTR_DISABLE;
    powercontgpio_conf.mode = GPIO_MODE_OUTPUT;
    powercontgpio_conf.pin_bit_mask = (1ULL << Power_Control);
    powercontgpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    powercontgpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&powercontgpio_conf);

    if (esp_reset_reason() == ESP_RST_SW || (v5m_present && POWER_KEY_RELEASED()) || wc == ESP_SLEEP_WAKEUP_EXT0 || wc == ESP_SLEEP_WAKEUP_EXT1) {
        gpio_set_level(Power_Control, 1);

    } else {
        const int poll_ms = 20;
        int elapsed = 0;

        while (elapsed < POWER_KEY_HOLD_MS_TO_BOOT) {
            if (POWER_KEY_RELEASED()) {

                gpio_set_level(Power_Control, 0);
                esp_sleep_enable_ext1_wakeup(1ULL << Power_Dec, ESP_EXT1_WAKEUP_ANY_LOW);
                esp_deep_sleep_start();
            }
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            elapsed += poll_ms;
        }
        gpio_set_level(Power_Control, 1);

    }

    esp_timer_create_args_t power_timer_args = {
        .callback = [](void* arg) {
            PowerManager* self = static_cast<PowerManager*>(arg);
            self->PowrSwitch();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_cotrol_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&power_timer_args, &power_timer_handle_));

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            PowerManager* self = static_cast<PowerManager*>(arg);
            self->CheckBatteryStatus();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_check_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = POWER_CBS_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_BATTERY_ADC_CHANNEL, &chan_config));
#if !POWER_CHARGE_DETECT_USE_GPIO
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_USBIN_ADC_CHANNEL, &chan_config));
#endif
}

void PowerManager::Start() {
    CheckBatteryStatus();
    ESP_ERROR_CHECK(esp_timer_start_periodic(power_timer_handle_, 200000));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
}

PowerManager::~PowerManager() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
    if (power_timer_handle_) {
        esp_timer_stop(power_timer_handle_);
        esp_timer_delete(power_timer_handle_);
    }
    if (adc_handle_ != nullptr) {
        adc_oneshot_del_unit(adc_handle_);
    }
}

bool PowerManager::IsCharging() {

    if (battery_level_ == 100) {
        return false;
    }
    return is_charging_;
}

bool PowerManager::IsDischarging() {

    return !is_charging_;
}

uint8_t PowerManager::GetBatteryLevel() {
    return battery_level_;
}

void PowerManager::OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
    on_low_battery_status_changed_ = callback;
}

void PowerManager::OnChargingStatusChanged(std::function<void(bool)> callback) {
    on_charging_status_changed_ = callback;
}

void PowerManager::OnPowerUi(std::function<void(PowerUiHint)> callback) {
    on_power_ui_ = std::move(callback);
}

void PowerManager::shutdown() {
    if (!shutdown_first_) {
        return;
    }
    shutdown_first_ = false;
    BaseType_t ok = xTaskCreate(ShutdownTask, "pm_shutdown", 4096, this, tskIDLE_PRIORITY + 5, nullptr);
    if (ok != pdPASS) {
        BeginEmergencyShutdown();
    }
}

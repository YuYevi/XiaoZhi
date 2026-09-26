#pragma once

#include "backlight.h"
#include <mutex>

// Native PWM brightness control with a permanent shutdown barrier. Startup
// stays at the native constructor's zero duty until the first frame is ready.
class BajiBacklight final : public PwmBacklight {
public:
    BajiBacklight(gpio_num_t pin, bool output_invert = false)
        : PwmBacklight(pin, output_invert) {}

    void TurnOffImmediately() {
        std::lock_guard<std::mutex> lock(output_mutex_);
        shutdown_prepared_ = true;
        if (transition_timer_ != nullptr) {
            esp_timer_stop(transition_timer_);
        }
        target_brightness_ = 0;
        brightness_ = 0;
        PwmBacklight::SetBrightnessImpl(0);
    }

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        std::lock_guard<std::mutex> lock(output_mutex_);
        PwmBacklight::SetBrightnessImpl(shutdown_prepared_ ? 0 : brightness);
    }

private:
    std::mutex output_mutex_;
    bool shutdown_prepared_ = false;
};

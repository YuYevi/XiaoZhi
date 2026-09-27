#include "battery_monitor.h"
#include "config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <driver/gpio.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>

namespace {
constexpr const char* kTag = "BajiBattery";
// V4 also records whether insertion display protection is still active.
// V3 already has the capacity anchor and can be migrated without discarding it.
constexpr uint32_t kStateVersion = 0x42415404;
struct SavedState {
    uint32_t version;
    int32_t percent_milli, millivolts, usb, correction_mv;
    int32_t full, charging, near_cv_mv;
    int32_t model_milli, anchor_soc_milli, anchor_curve_milli;
    int32_t display_floor_active, display_floor_reference_mv;
};
RTC_DATA_ATTR SavedState rtc_state;

bool ValidState(const SavedState& state) {
    const bool anchor_valid =
        (state.anchor_soc_milli == -1000 && state.anchor_curve_milli == 0) ||
        (state.anchor_soc_milli >= 0 && state.anchor_soc_milli <= 100000 &&
         state.anchor_curve_milli > 0 && state.anchor_curve_milli <= 100000);
    return state.version == kStateVersion && state.percent_milli >= 0 && state.percent_milli <= 100000 &&
        state.model_milli >= 0 && state.model_milli <= 100000 && anchor_valid &&
        state.millivolts >= 2800 && state.millivolts <= 4350 && (state.usb == 0 || state.usb == 1) &&
        state.correction_mv >= -200 && state.correction_mv <= 200 &&
        (state.full == 0 || state.full == 1) && (state.charging == 0 || state.charging == 1) &&
        state.near_cv_mv >= 0 && state.near_cv_mv <= 4350 &&
        ((state.display_floor_active == 0 && state.display_floor_reference_mv == 0) ||
         (state.display_floor_active == 1 && state.display_floor_reference_mv >= 2800 &&
          state.display_floor_reference_mv <= 4350));
}

SavedState MakeState(const BajiBatteryMonitor::Snapshot& state) {
    return {kStateVersion, static_cast<int32_t>(std::lround(state.precise_level * 1000)),
        state.millivolts, state.voltage_usb, state.correction_mv, state.full,
        state.voltage_charging, state.near_cv_mv,
        static_cast<int32_t>(std::lround(state.model_percent * 1000)),
        static_cast<int32_t>(std::lround(state.anchor_soc * 1000)),
        static_cast<int32_t>(std::lround(state.anchor_curve * 1000)),
        state.display_floor_active ? 1 : 0, state.display_floor_reference_mv};
}

constexpr uint8_t kTraceUsb = 1 << 0;
constexpr uint8_t kTraceCharging = 1 << 1;
constexpr uint8_t kTraceFull = 1 << 2;
constexpr uint8_t kTraceValid = 1 << 3;
constexpr uint8_t kTraceCableEdge = 1 << 4;
constexpr uint8_t kTraceVoltageUsb = 1 << 5;
}

BajiBatteryMonitor::BajiBatteryMonitor() {
    gpio_config_t gpio = {};
    gpio.pin_bit_mask = (1ULL << POWER_USB_IN);
    gpio.mode = GPIO_MODE_INPUT;
    gpio.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&gpio));
    gpio.pin_bit_mask = (1ULL << POWER_CHARGE_STATUS_PIN);
    gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&gpio));
    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id = POWER_CBS_ADC_UNIT;
    const auto created = adc_oneshot_new_unit(&unit, &adc_);
    if (created != ESP_OK) {
        ESP_LOGE(kTag, "ADC unavailable: %s; battery remains unknown", esp_err_to_name(created));
        return;
    }
    adc_oneshot_chan_cfg_t channel = {};
    channel.atten = ADC_ATTEN_DB_12;
    channel.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_, POWER_BATTERY_ADC_CHANNEL, &channel));
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = POWER_CBS_ADC_UNIT;
    cal.chan = POWER_BATTERY_ADC_CHANNEL;
    cal.atten = channel.atten;
    cal.bitwidth = channel.bitwidth;
    const auto calibrated = adc_cali_create_scheme_curve_fitting(&cal, &calibration_);
    if (calibrated != ESP_OK) {
        ESP_LOGE(kTag, "ADC calibration unavailable: %s; no raw-to-percent fallback", esp_err_to_name(calibrated));
    } else {
        ESP_LOGI(kTag, "Calibrated ADC GPIO4, R8/R13=200k/200k, charge STAT GPIO16");
    }
    RestoreState();
}

BajiBatteryMonitor::~BajiBatteryMonitor() {
    if (calibration_) adc_cali_delete_scheme_curve_fitting(calibration_);
    if (adc_) adc_oneshot_del_unit(adc_);
}

void BajiBatteryMonitor::RestoreState() {
    SavedState state = {};
    const auto reset = esp_reset_reason();
    const bool trusted = reset == ESP_RST_SW && ValidState(rtc_state);
    if ((trusted || reset == ESP_RST_DEEPSLEEP) && ValidState(rtc_state)) {
        state = rtc_state;
    } else {
        nvs_handle_t nvs = 0;
        if (nvs_open("baji_battery", NVS_READONLY, &nvs) == ESP_OK) {
            size_t size = sizeof(state);
            const auto read = nvs_get_blob(nvs, "state", &state, &size);
            if (read == ESP_OK && size == offsetof(SavedState, display_floor_active) &&
                state.version == 0x42415403) {
                state.version = kStateVersion;
                state.display_floor_active = state.display_floor_reference_mv = 0;
            } else if (read != ESP_OK || size != sizeof(state)) {
                state = {};
            }
            nvs_close(nvs);
        }
    }
    if (ValidState(state)) {
        estimator_.Restore(state.percent_milli / 1000.0f, state.millivolts, state.usb,
            state.correction_mv, trusted, state.full, state.charging, state.near_cv_mv,
            state.model_milli / 1000.0f, state.display_floor_active != 0,
            state.display_floor_reference_mv);
        estimator_.RestoreDischargeAnchor({state.anchor_soc_milli / 1000.0f,
            state.anchor_curve_milli / 1000.0f});
    }
}

bool BajiBatteryMonitor::ReadMillivolts(int& mv) {
    if (!adc_ || !calibration_) return false;
    std::array<int, 16> samples;
    // Discard the first conversion after the high-impedance divider is selected.
    int raw = 0;
    adc_oneshot_read(adc_, POWER_BATTERY_ADC_CHANNEL, &raw);
    for (auto& sample : samples) {
        int adc_mv = 0;
        const auto read = adc_oneshot_read(adc_, POWER_BATTERY_ADC_CHANNEL, &raw);
        if (read != ESP_OK || adc_cali_raw_to_voltage(calibration_, raw, &adc_mv) != ESP_OK) {
            if (++errors_ == 1 || errors_ % 12 == 0) ESP_LOGW(kTag, "ADC read failed (%u); retaining last valid sample", errors_);
            return false;
        }
        sample = adc_mv * POWER_BATTERY_DIVIDER_NUM / POWER_BATTERY_DIVIDER_DEN;
    }
    std::sort(samples.begin(), samples.end());
    int total = 0;
    for (size_t i = 4; i < 12; ++i) total += samples[i];
    mv = total / 8;
    if (mv < 2800 || mv > 4350) {
        if (++errors_ == 1 || errors_ % 12 == 0) ESP_LOGW(kTag, "Implausible VBAT=%dmV; check divider/battery", mv);
        return false;
    }
    errors_ = 0;
    return true;
}

void BajiBatteryMonitor::Publish(int64_t now_ms) {
    Snapshot next;
    next.valid = estimator_.Valid(now_ms);
    next.level = next.valid ? estimator_.Percent() : -1;
    next.precise_level = next.valid ? estimator_.PrecisePercent() : -1;
    next.model_percent = next.valid ? estimator_.TargetPercent() : -1;
    const auto anchor = estimator_.GetDischargeAnchor();
    if (anchor.Valid()) {
        next.anchor_soc = anchor.soc;
        next.anchor_curve = anchor.curve;
    }
    next.millivolts = estimator_.Millivolts();
    next.correction_mv = estimator_.CorrectionMillivolts();
    next.usb = usb_;
    next.charging = charging_;
    next.full = estimator_.Full();
    next.voltage_usb = estimator_.VoltageWasOnUsb();
    next.voltage_charging = estimator_.VoltageWasCharging();
    next.supply_low = estimator_.SupplyLow();
    next.display_floor_active = estimator_.DisplayFloorActive();
    next.display_floor_reference_mv = estimator_.DisplayFloorReferenceMillivolts();
    next.near_cv_mv = estimator_.RecentNearCvMillivolts();
    const SavedState saved = next.valid ? MakeState(next) : SavedState{};
    portENTER_CRITICAL(&snapshot_mux_);
    snapshot_ = next;
    if (next.valid) rtc_state = saved;
    portEXIT_CRITICAL(&snapshot_mux_);
}

BajiBatteryMonitor::Snapshot BajiBatteryMonitor::GetSnapshot() {
    portENTER_CRITICAL(&snapshot_mux_);
    const auto result = snapshot_;
    portEXIT_CRITICAL(&snapshot_mux_);
    return result;
}

void BajiBatteryMonitor::RecordTrace(int64_t now_ms, bool cable_edge) {
    const auto state = GetSnapshot();
    const uint8_t flags = (state.usb ? kTraceUsb : 0) |
        (state.charging ? kTraceCharging : 0) | (state.full ? kTraceFull : 0) |
        (state.valid ? kTraceValid : 0) | (cable_edge ? kTraceCableEdge : 0) |
        (state.voltage_usb ? kTraceVoltageUsb : 0);
    trace_[trace_next_] = {now_ms,
        static_cast<int32_t>(std::lround(state.precise_level * 1000)),
        static_cast<int32_t>(std::lround(state.model_percent * 1000)),
        static_cast<uint16_t>(state.millivolts), flags};
    trace_next_ = (trace_next_ + 1) % kTraceCapacity;
    trace_count_ = std::min(trace_count_ + 1, kTraceCapacity);
    if (!state.usb) trace_has_discharge_ = true;
}

void BajiBatteryMonitor::DumpDischargeTrace() {
    if (!trace_has_discharge_) return;
    ESP_LOGI(kTag, "Offline discharge trace: up to 64 minute/USB-edge samples; uptime timestamps");
    const unsigned first = (trace_next_ + kTraceCapacity - trace_count_) % kTraceCapacity;
    for (unsigned i = 0; i < trace_count_; ++i) {
        const auto& sample = trace_[(first + i) % kTraceCapacity];
        if ((sample.flags & kTraceUsb) && !(sample.flags & kTraceCableEdge)) continue;
        // CONFIG_LIBC_NEWLIB_NANO_FORMAT does not support %lld. Format the
        // two parts separately to retain millisecond uptime beyond 49 days.
        const auto upper_ms = static_cast<unsigned>(sample.time_ms / 1000000000LL);
        const auto lower_ms = static_cast<unsigned>(sample.time_ms % 1000000000LL);
        char timestamp[24];
        if (upper_ms) snprintf(timestamp, sizeof(timestamp), "%u%09u", upper_ms, lower_ms);
        else snprintf(timestamp, sizeof(timestamp), "%u", lower_ms);
        ESP_LOGI(kTag, "Offline t=%sms VBAT=%umV SOC=%.3f%% target=%.3f%% USB=%d active_charge=%d full=%d valid=%d edge=%d voltage_usb=%d",
            timestamp,
            static_cast<unsigned>(sample.millivolts),
            sample.soc_milli / 1000.0, sample.target_milli / 1000.0,
            !!(sample.flags & kTraceUsb), !!(sample.flags & kTraceCharging),
            !!(sample.flags & kTraceFull), !!(sample.flags & kTraceValid),
            !!(sample.flags & kTraceCableEdge), !!(sample.flags & kTraceVoltageUsb));
    }
    ESP_LOGI(kTag, "Offline discharge trace complete");
    // Retain the samples: USB enumeration or a late host monitor can miss a
    // whole replay. Poll retries for ten minutes without writing to flash.
}

void BajiBatteryMonitor::Poll() {
    const int64_t now = esp_timer_get_time() / 1000;
    const int usb_gpio = gpio_get_level(POWER_USB_IN);
    const int stat_gpio = gpio_get_level(POWER_CHARGE_STATUS_PIN);
    const bool usb = usb_input_.Update(usb_gpio == POWER_USB_VBUS_ACTIVE_LEVEL, now);
    const bool stat_active = charge_input_.Update(stat_gpio == POWER_CHARGE_STATUS_ACTIVE_LEVEL, now);
    const bool charging = usb && stat_active;
    const bool cable_changed = usb != usb_;
    if (usb != usb_ || charging != charging_) {
        ESP_LOGI(kTag, "Power changed: usb=%d active_charge=%d; preserving SOC", usb, charging);
    }
    usb_ = usb;
    charging_ = charging;
    estimator_.SetPower(usb_, charging_, now);
    // Keep post-insertion readings out of the pre-insertion voltage baseline
    // while the GPIO candidate has not yet passed its debounce window.
    if (!usb_input_.Pending() && !charge_input_.Pending() && now - sample_at_ms_ >= 5000) {
        sample_at_ms_ = now;
        int mv = 0;
        if (ReadMillivolts(mv)) estimator_.Observe(mv, now);
    }
    Publish(now);
    if (cable_changed) {
        if (!usb_) {
            // Start a new discharge record; plugged-in minute samples must
            // not evict it while waiting for the host to collect the replay.
            trace_next_ = trace_count_ = 0;
            trace_has_discharge_ = false;
        }
        RecordTrace(now, true);
        // USB serial needs time to enumerate and the host monitor to reconnect.
        // Retry replay while plugged in, even if no host was reading initially.
        trace_dump_at_ms_ = usb_ && trace_has_discharge_ ? now + 10000 : -1;
        trace_replay_until_ms_ = usb_ && trace_has_discharge_ ? now + 610000 : -1;
        // Save the settled anchor as well as an immediate full-state change.
        // Otherwise an unexpected reset within ten minutes could reuse a
        // checkpoint from the opposite side of the cable transition.
        checkpoint_at_ms_ = now + 35000;
    }
    if (trace_dump_at_ms_ >= 0 && now >= trace_dump_at_ms_ && usb_) {
        DumpDischargeTrace();
        trace_dump_at_ms_ = now + 60000 <= trace_replay_until_ms_ ? now + 60000 : -1;
    }
    const bool full_changed = estimator_.Full() != last_full_;
    const bool display_floor_changed = estimator_.DisplayFloorActive() != last_display_floor_active_;
    if (display_floor_changed) {
        last_display_floor_active_ = estimator_.DisplayFloorActive();
        ESP_LOGI(kTag, "Insertion display hold=%d reference=%dmV SOC=%.3f%% target=%.3f%%",
            last_display_floor_active_, estimator_.DisplayFloorReferenceMillivolts(),
            estimator_.PrecisePercent(), estimator_.TargetPercent());
    }
    if (full_changed) {
        last_full_ = estimator_.Full();
        ESP_LOGI(kTag, "Charge cycle: full=%d inferred=%d supply_low=%d VBAT=%dmV",
            last_full_, estimator_.FullInferred(), estimator_.SupplyLow(), estimator_.Millivolts());
        if (estimator_.SupplyLow()) {
            ESP_LOGW(kTag, "Voltage below recharge range despite USB; verify input supply and charger");
        }
    }
    if (now - log_at_ms_ >= 60000) {
        log_at_ms_ = now;
        if (!usb_ && !cable_changed) RecordTrace(now, false);
        const auto state = GetSnapshot();
        ESP_LOGI(kTag, "VBAT=%dmV correction=%dmV target=%.3f%% SOC=%.3f%% valid=%d USB=%d active_charge=%d full=%d supply_low=%d gpio_usb=%d gpio_stat=%d anchor_soc=%.3f%% anchor_curve=%.3f%% display_hold=%d hold_reference=%dmV",
            state.millivolts, state.correction_mv, state.model_percent, state.precise_level, state.valid,
            state.usb, state.charging, state.full, state.supply_low, usb_gpio, stat_gpio,
            state.anchor_soc, state.anchor_curve, state.display_floor_active, state.display_floor_reference_mv);
    }
    // NVS writes run in the battery task, never in the shared ESP timer task.
    const bool checkpoint_due = checkpoint_at_ms_ >= 0 && now >= checkpoint_at_ms_;
    if (full_changed || display_floor_changed || checkpoint_due || now - save_at_ms_ >= 600000) {
        if (checkpoint_due) checkpoint_at_ms_ = -1;
        save_at_ms_ = now;
        SaveState();
    }
}

void BajiBatteryMonitor::SaveState() {
    const auto current = GetSnapshot();
    if (!current.valid) return;
    const SavedState state = MakeState(current);
    nvs_handle_t nvs = 0;
    auto err = nvs_open("baji_battery", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        SavedState old = {};
        size_t size = sizeof(old);
        const auto read = nvs_get_blob(nvs, "state", &old, &size);
        if (read != ESP_OK || size != sizeof(old) || !ValidState(old) ||
            old.percent_milli != state.percent_milli || old.usb != state.usb ||
            old.full != state.full || old.charging != state.charging ||
            old.model_milli != state.model_milli || old.anchor_soc_milli != state.anchor_soc_milli ||
            old.anchor_curve_milli != state.anchor_curve_milli || old.near_cv_mv != state.near_cv_mv ||
            old.display_floor_active != state.display_floor_active ||
            old.display_floor_reference_mv != state.display_floor_reference_mv ||
            std::abs(old.millivolts - state.millivolts) >= 30 ||
            std::abs(old.correction_mv - state.correction_mv) >= 20) {
            err = nvs_set_blob(nvs, "state", &state, sizeof(state));
            if (err == ESP_OK) err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) ESP_LOGW(kTag, "Battery checkpoint not saved: %s", esp_err_to_name(err));
}

#pragma once

#include "battery_estimator.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <freertos/FreeRTOS.h>

// Single writer: normal-mode battery task, or the charging-only loop.
class BajiBatteryMonitor {
public:
    struct Snapshot {
        int level = -1;
        int millivolts = 0;
        int sample_millivolts = 0;
        int64_t sample_at_ms = -1;
        bool sample_valid = false;
        int correction_mv = 0;
        float precise_level = -1;
        float model_percent = -1;
        float anchor_soc = -1;
        float anchor_curve = 0;
        bool valid = false;
        bool usb = false;
        bool charging = false;
        bool full = false;
        bool voltage_usb = false;
        bool voltage_charging = false;
        bool supply_low = false;
        bool display_floor_active = false;
        int display_floor_reference_mv = 0;
        int near_cv_mv = 0;
    };

    BajiBatteryMonitor();
    ~BajiBatteryMonitor();
    void Poll();
    Snapshot GetSnapshot();
    void SaveState();

private:
    bool ReadMillivolts(int& mv);
    void Publish(int64_t now_ms);
    void RestoreState();
    void RecordTrace(int64_t now_ms, bool cable_edge);
    void DumpDischargeTrace();
    struct TraceSample {
        int64_t time_ms;
        int32_t soc_milli, target_milli;
        uint16_t millivolts;
        uint8_t flags;
    };
    static constexpr unsigned kTraceCapacity = 64;
    static_assert(sizeof(TraceSample) * kTraceCapacity < 2048);
    TraceSample trace_[kTraceCapacity] = {};
    unsigned trace_next_ = 0, trace_count_ = 0;
    bool trace_has_discharge_ = false;
    int64_t trace_dump_at_ms_ = -1;
    int64_t trace_replay_until_ms_ = -1;
    int64_t checkpoint_at_ms_ = -1;
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    baji::BatteryEstimator estimator_;
    // VBUS is a clean power-present signal on this board and only needs a
    // short hardware/glitch filter. STAT remains deliberately conservative:
    // its transition participates in charge-completion inference.
    baji::StableInput usb_input_{150}, charge_input_;
    portMUX_TYPE snapshot_mux_ = portMUX_INITIALIZER_UNLOCKED;
    Snapshot snapshot_;
    bool usb_ = false, charging_ = false;
    bool last_full_ = false;
    bool last_display_floor_active_ = false;
    int64_t sample_at_ms_ = -5000, estimator_sample_at_ms_ = -5000;
    int64_t log_at_ms_ = -60000, save_at_ms_ = 0;
    int sample_millivolts_ = 0;
    int64_t last_valid_sample_at_ms_ = -1;
    unsigned errors_ = 0;
};

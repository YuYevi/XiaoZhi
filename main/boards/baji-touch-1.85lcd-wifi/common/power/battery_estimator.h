#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace baji {

// A debounced input can be exercised independently of GPIO and the scheduler.
class StableInput {
public:
    explicit StableInput(int64_t debounce_ms = 2000) : debounce_ms_(debounce_ms) {}

    bool Update(bool raw, int64_t now_ms) {
        if (!initialized_) {
            initialized_ = true;
            candidate_ = value_ = raw;
            since_ms_ = now_ms;
        } else if (raw != candidate_) {
            candidate_ = raw;
            since_ms_ = now_ms;
        } else if (now_ms - since_ms_ >= debounce_ms_) {
            value_ = candidate_;
        }
        return value_;
    }
    bool Pending() const { return candidate_ != value_; }
private:
    bool initialized_ = false, candidate_ = false, value_ = false;
    int64_t since_ms_ = 0;
    int64_t debounce_ms_ = 2000;
};

class BatteryEstimator {
public:
    struct DischargeAnchor {
        float soc = -1;
        float curve = 0;
        bool Valid() const {
            return std::isfinite(soc) && std::isfinite(curve) &&
                soc >= 0 && soc <= 100 && curve > 0 && curve <= 100;
        }
    };
    // Reference Li-ion OCV curve: ESP-IDF adc_battery_estimation model 2.
    // Endpoints follow the supplied 703048 specification (3.0 V / 4.2 V).
    // This is a voltage estimate, not a characterized 703048 capacity curve.
    static float VoltagePercent(float mv) {
        constexpr int voltage[] = {3000, 3675, 3687, 3710, 3741, 3762,
            3775, 3784, 3793, 3806, 3821, 3841, 3884, 3918, 3945,
            3975, 4008, 4045, 4086, 4129, 4200};
        if (mv <= voltage[0]) return 0;
        for (int i = 1; i < 21; ++i) {
            if (mv < voltage[i]) {
                return (i - 1) * 5.0f + 5.0f * (mv - voltage[i - 1]) /
                    (voltage[i] - voltage[i - 1]);
            }
        }
        return 100;
    }

    void Restore(float percent, int mv, bool usb, float correction_mv = 0, bool trusted = false,
                 bool full = false, bool charging = true, int near_cv_mv = 0, float model_percent = -1,
                 bool display_floor_active = false, int display_floor_reference_mv = 0) {
        if (percent >= 0 && percent <= 100 && mv >= 2800 && mv <= 4350) {
            saved_percent_ = percent;
            saved_mv_ = mv;
            saved_usb_ = usb;
            saved_correction_mv_ = correction_mv;
            saved_trusted_ = trusted;
            saved_full_ = full;
            saved_charging_ = charging;
            saved_near_cv_mv_ = near_cv_mv;
            saved_model_percent_ = model_percent >= 0 && model_percent <= 100 ? model_percent : percent;
            saved_display_floor_active_ = display_floor_active;
            saved_display_floor_reference_mv_ = display_floor_reference_mv;
        }
    }

    void RestoreDischargeAnchor(DischargeAnchor anchor) { saved_anchor_ = anchor; }

    void SetPower(bool usb, bool charging, int64_t now_ms) {
        charging = usb && charging;
        const bool cable_changed = power_initialized_ && usb != usb_;
        const bool phase_changed = power_initialized_ && usb && usb_ && charging != charging_;
        if (power_initialized_ && (cable_changed || phase_changed) && !transition_pending_) {
            // Preserve a falling model rather than a lagging display; do not
            // credit a rising target before the capacity estimate catches up.
            // Full charge is a 100% anchor even when its voltage is <4.2 V.
            transition_soc_ = full_ ? 100.0f : std::min(TargetPercent(), percent_);
            transition_mv_ = VoltageAtPercent(transition_soc_);
            transition_voltage_mv_ = filtered_mv_;
        }
        if (cable_changed && !usb) {
            // A confirmed full cycle may be unplugged before the next ADC
            // sample. Keep that display state eligible for a quick replug;
            // the normal USB cap still protects unfinished charges at 99%.
            full_display_hold_ = full_ || full_display_hold_;
            ClearDisplayFloor();
        } else if (cable_changed && usb && initialized_) {
            BeginDisplayFloor();
        }
        if (!power_initialized_) session_inactive_ = usb && !charging;
        if (cable_changed) {
            full_ = false;
            supply_low_ = false;
            session_inactive_ = usb && !charging;
            full_inferred_ = false;
            near_full_at_ms_ = -1;
            near_full_since_ms_ = -1;
            charge_peak_mv_ = 0;
            low_supply_since_ms_ = -1;
        }
        if (power_initialized_ && (cable_changed || phase_changed)) {
            // Preserve capacity across the I*R / polarization step. Do not
            // mix voltage samples taken on opposite sides of a cable change.
            settling_until_ms_ = now_ms + 30000;
            transition_pending_ = initialized_;
            filter_reset_ = true;
        }
        if (phase_changed && !charging) {
            // A near-full top-up can terminate after the charger's 16 s EOC
            // blanking time, before the regular 30 s CV observation completes.
            if (initialized_ && last_valid_ms_ >= 0 && now_ms - last_valid_ms_ <= 10000 &&
                filtered_mv_ >= kNearCvMv) {
                near_full_at_ms_ = now_ms;
                charge_peak_mv_ = std::max(charge_peak_mv_, filtered_mv_);
            }
            // A stopped charger no longer contributes the learned charging
            // voltage rise. Do not deduct it from the relaxed battery voltage.
            charge_step_mv_ = 0;
            step_mv_ = std::min(step_mv_, 0.0f);
            transition_mv_ = std::max(transition_mv_, filtered_mv_);
        }
        if (charging) session_inactive_ = false;
        if (!power_initialized_ || usb != usb_ || charging != charging_) full_since_ms_ = -1;
        power_initialized_ = true;
        usb_ = usb;
        charging_ = charging;
        if (!usb_) full_ = false;
    }

    bool Observe(int mv, int64_t now_ms) {
        if (mv < 2800 || mv > 4350) return false;
        if (last_valid_ms_ >= 0 && now_ms - last_valid_ms_ > 15000) {
            full_since_ms_ = near_full_since_ms_ = low_supply_since_ms_ = -1;
            display_floor_drop_since_ms_ = critical_since_ms_ = -1;
        }
        voltage_usb_ = usb_;
        voltage_charging_ = charging_;
        const float dt = last_ms_ < 0 ? 0 : std::clamp<float>((now_ms - last_ms_) / 1000.0f, 0, 5);
        last_ms_ = now_ms;
        last_valid_ms_ = now_ms;
        if (samples_ == 0 || filter_reset_) {
            filtered_mv_ = mv;
            filter_reset_ = false;
        } else {
            filtered_mv_ += (mv - filtered_mv_) * dt / (20.0f + dt);
        }
        if (++samples_ < 3) return true;

        if (!initialized_) {
            percent_ = VoltagePercent(filtered_mv_);
            // Restore only a nearby measurement. A battery changed or charged
            // substantially while off must be initialized from fresh voltage.
            const bool same_phase = saved_usb_ == usb_ && saved_charging_ == charging_;
            const int tolerance = same_phase ? 80 : 200;
            bool restored = false;
            if (saved_percent_ >= 0 && std::abs(filtered_mv_ - saved_mv_) <= tolerance) {
                // Same-phase voltage changes happened while off; they must
                // not be absorbed as a new cable/charge polarization step.
                float restored_step = same_phase ? saved_correction_mv_ :
                    filtered_mv_ - saved_mv_ + saved_correction_mv_;
                if (usb_ && !same_phase) {
                    // An edge may have been saved before its first ADC sample.
                    // Transfer the saved model, not the uncorrected old VBAT
                    // or the higher smoothed percentage.
                    float baseline = VoltageAtPercent(saved_full_ ? 100.0f :
                        std::min(saved_model_percent_, saved_percent_));
                    if (!charging_) baseline = std::max(baseline, static_cast<float>(saved_mv_));
                    restored_step = filtered_mv_ - baseline;
                }
                restored_step = std::clamp(restored_step, -200.0f, 200.0f);
                if (usb_ && !charging_) restored_step = std::min(restored_step, 0.0f);
                float restored_target = VoltagePercent(filtered_mv_ - restored_step);
                const bool saved_anchor_plausible = !saved_usb_ && !usb_ && saved_anchor_.Valid();
                if (saved_anchor_plausible) {
                    // Reuse the calibration unchanged: voltage lost while off
                    // must not be absorbed into a new voltage correction.
                    restored_step = 0;
                    restored_target = AnchoredPercent(filtered_mv_, saved_anchor_);
                }
                const bool saved_full_plausible = saved_full_ && saved_usb_ && usb_ &&
                    filtered_mv_ >= kMaintenanceFloorMv;
                if (saved_trusted_ || saved_full_plausible || saved_anchor_plausible ||
                    std::abs(saved_percent_ - restored_target) <= 15) {
                    percent_ = saved_percent_;
                    restored = true;
                    step_mv_ = restored_step;
                    charge_step_mv_ = charging_ ? step_mv_ : 0;
                    charge_step_reference_mv_ = filtered_mv_;
                    if (!usb_) {
                        if (saved_anchor_plausible) {
                            discharge_anchor_ = saved_anchor_;
                            if (!saved_trusted_) percent_ = std::min(percent_, restored_target);
                        } else {
                            discharge_anchor_ = MakeDischargeAnchor(saved_model_percent_, saved_mv_, filtered_mv_);
                        }
                        step_mv_ = charge_step_mv_ = 0;
                    }
                }
            }
            if (!usb_ && !discharge_anchor_.Valid()) {
                discharge_anchor_ = {percent_, VoltagePercent(filtered_mv_)};
            }
            if (usb_) percent_ = std::min(percent_, 99.0f);
            if (restored && saved_full_ && saved_usb_ && usb_ &&
                filtered_mv_ >= kMaintenanceFloorMv) {
                full_ = true;
                percent_ = saved_percent_;
                step_mv_ = charge_step_mv_ = 0;
            }
            // A quick software restart must not discard the CV evidence just
            // before EOC. Cold checkpoints cannot establish how long ago it was.
            if (restored && saved_trusted_ && saved_usb_ && usb_ &&
                saved_near_cv_mv_ >= kNearCvMv && saved_near_cv_mv_ <= 4350) {
                charge_peak_mv_ = saved_near_cv_mv_;
                near_full_at_ms_ = now_ms;
            }
            // Preserve a temporary visible percentage across a restart while
            // charging, but only when the saved display/model gap and fresh
            // voltage are consistent. This is separate from the capacity
            // model and is rejected after a real voltage drop while offline.
            if (restored && saved_display_floor_active_ && usb_ && !full_ &&
                filtered_mv_ >= saved_mv_ - 20) {
                display_floor_ = saved_percent_;
                display_floor_reference_mv_ = saved_display_floor_reference_mv_ > 0 ?
                    saved_display_floor_reference_mv_ : filtered_mv_;
                display_floor_drop_since_ms_ = -1;
            }
            initialized_ = true;
            return true;
        }
        UpdateChargeCycle(now_ms);
        if (now_ms < settling_until_ms_ && !full_) return true;
        if (transition_pending_) {
            if (!usb_) {
                discharge_anchor_ = MakeDischargeAnchor(transition_soc_, transition_voltage_mv_, filtered_mv_);
                step_mv_ = charge_step_mv_ = 0;
            } else {
                step_mv_ = std::clamp(filtered_mv_ - transition_mv_, -200.0f, 200.0f);
                if (!charging_) step_mv_ = std::min(step_mv_, 0.0f);
                charge_step_mv_ = charging_ ? step_mv_ : 0;
            }
            charge_step_reference_mv_ = filtered_mv_;
            if (usb_ && display_floor_ >= 0 && display_floor_baseline_pending_) {
                // Establish the powered baseline after the cable settling
                // sample; the pre-edge voltage is not a fault reference.
                display_floor_reference_mv_ = filtered_mv_;
                display_floor_drop_since_ms_ = -1;
                display_floor_baseline_pending_ = false;
            }
            transition_pending_ = false;
        }
        if (full_) {
            step_mv_ = charge_step_mv_ = 0;
            ClearDisplayFloor();
        } else if (charging_ && charge_step_mv_ != 0) {
            // Retain the observed charge-voltage step. Decaying it merely with
            // time would fabricate charge even at an unchanged battery voltage.
            // Polarization compensation tapers towards the 4.2 V CV endpoint.
            // If insertion already reached the CV region, there is no usable
            // voltage span from which to infer added capacity. Keep the edge
            // correction until STAT confirms completion, instead of erasing it
            // at the first 4.2 V sample and fabricating charge on every plug-in.
            const float taper = charge_step_reference_mv_ >= 4180 ? 1.0f :
                std::clamp((4200.0f - filtered_mv_) /
                    (4200.0f - charge_step_reference_mv_), 0.0f, 1.0f);
            const float tapered_step = charge_step_mv_ * taper;
            // Once polarization has tapered, a voltage fall must not grow it
            // again and count the same voltage fall twice.
            step_mv_ = charge_step_mv_ > 0 ? std::min(step_mv_, tapered_step) :
                std::max(step_mv_, tapered_step);
        }

        if (display_floor_ >= 0) {
            if (!usb_ || supply_low_) {
                ClearDisplayFloor();
            } else if (filtered_mv_ <= 3400) {
                ClearDisplayFloor();
            } else if (filtered_mv_ + kDisplayFloorDropMv < display_floor_reference_mv_) {
                if (display_floor_drop_since_ms_ < 0) display_floor_drop_since_ms_ = now_ms;
                if (now_ms - display_floor_drop_since_ms_ >= kDisplayFloorDropHoldMs) {
                    ClearDisplayFloor();
                }
            } else {
                display_floor_drop_since_ms_ = -1;
                display_floor_reference_mv_ = std::max(display_floor_reference_mv_, filtered_mv_);
            }
        }

        // A real discharge while unplugged must release the carried full
        // display before a later cable insertion can use it.
        if (!usb_ && full_display_hold_ && percent_ < 99.5f) {
            full_display_hold_ = false;
        }

        float target = TargetPercent();
        if (!usb_) target = std::min(target, percent_);
        // Keep the old display from stepping down while healthy charging is
        // settling. The independent voltage target is never raised.
        if (usb_ && display_floor_ >= 0 && target < display_floor_) {
            target = display_floor_;
        } else if (display_floor_ >= 0 && target >= display_floor_ - 0.05f) {
            ClearDisplayFloor();
        }

        // Sustained low voltage must not be hidden by a slowly changing UI.
        if (filtered_mv_ <= 3400) {
            if (critical_since_ms_ < 0) critical_since_ms_ = now_ms;
        } else {
            critical_since_ms_ = -1;
        }
        const bool critical = critical_since_ms_ >= 0 && now_ms - critical_since_ms_ >= 15000;
        const float time_constant = full_ ? 10.0f : 180.0f;
        const float correction = (target - percent_) * dt / (time_constant + dt);
        const float down_rate = critical ? 1.0f / 5 : 1.0f / 30;
        // Bound by elapsed time, never by number of reads or missed timer ticks.
        percent_ += std::clamp(correction, -dt * down_rate, dt / (full_ ? 5.0f : 30.0f));
        percent_ = std::clamp(percent_, 0.0f, 100.0f);
        return true;
    }

    bool Valid(int64_t now_ms) const {
        return initialized_ && last_valid_ms_ >= 0 && now_ms - last_valid_ms_ <= 60000;
    }
    int Percent() const {
        if (!initialized_) return -1;
        // Give 100% the same one-percent interval as the following steps;
        // rounding to nearest used to spend only half a step at full. Keep
        // the conservative low-battery rounding and the unfinished-charge cap.
        const int displayed = percent_ >= 10 ? static_cast<int>(std::ceil(percent_ - 0.001f)) :
            static_cast<int>(std::lround(percent_));
        // Match the same threshold used by the ceil-based display conversion
        // above. The independent full_display_hold_ credential is what makes
        // this exception safe for a previously confirmed full battery.
        const bool carry_full_display = usb_ && !full_ && full_display_hold_ &&
            display_floor_ >= 99.001f;
        return std::clamp(displayed, 0, usb_ && !full_ && !carry_full_display ? 99 : 100);
    }
    float PrecisePercent() const { return percent_; }
    int Millivolts() const { return std::lround(filtered_mv_); }
    float TargetPercent() const {
        if (full_) return 100;
        if (transition_pending_) return transition_soc_;
        if (!usb_ && discharge_anchor_.Valid()) return AnchoredPercent(filtered_mv_, discharge_anchor_);
        const float target = VoltagePercent(filtered_mv_ - step_mv_);
        const float safe_target = filtered_mv_ <= 3400 ? std::min(target, VoltagePercent(filtered_mv_)) : target;
        return usb_ ? std::min(safe_target, 99.0f) : safe_target;
    }
    DischargeAnchor GetDischargeAnchor() const {
        // Export a coherent provisional anchor if a reset happens during the
        // settling window. Before the first battery sample, voltage_usb_ still
        // identifies the old phase and restore handles the edge explicitly.
        if (!usb_ && transition_pending_)
            return MakeDischargeAnchor(transition_soc_, transition_voltage_mv_, filtered_mv_);
        return discharge_anchor_;
    }
    // During settling, persist the baseline already applied to this voltage,
    // rather than pairing a post-edge sample with a pre-edge correction.
    int CorrectionMillivolts() const {
        if (full_) return 0;
        float correction = transition_pending_ ?
            std::clamp(filtered_mv_ - transition_mv_, -200.0f, 200.0f) : step_mv_;
        if (voltage_usb_ && !voltage_charging_) correction = std::min(correction, 0.0f);
        return std::lround(correction);
    }
    bool VoltageWasOnUsb() const { return voltage_usb_; }
    bool VoltageWasCharging() const { return voltage_charging_; }
    bool Full() const { return full_; }
    bool SupplyLow() const { return supply_low_; }
    bool FullInferred() const { return full_inferred_; }
    bool DisplayFloorActive() const { return display_floor_ >= 0; }
    int DisplayFloorReferenceMillivolts() const {
        return display_floor_reference_mv_ > 0 ? std::lround(display_floor_reference_mv_) : 0;
    }
    int RecentNearCvMillivolts() const {
        return near_full_at_ms_ >= 0 && last_valid_ms_ - near_full_at_ms_ <= 300000 ?
            std::lround(charge_peak_mv_) : 0;
    }

private:
    static DischargeAnchor MakeDischargeAnchor(float soc, float previous_mv, float mv) {
        // Only a bounded voltage step can be attributed to changing supply.
        // A failing battery dropping hundreds of mV must not become "100%".
        const float previous_curve = VoltagePercent(previous_mv);
        if (previous_mv - mv > 200 && previous_curve > 0) {
            soc *= VoltagePercent(mv + 200) / previous_curve;
        }
        return {std::clamp(soc, 0.0f, 100.0f), VoltagePercent(mv)};
    }
    static float VoltageAtPercent(float soc) {
        float low = 3000, high = 4200;
        for (int i = 0; i < 20; ++i) {
            const float mid = (low + high) / 2;
            if (VoltagePercent(mid) < soc) low = mid;
            else high = mid;
        }
        return (low + high) / 2;
    }
    static float AnchoredPercent(float mv, const DischargeAnchor& anchor) {
        const float curve = VoltagePercent(mv);
        const float loaded_anchor_mv = VoltageAtPercent(anchor.curve);
        const float ocv_anchor_mv = VoltageAtPercent(anchor.soc);
        if (loaded_anchor_mv <= 3400 || ocv_anchor_mv <= 3400) {
            // Near empty there is no usable voltage span for load correction.
            return std::clamp(std::min(curve, anchor.soc * curve / anchor.curve), 0.0f, anchor.soc);
        }
        // Supply/load changes act on voltage. Scaling curve percentages instead
        // moved the full endpoint onto the much steeper 4.05 V part of the OCV
        // curve, exaggerating subsequent high-SOC depletion.
        const float offset_mv = std::clamp(ocv_anchor_mv - loaded_anchor_mv, -200.0f, 200.0f);
        // Remove correction continuously over the discharge span, not on a
        // timer or abruptly at the low-voltage knee. This also gives a positive
        // voltage slope for negative offsets when both anchors exceed 3.4 V.
        const float weight = std::clamp((mv - 3400.0f) / (loaded_anchor_mv - 3400.0f), 0.0f, 1.0f);
        return std::clamp(VoltagePercent(mv + offset_mv * weight), 0.0f, anchor.soc);
    }
    // ETA6098 CV is 4.16..4.24 V; recharge starts 160 mV below EOC.
    // These are voltage plausibility guards for STAT, not voltage-only SOC.
    static constexpr int kNearCvMv = 4140;
    static constexpr int kBootFullMv = 4100;
    static constexpr int kMaintenanceFloorMv = 3980;
    static constexpr int kDisplayFloorDropMv = 25;
    static constexpr int64_t kDisplayFloorDropHoldMs = 120000;

    void BeginDisplayFloor() {
        if (!initialized_ || full_) return;
        if (display_floor_ < 0 || percent_ > display_floor_) display_floor_ = percent_;
        display_floor_reference_mv_ = filtered_mv_;
        display_floor_drop_since_ms_ = -1;
        display_floor_baseline_pending_ = true;
    }

    void ClearDisplayFloor() {
        display_floor_ = -1;
        display_floor_reference_mv_ = 0;
        display_floor_drop_since_ms_ = -1;
        display_floor_baseline_pending_ = false;
    }

    void UpdateChargeCycle(int64_t now_ms) {
        if (!usb_) return;
        if (charging_ && filtered_mv_ >= kNearCvMv) {
            if (near_full_since_ms_ < 0) near_full_since_ms_ = now_ms;
            if (now_ms - near_full_since_ms_ >= 30000) {
                near_full_at_ms_ = now_ms;
                charge_peak_mv_ = std::max(charge_peak_mv_, filtered_mv_);
            }
        } else {
            near_full_since_ms_ = -1;
        }

        // Latch completed charge through normal relaxation and recharge. A
        // genuinely low battery on USB must still be reported, not frozen full.
        if (full_) {
            if (filtered_mv_ < kMaintenanceFloorMv) {
                if (low_supply_since_ms_ < 0) low_supply_since_ms_ = now_ms;
                const int64_t timeout = filtered_mv_ <= 3400 ? 15000 : 120000;
                if (now_ms - low_supply_since_ms_ >= timeout) {
                    full_ = false;
                    supply_low_ = true;
                    near_full_at_ms_ = -1;
                    session_inactive_ = false;
                }
            } else {
                low_supply_since_ms_ = -1;
            }
            if (full_) return;
        }

        const bool recent_cv = near_full_at_ms_ >= 0 && now_ms - near_full_at_ms_ <= 300000;
        const float completion_floor = std::max(4000.0f, charge_peak_mv_ - 180.0f);
        const bool completed = !charging_ && recent_cv && filtered_mv_ >= completion_floor;
        const bool already_full = !charging_ && session_inactive_ && filtered_mv_ >= kBootFullMv;
        if (completed || already_full) {
            if (full_since_ms_ < 0) full_since_ms_ = now_ms;
            if (now_ms - full_since_ms_ >= (completed ? 120000 : 300000)) {
                full_ = true;
                supply_low_ = false;
                full_inferred_ = !completed;
                low_supply_since_ms_ = -1;
            }
        } else {
            full_since_ms_ = -1;
        }
    }

    bool power_initialized_ = false, usb_ = false, charging_ = false;
    bool initialized_ = false, full_ = false, filter_reset_ = false, transition_pending_ = false;
    bool full_display_hold_ = false;
    bool voltage_usb_ = false;
    bool voltage_charging_ = false, saved_charging_ = false, saved_full_ = false;
    bool session_inactive_ = false, full_inferred_ = false, supply_low_ = false;
    int samples_ = 0, saved_mv_ = 0;
    int saved_near_cv_mv_ = 0;
    DischargeAnchor discharge_anchor_, saved_anchor_;
    bool saved_usb_ = false, saved_trusted_ = false;
    float percent_ = 0, filtered_mv_ = 0, step_mv_ = 0, transition_mv_ = 0, saved_percent_ = -1;
    float charge_step_mv_ = 0, charge_step_reference_mv_ = 0, saved_correction_mv_ = 0;
    float charge_peak_mv_ = 0;
    float transition_soc_ = 0, transition_voltage_mv_ = 0, saved_model_percent_ = -1;
    float display_floor_ = -1, display_floor_reference_mv_ = 0;
    bool display_floor_baseline_pending_ = false;
    bool saved_display_floor_active_ = false;
    int saved_display_floor_reference_mv_ = 0;
    int64_t last_ms_ = -1, last_valid_ms_ = -1, settling_until_ms_ = 0;
    int64_t full_since_ms_ = -1, critical_since_ms_ = -1;
    int64_t near_full_since_ms_ = -1, near_full_at_ms_ = -1, low_supply_since_ms_ = -1;
    int64_t display_floor_drop_since_ms_ = -1;
};

}  // namespace baji

#include "../power/battery_estimator.h"
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Device {
    baji::BatteryEstimator battery;
    int64_t now = 0;
    bool usb = false, charging = false;
    void Read(int mv, int seconds = 5) {
        now += seconds * 1000;
        battery.SetPower(usb, charging, now);
        battery.Observe(mv, now);
    }
    void Warm(int mv) { for (int i = 0; i < 3; ++i) Read(mv); }
    void Run(int mv, int seconds) { for (int i = 0; i < seconds / 5; ++i) Read(mv); }
    void SetCable(bool connected) {
        usb = charging = connected;
        battery.SetPower(usb, charging, now);
    }
    void SetCharging(bool active) {
        charging = active;
        battery.SetPower(usb, charging, now);
    }
};

Device Restart(const Device& source, int fresh_mv, bool software_reset) {
    Device restarted;
    restarted.usb = source.usb;
    restarted.charging = source.charging;
    restarted.battery.Restore(source.battery.PrecisePercent(), source.battery.Millivolts(),
        source.battery.VoltageWasOnUsb(), source.battery.CorrectionMillivolts(), software_reset,
        source.battery.Full(), source.battery.VoltageWasCharging(), source.battery.RecentNearCvMillivolts(),
        source.battery.TargetPercent(), source.battery.DisplayFloorActive(),
        source.battery.DisplayFloorReferenceMillivolts());
    restarted.battery.RestoreDischargeAnchor(source.battery.GetDischargeAnchor());
    restarted.Warm(fresh_mv);
    return restarted;
}

Device CompletedCharge() {
    Device device;
    device.usb = device.charging = true;
    device.Warm(4160);
    device.Run(4160, 120);
    device.SetCharging(false);
    device.Run(4142, 600);
    Check(device.battery.Full() && device.battery.Percent() == 100,
        "full-unplug fixture has actual CV and charger completion evidence");
    return device;
}

Device LaggingDisplay(int displayed) {
    Device device;
    // Just above the next display boundary, while the voltage model already
    // reports slightly less capacity. This is ordinary discharge smoothing,
    // not evidence of new depletion after the cable is inserted.
    device.battery.Restore(displayed - 0.98f, 4051, false, 0, true,
        false, false, 0, displayed - 1.55f);
    device.Warm(4051);
    Check(device.battery.Percent() == displayed &&
        device.battery.TargetPercent() < device.battery.PrecisePercent() - 0.5f,
        "charging-boundary fixture has a higher display than its independent voltage model");
    return device;
}

void CheckChargingDisplay(Device& device, int mv, int seconds, const char* message) {
    int previous = device.battery.Percent();
    bool monotonic = true;
    for (int elapsed = 0; elapsed < seconds; elapsed += 5) {
        device.Read(mv);
        const int current = device.battery.Percent();
        monotonic = monotonic && current >= previous;
        previous = current;
    }
    Check(monotonic, message);
}

void CheckRestart(const Device& source, int fresh_mv, int expected, bool software_reset) {
    auto restarted = Restart(source, fresh_mv, software_reset);
    Check(std::abs(restarted.battery.Percent() - expected) <= 1,
        "cable transition checkpoint preserves percentage at restart");
    restarted.Run(fresh_mv, 1800);
    Check(std::abs(restarted.battery.Percent() - expected) <= 2,
        "cable transition checkpoint retains compensation after restart");
    Check(std::abs(restarted.battery.TargetPercent() - expected) <= 2,
        "cable transition checkpoint does not hide an incorrect voltage target");
}
}

int main() {
    using baji::BatteryEstimator;
    float previous = -1;
    for (int mv = 2700; mv <= 4400; ++mv) {
        const auto percent = BatteryEstimator::VoltagePercent(mv);
        Check(percent >= previous && percent >= 0 && percent <= 100, "OCV curve bounds and monotonicity");
        previous = percent;
    }
    Check(BatteryEstimator::VoltagePercent(3000) == 0, "3.0 V endpoint");
    Check(BatteryEstimator::VoltagePercent(4200) == 100, "4.2 V endpoint");

    // A discharging integer represents its complete 1% interval. Rounding
    // to nearest made 100% occupy only half the interval of the next steps.
    struct DisplayCase { float precise; int displayed; };
    const DisplayCase display_cases[] = {{100.0f, 100}, {99.99f, 100}, {99.5f, 100},
             {99.01f, 100}, {99.0f, 99}, {98.6f, 99}, {98.0f, 98},
             {10.1f, 11}, {10.0f, 10}, {9.49f, 9}, {9.51f, 10},
             {0.51f, 1}, {0.49f, 0}, {0.0f, 0}};
    for (const auto sample : display_cases) {
        Device quantized;
        quantized.battery.Restore(sample.precise, 3800, false, 0, true);
        quantized.Warm(3800);
        Check(quantized.battery.Percent() == sample.displayed,
            "discharge quantization gives a full top interval and preserves the low empty threshold");
        const float precise_before = quantized.battery.PrecisePercent();
        for (int redraw = 0; redraw < 100; ++redraw) {
            Check(quantized.battery.Percent() == sample.displayed &&
                quantized.battery.PrecisePercent() == precise_before,
                "repeated display reads cannot consume or create capacity");
        }
    }
    Device charging_quantized;
    charging_quantized.usb = charging_quantized.charging = true;
    charging_quantized.battery.Restore(99.9f, 4200, true, 0, true);
    charging_quantized.Warm(4200);
    Check(charging_quantized.battery.Percent() == 99 && !charging_quantized.battery.Full(),
        "display quantization cannot turn an unfinished USB charge into 100 percent");

    // Exercise voltage-domain anchor compensation across the taper endpoint,
    // including negative offsets and near-empty anchors where a naive taper
    // can reverse the voltage/SOC slope. These are model invariants, not a
    // claimed capacity calibration of the physical 703048 cell.
    struct AnchorCase { float soc; int loaded_mv; };
    const AnchorCase anchor_cases[] = {{100.0f, 4096}, {75.0f, 3900}, {15.0f, 3740},
             {60.0f, 4100}, {30.0f, 3990}, {1.0f, 3500},
             {0.1f, 3450}, {3.0f, 3375}, {70.0f, 3340}};
    for (const auto sample : anchor_cases) {
        const BatteryEstimator::DischargeAnchor anchor{
            sample.soc, BatteryEstimator::VoltagePercent(sample.loaded_mv)};
        float last_target = 100;
        for (int mv = 4300; mv >= 2800; --mv) {
            Device probe;
            probe.battery.Restore(sample.soc, mv, false, 0, true);
            probe.battery.RestoreDischargeAnchor(anchor);
            probe.Warm(mv);
            const float target = probe.battery.TargetPercent();
            Check(std::isfinite(target) && target >= 0 && target <= sample.soc + 0.001f,
                "positive, negative and near-empty anchors produce finite bounded capacity");
            Check(target <= last_target + 0.001f,
                "anchored capacity remains monotonic through the low-voltage taper");
            if (mv <= 3400) {
                Check(target <= BatteryEstimator::VoltagePercent(mv) + 0.001f,
                    "no anchor compensation masks the physical low-voltage region");
            }
            if (mv == 3000) Check(target == 0, "every anchor reaches the true empty endpoint");
            last_target = target;
        }
    }

    // USB uses the short board-level debounce selected by the monitor; the
    // constructor is supplied by the production header in the same change.
    baji::StableInput usb_input(150);
    Check(!usb_input.Update(false, 0), "initial USB state");
    Check(!usb_input.Update(true, 100) && usb_input.Pending(), "USB insertion begins debounce");
    Check(!usb_input.Update(false, 170) && !usb_input.Pending(), "short insertion pulse is ignored");
    Check(!usb_input.Update(true, 200) && usb_input.Pending(), "stable USB insertion starts anew");
    Check(!usb_input.Update(true, 349), "USB insertion needs the full 150 ms");
    Check(usb_input.Update(true, 350) && !usb_input.Pending(), "USB insertion is accepted at 150 ms");
    Check(usb_input.Update(false, 400) && usb_input.Pending(), "USB removal begins debounce");
    Check(usb_input.Update(true, 450) && !usb_input.Pending(), "short removal pulse is ignored");
    Check(usb_input.Update(false, 500) && usb_input.Pending(), "stable USB removal starts anew");
    Check(usb_input.Update(false, 649), "USB removal needs the full 150 ms");
    Check(!usb_input.Update(false, 650) && !usb_input.Pending(), "USB removal is accepted at 150 ms");

    baji::StableInput charge_input;
    Check(!charge_input.Update(false, 0), "initial STAT state");
    Check(!charge_input.Update(true, 100) && charge_input.Pending(), "STAT begins debounce");
    Check(!charge_input.Update(false, 900) && !charge_input.Pending(), "short STAT pulse is ignored");
    Check(!charge_input.Update(true, 1000), "second STAT edge pending");
    Check(!charge_input.Update(true, 2999), "STAT retains its full 2-second debounce");
    Check(charge_input.Update(true, 3000) && !charge_input.Pending(), "stable STAT edge accepted once");

    Device stable;
    Check(!stable.battery.Valid(0) && stable.battery.Percent() == -1, "no fabricated startup percentage");
    Check(!stable.battery.Observe(4500, 0), "impossible ADC voltage ignored");
    stable.Warm(3800);
    const int initial = stable.battery.Percent();
    Check(initial >= 42 && initial <= 44, "initial voltage estimate");
    stable.Run(3800, 600);
    Check(stable.battery.Percent() == initial, "no timer-driven drain at constant voltage");
    for (int i = 0; i < 120; ++i) stable.Read(i % 2 ? 3795 : 3805);
    Check(std::abs(stable.battery.Percent() - initial) <= 2, "small voltage noise is bounded");

    for (const int plugged_seconds : {5, 15, 25, 60, 300}) {
        Device cable;
        cable.Warm(3800);
        const int before = cable.battery.Percent();
        cable.usb = cable.charging = true;
        cable.Run(4000, plugged_seconds);
        Check(std::abs(cable.battery.Percent() - before) <= 1, "insertion voltage step is not capacity");
        cable.usb = cable.charging = false;
        cable.Run(3800, 1800);
        Check(std::abs(cable.battery.Percent() - before) <= 2, "unplug and voltage relaxation do not change capacity");
        Check(cable.battery.TargetPercent() < 50, "short insertion cannot poison unplug baseline");
    }

    for (const bool software_reset : {false, true}) {
        for (const int sampled_seconds : {0, 5, 10, 15, 25}) {
            Device inserting;
            inserting.Warm(3800);
            const int before = inserting.battery.Percent();
            inserting.SetCable(true);
            inserting.Run(4000, sampled_seconds);
            Check(inserting.battery.VoltageWasOnUsb() == (sampled_seconds != 0),
                "insertion checkpoint pairs voltage with its sampled USB state");
            Check(inserting.battery.CorrectionMillivolts() == (sampled_seconds == 0 ? 0 : 200),
                "insertion checkpoint exports pending polarization correction");
            CheckRestart(inserting, 4000, before, software_reset);

            Device unplugging;
            unplugging.Warm(3800);
            unplugging.SetCable(true);
            unplugging.Run(4000, 300);
            const int charged = unplugging.battery.Percent();
            unplugging.SetCable(false);
            unplugging.Run(3800, sampled_seconds);
            Check(unplugging.battery.VoltageWasOnUsb() == (sampled_seconds == 0),
                "unplug checkpoint pairs voltage with its sampled USB state");
            Check(unplugging.battery.CorrectionMillivolts() == (sampled_seconds == 0 ? 200 : 0),
                "unplug checkpoint exports correction for the saved voltage");
            CheckRestart(unplugging, 3800, charged, software_reset);
        }

        Device short_cycle;
        short_cycle.Warm(3800);
        const int before = short_cycle.battery.Percent();
        short_cycle.SetCable(true);
        short_cycle.Run(4000, 5);
        short_cycle.SetCable(false);
        CheckRestart(short_cycle, 3800, before, software_reset);
        short_cycle.Run(3800, 5);
        CheckRestart(short_cycle, 3800, before, software_reset);
        short_cycle.SetCable(true);
        CheckRestart(short_cycle, 4000, before, software_reset);
        short_cycle.Run(4000, 5);
        CheckRestart(short_cycle, 4000, before, software_reset);

        Device no_sample_cycle;
        no_sample_cycle.Warm(3800);
        no_sample_cycle.SetCable(true);
        no_sample_cycle.SetCable(false);
        CheckRestart(no_sample_cycle, 3800, before, software_reset);
    }

    Device saved;
    saved.battery.Restore(44, 4000, true, 200);
    saved.usb = saved.charging = true;
    saved.Warm(4000);
    Check(saved.battery.Percent() == 44, "cold restart retains charge-voltage compensation");
    saved.Run(4000, 600);
    Check(std::abs(saved.battery.Percent() - 44) <= 2, "restored compensation does not fabricate charge");
    Device changed;
    changed.battery.Restore(90, 4100, false);
    changed.Warm(3700);
    Check(changed.battery.Percent() < 20, "stale checkpoint rejected after battery change");

    Device cv;
    cv.usb = cv.charging = true;
    cv.battery.Restore(70, 4200, true, 0, true);
    cv.Warm(4200);
    cv.Run(4200, 1800);
    Check(!cv.battery.Full() && cv.battery.Percent() <= 99, "4.2 V while still charging is not full");
    cv.charging = false;
    cv.Run(4200, 300);
    Check(cv.battery.Full() && cv.battery.Percent() == 100, "hardware completion and stable full voltage converge to 100");
    Device fault;
    fault.usb = true;
    fault.Warm(4000);
    fault.Run(4000, 600);
    Check(!fault.battery.Full() && fault.battery.Percent() < 100, "inactive STAT at low voltage is not full");
    Device restart_full;
    restart_full.usb = true;
    restart_full.battery.Restore(100, 4200, true, 0, true, true, false);
    restart_full.Warm(4200);
    Check(restart_full.battery.Full() && restart_full.battery.Percent() == 100, "warm restart retains confirmed full charge");

    // Reported full-unplug failure: the previous estimator changed a confirmed
    // 100% at 4142 mV into 90.14% after 520 s of an unchanged 4050 mV input.
    // Even unplugging with no voltage step at all slowly fabricated a 4% loss.
    for (const int loaded_mv : {4142, 4100, 4050}) {
        auto unplugged = CompletedCharge();
        unplugged.SetCable(false);
        Check(!unplugged.battery.Full(), "unplug still ends the charging completion indicator");
        for (int elapsed = 5; elapsed <= 3600; elapsed += 5) {
            unplugged.Read(loaded_mv);
            Check(std::abs(unplugged.battery.TargetPercent() - 100) < 0.01f,
                "constant loaded voltage cannot reduce the full discharge model with time");
            Check(unplugged.battery.Percent() == 100,
                "constant loaded voltage retains full capacity for an hour after unplug");
            if (elapsed == 520) {
                Check(unplugged.battery.PrecisePercent() > 99.99f,
                    "520-second full-to-90 regression is corrected, not only slowed");
            }
        }
    }

    // Replay the inferred voltage shape of the documented 100 -> 95% run.
    // Raw timestamps and VBAT were lost in that old log, so each point here
    // uses a deliberate synthetic 60-second interval, not recovered timing.
    // The regression is the target's excessive response to 4096 -> 4050 mV;
    // it does not establish the actual cell's remaining capacity.
    auto high_end_replay = CompletedCharge();
    high_end_replay.SetCable(false);
    high_end_replay.Run(4096, 60);
    const auto replay_anchor = high_end_replay.battery.GetDischargeAnchor();
    float previous_replay_soc = high_end_replay.battery.PrecisePercent();
    for (const int mv : {4094, 4082, 4076, 4068, 4066, 4060, 4059, 4056, 4051, 4050}) {
        high_end_replay.Run(mv, 60);
        Check(high_end_replay.battery.PrecisePercent() <= previous_replay_soc + 0.001f,
            "high-end replay depletes monotonically without displayed recovery");
        previous_replay_soc = high_end_replay.battery.PrecisePercent();
    }
    Check(high_end_replay.battery.TargetPercent() > 96 &&
        high_end_replay.battery.TargetPercent() < 98,
        "high-end voltage drop follows the compensated voltage curve instead of the loaded percentage ratio");
    Check(high_end_replay.battery.Percent() >= 97 && high_end_replay.battery.Percent() < 100,
        "synthetic ten-minute replay reduces premature high-end loss without locking the display at full");
    for (const bool software_reset : {false, true}) {
        auto restarted = Restart(high_end_replay, high_end_replay.battery.Millivolts(), software_reset);
        Check(restarted.battery.PrecisePercent() <= high_end_replay.battery.PrecisePercent() + 0.001f,
            "restarting after high-end discharge cannot manufacture capacity");
        Check(std::abs(restarted.battery.GetDischargeAnchor().soc - replay_anchor.soc) < 0.001f &&
            std::abs(restarted.battery.GetDischargeAnchor().curve - replay_anchor.curve) < 0.001f,
            "high-end voltage compensation retains its calibration across restart");
    }
    high_end_replay.Run(4050, 3600);
    const float settled_replay_target = high_end_replay.battery.TargetPercent();
    high_end_replay.Run(4050, 3600);
    Check(std::abs(high_end_replay.battery.TargetPercent() - settled_replay_target) < 0.001f &&
        std::abs(high_end_replay.battery.PrecisePercent() - settled_replay_target) < 0.01f,
        "a flat voltage following the replay converges without timer-driven target depletion");
    const float replay_before_cables = high_end_replay.battery.PrecisePercent();
    for (int cycle = 0; cycle < 5; ++cycle) {
        high_end_replay.SetCable(true);
        high_end_replay.Run(4200, 25);
        high_end_replay.SetCable(false);
        high_end_replay.Run(4050, 60);
        Check(high_end_replay.battery.PrecisePercent() <= replay_before_cables + 0.01f &&
            high_end_replay.battery.TargetPercent() <= settled_replay_target + 0.01f,
            "short repeated plugs cannot turn voltage-domain compensation into added charge");
    }

    for (const bool software_reset : {false, true}) {
        for (const int loaded_mv : {4100, 4050}) {
            for (const int sampled_seconds : {0, 5, 25, 60}) {
                auto unplugged = CompletedCharge();
                unplugged.SetCable(false);
                unplugged.Run(loaded_mv, sampled_seconds);
                auto restarted = Restart(unplugged, loaded_mv, software_reset);
                Check(restarted.battery.Percent() == 100,
                    "restart on either side of the unplug settling window retains full capacity");
                restarted.Run(loaded_mv, 3600);
                Check(restarted.battery.Percent() == 100 &&
                    std::abs(restarted.battery.TargetPercent() - 100) < 0.01f,
                    "full discharge anchor survives warm and cold restart without later drift");
            }
        }
    }

    auto anchored_discharge = CompletedCharge();
    anchored_discharge.SetCable(false);
    anchored_discharge.Run(4050, 60);
    const auto original_anchor = anchored_discharge.battery.GetDischargeAnchor();
    Check(original_anchor.Valid(), "stable full unplug establishes a valid discharge anchor");
    auto cold_discharge = Restart(anchored_discharge, 4030, false);
    const auto restored_anchor = cold_discharge.battery.GetDischargeAnchor();
    Check(std::abs(restored_anchor.soc - original_anchor.soc) < 0.001f &&
        std::abs(restored_anchor.curve - original_anchor.curve) < 0.001f,
        "cold restart restores the prior discharge anchor without recalibrating it");
    Check(cold_discharge.battery.PrecisePercent() < 99 &&
        cold_discharge.battery.TargetPercent() < 99,
        "a real 20 mV loss while off is not absorbed into a new discharge anchor");
    const float cold_target = cold_discharge.battery.TargetPercent();
    cold_discharge.Run(4030, 3600);
    Check(std::abs(cold_discharge.battery.TargetPercent() - cold_target) < 0.01f &&
        std::abs(cold_discharge.battery.PrecisePercent() - cold_target) < 0.01f,
        "restored cold discharge does not invent additional capacity loss at constant voltage");

    int last_anchored_percent = anchored_discharge.battery.Percent();
    for (int i = 1; i <= 2880; ++i) {
        anchored_discharge.Read(4050 - i * 1050 / 2880);
        const int percent = anchored_discharge.battery.Percent();
        Check(percent <= last_anchored_percent && last_anchored_percent - percent <= 1,
            "anchored discharge follows real falling voltage smoothly and monotonically");
        last_anchored_percent = percent;
    }
    Check(anchored_discharge.battery.Percent() < 3,
        "a full discharge anchor cannot mask the battery empty endpoint");
    anchored_discharge.Run(3000, 600);
    Check(anchored_discharge.battery.Percent() == 0 && anchored_discharge.battery.TargetPercent() < 0.01f,
        "sustained 3.0 V reaches zero even with a full-charge anchor");

    auto replugged_full = CompletedCharge();
    replugged_full.SetCable(false);
    Check(replugged_full.battery.Percent() == 100,
        "confirmed full charge remains 100 percent immediately after unplug");
    replugged_full.SetCable(true);
    Check(replugged_full.battery.Percent() == 100 && !replugged_full.battery.Full(),
        "confirmed full charge remains 100 percent on immediate replug before completion re-detection");
    replugged_full.Run(4050, 60);
    Check(replugged_full.battery.Percent() == 100 && replugged_full.battery.TargetPercent() >= 99,
        "full-anchor transfer into active charging cannot invent loss at constant USB voltage");

    auto implausible_unplug = CompletedCharge();
    implausible_unplug.SetCable(false);
    implausible_unplug.Run(3500, 60);
    Check(implausible_unplug.battery.TargetPercent() < 15,
        "a gross voltage loss on unplug cannot be accepted as a full discharge anchor");
    implausible_unplug.Run(3500, 3600);
    Check(implausible_unplug.battery.Percent() < 15,
        "an implausibly low unplug voltage cannot leave the display frozen at full");

    auto lagging_display = CompletedCharge();
    lagging_display.SetCable(false);
    lagging_display.Run(4050, 60);
    lagging_display.Run(4008, 60);
    const float depleted_model = lagging_display.battery.TargetPercent();
    Check(lagging_display.battery.PrecisePercent() > depleted_model + 1,
        "edge-transfer fixture has a display still catching up with actual modeled depletion");
    lagging_display.SetCable(true);
    lagging_display.Run(4150, 60);
    Check(lagging_display.battery.TargetPercent() <= depleted_model + 0.01f,
        "plugging in cannot replace depleted model capacity with the lagging higher display");
    lagging_display.SetCable(false);
    lagging_display.Run(4008, 60);
    Check(lagging_display.battery.TargetPercent() <= depleted_model + 0.01f,
        "unplugging cannot turn UI smoothing lag into restored capacity");

    // User reports 93 -> 92 immediately after insertion; COM6 also recorded
    // 97 -> 96 with active STAT and a stable 4194..4197 mV charging voltage.
    // A lower pre-insertion model must survive for unplug/restart accuracy,
    // but its old smoothing debt must not appear as depletion while charging.
    for (const int displayed : {93, 97}) {
        for (const int charge_mv : {4150, 4195}) {
            auto inserting = LaggingDisplay(displayed);
            const float original_model = inserting.battery.TargetPercent();
            inserting.SetCable(true);
            CheckChargingDisplay(inserting, charge_mv, 180,
                "stable charging must not replay 93-to-92 or 97-to-96 smoothing debt");
            Check(inserting.battery.Percent() == displayed &&
                !inserting.battery.Full(),
                "stable insertion neither deducts old display debt nor fabricates completed charge");
            Check(inserting.battery.TargetPercent() <= original_model + 0.01f,
                "charging display retention cannot raise the independent voltage model");

            auto noised = inserting;
            int previous_charge = noised.battery.Percent();
            bool noise_monotonic = true;
            for (int i = 0; i < 120; ++i) {
                noised.Read(charge_mv + (i % 2 ? -3 : 3));
                noise_monotonic = noise_monotonic && noised.battery.Percent() >= previous_charge;
                previous_charge = noised.battery.Percent();
            }
            Check(noise_monotonic,
                "ordinary charging ADC ripple cannot cause downward display steps");

            auto briefly_loaded = inserting;
            CheckChargingDisplay(briefly_loaded, charge_mv - 50, 60,
                "a short USB load sag must not defeat the charging display guard");
            CheckChargingDisplay(briefly_loaded, charge_mv, 180,
                "charging recovers from a transient load without a delayed percentage drop");
            Check(!briefly_loaded.battery.SupplyLow(),
                "a recovered short load sag is not a sustained USB supply fault");

            auto persistent_dip = inserting;
            persistent_dip.Run(charge_mv - 50, 900);
            Check(!persistent_dip.battery.DisplayFloorActive() &&
                persistent_dip.battery.Percent() < displayed,
                "sustained 50 mV loss during an unfinished charge must escape display protection");

            auto interrupted_dip = inserting;
            interrupted_dip.Run(charge_mv - 50, 90);
            Check(interrupted_dip.battery.DisplayFloorActive(),
                "90 seconds of USB voltage decline must keep the display guard active");
            interrupted_dip.Read(4500, 60);
            interrupted_dip.Read(charge_mv - 50);
            interrupted_dip.Run(charge_mv - 50, 90);
            Check(interrupted_dip.battery.DisplayFloorActive(),
                "an invalid ADC gap cannot count towards releasing the display guard");
            interrupted_dip.Run(charge_mv - 50, 120);
            Check(!interrupted_dip.battery.DisplayFloorActive() &&
                interrupted_dip.battery.Percent() < displayed,
                "fresh sustained USB voltage decline releases the display guard after ADC recovery");

            if (charge_mv == 4150) {
                auto gaining = inserting;
                CheckChargingDisplay(gaining, 4200, 900,
                    "charging remains monotonic as its independent voltage model catches up");
                Check(gaining.battery.Percent() > displayed &&
                    gaining.battery.Percent() <= 99 && !gaining.battery.Full(),
                    "display protection still allows real charge progress before hardware completion");
            }

            // A brief STAT interruption changes the voltage phase without
            // ending this cable session or completing the battery charge.
            auto stopped = inserting;
            stopped.SetCharging(false);
            CheckChargingDisplay(stopped, charge_mv - 60, 60,
                "normal charger stopping voltage relaxation cannot lower the display");
            stopped.SetCharging(true);
            CheckChargingDisplay(stopped, charge_mv, 60,
                "resumed charging cannot lower the display after a normal STAT edge");

            for (const bool software_reset : {false, true}) {
                for (const int sampled_seconds : {0, 5, 25, 60}) {
                    auto pending = LaggingDisplay(displayed);
                    pending.SetCable(true);
                    pending.Run(charge_mv, sampled_seconds);
                    auto restarted = Restart(pending, charge_mv, software_reset);
                    Check(restarted.battery.Percent() >= displayed,
                        "charging restart preserves the pre-insertion displayed percentage");
                    CheckChargingDisplay(restarted, charge_mv, 180,
                        "charging restart must not reintroduce the deferred downward display step");
                    Check(restarted.battery.TargetPercent() <= original_model + 0.15f,
                        "charging restart preserves the lower voltage model independently of the display");
                }
                auto colder = Restart(inserting, charge_mv - 50, software_reset);
                Check(colder.battery.TargetPercent() < original_model - 1,
                    "same-phase restart must not absorb real USB voltage loss into a new correction");
            }

            inserting.SetCable(false);
            inserting.Run(4051, 60);
            Check(inserting.battery.TargetPercent() <= original_model + 0.01f,
                "unplug after protected charging cannot convert display debt into capacity");
            for (const int pulse_seconds : {5, 25, 60}) {
                const float before_pulse = inserting.battery.TargetPercent();
                inserting.SetCable(true);
                CheckChargingDisplay(inserting, charge_mv, pulse_seconds,
                    "each quick insertion keeps the displayed percentage from falling while attached");
                inserting.SetCable(false);
                inserting.Run(4051, 60);
                Check(inserting.battery.TargetPercent() <= before_pulse + 0.01f,
                    "quick cable pulses cannot feed protected display capacity back into the model");
            }

            auto actual_loss = LaggingDisplay(displayed);
            actual_loss.SetCable(true);
            actual_loss.Run(charge_mv, 180);
            actual_loss.Run(3900, 900);
            Check(!actual_loss.battery.Full() && actual_loss.battery.Percent() < displayed - 5,
                "unfinished charge must still report sustained real voltage loss on USB");
            actual_loss.Run(3000, 1200);
            Check(actual_loss.battery.Percent() <= 3,
                "charging display protection cannot hide a genuinely exhausted battery");

            // Once a persistent USB voltage decline has released the display
            // floor, restarting at that lower voltage must not recreate it
            // from the remaining display/model gap.
            auto released = LaggingDisplay(displayed);
            released.SetCable(true);
            released.Run(charge_mv, 180);
            released.Run(charge_mv - 50, 180);
            auto released_restart = Restart(released, charge_mv - 50, true);
            released_restart.Run(charge_mv - 50, 1800);
            Check(released_restart.battery.Percent() < displayed &&
                std::abs(released_restart.battery.PrecisePercent() -
                    released_restart.battery.TargetPercent()) < 0.25f,
                "a released USB display floor stays released after restart");
        }
    }

    // A real decline may begin before STAT changes. Stopping the charger must
    // not discard that evidence and pin the old percentage indefinitely.
    auto stopping_during_loss = LaggingDisplay(93);
    stopping_during_loss.SetCable(true);
    stopping_during_loss.Run(4150, 180);
    stopping_during_loss.Run(3900, 60);
    stopping_during_loss.SetCharging(false);
    stopping_during_loss.Run(3850, 1800);
    Check(!stopping_during_loss.battery.DisplayFloorActive() &&
        stopping_during_loss.battery.Percent() < 93 &&
        std::abs(stopping_during_loss.battery.PrecisePercent() -
            stopping_during_loss.battery.TargetPercent()) < 0.25f,
        "STAT stopping during a genuine voltage decline cannot indefinitely preserve the old display floor");

    // Once a fault has released protection, toggling STAT with the cable still
    // connected cannot recreate the floor from the remaining smoothing lag.
    auto restarting_during_loss = LaggingDisplay(93);
    restarting_during_loss.SetCable(true);
    restarting_during_loss.Run(4150, 180);
    restarting_during_loss.Run(3900, 180);
    Check(!restarting_during_loss.battery.DisplayFloorActive(),
        "sustained low USB voltage releases protection before the STAT-toggle fixture");
    restarting_during_loss.SetCharging(false);
    restarting_during_loss.Run(3850, 30);
    restarting_during_loss.SetCharging(true);
    Check(!restarting_during_loss.battery.DisplayFloorActive(),
        "brief stop and restart cannot recreate an already-released charging display floor");
    restarting_during_loss.Run(3900, 1800);
    Check(!restarting_during_loss.battery.DisplayFloorActive() &&
        restarting_during_loss.battery.Percent() < 93 &&
        std::abs(restarting_during_loss.battery.PrecisePercent() -
            restarting_during_loss.battery.TargetPercent()) < 0.25f,
        "resumed charging after a real supply decline still converges to the voltage model");

    // Exact COM6 checkpoint and subsequent minute voltage values: the old
    // estimator deducted the pre-insertion model/display gap as 97 -> 96.
    Device recorded_insertion;
    recorded_insertion.battery.Restore(96.477f, 4051, false, 0, true,
        false, false, 0, 95.534f);
    recorded_insertion.Warm(4051);
    Check(recorded_insertion.battery.Percent() == 97 &&
        std::abs(recorded_insertion.battery.TargetPercent() - 95.534f) < 0.01f,
        "COM6 insertion replay preserves its recorded display and model checkpoint");
    recorded_insertion.SetCable(true);
    for (const int mv : {4196, 4197, 4194}) {
        CheckChargingDisplay(recorded_insertion, mv, 60,
            "COM6 4196/4197/4194 mV replay cannot spend pre-insertion display debt while charging");
        Check(recorded_insertion.battery.Percent() == 97 &&
            !recorded_insertion.battery.Full(),
            "COM6 stable charging replay retains 97 percent without fabricating full charge");
        Check(recorded_insertion.battery.TargetPercent() <= 95.534f + 0.15f,
            "COM6 charging noise cannot feed the protected higher display back into the voltage model");
    }
    Check(recorded_insertion.battery.TargetPercent() <= 95.534f + 0.01f,
        "COM6 insertion replay ends without manufacturing modeled capacity");

    for (const int plugged_mv : {4150, 4200}) {
        for (const int plugged_seconds : {5, 25, 60}) {
            auto repeated_edges = CompletedCharge();
            repeated_edges.SetCable(false);
            repeated_edges.Run(4050, 60);
            repeated_edges.Run(4008, 1800);
            const float model_before = repeated_edges.battery.TargetPercent();
            const float display_before = repeated_edges.battery.PrecisePercent();
            Check(model_before < 99 && model_before > 80,
                "repeated-edge fixture has genuinely depleted below its full anchor");
            for (int cycle = 0; cycle < 10; ++cycle) {
                repeated_edges.SetCable(true);
                repeated_edges.Run(plugged_mv, plugged_seconds);
                repeated_edges.SetCable(false);
                repeated_edges.Run(4008, 60);
                Check(repeated_edges.battery.TargetPercent() <= model_before + 0.01f &&
                    repeated_edges.battery.PrecisePercent() <= display_before + 0.01f,
                    "repeated voltage-only cable pulses cannot create battery capacity");
            }
        }
    }

    // Reproduce the reported failure: after learning a 200 mV insertion rise,
    // the old algorithm restored 60 mV of compensation at charge termination
    // and changed 99% into 89%, despite the cable remaining connected.
    Device charge_cycle;
    charge_cycle.Warm(3800);
    charge_cycle.SetCable(true);
    charge_cycle.Run(4000, 60);
    for (int mv = 4000; mv <= 4200; ++mv) charge_cycle.Run(mv, 10);
    charge_cycle.Run(4200, 1800);
    Check(charge_cycle.battery.Percent() == 99 && !charge_cycle.battery.Full(),
        "active CV charge reaches 99 but cannot itself confirm completion");
    charge_cycle.SetCharging(false);
    for (int i = 0; i < 360; ++i) {
        charge_cycle.Read(4140);
        Check(charge_cycle.battery.Percent() >= 99,
            "normal charge termination must not reproduce the 99-to-89 fall");
        Check(charge_cycle.battery.CorrectionMillivolts() <= 0,
            "stopped charger must not deduct a positive charging correction");
    }
    Check(charge_cycle.battery.Full() && charge_cycle.battery.Percent() == 100,
        "completed charge remains full after its voltage relaxes below 4.15 V");

    Device taper;
    taper.Warm(3800);
    taper.SetCable(true);
    taper.Run(4000, 60);
    taper.Run(4200, 600);
    const int minimum_correction = taper.battery.CorrectionMillivolts();
    taper.Run(4140, 600);
    Check(taper.battery.CorrectionMillivolts() <= minimum_correction,
        "voltage falling during active charge cannot regrow faded compensation");
    Check(taper.battery.TargetPercent() > 95,
        "active charge voltage fall must not be deducted twice");
    Check(!taper.battery.Full(), "active STAT never confirms full from voltage alone");

    // The device log had 4148 mV, a stale 117 mV deduction and an 86% display.
    for (const bool software_reset : {false, true}) {
        Device logged_state;
        logged_state.usb = true;
        logged_state.battery.Restore(86, 4148, true, 117, software_reset, false, false);
        logged_state.Warm(4148);
        Check(logged_state.battery.CorrectionMillivolts() == 0 &&
            logged_state.battery.TargetPercent() > 96,
            "inactive startup rejects the stale charging correction seen on COM6");
        logged_state.Run(4148, 290);
        Check(!logged_state.battery.Full(), "inactive boot needs sustained evidence before inferring full");
        logged_state.Run(4148, 310);
        Check(logged_state.battery.Full() && logged_state.battery.Percent() == 100 &&
            logged_state.battery.FullInferred(),
            "already-stopped charger at boot is recovered with explicit inferred provenance");
    }

    // ETA6098's low CV limit is 4.16 V; its recharge offset is 160 mV.
    for (const int cv_mv : {4160, 4200}) {
        Device maintenance;
        maintenance.usb = maintenance.charging = true;
        maintenance.Warm(cv_mv);
        maintenance.Run(cv_mv, 600);
        maintenance.SetCharging(false);
        maintenance.Run(cv_mv - 160, 600);
        Check(maintenance.battery.Full() && maintenance.battery.Percent() == 100,
            "low-tolerance CV completion allows the specified recharge voltage window");
        for (int cycle = 0; cycle < 4; ++cycle) {
            maintenance.SetCharging(true);
            maintenance.Run(cv_mv - 160, 120);
            Check(maintenance.battery.Full() && maintenance.battery.Percent() == 100,
                "automatic recharge cannot erase a completed-charge session");
            maintenance.Run(cv_mv, 120);
            maintenance.SetCharging(false);
            maintenance.Run(cv_mv - 160, 120);
            Check(maintenance.battery.Full() && maintenance.battery.Percent() == 100,
                "repeated stop-and-recharge cycles retain full charge");
        }
        for (const bool software_reset : {false, true}) {
            for (const bool recharging : {false, true}) {
                maintenance.SetCharging(recharging);
                for (const int sampled_seconds : {0, 5, 25, 60}) {
                    auto phase = maintenance;
                    phase.Run(cv_mv - 160, sampled_seconds);
                    auto restarted = Restart(phase, cv_mv - 160, software_reset);
                    Check(restarted.battery.Full() && restarted.battery.Percent() == 100,
                        "restart during a full maintenance phase preserves 100 percent");
                    restarted.Run(cv_mv - 160, 600);
                    Check(restarted.battery.Full() && restarted.battery.Percent() == 100,
                        "restored completion survives normal recharge-window voltage");
                }
            }
        }
        maintenance.SetCable(false);
        Check(!maintenance.battery.Full(), "unplug ends the full-charge session immediately");
    }

    for (const bool charging : {false, true}) {
        auto weak_supply = charge_cycle;
        weak_supply.SetCharging(charging);
        weak_supply.Run(3900, 60);
        Check(weak_supply.battery.Full(), "brief load sag does not cancel confirmed full charge");
        weak_supply.Run(3900, 180);
        Check(!weak_supply.battery.Full() && weak_supply.battery.SupplyLow(),
            "sustained low battery on USB cancels full and reports inadequate supply");
        weak_supply.Run(3900, 600);
        Check(weak_supply.battery.Percent() < 80,
            "USB presence must not hide real sustained battery depletion");
        weak_supply.SetCharging(false);
        weak_supply.Run(3900, 600);
        Check(!weak_supply.battery.Full(), "lost full state is not reasserted from stale CV history");
    }

    auto critical_supply = charge_cycle;
    critical_supply.Run(3000, 90);
    Check(!critical_supply.battery.Full() && critical_supply.battery.SupplyLow(),
        "critical battery voltage exits full sooner than the normal low-supply timeout");
    critical_supply.Run(3000, 900);
    Check(critical_supply.battery.Percent() <= 3,
        "critical voltage cannot be hidden by the USB full-charge latch");

    Device stopped_early;
    stopped_early.usb = stopped_early.charging = true;
    stopped_early.Warm(4100);
    stopped_early.Run(4100, 600);
    stopped_early.SetCharging(false);
    stopped_early.Run(4000, 600);
    Check(!stopped_early.battery.Full() && stopped_early.battery.Percent() < 100,
        "STAT stopping before near-CV evidence is not completion");

    Device short_topup;
    short_topup.usb = short_topup.charging = true;
    short_topup.Warm(4160);
    short_topup.Run(4160, 20);
    short_topup.SetCharging(false);
    short_topup.Run(4040, 600);
    Check(short_topup.battery.Full() && short_topup.battery.Percent() == 100,
        "short near-full top-up ending after the 16-second charger blanking time can complete");

    Device brief_noise;
    brief_noise.usb = brief_noise.charging = true;
    brief_noise.Warm(4100);
    brief_noise.Read(4160);
    brief_noise.SetCharging(false);
    brief_noise.Run(4040, 600);
    Check(!brief_noise.battery.Full(), "single noisy sample without filtered near-CV evidence is not completion");

    Device old_cv;
    old_cv.usb = old_cv.charging = true;
    old_cv.Warm(4200);
    old_cv.Run(4200, 120);
    old_cv.Run(4050, 600);
    old_cv.SetCharging(false);
    old_cv.Run(4050, 600);
    Check(!old_cv.battery.Full(), "old near-CV evidence expires before an unrelated stop");

    Device plugged_full;
    plugged_full.Warm(4148);
    plugged_full.usb = true;
    plugged_full.charging = false;
    plugged_full.Run(4148, 290);
    Check(!plugged_full.battery.Full(), "new inactive USB session also requires its full observation interval");
    plugged_full.Run(4148, 310);
    Check(plugged_full.battery.Full() && plugged_full.battery.Percent() == 100 &&
        plugged_full.battery.FullInferred(), "plugging an already-full battery infers completion even without a STAT pulse");

    Device pending_completion;
    pending_completion.usb = pending_completion.charging = true;
    pending_completion.Warm(4160);
    pending_completion.Run(4160, 120);
    pending_completion.SetCharging(false);
    pending_completion.Run(4020, 30);
    Check(!pending_completion.battery.Full() && pending_completion.battery.RecentNearCvMillivolts() >= 4140,
        "pending completion retains recent CV evidence before the full confirmation interval");
    auto pending_restart = Restart(pending_completion, 4020, true);
    pending_restart.Run(4020, 600);
    Check(pending_restart.battery.Full() && pending_restart.battery.Percent() == 100,
        "software restart during EOC relaxation retains recent charge completion evidence");
    auto pending_cold = Restart(pending_completion, 4020, false);
    pending_cold.Run(4020, 600);
    Check(!pending_cold.battery.Full(), "cold boot does not reuse undated unconfirmed CV evidence");

    Device missing_samples;
    missing_samples.usb = missing_samples.charging = true;
    missing_samples.Warm(4200);
    missing_samples.Run(4200, 120);
    missing_samples.SetCharging(false);
    missing_samples.Run(4140, 60);
    missing_samples.Read(4500, 60);
    missing_samples.Read(4140);
    Check(!missing_samples.battery.Full(), "invalid ADC period cannot complete the stable EOC interval");
    missing_samples.Run(4140, 115);
    Check(!missing_samples.battery.Full(), "EOC stability interval restarts after missing samples");
    missing_samples.Run(4140, 20);
    Check(missing_samples.battery.Full(), "valid samples can confirm completion after recovering from a short ADC gap");

    Device missing_at_boot;
    missing_at_boot.usb = true;
    missing_at_boot.Warm(4148);
    missing_at_boot.Run(4148, 290);
    missing_at_boot.Read(4500, 60);
    missing_at_boot.Read(4148);
    missing_at_boot.Run(4148, 290);
    Check(!missing_at_boot.battery.Full(), "invalid ADC time cannot complete inactive-session inference");
    missing_at_boot.Run(4148, 20);
    Check(missing_at_boot.battery.Full(), "inactive-session inference resumes only after a fresh stable interval");

    Device stale_topup;
    stale_topup.usb = stale_topup.charging = true;
    stale_topup.Warm(4160);
    stale_topup.Run(4160, 20);
    stale_topup.Read(4500, 60);
    stale_topup.SetCharging(false);
    stale_topup.Run(4040, 600);
    Check(!stale_topup.battery.Full(), "STAT edge cannot use an old voltage sample as short-top-up evidence");

    for (const int sampled_seconds : {0, 5, 25, 60}) {
        Device interrupted;
        interrupted.Warm(3800);
        interrupted.SetCable(true);
        interrupted.Run(4000, 600);
        interrupted.SetCharging(false);
        interrupted.Run(3900, sampled_seconds);
        Check(interrupted.battery.VoltageWasCharging() == (sampled_seconds == 0),
            "stopping-phase checkpoint records the state of its voltage sample");
        auto stopped_restart = Restart(interrupted, 3900, true);
        Check(std::abs(stopped_restart.battery.Percent() - interrupted.battery.Percent()) <= 1,
            "software restart during charge stopping preserves displayed capacity");
        Check(stopped_restart.battery.CorrectionMillivolts() <= 0,
            "restart in a stopped phase cannot restore positive charging compensation");
        stopped_restart.Run(3900, 600);
        Check(!stopped_restart.battery.Full(), "low-voltage phase restart cannot fabricate charge completion");

        interrupted.Run(3900, 600);
        interrupted.SetCharging(true);
        interrupted.Run(4000, sampled_seconds);
        Check(interrupted.battery.VoltageWasCharging() == (sampled_seconds != 0),
            "restarting-charge checkpoint records the state of its voltage sample");
        auto active_restart = Restart(interrupted, 4000, true);
        Check(std::abs(active_restart.battery.Percent() - interrupted.battery.Percent()) <= 1,
            "software restart during charging restart preserves displayed capacity");
        active_restart.Run(4000, 600);
        Check(!active_restart.battery.Full(), "resumed low-voltage charging cannot fabricate completion");
    }

    Device ramp;
    ramp.Warm(4200);
    int last_percent = ramp.battery.Percent();
    for (int i = 1; i <= 2880; ++i) {
        ramp.Read(4200 - i * 1200 / 2880);
        const int percent = ramp.battery.Percent();
        Check(percent <= last_percent && last_percent - percent <= 1, "long discharge is smooth and monotonic");
        last_percent = percent;
    }
    Check(ramp.battery.Percent() < 3, "sustained empty battery is not hidden by smoothing");
    Check(!ramp.battery.Valid(ramp.now + 61000), "stale ADC data becomes unknown");
    ramp.Read(3050, 65);
    Check(ramp.battery.Valid(ramp.now), "ADC recovery restores validity without reboot");
    Device empty;
    empty.Warm(3900);
    empty.Run(3000, 600);
    Check(empty.battery.Percent() <= 3, "persistent critical voltage escapes normal smoothing");

    if (failures != 0) {
        std::cerr << failures << " battery regression checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: OCV, fast USB and stable STAT debounce, noise, cable and STAT transitions, checkpoint restarts, "
        "93-to-92/97-to-96 insertion, 99-to-89 and full-unplug regressions, discharge anchor persistence, charge completion/recharge, "
        "weak supply, discharge and ADC recovery\n";
}

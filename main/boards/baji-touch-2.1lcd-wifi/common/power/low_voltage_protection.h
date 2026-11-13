#pragma once

#include <cstdint>

namespace baji {

class LowVoltageProtection {
public:
    static constexpr int kNormalCutoffMv = 3400;
    static constexpr int kEmergencyCutoffMv = 3300;
    static constexpr int kRestartMv = 3550;
    static constexpr int kChargingRestartMv = 3750;
    static constexpr int kChargingRestartStableMs = 15000;

    bool Update(bool sample_valid, bool usb, int millivolts,
                int64_t sample_at_ms, int64_t now_ms) {
        if (usb) {
            Reset();
            return false;
        }
        if (!sample_valid || sample_at_ms < 0 ||
            now_ms - sample_at_ms > 2500 || sample_at_ms < last_sample_at_ms_) {
            Reset();
            return false;
        }
        if (sample_at_ms == last_sample_at_ms_) return false;
        if (last_sample_at_ms_ >= 0 && sample_at_ms - last_sample_at_ms_ > 2500) Reset();
        last_sample_at_ms_ = sample_at_ms;

        if (millivolts <= kNormalCutoffMv) {
            if (low_since_ms_ < 0) low_since_ms_ = sample_at_ms;
        } else {
            low_since_ms_ = -1;
        }
        if (millivolts <= kEmergencyCutoffMv) {
            if (emergency_since_ms_ < 0) emergency_since_ms_ = sample_at_ms;
        } else {
            emergency_since_ms_ = -1;
        }
        return (low_since_ms_ >= 0 && sample_at_ms - low_since_ms_ >= 15000) ||
            (emergency_since_ms_ >= 0 && sample_at_ms - emergency_since_ms_ >= 3000);
    }

    void Reset() {
        low_since_ms_ = emergency_since_ms_ = last_sample_at_ms_ = -1;
    }

private:
    int64_t low_since_ms_ = -1;
    int64_t emergency_since_ms_ = -1;
    int64_t last_sample_at_ms_ = -1;
};

} // namespace baji

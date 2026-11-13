#pragma once

#include <cstddef>
#include <cstdint>

namespace baji::touch {

struct Sample {
    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed = false;
    int64_t last_point_us = 0;
    bool cancelled = false;
};

// A short FIFO preserves press/move/release ordering while the LVGL task is
// busy rendering a frame.  The sampler owns the producer side and the LVGL
// task consumes it under the same critical section.
class SampleBuffer {
public:
    static constexpr size_t kCapacity = 16;
    static constexpr uint8_t kNoContactConfirmations = 2;

    bool Push(uint16_t x, uint16_t y, bool pressed, int64_t now_us) {
        // A timed-out or overflowed gesture is cancelled for LVGL. Keep
        // ignoring the same physical contact until its UP/empty report arrives
        // so a late CONTACT cannot start a ghost tap.
        if (ignore_until_release_) {
            no_contact_count_ = 0;
            if (!pressed) {
                current_.x = x;
                current_.y = y;
                current_.pressed = false;
                current_.last_point_us = now_us;
                ignore_until_release_ = false;
            }
            return false;
        }

        no_contact_count_ = 0;
        const bool was_pressed = current_.pressed;
        const bool moved = x != current_.x || y != current_.y;
        if (pressed) {
            current_ = {x, y, true, now_us, false};
        } else {
            // Use the controller's final UP coordinate.  It can be newer than
            // the last CONTACT sample when LVGL or the shared bus was busy.
            current_.x = x;
            current_.y = y;
            current_.pressed = false;
            current_.last_point_us = now_us;
            current_.cancelled = false;
        }

        bool overflow = false;
        if (!was_pressed && pressed) {
            overflow = Enqueue(current_, true);
        } else if (was_pressed && !pressed) {
            overflow = Enqueue(current_, false);
        } else if (pressed && moved) {
            // Coalesce only queued moves; never replace the press origin.
            if (size_ != 0 && Tail().sample.pressed && !Tail().press_start) {
                Tail().sample = current_;
            } else {
                overflow = Enqueue(current_, false);
            }
        } else if (pressed && size_ != 0 && Tail().sample.pressed) {
            Tail().sample.last_point_us = now_us;
        }
        if (overflow) Cancel(now_us);
        return overflow;
    }

    // Some CST836U firmware reports UP with an event code and one point;
    // other revisions first report a zero-point frame. Require two consecutive
    // empty samples for the latter so one transient bus/controller gap cannot
    // split an active drag. Explicit UP frames still go through Push() at once.
    bool ObserveNoContact(int64_t now_us) {
        if (ignore_until_release_) {
            if (++no_contact_count_ < kNoContactConfirmations) {
                return false;
            }
            no_contact_count_ = 0;
            ignore_until_release_ = false;
            current_.pressed = false;
            current_.last_point_us = now_us;
            return true;
        }
        if (!current_.pressed) {
            no_contact_count_ = 0;
            return false;
        }
        if (++no_contact_count_ < kNoContactConfirmations) {
            return false;
        }
        no_contact_count_ = 0;
        Push(current_.x, current_.y, false, now_us);
        return true;
    }

    bool Expire(int64_t now_us, int64_t timeout_us) {
        if (!current_.pressed || now_us < current_.last_point_us) {
            return false;
        }
        const uint64_t age_us = static_cast<uint64_t>(now_us) -
                                static_cast<uint64_t>(current_.last_point_us);
        if (timeout_us > 0 && age_us < static_cast<uint64_t>(timeout_us)) {
            return false;
        }
        // No report for a whole timeout means the gesture was lost. End it
        // without generating a normal LVGL release, which could click the
        // object under a stale or corrupted coordinate.
        Cancel(now_us);
        return true;
    }

    Sample Pop() {
        if (size_ != 0) {
            output_ = entries_[head_].sample;
            head_ = (head_ + 1) % kCapacity;
            --size_;
            const Sample sample = output_;
            // Cancellation is an edge for the LVGL consumer, not a persistent
            // state. Do not reset the input device again on every empty read.
            output_.cancelled = false;
            return sample;
        }
        return output_;
    }

    const Sample& Current() const { return current_; }
    bool Pending() const { return size_ != 0; }

    void Clear() {
        head_ = 0;
        size_ = 0;
        current_ = {};
        output_ = {};
        no_contact_count_ = 0;
        ignore_until_release_ = false;
    }

    // Used when the display sleeps while a finger may still be down. The
    // consumer is reset separately, so do not queue a cancellation sample.
    void SuppressUntilRelease() {
        current_.pressed = false;
        no_contact_count_ = 0;
        ignore_until_release_ = true;
    }

    void Cancel(int64_t now_us) {
        current_.pressed = false;
        current_.last_point_us = now_us;
        current_.cancelled = false;
        no_contact_count_ = 0;
        ignore_until_release_ = true;
        head_ = 0;
        size_ = 1;
        entries_[0] = {{current_.x, current_.y, false, now_us, true}, false};
    }

private:
    struct Entry {
        Sample sample;
        bool press_start = false;
    };

    Entry& Tail() { return entries_[(head_ + size_ - 1) % kCapacity]; }

    bool Enqueue(const Sample& sample, bool press_start) {
        if (size_ == kCapacity) {
            return true;
        }
        entries_[(head_ + size_) % kCapacity] = {sample, press_start};
        ++size_;
        return false;
    }

    Entry entries_[kCapacity]{};
    size_t head_ = 0;
    size_t size_ = 0;
    Sample current_{};
    Sample output_{};
    uint8_t no_contact_count_ = 0;
    bool ignore_until_release_ = false;
};

}  // namespace baji::touch

#include "../hardware/touch_sample_buffer.h"
#include "../hardware/baji_touch_cst836u_protocol.h"

#include <iostream>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestOrderedSamplesAndCoalescing() {
    baji::touch::SampleBuffer samples;
    Check(!samples.Push(10, 20, true, 1000), "DOWN queues a pressed sample");
    Check(!samples.Push(15, 25, true, 2000), "first CONTACT queues a move");
    Check(!samples.Push(30, 40, true, 3000), "later CONTACT coalesces the queued move");
    Check(!samples.Push(31, 41, false, 4000), "UP queues a release with its final coordinate");

    const auto down = samples.Pop();
    const auto move = samples.Pop();
    const auto up = samples.Pop();
    Check(down.pressed && down.x == 10 && down.y == 20, "press origin is preserved");
    Check(move.pressed && move.x == 30 && move.y == 40, "coalesced move keeps the newest point");
    Check(!up.pressed && up.x == 31 && up.y == 41, "release uses the final UP coordinate");
    Check(!samples.Pending(), "ordered samples drain completely");
}

void TestCst836uFrames() {
    using baji::touch::cst836u::DecodeFrame;
    using baji::touch::cst836u::Event;
    using baji::touch::cst836u::FrameStatus;
    using baji::touch::cst836u::Point;

    Point point{};
    const uint8_t down[5] = {1, 0x01, 0x23, 0x00, 0xab};
    Check(DecodeFrame(down, 359, 359, point) == FrameStatus::Point,
          "DOWN frame decodes from the five documented registers");
    Check(point.event == Event::Down && point.x == 0x123 && point.y == 0x0ab,
          "event bits are separated from 12-bit coordinates");

    const uint8_t up[5] = {1, 0x41, 0x23, 0x00, 0xab};
    Check(DecodeFrame(up, 359, 359, point) == FrameStatus::Point && point.event == Event::Up,
          "UP with one retained coordinate remains an explicit release event");

    const uint8_t contact[5] = {1, 0x81, 0x23, 0x00, 0xab};
    Check(DecodeFrame(contact, 359, 359, point) == FrameStatus::Point &&
          point.event == Event::Contact, "CONTACT event is preserved");

    const uint8_t empty[5] = {0, 0xc0, 0xff, 0x0f, 0xff};
    Check(DecodeFrame(empty, 359, 359, point) == FrameStatus::NoContact,
          "empty frame does not interpret stale event or coordinate bytes");
    const uint8_t multiple[5] = {2, 0x01, 0x23, 0x00, 0xab};
    Check(DecodeFrame(multiple, 359, 359, point) == FrameStatus::InvalidCount,
          "unsupported point count is rejected");
    const uint8_t reserved_event[5] = {1, 0xc1, 0x23, 0x00, 0xab};
    Check(DecodeFrame(reserved_event, 359, 359, point) == FrameStatus::InvalidEvent,
          "reserved event code is rejected");
    const uint8_t outside_panel[5] = {1, 0x01, 0x68, 0x00, 0xab};
    Check(DecodeFrame(outside_panel, 359, 359, point) == FrameStatus::OutOfBounds,
          "out-of-panel coordinate is rejected before mirroring");
}

void TestEmptyFrameDebounce() {
    baji::touch::SampleBuffer samples;
    samples.Push(100, 110, true, 1000);
    Check(!samples.ObserveNoContact(2000), "one empty frame does not split a drag");
    Check(samples.Current().pressed, "one empty frame keeps contact active");
    samples.Push(105, 115, true, 3000);
    Check(!samples.ObserveNoContact(4000), "a contact frame clears the empty-frame streak");
    Check(samples.ObserveNoContact(5000), "two consecutive empty frames release the contact");
    Check(!samples.Current().pressed, "confirmed empty frames clear pressed state");
    Check(samples.Pop().pressed, "debounced release preserves the original press");
    Check(samples.Pop().pressed, "the contact after a transient gap stays in the gesture");
    Check(!samples.Pop().pressed, "debounced release reaches LVGL");
}

void TestTimeoutCancelsAndWaitsForRelease() {
    baji::touch::SampleBuffer samples;
    samples.Push(50, 60, true, 1000);
    Check(!samples.Expire(100999, 100000), "timeout does not fire early");
    Check(samples.Expire(101000, 100000), "lost contact expires at the timeout");
    const auto cancelled = samples.Pop();
    Check(!cancelled.pressed && cancelled.cancelled, "timeout emits an LVGL cancellation edge");
    Check(!samples.Push(70, 80, true, 102000), "late CONTACT is ignored after cancellation");
    Check(!samples.Current().pressed && !samples.Pending(), "late CONTACT cannot restart the gesture");
    Check(!samples.ObserveNoContact(103000), "one empty frame does not unlock a cancelled gesture");
    Check(samples.ObserveNoContact(103500), "confirmed physical release unlocks the cancelled gesture");
    Check(!samples.Push(70, 80, true, 104000), "a fresh DOWN is accepted after physical release");
    Check(samples.Pop().pressed, "fresh gesture reaches LVGL");
}

void TestSleepSuppressesHeldContact() {
    baji::touch::SampleBuffer samples;
    samples.Push(10, 20, true, 1000);
    samples.Clear();
    samples.SuppressUntilRelease();
    Check(!samples.Push(11, 21, true, 2000), "held contact after wake remains suppressed");
    Check(!samples.ObserveNoContact(3000), "one empty wake sample does not clear suppression");
    Check(samples.ObserveNoContact(4000), "confirmed release clears wake suppression");
    samples.Push(12, 22, true, 5000);
    Check(samples.Current().pressed && samples.Pending(), "fresh touch works after wake release");
}

void TestOverflowCancelsAndWaitsForRelease() {
    baji::touch::SampleBuffer samples;
    bool overflowed = false;
    int64_t now = 1000;
    for (int tap = 0; tap < 9; ++tap) {
        overflowed = samples.Push(static_cast<uint16_t>(tap), 1, true, now++);
        if (overflowed) break;
        samples.Push(static_cast<uint16_t>(tap), 1, false, now++);
    }
    Check(overflowed, "unconsumed taps are bounded by the FIFO capacity");
    const auto cancelled = samples.Pop();
    Check(cancelled.cancelled && !cancelled.pressed, "overflow discards stale events and cancels LVGL");
    samples.Push(20, 20, true, now++);
    Check(!samples.Current().pressed && !samples.Pending(), "overflowed physical contact stays suppressed");
    samples.Push(20, 20, false, now++);
    samples.Push(21, 21, true, now++);
    Check(samples.Current().pressed && samples.Pending(), "new DOWN works after the overflowed contact lifts");
}

}  // namespace

int main() {
    TestCst836uFrames();
    TestOrderedSamplesAndCoalescing();
    TestEmptyFrameDebounce();
    TestTimeoutCancelsAndWaitsForRelease();
    TestSleepSuppressesHeldContact();
    TestOverflowCancelsAndWaitsForRelease();
    if (failures == 0) {
        std::cout << "Touch sample buffer tests passed\n";
        return 0;
    }
    std::cerr << failures << " touch sample buffer test(s) failed\n";
    return 1;
}

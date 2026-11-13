#pragma once

#include <cstdint>

namespace baji::touch::cst836u {

enum class Event : uint8_t {
    Down = 0,
    Up = 1,
    Contact = 2,
};

enum class FrameStatus : uint8_t {
    Point,
    NoContact,
    InvalidCount,
    InvalidEvent,
    OutOfBounds,
};

struct Point {
    Event event = Event::Down;
    uint16_t x = 0;
    uint16_t y = 0;
};

// CST836U registers 0x02..0x06: count, XH, XL, YH, YL.  The event is in
// XH[7:6], while only XH[3:0] belongs to the coordinate.
inline FrameStatus DecodeFrame(const uint8_t (&frame)[5], uint16_t max_x,
                               uint16_t max_y, Point& point) {
    const uint8_t count = frame[0] & 0x0f;
    if (count == 0) return FrameStatus::NoContact;
    if (count != 1) return FrameStatus::InvalidCount;

    const uint8_t event = (frame[1] >> 6) & 0x03;
    if (event > static_cast<uint8_t>(Event::Contact)) return FrameStatus::InvalidEvent;

    const uint16_t x = static_cast<uint16_t>(((frame[1] & 0x0f) << 8) | frame[2]);
    const uint16_t y = static_cast<uint16_t>(((frame[3] & 0x0f) << 8) | frame[4]);
    if (x > max_x || y > max_y) return FrameStatus::OutOfBounds;

    point.event = static_cast<Event>(event);
    point.x = x;
    point.y = y;
    return FrameStatus::Point;
}

}  // namespace baji::touch::cst836u

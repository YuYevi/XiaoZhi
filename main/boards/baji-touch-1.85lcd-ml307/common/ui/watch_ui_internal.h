#pragma once

#include <lvgl.h>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

// Shared UI styling and helpers. Page state remains owned by WatchUi.
namespace baji::ui {
inline constexpr uint32_t kBg = 0x09090f, kAmber = 0xf59e0b, kTeal = 0x2dd4bf;
inline constexpr uint32_t kSurface = 0x191920, kMuted = 0x9898a5, kText = 0xf2f1f6;
inline constexpr uint32_t kRose = 0xf9a8d4, kGreen = 0x6ee7b7, kBlue = 0x93c5fd, kPurple = 0xc4b5fd;
inline constexpr const char* kDays[] = {"日", "一", "二", "三", "四", "五", "六"};

size_t NextCharacter(const std::string& s, size_t from);
size_t CharacterCount(const std::string& s);
void SetLabelText(lv_obj_t* label, const char* text);
void SingleLine(lv_obj_t* label);
uint32_t BlendColor(uint32_t a, uint32_t b, float amount);
std::tm Date(int64_t now);
std::string Number(int n);
std::string CountdownText(uint32_t seconds, bool hours);
std::string AlarmRepeat(uint8_t weekdays, bool english);
void SetMenuPlateScale(void* obj, int32_t scale);
void AnimateMenuPlate(lv_obj_t* plate, int scale, int duration);
void AnimateY(lv_obj_t* obj, int from, int to, int duration = 220);
}  // namespace baji::ui

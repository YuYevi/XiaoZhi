#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"

using namespace baji::ui;

namespace {
bool Light(uint32_t c) {
    return ((c >> 16) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) > 160000;
}
}  // namespace

void WatchUi::RenderLightstick() {
    PageTitle("应援灯", kPurple);
    auto* preview = Box(content_, 139, 91, 82, 82, light_color_, LV_RADIUS_CIRCLE);
    Border(preview, 0xffffff, 50);
    auto* favorite = Button(content_, 118, 183, 124, 34, 0x28233a, 17, [this] {
        light_color_ = 0x9a91f2;
        Render();
    });
    Border(favorite, kPurple, light_color_ == 0x9a91f2 ? 120 : 40);
    lv_obj_align(Icon(favorite, "star", 0, 0, 18, kPurple), LV_ALIGN_LEFT_MID, 14, 0);
    Text(favorite, "马嘉祺", 43, 6, 71, 14, 0xdfd6f4, 400, false, 22);
    Text(content_, "基础颜色", 110, 228, 140, 12, kMuted, 400, true, 18);
    constexpr uint32_t colors[] = {0xff0000, 0xff6600, 0xffdd00, 0x00cc44,
                                   0x00bbff, 0x8800ff, 0xff00bb, 0xffffff};
    for (int i = 0; i < 8; ++i) {
        const uint32_t color = colors[i];
        const bool selected = light_color_ == color;
        auto* hit = Button(content_, 41 + i * 35, 250, 32, 36, kBg, 16, [this, color] {
            light_color_ = color;
            Render();
        });
        auto* swatch = Box(hit, 2, 4, 28, 28, color, 14);
        Border(swatch, 0xffffff, selected ? 255 : 45, selected ? 2 : 1);
        if (selected) CenteredIcon(swatch, "check", 16, Light(color) ? 0x22222a : 0xffffff);
    }
    auto* apply = Button(content_, 110, 301, 140, 38, 0x413656, 19, [this] {
        CloseModal();
        light_applied_ = true;
        Emit(Action::SetFlashlight, 1);
        ResetMenuInteraction();
        modal_ = Button(root_, 0, 0, 360, 360, light_color_, 0, [this] {
            light_applied_ = false;
            CloseModal();
            Emit(Action::SetFlashlight, 0);
        });
        Text(modal_, "点击任意处退出", 80, 169, 200, 14,
             Light(light_color_) ? 0x292932 : 0xffffff, 400, true, 22);
        UpdateAnimationTimer();
    }, "点亮应援灯", 14);
    Border(apply, kPurple, 90);
}

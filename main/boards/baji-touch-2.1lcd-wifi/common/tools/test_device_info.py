"""Check the production device-info layout with host widget substitutes."""

from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest


COMMON = Path(__file__).resolve().parents[1]

PREAMBLE = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
constexpr uint32_t kBlue = 0x7eb8f2, kMuted = 0x777777, kText = 0xffffff;
constexpr uint32_t kPurple = 0x9977aa, kRose = 0xdd8899, kSurface = 0x19191e;
constexpr int LV_ALIGN_RIGHT_MID = 1, LV_TEXT_ALIGN_RIGHT = 2;
struct lv_obj_t {
    lv_obj_t* parent;
    int x, y, width, height, align = 0, radius;
    uint32_t color;
    std::string kind, text;
    std::function<void()> action;
};
std::vector<std::unique_ptr<lv_obj_t>> objects;
lv_obj_t* Add(lv_obj_t* parent, int x, int y, int width, int height, uint32_t color,
              const char* kind, const char* text = "", int radius = 0) {
    auto node = std::make_unique<lv_obj_t>();
    node->parent = parent; node->x = x; node->y = y;
    node->width = width; node->height = height; node->color = color;
    node->kind = kind; node->text = text; node->radius = radius;
    auto* result = node.get(); objects.push_back(std::move(node)); return result;
}
lv_obj_t* Find(const std::string& text) {
    for (const auto& obj : objects) if (obj->text == text) return obj.get();
    return nullptr;
}
void SingleLine(lv_obj_t*) {}
void lv_obj_align(lv_obj_t*, int, int, int) {}
void lv_obj_set_style_text_align(lv_obj_t* node, int align, int) { node->align = align; }
uint32_t BlendColor(uint32_t, uint32_t color, float) { return color ^ 1; }
class WatchUi {
public:
    enum class Page { Settings, DisplaySettings, DeviceInfo };
    enum class Action { CheckUpdate, FactoryReset };
    struct Settings { int language = 0; };
    struct Snapshot { Settings settings; std::string storage_error; } snapshot_;
    struct Device {
        std::string firmware = "V9.8.7", mac_address = "12:34:56:ab:cd:ef";
    } device_;
    lv_obj_t *content_ = nullptr, *page_scroll_ = nullptr;
    Page page_ = Page::Settings;
    void Navigate(Page page) { page_ = page; }
    void SaveSettings(Settings) {}
    void Confirm(const char*, const char*, std::function<void()>) {}
    void Emit(Action) {}
    void PageTitle(const char* text, uint32_t color) {
        Add(content_, 104, 50, 152, 24, color, "title", text);
    }
    lv_obj_t* ScrollList(lv_obj_t* parent, int x, int y, int w, int h) {
        return Add(parent, x, y, w, h, 0, "list");
    }
    lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color,
                   int radius = 0, int = 255) {
        return Add(parent, x, y, w, h, color, "box", "", radius);
    }
    lv_obj_t* Text(lv_obj_t* parent, const char* text, int x, int y, int w,
                    float size = 12, uint32_t color = kText, int = 400,
                    bool = false, float line = 0, float = 12) {
        return Add(parent, x, y, w, line ? line : size * 1.5f, color, "text", text);
    }
    lv_obj_t* Button(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color,
                      int radius, std::function<void()> action) {
        auto* node = Add(parent, x, y, w, h, color, "button", "", radius);
        node->action = std::move(action); return node;
    }
    void Border(lv_obj_t*, uint32_t, int) {}
    lv_obj_t* Icon(lv_obj_t* parent, const char* name, int x, int y, int size, uint32_t color) {
        return Add(parent, x, y, size, size, color, "icon", name);
    }
    lv_obj_t* CenteredIcon(lv_obj_t* parent, const char* name, int size, uint32_t color) {
        return Icon(parent, name, 0, 0, size, color);
    }
    lv_obj_t* SettingsRow(lv_obj_t*, int, const char*, const char*, uint32_t,
                          std::function<void()>);
    void RenderSettings();
    void RenderDeviceInfo();
};
'''

CASES = r'''
int main() {
    WatchUi ui;
    ui.RenderSettings();
    assert(!Find("WLAN") && !Find("关于手表"));
    assert(Find("设备信息") && Find("恢复出厂设置"));
    auto* info = Find("info");
    assert(info && info->color == kBlue);
    assert(info->parent->color == BlendColor(kSurface, kBlue, .14f));
    assert(info->parent->radius == 9);
    assert(info->parent->width == 28 && info->parent->height == 28);
    assert(info->parent->color == Find("download")->parent->color);
    assert(info->parent->radius == Find("download")->parent->radius);
    info->parent->parent->action();
    assert(ui.page_ == WatchUi::Page::DeviceInfo);
    assert(Find("sun")->color == kPurple);
    assert(Find("trash-2")->color == kBlue);
    assert(Find("trash-2")->parent->parent->color == kSurface);

    objects.clear(); ui.RenderDeviceInfo();
    const char* labels[] = {"系统版本：", "主控芯片：", "屏幕：", "唤醒词：", "MAC地址："};
    const char* values[] = {"V9.8.7", "ESP32-S3", "2.1寸屏幕", "你好小智", "12:34:56:ab:cd:ef"};
    assert(!Find("SD卡：") && !Find("未挂载"));
    assert(ui.page_scroll_->width == 240 && ui.page_scroll_->height == 210);
    for (int i = 0; i < 5; ++i) {
        const auto *label = Find(labels[i]), *value = Find(values[i]);
        assert(label && value && value->align == LV_TEXT_ALIGN_RIGHT);
        assert(label->y == i * 42 + 10 && value->y == label->y);
        assert(label->x + label->width < value->x);
        assert(value->x + value->width <= ui.page_scroll_->width);
        assert(value->y + value->height <= ui.page_scroll_->height);
    }
    int dividers = 0;
    for (const auto& obj : objects) if (obj->kind == "box" && obj->height == 1) ++dividers;
    assert(dividers == 4);
    objects.clear(); ui.device_.firmware.clear(); ui.RenderDeviceInfo();
    assert(Find("--"));
    objects.clear(); ui.device_.firmware = "V9.8.7"; ui.device_.mac_address.clear();
    ui.RenderDeviceInfo(); assert(Find("MAC地址：") && Find("--"));
}
'''


class DeviceInfoTest(unittest.TestCase):
    def test_production_rows_and_icon_styles(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A host C++ compiler is required")
        settings = (COMMON / "ui/pages/settings_page.cc").read_text(encoding="utf-8")
        settings = settings[settings.index("void WatchUi::RenderSettings()") :]
        widgets = (COMMON / "ui/watch_ui_widgets.cc").read_text(encoding="utf-8")
        row = widgets[widgets.index("lv_obj_t* WatchUi::SettingsRow(") :
                      widgets.index("void WatchUi::RefreshClock(")]
        with tempfile.TemporaryDirectory(prefix="watch-device-info-") as temp:
            source = Path(temp) / "test.cc"
            source.write_text(PREAMBLE + row + settings + CASES, encoding="utf-8")
            executable = Path(temp) / "test.exe"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_navigation_and_translations(self):
        source = (COMMON / "ui/watch_ui.cc").read_text(encoding="utf-8")
        self.assertIn("case Page::DeviceInfo:\n            RenderDeviceInfo();", source)
        self.assertIn("case Page::DisplaySettings:\n        case Page::DeviceInfo:\n"
                      "            Navigate(Page::Settings);", source)
        widgets = (COMMON / "ui/watch_ui_widgets.cc").read_text(encoding="utf-8")
        translations = dict(re.findall(r'\{"([^"\n]+)", "([^"\n]+)"\}', widgets))
        for text in ("设备信息", "系统版本：", "主控芯片：", "屏幕：", "唤醒词：",
                     "MAC地址：", "2.1寸屏幕"):
            self.assertIn(text, translations)
        self.assertEqual(translations["MAC地址："], "MAC address")
        runtime = (COMMON / "watch/watch_runtime.cc").read_text(encoding="utf-8")
        self.assertIn("device.mac_address = SystemInfo::GetMacAddress();", runtime)
        config = (COMMON.parent / "config.json").read_text(encoding="utf-8")
        self.assertIn("CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y", config)

    def test_existing_font_and_icon_coverage(self):
        data = (COMMON / "resources/watch_ui.pack").read_bytes()
        header = struct.unpack_from("<8s10I", data)
        font = struct.Struct("<HBBhhIIII")
        glyph = struct.Struct("<IIHBBbb")
        faces = {}
        advances = {}
        for i in range(header[3]):
            fields = font.unpack_from(data, header[7] + i * font.size)
            half_px = fields[0] if fields[2] == 1 else fields[0] * 2
            if fields[1] == 4:
                faces[half_px] = {glyph.unpack_from(data, fields[6] + j * glyph.size)[0]
                                  for j in range(fields[5])}
                advances[half_px] = {entry[0]: entry[2] for entry in
                                     (glyph.unpack_from(data, fields[6] + j * glyph.size)
                                      for j in range(fields[5]))}
        required = "设备信息系统版本：主控芯片屏幕唤醒词MAC地址2.1寸你好小智0123456789abcdef:"
        self.assertFalse(set(map(ord, required)) - faces[28])
        self.assertTrue(set(map(ord, "设备信息")) <= faces[32] | faces[28])
        widest_hex = max(advances[28][ord(char)] for char in "0123456789abcdef")
        self.assertLessEqual((12 * widest_hex + 5 * advances[28][ord(":")]) / 16, 142)
        icon = struct.Struct("<32sHHII")
        icons = {icon.unpack_from(data, header[8] + i * icon.size)[0].rstrip(b"\0")
                 for i in range(header[4])}
        self.assertIn(b"info@18", icons)


if __name__ == "__main__":
    unittest.main()

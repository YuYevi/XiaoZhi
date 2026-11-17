"""Exercise production OTA flow and watch dialogs using host hardware substitutes."""

from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest


COMMON = Path(__file__).resolve().parents[1]
ROOT = COMMON.parents[3]


def method(path, signature):
    source = path.read_text(encoding="utf-8")
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


BASE = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "display/firmware_update.h"
#include "device_state.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_TIMEOUT = -2, ESP_ERR_OTA_VALIDATE_FAILED = -3;
'''

UI = r'''
constexpr uint32_t kBg = 0x09090f, kText = 0xf2f1f6;
constexpr int LV_OBJ_FLAG_CLICKABLE = 1, LV_OBJ_FLAG_HIDDEN = 2;
struct lv_obj_t {
    lv_obj_t* parent;
    int x, y, width, height, radius = 0, flags = 0;
    bool deleted = false;
    uint32_t color;
    std::string text;
    std::function<void()> action;
};
std::vector<std::unique_ptr<lv_obj_t>> objects;
void lv_obj_delete(lv_obj_t* node) {
    node->deleted = true;
    for (const auto& child : objects) if (child->parent == node) lv_obj_delete(child.get());
}
void lv_obj_add_flag(lv_obj_t* node, int flag) { node->flags |= flag; }
void lv_obj_remove_flag(lv_obj_t* node, int flag) { node->flags &= ~flag; }
void lv_obj_set_width(lv_obj_t* node, int width) { node->width = width; }
void SetLabelText(lv_obj_t* node, const char* text) { node->text = text; }
void SingleLine(lv_obj_t*) {}
lv_obj_t* Find(const std::string& text) {
    for (const auto& node : objects) if (!node->deleted && node->text == text) return node.get();
    return nullptr;
}
void Click(const char* text) { auto action = Find(text)->action; action(); }
class WatchUi {
public:
    lv_obj_t *root_, *firmware_overlay_ = nullptr, *firmware_progress_ = nullptr,
             *firmware_percent_ = nullptr, *toast_ = nullptr;
    bool power_transition_ = false, swiped_ = true;
    FirmwareUpdateStage firmware_stage_ = FirmwareUpdateStage::Confirm;
    int firmware_progress_value_ = 0;
    std::string firmware_version_, firmware_notes_;
    std::function<void(bool)> firmware_decision_;
    struct Settings { int language = 0; };
    struct Snapshot { Settings settings; } snapshot_;
    WatchUi() { root_ = Box(nullptr, 0, 0, 360, 360, kBg); }
    void ResetMenuInteraction() {}
    void ToggleControl(bool) {}
    void ShowPowerMenu(bool) {}
    void CloseModal() {}
    void UpdateAnimationTimer() {}
    lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
        auto node = std::make_unique<lv_obj_t>();
        node->parent = parent; node->x = x; node->y = y; node->width = w; node->height = h;
        node->color = color; node->radius = radius;
        auto* result = node.get(); objects.push_back(std::move(node)); return result;
    }
    void Border(lv_obj_t*, uint32_t, int) {}
    lv_obj_t* Text(lv_obj_t* p, const char* text, int x, int y, int w, float = 12,
                  uint32_t color = kText, int = 400, bool = false, float line = 22) {
        auto* node = Box(p, x, y, w, line, color); node->text = text; return node;
    }
    lv_obj_t* ScrollList(lv_obj_t* p, int x, int y, int w, int h) { return Box(p, x, y, w, h, 0); }
    lv_obj_t* Button(lv_obj_t* p, int x, int y, int w, int h, uint32_t c, int r,
                     std::function<void()> action, const char* text, float) {
        auto* node = Box(p, x, y, w, h, c, r);
        node->text = text; node->action = std::move(action); return node;
    }
    void ConfirmFirmwareUpdate(const std::string&, const std::string&, std::function<void(bool)>);
    void CloseFirmwareUpdate();
    void RenderFirmwareUpdate();
    void UpdateFirmwareUpdate(FirmwareUpdateStage, int);
};
'''

UI_CASES = r'''
int main() {
    WatchUi ui;
    int calls = 0; bool accepted = false;
    auto decision = [&](bool value) { ++calls; accepted = value; };
    ui.ConfirmFirmwareUpdate("9.8.7", "修复已知问题", decision);
    assert(!ui.swiped_);
    assert(Find("固件升级") && Find("发现新版本：9.8.7") && Find("修复已知问题"));
    assert(Find("是否立即升级？") && Find("确认升级")->color == 0xffa52a);
    assert(Find("取消")->color == 0x36363c && calls == 0);
    auto* panel = Find("固件升级")->parent;
    assert(panel->x == 48 && panel->y == 60 && panel->width == 264 && panel->height == 240);
    for (const auto& node : objects) {
        if (!node->deleted && node->parent == panel) {
            assert(node->x >= 0 && node->y >= 0);
            assert(node->x + node->width <= panel->width && node->y + node->height <= panel->height);
        }
    }
    Click("取消"); assert(calls == 1 && !accepted && !ui.firmware_overlay_);
    ui.CloseFirmwareUpdate(); assert(calls == 1);
    ui.ConfirmFirmwareUpdate("9.8.7", "修复已知问题", decision);
    Click("确认升级"); assert(calls == 2 && accepted && Find("正在升级") && Find("0%"));
    assert(!Find("取消") && !Find("确认升级"));
    ui.UpdateFirmwareUpdate(FirmwareUpdateStage::Downloading, 80);
    assert(Find("80%") && ui.firmware_progress_->width == 172);
    ui.UpdateFirmwareUpdate(FirmwareUpdateStage::Downloading, 200); assert(Find("100%"));
    ui.UpdateFirmwareUpdate(FirmwareUpdateStage::Verifying, 0);
    assert(Find("正在校验固件...") && Find("100%") && !Find("修复已知问题"));
    ui.UpdateFirmwareUpdate(FirmwareUpdateStage::Complete, 0);
    assert(Find("升级完成，正在重启...") && Find("100%"));
    ui.UpdateFirmwareUpdate(FirmwareUpdateStage::Failed, 0);
    assert(Find("升级失败，已保留当前固件") && !Find("100%"));
    Click("知道了"); assert(!ui.firmware_overlay_ && calls == 2);
    ui.UpdateFirmwareUpdate(FirmwareUpdateStage::Downloading, -10);
    assert(Find("正在下载固件...") && Find("0%"));
    assert(ui.firmware_progress_->flags & LV_OBJ_FLAG_HIDDEN);
    ui.CloseFirmwareUpdate(); ui.snapshot_.settings.language = 1;
    ui.ConfirmFirmwareUpdate("9.8.7", "", decision);
    assert(Find("Firmware update") && Find("New version: 9.8.7") && Find("Update now?"));
    Click("Cancel"); assert(calls == 3 && !accepted);
    ui.power_transition_ = true;
    ui.ConfirmFirmwareUpdate("9.8.7", "", decision);
    assert(calls == 4 && !accepted && !ui.firmware_overlay_);
}
'''

DOWNLOAD = r'''
constexpr int MALLOC_CAP_INTERNAL = 0, OTA_WITH_SEQUENTIAL_WRITES = 1;
using esp_ota_handle_t = int;
struct esp_image_header_t { char bytes[24]; };
struct esp_image_segment_header_t { char bytes[8]; };
struct esp_app_desc_t { char bytes[256]; };
struct Partition { const char* label = "ota_1"; unsigned address = 0x1000; } partition;
std::string body, written;
size_t advertised = 0, chunk_size = 7;
int read_fail_at = -1, begin_error = 0, write_error = 0, end_error = 0, boot_error = 0;
int aborted = 0, allocations = 0;
std::vector<std::string> events;
int64_t esp_timer_get_time() { static int64_t time = 0; return time += 1000000; }
void* heap_caps_malloc(size_t size, int) { ++allocations; return malloc(size); }
void heap_caps_free(void* ptr) { --allocations; free(ptr); }
Partition* esp_ota_get_next_update_partition(void*) { return &partition; }
int esp_ota_begin(Partition*, int, int* handle) { *handle = 1; return begin_error; }
void esp_ota_abort(int) { ++aborted; }
int esp_ota_write(int handle, const void* bytes, size_t count) {
    assert(handle == 1); written.append(static_cast<const char*>(bytes), count); return write_error;
}
int esp_ota_end(int) { events.push_back("end"); return end_error; }
int esp_ota_set_boot_partition(Partition*) { events.push_back("boot"); return boot_error; }
struct Http {
    size_t offset = 0;
    bool Open(const char*, const std::string&) { return true; }
    int GetStatusCode() { return 200; }
    size_t GetBodyLength() { return advertised; }
    int Read(char* out, size_t size) {
        if (read_fail_at >= 0 && offset >= size_t(read_fail_at)) return -1;
        size_t count = std::min({size, chunk_size, body.size() - offset});
        memcpy(out, body.data() + offset, count); offset += count; return count;
    }
    void Close() {}
};
struct Network { std::unique_ptr<Http> CreateHttp(int) { return std::make_unique<Http>(); } } network;
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Network* GetNetwork() { return &network; }
};
struct Ota {
    static bool Upgrade(const std::string&, std::function<void(int, size_t)>, std::function<void()>);
};
'''

DOWNLOAD_CASES = r'''
bool Run() {
    events.clear(); written.clear(); aborted = 0;
    bool result = Ota::Upgrade("https://firmware", [](int progress, size_t) {
        assert(progress >= 0 && progress <= 100);
    }, [] { events.push_back("verify"); });
    assert(allocations == 0); return result;
}
int main() {
    body = std::string(10001, 'f'); advertised = body.size();
    assert(Run() && written == body);
    assert((events == std::vector<std::string>{"verify", "end", "boot"}));
    end_error = ESP_ERR_OTA_VALIDATE_FAILED;
    assert(!Run() && (events == std::vector<std::string>{"verify", "end"})); end_error = 0;
    boot_error = -1; assert(!Run()); boot_error = 0;
    read_fail_at = 500; assert(!Run() && aborted == 1 && events.empty()); read_fail_at = -1;
    write_error = -1; assert(!Run() && aborted == 1 && events.empty()); write_error = 0;
    begin_error = -1; assert(!Run() && events.empty()); begin_error = 0;
    advertised = body.size() + 1; assert(!Run() && aborted == 1 && events.empty());
    advertised = body.size() - 1; assert(!Run() && aborted == 1 && events.empty());
    body = std::string(100, 'x'); advertised = body.size();
    assert(!Run() && written.empty() && events.empty());
    body = std::string(8192, 'f'); advertised = body.size(); chunk_size = 4096;
    assert(Run() && written == body);
    advertised = 0; assert(!Run() && events.empty());
}
'''

APPLICATION = r'''
namespace Lang {
namespace Strings {
constexpr auto CHECKING_NEW_VERSION = "checking", CHECK_NEW_VERSION_FAILED = "%d %s",
 ERROR = "error", ACTIVATION = "activation", OTA_UPGRADE = "OTA", UPGRADING = "upgrading",
 NEW_VERSION = "new", UPGRADE_FAILED = "failed";
}
namespace Sounds { constexpr auto OGG_EXCLAMATION = "error", OGG_UPGRADE = "upgrade"; }
}
std::function<void()> delayed;
void vTaskDelay(int) { if (delayed) { auto fn = std::move(delayed); delayed = {}; fn(); } }
enum class PowerSaveLevel { LOW_POWER, PERFORMANCE };
struct Display {
    bool supports_prompt = true, accepts = false;
    int prompts = 0;
    std::string version, notes;
    std::vector<FirmwareUpdateStage> stages;
    void SetStatus(const char*) {}
    void SetChatMessage(const char*, const char*) {}
    bool ConfirmFirmwareUpdate(const std::string& v, const std::string& n, std::function<void(bool)> fn) {
        if (!supports_prompt) return false;
        ++prompts; version = v; notes = n;
        delayed = [this, fn] { fn(accepts); }; return true;
    }
    bool UpdateFirmwareUpdate(FirmwareUpdateStage stage, int = 0) { stages.push_back(stage); return supports_prompt; }
} display;
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
struct Ota {
    bool available = true, activation = false;
    int checks = 0, valid = 0;
    static inline bool success = true;
    static inline int downloads = 0;
    static bool Upgrade(const std::string&, std::function<void(int, size_t)> progress,
                        std::function<void()> verifying) {
        ++downloads; progress(80, 100); verifying(); return success;
    }
    int CheckVersion() { ++checks; return ESP_OK; }
    bool HasNewVersion() { return available; }
    std::string GetFirmwareVersion() { return "9.8.7"; }
    std::string GetFirmwareUrl() { return "https://firmware"; }
    std::string GetFirmwareNotes() { return "server release notes"; }
    std::string GetCheckVersionUrl() { return "https://ota"; }
    void MarkCurrentVersionValid() { ++valid; }
    bool HasActivationCode() { return false; }
    bool HasActivationChallenge() { return activation; }
    const std::string& GetActivationCode() { static std::string s; return s; }
    const std::string& GetActivationMessage() { static std::string s; return s; }
    int Activate() { activation = false; return ESP_OK; }
};
struct Audio { bool stopped = false; void Stop() { stopped = true; } void Start() { stopped = false; } };
struct Protocol { bool IsAudioChannelOpened() { return false; } void CloseAudioChannel() {} };
struct Application {
    DeviceState state = kDeviceStateActivating;
    std::unique_ptr<Ota> ota_ = std::make_unique<Ota>();
    std::unique_ptr<Protocol> protocol_;
    Audio audio_service_;
    bool rebooted = false;
    DeviceState GetDeviceState() { return state; }
    bool SetDeviceState(DeviceState next) { state = next; return true; }
    void Alert(const char*, const char*, const char*, const char*) {}
    void Schedule(std::function<void()> fn) { fn(); }
    void StopNotification() {}
    void ShowActivationCode(const std::string&, const std::string&) {}
    void Reboot() { rebooted = true; }
    void CheckNewVersion();
    bool UpgradeFirmware(const std::string&, const std::string&);
};
'''

APPLICATION_CASES = r'''
int main() {
    Application cancel; cancel.ota_->activation = true;
    cancel.CheckNewVersion();
    assert(display.prompts == 1 && Ota::downloads == 0 && !cancel.rebooted);
    assert(cancel.ota_->checks == 2 && cancel.ota_->valid == 2);
    assert(display.version == "9.8.7" && display.notes == "server release notes");
    display = Display{}; Application latest; latest.ota_->available = false;
    latest.CheckNewVersion(); assert(display.prompts == 0 && Ota::downloads == 0);
    display = Display{}; display.accepts = true; Application accept;
    accept.CheckNewVersion(); assert(accept.rebooted && Ota::downloads == 1);
    assert((display.stages == std::vector<FirmwareUpdateStage>{FirmwareUpdateStage::Downloading,
        FirmwareUpdateStage::Downloading, FirmwareUpdateStage::Verifying, FirmwareUpdateStage::Complete}));
    display = Display{}; display.accepts = true; Ota::success = false;
    Application failed; failed.CheckNewVersion();
    assert(!failed.rebooted && !failed.audio_service_.stopped && failed.state == kDeviceStateActivating);
    assert(display.stages.back() == FirmwareUpdateStage::Failed);
    assert(display.prompts == 1 && Ota::downloads == 2);
    display = Display{}; Ota::success = true; display.supports_prompt = false;
    Application legacy; legacy.CheckNewVersion(); assert(legacy.rebooted && Ota::downloads == 3);
    display = Display{}; Ota::success = false; Application manual; manual.state = kDeviceStateIdle;
    assert(!manual.UpgradeFirmware("https://firmware", "9.8.7"));
    assert(manual.state == kDeviceStateIdle && !manual.audio_service_.stopped);
}
'''


class OtaUpdateTest(unittest.TestCase):
    def run_cpp(self, source, extra_sources=()):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="watch-ota-") as temp:
            path = Path(temp) / "test.cc"
            path.write_text(BASE + source, encoding="utf-8")
            executable = Path(temp) / "test.exe"
            subprocess.run([compiler, "-std=c++17", "-I", str(ROOT / "main"),
                            str(path), *map(str, extra_sources), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_dialog_buttons_and_stages(self):
        source = COMMON / "ui/overlays/dialogs.cc"
        methods = "\n".join(method(source, "void WatchUi::" + name) for name in (
            "ConfirmFirmwareUpdate(", "CloseFirmwareUpdate(", "RenderFirmwareUpdate(",
            "UpdateFirmwareUpdate("))
        self.run_cpp(UI + methods + UI_CASES)

    def test_download_verification_and_failures(self):
        self.run_cpp(DOWNLOAD + method(ROOT / "main/ota.cc", "bool Ota::Upgrade(") + DOWNLOAD_CASES)

    def test_startup_decision_and_failure_recovery(self):
        source = ROOT / "main/application.cc"
        self.run_cpp(APPLICATION + method(source, "void Application::CheckNewVersion(") +
                     method(source, "bool Application::UpgradeFirmware(") + APPLICATION_CASES)

    def test_server_release_notes_and_stale_metadata(self):
        source = (ROOT / "main/ota.cc").read_text(encoding="utf-8")
        start = source.index("    has_new_version_ = false;")
        firmware = source[start:source.index("\n    cJSON_Delete(root);", start)]
        preamble = r'''
#include "managed_components/espressif__cjson/cJSON/cJSON.h"
struct Ota {
    std::string firmware_version_, firmware_url_, firmware_notes_, current_version_ = "1.0.0";
    bool has_new_version_ = false;
    bool IsNewVersionAvailable(const std::string&, const std::string& version) { return version == "9.8.7"; }
    void Parse(cJSON* root);
};
'''
        # Keep the real JSON parser and the production firmware-response block.
        preamble = preamble.replace('"managed_components/', '"' + ROOT.as_posix() + '/managed_components/')
        cases = r'''
int main() {
    Ota ota;
    auto parse = [&](const char* json) { auto* root = cJSON_Parse(json); assert(root); ota.Parse(root); cJSON_Delete(root); };
    parse(R"({"firmware":{"version":"9.8.7","url":"https://firmware","description":"修复已知问题"}})");
    assert(ota.has_new_version_ && ota.firmware_version_ == "9.8.7" && ota.firmware_notes_ == "修复已知问题");
    parse(R"({"firmware":{"version":"9.8.7","url":"https://firmware","release_notes":"server notes"}})");
    assert(ota.has_new_version_ && ota.firmware_notes_ == "server notes");
    parse(R"({"firmware":{"version":"9.8.7","url":"https://firmware","description":"","release_notes":"fallback"}})");
    assert(ota.firmware_notes_ == "fallback");
    parse(R"({"firmware":{"version":"9.8.7","url":"https://firmware","description":42}})");
    assert(ota.has_new_version_ && ota.firmware_notes_.empty());
    parse(R"({"firmware":{"version":"1.0.0","url":"https://firmware","force":1}})");
    assert(ota.has_new_version_);
    parse("{}");
    assert(!ota.has_new_version_ && ota.firmware_version_.empty() && ota.firmware_url_.empty() && ota.firmware_notes_.empty());
}
'''
        self.run_cpp(preamble + "void Ota::Parse(cJSON* root) {\n" + firmware + "\n}\n" + cases,
                     (ROOT / "managed_components/espressif__cjson/cJSON/cJSON.c",))

    def test_font_coverage_and_overlay_guards(self):
        data = (COMMON / "resources/watch_ui.pack").read_bytes()
        header = struct.unpack_from("<8s10I", data)
        font, glyph = struct.Struct("<HBBhhIIII"), struct.Struct("<IIHBBbb")
        common = set()
        for i in range(header[3]):
            face = font.unpack_from(data, header[7] + i * font.size)
            half_px = face[0] if face[2] == 1 else face[0] * 2
            if half_px == 28 and face[1] == 4:
                common = {glyph.unpack_from(data, face[6] + j * glyph.size)[0] for j in range(face[5])}
        source = (COMMON / "ui/overlays/dialogs.cc").read_text(encoding="utf-8")
        source = source[:source.index("void WatchUi::CloseModal()")]
        required = set(map(ord, "".join(re.findall(r'"([^"\n]*)"', source))))
        self.assertFalse(required - common, "OTA text must render with the existing font")
        ui = COMMON / "ui/watch_ui.cc"
        self.assertIn("firmware_overlay_", method(ui, "void WatchUi::Navigate("))
        self.assertIn("firmware_overlay_", method(ui, "void WatchUi::GoBack("))
        self.assertIn("firmware_overlay_", method(ui, "void WatchUi::Tick("))
        self.assertIn("firmware_overlay_", method(ui, "void WatchUi::Gesture("))
        boot = COMMON / "ui/pages/boot_page.cc"
        self.assertIn("firmware_overlay_", method(boot, "void WatchUi::UpdateBootState("))
        runtime = COMMON / "watch/watch_runtime.cc"
        for name in ("Action(", "Sleep(", "PowerTap(", "PowerLongPress(", "RequestPowerChange(", "Tick("):
            self.assertIn("IsFirmwareUpdateActive()", method(runtime, "void WatchRuntime::" + name))


if __name__ == "__main__":
    unittest.main()

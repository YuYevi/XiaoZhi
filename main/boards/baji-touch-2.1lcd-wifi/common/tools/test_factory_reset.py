"""Run the production factory-reset implementation with a host NVS substitute."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


COMMON = Path(__file__).resolve().parents[1]

PREAMBLE = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <string>
using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = 1, ESP_ERR_NVS_NOT_FOUND = 2;
constexpr int ESP_ERR_NVS_NO_FREE_PAGES = 3, ESP_ERR_NVS_NEW_VERSION_FOUND = 4;
constexpr int ESP_ERR_INVALID_STATE = 5, NVS_READONLY = 0, NVS_READWRITE = 1;
using Entries = std::map<std::string, uint8_t>;
std::map<std::string, Entries> disk;
struct Handle { std::string name; Entries staged; };
std::map<int, Handle> handles;
std::deque<int> init_results;
int open_result, set_result, commit_result, get_result, erase_result;
int erases, inits, next_handle;

esp_err_t nvs_flash_init() {
    ++inits;
    if (init_results.empty()) return ESP_OK;
    const int result = init_results.front();
    init_results.pop_front();
    return result;
}
esp_err_t nvs_flash_erase() {
    assert(handles.empty());
    ++erases;
    if (erase_result != ESP_OK) return erase_result;
    disk.clear();
    return ESP_OK;
}
esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    if (open_result != ESP_OK) return open_result;
    if (mode == NVS_READONLY && !disk.count(name)) return ESP_ERR_NVS_NOT_FOUND;
    *handle = ++next_handle;
    handles[*handle] = {name, disk.count(name) ? disk.at(name) : Entries{}};
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
    if (set_result != ESP_OK) return set_result;
    handles.at(handle).staged[key] = value;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* value) {
    if (get_result != ESP_OK) return get_result;
    const auto& entries = handles.at(handle).staged;
    if (!entries.count(key)) return ESP_ERR_NVS_NOT_FOUND;
    *value = entries.at(key);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    if (commit_result != ESP_OK) return commit_result;
    const auto& entry = handles.at(handle);
    disk[entry.name] = entry.staged;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handles.erase(handle) == 1); }
void Reset() {
    assert(handles.empty());
    disk = {{"watch", {{"state0", 1}, {"state1", 2}, {"active", 1}, {"net_off", 1}}},
            {"wifi", {{"ssid", 3}, {"password", 4}, {"ssid1", 5}, {"password1", 6}}},
            {"display", {{"brightness", 7}}}, {"audio", {{"volume", 8}}},
            {"mqtt", {{"credentials", 9}}}, {"board", {{"uuid", 10}}},
            {"other_app", {{"config", 11}}}};
    init_results.clear();
    open_result = set_result = commit_result = get_result = erase_result = ESP_OK;
    erases = inits = next_handle = 0;
}
'''

CASES = r'''
int main() {
    // Ordinary boot, empty namespace, and a cleared request must preserve data.
    Reset(); const auto original = disk;
    assert(baji::CompletePendingFactoryReset() == ESP_OK && disk == original && erases == 0);
    disk["factory_reset"] = {};
    assert(baji::CompletePendingFactoryReset() == ESP_OK && erases == 0);
    disk["factory_reset"]["pending"] = 0;
    assert(baji::CompletePendingFactoryReset() == ESP_OK && erases == 0);

    // Request is durable but nondestructive. Even shutdown writes are erased at boot.
    Reset();
    assert(baji::RequestFactoryReset() == ESP_OK && erases == 0);
    assert(disk.at("factory_reset").at("pending") == 1 && disk.at("wifi") == original.at("wifi"));
    disk["wifi"]["password"] = 99;
    disk["baji_battery"]["state"] = 42;
    assert(baji::CompletePendingFactoryReset() == ESP_OK && disk.empty() && erases == 1);
    assert(handles.empty() && inits == 2);
    assert(baji::CompletePendingFactoryReset() == ESP_OK && erases == 1);

    // Failed requests never claim success or erase existing user data.
    for (int* fault : {&open_result, &set_result, &commit_result}) {
        Reset(); *fault = ESP_FAIL;
        assert(baji::RequestFactoryReset() == ESP_FAIL);
        assert(handles.empty() && disk == original && erases == 0);
    }

    // Read/init errors and unexpected markers must not authorize an erase.
    Reset(); init_results = {ESP_FAIL};
    assert(baji::CompletePendingFactoryReset() == ESP_FAIL && disk == original && erases == 0);
    Reset(); open_result = ESP_FAIL;
    assert(baji::CompletePendingFactoryReset() == ESP_FAIL && erases == 0);
    Reset(); assert(baji::RequestFactoryReset() == ESP_OK); get_result = ESP_FAIL;
    assert(baji::CompletePendingFactoryReset() == ESP_FAIL && handles.empty() && erases == 0);
    Reset(); disk["factory_reset"]["pending"] = 2;
    assert(baji::CompletePendingFactoryReset() == ESP_ERR_INVALID_STATE && erases == 0);

    // A failed erase retains the request so the next boot can retry it.
    Reset(); assert(baji::RequestFactoryReset() == ESP_OK); erase_result = ESP_FAIL;
    assert(baji::CompletePendingFactoryReset() == ESP_FAIL);
    assert(disk.at("factory_reset").at("pending") == 1 && disk.at("watch") == original.at("watch"));
    erase_result = ESP_OK;
    assert(baji::CompletePendingFactoryReset() == ESP_OK && disk.empty() && erases == 2);

    // Initialization failing after erase must not resurrect any user data.
    Reset(); assert(baji::RequestFactoryReset() == ESP_OK); init_results = {ESP_OK, ESP_FAIL};
    assert(baji::CompletePendingFactoryReset() == ESP_FAIL && disk.empty());
    assert(baji::CompletePendingFactoryReset() == ESP_OK && erases == 1);

    // Preserve ESP-IDF's existing NVS version/full-partition recovery behavior.
    for (const int error : {ESP_ERR_NVS_NO_FREE_PAGES, ESP_ERR_NVS_NEW_VERSION_FOUND}) {
        Reset(); init_results = {error, ESP_OK};
        assert(baji::CompletePendingFactoryReset() == ESP_OK && disk.empty() && erases == 1);
        Reset(); init_results = {error}; erase_result = ESP_FAIL;
        assert(baji::CompletePendingFactoryReset() == ESP_FAIL && disk == original && inits == 1);
        Reset(); init_results = {error, ESP_FAIL};
        assert(baji::CompletePendingFactoryReset() == ESP_FAIL && disk.empty());
    }
    assert(handles.empty());
    std::cout << "Factory reset persistence and failure checks passed\n";
}
'''


class FactoryResetTest(unittest.TestCase):
    def test_production_implementation(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A host C++ compiler is required")
        source = (COMMON / "watch/factory_reset.cc").read_text(encoding="utf-8")
        implementation = source[source.index("namespace baji {"):]
        with tempfile.TemporaryDirectory(prefix="watch-factory-reset-") as temp:
            test_source = Path(temp) / "test.cc"
            test_source.write_text(PREAMBLE + implementation + CASES, encoding="utf-8")
            executable = Path(temp) / "test.exe"
            subprocess.run([compiler, "-std=c++17", str(test_source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_boot_and_action_wiring(self):
        source = (COMMON / "power/power_boot.cc").read_text(encoding="utf-8")
        charging = source[source.index('extern "C" void board_charging_only_main'):]
        self.assertLess(charging.index("baji::CompletePendingFactoryReset()"),
                        charging.index("BajiBatteryMonitor battery"))
        normal = source[source.index('extern "C" void __wrap_app_main'):]
        self.assertLess(normal.index("baji::CompletePendingFactoryReset()"),
                        normal.index("__real_app_main()"))
        runtime = (COMMON / "watch/watch_runtime.cc").read_text(encoding="utf-8")
        action = runtime[runtime.index("void WatchRuntime::Action("):]
        self.assertIn("sleep_requested_ || power_transition_", action)
        reset = action[action.index("case WatchUi::Action::FactoryReset:"):
                       action.index("case WatchUi::Action::SetPowerSave:")]
        self.assertLess(reset.index("kDeviceStateUpgrading"), reset.index("baji::RequestFactoryReset()"))
        self.assertIn("result != ESP_OK", reset)
        self.assertIn("else RequestPowerChange(true);", reset)
        self.assertNotIn("ResetWatchData", runtime)

    def test_settings_describes_full_reset(self):
        source = (COMMON / "ui/pages/settings_page.cc").read_text(encoding="utf-8")
        self.assertIn('row("trash-2", "恢复出厂设置", kBlue', source)
        self.assertIn("清除全部用户数据", source)
        self.assertIn("重新配网", source)
        self.assertNotIn("Wi-Fi 保留", source)
        self.assertNotIn("Wi-Fi will be kept", source)


if __name__ == "__main__":
    unittest.main()

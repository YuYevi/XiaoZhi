"""Run the production gesture/provisioning methods with host hardware substitutes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[5]
BOARD = ROOT / "main/boards/baji-touch-2.1lcd-wifi"


def method(path, signature):
    source = path.read_text(encoding="utf-8")
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


PREAMBLE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "device_state.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(value) assert((value) == 0)
#define CONFIG_USE_HOTSPOT_WIFI_PROVISIONING 1
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
int64_t now_us = 1000000;
int64_t esp_timer_get_time() { return now_us; }
int esp_timer_stop(void*) { return 0; }
int esp_timer_start_once(void*, uint64_t) { return 0; }
void vTaskDelay(int) {}
void vTaskDelete(void*) {}
bool fail_worker = false;
std::vector<std::function<void()>> workers;
int xTaskCreate(void (*fn)(void*), const char*, int, void* arg, int, void**) {
    if (fail_worker) return 0;
    workers.push_back([=] { fn(arg); });
    return pdPASS;
}
void RunWorkers() {
    auto batch = std::move(workers);
    workers.clear();
    for (auto& work : batch) work();
}
enum class NetworkEvent { Connected, Disconnected, Scanning, Connecting,
                          WifiConfigModeEnter, WifiConfigModeExit };
struct Display {
    bool menu = false;
    void ShowNotification(const char*) {}
    bool IsPowerMenuOpen() { return menu; }
    void ShowPowerMenu(bool show) { menu = show; }
    void UpdateStatusBar(bool) {}
};
struct DeviceStateMachine {
    bool IsValidTransition(DeviceState from, DeviceState to) const;
};
struct Application {
    DeviceState state = kDeviceStateIdle;
    bool protocol_ = true;
    void* activation_task_handle_ = nullptr;
    int activations = 0, alerts = 0;
    std::vector<std::function<void()>> queue;
    static Application& GetInstance() { static Application app; return app; }
    DeviceState GetDeviceState() { return state; }
    bool SetDeviceState(DeviceState next) {
        assert(DeviceStateMachine().IsValidTransition(state, next));
        state = next;
        return true;
    }
    void Schedule(std::function<void()> fn) { queue.push_back(std::move(fn)); }
    void ResetProtocol() { Schedule([this] { protocol_ = false; }); }
    void Alert(const char*, const char*, const char*, const char*) { ++alerts; }
    void ActivationTask() { ++activations; protocol_ = true; state = kDeviceStateIdle; }
    void HandleNetworkConnectedEvent();
    void Flush() {
        while (!queue.empty()) {
            auto batch = std::move(queue);
            queue.clear();
            for (auto& task : batch) task();
        }
    }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
namespace Lang {
namespace Strings {
    const char* ENTERING_WIFI_CONFIG_MODE = "enter";
    const char* CONNECT_TO_HOTSPOT = "connect";
    const char* ACCESS_VIA_BROWSER = "url";
    const char* WIFI_CONFIG_MODE = "config";
}
namespace Sounds { const char* OGG_WIFICONFIG = "sound"; }
}
struct WifiManager {
    bool config = false, connected = true;
    int station_starts = 0, ap_starts = 0;
    std::function<void(NetworkEvent)> event;
    static WifiManager& GetInstance() { static WifiManager wifi; return wifi; }
    bool IsConfigMode() { return config; }
    bool IsConnected() { return connected; }
    std::string GetSsid() { return "saved"; }
    std::string GetApSsid() { return "BAJI"; }
    std::string GetApWebUrl() { return "http://192.168.4.1"; }
    void StartConfigAp() { ++ap_starts; config = true; event(NetworkEvent::WifiConfigModeEnter); }
    void StopConfigAp() { if (config) { config = false; event(NetworkEvent::WifiConfigModeExit); } }
    void StopStation() { connected = false; event(NetworkEvent::Disconnected); }
    void StartStation() { ++station_starts; }
};
struct SsidManager {
    std::vector<int> saved{1};
    static SsidManager& GetInstance() { static SsidManager ssids; return ssids; }
    const std::vector<int>& GetSsidList() { return saved; }
};
struct WifiBoard {
    void* connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    std::atomic<bool> config_mode_pending_{false}, config_mode_cancelled_{false};
    std::atomic<uint32_t> config_mode_generation_{0};
    std::function<void(NetworkEvent, const std::string&)> network_event_callback_;
    int automatic_reconnects = 0;
    Display* GetDisplay() { return Board::GetInstance().GetDisplay(); }
    void TryWifiConnect() { ++automatic_reconnects; }
    bool IsInWifiConfigMode() const { return WifiManager::GetInstance().IsConfigMode(); }
    void StartWifiConfigMode();
    void EnterWifiConfigMode();
    void ExitWifiConfigMode();
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    static void OnWifiConnectTimeout(void* arg);
};
struct WatchUi { enum class Action { StartWifiConfig }; };
struct WatchRuntime {
    bool power_transition_ = false, awake_ = true, sleep_requested_ = false;
    int64_t power_tap_at_ = 0, last_activity_ = 0;
    uint8_t power_tap_count_ = 0;
    std::atomic<uint32_t> power_tap_generation_{0};
    void* power_tap_timer_ = nullptr;
    Display display_;
    int backs = 0, chats = 0, configs = 0;
    bool IsAwake() { return awake_ && !power_transition_; }
    void Wake() { awake_ = true; sleep_requested_ = false; }
    void Back() { ++backs; }
    void Chat() { ++chats; }
    void Action(WatchUi::Action, int) { ++configs; }
    void CancelPowerTap();
    void FinishPowerTap();
    void PowerTap();
    void PowerLongPress();
};
'''

CASES = r'''
int main() {
    auto& app = Application::GetInstance();
    auto& wifi = WifiManager::GetInstance();
    WifiBoard board;
    wifi.event = [&](NetworkEvent event) { board.OnNetworkEvent(event); };
    board.network_event_callback_ = [&](NetworkEvent event, const std::string&) {
        if (event == NetworkEvent::Connected)
            app.Schedule([&] { app.HandleNetworkConnectedEvent(); });
    };
    auto tap = [](WatchRuntime& watch, int gap = 100000) { now_us += gap; watch.PowerTap(); };
    auto expire = [&](WatchRuntime& watch) {
        now_us += 320000;
        timer_callback(&watch);
        app.Flush();
    };
    WatchRuntime single;
    tap(single); expire(single);
    assert(single.backs == 1 && !single.chats && !single.configs);
    WatchRuntime twice;
    tap(twice); tap(twice);
    assert(!twice.chats);
    expire(twice);
    assert(twice.chats == 1 && !twice.backs && !twice.configs);
    WatchRuntime triple;
    tap(triple); tap(triple); tap(triple);
    assert(triple.configs == 1 && !triple.chats && !triple.backs);
    expire(triple);
    tap(triple); tap(triple); tap(triple); expire(triple);
    assert(triple.configs == 2 && !triple.chats && !triple.backs);
    WatchRuntime stale;
    tap(stale); now_us += 320000; timer_callback(&stale);
    tap(stale); tap(stale); tap(stale); app.Flush();
    assert(stale.backs == 1 && stale.configs == 1 && !stale.chats);
    WatchRuntime boundary;
    tap(boundary); tap(boundary, 320000); expire(boundary);
    assert(boundary.backs == 2 && !boundary.configs && !boundary.chats);
    WatchRuntime wake;
    wake.awake_ = false; tap(wake); expire(wake);
    assert(wake.awake_ && !wake.backs && !wake.chats && !wake.configs);
    tap(wake); wake.PowerLongPress(); expire(wake);
    assert(wake.display_.menu && !wake.backs && !wake.configs);
    tap(wake); expire(wake);
    assert(!wake.display_.menu && !wake.backs);
    wake.power_transition_ = true; tap(wake); expire(wake);
    assert(!wake.backs && !wake.configs);

    // Cancel before the delayed enter task has run. The old task must stay inert.
    board.EnterWifiConfigMode();
    assert(board.config_mode_pending_);
    board.ExitWifiConfigMode();
    app.Flush(); RunWorkers(); app.Flush(); RunWorkers();
    assert(!wifi.config && !board.config_mode_pending_ && wifi.ap_starts == 0);
    assert(app.protocol_ && app.activations == 1);

    // Enter -> cancel -> enter while both delayed workers are still queued.
    board.EnterWifiConfigMode(); board.ExitWifiConfigMode(); board.EnterWifiConfigMode();
    app.Flush(); RunWorkers(); app.Flush();
    assert(wifi.config && wifi.ap_starts == 1 && app.state == kDeviceStateWifiConfiguring);
    board.ExitWifiConfigMode(); app.Flush();
    assert(!wifi.config && app.state == kDeviceStateIdle && wifi.station_starts == 1);
    assert(board.automatic_reconnects == 0 && SsidManager::GetInstance().saved.size() == 1);
    WifiBoard::OnWifiConnectTimeout(&board); app.Flush();
    assert(!wifi.config && wifi.ap_starts == 1);
    wifi.connected = true; board.OnNetworkEvent(NetworkEvent::Connected, "saved");
    app.Flush(); RunWorkers();
    assert(app.protocol_ && app.activations == 2 && app.state == kDeviceStateIdle);
    board.OnNetworkEvent(NetworkEvent::Connected, "saved"); app.Flush(); RunWorkers();
    assert(app.activations == 2); // An initialized protocol needs no new activation.

    // No saved credentials: cancelling closes the AP and never restarts it.
    wifi.connected = false; SsidManager::GetInstance().saved.clear();
    board.EnterWifiConfigMode(); app.Flush(); RunWorkers(); app.Flush();
    board.ExitWifiConfigMode(); app.Flush();
    WifiBoard::OnWifiConnectTimeout(&board); app.Flush();
    assert(!wifi.config && app.state == kDeviceStateIdle && wifi.station_starts == 1);
    board.EnterWifiConfigMode(); app.Flush(); RunWorkers(); app.Flush();
    assert(wifi.config && app.state == kDeviceStateWifiConfiguring);

    // Web completion still follows the native automatic reconnect path.
    wifi.StopConfigAp();
    assert(board.automatic_reconnects == 1);
    app.state = kDeviceStateIdle; app.protocol_ = true; fail_worker = true;
    board.EnterWifiConfigMode(); app.Flush();
    assert(!board.config_mode_pending_ && app.protocol_);
    fail_worker = false; app.state = kDeviceStateStarting;
    const int alerts_before = app.alerts;
    board.EnterWifiConfigMode();
    assert(wifi.config && app.state == kDeviceStateWifiConfiguring);
    board.ExitWifiConfigMode(); app.Flush();
    assert(!wifi.config && app.state == kDeviceStateIdle);
    assert(app.alerts == alerts_before); // An old prompt must not override cancellation.
    std::cout << "Power gestures and Wi-Fi cancellation checks passed\n";
}
'''


class PowerWifiControlsTest(unittest.TestCase):
    def test_production_methods(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A host C++ compiler is required")
        runtime = BOARD / "common/watch/watch_runtime.cc"
        wifi = ROOT / "main/boards/common/wifi_board.cc"
        methods = [method(runtime, f"void WatchRuntime::{name}(") for name in (
            "CancelPowerTap", "FinishPowerTap", "PowerTap", "PowerLongPress")]
        methods += [method(wifi, f"void WifiBoard::{name}(") for name in (
            "StartWifiConfigMode", "EnterWifiConfigMode", "ExitWifiConfigMode",
            "OnNetworkEvent", "OnWifiConnectTimeout")]
        methods.append(method(ROOT / "main/device_state_machine.cc",
                              "bool DeviceStateMachine::IsValidTransition("))
        methods.append(method(ROOT / "main/application.cc",
                              "void Application::HandleNetworkConnectedEvent("))
        source = runtime.read_text(encoding="utf-8")
        start = source.index(".callback = [](void* context)") + len(".callback = ")
        callback = source[start:source.index(",\n        .arg = this", start)]
        code = PREAMBLE + "\n".join(methods) + "\nauto timer_callback = " + callback + ";\n" + CASES
        with tempfile.TemporaryDirectory(prefix="watch-power-wifi-") as temp:
            test_source = Path(temp) / "test.cc"
            test_source.write_text(code, encoding="utf-8")
            executable = Path(temp) / "test.exe"
            subprocess.run([compiler, "-std=c++17", "-I", str(ROOT / "main"),
                            str(test_source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_settings_has_no_wlan_entry(self):
        source = (BOARD / "common/ui/pages/settings_page.cc").read_text(encoding="utf-8")
        self.assertNotIn('row("wifi"', source)
        self.assertNotIn("Page::Network", source)
        self.assertNotIn("settings_wifi_label_", source)


if __name__ == "__main__":
    unittest.main()

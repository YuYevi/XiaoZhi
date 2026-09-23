#include "watch_runtime.h"

#include "hardware/baji_display.h"
#include "hardware/baji_audio_codec.h"
#include "config.h"
#include "application.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "dual_network_board.h"
#include <wifi_manager.h>
#include <esp_network.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace {
constexpr const char* kTag = "WatchRuntime";
bool ChatState(DeviceState state) {
    return state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking;
}

bool ScreenCanSleep(DeviceState state) {
    // Provisioning is a background AP service; switching off only the screen
    // must not prevent the phone from saving Wi-Fi credentials.
    return state == kDeviceStateIdle || state == kDeviceStateWifiConfiguring;
}

// Use the native provisioning server on this device's AP address. Its /scan
// endpoint copies its cached records; it does not steal the driver's scan list.
bool PortalRequest(const char* path, const std::string* body, std::string& response, std::string& error) {
    auto& manager = WifiManager::GetInstance();
    if (!manager.IsConfigMode()) { error = "配网服务尚未就绪，请重试"; return false; }
    if (manager.GetApWebUrl() != "http://192.168.4.1") { error = "配网服务地址不匹配"; return false; }
    const std::string url = std::string("http://192.168.4.1") + path;
    // Explicitly select the native ESP/lwIP network, never the active board's
    // modem transport. Credentials are sent only to this device's config AP.
    EspNetwork network;
    auto client = network.CreateHttp();
    if (!client) { error = "无法创建配网请求"; return false; }
    const int timeout_ms = body ? 35000 : 5000;
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    client->SetTimeout(timeout_ms);
    client->SetKeepAlive(false);
    if (body) {
        client->SetHeader("Content-Type", "application/json");
        client->SetContent(std::string(*body));
    }
    if (!client->Open(body ? "POST" : "GET", url) || client->GetStatusCode() != 200) {
        client->Close();
        error = "配网请求失败或超时，请重试";
        return false;
    }
    response.clear();
    char buffer[512];
    for (;;) {
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) { error = "配网请求超时，请重试"; client->Close(); return false; }
        client->SetTimeout(std::max<int64_t>(1, remaining_us / 1000));
        const int count = client->Read(buffer, sizeof(buffer));
        if (count < 0) { error = "读取配网服务失败，请重试"; client->Close(); return false; }
        if (count == 0) break;
        if (response.size() + count > 16384) { error = "配网响应过大，请重试"; client->Close(); return false; }
        response.append(buffer, count);
    }
    client->Close();
    return true;
}

void SortNetworks(std::vector<WatchUi::WifiNetwork>& networks) {
    std::sort(networks.begin(), networks.end(), [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
    std::vector<WatchUi::WifiNetwork> unique;
    for (auto& item : networks) {
        if (!item.ssid.empty() && std::none_of(unique.begin(), unique.end(), [&](const auto& old) {
            return old.ssid == item.ssid;
        })) unique.push_back(std::move(item));
        if (unique.size() == 32) break;
    }
    networks = std::move(unique);
}

bool WaitForPortal() {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (WifiManager::GetInstance().IsConfigMode()) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

bool CredentialsSaved(const std::string& ssid, const std::string& password) {
    // Native SsidManager doesn't return write errors. Verify the requested record
    // before reporting success, without logging or exporting the password.
    nvs_handle_t handle = 0;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    bool found = false;
    for (int i = 0; i < 10 && !found; ++i) {
        const auto suffix = i ? std::to_string(i) : std::string();
        char stored_ssid[33] = {}, stored_password[65] = {};
        size_t ssid_size = sizeof(stored_ssid), password_size = sizeof(stored_password);
        if (nvs_get_str(handle, ("ssid" + suffix).c_str(), stored_ssid, &ssid_size) == ESP_OK &&
            nvs_get_str(handle, ("password" + suffix).c_str(), stored_password, &password_size) == ESP_OK)
            found = ssid == stored_ssid && password == stored_password;
        std::fill(std::begin(stored_password), std::end(stored_password), '\0');
    }
    nvs_close(handle);
    return found;
}

bool HasCredentialSlot(const std::string& ssid) {
    nvs_handle_t handle = 0;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return true;
    int occupied = 0;
    bool existing = false;
    for (int i = 0; i < 10; ++i) {
        const auto key = std::string("ssid") + (i ? std::to_string(i) : std::string());
        char saved[33] = {};
        size_t size = sizeof(saved);
        if (nvs_get_str(handle, key.c_str(), saved, &size) == ESP_OK) {
            ++occupied;
            existing |= ssid == saved;
        }
    }
    nvs_close(handle);
    // Native AddSsid evicts its last record when full. Refuse that implicit
    // deletion here; the existing mobile portal can manage saved networks.
    return existing || occupied < 10;
}
}

WatchRuntime::WatchRuntime(Board& board, BajiDisplay& display)
    : board_(board), display_(display), services_(WatchServices::GetInstance()) {}

void WatchRuntime::Initialize() {
    std::string error;
    if (!services_.Initialize(&error)) ESP_LOGE(kTag, "Settings: %s", error.c_str());
    display_.SetWatchActionCallback([this](WatchUi::Action action, int value) {
        Application::GetInstance().Schedule([this, action, value]() { Action(action, value); });
    });
    display_.SetWatchWifiCallbacks([this]() {
        Application::GetInstance().Schedule([this]() { RequestWifiScan(); });
    }, [this](std::string ssid, std::string password) {
        Application::GetInstance().Schedule([this, ssid = std::move(ssid), password = std::move(password)]() mutable {
            RequestWifiConnect(std::move(ssid), std::move(password));
        });
    });
    last_activity_ = esp_timer_get_time();
    const esp_timer_create_args_t tap_args = {
        .callback = [](void* context) {
            auto* self = static_cast<WatchRuntime*>(context);
            const uint32_t generation = self->power_tap_generation_.load();
            Application::GetInstance().Schedule([self, generation]() {
                if (generation != self->power_tap_generation_.load() || !self->power_tap_at_) return;
                if (esp_timer_get_time() - self->power_tap_at_ < 320000) return;
                self->power_tap_at_ = 0;
                if (self->IsAwake()) self->Back();
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "watch_power_tap",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tap_args, &power_tap_timer_));
}

void WatchRuntime::SetNetworkActions(std::function<void(int)> change, std::function<void()> configure) {
    change_network_ = std::move(change);
    configure_network_ = std::move(configure);
}

void WatchRuntime::Start() {
    if (started_) return;
    started_ = true;
    services_.RegisterMcpTools();
    services_.SetReminderCallback([this](const std::vector<WatchReminder>&) {
        // WatchServices delivers on the application task, outside its lock.
        Wake();
    });
    std::string error;
    if (!services_.Start(&error)) display_.ShowNotification(error, 10000);
    const esp_timer_create_args_t args = {
        .callback = [](void* context) {
            auto* self = static_cast<WatchRuntime*>(context);
            if (self->tick_pending_.exchange(true)) return;
            Application::GetInstance().Schedule([self]() {
                self->Tick();
                self->tick_pending_ = false;
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "watch_runtime",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 250000));
    ESP_LOGI(kTag, "Started: power-button wake, automatic screen off, local reminders");
}

void WatchRuntime::Wake() {
    if (power_transition_) return;
    sleep_requested_ = false;
    last_activity_ = esp_timer_get_time();
    if (awake_) return;
    awake_ = true;
    display_.SetWatchAwake(true);
    board_.GetBacklight()->RestoreBrightness();
    auto& app = Application::GetInstance();
    if (network_enabled_ && started_ && !ringing_ && !services_.Snapshot().settings.power_save && app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().EnableWakeWordDetection(true);
    ApplyPowerSettings();
    ESP_LOGI(kTag, "Screen awake");
}

void WatchRuntime::Sleep() {
    auto& app = Application::GetInstance();
    if (!awake_ || flashlight_ || !ScreenCanSleep(app.GetDeviceState()) || ringing_ ||
        (!reminder_timed_out_ && !services_.Snapshot().reminders.empty())) return;
    awake_ = false;
    sleep_requested_ = false;
    CancelPowerTap();
    display_.ShowPowerMenu(false);
    display_.SetWatchAwake(false);
    board_.GetBacklight()->SetBrightness(0);
    app.GetAudioService().EnableWakeWordDetection(false);
    app.GetAudioService().EnableVoiceProcessing(false);
    if (app.GetDeviceState() == kDeviceStateIdle) board_.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(kTag, "Screen asleep; only the power button or a reminder can wake it");
}

void WatchRuntime::Back() {
    if (!IsAwake()) return;
    last_activity_ = esp_timer_get_time();
    if (display_.IsPowerMenuOpen()) {
        display_.ShowPowerMenu(false);
        return;
    }
    if (display_.IsWatchChat()) StopChat();
    display_.WatchBack();
}

void WatchRuntime::Chat() {
    if (!IsAwake()) return;
    last_activity_ = esp_timer_get_time();
    display_.ShowPowerMenu(false);
    display_.ShowWatchChat();
    Action(WatchUi::Action::StartChat, 0);
}

void WatchRuntime::CancelPowerTap() {
    ++power_tap_generation_;
    power_tap_at_ = 0;
    if (power_tap_timer_) esp_timer_stop(power_tap_timer_);
}

void WatchRuntime::PowerTap() {
    if (power_transition_) return;
    if (!awake_ || sleep_requested_) {
        CancelPowerTap();
        Wake();
        return;
    }
    if (display_.IsPowerMenuOpen()) {
        CancelPowerTap();
        display_.ShowPowerMenu(false);
        last_activity_ = esp_timer_get_time();
        return;
    }
    const int64_t now = esp_timer_get_time();
    last_activity_ = now;
    if (power_tap_at_ && now - power_tap_at_ < 320000) {
        CancelPowerTap();
        Chat();
        return;
    }
    // If dispatch was delayed past the timer deadline, preserve the first tap.
    if (power_tap_at_) Back();
    CancelPowerTap();
    power_tap_at_ = now;
    ESP_ERROR_CHECK(esp_timer_start_once(power_tap_timer_, 320000));
}

void WatchRuntime::PowerLongPress() {
    if (power_transition_) return;
    CancelPowerTap();
    Wake();
    display_.ShowPowerMenu(true);
}

void WatchRuntime::RequestSleep() {
    CancelPowerTap();
    display_.ShowPowerMenu(false);
    const auto state = Application::GetInstance().GetDeviceState();
    if (ringing_ || (!reminder_timed_out_ && !services_.Snapshot().reminders.empty())) {
        display_.ShowNotification("请先关闭提醒");
        return;
    }
    if (!ScreenCanSleep(state) && !ChatState(state)) {
        display_.ShowNotification("系统任务结束后可息屏");
        return;
    }
    flashlight_ = false;
    sleep_requested_ = true;
    StopChat();
    if (!stop_chat_requested_ && !chat_start_submitted_) Sleep();
}

void WatchRuntime::RequestPowerChange(bool reboot) {
    if (power_transition_) return;
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateUpgrading) {
        display_.ShowPowerMenu(false);
        display_.ShowNotification("正在更新，请完成后重试");
        return;
    }
    if (!reboot && !power_off_) return;
    CancelPowerTap();
    sleep_requested_ = false;
    StopRing();
    StopChat();
    power_transition_ = true;
    power_reboot_ = reboot;
    power_reboot_at_ = esp_timer_get_time() + 2100000;
    display_.ShowPowerTransition(reboot);
    app.GetAudioService().EnableWakeWordDetection(false);
    if (!reboot) power_off_();
}

void WatchRuntime::StopChat() {
    // A start and a back gesture can be queued in the same application batch,
    // before its native toggle event changes Idle into Connecting.
    ++chat_request_generation_;
    chat_start_pending_ = false;
    stop_chat_requested_ = stop_chat_requested_ || chat_start_submitted_ ||
        ChatState(Application::GetInstance().GetDeviceState());
    ContinueStopChat();
}

void WatchRuntime::ContinueStopChat() {
    if (!stop_chat_requested_) return;
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
#if BAJI_WATCH_LOCAL_AUDIO
    // The approved ownership API can close the channel synchronously, without
    // toggling an Idle state back into a new conversation after a remote close.
    if (ringing_) return;
    if (ChatState(state) || chat_start_submitted_) {
        if (!app.BeginLocalAudio()) return;
        app.EndLocalAudio();
    }
    stop_chat_requested_ = chat_start_submitted_ = false;
    chat_stop_submitted_ = chat_abort_submitted_ = false;
#else
    if (state == kDeviceStateSpeaking && !chat_abort_submitted_) {
        chat_abort_submitted_ = true;
        app.AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening && !chat_stop_submitted_) {
        chat_stop_submitted_ = true;
        app.ToggleChatState();
    } else if (!ChatState(state) && !chat_start_submitted_) {
        stop_chat_requested_ = false;
        chat_stop_submitted_ = false;
        chat_abort_submitted_ = false;
    }
#endif
}

void WatchRuntime::OnNetworkEvent(NetworkEvent event) {
    Application::GetInstance().Schedule([this, event]() {
        connected_ = event == NetworkEvent::Connected;
        last_ui_tick_ = 0;
    });
}

void WatchRuntime::OnPerformanceRequested() {
    // Native network/audio requests are not physical wake sources. Tick keeps
    // sleeping audio disabled without reopening the screen on reconnection.
}

void WatchRuntime::Action(WatchUi::Action action, int value) {
    auto& app = Application::GetInstance();
    if (!IsAwake() || sleep_requested_) return;
    last_activity_ = esp_timer_get_time();
    switch (action) {
        case WatchUi::Action::Activity: break;
        case WatchUi::Action::StartChat:
            if (!network_enabled_) {
                display_.ShowNotification("请先开启 WLAN 或移动数据");
            } else if (!services_.Snapshot().reminders.empty()) {
                display_.ShowNotification("请先关闭提醒");
            } else if (app.GetDeviceState() == kDeviceStateIdle) {
                if (stop_chat_requested_ || chat_start_pending_ || chat_start_submitted_) break;
                stop_chat_requested_ = false;
                chat_stop_submitted_ = false;
                chat_abort_submitted_ = false;
                chat_start_pending_ = true;
                const uint32_t generation = ++chat_request_generation_;
                app.Schedule([this, generation]() {
                    if (!chat_start_pending_ || generation != chat_request_generation_) return;
                    chat_start_pending_ = false;
                    auto& application = Application::GetInstance();
                    if (application.GetDeviceState() != kDeviceStateIdle ||
                        !services_.Snapshot().reminders.empty()) return;
                    chat_start_submitted_ = true;
                    application.ToggleChatState();
                    // Application processes toggle events before the next scheduled
                    // batch. Keep a pending stop alive until that event has run.
                    application.Schedule([this]() {
                        chat_start_submitted_ = false;
                        ContinueStopChat();
                    });
                });
            } else if (!ChatState(app.GetDeviceState())) {
                display_.ShowNotification("正在准备网络，请稍候");
            }
            break;
        case WatchUi::Action::StopChat: StopChat(); break;
        case WatchUi::Action::SetBrightness:
            board_.GetBacklight()->SetBrightness(std::clamp(value, 5, 100), true);
            ApplyPowerSettings();
            break;
        case WatchUi::Action::SetVolume:
            board_.GetAudioCodec()->SetOutputVolume(std::clamp(value, 0, 100));
            break;
        case WatchUi::Action::SetNetwork:
            if (change_network_ && !ringing_) change_network_(value);
            break;
        case WatchUi::Action::StartWifiConfig:
            if (configure_network_ && !ringing_) configure_network_();
            break;
        case WatchUi::Action::Sleep:
            RequestSleep();
            break;
        case WatchUi::Action::PowerOff: RequestPowerChange(false); break;
        case WatchUi::Action::Reboot:
            RequestPowerChange(true);
            break;
        case WatchUi::Action::CheckUpdate:
            if (app.GetDeviceState() == kDeviceStateIdle && !ringing_) app.Reboot();
            else display_.ShowNotification("设备忙，请稍后重试");
            break;
        case WatchUi::Action::FactoryReset: {
            std::string error;
            if (!services_.ResetWatchData(&error)) display_.ShowNotification(error);
            else { ApplyPowerSettings(); display_.ShowNotification("已重置手表数据，Wi-Fi 已保留"); }
            break;
        }
        case WatchUi::Action::SetPowerSave:
        case WatchUi::Action::SetAlwaysOn:
        case WatchUi::Action::SetLanguage: {
            auto settings = services_.Snapshot().settings;
            if (action == WatchUi::Action::SetPowerSave) settings.power_save = value != 0;
            else if (action == WatchUi::Action::SetAlwaysOn) {
                settings.screen_timeout_seconds = value ? 0 : 15;
                if (value) settings.power_save = false;
            } else settings.language = value == 1 ? 1 : 0;
            std::string error;
            if (!services_.SaveSettings(settings, &error)) display_.ShowNotification(error);
            else ApplyPowerSettings();
            break;
        }
        case WatchUi::Action::SetFlashlight:
            flashlight_ = value != 0;
            ApplyPowerSettings();
            break;
        case WatchUi::Action::SettingsChanged: ApplyPowerSettings(); break;
    }
}

void WatchRuntime::StopRing() {
    if (!ringing_) return;
    ringing_ = false;
    auto& app = Application::GetInstance();
#if BAJI_WATCH_LOCAL_AUDIO
    app.EndLocalAudio();
#else
    if (app.GetDeviceState() == kDeviceStateIdle) app.GetAudioService().ResetDecoder();
#endif
    static_cast<BajiAudioCodec*>(board_.GetAudioCodec())->SetAlertVolume(-1);
    if (network_enabled_ && awake_ && !services_.Snapshot().settings.power_save && app.GetDeviceState() == kDeviceStateIdle)
        app.GetAudioService().EnableWakeWordDetection(true);
    last_activity_ = esp_timer_get_time();
}

void WatchRuntime::UpdateReminder(const WatchSnapshot& snapshot, int64_t now) {
    if (snapshot.reminders.empty()) {
        StopRing();
        reminder_token_ = 0;
        reminder_timed_out_ = false;
        return;
    }
    const uint32_t token = snapshot.reminders.back().token;
    if (token != reminder_token_) {
        reminder_token_ = token;
        reminder_timed_out_ = false;
        ring_started_ = now;
        next_tone_ = 0;
        Wake();
    }
    if (ringing_ && now - ring_started_ >= 60000000) {
        StopRing();
        reminder_timed_out_ = true;  // Leave the visible reminder until acknowledged.
    }
    const bool audible = snapshot.settings.alarm_volume > 0 &&
        std::any_of(snapshot.reminders.begin(), snapshot.reminders.end(), [](const auto& item) { return item.audible; });
    if (!audible && now - ring_started_ >= 60000000) reminder_timed_out_ = true;
    if (!audible || reminder_timed_out_) { StopRing(); return; }
    auto& app = Application::GetInstance();
    if (!ringing_) {
#if BAJI_WATCH_LOCAL_AUDIO
        if (!app.BeginLocalAudio()) return;
#else
        // Native-only compatibility: show the alert immediately and wait for AI to finish.
        if (app.GetDeviceState() != kDeviceStateIdle || !app.GetAudioService().IsPlaybackIdle()) return;
        app.GetAudioService().EnableWakeWordDetection(false);
#endif
        ringing_ = true;
        ring_started_ = now;
        static_cast<BajiAudioCodec*>(board_.GetAudioCodec())->SetAlertVolume(snapshot.settings.alarm_volume);
        ESP_LOGI(kTag, "Local reminder ringing");
    }
#if !BAJI_WATCH_LOCAL_AUDIO
    if (app.GetDeviceState() != kDeviceStateIdle) { StopRing(); return; }
#endif
    app.GetAudioService().EnableWakeWordDetection(false);
    if (now >= next_tone_ && app.GetAudioService().IsPlaybackIdle()) {
        app.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        next_tone_ = now + 3000000;
    }
}

void WatchRuntime::Tick() {
    auto& app = Application::GetInstance();
    const int64_t now = esp_timer_get_time();
    if (power_transition_) {
        ContinueStopChat();
        if (power_reboot_ && now >= power_reboot_at_) app.Reboot();
        return;
    }
    const auto snapshot = services_.Snapshot();
    UpdateWifiState();
    if (applied_power_save_ != snapshot.settings.power_save) ApplyPowerSettings();
    const auto state = app.GetDeviceState();
    if (static_cast<int>(state) != last_state_) {
        last_state_ = state;
        if (ChatState(state)) {
            if (!awake_ || sleep_requested_) StopChat();
            else if (!stop_chat_requested_) display_.ShowWatchChat();
        }
    }
    ContinueStopChat();
    UpdateReminder(snapshot, now);
    if (sleep_requested_ && !stop_chat_requested_ && !chat_start_submitted_) Sleep();
    if ((!network_enabled_ || !awake_ || snapshot.settings.power_save) && state == kDeviceStateIdle)
        app.GetAudioService().EnableWakeWordDetection(false);
    if (!ScreenCanSleep(state) || (!snapshot.reminders.empty() && !reminder_timed_out_)) last_activity_ = now;
    const int timeout = snapshot.settings.screen_timeout_seconds;
    if (awake_ && timeout && now - last_activity_ >= int64_t(timeout) * 1000000) Sleep();
    if (now - last_ui_tick_ < 1000000) return;
    last_ui_tick_ = now;
    WatchUi::DeviceSnapshot device;
    bool discharging = false;
    board_.GetBatteryLevel(device.battery, device.charging, discharging);
    device.volume = board_.GetAudioCodec()->output_volume();
    // Report the saved brightness while asleep, rather than moving the slider to zero.
    Settings display_settings("display");
    device.brightness = std::clamp<int>(display_settings.GetInt("brightness", 75), 5, 100);
    const bool cellular = static_cast<DualNetworkBoard&>(board_).GetNetworkType() == NetworkType::ML307;
    device.network = network_enabled_ ? (cellular ? "4G" : "Wi-Fi") : "无网络";
    if (network_enabled_ && !connected_) device.network += " 未连接";
    auto& wifi = WifiManager::GetInstance();
    device.wifi_connected = network_enabled_ && !cellular && wifi.IsConnected();
    if (device.wifi_connected) device.wifi_ssid = wifi.GetSsid();
    device.chat_active = ChatState(app.GetDeviceState());
    device.firmware = esp_app_get_description()->version;
    charging_ = device.charging;
    display_.TickWatch(device);
}

void WatchRuntime::ApplyPowerSettings() {
    const auto settings = services_.Snapshot().settings;
    applied_power_save_ = settings.power_save;
    if (awake_) {
        Settings display_settings("display");
        const int brightness = std::clamp<int>(display_settings.GetInt("brightness", 75), 5, 100);
        board_.GetBacklight()->SetBrightness(flashlight_ ? 100 : settings.power_save ? std::min(brightness, 35) : brightness);
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle) {
        board_.SetPowerSaveLevel(settings.power_save || !awake_ ? PowerSaveLevel::LOW_POWER : PowerSaveLevel::BALANCED);
        if (started_ && !ringing_) app.GetAudioService().EnableWakeWordDetection(network_enabled_ && awake_ && !settings.power_save);
    }
}

bool WatchRuntime::LaunchWifiWorker(std::function<void()> work) {
    auto* task = new std::function<void()>(std::move(work));
    if (xTaskCreate([](void* arg) {
        std::unique_ptr<std::function<void()>> callback(static_cast<std::function<void()>*>(arg));
        (*callback)();
        callback.reset();
        vTaskDelete(nullptr);
    }, "watch_wifi", 6144, task, 2, nullptr) != pdPASS) {
        delete task;
        wifi_worker_busy_ = false;
        return false;
    }
    return true;
}

void WatchRuntime::RequestWifiScan() {
    if (!IsAwake() || sleep_requested_) return;
    last_activity_ = esp_timer_get_time();
    if (!network_enabled_) { display_.SetWatchWifiNetworks({}, false, "请先开启 WLAN"); return; }
    if (static_cast<DualNetworkBoard&>(board_).GetNetworkType() != NetworkType::WIFI) {
        display_.SetWatchWifiNetworks({}, false, "请先切换 Wi-Fi 模式");
        return;
    }
    auto& manager = WifiManager::GetInstance();
    if (!manager.IsInitialized()) { display_.SetWatchWifiNetworks({}, false, "Wi-Fi 正在初始化"); return; }
    if (wifi_worker_busy_.exchange(true)) return;
    if (wifi_connecting_ || ChatState(Application::GetInstance().GetDeviceState())) {
        wifi_worker_busy_ = false;
        display_.SetWatchWifiNetworks(wifi_networks_, false, "请先结束对话或等待连接完成");
        return;
    }
    const bool station_scan = manager.IsConnected() && !manager.IsConfigMode();
    if (station_scan) {
        // The native station consumes SCAN_DONE and attempts a connection. Until
        // a coordinated native scan API is approved, don't launch a competing
        // scan that can alter the selected network. Show only the actual AP.
        wifi_worker_busy_ = false;
        wifi_ap_record_t current{};
        wifi_networks_.clear();
        if (esp_wifi_sta_get_ap_info(&current) == ESP_OK) {
            const std::string ssid(reinterpret_cast<const char*>(current.ssid),
                strnlen(reinterpret_cast<const char*>(current.ssid), sizeof(current.ssid)));
            wifi_networks_.push_back({ssid, current.rssi,
                current.authmode != WIFI_AUTH_OPEN, true});
        }
        display_.SetWatchWifiNetworks(wifi_networks_, false, "附近网络扫描暂不可用，可用手机配网");
        return;
    }
    if (!station_scan && !manager.IsConfigMode()) {
        if (configure_network_) configure_network_();
    }
    display_.SetWatchWifiNetworks(wifi_networks_, true, "");
    if (!LaunchWifiWorker([this]() {
        std::vector<WatchUi::WifiNetwork> networks;
        std::string error;
        if (!WaitForPortal()) {
            error = "配网服务尚未就绪，请稍后重试";
        } else {
            std::string response;
            if (PortalRequest("/scan", nullptr, response, error)) {
                cJSON* json = cJSON_Parse(response.c_str());
                const auto aps = json ? cJSON_GetObjectItemCaseSensitive(json, "aps") : nullptr;
                if (!cJSON_IsArray(aps)) error = "无法读取附近网络，请重试";
                else {
                    cJSON* item = nullptr;
                    cJSON_ArrayForEach(item, aps) {
                        const auto ssid = cJSON_GetObjectItemCaseSensitive(item, "ssid");
                        const auto rssi = cJSON_GetObjectItemCaseSensitive(item, "rssi");
                        const auto auth = cJSON_GetObjectItemCaseSensitive(item, "authmode");
                        if (cJSON_IsString(ssid) && ssid->valuestring && cJSON_IsNumber(rssi))
                            networks.push_back({ssid->valuestring, rssi->valueint, !cJSON_IsNumber(auth) || auth->valueint != WIFI_AUTH_OPEN, false});
                    }
                }
                cJSON_Delete(json);
            }
        }
        SortNetworks(networks);
        Application::GetInstance().Schedule([this, networks = std::move(networks), error]() mutable {
            wifi_worker_busy_ = false;
            if (error.empty()) wifi_networks_ = std::move(networks);
            display_.SetWatchWifiNetworks(wifi_networks_, false, error.c_str());
        });
    })) display_.SetWatchWifiNetworks(wifi_networks_, false, "无法启动扫描，请重试");
}

void WatchRuntime::RequestWifiConnect(std::string ssid, std::string password) {
    if (!IsAwake() || sleep_requested_) return;
    last_activity_ = esp_timer_get_time();
    if (!network_enabled_) { display_.SetWatchWifiConnectionState("failed", "请先开启 WLAN"); return; }
    if (ssid.empty() || ssid.size() > 31 || password.size() > 63 ||
        ssid.find('\0') != std::string::npos || password.find('\0') != std::string::npos) {
        // The existing native provisioning implementation copies at most 31/63
        // bytes; reject longer credentials instead of silently truncating them.
        display_.SetWatchWifiConnectionState("failed", "名称最多31字节，密码最多63字节");
        return;
    }
    if (static_cast<DualNetworkBoard&>(board_).GetNetworkType() != NetworkType::WIFI) {
        display_.SetWatchWifiConnectionState("failed", "请先切换 Wi-Fi 模式");
        return;
    }
    if (wifi_worker_busy_.exchange(true)) {
        display_.SetWatchWifiConnectionState("failed", "正在处理 Wi-Fi，请稍后重试");
        return;
    }
    auto& manager = WifiManager::GetInstance();
    if (!manager.IsInitialized() || wifi_connecting_ || ChatState(Application::GetInstance().GetDeviceState())) {
        wifi_worker_busy_ = false;
        display_.SetWatchWifiConnectionState("failed", "请结束对话或等待 Wi-Fi 就绪");
        return;
    }
    if (manager.IsConnected() && manager.GetSsid() == ssid) {
        wifi_worker_busy_ = false;
        display_.SetWatchWifiConnectionState("connected", "");
        return;
    }
    wifi_target_ = ssid;
    display_.SetWatchWifiConnectionState("connecting", "");
    if (!manager.IsConfigMode() && configure_network_) configure_network_();
    if (!LaunchWifiWorker([this, ssid = std::move(ssid), password = std::move(password)]() mutable {
        std::string error, response;
        bool saved = false;
        if (!HasCredentialSlot(ssid)) error = "已保存10个网络，请先在手机配网页管理网络";
        else if (!WaitForPortal()) error = "配网服务尚未就绪，请重试";
        else {
            cJSON* request = cJSON_CreateObject();
            cJSON_AddStringToObject(request, "ssid", ssid.c_str());
            cJSON_AddStringToObject(request, "password", password.c_str());
            char* json_text = cJSON_PrintUnformatted(request);
            cJSON_Delete(request);
            std::string body = json_text ? json_text : "";
            cJSON_free(json_text);
            if (body.empty()) error = "无法创建连接请求";
            else if (PortalRequest("/submit", &body, response, error)) {
                auto json = cJSON_Parse(response.c_str());
                if (!json || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "success")))
                    error = "连接失败，请检查密码和信号后重试";
                else if (!CredentialsSaved(ssid, password)) error = "连接参数保存失败，请重试";
                else saved = true;
                cJSON_Delete(json);
            }
            std::fill(body.begin(), body.end(), '\0');
        }
        std::fill(password.begin(), password.end(), '\0');
        if (saved) {
            const std::string body = "{}";
            if (!PortalRequest("/exit", &body, response, error)) saved = false;
        }
        Application::GetInstance().Schedule([this, saved, error]() {
            wifi_worker_busy_ = false;
            if (!saved) display_.SetWatchWifiConnectionState("failed", error.c_str());
            else {
                wifi_connecting_ = true;
                wifi_connect_deadline_ = esp_timer_get_time() + 60000000;
                display_.SetWatchWifiConnectionState("connecting", "");
            }
        });
    })) display_.SetWatchWifiConnectionState("failed", "无法启动连接，请重试");
}

void WatchRuntime::UpdateWifiState() {
    if (!wifi_connecting_) return;
    auto& manager = WifiManager::GetInstance();
    if (manager.IsConnected()) {
        wifi_connecting_ = false;
        const auto current = manager.GetSsid();
        bool present = false;
        for (auto& network : wifi_networks_) {
            network.connected = network.ssid == current;
            present |= network.connected;
        }
        if (!present) wifi_networks_.insert(wifi_networks_.begin(), {current, manager.GetRssi(), true, true});
        display_.SetWatchWifiNetworks(wifi_networks_, false, "");
        display_.SetWatchWifiConnectionState("connected", "");
        if (current != wifi_target_) display_.ShowNotification("已连接 " + current + "，目标网络已保存", 5000);
    } else if (esp_timer_get_time() >= wifi_connect_deadline_) {
        wifi_connecting_ = false;
        display_.SetWatchWifiConnectionState("failed", "连接超时，可检查密码后重试");
    }
}

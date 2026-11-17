#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <utility>
#include <memory>

#include <material_symbols.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "BAJI";
    config.language = Lang::CODE;
    config.show_ota_config = true;
    config.show_sleep_config = true;

    // Set a DHCP hostname so the router shows a friendly name instead of "espressif".
    // Uses the same "<prefix>-<last 2 MAC bytes>" scheme as the config AP SSID.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4], mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            if (!config_mode_cancelled_) TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    Application::GetInstance().Schedule([board]() {
        if (board->config_mode_cancelled_ || WifiManager::GetInstance().IsConnected()) return;
        ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");
        WifiManager::GetInstance().StopStation();
        board->StartWifiConfigMode();
    });
}

void WifiBoard::StartWifiConfigMode() {
    config_mode_pending_ = false;
    config_mode_cancelled_ = false;
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    const uint32_t generation = config_mode_generation_.load();
    Application::GetInstance().Schedule([this, &wifi_manager, generation]() {
        if (generation != config_mode_generation_ || config_mode_cancelled_ || !wifi_manager.IsConfigMode()) return;
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    if (IsInWifiConfigMode() || config_mode_pending_) return;
    ESP_LOGI(TAG, "EnterWifiConfigMode called");

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state != kDeviceStateStarting && state != kDeviceStateSpeaking &&
        state != kDeviceStateNotifying && state != kDeviceStateListening && state != kDeviceStateIdle) {
        ESP_LOGE(TAG, "Cannot enter WiFi config mode in device state: %d", state);
        return;
    }
    config_mode_cancelled_ = false;
    const uint32_t generation = ++config_mode_generation_;
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    if (state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
        state == kDeviceStateListening || state == kDeviceStateIdle) {
        config_mode_pending_ = true;
        auto* request = new std::pair<WifiBoard*, uint32_t>(this, generation);
        const auto result = xTaskCreate([](void* arg) {
            std::unique_ptr<std::pair<WifiBoard*, uint32_t>> request(
                static_cast<std::pair<WifiBoard*, uint32_t>*>(arg));
            auto* board = request->first;
            const auto generation = request->second;

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            Application::GetInstance().Schedule([board, generation]() {
                if (!board->config_mode_pending_ || generation != board->config_mode_generation_) return;
                esp_timer_stop(board->connect_timer_);
                WifiManager::GetInstance().StopStation();
                board->StartWifiConfigMode();
            });
            request.reset();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, request, 2, NULL);
        if (result != pdPASS) {
            delete request;
            config_mode_pending_ = false;
            ESP_LOGE(TAG, "Failed to create WiFi config task");
        } else {
            // Reset only after the delayed transition was successfully queued.
            app.ResetProtocol();
        }
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

void WifiBoard::ExitWifiConfigMode() {
    auto& wifi = WifiManager::GetInstance();
    if (!config_mode_pending_ && !wifi.IsConfigMode()) return;
    const uint32_t generation = ++config_mode_generation_;
    config_mode_pending_ = false;
    config_mode_cancelled_ = true;
    esp_timer_stop(connect_timer_);
    wifi.StopConfigAp();
    in_config_mode_ = false;

    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateWifiConfiguring) app.SetDeviceState(kDeviceStateIdle);
    // Finish any queued protocol reset before reporting a still-live connection.
    app.Schedule([this, generation]() {
        if (generation != config_mode_generation_ || !config_mode_cancelled_) return;
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConnected()) OnNetworkEvent(NetworkEvent::Connected, wifi.GetSsid());
        else if (!SsidManager::GetInstance().GetSsidList().empty()) wifi.StartStation();
    });
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (!wifi.IsConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

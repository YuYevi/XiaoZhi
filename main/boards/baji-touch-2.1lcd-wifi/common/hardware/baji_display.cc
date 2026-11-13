#include "baji_display.h"
#include "baji_touch_cst836u.h"
#include "baji_touch_io.h"
#include "touch_sample_buffer.h"

#include "assets.h"
#include "config.h"
#include "lvgl_theme.h"
#include "system_info.h"
#include "resources/charging_icon.h"

#include <esp_lvgl_port.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <esp_err.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <utility>

extern "C" esp_err_t __real_lvgl_port_init(const lvgl_port_cfg_t* cfg);

namespace {

// Match LVGL's 5 ms input timer.  The sampler performs the short I2C read
// outside the LVGL task, so this keeps motion latency bounded without making
// rendering wait on the shared codec bus.
constexpr uint32_t kTouchSamplePeriodMs = 5;
constexpr uint32_t kTouchIdlePeriodMs = 40;
constexpr uint32_t kTouchReleaseTimeoutMs = 90;
constexpr uint32_t kTouchErrorBackoffMs = 100;
constexpr UBaseType_t kTouchSamplerPriority = 6;
constexpr uint32_t kTouchSamplerStackBytes = 3072;

portMUX_TYPE g_touch_lock = portMUX_INITIALIZER_UNLOCKED;
baji::touch::SampleBuffer g_touch_samples;
StaticSemaphore_t g_touch_sem_storage;
StaticSemaphore_t g_touch_idle_storage;
SemaphoreHandle_t g_touch_sem = nullptr;
SemaphoreHandle_t g_touch_idle = nullptr;
StaticTask_t g_touch_task_storage;
StackType_t g_touch_task_stack[kTouchSamplerStackBytes / sizeof(StackType_t)];
TaskHandle_t g_touch_task = nullptr;
esp_lcd_touch_handle_t g_touch_device = nullptr;
std::atomic<bool> g_touch_isr_registered{false};
std::atomic<bool> g_touch_ignore_until_up{false};
std::atomic<bool> g_touch_enabled{false};
int32_t g_touch_max_x = 0;
int32_t g_touch_max_y = 0;

TickType_t TouchWaitTicks(uint32_t ms) {
    return std::max<TickType_t>(1, pdMS_TO_TICKS(ms));
}

bool TouchExpire(int64_t now_us) {
    bool expired = false;
    portENTER_CRITICAL(&g_touch_lock);
    expired = g_touch_samples.Expire(now_us, static_cast<int64_t>(kTouchReleaseTimeoutMs) * 1000);
    portEXIT_CRITICAL(&g_touch_lock);
    if (expired) g_touch_ignore_until_up.store(true);
    return expired;
}

bool TouchStore(uint16_t x, uint16_t y, bool pressed, int64_t now_us) {
    x = std::min<uint16_t>(x, static_cast<uint16_t>(std::max<int32_t>(0, g_touch_max_x)));
    y = std::min<uint16_t>(y, static_cast<uint16_t>(std::max<int32_t>(0, g_touch_max_y)));
    portENTER_CRITICAL(&g_touch_lock);
    const bool overflow = g_touch_samples.Push(x, y, pressed, now_us);
    portEXIT_CRITICAL(&g_touch_lock);
    if (overflow) g_touch_ignore_until_up.store(true);
    return overflow;
}

void IRAM_ATTR TouchInterrupt(esp_lcd_touch_handle_t) {
    BaseType_t higher = pdFALSE;
    if (g_touch_sem != nullptr) {
        xSemaphoreGiveFromISR(g_touch_sem, &higher);
    }
    if (higher == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

bool TouchReadChip(bool from_interrupt) {
    (void)from_interrupt;
    if (g_touch_device == nullptr || esp_lcd_touch_read_data(g_touch_device) != ESP_OK) {
        return false;
    }
    esp_lcd_touch_point_data_t point{};
    uint8_t point_count = 0;
    if (esp_lcd_touch_get_data(g_touch_device, &point, &point_count, 1) != ESP_OK) {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    if (g_touch_ignore_until_up.load()) {
        if (point_count != 0 && point.track_id == BAJI_CST836U_EVENT_UP) {
            g_touch_ignore_until_up.store(false);
        }
        if (point_count == 0) {
            bool confirmed_release = false;
            portENTER_CRITICAL(&g_touch_lock);
            confirmed_release = g_touch_samples.ObserveNoContact(now_us);
            portEXIT_CRITICAL(&g_touch_lock);
            if (confirmed_release) g_touch_ignore_until_up.store(false);
            return true;
        }
        // Ignore a finger already on the glass when the display wakes. This
        // prevents the wake transition from becoming an accidental tap.
        if (g_touch_ignore_until_up.load()) return true;
    }
    if (point_count != 0) {
        switch (point.track_id) {
        case BAJI_CST836U_EVENT_DOWN:
        case BAJI_CST836U_EVENT_CONTACT:
            TouchStore(point.x, point.y, true, now_us);
            break;
        case BAJI_CST836U_EVENT_UP:
            // CST836U reports UP with count=1 and the last coordinate.  The
            // stock CST816S adapter drops the event bits and turns this into
            // another PRESSED sample, which is the source of stuck/ghost
            // gestures on this controller.
            TouchStore(point.x, point.y, false, now_us);
            break;
        default:
            // The custom driver rejects unknown events, but keep this guard at
            // the LVGL boundary so a malformed frame cannot become a press.
            TouchExpire(now_us);
            break;
        }
    } else {
        portENTER_CRITICAL(&g_touch_lock);
        g_touch_samples.ObserveNoContact(now_us);
        portEXIT_CRITICAL(&g_touch_lock);
    }
    return true;
}

void TouchSamplerTask(void*) {
    unsigned failures = 0;
    unsigned retries = 0;
    for (;;) {
        if (!g_touch_enabled.load()) {
            xSemaphoreGive(g_touch_idle);
            xSemaphoreTake(g_touch_sem, portMAX_DELAY);
            failures = retries = 0;
            continue;
        }
        portENTER_CRITICAL(&g_touch_lock);
        bool pressed = g_touch_samples.Current().pressed;
        portEXIT_CRITICAL(&g_touch_lock);
        const uint32_t wait_ms = failures >= 3 ? kTouchErrorBackoffMs :
            (pressed || retries || g_touch_ignore_until_up.load() ? kTouchSamplePeriodMs : kTouchIdlePeriodMs);
        const bool from_interrupt = xSemaphoreTake(g_touch_sem, TouchWaitTicks(wait_ms)) == pdTRUE;
        if (!g_touch_enabled.load()) {
            // A pause can interrupt the periodic wait. Acknowledge it here so
            // the owner can safely tear down or pause the touch device.
            xSemaphoreGive(g_touch_idle);
            continue;
        }
        TouchExpire(esp_timer_get_time());
        portENTER_CRITICAL(&g_touch_lock);
        pressed = g_touch_samples.Current().pressed;
        portEXIT_CRITICAL(&g_touch_lock);
        const bool interrupt_active = g_touch_device != nullptr &&
            g_touch_device->config.int_gpio_num != GPIO_NUM_NC &&
            gpio_get_level(g_touch_device->config.int_gpio_num) == g_touch_device->config.levels.interrupt;
        if (from_interrupt || pressed || retries != 0 || interrupt_active ||
            g_touch_ignore_until_up.load()) {
            if (TouchReadChip(from_interrupt)) {
                failures = retries = 0;
            } else {
                if (failures < 3) ++failures;
                if (from_interrupt) retries = 3;
                else if (retries != 0) --retries;
                // A shared-bus NACK must not spin the sampler or make LVGL
                // wait on I2C. Expiry still releases a lost gesture.
                vTaskDelay(TouchWaitTicks(kTouchSamplePeriodMs));
            }
        }
        TouchExpire(esp_timer_get_time());
    }
}

void TouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    portENTER_CRITICAL(&g_touch_lock);
    const bool expired = g_touch_samples.Expire(
        esp_timer_get_time(), static_cast<int64_t>(kTouchReleaseTimeoutMs) * 1000);
    const auto sample = g_touch_samples.Pop();
    data->continue_reading = g_touch_samples.Pending();
    portEXIT_CRITICAL(&g_touch_lock);
    if (expired) g_touch_ignore_until_up.store(true);
    if (sample.cancelled) {
        // Timeout and FIFO overflow abort the gesture. Reset the active LVGL
        // object in the input task before the RELEASED state can emit CLICKED.
        lv_indev_reset(indev, nullptr);
    }
    data->point.x = sample.x;
    data->point.y = sample.y;
    data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void PauseTouchSampler(bool ignore_until_up = false);
bool ResumeTouchSampler();
void StopTouchSampler();

bool StartTouchSampler(esp_lcd_touch_handle_t touch, lv_indev_t* indev,
                       int32_t max_x, int32_t max_y) {
    if (touch == nullptr || indev == nullptr || g_touch_device != nullptr) return false;
    g_touch_device = touch;
    g_touch_max_x = max_x;
    g_touch_max_y = max_y;
    portENTER_CRITICAL(&g_touch_lock);
    g_touch_samples.Clear();
    portEXIT_CRITICAL(&g_touch_lock);
    if (g_touch_task == nullptr) {
        g_touch_sem = xSemaphoreCreateBinaryStatic(&g_touch_sem_storage);
        g_touch_idle = xSemaphoreCreateBinaryStatic(&g_touch_idle_storage);
        g_touch_task = xTaskCreateStatic(TouchSamplerTask, "baji_touch",
                                         kTouchSamplerStackBytes / sizeof(StackType_t), nullptr,
                                         kTouchSamplerPriority, g_touch_task_stack, &g_touch_task_storage);
        if (g_touch_task == nullptr) {
            g_touch_device = nullptr;
            return false;
        }
    }
    g_touch_enabled.store(false);
    if (touch->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_intr_disable(touch->config.int_gpio_num);
        // The LCD TE transport installs the shared GPIO ISR service before
        // this display initializes its touch input. Do not install it again:
        // IDF logs ESP_ERR_INVALID_STATE at ERROR level for that harmless call.
        touch->config.interrupt_callback = TouchInterrupt;
        const esp_err_t ret = gpio_isr_handler_add(touch->config.int_gpio_num,
                                                    reinterpret_cast<gpio_isr_t>(TouchInterrupt), touch);
        if (ret != ESP_OK) {
            touch->config.interrupt_callback = nullptr;
            g_touch_device = nullptr;
            return false;
        }
        g_touch_isr_registered = true;
    }
    lv_indev_set_read_cb(indev, TouchReadCb);
    lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
    if (lv_timer_t* timer = lv_indev_get_read_timer(indev)) {
        lv_timer_set_period(timer, kTouchSamplePeriodMs);
    }
    g_touch_ignore_until_up.store(false);
    if (!ResumeTouchSampler()) {
        StopTouchSampler();
        return false;
    }
    return true;
}

void PauseTouchSampler(bool ignore_until_up) {
    if (g_touch_task == nullptr || g_touch_device == nullptr) return;
    g_touch_enabled.store(false);
    if (g_touch_device->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_intr_disable(g_touch_device->config.int_gpio_num);
    }
    // Drain a previous acknowledgement before waiting for the acknowledgement
    // belonging to this pause request. This joins any in-flight I2C call.
    xSemaphoreTake(g_touch_idle, 0);
    xSemaphoreGive(g_touch_sem);
    xSemaphoreTake(g_touch_idle, portMAX_DELAY);
    portENTER_CRITICAL(&g_touch_lock);
    g_touch_samples.Clear();
    if (ignore_until_up) g_touch_samples.SuppressUntilRelease();
    portEXIT_CRITICAL(&g_touch_lock);
    if (ignore_until_up) g_touch_ignore_until_up = true;
}

bool ResumeTouchSampler() {
    if (g_touch_task == nullptr || g_touch_device == nullptr) return false;
    if (g_touch_enabled.load()) return true;
    g_touch_enabled.store(true);
    if (g_touch_device->config.int_gpio_num != GPIO_NUM_NC) {
        const esp_err_t ret = gpio_intr_enable(g_touch_device->config.int_gpio_num);
        if (ret != ESP_OK) {
            g_touch_enabled.store(false);
            return false;
        }
    }
    xSemaphoreGive(g_touch_sem);
    return true;
}

void StopTouchSampler() {
    if (g_touch_device == nullptr) return;
    PauseTouchSampler();
    if (g_touch_isr_registered && g_touch_device->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_isr_handler_remove(g_touch_device->config.int_gpio_num);
        g_touch_isr_registered = false;
    }
    g_touch_device->config.interrupt_callback = nullptr;
    g_touch_device = nullptr;
}

}  // namespace

extern "C" esp_err_t __wrap_lvgl_port_init(const lvgl_port_cfg_t* cfg) {
    // The watch rebuilds nested pages from LVGL input callbacks. Reserve room
    // for that event/layout stack without changing the native display class.
    auto watch_cfg = *cfg;
    watch_cfg.task_stack = std::max(watch_cfg.task_stack, 16 * 1024);
    // Resume promptly at TE/DMA completion; audio processing keeps its higher priority.
    watch_cfg.task_priority = std::max(watch_cfg.task_priority, 4);
    // ESP_LOGI("BajiDisplay", "Watch LVGL task stack: %d bytes", watch_cfg.task_stack);
    return __real_lvgl_port_init(&watch_cfg);
}

BajiDisplay::BajiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y,
                         bool mirror_x, bool mirror_y, bool swap_xy, bool quiet_boot)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy), quiet_boot_(quiet_boot) {
    DisplayLockGuard lock(this);
    auto screen = lv_display_get_screen_active(display_);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

BajiDisplay::~BajiDisplay() {
    DisplayLockGuard lock(this);
    watch_ui_.reset();
    if (touch_indev_) {
        StopTouchSampler();
        lv_indev_delete(touch_indev_);
        touch_indev_ = nullptr;
    }
    if (touch_) esp_lcd_touch_del(touch_);
    if (touch_io_) esp_lcd_panel_io_del(touch_io_);
}

void BajiDisplay::InitializeTouch(i2c_master_bus_handle_t i2c_bus) {
    DisplayLockGuard lock(this);
    if (touch_indev_) return;

    // The touch controller shares I2C0 with ES8311/TCA9554.  Use fast mode;
    // the dedicated adapter bounds every transaction to 20 ms.
    ESP_ERROR_CHECK(baji_touch_new_i2c_io(i2c_bus, &touch_io_));
    const esp_lcd_touch_config_t touch_config = {
        .x_max = DISPLAY_WIDTH - 1,
        .y_max = DISPLAY_HEIGHT - 1,
        // The shared reset was already pulsed before LCD initialization.
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
        },
    };
    ESP_ERROR_CHECK(baji_touch_new_i2c_cst836u(touch_io_, &touch_config, &touch_));
    ESP_ERROR_CHECK(gpio_set_pull_mode(TOUCH_INT_GPIO, GPIO_PULLUP_ONLY));
    // Keep I2C reads out of the LVGL task.  The sampler is woken by INT and
    // continues at 5 ms while pressed; LVGL only consumes its bounded FIFO.
    touch_indev_ = lv_indev_create();
    ESP_ERROR_CHECK(touch_indev_ ? ESP_OK : ESP_ERR_NO_MEM);
    lv_indev_set_type(touch_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_disp(touch_indev_, display_);
    ESP_ERROR_CHECK(StartTouchSampler(touch_, touch_indev_, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1)
                        ? ESP_OK : ESP_ERR_INVALID_STATE);
    lv_indev_add_event_cb(touch_indev_, [](lv_event_t* event) {
        static_cast<BajiDisplay*>(lv_event_get_user_data(event))->OnTouchPressed();
    }, LV_EVENT_PRESSED, this);
    // ESP_LOGI("BajiDisplay", "CST836U touch registered: I2C 0x%02X, 400kHz, INT GPIO%d, sampler=%ums",
    //          TOUCH_I2C_ADDRESS, TOUCH_INT_GPIO,
    //          static_cast<unsigned>(kTouchSamplePeriodMs));
}

void BajiDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (quiet_boot_ || setup_ui_called_) return;

    LoadPackagedTextFont();

    // Native theme/font updates still reference these objects. Keep them alive,
    // but let the board's watch root own the visible screen and touch handling.
    SpiLcdDisplay::SetupUI();
    auto screen = lv_display_get_screen_active(display_);
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_add_flag(lv_obj_get_child(screen, i), LV_OBJ_FLAG_HIDDEN);
    }
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    // Native scrolling labels remain hidden and do not need animation timers.
    if (status_label_) lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    if (chat_message_label_) lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_CLIP);

    auto theme = static_cast<LvglTheme*>(current_theme_);
    watch_ui_ = std::make_unique<WatchUi>(display_, theme->text_font()->font(),
        theme->large_icon_font()->font(), WatchServices::GetInstance(),
        [this](WatchUi::Action action, int value) {
            if (watch_action_) watch_action_(action, value);
        });
    watch_ui_->Create();
    watch_ui_->SetWifiCallbacks(watch_wifi_scan_, watch_wifi_connect_);
    watch_ui_->SetAwake(watch_awake_);
    lv_refr_now(display_);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0));
    if (first_frame_callback_) {
        auto callback = std::move(first_frame_callback_);
        callback();
    }
}

void BajiDisplay::LoadPackagedTextFont() {
    // The native application applies assets after networking. A watch must
    // display its Chinese menus offline too. Reuse the already packaged font,
    // without starting the audio models before AudioService::Initialize().
    void* asset_data = nullptr;
    size_t asset_size = 0;
    auto& assets = Assets::GetInstance();
    if (assets.GetAssetData("index.json", asset_data, asset_size)) {
        auto index = cJSON_ParseWithLength(static_cast<const char*>(asset_data), asset_size);
        auto font = cJSON_GetObjectItem(index, "text_font");
        if (cJSON_IsString(font) && assets.GetAssetData(font->valuestring, asset_data, asset_size)) {
            auto text_font = std::make_shared<LvglCBinFont>(asset_data);
            if (text_font->font() && SetTextFont(text_font)) {
                // ESP_LOGI("BajiDisplay", "Loaded native font for offline watch UI");
            }
        }
        cJSON_Delete(index);
    }
}

void BajiDisplay::SetFirstFrameCallback(std::function<void()> callback) {
    DisplayLockGuard lock(this);
    first_frame_callback_ = std::move(callback);
}

void BajiDisplay::RefreshNow() {
    DisplayLockGuard lock(this);
    lv_refr_now(display_);
    // The SPI parameter transaction drains queued color transfers first.
    // A negative command with no parameters sends nothing to the panel.
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0));
}

void BajiDisplay::SetTheme(Theme* theme) {
    if (!theme) return;
    DisplayLockGuard lock(this);
    if (setup_ui_called_) {
        SpiLcdDisplay::SetTheme(theme);
    } else {
        Display::SetTheme(theme);
    }
    if (watch_ui_) {
        auto lvgl_theme = static_cast<LvglTheme*>(theme);
        // SetTextFont retains the previous font owner until this override returns.
        watch_ui_->SetFonts(lvgl_theme->text_font()->font(), lvgl_theme->large_icon_font()->font());
    }
    RefreshChargingStyle();
}

void BajiDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetStatus(status ? status : "");
}

void BajiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!watch_ui_) return;
    if (role && std::strcmp(role, "system") == 0) {
        if (content && SystemInfo::GetUserAgent() == content) return;
        // Activation codes, provisioning instructions and errors must also be
        // visible outside the conversation page. During boot an empty message
        // clears stale setup instructions; later it preserves notifications.
        if ((content && content[0]) || watch_ui_->IsBooting())
            watch_ui_->SetSystemMessage(content ? content : "", 15000);
    } else {
        watch_ui_->SetChatMessage(role ? role : "assistant", content ? content : "");
    }
}

void BajiDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (!watch_ui_) return;
    watch_ui_->ClearChatMessages();
}

void BajiDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    // The watch owns its conversation animation. Keep the native GIF stopped.
    if (watch_ui_) watch_ui_->SetEmotion(emotion ? emotion : "neutral");
}

void BajiDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void BajiDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (watch_ui_ && notification && notification[0]) {
        watch_ui_->SetSystemMessage(notification, std::max(duration_ms, 0));
    }
}

void BajiDisplay::UpdateStatusBar(bool update_all) {
    DisplayLockGuard lock(this);
    // The board supplies a cached DeviceSnapshot to TickWatch; keep network
    // state reads out of the native display clock tick.
    // Charging-only startup also reaches this method without Board/Application.
    (void)update_all;
}

void BajiDisplay::SetWatchActionCallback(WatchUi::ActionCallback callback) {
    DisplayLockGuard lock(this);
    watch_action_ = std::move(callback);
}

void BajiDisplay::SetWatchAwake(bool awake) {
    DisplayLockGuard lock(this);
    if (watch_awake_ == awake) return;
    if (touch_indev_) {
        if (!awake) {
            PauseTouchSampler(true);
            lv_indev_wait_release(touch_indev_);
        } else {
            // A contact held through wake is discarded until its UP report.
            g_touch_ignore_until_up = true;
            if (!ResumeTouchSampler()) {
                ESP_LOGE("BajiDisplay", "Touch sampler could not resume after wake");
            }
        }
    }
    watch_awake_ = awake;
    if (watch_ui_) watch_ui_->SetAwake(awake);
}

void BajiDisplay::TickWatch(const WatchUi::DeviceSnapshot& snapshot) {
    DisplayLockGuard lock(this);
    if (!watch_ui_) return;
    watch_ui_->UpdateDeviceSnapshot(snapshot);
    watch_ui_->Tick();
}

void BajiDisplay::ShowWatchChat() {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->ShowChat(false);
}

void BajiDisplay::WatchBack() {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->GoBack();
}

bool BajiDisplay::IsWatchChat() {
    DisplayLockGuard lock(this);
    return watch_ui_ && watch_ui_->IsChat();
}

void BajiDisplay::UpdateBootState(DeviceState state) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->UpdateBootState(state);
}

bool BajiDisplay::IsWatchBooting() {
    DisplayLockGuard lock(this);
    return !watch_ui_ || watch_ui_->IsBooting();
}

void BajiDisplay::ShowPowerMenu(bool show) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->ShowPowerMenu(show);
}

bool BajiDisplay::IsPowerMenuOpen() {
    DisplayLockGuard lock(this);
    return watch_ui_ && watch_ui_->IsPowerMenuOpen();
}

void BajiDisplay::ShowPowerTransition(bool reboot) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->ShowPowerTransition(reboot);
}

void BajiDisplay::SetWatchWifiCallbacks(std::function<void()> scan,
    std::function<void(std::string, std::string)> connect) {
    DisplayLockGuard lock(this);
    watch_wifi_scan_ = std::move(scan);
    watch_wifi_connect_ = std::move(connect);
    if (watch_ui_) watch_ui_->SetWifiCallbacks(watch_wifi_scan_, watch_wifi_connect_);
}

void BajiDisplay::SetWatchWifiNetworks(const std::vector<WatchUi::WifiNetwork>& networks,
    bool scanning, const char* error) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetWifiNetworks(networks, scanning, error ? error : "");
}

void BajiDisplay::SetWatchWifiConnectionState(const char* state, const char* error) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetWifiConnectionState(state ? state : "", error ? error : "");
}

void BajiDisplay::OnTouchPressed() {
    DisplayLockGuard lock(this);
    if (quiet_boot_) return;
    if (!watch_awake_) {
        // Sleeping touch never reaches an object or emits an activity action.
        // Keep suppressing until release, including a power-key wake mid-touch.
        lv_indev_wait_release(touch_indev_);
        lv_indev_stop_processing(touch_indev_);
        return;
    }
    if (watch_action_) watch_action_(WatchUi::Action::Activity, 0);
}

void BajiDisplay::RefreshChargingStyle() {
    if (!charging_fullscreen_) return;
    const lv_font_t* text_font = LV_FONT_DEFAULT;
    if (auto theme = static_cast<LvglTheme*>(current_theme_)) {
        text_font = theme->text_font()->font();
    }
    // Rebind on theme changes before a replaced runtime font is released.
    lv_obj_set_style_text_font(charging_fullscreen_, text_font, 0);
    lv_obj_align(charging_icon_, LV_ALIGN_CENTER, 0, -lv_font_get_line_height(text_font));
    lv_obj_align_to(charging_caption_, charging_icon_, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}

void BajiDisplay::ShowChargingFullscreen(bool show) {
    DisplayLockGuard lock(this);
    if (!show) {
        if (charging_fullscreen_) lv_obj_add_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!charging_fullscreen_) {
        auto& services = WatchServices::GetInstance();
        services.Initialize();
        charging_english_ = services.Snapshot().settings.language != 0;
        LoadPackagedTextFont();
        auto screen = lv_display_get_screen_active(display_);
        charging_fullscreen_ = lv_obj_create(screen);
        lv_obj_set_size(charging_fullscreen_, width_, height_);
        lv_obj_set_style_radius(charging_fullscreen_, 0, 0);
        lv_obj_set_style_bg_color(charging_fullscreen_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(charging_fullscreen_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(charging_fullscreen_, lv_color_white(), 0);
        lv_obj_set_style_border_width(charging_fullscreen_, 0, 0);
        lv_obj_set_style_pad_all(charging_fullscreen_, 0, 0);
        lv_obj_center(charging_fullscreen_);
        lv_obj_remove_flag(charging_fullscreen_, LV_OBJ_FLAG_SCROLLABLE);

        charging_icon_ = lv_image_create(charging_fullscreen_);
        lv_image_set_src(charging_icon_, &baji_charging_icon);
        lv_obj_set_style_image_recolor(charging_icon_, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(charging_icon_, LV_OPA_COVER, 0);
        charging_caption_ = lv_label_create(charging_fullscreen_);
        lv_label_set_text(charging_caption_, charging_english_ ? "Charging" : "正在充电");
        lv_obj_set_width(charging_caption_, width_ * 85 / 100);
        lv_label_set_long_mode(charging_caption_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(charging_caption_, LV_TEXT_ALIGN_CENTER, 0);
    }
    RefreshChargingStyle();
    lv_obj_remove_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(charging_fullscreen_);
}

void BajiDisplay::UpdateChargingState(bool usb, bool charging, bool full) {
    DisplayLockGuard lock(this);
    if (!charging_fullscreen_) return;
    const char* caption = full && usb
        ? (charging_english_ ? "Charging complete" : "充电完成")
        : charging ? (charging_english_ ? "Charging" : "正在充电")
        : (charging_english_ ? "Power connected" : "已接电源");
    if (std::strcmp(lv_label_get_text(charging_caption_), caption) == 0) return;
    lv_label_set_text(charging_caption_, caption);
    RefreshChargingStyle();
}

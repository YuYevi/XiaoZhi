#include "dual_network_board.h"
#include "application.h"
#include "config.h"
#include "hardware/baji_audio_codec.h"
#include "hardware/baji_display.h"
#include "power/power_manager.h"
#include "watch/watch_runtime.h"
#include "assets/lang_config.h"
#include "settings.h"

#include <iot_button.h>
#include <button_types.h>
#include <at_uart.h>
#include <algorithm>
#include <atomic>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_io_expander_tca9554.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <cstdio>

namespace {

constexpr uint64_t LCD_OPCODE_READ_CMD = 0x03ULL;

static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {

    {0xfe, (uint8_t []){0x00}, 0, 0},
    {0xef, (uint8_t []){0x00}, 0, 0},
    {0x80, (uint8_t []){0x19}, 1, 0},
    {0x82, (uint8_t []){0x09}, 1, 0},
    {0x83, (uint8_t []){0x03}, 1, 0},
    {0x88, (uint8_t []){0x00}, 1, 0},
    {0x89, (uint8_t []){0x38}, 1, 0},
    {0x8A, (uint8_t []){0x40}, 1, 0},
    {0x8B, (uint8_t []){0x0A}, 1, 0},
    {0x8C, (uint8_t []){0x00}, 1, 0},
    {0x81, (uint8_t []){0xFF}, 1, 0},
    {0x84, (uint8_t []){0xFF}, 1, 0},
    {0x85, (uint8_t []){0xFF}, 1, 0},
    {0x86, (uint8_t []){0xFF}, 1, 0},
    {0x87, (uint8_t []){0xFF}, 1, 0},
    {0x8E, (uint8_t []){0xFF}, 1, 0},
    {0x8F, (uint8_t []){0xFF}, 1, 0},
    {0x98, (uint8_t []){0x3E}, 1, 0},
    {0x99, (uint8_t []){0x3E}, 1, 0},
    {0x7D, (uint8_t []){0x72}, 1, 0},
    {0x70, (uint8_t []){0x02,0x03,0x03,0x06,0x03,0x03,0x09,0x07,0x09,0x03}, 10, 0},
    {0x90, (uint8_t []){0x06,0x06,0x01,0x01}, 4, 0},
    {0x93, (uint8_t []){0x02,0xFF,0x00}, 3, 0},
    {0xCB, (uint8_t []){0x02}, 1, 0},
    {0xFB, (uint8_t []){0x00,0x00}, 2, 0},
    {0xF6, (uint8_t []){0xC0}, 1, 0},
    {0x6C, (uint8_t []){0x00,0x00,0x22,0x00,0xCC,0x04,0x58}, 7, 0},
    {0xAA, (uint8_t []){0x0B,0x00}, 2, 0},
    {0xEC, (uint8_t []){0x07}, 1, 0},
    {0xF9, (uint8_t []){0x40}, 1, 0},
    {0xEB, (uint8_t []){0x01,0x67}, 2, 0},
    {0x74, (uint8_t []){0x01, 0x60, 0x00, 0x00, 0x00, 0x00}, 6, 0},

    {0xB5, (uint8_t []){0x14, 0x14, 0x14}, 3, 0},

    {0x6E, (uint8_t []){
        0x0B, 0x0B, 0x09, 0x09, 0x13, 0x13, 0x11, 0x11,
        0x16, 0x15, 0x01, 0x04, 0x00, 0x0D, 0x1D, 0x00,
        0x00, 0x1D, 0x0D, 0x00, 0x04, 0x08, 0x15, 0x16,
        0x12, 0x12, 0x14, 0x14, 0x0A, 0x0A, 0x0C, 0x0C
    }, 32, 0},

    {0x60, (uint8_t []){0x38, 0x1C, 0x13, 0x56}, 4, 0},

    {0x61, (uint8_t []){0xF8, 0x0A, 0x13, 0x56}, 4, 0},

    {0x62, (uint8_t []){0xF8, 0x0B, 0x13, 0x56}, 4, 0},

    {0x63, (uint8_t []){0x38, 0x1C, 0x13, 0x56}, 4, 0},

    {0x64, (uint8_t []){0x38, 0x20, 0x72, 0xF8, 0x13, 0x56}, 6, 0},

    {0x65, (uint8_t []){0x78, 0x1A, 0x70, 0x0B, 0x56, 0x13}, 6, 0},

    {0x66, (uint8_t []){0x38, 0x24, 0x72, 0xFC, 0x13, 0x56}, 6, 0},

    {0x68, (uint8_t []){0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A}, 7, 0},

    {0x69, (uint8_t []){0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A}, 7, 0},

    {0x6A, (uint8_t []){0x00, 0x00}, 2, 0},

    {0x3A, (uint8_t []){0x55}, 1, 0},

    {0x36, (uint8_t []){0x00}, 1, 0},

    {0x7C, (uint8_t []){0xB6, 0x29}, 2, 0},

    {0xAC, (uint8_t []){0x40}, 1, 0},

    {0xC3, (uint8_t []){0x1A}, 1, 0},
    {0xC4, (uint8_t []){0x24}, 1, 0},
    {0xC9, (uint8_t []){0x2F}, 1, 0},

    {0xF0, (uint8_t []){0x11, 0x17, 0x08, 0x06, 0x05, 0x38}, 6, 0},

    {0xF1, (uint8_t []){0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F}, 6, 0},

    {0xF2, (uint8_t []){0x11, 0x17, 0x08, 0x06, 0x05, 0x38}, 6, 0},

    {0xF3, (uint8_t []){0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F}, 6, 0},

    {0xB4, (uint8_t []){0x0A}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},

    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xEE, (uint8_t []){0x00}, 0, 0},

    {0x11, (uint8_t []){0x00}, 0, 20},

    {0x29, (uint8_t []){0x00}, 0, 20},
};

static bool s_baji_spi_bus_inited = false;
static i2c_master_bus_handle_t g_baji185_i2c_bus = nullptr;
static esp_io_expander_handle_t g_baji185_io_expander = nullptr;

#if BAJI185_RUN_LED_AUTO_BLINK
static esp_timer_handle_t g_baji185_run_led_timer = nullptr;
#endif

i2c_master_bus_handle_t baji_185_get_i2c_bus(void)
{
    if (g_baji185_i2c_bus == nullptr) {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &g_baji185_i2c_bus));
    }
    return g_baji185_i2c_bus;
}

esp_io_expander_handle_t baji_185_get_io_expander(void)
{
    return g_baji185_io_expander;
}

esp_err_t baji_185_set_io_level(uint32_t mask, bool high) {
    static std::mutex output_mutex;
    std::lock_guard<std::mutex> lock(output_mutex);
    return esp_io_expander_set_level(g_baji185_io_expander, mask, high ? 1 : 0);
}

#if BAJI185_RUN_LED_AUTO_BLINK
static void baji185_run_led_timer_cb(void* arg)
{
    (void)arg;
    static bool on;
    on = !on;
    baji_185_set_io_level(BAJI185_IOX_PIN_MASK_RUN_LED, on);
}
#endif

void baji_185_ensure_io_expander(void)
{
    if (g_baji185_io_expander != nullptr) {
        return;
    }
    baji_185_get_i2c_bus();
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(g_baji185_i2c_bus, TCA9554_I2C_ADDR, &g_baji185_io_expander));

    const uint32_t in_pins = BAJI185_IOX_PIN_MASK_VOL_DOWN | BAJI185_IOX_PIN_MASK_VOL_UP;
    const uint32_t out_pins = BAJI185_IOX_PIN_MASK_4G_PWRON | BAJI185_IOX_PIN_MASK_4G_RST |
                              BAJI185_IOX_PIN_MASK_PA | BAJI185_IOX_PIN_MASK_RUN_LED | BAJI185_IOX_PIN_MASK_LCD_RST;
    ESP_ERROR_CHECK(esp_io_expander_set_dir(g_baji185_io_expander, in_pins, IO_EXPANDER_INPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_dir(g_baji185_io_expander, out_pins, IO_EXPANDER_OUTPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_4G_PWRON, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_4G_RST, 1));

#ifdef AUDIO_CODEC_PA_INVERTED
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_PA, AUDIO_CODEC_PA_INVERTED ? 1 : 0));
#else
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_PA, 0));
#endif
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_RUN_LED, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_LCD_RST, 1));

#if BAJI185_RUN_LED_AUTO_BLINK
    if (g_baji185_run_led_timer == nullptr) {
        const esp_timer_create_args_t targs = {
            .callback = baji185_run_led_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "baji185_run_led",
        };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &g_baji185_run_led_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(g_baji185_run_led_timer, 500000));
    }
#endif
}

void baji_185_initialize_spi(void)
{
    if (s_baji_spi_bus_inited) {
        return;
    }

    const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                            QSPI_PIN_NUM_LCD_DATA0,
                                                                            QSPI_PIN_NUM_LCD_DATA1,
                                                                            QSPI_PIN_NUM_LCD_DATA2,
                                                                            QSPI_PIN_NUM_LCD_DATA3,
                                                                            QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    s_baji_spi_bus_inited = true;
}

static void ResetLcd(void)
{
    baji_185_ensure_io_expander();
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(120));
}

}  // namespace

BajiDisplay* baji_185_create_lcd_display(bool quiet_boot)
{
    baji_185_ensure_io_expander();
    baji_185_initialize_spi();
    ResetLcd();

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = QSPI_PIN_NUM_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    if (!quiet_boot) {
        printf("-------------------------------------- Version selection -------------------------------------- \r\n");
    }
    esp_err_t ret;
    int lcd_cmd = 0x04;
    uint8_t register_data[3] = {};
    size_t param_size = sizeof(register_data);
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= LCD_OPCODE_READ_CMD << 24;
    ret = esp_lcd_panel_io_rx_param(panel_io, lcd_cmd, register_data, param_size);
    if (!quiet_boot) {
        if (ret == ESP_OK) {
            printf("Register 0x04 data: %02x %02x %02x\n", register_data[0], register_data[1], register_data[2]);
        } else {
            printf("Failed to read register 0x04, error code: %d\n", ret);
        }
    }

    if (register_data[0] == 0x00 && register_data[1] == 0x7F && register_data[2] == 0x7F) {
        if (!quiet_boot) {
            printf("Vendor-specific initialization for case 1.\n");
        }
    } else if (register_data[0] == 0xFF && register_data[1] == 0xFF && register_data[2] == 0xFF) {
        vendor_config.init_cmds = vendor_specific_init_new;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(st77916_lcd_init_cmd_t);
        if (!quiet_boot) {
            printf("Vendor-specific initialization for case 2.\n");
        }
    }
    if (!quiet_boot) {
        printf("------------------------------------- End of version selection------------------------------------- \r\n");
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    return new BajiDisplay(panel_io, panel,
                             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                             DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, quiet_boot);
}


namespace {
uint8_t VolumeUpLevel(button_driver_t*) {
    uint32_t level = BAJI185_IOX_PIN_MASK_VOL_UP;
    if (esp_io_expander_get_level(baji_185_get_io_expander(), BAJI185_IOX_PIN_MASK_VOL_UP, &level) != ESP_OK) return 0;
    return (level & BAJI185_IOX_PIN_MASK_VOL_UP) == 0;
}
uint8_t VolumeDownLevel(button_driver_t*) {
    uint32_t level = BAJI185_IOX_PIN_MASK_VOL_DOWN;
    if (esp_io_expander_get_level(baji_185_get_io_expander(), BAJI185_IOX_PIN_MASK_VOL_DOWN, &level) != ESP_OK) return 0;
    return (level & BAJI185_IOX_PIN_MASK_VOL_DOWN) == 0;
}
}

class BajiBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    bool switching_ = false;
    bool network_offline_ = false;
    std::atomic<int64_t> confirm_until_{0};
    BajiDisplay* display_ = nullptr;
    PowerManager* power_ = nullptr;
    WatchRuntime* watch_ = nullptr;
    button_driver_t button_drivers_[3] = {};
    button_handle_t buttons_[3] = {};

    void InitializeI2c() {
        i2c_bus_ = baji_185_get_i2c_bus();
    }

    void InitializeTca9554() {
        baji_185_ensure_io_expander();
    }

    void InitializeSpi() {
        baji_185_initialize_spi();
    }

    void InitializeSt77916Display() {
        display_ = baji_185_create_lcd_display();
        ESP_ERROR_CHECK(display_ ? ESP_OK : ESP_FAIL);
    }

    void SetExpander(uint32_t mask, bool high) {
        ESP_ERROR_CHECK(baji_185_set_io_level(mask, high));
    }
    void PulseModemPower() {
        SetExpander(BAJI185_IOX_PIN_MASK_4G_PWRON, true);
        vTaskDelay(pdMS_TO_TICKS(2000));
        SetExpander(BAJI185_IOX_PIN_MASK_4G_PWRON, false);
    }
    void InitializeModemPower() {
        SetExpander(BAJI185_IOX_PIN_MASK_4G_RST, true);
        // PWRKEY toggles the module. Check its state before pulsing after an ESP reset.
        const bool enable = !network_offline_ && GetNetworkType() == NetworkType::ML307;
        bool powered;
        {
            AtUart probe(UART_4G_TXD, UART_4G_RXD, UART0_DTR);
            probe.Initialize();
            ESP_ERROR_CHECK(probe.IsInitialized() ? ESP_OK : ESP_FAIL);
            powered = probe.SetBaudRate(921600, 3000);
            // Restore normal mode if a previous firmware left the module in flight mode.
            if (powered && enable && !probe.SendCommand("AT+CFUN=1")) {
                ESP_LOGW("BajiBoard", "Modem did not acknowledge normal mode");
            }
        } // Release the probe UART before the native Ml307Board starts its own task.
        if (powered != enable) {
            PulseModemPower();
            vTaskDelay(pdMS_TO_TICKS(enable ? 1500 : 500));
        }
    }
    void RequestNetworkSwitch() {
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        if (switching_ || state == kDeviceStateActivating || state == kDeviceStateUpgrading ||
            state == kDeviceStateNotifying || state == kDeviceStateAudioTesting || state == kDeviceStateFatalError || state == kDeviceStateUnknown) {
            display_->ShowNotification("设备忙，请稍后切换网络");
            return;
        }
        if (!SaveOfflineMode(false)) return;
        switching_ = true;
        SwitchNetworkType();
    }
    bool SaveOfflineMode(bool offline) {
        nvs_handle_t handle = 0;
        esp_err_t result = nvs_open("watch", NVS_READWRITE, &handle);
        if (result == ESP_OK) {
            result = nvs_set_u8(handle, "net_off", offline ? 1 : 0);
            if (result == ESP_OK) result = nvs_commit(handle);
            nvs_close(handle);
        }
        if (result != ESP_OK) display_->ShowNotification("网络设置保存失败，请重试");
        return result == ESP_OK;
    }
    void SelectWatchNetwork(int requested) {
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        if (switching_ || (state != kDeviceStateIdle && state != kDeviceStateStarting &&
                           state != kDeviceStateWifiConfiguring)) {
            display_->ShowNotification("请结束对话或等待系统任务完成");
            return;
        }
        const bool offline = requested < 0;
        if (!offline && ((requested == 1) != (GetNetworkType() == NetworkType::ML307))) {
            RequestNetworkSwitch();
        } else if (offline != network_offline_ && SaveOfflineMode(offline)) {
            switching_ = true;
            app.Reboot();
        }
    }
    void ChangeVolume(int delta) {
        if (!watch_->IsAwake()) return;
        auto* codec = GetAudioCodec();
        const int volume = std::clamp(codec->output_volume() + delta, 0, 100);
        codec->SetOutputVolume(volume);
        display_->ShowNotification(std::string(Lang::Strings::VOLUME) + std::to_string(volume));
    }
    void InitializeButtons() {
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&config));
        button_drivers_[0].get_key_level = [](button_driver_t*) -> uint8_t { return !gpio_get_level(BOOT_BUTTON_GPIO); };
        button_drivers_[1].get_key_level = VolumeUpLevel;
        button_drivers_[2].get_key_level = VolumeDownLevel;
        for (int i = 0; i < 3; ++i) {
            const button_config_t config = { .long_press_time = static_cast<uint16_t>(i == 1 ? 3000 : 2000), .short_press_time = 0 };
            ESP_ERROR_CHECK(iot_button_create(&config, &button_drivers_[i], &buttons_[i]));
        }
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[0], BUTTON_SINGLE_CLICK, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() {
                if (self->switching_ || !self->watch_->IsAwake()) return;
                if (Application::GetInstance().GetDeviceState() == kDeviceStateStarting && self->GetNetworkType() == NetworkType::WIFI) {
                    static_cast<WifiBoard&>(self->GetCurrentBoard()).EnterWifiConfigMode();
                } else if (self->GetNetworkType() == NetworkType::ML307 &&
                           (Application::GetInstance().GetDeviceState() == kDeviceStateStarting ||
                            Application::GetInstance().GetDeviceState() == kDeviceStateWifiConfiguring)) {
                    self->display_->ShowNotification("正在连接4G，请稍候");
                } else {
                    self->watch_->Back();
                }
            });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[0], BUTTON_DOUBLE_CLICK, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() { self->watch_->Chat(); });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[1], BUTTON_SINGLE_CLICK, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() {
                if (!self->watch_->IsAwake()) return;
                const int64_t deadline = self->confirm_until_.exchange(0);
                if (deadline > esp_timer_get_time()) self->RequestNetworkSwitch();
                else self->ChangeVolume(10);
            });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[1], BUTTON_LONG_PRESS_START, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() {
                if (!self->watch_->IsAwake()) return;
                self->confirm_until_ = esp_timer_get_time() + 5000000;
                self->display_->ShowNotification(self->GetNetworkType() == NetworkType::WIFI ? "切换4G？短按音量+确认" : "切换WiFi？短按音量+确认", 2000);
            });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[2], BUTTON_SINGLE_CLICK, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() { self->ChangeVolume(-10); });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[2], BUTTON_LONG_PRESS_START, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() {
                if (!self->watch_->IsAwake()) return;
                self->GetAudioCodec()->SetOutputVolume(0);
                self->display_->ShowNotification(Lang::Strings::MUTED);
            });
        }, this));
    }

    void InitializeWatch() {
        watch_ = new WatchRuntime(*this, *display_);
        watch_->SetNetworkEnabled(!network_offline_);
        watch_->Initialize();
        watch_->SetPowerOffAction([this]() { power_->shutdown(); });
        power_->OnPowerKey([this](PowerKeyEvent event) {
            Application::GetInstance().Schedule([this, event]() {
                if (event == PowerKeyEvent::Tap) watch_->PowerTap();
                else watch_->PowerLongPress();
            });
        });
        watch_->SetNetworkActions([this](int requested) {
            SelectWatchNetwork(requested);
        }, [this]() {
            if (network_offline_) { display_->ShowNotification("请先开启 WLAN"); return; }
            const auto state = Application::GetInstance().GetDeviceState();
            if (GetNetworkType() == NetworkType::WIFI &&
                (state == kDeviceStateIdle || state == kDeviceStateStarting))
                static_cast<WifiBoard&>(GetCurrentBoard()).EnterWifiConfigMode();
            else if (GetNetworkType() == NetworkType::WIFI && state == kDeviceStateWifiConfiguring)
                return;
            else display_->ShowNotification("请先切换 Wi-Fi，并结束当前对话");
        });
        power_->OnLowBatteryStatusChanged([this](bool low) {
            if (!low) return;
            Application::GetInstance().Schedule([this]() {
                if (!power_->IsCharging()) display_->ShowNotification("电量偏低，请及时充电", 10000);
            });
        });
    }

public:
    BajiBoard() : DualNetworkBoard(UART_4G_TXD, UART_4G_RXD, UART0_DTR, 0) {
        network_offline_ = Settings("watch").GetBool("net_off", false);
        // Complete BAJI's power-on gate before initializing screen and audio.
        power_ = new PowerManager(POWER_USB_IN);
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        InitializeSt77916Display();
        display_->InitializeTouch(i2c_bus_);
        InitializeWatch();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }
    AudioCodec* GetAudioCodec() override {
        static BajiAudioCodec codec(i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
#if AUDIO_INPUT_USE_SILICON_MIC
            AUDIO_I2S_GPIO_DIN_ANALOG,
#else
            AUDIO_I2S_GPIO_DIN,
#endif
            [this](bool enabled) { SetExpander(BAJI185_IOX_PIN_MASK_PA, enabled != static_cast<bool>(AUDIO_CODEC_PA_INVERTED)); },
            AUDIO_CODEC_ES8311_ADDR);
        return &codec;
    }
    Display* GetDisplay() override { return display_; }
    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        level = power_->GetBatteryLevel();
        charging = power_->IsCharging();
        discharging = power_->IsDischarging();
        return true;
    }
    void StartNetwork() override {
        // Application calls this after board and display initialization completes.
        power_->Start();
        watch_->Start();
        InitializeModemPower();
        if (network_offline_) {
            // Keep the original network objects untouched. A restart owns their
            // lifecycle, so disabling either radio never races a native task.
            // The native state machine requires Starting -> Activating -> Idle.
            // No network event is emitted, so no server activation job starts.
            Application::GetInstance().SetDeviceState(kDeviceStateActivating);
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            ESP_LOGI("BajiBoard", "Networks disabled; local watch remains available");
        } else {
            DualNetworkBoard::StartNetwork();
        }
    }
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (watch_ && level != PowerSaveLevel::LOW_POWER) watch_->OnPerformanceRequested();
        DualNetworkBoard::SetPowerSaveLevel(level);
    }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        DualNetworkBoard::SetNetworkEventCallback([this, callback](NetworkEvent event, const std::string& data) {
            if (watch_) watch_->OnNetworkEvent(event);
            if (callback) callback(event, data);
        });
    }
};

DECLARE_BOARD(BajiBoard);

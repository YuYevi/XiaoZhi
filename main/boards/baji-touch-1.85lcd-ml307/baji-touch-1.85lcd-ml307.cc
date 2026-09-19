#include "wifi_board.h"
#include "application.h"
#include "config.h"
#include "common/baji_audio_codec.h"
#include "common/baji_display.h"
#include "common/baji_ml307.h"
#include "common/power_manager.h"
#include "power_save_timer.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <iot_button.h>
#include <button_types.h>
#include <wifi_manager.h>
#include <algorithm>
#include <atomic>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_timer.h>
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

class BajiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    enum class NetworkMode { Wifi, Ml307 };
    std::atomic<NetworkMode> network_mode_{NetworkMode::Wifi};
    std::atomic<bool> switching_{false};
    std::atomic<int64_t> confirm_until_{0};
    std::atomic<std::shared_ptr<BajiMl307>> modem_;
    std::atomic<bool> accept_network_events_{false};
    std::atomic<uint32_t> network_generation_{0};
    BajiDisplay* display_ = nullptr;
    PowerManager* power_ = nullptr;
    PowerSaveTimer* sleep_ = nullptr;
    NetworkEventCallback callback_;
    bool modem_powered_ = false;
    bool prefer_cellular_ = false;
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
    std::shared_ptr<BajiMl307> CurrentModem() const {
        return modem_.load();
    }
    void ForwardNetworkEvent(NetworkMode source, uint32_t generation, NetworkEvent event, const std::string& data) {
        if (!accept_network_events_ || network_mode_ != source || network_generation_ != generation) return;
        Application::GetInstance().Schedule([this, source, generation, event, data]() {
            if (accept_network_events_ && network_mode_ == source && network_generation_ == generation && callback_) {
                callback_(event, data);
            }
        });
    }
    void PulseModemPower() {
        SetExpander(BAJI185_IOX_PIN_MASK_4G_PWRON, true);
        vTaskDelay(pdMS_TO_TICKS(2000));
        SetExpander(BAJI185_IOX_PIN_MASK_4G_PWRON, false);
    }
    void StartCellular() {
        SetExpander(BAJI185_IOX_PIN_MASK_4G_RST, true);
        // A software reset may leave the modem powered. Avoid toggling it off.
        auto detected = AtModem::Detect(UART_4G_TXD, UART_4G_RXD, UART0_DTR, 115200, 1200);
        if (!detected) detected = AtModem::Detect(UART_4G_TXD, UART_4G_RXD, UART0_DTR, 921600, 1200);
        modem_powered_ = detected != nullptr;
        detected.reset();
        if (!modem_powered_) {
            PulseModemPower();
            vTaskDelay(pdMS_TO_TICKS(1500));
            modem_powered_ = true;
        }
        auto modem = std::make_shared<BajiMl307>(UART_4G_TXD, UART_4G_RXD, UART0_DTR);
        const auto generation = network_generation_.load();
        modem->SetNetworkEventCallback([this, generation](NetworkEvent event, const std::string& data) {
            ForwardNetworkEvent(NetworkMode::Ml307, generation, event, data);
        });
        modem_.store(modem);
        network_mode_ = NetworkMode::Ml307;
        accept_network_events_ = true;
        modem->StartNetwork();
    }
    void StopCellular() {
        if (auto modem = CurrentModem()) modem->StopNetwork();
        if (modem_powered_) {
            PulseModemPower();
            vTaskDelay(pdMS_TO_TICKS(500));
            modem_powered_ = false;
        }
        // Select Wi-Fi before releasing the modem queried by status updates.
        network_mode_ = NetworkMode::Wifi;
        // Retain the disconnected modem until it can be replaced on the next start.
    }
    void StopWifi() {
        esp_timer_stop(connect_timer_);
        auto& wifi = WifiManager::GetInstance();
        wifi.SetEventCallback(nullptr);
        wifi.StopStation();
        wifi.StopConfigAp();
        in_config_mode_ = false;
    }
    void RunNetworkChange(NetworkMode target, bool initial) {
        if (target == NetworkMode::Ml307) {
            if (!initial) StopWifi();
            // Close the previous UART before probing it again.
            auto previous = modem_.exchange({});
            while (previous && previous.use_count() > 1) vTaskDelay(pdMS_TO_TICKS(20));
            previous.reset();
            StartCellular();
        } else {
            if (!initial) {
                StopCellular();
            } else {
                auto probe = AtModem::Detect(UART_4G_TXD, UART_4G_RXD, UART0_DTR, 115200, 1200);
                const bool alive = probe != nullptr;
                probe.reset();
                if (alive) PulseModemPower();
            }
            network_mode_ = NetworkMode::Wifi;
            const auto generation = network_generation_.load();
            WifiBoard::SetNetworkEventCallback([this, generation](NetworkEvent event, const std::string& data) {
                ForwardNetworkEvent(NetworkMode::Wifi, generation, event, data);
            });
            accept_network_events_ = true;
            WifiBoard::StartNetwork();
        }
        switching_ = false;
    }
    void LaunchNetworkChange(NetworkMode target, bool initial) {
        struct Context { BajiBoard* board; NetworkMode target; bool initial; };
        auto* context = new Context{this, target, initial};
        if (xTaskCreate([](void* arg) {
                auto* context = static_cast<Context*>(arg);
                const auto job = *context;
                delete context;
                job.board->RunNetworkChange(job.target, job.initial);
                vTaskDelete(nullptr);
            }, "baji_network", 8192, context, 5, nullptr) != pdPASS) {
            delete context;
            switching_ = false;
            display_->ShowNotification("网络切换任务创建失败");
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
        switching_ = true;
        accept_network_events_ = false;
        ++network_generation_;
        const auto target = network_mode_ == NetworkMode::Wifi ? NetworkMode::Ml307 : NetworkMode::Wifi;
        if (state == kDeviceStateSpeaking) app.AbortSpeaking(kAbortReasonNone);
        if (state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring) {
            app.SetDeviceState(kDeviceStateIdle);
        }
        // ResetProtocol queues cleanup. Queue the switch behind it on the same
        // main loop, so no protocol still owns a socket on the old interface.
        app.ResetProtocol();
        app.Schedule([this, target]() {
            Application::GetInstance().Schedule([this, target]() {
                auto& app = Application::GetInstance();
                // The current application restarts activation from this supported
                // state after Connected, rebuilding the protocol for the new network.
                app.SetDeviceState(kDeviceStateWifiConfiguring);
                Settings settings("network", true);
                settings.SetInt("type", target == NetworkMode::Ml307 ? 1 : 0);
                display_->ShowNotification(target == NetworkMode::Ml307 ? Lang::Strings::SWITCH_TO_4G_NETWORK : Lang::Strings::SWITCH_TO_WIFI_NETWORK);
                LaunchNetworkChange(target, false);
            });
        });
    }
    void ChangeVolume(int delta) {
        sleep_->WakeUp();
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
                self->sleep_->WakeUp();
                if (self->switching_) return;
                if (self->network_mode_ == NetworkMode::Ml307) {
                    if (auto modem = self->CurrentModem()) modem->StartNetwork();
                }
                if (Application::GetInstance().GetDeviceState() == kDeviceStateStarting && self->network_mode_ == NetworkMode::Wifi) {
                    self->EnterWifiConfigMode();
                } else if (self->network_mode_ == NetworkMode::Ml307 &&
                           (Application::GetInstance().GetDeviceState() == kDeviceStateStarting ||
                            Application::GetInstance().GetDeviceState() == kDeviceStateWifiConfiguring)) {
                    self->display_->ShowNotification("正在连接4G，请稍候");
                } else {
                    Application::GetInstance().ToggleChatState();
                }
            });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[1], BUTTON_SINGLE_CLICK, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() {
                const int64_t deadline = self->confirm_until_.exchange(0);
                if (deadline > esp_timer_get_time()) self->RequestNetworkSwitch();
                else self->ChangeVolume(10);
            });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[1], BUTTON_LONG_PRESS_START, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            self->confirm_until_ = esp_timer_get_time() + 5000000;
            self->display_->ShowNotification(self->network_mode_ == NetworkMode::Wifi ? "切换4G？短按音量+确认" : "切换WiFi？短按音量+确认", 2000);
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[2], BUTTON_SINGLE_CLICK, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() { self->ChangeVolume(-10); });
        }, this));
        ESP_ERROR_CHECK(iot_button_register_cb(buttons_[2], BUTTON_LONG_PRESS_START, nullptr, [](void*, void* context) {
            auto* self = static_cast<BajiBoard*>(context);
            Application::GetInstance().Schedule([self]() {
                self->sleep_->WakeUp();
                self->GetAudioCodec()->SetOutputVolume(0);
                self->display_->ShowNotification(Lang::Strings::MUTED);
            });
        }, this));
    }

    void InitializePowerSaveTimer() {
        sleep_ = new PowerSaveTimer(-1, 60, 300);
        sleep_->OnEnterSleepMode([this]() {
            display_->SetChatMessage("system", "");
            display_->SetEmotion("sleepy");
            GetBacklight()->SetBrightness(1);
        });
        sleep_->OnExitSleepMode([this]() {
            display_->SetChatMessage("system", "");
            display_->SetEmotion("neutral");
            GetBacklight()->RestoreBrightness();
        });
        sleep_->OnShutdownRequest([this]() { power_->shutdown(); });
        power_->OnChargingStatusChanged([this](bool charging) { sleep_->SetEnabled(!charging); });
        power_->OnPowerUi([this](PowerUiHint) { display_->ShowNotification("正在关机...", 2000); });
    }

public:
    BajiBoard() {
        Settings settings("network");
        prefer_cellular_ = settings.GetInt("type", 1) == 1;
        // Complete BAJI's power-on gate before initializing screen and audio.
        power_ = new PowerManager(POWER_USB_IN);
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        InitializeSt77916Display();
        InitializePowerSaveTimer();
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
        sleep_->SetEnabled(power_->IsDischarging());
        switching_ = true;
        LaunchNetworkChange(prefer_cellular_ ? NetworkMode::Ml307 : NetworkMode::Wifi, true);
    }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        callback_ = std::move(callback);

    }
    NetworkInterface* GetNetwork() override {
        auto modem = CurrentModem();
        return network_mode_ == NetworkMode::Ml307 && modem ? modem->GetNetwork() : WifiBoard::GetNetwork();
    }
    const char* GetNetworkStateIcon() override {
        auto modem = CurrentModem();
        return network_mode_ == NetworkMode::Ml307 && modem ? modem->GetNetworkStateIcon() : WifiBoard::GetNetworkStateIcon();
    }
    std::string GetBoardType() override {
        return network_mode_ == NetworkMode::Ml307 ? "ml307" : "wifi";
    }
    std::string GetBoardJson() override {
        auto modem = CurrentModem();
        return network_mode_ == NetworkMode::Ml307 && modem ? modem->GetBoardJson() : WifiBoard::GetBoardJson();
    }
    std::string GetDeviceStatusJson() override {
        auto modem = CurrentModem();
        return network_mode_ == NetworkMode::Ml307 && modem ? modem->GetDeviceStatusJson() : WifiBoard::GetDeviceStatusJson();
    }
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) sleep_->WakeUp();
        auto modem = CurrentModem();
        if (network_mode_ == NetworkMode::Ml307 && modem) modem->SetPowerSaveLevel(level);
        else WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(BajiBoard);

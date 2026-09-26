#include "baji_audio_codec.h"
#include "config.h"

#include <esp_log.h>
#include <freertos/task.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <utility>

#ifndef AUDIO_INPUT_USE_SILICON_MIC
#define AUDIO_INPUT_USE_SILICON_MIC 0
#endif
#ifndef AUDIO_MIC_PCM_RIGHT_SHIFT
#define AUDIO_MIC_PCM_RIGHT_SHIFT 12
#endif

namespace {
constexpr char kTag[] = "BajiAudioCodec";
#if AUDIO_INPUT_USE_SILICON_MIC
constexpr int kSampleBits = 32;
static_assert(AUDIO_MIC_PCM_RIGHT_SHIFT >= 0 && AUDIO_MIC_PCM_RIGHT_SHIFT < 32,
    "AUDIO_MIC_PCM_RIGHT_SHIFT must be between 0 and 31");
#else
constexpr int kSampleBits = 16;
#endif
}

BajiAudioCodec::BajiAudioCodec(void* i2c_master_handle, i2c_port_t i2c_port,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din,
    std::function<void(bool)> set_pa_enabled, uint8_t es8311_addr, bool use_mclk)
    : set_pa_enabled_(std::move(set_pa_enabled)) {
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;
    assert(input_sample_rate_ == output_sample_rate_);

    if (set_pa_enabled_) {
        set_pa_enabled_(false);
    }
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != nullptr);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != nullptr);
    ResetCodec();

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != nullptr);
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = GPIO_NUM_NC;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(codec_if_ != nullptr);
    ESP_LOGI(kTag, "ES8311 initialized, microphone input: %s",
        AUDIO_INPUT_USE_SILICON_MIC ? "external digital" : "ES8311 ADC");
}

BajiAudioCodec::~BajiAudioCodec() {
    if (set_pa_enabled_) {
        set_pa_enabled_(false);
    }
    if (dev_ != nullptr) {
        esp_codec_dev_close(dev_);
        esp_codec_dev_delete(dev_);
    }
    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
    }
    if (rx_handle_ != nullptr) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
    }
}

void BajiAudioCodec::ResetCodec() {
    // Keep the reset delay used by the current native ES8311 implementation.
    uint8_t reset_value = 0x1F;
    ESP_ERROR_CHECK(static_cast<esp_err_t>(
        ctrl_if_->write_reg(ctrl_if_, 0x00, 1, &reset_value, 1)));
    vTaskDelay(pdMS_TO_TICKS(5));
}

void BajiAudioCodec::UpdateDeviceState() {
    // Disable the amplifier before closing the DAC or changing its clocks.
    // The previous reverse order exposed the DAC power-down transient.
    if ((!output_enabled_ || shutdown_prepared_) && set_pa_enabled_) {
        set_pa_enabled_(false);
    }
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != nullptr);

        // esp_codec_dev_open also reconfigures I2S. Digital microphones send
        // 24 valid bits in 32-bit slots, so the opened device must retain that
        // width; Write converts Xiaozhi's 16-bit playback PCM to match it.
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = kSampleBits,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = static_cast<uint32_t>(input_sample_rate_),
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
#if !AUDIO_INPUT_USE_SILICON_MIC
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, input_gain_));
#endif
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, alert_volume_ >= 0 ? alert_volume_ : output_volume_));
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_close(dev_));
        esp_codec_dev_delete(dev_);
        dev_ = nullptr;
    }
    if (set_pa_enabled_) {
        set_pa_enabled_(output_enabled_ && !shutdown_prepared_);
    }
}

void BajiAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk,
    gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(output_sample_rate_));
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if AUDIO_INPUT_USE_SILICON_MIC
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
#else
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
#endif
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.gpio_cfg.mclk = mclk;
    std_cfg.gpio_cfg.bclk = bclk;
    std_cfg.gpio_cfg.ws = ws;
    std_cfg.gpio_cfg.dout = dout;
    std_cfg.gpio_cfg.din = din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
#if AUDIO_INPUT_USE_SILICON_MIC
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
}

void BajiAudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    volume = std::clamp(volume, 0, 100);
    if (dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, alert_volume_ >= 0 ? alert_volume_ : volume));
    }
    AudioCodec::SetOutputVolume(volume);
}

void BajiAudioCodec::SetAlertVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    alert_volume_ = volume < 0 ? -1 : std::clamp(volume, 0, 100);
    if (dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, alert_volume_ >= 0 ? alert_volume_ : output_volume_));
    }
}

void BajiAudioCodec::SetInputGain(float gain) {
    if (!std::isfinite(gain)) {
        return;
    }
    std::lock_guard<std::mutex> lock(data_if_mutex_);
#if !AUDIO_INPUT_USE_SILICON_MIC
    if (dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, gain));
    }
#endif
    AudioCodec::SetInputGain(gain);
}

void BajiAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void BajiAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (shutdown_prepared_) {
        if (set_pa_enabled_) {
            set_pa_enabled_(false);
        }
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

void BajiAudioCodec::PrepareForShutdown() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (shutdown_prepared_) {
        return;
    }
    shutdown_prepared_ = true;

    // Use the same lock as Write so an in-flight buffer completes before the
    // mute barrier. Keep capture available until the application stops it.
    if (dev_ != nullptr) {
        // Use the codec mute control rather than changing output_volume_, so
        // the user's persisted volume remains untouched across reboot.
        const int result = esp_codec_dev_set_out_mute(dev_, true);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(kTag, "Codec mute failed during shutdown: %d", result);
        }
    }
    output_enabled_ = false;
    if (set_pa_enabled_) {
        set_pa_enabled_(false);
    }
    // Do not close/reconfigure I2S here. A capture task may still be unwinding;
    // keeping the clocks stable also avoids another DAC power transient.
}

int BajiAudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || dev_ == nullptr || samples <= 0) {
        return 0;
    }
#if AUDIO_INPUT_USE_SILICON_MIC
    input_buffer_.resize(samples);
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle_, input_buffer_.data(),
        samples * sizeof(int32_t), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Microphone read failed: %s", esp_err_to_name(err));
        return 0;
    }
    int count = bytes_read / sizeof(int32_t);
    for (int i = 0; i < count; ++i) {
        int32_t pcm = input_buffer_[i] >> AUDIO_MIC_PCM_RIGHT_SHIFT;
        pcm = std::clamp(pcm, static_cast<int32_t>(INT16_MIN),
            static_cast<int32_t>(INT16_MAX));
        // Preserve BAJI's linear software gain for the external microphone.
        float amplified = pcm * (input_gain_ > 0 ? input_gain_ : 1.0f);
        dest[i] = static_cast<int16_t>(std::clamp(amplified,
            static_cast<float>(INT16_MIN), static_cast<float>(INT16_MAX)));
    }
    return count;
#else
    return esp_codec_dev_read(dev_, dest, samples * sizeof(int16_t)) == ESP_OK
        ? samples : 0;
#endif
}

int BajiAudioCodec::Write(const int16_t* data, int samples) {
    // Keep the device alive through playback while the audio power timer may
    // disable output. Read stays independent so duplex capture can continue.
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (shutdown_prepared_ || !output_enabled_ || dev_ == nullptr || samples <= 0) {
        return 0;
    }
#if AUDIO_INPUT_USE_SILICON_MIC
    output_buffer_.resize(samples);
    for (int i = 0; i < samples; ++i) {
        // Multiplication is defined for negative PCM values as well.
        output_buffer_[i] = static_cast<int32_t>(data[i]) * 65536;
    }
    return esp_codec_dev_write(dev_, output_buffer_.data(),
        samples * sizeof(int32_t)) == ESP_OK ? samples : 0;
#else
    return esp_codec_dev_write(dev_, const_cast<int16_t*>(data),
        samples * sizeof(int16_t)) == ESP_OK ? samples : 0;
#endif
}

#ifndef BAJI_AUDIO_CODEC_H
#define BAJI_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

#include <functional>
#include <mutex>
#include <vector>

// BAJI's amplifier lives on the TCA9554. The board supplies a logical power
// callback so neither the codec driver nor other boards need virtual GPIOs.
class BajiAudioCodec : public AudioCodec {
public:
    BajiAudioCodec(void* i2c_master_handle, i2c_port_t i2c_port,
        int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
        gpio_num_t dout, gpio_num_t din,
        std::function<void(bool)> set_pa_enabled, uint8_t es8311_addr,
        bool use_mclk = true);
    ~BajiAudioCodec() override;

    void SetOutputVolume(int volume) override;
    // Temporary reminder volume; -1 restores media volume without writing NVS.
    void SetAlertVolume(int volume);
    void SetInputGain(float gain) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    esp_codec_dev_handle_t dev_ = nullptr;
    std::function<void(bool)> set_pa_enabled_;
    std::mutex data_if_mutex_;
    int alert_volume_ = -1;
    std::vector<int32_t> input_buffer_;
    std::vector<int32_t> output_buffer_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk,
        gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    void ResetCodec();
    void UpdateDeviceState();
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;
};

#endif

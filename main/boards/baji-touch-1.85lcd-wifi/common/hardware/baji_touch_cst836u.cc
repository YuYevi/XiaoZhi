#include "baji_touch_cst836u.h"
#include "baji_touch_cst836u_protocol.h"

#include <cstdlib>
#include <cstring>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "CST836U";
constexpr uint8_t kDataStartRegister = 0x02;
constexpr uint8_t kChipIdRegister = 0xA7;
constexpr uint8_t kMaximumPoints = 1;
esp_err_t ReadId(esp_lcd_touch_handle_t tp) {
    uint8_t id = 0;
    const esp_err_t result = esp_lcd_panel_io_rx_param(tp->io, kChipIdRegister, &id, sizeof(id));
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "IC id: 0x%02X", id);
    }
    return result;
}

esp_err_t ReadData(esp_lcd_touch_handle_t tp) {
    if (tp == nullptr) return ESP_ERR_INVALID_ARG;

    uint8_t frame[5]{};
    const esp_err_t result = esp_lcd_panel_io_rx_param(tp->io, kDataStartRegister,
                                                       &frame, sizeof(frame));
    if (result != ESP_OK) return result;

    baji::touch::cst836u::Point point{};
    const auto status = baji::touch::cst836u::DecodeFrame(
        frame, tp->config.x_max, tp->config.y_max, point);
    // UP commonly reports count=1 and retains the last coordinate. Preserve
    // that frame as one point and carry the event in track_id; the board
    // sampler turns it into an LVGL release without losing the final point.
    if (status != baji::touch::cst836u::FrameStatus::Point &&
        status != baji::touch::cst836u::FrameStatus::NoContact) {
        ESP_LOGW(kTag, "Rejected frame: status=%u count=%u", static_cast<unsigned>(status),
                 static_cast<unsigned>(frame[0] & 0x0F));
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 0;
        portEXIT_CRITICAL(&tp->data.lock);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t points = status == baji::touch::cst836u::FrameStatus::Point ? 1 : 0;
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = points;
    if (points != 0) {
        tp->data.coords[0].track_id = static_cast<uint8_t>(point.event);
        tp->data.coords[0].x = point.x;
        tp->data.coords[0].y = point.y;
        tp->data.coords[0].strength = 0;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

bool GetData(esp_lcd_touch_handle_t tp, uint16_t* x, uint16_t* y,
             uint16_t* strength, uint8_t* point_count, uint8_t max_points) {
    if (tp == nullptr || x == nullptr || y == nullptr || point_count == nullptr || max_points == 0) {
        return false;
    }
    portENTER_CRITICAL(&tp->data.lock);
    *point_count = tp->data.points > max_points ? max_points : tp->data.points;
    for (uint8_t i = 0; i < *point_count; ++i) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength != nullptr) strength[i] = tp->data.coords[i].strength;
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
    return *point_count != 0;
}

esp_err_t GetTrackIds(esp_lcd_touch_handle_t tp, uint8_t* track_ids, uint8_t point_count) {
    if (tp == nullptr || track_ids == nullptr || point_count > kMaximumPoints) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&tp->data.lock);
    for (uint8_t i = 0; i < point_count; ++i) {
        track_ids[i] = tp->data.coords[i].track_id;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

esp_err_t Delete(esp_lcd_touch_handle_t tp) {
    if (tp == nullptr) return ESP_ERR_INVALID_ARG;
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        // Match esp_lcd_touch_cst816s: only remove an ISR it registered.
        if (tp->config.interrupt_callback != nullptr) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) gpio_reset_pin(tp->config.rst_gpio_num);
    free(tp);
    return ESP_OK;
}

esp_err_t Reset(esp_lcd_touch_handle_t tp) {
    if (tp == nullptr) return ESP_ERR_INVALID_ARG;
    if (tp->config.rst_gpio_num == GPIO_NUM_NC) return ESP_OK;
    esp_err_t result = gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(200));
    result = gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

}  // namespace

extern "C" esp_err_t baji_touch_new_i2c_cst836u(esp_lcd_panel_io_handle_t io,
                                                   const esp_lcd_touch_config_t* config,
                                                   esp_lcd_touch_handle_t* tp) {
    if (io == nullptr || config == nullptr || tp == nullptr) return ESP_ERR_INVALID_ARG;
    *tp = nullptr;

    auto* touch = static_cast<esp_lcd_touch_handle_t>(calloc(1, sizeof(esp_lcd_touch_t)));
    if (touch == nullptr) return ESP_ERR_NO_MEM;
    touch->io = io;
    touch->read_data = ReadData;
    touch->get_xy = GetData;
    touch->get_track_id = GetTrackIds;
    touch->del = Delete;
    touch->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&touch->config, config, sizeof(*config));

    esp_err_t result = ESP_OK;
    if (config->int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_config = {
            .pin_bit_mask = BIT64(config->int_gpio_num),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = config->levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        };
        result = gpio_config(&int_config);
        if (result != ESP_OK) goto fail;
    }
    if (config->rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_config = {
            .pin_bit_mask = BIT64(config->rst_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        result = gpio_config(&rst_config);
        if (result != ESP_OK) goto fail;
    }
    result = Reset(touch);
    if (result != ESP_OK) goto fail;
    result = ReadId(touch);
    if (result != ESP_OK) goto fail;
    *tp = touch;
    return ESP_OK;

fail:
    (void)Delete(touch);
    return result;
}

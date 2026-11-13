#include "baji_touch_io.h"

#include <esp_lcd_panel_io_interface.h>
#include <new>

#include "config.h"

namespace {

constexpr int kTouchI2cTimeoutMs = 20;

struct TouchI2cIo {
    esp_lcd_panel_io_t base{};
    i2c_master_dev_handle_t device = nullptr;
};

esp_err_t Read(esp_lcd_panel_io_t* io, int command, void* data, size_t size) {
    if (io == nullptr || data == nullptr || size == 0 || command < -1 || command > 0xff) {
        return ESP_ERR_INVALID_ARG;
    }

    auto* touch_io = reinterpret_cast<TouchI2cIo*>(io);
    auto* bytes = static_cast<uint8_t*>(data);
    if (command == -1) {
        return i2c_master_receive(touch_io->device, bytes, size, kTouchI2cTimeoutMs);
    }

    const uint8_t address = static_cast<uint8_t>(command);
    return i2c_master_transmit_receive(touch_io->device, &address, 1, bytes, size,
                                       kTouchI2cTimeoutMs);
}

esp_err_t Delete(esp_lcd_panel_io_t* io) {
    if (io == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* touch_io = reinterpret_cast<TouchI2cIo*>(io);
    const esp_err_t result = i2c_master_bus_rm_device(touch_io->device);
    if (result == ESP_OK) {
        delete touch_io;
    }
    return result;
}

esp_err_t UnsupportedWrite(esp_lcd_panel_io_handle_t, int, const void*, size_t) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t UnsupportedCallbacks(esp_lcd_panel_io_handle_t,
                               const esp_lcd_panel_io_callbacks_t*, void*) {
    return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace

esp_err_t baji_touch_new_i2c_io(i2c_master_bus_handle_t bus,
                                esp_lcd_panel_io_handle_t* out) {
    if (out == nullptr || bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = nullptr;

    auto* touch_io = new (std::nothrow) TouchI2cIo;
    if (touch_io == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = TOUCH_I2C_ADDRESS;
    device_config.scl_speed_hz = 400000;
    const esp_err_t result = i2c_master_bus_add_device(bus, &device_config, &touch_io->device);
    if (result != ESP_OK) {
        delete touch_io;
        return result;
    }

    touch_io->base.rx_param = Read;
    touch_io->base.del = Delete;
    touch_io->base.tx_param = UnsupportedWrite;
    touch_io->base.tx_color = UnsupportedWrite;
    touch_io->base.register_event_callbacks = UnsupportedCallbacks;
    *out = &touch_io->base;
    return ESP_OK;
}

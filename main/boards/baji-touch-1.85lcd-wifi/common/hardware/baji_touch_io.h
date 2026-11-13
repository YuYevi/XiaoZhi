#pragma once

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>

// The CST836U shares the codec bus.  The stock esp_lcd I2C panel adapter can
// wait for the shared bus for a long time; this small adapter keeps each touch
// transaction bounded so an audio or expander transfer cannot stall input.
esp_err_t baji_touch_new_i2c_io(i2c_master_bus_handle_t bus,
                                esp_lcd_panel_io_handle_t* out);

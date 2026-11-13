#pragma once

#include <esp_lcd_touch.h>

#ifdef __cplusplus
extern "C" {
#endif

// CST836U uses the CST816S register map but puts a two-bit event code in the
// high bits of the X high byte.  The stock CST816S driver discards those bits.
esp_err_t baji_touch_new_i2c_cst836u(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_touch_config_t* config,
                                     esp_lcd_touch_handle_t* tp);

enum {
    BAJI_CST836U_EVENT_DOWN = 0,
    BAJI_CST836U_EVENT_UP = 1,
    BAJI_CST836U_EVENT_CONTACT = 2,
};

#ifdef __cplusplus
}
#endif

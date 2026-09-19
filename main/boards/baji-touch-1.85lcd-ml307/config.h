#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

#define AUDIO_INPUT_SAMPLE_RATE     24000
#define AUDIO_OUTPUT_SAMPLE_RATE    24000
#define AUDIO_MIC_PCM_RIGHT_SHIFT   12
#define AUDIO_INPUT_REFERENCE       false
#define AUDIO_INPUT_USE_SILICON_MIC 0

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC
#define POWER_USB_IN            GPIO_NUM_5
#define Power_Control           GPIO_NUM_1
#define Power_Dec               GPIO_NUM_2

#define AUDIO_I2S_GPIO_MCLK       GPIO_NUM_45
#define AUDIO_I2S_GPIO_WS         GPIO_NUM_46
#define AUDIO_I2S_GPIO_BCLK       GPIO_NUM_41
#define AUDIO_I2S_GPIO_DIN        GPIO_NUM_40
#define AUDIO_I2S_GPIO_DOUT       GPIO_NUM_42
#define AUDIO_I2S_GPIO_DIN_ANALOG GPIO_NUM_39

#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_6
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_7
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_PA_PIN      AUDIO_CODEC_PA_GPIO_VIRTUAL
#define AUDIO_CODEC_PA_INVERTED 0

#define TCA9554_I2C_ADDR          0x20
#define TCA9554_GPIO_VIRTUAL_BASE 50

#define BAJI185_IOX_PIN_MASK_4G_PWRON (1u << 0)
#define BAJI185_IOX_PIN_MASK_4G_RST   (1u << 1)
#define BAJI185_IOX_PIN_MASK_VOL_UP   (1u << 2)
#define BAJI185_IOX_PIN_MASK_VOL_DOWN (1u << 3)
#define BAJI185_IOX_PIN_MASK_PA       (1u << 5)
#define BAJI185_IOX_PIN_MASK_RUN_LED  (1u << 6)
#define BAJI185_IOX_PIN_MASK_LCD_RST  (1u << 7)

#define ML307_MOD_PWRON_GPIO_VIRTUAL ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 0))
#define ML307_MOD_RST_GPIO_VIRTUAL   ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 1))
#define AUDIO_CODEC_PA_GPIO_VIRTUAL  ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 5))
#define BAJI185_RUN_LED_GPIO_VIRTUAL ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 6))
#define QSPI_PIN_NUM_LCD_RST_VIRTUAL ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 7))
#define BAJI185_RUN_LED_AUTO_BLINK   0

#define UART0_DTR   GPIO_NUM_38
#define UART_4G_RXD GPIO_NUM_48
#define UART_4G_TXD GPIO_NUM_47

#define ML307_ENABLE_EDRX 1
#define ML307_EDRX_ACT    7
#define ML307_EDRX_VALUE  "0011"

#define DISPLAY_WIDTH    360
#define DISPLAY_HEIGHT   360
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define QSPI_LCD_H_RES         (360)
#define QSPI_LCD_V_RES         (360)
#define QSPI_LCD_BIT_PER_PIXEL (16)

#define QSPI_LCD_HOST          SPI2_HOST
#define QSPI_PIN_NUM_LCD_PCLK  GPIO_NUM_12
#define QSPI_PIN_NUM_LCD_CS    GPIO_NUM_10
#define QSPI_PIN_NUM_LCD_DATA0 GPIO_NUM_11
#define QSPI_PIN_NUM_LCD_DATA1 GPIO_NUM_13
#define QSPI_PIN_NUM_LCD_DATA2 GPIO_NUM_14
#define QSPI_PIN_NUM_LCD_DATA3 GPIO_NUM_9
#define QSPI_PIN_NUM_LCD_RST   GPIO_NUM_NC
#define QSPI_PIN_NUM_LCD_BL    GPIO_NUM_15

#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_BACKLIGHT_PIN           QSPI_PIN_NUM_LCD_BL
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define POWER_SHUTDOWN_HOLD_TICKS             13
#define POWER_SHUTDOWN_RELEASE_DEBOUNCE_TICKS 4
#define POWER_CHARGING_FULLSCREEN_BACKLIGHT   5
#define POWER_KEY_HOLD_MS_TO_BOOT             3000
#define POWER_KEY_STABLE_RELEASE_MS           80

#define POWER_CBS_ADC_UNIT        ADC_UNIT_1
#define POWER_BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define POWER_USBIN_ADC_CHANNEL   ADC_CHANNEL_1

#define POWER_CHARGE_DETECT_USE_GPIO 1
#define POWER_USB_VBUS_ACTIVE_LEVEL  1
#define POWER_KEY_LEVEL_WHEN_PRESSED 0
#define POWER_KEY_PRESSED()          (gpio_get_level(Power_Dec) == (POWER_KEY_LEVEL_WHEN_PRESSED))
#define POWER_KEY_RELEASED()         (gpio_get_level(Power_Dec) != (POWER_KEY_LEVEL_WHEN_PRESSED))

#define TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                        \
        .data0_io_num = d0,                                                   \
        .data1_io_num = d1,                                                   \
        .sclk_io_num = sclk,                                                  \
        .data2_io_num = d2,                                                   \
        .data3_io_num = d3,                                                   \
        .max_transfer_sz = max_trans_sz,                                      \
    }

#endif // _BOARD_CONFIG_H_

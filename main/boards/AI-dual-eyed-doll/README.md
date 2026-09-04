# AI-dual-eyed-doll

Custom dual-eye firmware based on XiaoZhi voice conversation. Hardware is **not** pin-compatible with Waveshare ESP32-S3-Touch-LCD-1.85, so this is a unique board identity.

## Hardware

- ESP32-S3R8, **4MB flash**, ES8390 codec (ES8389 driver), LM4890 speaker amp
- Two 0.71" GC9D01N 160x160 SPI panels (shared SCLK/MOSI/DC/RST, separate CS)
- Left eye: CS GPIO12. Right eye: CS GPIO13, hardware-mirrored so both screens share one RGB565 asset
- Official `espressif/esp_lcd_gc9d01` panel driver plus the vendor init sequence used by the working dual-eye firmware
- PA_SD on GPIO18 (KEY_2 removed; R44 DNP). GPIO35 is left to octal PSRAM
- Touch_out3 / Touch_out4 are not enabled (GPIO36/37 are PSRAM pins)

Eyes only show the eight static expressions under `assets/`. The original 1.85" LVGL UI, built-in fonts, and emoji collections are not compiled (`CONFIG_USE_LVGL=n`) and are not flashed (`CONFIG_FLASH_NONE_ASSETS`).

## Controls

| Input | GPIO | Action |
| --- | --- | --- |
| Touch 1 | 8 | Happy face + `Short_laugh.ogg` |
| Touch 2 | 38 | Angry face + `tsundere.ogg` |
| Power | 2 | Short: screen on/off. Long (~2s): power off (`PWR_CTRL` GPIO1) |
| KEY 1 | 17 | Short: toggle AI chat. Long: enter/exit WiFi AP config |
| BOOT | 0 | Unused at runtime (download / reset) |

Wake-word conversation and the original no-SSID boot provisioning path are unchanged. KEY 1 only adds extra triggers.

ES8390 is driven with the shared ES8389 codec. Both analog mics are opened as stereo input (`AUDIO_INPUT_CHANNELS 2`).

## Build

Flash is 4MB (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, `partitions/v2/4m.csv`): factory app `0x2F0000` (~3MB) plus a 1MB `assets` slot. This board already uses `CONFIG_FLASH_NONE_ASSETS`, so the assets partition is left empty. Dual-bank OTA does not fit on 4MB, so app rollback is disabled. Flash and octal PSRAM run at 40MHz in DIO, with WiFi IRAM options, to avoid cache MMU faults on this 4MB module.

```sh
python3 scripts/release.py AI-dual-eyed-doll
```

## Notes

- ESP32-S3R8 octal PSRAM occupies GPIO33-37 and GPIO47-48. Do not use those pads as GPIO.
- TTP233H touch pads are treated as active-high.
- Physical hardware still needs to be checked after a successful build.

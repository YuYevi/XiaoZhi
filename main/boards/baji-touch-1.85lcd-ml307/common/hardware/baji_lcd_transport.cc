#include "config.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <hal/gpio_ll.h>
#include <limits>
#include <lvgl.h>
#include <soc/gpio_struct.h>
#include <src/draw/sw/blend/lv_draw_sw_blend_private.h>

// This board's complete frame submission path: pixel preparation, TE/DMA
// synchronization and the matching RGB565 blend operation.
namespace baji::lcd {

// Convert into a separate wire image. Never byte-swap the persistent LVGL
// canvas: direct mode retains unchanged pixels between dirty-area redraws.
inline void PrepareFrame(uint16_t* wire, const uint16_t* canvas, size_t width,
                         size_t height, bool mirror_x, bool mirror_y) {
    // On this board both axes are mirrored. Reversing all four bytes of a
    // pixel pair performs the 180-degree rotation and RGB565 swap together.
    // The alias type is intentional: pixels originate as uint16_t. A memcpy
    // per pair would remain an out-of-line call with IDF's -fno-builtin-memcpy.
    if (mirror_x && mirror_y && (width * height) % 2 == 0 &&
        ((reinterpret_cast<uintptr_t>(canvas) | reinterpret_cast<uintptr_t>(wire)) & 3) == 0) {
        typedef uint32_t PixelPair __attribute__((__may_alias__));
        const size_t count = width * height / 2;
        const auto* source = reinterpret_cast<const PixelPair*>(canvas);
        auto* target = reinterpret_cast<PixelPair*>(wire);
        for (size_t i = 0; i < count; ++i) target[i] = __builtin_bswap32(source[count - i - 1]);
        return;
    }
    for (size_t y = 0; y < height; ++y) {
        const auto* source = canvas + (mirror_y ? height - 1 - y : y) * width;
        auto* target = wire + y * width;
        for (size_t x = 0; x < width; ++x) {
            const uint16_t pixel = source[mirror_x ? width - 1 - x : x];
            target[x] = static_cast<uint16_t>((pixel << 8) | (pixel >> 8));
        }
    }
}

// Same integer arithmetic as this project's LVGL lv_color_16_16_mix(), with
// opacity scaling hoisted out of the image loop. No new color approximation.
inline uint16_t MixRgb565(uint16_t foreground, uint16_t background, uint32_t mix) {
    const uint32_t bg = (background | (uint32_t(background) << 16)) & 0x7e0f81f;
    const uint32_t fg = (foreground | (uint32_t(foreground) << 16)) & 0x7e0f81f;
    const uint32_t result = ((((fg - bg) * mix) >> 5) + bg) & 0x7e0f81f;
    return static_cast<uint16_t>((result >> 16) | result);
}

// Unsigned subtraction also works across the 32-bit microsecond timer wrap.
inline bool FreshEdge(uint32_t sequence, uint32_t previous, uint32_t edge_us,
                      uint32_t now_us, uint32_t maximum_age_us) {
    return sequence != previous && now_us - edge_us <= maximum_age_us;
}

}  // namespace baji::lcd

namespace {
constexpr char kTag[] = "BajiLcdSync";
constexpr size_t kWidth = DISPLAY_WIDTH;
constexpr size_t kHeight = DISPLAY_HEIGHT;
constexpr size_t kLines = 20;
constexpr size_t kStripBytes = kWidth * kLines * sizeof(uint16_t);
// Qualify a real edge, then start halfway through that scan. The writer follows
// the current scan and precedes the next one, leaving milliseconds on both sides.
constexpr uint32_t kFreshEdgeUs = 80;

// One LCD on this board. ISR-visible state lives in internal static RAM.
struct Transport {
    lv_display_t* display = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t* wire = nullptr;
    uint8_t* dma[2]{};
    SemaphoreHandle_t te = nullptr;
    SemaphoreHandle_t done = nullptr;
    SemaphoreHandle_t phase = nullptr;
    esp_timer_handle_t phase_timer = nullptr;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t sequence = 0, edge_us = 0, period_us = 0, high_us = 0;
    uint32_t dma_done_us = 0;
    bool mirror_x = false, mirror_y = false;
    uint32_t frames = 0, timeouts = 0, late_edges = 0, overruns = 0;
    uint32_t max_transfer_us = 0, max_phase_us = 0, max_prepare_us = 0;
    uint32_t render_started_us = 0, max_render_us = 0, max_first_strip_us = 0;
    int32_t min_scan_margin_us = std::numeric_limits<int32_t>::max();
    uint32_t scan_deadline_misses = 0;
    uint32_t report_at_us = 0, last_period_us = 0;
} transport;

uint32_t Micros() { return static_cast<uint32_t>(esp_timer_get_time()); }

void IRAM_ATTR TeEdge(void*) {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    const bool rising = gpio_ll_get_level(&GPIO, QSPI_PIN_NUM_LCD_TE);
    portENTER_CRITICAL_ISR(&transport.mux);
    if (rising) {
        const uint32_t period = now - transport.edge_us;
        if (transport.sequence && period >= 5000 && period <= 100000)
            transport.period_us = period;
        transport.edge_us = now;
        ++transport.sequence;
    } else if (transport.sequence) {
        transport.high_us = now - transport.edge_us;
    }
    portEXIT_CRITICAL_ISR(&transport.mux);
    if (rising) {
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(transport.te, &wake);
        if (wake) portYIELD_FROM_ISR();
    }
}

bool IRAM_ATTR TransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    transport.dma_done_us = static_cast<uint32_t>(esp_timer_get_time());
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(transport.done, &wake);
    // The IDF SPI LCD adapter ignores the callback's return value. Yield here
    // explicitly, otherwise each strip can wait for the next 10 ms RTOS tick.
    if (wake) portYIELD_FROM_ISR();
    return wake == pdTRUE;
}

bool WaitSafePhase(uint32_t* accepted_us, uint32_t* period_us, uint32_t* high_us) {
    uint32_t previous;
    portENTER_CRITICAL(&transport.mux);
    previous = transport.sequence;
    portEXIT_CRITICAL(&transport.mux);
    while (xSemaphoreTake(transport.te, 0) == pdTRUE) {}
    const uint32_t started = Micros();
    while (Micros() - started < 150000) {
        if (xSemaphoreTake(transport.te, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        uint32_t sequence, edge;
        portENTER_CRITICAL(&transport.mux);
        sequence = transport.sequence;
        edge = transport.edge_us;
        *period_us = transport.period_us;
        *high_us = transport.high_us;
        portEXIT_CRITICAL(&transport.mux);
        if (baji::lcd::FreshEdge(sequence, previous, edge, Micros(), kFreshEdgeUs) &&
            gpio_get_level(QSPI_PIN_NUM_LCD_TE) && *period_us && *high_us < *period_us) {
            const uint32_t age = Micros() - edge;
            if (age < *period_us / 2) {
                while (xSemaphoreTake(transport.phase, 0) == pdTRUE) {}
                ESP_ERROR_CHECK(esp_timer_start_once(transport.phase_timer, *period_us / 2 - age));
                const bool fired = xSemaphoreTake(transport.phase, pdMS_TO_TICKS(50)) == pdTRUE;
                if (!fired) esp_timer_stop(transport.phase_timer);
                const uint32_t phase = Micros() - edge;
                if (fired && phase >= *period_us / 3 && phase <= *period_us * 2 / 3) {
                    *accepted_us = edge;
                    return true;
                }
            }
        }
        previous = sequence;
        ++transport.late_edges;
    }
    ++transport.timeouts;
    return false;
}

void Flush(lv_display_t* display, const lv_area_t*, uint8_t* canvas) {
    // DIRECT mode can report several dirty areas in a refresh. Only the final
    // callback owns a fully composed image; intermediate callbacks send nothing.
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }
    const uint32_t prepared_at = Micros();
    const uint32_t render_us = prepared_at - transport.render_started_us;
    transport.max_render_us = std::max(transport.max_render_us, render_us);
    baji::lcd::PrepareFrame(transport.wire, reinterpret_cast<const uint16_t*>(canvas),
                           kWidth, kHeight, transport.mirror_x, transport.mirror_y);
    std::memcpy(transport.dma[0], transport.wire, kStripBytes);
    // Complete all preparation before waiting for the selected scan phase.
    const uint8_t columns[]{0, 0, static_cast<uint8_t>((kWidth - 1) >> 8), static_cast<uint8_t>(kWidth - 1)};
    const uint8_t rows[]{0, 0, 0, static_cast<uint8_t>(kLines - 1)};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(transport.io, 0x02002a00, columns, sizeof(columns)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(transport.io, 0x02002b00, rows, sizeof(rows)));
    transport.max_prepare_us = std::max(transport.max_prepare_us, Micros() - prepared_at);
    uint32_t edge = 0, period = 0, high = 0;
    const bool synchronized = WaitSafePhase(&edge, &period, &high);
    const uint32_t started = Micros();
    // A missing cable/pulse must not deadlock the UI. Report the degraded path;
    // retry synchronization on every subsequent frame instead of disabling TE.
    if (!synchronized && transport.timeouts <= 3)
        ESP_LOGE(kTag, "No fresh TE on GPIO%d; frame is unsynchronized", QSPI_PIN_NUM_LCD_TE);

    for (size_t y = 0, index = 0; y < kHeight; y += kLines, index ^= 1) {
        const size_t rows = std::min(kLines, kHeight - y);
        const uint32_t strip_started = Micros();
        if (y == 0) {
            ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(transport.io, 0x32002c00, transport.dma[index],
                                                     rows * kWidth * sizeof(uint16_t)));
        } else {
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(transport.panel, 0, y, kWidth, y + rows,
                                                    transport.dma[index]));
        }
        const size_t next = y + rows;
        if (next < kHeight) {
            const size_t next_rows = std::min(kLines, kHeight - next);
            std::memcpy(transport.dma[index ^ 1], transport.wire + next * kWidth,
                        next_rows * kWidth * sizeof(uint16_t));
        }
        // Do not reuse either strip until its actual DMA callback has fired.
        ESP_ERROR_CHECK(xSemaphoreTake(transport.done, pdMS_TO_TICKS(100)) == pdTRUE
                            ? ESP_OK : ESP_ERR_TIMEOUT);
        if (y == 0 && synchronized)
            transport.max_first_strip_us = std::max(transport.max_first_strip_us,
                                                    transport.dma_done_us - edge);
        if (synchronized && period) {
            if (high && high < period) {
                // At 40 MHz quad SPI one 360-pixel RGB565 row takes >=36 us.
                // Subtract remaining payload time from the ISR timestamp to
                // conservatively bound completion of the strip's first row.
                const uint32_t first_row_done = transport.dma_done_us - edge - (rows - 1) * 36;
                const uint32_t next_scan_at = period + high + y * (period - high) / kHeight;
                const uint32_t last_row_start = strip_started - edge + (rows - 1) * 36;
                const uint32_t current_scan_end = high + ((y + rows) * (period - high) + kHeight - 1) / kHeight;
                const int32_t before_next = static_cast<int32_t>(next_scan_at) - static_cast<int32_t>(first_row_done);
                const int32_t after_current = static_cast<int32_t>(last_row_start) - static_cast<int32_t>(current_scan_end);
                const int32_t margin = std::min(before_next, after_current);
                transport.min_scan_margin_us = std::min(transport.min_scan_margin_us, margin);
                if (margin <= 0) ++transport.scan_deadline_misses;
            }
        }
    }
    const uint32_t finished = transport.dma_done_us;
    const uint32_t transfer_us = finished - started;
    ++transport.frames;
    transport.max_transfer_us = std::max(transport.max_transfer_us, transfer_us);
    if (synchronized) {
        transport.max_phase_us = std::max(transport.max_phase_us, started - edge);
        // Crossing the next TE is intentional: lower rows are still being
        // scanned from the old frame. Missing their NEXT scan is the failure.
        if (period && finished - edge >= period + high + (kHeight - 1) * (period - high) / kHeight)
            ++transport.overruns;
    }
    transport.last_period_us = period;
    const uint32_t now = Micros();
    if (transport.frames <= 3 || now - transport.report_at_us >= 5000000) {
        ESP_LOGI(kTag, "frames=%lu TE=%luus high=%luus tx=%luus max=%luus phase_max=%luus prepare_max=%luus render=%luus render_max=%luus first_strip_max=%luus scan_margin_min=%ldus scan_miss=%lu timeout=%lu late=%lu overrun=%lu",
                 static_cast<unsigned long>(transport.frames), static_cast<unsigned long>(period),
                 static_cast<unsigned long>(high), static_cast<unsigned long>(transfer_us),
                 static_cast<unsigned long>(transport.max_transfer_us), static_cast<unsigned long>(transport.max_phase_us),
                 static_cast<unsigned long>(transport.max_prepare_us), static_cast<unsigned long>(render_us),
                 static_cast<unsigned long>(transport.max_render_us), static_cast<unsigned long>(transport.max_first_strip_us),
                 static_cast<long>(transport.min_scan_margin_us), static_cast<unsigned long>(transport.scan_deadline_misses),
                 static_cast<unsigned long>(transport.timeouts),
                 static_cast<unsigned long>(transport.late_edges), static_cast<unsigned long>(transport.overruns));
        transport.report_at_us = now;
    }
    lv_display_flush_ready(display);
}

void DisplayDeleted(lv_event_t*) {
    gpio_intr_disable(QSPI_PIN_NUM_LCD_TE);
    gpio_isr_handler_remove(QSPI_PIN_NUM_LCD_TE);
    // Drain the SPI queue before its callbacks/semaphores/buffers disappear.
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(transport.io, -1, nullptr, 0));
    const esp_lcd_panel_io_callbacks_t callbacks{};
    esp_lcd_panel_io_register_event_callbacks(transport.io, &callbacks, nullptr);
    esp_timer_stop(transport.phase_timer);
    ESP_ERROR_CHECK(esp_timer_delete(transport.phase_timer));
    // Stop/delete does not join an already dispatched timer callback. The lock
    // keeps it from giving a semaphore after this task releases that object.
    portENTER_CRITICAL(&transport.mux);
    const auto phase = transport.phase;
    transport.phase = nullptr;
    portEXIT_CRITICAL(&transport.mux);
    vSemaphoreDelete(phase);
    vSemaphoreDelete(transport.te);
    vSemaphoreDelete(transport.done);
    heap_caps_free(transport.wire);
    heap_caps_free(transport.dma[0]);
    heap_caps_free(transport.dma[1]);
    transport.display = nullptr;
}
}  // namespace

extern "C" lv_display_t* __real_lvgl_port_add_disp(const lvgl_port_display_cfg_t*);

extern "C" lv_display_t* __wrap_lvgl_port_add_disp(const lvgl_port_display_cfg_t* config) {
    // Board-only link wrapper; other boards keep the native SPI display path.
    ESP_ERROR_CHECK(config && config->hres == kWidth && config->vres == kHeight &&
                    !config->rotation.swap_xy && !transport.display ? ESP_OK : ESP_ERR_INVALID_ARG);
    lvgl_port_lock(0);
    auto frame_config = *config;
    frame_config.buffer_size = kWidth * kHeight;
    frame_config.double_buffer = false;
    frame_config.flags.buff_dma = false;
    frame_config.flags.buff_spiram = true;
    frame_config.flags.swap_bytes = false;
    frame_config.flags.full_refresh = false;
    frame_config.flags.direct_mode = true;
    // Address RAM in the controller's unmirrored scan order. Apply the board's
    // original orientation losslessly to the staging image before waiting TE.
    frame_config.rotation.mirror_x = false;
    frame_config.rotation.mirror_y = false;
    auto* display = __real_lvgl_port_add_disp(&frame_config);
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_ERR_NO_MEM);
    transport.display = display;
    transport.io = config->io_handle;
    transport.panel = config->panel_handle;
    transport.mirror_x = config->rotation.mirror_x;
    transport.mirror_y = config->rotation.mirror_y;
    transport.wire = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, kWidth * kHeight * 2,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto& buffer : transport.dma)
        buffer = static_cast<uint8_t*>(heap_caps_malloc(kStripBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    transport.te = xSemaphoreCreateBinary();
    transport.done = xSemaphoreCreateBinary();
    transport.phase = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(transport.wire && transport.dma[0] && transport.dma[1] && transport.te && transport.done && transport.phase
                        ? ESP_OK : ESP_ERR_NO_MEM);
    esp_timer_create_args_t phase_timer{};
    phase_timer.callback = [](void*) {
        portENTER_CRITICAL(&transport.mux);
        if (transport.phase) xSemaphoreGive(transport.phase);
        portEXIT_CRITICAL(&transport.mux);
    };
    phase_timer.name = "lcd_phase";
    ESP_ERROR_CHECK(esp_timer_create(&phase_timer, &transport.phase_timer));
    const esp_lcd_panel_io_callbacks_t callbacks{.on_color_trans_done = TransferDone};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(transport.io, &callbacks, nullptr));
    const esp_err_t isr_result = gpio_install_isr_service(0);
    ESP_ERROR_CHECK(isr_result == ESP_ERR_INVALID_STATE ? ESP_OK : isr_result);
    gpio_config_t te_config{};
    te_config.pin_bit_mask = 1ULL << QSPI_PIN_NUM_LCD_TE;
    te_config.mode = GPIO_MODE_INPUT;
    te_config.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&te_config));
    ESP_ERROR_CHECK(gpio_isr_handler_add(QSPI_PIN_NUM_LCD_TE, TeEdge, nullptr));
    // Set TE output for every supported panel initialization sequence.
    const uint8_t mode = 0;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(transport.io, 0x02003500, &mode, 1));
    lv_display_set_flush_cb(display, Flush);
    lv_display_add_event_cb(display, [](lv_event_t*) { transport.render_started_us = Micros(); },
                            LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display, DisplayDeleted, LV_EVENT_DELETE, nullptr);
    ESP_LOGI(kTag, "GPIO%d fresh TE + half-period phase; composed 360x360 frame, 2x%u-line internal DMA, native scan order",
             QSPI_PIN_NUM_LCD_TE, static_cast<unsigned>(kLines));
    lvgl_port_unlock();
    return display;
}

extern "C" void __real_lv_draw_sw_blend_image_to_rgb565(lv_draw_sw_blend_image_dsc_t*);

extern "C" void __wrap_lv_draw_sw_blend_image_to_rgb565(lv_draw_sw_blend_image_dsc_t* dsc) {
    if (dsc->src_color_format != LV_COLOR_FORMAT_RGB565 || dsc->mask_buf ||
        dsc->blend_mode != LV_BLEND_MODE_NORMAL || dsc->opa >= LV_OPA_MAX) {
        __real_lv_draw_sw_blend_image_to_rgb565(dsc);
        return;
    }
    const uint32_t mix = (uint32_t(dsc->opa) + 4) >> 3;
    const auto* source = static_cast<const uint8_t*>(dsc->src_buf);
    auto* target = static_cast<uint8_t*>(dsc->dest_buf);
    for (int32_t y = 0; y < dsc->dest_h; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(source);
        auto* dst = reinterpret_cast<uint16_t*>(target);
        for (int32_t x = 0; x < dsc->dest_w; ++x)
            dst[x] = baji::lcd::MixRgb565(src[x], dst[x], mix);
        source += dsc->src_stride;
        target += dsc->dest_stride;
    }
}

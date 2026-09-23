#include "watch_resources.h"

#include "assets.h"
#include "application.h"
#include <src/misc/cache/instance/lv_image_cache.h>
#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
constexpr char kTag[] = "WatchResources";
constexpr size_t kFrameBytes = 360 * 360 * 2;

#pragma pack(push, 1)
struct PackHeader {
    char magic[8];
    uint32_t version, bytes, font_count, icon_count, frame_count, fps;
    uint32_t fonts, icons, frames, video;
};
struct PackedVideoClips { uint32_t standby_frames, speaking_frames; };
struct PackedFont {
    uint16_t px;
    uint8_t bold, reserved;
    int16_t line_height, base_line;
    uint32_t count, glyphs, bitmap, bitmap_bytes;
};
struct PackedGlyph {
    uint32_t unicode, bitmap;
    uint16_t advance;
    uint8_t width, height;
    int8_t x, y;
};
struct PackedIcon {
    char name[32];
    uint16_t width, height;
    uint32_t pixels, bytes;
};
struct PackedFrame { uint32_t offset, bytes; };
#pragma pack(pop)

static_assert(sizeof(PackHeader) == 48 && sizeof(PackedVideoClips) == 8 && sizeof(PackedFont) == 24 &&
              sizeof(PackedGlyph) == 14 && sizeof(PackedIcon) == 44);

template <typename T> T Read(const uint8_t* source, size_t offset) {
    T value;
    std::memcpy(&value, source + offset, sizeof(value));
    return value;
}

bool Range(size_t offset, size_t bytes, size_t size) {
    return offset <= size && bytes <= size - offset;
}

struct PsramDeleter {
    void operator()(void* pointer) const { heap_caps_free(pointer); }
};

struct FontResource {
    lv_font_t font{};
    lv_font_fmt_txt_dsc_t descriptor{};
    lv_font_fmt_txt_cmap_t cmap{};
    std::unique_ptr<lv_font_fmt_txt_glyph_dsc_t, PsramDeleter> glyphs;
    std::unique_ptr<uint16_t, PsramDeleter> unicode;
};

struct IconResource {
    lv_image_dsc_t image{};
    std::vector<uint8_t> pixels;
};

struct VideoStatistics {
    bool playing = false;
    int64_t active_since_us = 0;
    uint64_t active_us = 0, reported_active_us = 0;
    uint64_t decode_us = 0, update_us = 0;
    uint64_t reported_decode_us = 0, reported_update_us = 0;
    uint32_t frames = 0, reported_frames = 0;
    uint32_t max_decode_us = 0, interval_max_decode_us = 0;
};

struct VideoClip { uint32_t first = 0, count = 0; };

struct ResourceState {
    PackHeader header{};
    VideoClip standby, speaking;
    bool video_speaking = false;
    uint8_t* ui = nullptr;
    int common_font_half_px = 22;
    std::map<int, std::unique_ptr<FontResource>> fonts;
    std::map<std::string, std::unique_ptr<IconResource>> icons;
    lv_obj_t* video_image = nullptr;
    lv_timer_t* timer = nullptr;
    jpeg_dec_handle_t decoder = nullptr;
    uint8_t* compressed_frame = nullptr;
    size_t compressed_capacity = 0;
    bool requires_attach = false;
    uint8_t* output[2] = {};
    int output_index = 0;
    int last_frame = -1;
    uint32_t start_tick = 0;
    lv_image_dsc_t frame{};
    VideoStatistics stats{};
};

ResourceState& State() {
    // Intentionally keep resource pointers valid for all LVGL font styles.
    static auto* state = new ResourceState;
    return *state;
}

VideoClip CurrentClip() {
    const auto& state = State();
    return state.video_speaking ? state.speaking : state.standby;
}

void ReportVideoStatistics(bool final) {
    auto& stats = State().stats;
    if (!stats.frames) return;
    const uint64_t active_us = stats.active_us +
        (stats.playing ? esp_timer_get_time() - stats.active_since_us : 0);
    const uint64_t elapsed = final ? active_us : active_us - stats.reported_active_us;
    if (!final && elapsed < 5000000) return;
    const uint32_t frames = final ? stats.frames : stats.frames - stats.reported_frames;
    const uint64_t decode_us = final ? stats.decode_us : stats.decode_us - stats.reported_decode_us;
    const uint64_t update_us = final ? stats.update_us : stats.update_us - stats.reported_update_us;
    // Counts completed JPEG decodes submitted to LVGL, not LCD/DMA flushes.
    ESP_LOGI(kTag,
             "Video %s: frames=%lu, submitted=%.1f fps, JPEG avg/max=%.1f/%.1f ms, "
             "LVGL update avg=%.1f ms, active=%.1f s, PSRAM free/largest=%u/%u, internal free=%u",
             final ? "stopped" : "stats", static_cast<unsigned long>(frames),
             elapsed ? frames * 1000000.0 / elapsed : 0.0,
             frames ? decode_us / (frames * 1000.0) : 0.0,
             (final ? stats.max_decode_us : stats.interval_max_decode_us) / 1000.0,
             frames ? update_us / (frames * 1000.0) : 0.0, active_us / 1000000.0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    stats.reported_active_us = active_us;
    stats.reported_frames = stats.frames;
    stats.reported_decode_us = stats.decode_us;
    stats.reported_update_us = stats.update_us;
    stats.interval_max_decode_us = 0;
}

bool LoadPack() {
    auto& state = State();
    if (state.ui) return true;
    // Called with DisplayLock held: native upgrading takes the same lock for
    // its system message before it unmaps the assets partition.
    if (Application::GetInstance().GetDeviceState() == kDeviceStateUpgrading) return false;
    void* source = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("watch_ui.pack", source, size) ||
        size < sizeof(PackHeader)) return false;
    const auto header = Read<PackHeader>(static_cast<uint8_t*>(source), 0);
    const size_t metadata_start = sizeof(PackHeader) + (header.version == 4 ? sizeof(PackedVideoClips) : 0);
    if (std::memcmp(header.magic, "BAJIUI1", 8) ||
        (header.version < 1 || header.version > 4) ||
        header.bytes != size || header.video < metadata_start || header.video > size ||
        header.font_count > 100 || header.icon_count > 256 || !header.frame_count || header.frame_count > 1000 ||
        header.fps == 0 || header.fps > 30 ||
        header.fonts < metadata_start || header.icons < metadata_start || header.frames < metadata_start ||
        !Range(header.fonts, header.font_count * sizeof(PackedFont), header.video) ||
        !Range(header.icons, header.icon_count * sizeof(PackedIcon), header.video) ||
        !Range(header.frames, header.frame_count * sizeof(PackedFrame), header.video)) {
        ESP_LOGE(kTag, "Invalid watch resource pack");
        return false;
    }
    VideoClip standby{0, header.frame_count}, speaking{0, header.frame_count};
    if (header.version == 4) {
        const auto clips = Read<PackedVideoClips>(static_cast<uint8_t*>(source), sizeof(PackHeader));
        if (!clips.standby_frames || clips.standby_frames >= header.frame_count ||
            clips.speaking_frames != header.frame_count - clips.standby_frames) {
            ESP_LOGE(kTag, "Invalid standby/speaking frame ranges");
            return false;
        }
        standby.count = clips.standby_frames;
        speaking = {clips.standby_frames, clips.speaking_frames};
    }
    auto* copy = static_cast<uint8_t*>(heap_caps_malloc(header.video, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        ESP_LOGW(kTag, "Insufficient PSRAM for watch fonts and icons (%lu bytes)",
                 static_cast<unsigned long>(header.video));
        return false;
    }
    std::memcpy(copy, source, header.video);
    state.header = header;
    state.standby = standby;
    state.speaking = speaking;
    state.ui = copy;
    // Select the regular face with the broadest character coverage. The
    // readable watch pack uses 14px; earlier packs used 11px. Derive this
    // from the pack so an app/assets update never loses its CJK fallback.
    uint32_t common_count = 0;
    for (uint32_t i = 0; i < header.font_count; ++i) {
        const auto item = Read<PackedFont>(copy, header.fonts + i * sizeof(PackedFont));
        const int item_weight = item.reserved == 1 ? item.bold * 100 : item.bold ? 700 : 400;
        const int item_half_px = item.reserved == 1 ? item.px : item.px * 2;
        if (item.reserved <= 1 && item_weight == 400 && item_half_px > 0 &&
            item_half_px <= 254 && item.count > common_count && item.count <= 16000) {
            state.common_font_half_px = item_half_px;
            common_count = item.count;
        }
    }
    ESP_LOGI(kTag, "Watch pack: %lu fonts, %lu icons, %lu frames at %lu fps",
             static_cast<unsigned long>(header.font_count), static_cast<unsigned long>(header.icon_count),
             static_cast<unsigned long>(header.frame_count), static_cast<unsigned long>(header.fps));
    return true;
}

void VideoDeleted(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == State().video_image) {
        // The callback is already being destroyed; do not remove it recursively.
        State().video_image = nullptr;
        WatchResources::StopVideo();
    }
}

bool DecodeFrame(uint32_t index) {
    auto& state = State();
    if (state.requires_attach || !state.video_image || !state.decoder || index >= state.header.frame_count)
        return false;
    // Native assets upgrading acquires DisplayLock before unmapping. Every
    // call runs under that lock, and no mapped pointer survives this call.
    if (Application::GetInstance().GetDeviceState() == kDeviceStateUpgrading) {
        state.requires_attach = true;
        return false;
    }
    void* source = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("watch_ui.pack", source, size) ||
        size != state.header.bytes || size < sizeof(PackHeader) ||
        std::memcmp(source, &state.header, sizeof(PackHeader)) ||
        (state.header.version == 4 && std::memcmp(static_cast<uint8_t*>(source) + sizeof(PackHeader),
            state.ui + sizeof(PackHeader), sizeof(PackedVideoClips)))) {
        state.requires_attach = true;
        return false;
    }
    const auto encoded = Read<PackedFrame>(state.ui, state.header.frames + index * sizeof(PackedFrame));
    const auto current = Read<PackedFrame>(static_cast<uint8_t*>(source),
                                         state.header.frames + index * sizeof(PackedFrame));
    if (std::memcmp(&current, &encoded, sizeof(encoded))) {
        state.requires_attach = true;
        return false;
    }
    if (encoded.offset < state.header.video ||
        !Range(encoded.offset, encoded.bytes, size) || encoded.bytes > state.compressed_capacity) return false;
    std::memcpy(state.compressed_frame, static_cast<uint8_t*>(source) + encoded.offset, encoded.bytes);
    const int64_t started_us = esp_timer_get_time();
    jpeg_dec_io_t io{};
    io.inbuf = state.compressed_frame;
    io.inbuf_len = encoded.bytes;
    io.outbuf = state.output[state.output_index];
    jpeg_dec_header_info_t info{};
    if (jpeg_dec_parse_header(state.decoder, &io, &info) != JPEG_ERR_OK ||
        info.width != 360 || info.height != 360 || jpeg_dec_process(state.decoder, &io) != JPEG_ERR_OK) return false;
    const uint32_t decode_us = esp_timer_get_time() - started_us;
    const int64_t update_started_us = esp_timer_get_time();
    lv_image_cache_drop(&state.frame);
    state.frame.data = io.outbuf;
    lv_image_set_src(state.video_image, &state.frame);
    lv_obj_invalidate(state.video_image);
    state.output_index ^= 1;
    state.last_frame = index;
    ++state.stats.frames;
    state.stats.decode_us += decode_us;
    state.stats.update_us += esp_timer_get_time() - update_started_us;
    state.stats.max_decode_us = std::max(state.stats.max_decode_us, decode_us);
    state.stats.interval_max_decode_us = std::max(state.stats.interval_max_decode_us, decode_us);
    return true;
}

void VideoTick(lv_timer_t*) {
    auto& state = State();
    const auto clip = CurrentClip();
    if (!state.video_image || !state.decoder || !clip.count) return;
    const uint32_t index = clip.first + (static_cast<uint64_t>(lv_tick_get() - state.start_tick) *
                                        state.header.fps / 1000) % clip.count;
    if (state.last_frame == static_cast<int>(index)) return;
    if (!DecodeFrame(index)) {
        ESP_LOGW(kTag, "Video %s at frame %lu; last frame retained until reattach",
                 state.requires_attach ? "assets unavailable or upgrading" : "decode failed",
                 static_cast<unsigned long>(index));
        state.requires_attach = true;
        WatchResources::SetVideoPlaying(false);
    }
    ReportVideoStatistics(false);
}
}  // namespace

const lv_font_t* WatchResources::Font(float px, int weight) {
    if (!std::isfinite(px) || px < 1 || px > 127) return nullptr;
    if (weight == 0 || weight == 1) weight = weight ? 700 : 400;
    if (weight != 400 && weight != 600 && weight != 700 && weight != 800) return nullptr;
    if (!LoadPack()) return nullptr;
    auto& state = State();
    const int half_px = std::lround(px * 2);
    const int key = half_px * 10 + weight / 100;
    if (auto found = state.fonts.find(key); found != state.fonts.end()) return &found->second->font;
    PackedFont packed{};
    bool found = false;
    int legacy_distance = 1000;
    for (uint32_t i = 0; i < state.header.font_count; ++i) {
        auto item = Read<PackedFont>(state.ui, state.header.fonts + i * sizeof(PackedFont));
        const int item_half_px = item.reserved == 1 ? item.px : item.px * 2;
        const int item_weight = item.reserved == 1 ? item.bold * 100 : item.bold ? 700 : 400;
        if (item.reserved <= 1 && item_half_px == half_px && item_weight == weight) {
            packed = item; found = true; break;
        }
        // A firmware update can briefly encounter an older assets pack. Use
        // its closest half-pixel/bold approximation instead of falling all
        // the way back to the board's much larger general-purpose font.
        if (item.reserved == 0 && std::abs(item_half_px - half_px) <= 1 &&
            item_weight == (weight >= 600 ? 700 : 400)) {
            const int distance = std::abs(item_half_px - half_px);
            if (distance < legacy_distance) {
                packed = item; found = true; legacy_distance = distance;
            }
        }
    }
    if (!found || packed.count == 0 || packed.count > 16000 ||
        !Range(packed.glyphs, packed.count * sizeof(PackedGlyph), state.header.video) ||
        !Range(packed.bitmap, packed.bitmap_bytes, state.header.video)) return nullptr;
    auto font = std::unique_ptr<FontResource>(new (std::nothrow) FontResource);
    if (!font) return nullptr;
    font->glyphs.reset(static_cast<lv_font_fmt_txt_glyph_dsc_t*>(heap_caps_calloc(
        packed.count + 1, sizeof(lv_font_fmt_txt_glyph_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    font->unicode.reset(static_cast<uint16_t*>(heap_caps_malloc(
        packed.count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!font->glyphs || !font->unicode) {
        ESP_LOGW(kTag, "Insufficient PSRAM for %.1f px font descriptors", static_cast<double>(px));
        return nullptr;
    }
    for (uint32_t i = 0; i < packed.count; ++i) {
        const auto glyph = Read<PackedGlyph>(state.ui, packed.glyphs + i * sizeof(PackedGlyph));
        if (glyph.unicode < 32 || glyph.unicode > 65535 || glyph.bitmap >= 0x100000 ||
            !Range(glyph.bitmap, (glyph.width * glyph.height + 1) / 2, packed.bitmap_bytes)) return nullptr;
        auto& entry = font->glyphs.get()[i + 1];
        entry.bitmap_index = glyph.bitmap;
        entry.adv_w = glyph.advance;
        entry.box_w = glyph.width;
        entry.box_h = glyph.height;
        entry.ofs_x = glyph.x;
        entry.ofs_y = glyph.y;
        font->unicode.get()[i] = glyph.unicode - 32;
    }
    font->cmap.range_start = 32;
    font->cmap.range_length = static_cast<uint32_t>(font->unicode.get()[packed.count - 1]) + 1;
    font->cmap.glyph_id_start = 1;
    font->cmap.unicode_list = font->unicode.get();
    font->cmap.list_length = packed.count;
    font->cmap.type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY;
    font->descriptor.glyph_bitmap = state.ui + packed.bitmap;
    font->descriptor.glyph_dsc = font->glyphs.get();
    font->descriptor.cmaps = &font->cmap;
    font->descriptor.cmap_num = 1;
    font->descriptor.bpp = 4;
    font->font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    font->font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    font->font.line_height = packed.line_height;
    font->font.base_line = packed.base_line;
    font->font.underline_position = -1;
    font->font.underline_thickness = 1;
    font->font.dsc = &font->descriptor;
    // Keep dynamic Chinese text at the pack's readable common size, while
    // retaining the correct fallback when loading an older assets pack.
    if (!(half_px == state.common_font_half_px && weight == 400))
        font->font.fallback = Font(state.common_font_half_px * .5f, 400);
    auto* result = &font->font;
    state.fonts.emplace(key, std::move(font));
    return result;
}

const lv_image_dsc_t* WatchResources::Icon(const char* name, int px, uint32_t color) {
    (void)color;
    if (!name || px < 1 || px > 96 || !LoadPack()) return nullptr;
    auto& state = State();
    const std::string key = std::string(name) + '/' + std::to_string(px);
    if (auto found = state.icons.find(key); found != state.icons.end()) return &found->second->image;
    PackedIcon packed{};
    bool found = false;
    const std::string variant = std::string(name) + '@' + std::to_string(px);
    for (uint32_t i = 0; i < state.header.icon_count; ++i) {
        const auto icon = Read<PackedIcon>(state.ui, state.header.icons + i * sizeof(PackedIcon));
        if (std::strncmp(variant.c_str(), icon.name, sizeof(icon.name)) == 0) {
            packed = icon; found = true; break;
        }
        if (std::strncmp(name, icon.name, sizeof(icon.name)) == 0) { packed = icon; found = true; }
    }
    if (!found || packed.width == 0 || packed.width > 96 || packed.height != packed.width ||
        packed.bytes != packed.width * packed.height ||
        !Range(packed.pixels, packed.bytes, state.header.video)) return nullptr;
    auto icon = std::make_unique<IconResource>();
    const uint32_t stride = (px + 3) & ~3;
    icon->pixels.resize(stride * px);
    const auto* input = state.ui + packed.pixels;
    // Prototype sizes are exact rasterizations; other sizes use area resampling.
    for (int y = 0; y < px; ++y) {
        for (int x = 0; x < px; ++x) {
            unsigned sum = 0, count = 0;
            const int left = x * packed.width / px, right = std::max(left + 1, (x + 1) * packed.width / px);
            const int top = y * packed.height / px, bottom = std::max(top + 1, (y + 1) * packed.height / px);
            for (int yy = top; yy < bottom && yy < packed.height; ++yy)
                for (int xx = left; xx < right && xx < packed.width; ++xx) {
                    sum += input[yy * packed.width + xx]; ++count;
                }
            icon->pixels[y * stride + x] = sum / std::max(count, 1u);
        }
    }
    icon->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    icon->image.header.cf = LV_COLOR_FORMAT_A8;
    icon->image.header.w = px;
    icon->image.header.h = px;
    icon->image.header.stride = stride;
    icon->image.data_size = icon->pixels.size();
    icon->image.data = icon->pixels.data();
    auto* result = &icon->image;
    state.icons.emplace(key, std::move(icon));
    return result;
}

bool WatchResources::AttachVideo(lv_obj_t* image, bool speaking) {
    if (!image || !LoadPack()) return false;
    if (Application::GetInstance().GetDeviceState() == kDeviceStateUpgrading) return false;
    StopVideo();
    auto& state = State();
    void* source = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("watch_ui.pack", source, size) || size != state.header.bytes) return false;
    const auto current = Read<PackHeader>(static_cast<uint8_t*>(source), 0);
    if (std::memcmp(&current, &state.header, sizeof(current)) ||
        std::memcmp(source, state.ui, state.header.video)) {
        ESP_LOGW(kTag, "Watch resources changed; restart to load the new video and fonts together");
        return false;
    }
    size_t largest_frame = 0;
    for (uint32_t i = 0; i < state.header.frame_count; ++i) {
        const auto frame = Read<PackedFrame>(state.ui, state.header.frames + i * sizeof(PackedFrame));
        if (frame.offset < state.header.video || !Range(frame.offset, frame.bytes, size) ||
            !frame.bytes || frame.bytes > 0x100000) return false;
        largest_frame = std::max(largest_frame, static_cast<size_t>(frame.bytes));
    }
    if (!largest_frame) return false;
    state.compressed_capacity = largest_frame;
    state.compressed_frame = static_cast<uint8_t*>(heap_caps_malloc(
        largest_frame, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto& output : state.output)
        output = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!state.compressed_frame || !state.output[0] || !state.output[1]) {
        ESP_LOGW(kTag, "Video unavailable: needs %u bytes of free PSRAM",
                 static_cast<unsigned>(largest_frame + 2 * kFrameBytes));
        StopVideo();
        return false;
    }
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (jpeg_dec_open(&config, &state.decoder) != JPEG_ERR_OK) { StopVideo(); return false; }
    state.video_image = image;
    state.frame = {};
    state.frame.header.magic = LV_IMAGE_HEADER_MAGIC;
    state.frame.header.cf = LV_COLOR_FORMAT_RGB565;
    state.frame.header.w = state.frame.header.h = 360;
    state.frame.header.stride = 720;
    state.frame.data_size = kFrameBytes;
    state.output_index = 0;
    state.requires_attach = false;
    state.video_speaking = speaking;
    state.last_frame = -1;
    state.start_tick = lv_tick_get();
    state.stats = {};
    state.stats.playing = true;
    state.stats.active_since_us = esp_timer_get_time();
    if (!DecodeFrame(CurrentClip().first)) { StopVideo(); return false; }
    lv_obj_add_event_cb(image, VideoDeleted, LV_EVENT_DELETE, nullptr);
    state.timer = lv_timer_create(VideoTick, 1000 / state.header.fps, nullptr);
    ESP_LOGI(kTag, "AI video ready: 360x360, standby/speaking=%lu/%lu frames, target %lu fps, JPEG scratch=%u bytes, baked shading=%d",
             static_cast<unsigned long>(state.standby.count), static_cast<unsigned long>(state.speaking.count),
             static_cast<unsigned long>(state.header.fps),
             static_cast<unsigned>(largest_frame), state.header.version >= 2);
    return true;
}

bool WatchResources::VideoIncludesShading() {
    return LoadPack() && State().header.version >= 2;
}

void WatchResources::SetVideoSpeaking(bool speaking) {
    auto& state = State();
    if (state.video_speaking == speaking) return;
    state.video_speaking = speaking;
    state.last_frame = -1;
    state.start_tick = lv_tick_get();
    // Covered/asleep pages only remember the new clip. Do not allocate or
    // decode until visible, and never bypass the assets-upgrade guard.
    if (!state.timer || !state.stats.playing || state.requires_attach) return;
    if (!DecodeFrame(CurrentClip().first)) {
        state.requires_attach = true;
        SetVideoPlaying(false);
    }
}

void WatchResources::SetVideoPlaying(bool enabled) {
    auto& state = State();
    if (!state.timer) return;
    if (enabled && state.requires_attach) return;
    if (state.stats.playing == enabled) return;
    if (enabled) {
        const auto clip = CurrentClip();
        // last_frame is absolute in the pack, while time is local to a clip.
        // A state change during pause invalidates it and starts the new clip.
        if (state.last_frame < static_cast<int>(clip.first) ||
            state.last_frame >= static_cast<int>(clip.first + clip.count)) {
            if (!DecodeFrame(clip.first)) { state.requires_attach = true; return; }
        }
        state.stats.active_since_us = esp_timer_get_time();
        state.stats.playing = true;
        const uint32_t local_frame = state.last_frame - clip.first;
        state.start_tick = lv_tick_get() - (local_frame * 1000 + state.header.fps - 1) / state.header.fps;
        lv_timer_resume(state.timer);
    } else {
        state.stats.active_us += esp_timer_get_time() - state.stats.active_since_us;
        state.stats.playing = false;
        lv_timer_pause(state.timer);
    }
}

void WatchResources::StopVideo() {
    auto& state = State();
    if (state.stats.playing) {
        state.stats.active_us += esp_timer_get_time() - state.stats.active_since_us;
        state.stats.playing = false;
    }
    if (state.video_image) {
        lv_obj_remove_event_cb(state.video_image, VideoDeleted);
        if (lv_image_get_src(state.video_image) == &state.frame) lv_image_set_src(state.video_image, nullptr);
        state.video_image = nullptr;
    }
    if (state.timer) { lv_timer_delete(state.timer); state.timer = nullptr; }
    lv_image_cache_drop(&state.frame);
    if (state.decoder) { jpeg_dec_close(state.decoder); state.decoder = nullptr; }
    heap_caps_free(state.compressed_frame);
    state.compressed_frame = nullptr;
    state.compressed_capacity = 0;
    state.requires_attach = false;
    for (auto& output : state.output) { heap_caps_free(output); output = nullptr; }
    state.frame.data = nullptr;
    state.last_frame = -1;
    ReportVideoStatistics(true);
    state.stats = {};
}

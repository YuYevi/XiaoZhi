#include "ui/watch_ui.h"
#include "ui/watch_ui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <string_view>
#include "assets/lang_config.h"
#include <src/misc/cache/instance/lv_image_cache.h>
#include "ui/watch_resources.h"

using namespace baji::ui;

namespace {
struct Binding {
    std::function<void(lv_event_t*)> callback;
};
struct GradientTexture {
    lv_image_dsc_t image{};
    std::vector<uint16_t> pixels;
};
struct AlphaTexture {
    lv_image_dsc_t image{};
    std::vector<uint8_t> pixels;
    AlphaTexture(int w, int h) : pixels(w * h) {
        image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_A8;
        image.header.w = w;
        image.header.h = h;
        image.header.stride = w;
        image.data_size = pixels.size();
        image.data = pixels.data();
    }
};
bool HasCjk(const char* text) {
    auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        uint32_t code = *p++;
        int trailing = 0;
        if ((code & 0xe0) == 0xc0) {
            code &= 0x1f;
            trailing = 1;
        } else if ((code & 0xf0) == 0xe0) {
            code &= 0x0f;
            trailing = 2;
        } else if ((code & 0xf8) == 0xf0) {
            code &= 0x07;
            trailing = 3;
        }
        while (trailing-- && (*p & 0xc0) == 0x80)
            code = (code << 6) | (*p++ & 0x3f);
        if ((code >= 0x2e80 && code <= 0x9fff) || (code >= 0xf900 && code <= 0xfaff) ||
            (code >= 0x20000 && code <= 0x323af))
            return true;
    }
    return false;
}

void Dispatch(lv_event_t* e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e))
        return;
    auto* b = static_cast<Binding*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        delete b;
        return;
    }
    auto fn = b->callback;
    fn(e);
}

}  // namespace

namespace baji::ui {
std::string LocalizedText(const std::string& text, bool english) {
    using namespace Lang::Strings;
    struct Translation {
        const char* native;
        const char* chinese;
        const char* english;
    };
    static constexpr Translation messages[] = {
        {INITIALIZING, "正在初始化...", "Initializing..."},
        {LOADING_PROTOCOL, "登录服务器...", "Logging in..."},
        {DETECTING_MODULE, "检测模组...", "Detecting modem..."},
        {REGISTERING_NETWORK, "等待网络...", "Waiting for network..."},
        {CHECKING_NEW_VERSION, "检查新版本...", "Checking for updates..."},
        {SCANNING_WIFI, "扫描 Wi-Fi...", "Scanning Wi-Fi..."},
        {ENTERING_WIFI_CONFIG_MODE, "进入配网模式...", "Opening Wi-Fi setup..."},
        {WIFI_CONFIG_MODE, "配网模式", "Wi-Fi setup"},
        {ACTIVATION, "激活设备", "Activate device"},
        {CONNECTING, "连接中...", "Connecting..."},
        {STANDBY, "待命", "Standby"},
        {LISTENING, "聆听中...", "Listening..."},
        {SPEAKING, "说话中...", "Speaking..."},
        {ERROR, "错误", "Error"},
        {WARNING, "警告", "Warning"},
        {INFO, "信息", "Information"},
        {PIN_ERROR, "请插入 SIM 卡", "Please insert a SIM card"},
        {REG_ERROR, "无法接入网络，请检查流量卡状态", "Cannot connect. Check your SIM card"},
        {MODEM_INIT_ERROR, "模组初始化失败", "Modem initialization failed"},
        {SERVER_NOT_FOUND, "正在寻找可用服务", "Looking for available service"},
        {SERVER_NOT_CONNECTED, "无法连接服务，请稍后再试", "Cannot connect to service. Try again later"},
        {SERVER_TIMEOUT, "等待响应超时", "Response timed out"},
        {SERVER_ERROR, "发送失败，请检查网络", "Sending failed. Check your network"},
        {OTA_UPGRADE, "OTA 升级", "Firmware update"},
        {UPGRADING, "正在升级系统...", "Updating system..."},
        {UPGRADE_FAILED, "升级失败", "Update failed"},
        {LOADING_ASSETS, "加载资源...", "Loading resources..."},
        {DOWNLOAD_ASSETS_FAILED, "下载资源失败", "Resource download failed"},
        {PLEASE_WAIT, "请稍候...", "Please wait..."},
        {SWITCH_TO_WIFI_NETWORK, "切换到 Wi-Fi...", "Switching to Wi-Fi..."},
        {SWITCH_TO_4G_NETWORK, "切换到 4G...", "Switching to mobile data..."},
        {CONNECTION_SUCCESSFUL, "连接成功", "Connected"},
        {BATTERY_CHARGING, "正在充电", "Charging"},
        {BATTERY_FULL, "电量已满", "Battery full"},
        {BATTERY_LOW, "电量不足", "Low battery"},
        {BATTERY_NEED_CHARGE, "电量低，请充电", "Low battery. Please charge"},
        {MAX_VOLUME, "最大音量", "Maximum volume"},
        {MUTED, "已静音", "Muted"},
        {RTC_MODE_OFF, "AEC 关闭", "AEC off"},
        {RTC_MODE_ON, "AEC 开启", "AEC on"},
        {FLIGHT_MODE_OFF, "飞行模式已关闭", "Airplane mode off"},
        {FLIGHT_MODE_ON, "飞行模式已开启", "Airplane mode on"},
        {HELLO_MY_FRIEND, "你好，我的朋友！", "Hello, my friend!"},
        {"Upgrade successful, rebooting...", "升级成功，正在重启...", "Update complete. Restarting..."},
    };
    for (const auto& message : messages) {
        if (text == message.native || text == message.chinese)
            return english ? message.english : message.chinese;
    }

    // Translate only known framing. SSIDs, URLs, codes and server payloads
    // must survive the runtime language change without modification.
    const std::string_view source(text);
    const auto starts_with = [source](std::string_view prefix) {
        return !prefix.empty() && source.substr(0, prefix.size()) == prefix;
    };
    const std::string_view hotspot = CONNECT_TO_HOTSPOT;
    if (starts_with(hotspot)) {
        const std::string_view browser = ACCESS_VIA_BROWSER;
        const auto split = source.find(browser, hotspot.size());
        if (split != std::string_view::npos) {
            return std::string(english ? "Connect your phone to " : "手机连接热点 ") +
                   std::string(source.substr(hotspot.size(), split - hotspot.size())) +
                   (english ? "\nOpen " : "\n浏览器访问 ") +
                   std::string(source.substr(split + browser.size()));
        }
    }
    // This notification has both a fixed prefix and suffix around an SSID.
    // Handle it before CONNECTED_TO so its suffix cannot be mistaken for a name.
    constexpr std::string_view saved_prefix = "已连接 ";
    constexpr std::string_view saved_suffix = "，目标网络已保存";
    if (starts_with(saved_prefix) && source.size() >= saved_prefix.size() + saved_suffix.size() &&
        source.substr(source.size() - saved_suffix.size()) == saved_suffix) {
        const auto ssid = source.substr(saved_prefix.size(),
            source.size() - saved_prefix.size() - saved_suffix.size());
        return english ? "Connected to " + std::string(ssid) + "\nTarget network saved" : text;
    }
    static constexpr Translation prefixes[] = {
        {CONNECT_TO, "连接 ", "Connecting to "},
        {CONNECTED_TO, "已连接 ", "Connected to "},
        {VERSION, "版本 ", "Version "},
        {NEW_VERSION, "新版本 ", "New version "},
    };
    for (const auto& prefix : prefixes) {
        const std::string_view native = prefix.native;
        if (starts_with(native))
            return std::string(english ? prefix.english : prefix.chinese) +
                   std::string(source.substr(native.size()));
    }
    const std::string_view volume_prefix = VOLUME;
    if (starts_with(volume_prefix)) {
        const auto value = source.substr(volume_prefix.size());
        if (!value.empty() && value.find_first_not_of("0123456789") == std::string_view::npos)
            return std::string(english ? "Volume " : "音量 ") + std::string(value);
    }
    const std::string_view retry_format = CHECK_NEW_VERSION_FAILED;
    const auto delay_offset = retry_format.find("%d");
    const auto detail_offset = retry_format.find("%s");
    if (delay_offset != std::string_view::npos && detail_offset != std::string_view::npos &&
        detail_offset > delay_offset + 2 && starts_with(retry_format.substr(0, delay_offset))) {
        const auto separator = retry_format.substr(delay_offset + 2, detail_offset - delay_offset - 2);
        const auto split = source.find(separator, delay_offset);
        if (split != std::string_view::npos) {
            const auto seconds = source.substr(delay_offset, split - delay_offset);
            if (!seconds.empty() && seconds.find_first_not_of("0123456789") == std::string_view::npos) {
                return std::string(english ? "Update check failed; retry in " : "检查新版本失败，将在 ") +
                       std::string(seconds) + (english ? " seconds:\n" : " 秒后重试：\n") +
                       std::string(source.substr(split + separator.size()));
            }
        }
    }
    const std::string_view assets_format = FOUND_NEW_ASSETS;
    const auto assets_offset = assets_format.find("%s");
    if (assets_offset != std::string_view::npos && starts_with(assets_format.substr(0, assets_offset)))
        return std::string(english ? "Found new resources: " : "发现新资源: ") +
               std::string(source.substr(assets_offset));
    if (!english) return text;

    static const std::pair<const char*, const char*> words[] = {
        {"设置", "Settings"},
        {"显示设置", "Display"},
        {"熄屏与待机显示", "Sleep and standby"},
        {"时钟与日期", "Clock and date"},
        {"待机页面", "Standby screen"},
        {"语言", "Language"},
        {"简体中文", "English"},
        {"移动数据", "Mobile data"},
        {"常亮显示", "Always on"},
        {"自动熄屏", "Screen timeout"},
        {"15 秒", "15 seconds"},
        {"30 秒", "30 seconds"},
        {"1 分钟", "1 minute"},
        {"2 分钟", "2 minutes"},
        {"5 分钟", "5 minutes"},
        {"时", "hr"},
        {"分", "min"},
        {"秒", "sec"},
        {"保存", "Save"},
        {"暂停", "Pause"},
        {"继续", "Resume"},
        {"永不", "Never"},
        {"电源", "Power"},
        {"关机", "Power off"},
        {"重启", "Restart"},
        {"继续按住 8 秒可强制关机", "Hold 8 seconds to force off"},
        {"正在关机…", "Powering off…"},
        {"正在重启…", "Restarting…"},
        {"系统更新", "Update"},
        {"关于手表", "About"},
        {"重置手表数据", "Reset watch"},
        {"再点一次确认", "Tap again"},
        {"不可撤销", "Permanent"},
        {"点击检查", "Check"},
        {"已连接", "Connected"},
        {"未连接", "Offline"},
        {"控制中心", "Control center"},
        {"静音", "Mute"},
        {"息屏", "Screen off"},
        {"省电", "Power save"},
        {"手电筒", "Flashlight"},
        {"AI 男友", "AI companion"},
        {"专属陪伴", "Companion"},
        {"随时陪着你", "Always with you"},
        {"哥哥", "Talk"},
        {"日历", "Calendar"},
        {"闹钟", "Alarm"},
        {"倒计时", "Timer"},
        {"应援灯", "Light"},
        {"新建闹钟", "New alarm"},
        {"编辑闹钟", "Edit alarm"},
        {"点击 + 添加闹钟", "Tap + to add"},
        {"添加闹钟", "Add alarm"},
        {"还没有闹钟", "No alarms yet"},
        {"为下一件期待的事设个提醒", "Make time for what matters"},
        {"留一点时间给自己", "A little time for yourself"},
        {"通过 AI 添加日程", "Ask AI to add an event"},
        {"联网后自动校准时间", "Connect to sync time"},
        {"开始", "Start"},
        {"正在倒计时", "Counting down"},
        {"已暂停", "Paused"},
        {"今天", "Today"},
        {"今天没有日程", "No events today"},
        {"这天没有日程", "No events"},
        {"时间待同步", "Awaiting time"},
        {"联网校时后显示日期与日程", "Connect to sync time"},
        {"基础颜色", "Colors"},
        {"✦ 应用", "Apply"},
        {"点亮应援灯", "Light up"},
        {"点击任意处退出", "Tap to close"},
        {"取消", "Cancel"},
        {"确认", "Confirm"},
        {"设备提示", "Notice"},
        {"知道了", "OK"},
        {"时间到了", "Time is up"},
        {"停止", "Stop"},
        {"稍后5分", "Snooze 5m"},
        {"空格", "Space"},
        {"连接", "Join"},
        {"输入 WLAN 密码", "Wi-Fi password"},
        {"点击网络可连接", "Tap a network to join"},
        {"正在扫描 WLAN…", "Scanning Wi-Fi…"},
        {"未发现网络，点击重新扫描", "No networks. Tap to rescan"},
        {"WLAN 服务暂不可用", "Wi-Fi unavailable"},
        {"密码至少 8 位", "At least 8 characters"},
        {"设备将重启以切换网络。", "Restart to switch network."},
        {"设备将重启并关闭网络。", "Restart with network off."},
        {"切换网络需要重新启动。", "Restart to switch network."},
        {"重新启动并检查固件更新？", "Restart to check for updates?"},
        {"日程", "Event"},
        {"倒计时结束", "Timer finished"},
        {"无网络", "Network off"},
        {"Wi-Fi 未连接", "Wi-Fi disconnected"},
        {"4G 未连接", "4G disconnected"},
        {"待校时", "Awaiting time sync"},
        {"思考中...", "Thinking..."},
        {"设备忙，请稍后切换网络", "Device busy. Switch networks later"},
        {"网络设置保存失败，请重试", "Cannot save network. Try again"},
        {"请结束对话或等待系统任务完成", "End chat or wait for system tasks"},
        {"正在连接4G，请稍候", "Connecting to 4G. Please wait"},
        {"切换4G？短按音量+确认", "Switch to 4G? Press Volume +"},
        {"切换WiFi？短按音量+确认", "Switch to Wi-Fi? Press Volume +"},
        {"请先开启 WLAN", "Turn on Wi-Fi first"},
        {"请先切换 Wi-Fi，并结束当前对话", "Switch to Wi-Fi and end chat first"},
        {"电量偏低，请及时充电", "Low battery. Please charge"},
        {"请先关闭提醒", "Dismiss the reminder first"},
        {"系统任务结束后可息屏", "Wait for system tasks to finish"},
        {"正在更新，请完成后重试", "Updating. Try again when finished"},
        {"请先开启 WLAN 或移动数据", "Turn on Wi-Fi or mobile data first"},
        {"正在准备网络，请稍候", "Preparing network. Please wait"},
        {"设备忙，请稍后重试", "Device busy. Try again later"},
        {"已重置手表数据，Wi-Fi 已保留", "Watch reset. Wi-Fi settings kept"},
        {"配网服务尚未就绪，请重试", "Wi-Fi setup not ready. Try again"},
        {"配网服务尚未就绪，请稍后重试", "Wi-Fi setup not ready. Try later"},
        {"配网服务地址不匹配", "Wi-Fi setup address mismatch"},
        {"无法创建配网请求", "Cannot create Wi-Fi setup request"},
        {"配网请求失败或超时，请重试", "Wi-Fi setup failed. Try again"},
        {"配网请求超时，请重试", "Wi-Fi setup timed out. Try again"},
        {"读取配网服务失败，请重试", "Cannot read Wi-Fi setup. Try again"},
        {"配网响应过大，请重试", "Wi-Fi reply too large. Try again"},
        {"请先切换 Wi-Fi 模式", "Switch to Wi-Fi mode first"},
        {"Wi-Fi 正在初始化", "Wi-Fi is starting"},
        {"请先结束对话或等待连接完成", "End chat or wait for connection"},
        {"附近网络扫描暂不可用，可用手机配网", "Scan unavailable. Set up by phone"},
        {"无法读取附近网络，请重试", "Cannot read networks. Try again"},
        {"无法启动扫描，请重试", "Cannot start scan. Try again"},
        {"名称最多31字节，密码最多63字节", "Name: max 31 bytes; password: 63"},
        {"正在处理 Wi-Fi，请稍后重试", "Wi-Fi busy. Try again later"},
        {"请结束对话或等待 Wi-Fi 就绪", "End chat or wait for Wi-Fi"},
        {"已保存10个网络，请先在手机配网页管理网络", "10 networks saved. Manage by phone"},
        {"无法创建连接请求", "Cannot create connection request"},
        {"连接失败，请检查密码和信号后重试", "Failed. Check password and signal"},
        {"连接参数保存失败，请重试", "Cannot save Wi-Fi. Try again"},
        {"无法启动连接，请重试", "Cannot start connection. Try again"},
        {"连接超时，可检查密码后重试", "Timed out. Check password and retry"},
        {"提醒存储尚未初始化", "Reminder storage not ready"},
        {"设置数值超出范围", "Setting value out of range"},
        {"闹钟时间或标题无效（标题最多60字节）", "Invalid alarm time or title\nTitle limit: 60 bytes"},
        {"闹钟已不存在", "Alarm no longer exists"},
        {"最多保存8个闹钟", "Limit: 8 alarms"},
        {"提醒编号已耗尽", "No reminder IDs available"},
        {"日程日期或标题无效（标题最多60字节）", "Invalid event date or title\nTitle limit: 60 bytes"},
        {"日程已不存在", "Event no longer exists"},
        {"最多保存16条日程", "Limit: 16 events"},
        {"日期或时间无效", "Invalid date or time"},
        {"设置系统时间失败", "Cannot set system time"},
        {"倒计时范围为1秒至24小时", "Timer range: 1 second to 24 hours"},
        {"没有运行中的倒计时", "No running timer"},
        {"倒计时已到时", "Timer has finished"},
        {"没有暂停的倒计时", "No paused timer"},
        {"稍后提醒范围为1秒至1小时", "Snooze range: 1 second to 1 hour"},
        {"没有正在响铃的提醒", "No active reminder"},
        {"日程日期无效", "Invalid event date"},
        {"未知倒计时操作", "Unknown timer action"},
    };
    for (const auto& word : words)
        if (text == word.first) return word.second;

    // Service errors append the ESP-IDF error token. Translate only the fixed
    // framing and keep that token byte-for-byte for troubleshooting.
    static constexpr std::pair<std::string_view, std::string_view> errors[] = {
        {"保存失败: ", "Save failed: "},
        {"读取提醒失败: ", "Cannot load reminders: "},
        {"提醒服务启动失败: ", "Cannot start reminders: "},
    };
    for (const auto& error : errors) {
        if (!starts_with(error.first)) continue;
        const auto detail = source.substr(error.first.size());
        if (detail.substr(0, 4) == "ESP_" || detail.substr(0, 2) == "0x")
            return std::string(error.second) + std::string(detail);
    }
    return text;
}

size_t NextCharacter(const std::string& s, size_t from) {
    if (from >= s.size())
        return s.size();
    ++from;
    while (from < s.size() && (static_cast<unsigned char>(s[from]) & 0xc0) == 0x80)
        ++from;
    return from;
}

size_t CharacterCount(const std::string& s) {
    size_t count = 0;
    for (size_t pos = 0; pos < s.size(); pos = NextCharacter(s, pos))
        ++count;
    return count;
}

void SetLabelText(lv_obj_t* label, const char* text) {
    // Apply tracking before LVGL measures or wraps changing prose. Numeric
    // clocks retain their separately specified negative tracking.
    lv_obj_set_style_text_letter_space(label, HasCjk(text) ? 1 : 0, 0);
    lv_label_set_text(label, text);
}

void SingleLine(lv_obj_t* label) {
    // DOT needs a finite height; content-sized labels otherwise keep wrapping.
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

uint32_t BlendColor(uint32_t a, uint32_t b, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    uint32_t color = 0;
    for (int shift : {0, 8, 16}) {
        int first = (a >> shift) & 255, last = (b >> shift) & 255;
        color |= static_cast<uint32_t>(first + (last - first) * amount) << shift;
    }
    return color;
}

std::tm Date(int64_t now) {
    std::tm d{};
    time_t t = now;
    gmtime_r(&t, &d);
    return d;
}

std::string Number(int n) {
    char s[16];
    std::snprintf(s, sizeof(s), "%02d", n);
    return s;
}

std::string CountdownText(uint32_t seconds, bool hours) {
    return hours ? Number(seconds / 3600) + ":" + Number(seconds / 60 % 60) + ":" + Number(seconds % 60)
                 : Number(seconds / 60) + ":" + Number(seconds % 60);
}

std::string AlarmRepeat(uint8_t weekdays, bool english) {
    if (weekdays == 0) return english ? "Once" : "仅一次";
    if (weekdays == 0x7f) return english ? "Every day" : "每天";
    if (weekdays == 0x1f) return english ? "Weekdays" : "工作日";
    if (weekdays == 0x60) return english ? "Weekends" : "周末";
    constexpr const char* short_days[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    std::string value;
    for (int d = 0; d < 7; ++d) {
        if (!(weekdays & (1 << d))) continue;
        if (!value.empty()) value += ' ';
        value += english ? short_days[d] : kDays[(d + 1) % 7];
    }
    return value;
}

void SetMenuPlateScale(void* obj, int32_t scale) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), scale, 0);
}

void AnimateMenuPlate(lv_obj_t* plate, int scale, int duration) {
    const int current = lv_obj_get_style_transform_scale_x(plate, LV_PART_MAIN);
    lv_anim_delete(plate, SetMenuPlateScale);
    if (current == scale) return;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, plate);
    lv_anim_set_exec_cb(&animation, SetMenuPlateScale);
    lv_anim_set_values(&animation, current, scale);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void AnimateY(lv_obj_t* obj, int from, int to, int duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_exec_cb(&a, [](void* o, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t*>(o), y); });
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
}  // namespace baji::ui

const lv_font_t* WatchUi::Font(float px, int weight) {
    auto* f = WatchResources::Font(px, weight);
    return f ? f : fallback_font_;
}

void WatchUi::Bind(lv_obj_t* o, std::function<void(lv_event_t*)> f) {
    lv_obj_add_event_cb(o, Dispatch, LV_EVENT_ALL, new Binding{std::move(f)});
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

lv_obj_t* WatchUi::Box(lv_obj_t* p, int x, int y, int w, int h, uint32_t c, int r, lv_opa_t opa) {
    auto* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    // Decorative boxes must never steal a button's hit test. Interactive
    // surfaces and scroll areas opt in explicitly below.
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}

lv_obj_t* WatchUi::Text(lv_obj_t* p, const char* s, int x, int y, int w, float size, uint32_t color,
                        int weight, bool center, float line_height, float cjk_size) {
    auto* o = lv_label_create(p);
    const std::string text = LocalizedText(s ? s : "", snapshot_.settings.language != 0);
    const bool chinese = HasCjk(text.c_str());
    if (chinese)
        size = std::max(size, cjk_size);
    SetLabelText(o, text.c_str());
    // CSS places font metrics inside a line box using half-leading. A 41px
    // Segoe face has 56px natural metrics, so treating y as the font top put
    // the clock about eight pixels too low in the previous implementation.
    const auto* font = Font(size, weight);
    const float css_line = line_height > 0 ? (chinese ? std::max(line_height, size) : line_height)
                                           : size * 1.5f;
    lv_obj_set_pos(o, x, y + std::lround((css_line - font->line_height) * .5f));
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_line_space(o, std::lround(css_line) - font->line_height, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* WatchUi::Icon(lv_obj_t* p, const char* name, int x, int y, int size, uint32_t color) {
    auto* o = lv_image_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, size, size);
    if (auto* d = WatchResources::Icon(name, size, color))
        lv_image_set_src(o, d);
    lv_obj_set_style_image_recolor(o, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(o, 255, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void WatchUi::Border(lv_obj_t* o, uint32_t c, lv_opa_t a, int w) {
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_opa(o, a, 0);
}

lv_obj_t* WatchUi::CenteredIcon(lv_obj_t* parent, const char* name, int size, uint32_t color) {
    auto* icon = Icon(parent, name, 0, 0, size, color);
    // LVGL centers within the content area, including any parent border.
    lv_obj_center(icon);
    return icon;
}

void WatchUi::Gradient(lv_obj_t* o, uint32_t a, uint32_t b) {
    // Only menu icon tiles use small native RGB565 diagonal fills.
    lv_obj_update_layout(o);
    const int width = lv_obj_get_width(o), height = lv_obj_get_height(o);
    if (width < 1 || height < 1)
        return;
    auto texture = std::make_shared<GradientTexture>();
    texture->pixels.resize(width * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float position = static_cast<float>(x + y) / std::max(1, width + height - 2);
            const uint32_t color = BlendColor(a, b, position);
            // Ordered dither avoids visible diagonal RGB565 quantization bands.
            constexpr int bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            float threshold = (bayer[(y % 4) * 4 + x % 4] + .5f) / 16;
            int red =
                std::clamp(static_cast<int>(((color >> 16) & 255) * 31 / 255.0f + threshold), 0, 31);
            int green =
                std::clamp(static_cast<int>(((color >> 8) & 255) * 63 / 255.0f + threshold), 0, 63);
            int blue = std::clamp(static_cast<int>((color & 255) * 31 / 255.0f + threshold), 0, 31);
            texture->pixels[y * width + x] = red << 11 | green << 5 | blue;
        }
    texture->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    texture->image.header.cf = LV_COLOR_FORMAT_RGB565;
    texture->image.header.w = width;
    texture->image.header.h = height;
    texture->image.header.stride = width * 2;
    texture->image.data_size = texture->pixels.size() * 2;
    texture->image.data = reinterpret_cast<uint8_t*>(texture->pixels.data());
    lv_obj_set_style_bg_image_src(o, &texture->image, 0);
    lv_obj_set_style_clip_corner(o, true, 0);
    auto* owner = new std::shared_ptr<GradientTexture>(std::move(texture));
    lv_obj_add_event_cb(
        o,
        [](lv_event_t* e) {
            auto* value = static_cast<std::shared_ptr<GradientTexture>*>(lv_event_get_user_data(e));
            lv_image_cache_drop(&(*value)->image);
            delete value;
        },
        LV_EVENT_DELETE, owner);
}

void WatchUi::Glow(lv_obj_t* o, uint32_t c, int w, lv_opa_t a) {
    lv_obj_set_style_shadow_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_shadow_width(o, w, 0);
    lv_obj_set_style_shadow_opa(o, a, 0);
}

void WatchUi::Fade(lv_obj_t* parent, int x, int y, int width, int height, uint32_t color,
                   bool horizontal, bool reverse) {
    // Use the same A8 composition path as the vignette. LVGL's software
    // rectangle-gradient path on this board darkens the entire mask bounds.
    auto mask = std::make_shared<AlphaTexture>(width, height);
    int span = std::max(1, (horizontal ? width : height) - 1);
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column) {
            int opacity = (horizontal ? column : row) * 255 / span;
            mask->pixels[row * width + column] = reverse ? opacity : 255 - opacity;
        }
    auto* shade = lv_image_create(parent);
    lv_image_set_src(shade, &mask->image);
    lv_obj_set_pos(shade, x, y);
    lv_obj_set_style_image_recolor(shade, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(shade, 255, 0);
    lv_obj_remove_flag(shade, LV_OBJ_FLAG_CLICKABLE);
    auto* owner = new std::shared_ptr<AlphaTexture>(std::move(mask));
    lv_obj_add_event_cb(
        shade,
        [](lv_event_t* e) {
            auto* value = static_cast<std::shared_ptr<AlphaTexture>*>(lv_event_get_user_data(e));
            lv_image_cache_drop(&(*value)->image);
            delete value;
        },
        LV_EVENT_DELETE, owner);
}

lv_obj_t* WatchUi::Button(lv_obj_t* p, int x, int y, int w, int h, uint32_t c, int r,
                          std::function<void()> fn, const char* label, float fs, int weight) {
    auto* o = Box(p, x, y, w, h, c, r);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    // Keep glyphs, icons and the hit area on their native pixel grid. Only
    // the surface changes on press; resampling the entire tree blurs text.
    lv_obj_set_style_bg_color(o, lv_color_hex(BlendColor(c, 0xffffff, .10f)), LV_STATE_PRESSED);
    if (label) {
        auto* t = Text(o, label, 0, 0, w, fs, 0xffffff, weight, false, 0, 14);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(t);
    }
    struct Press { lv_point_t origin{}; uint32_t generation = 0; bool active = false, dragged = false; };
    auto press = std::make_shared<Press>();
    Bind(o, [this, press, fn = std::move(fn)](lv_event_t* e) {
        const auto code = lv_event_get_code(e);
        auto* input = lv_indev_active();
        if (code == LV_EVENT_PRESSED) {
            press->generation = menu_input_generation_;
            press->dragged = false;
            press->active = input != nullptr;
            if (input) lv_indev_get_point(input, &press->origin);
        }
        // Overlays consume page gestures. Each button therefore owns drag
        // cancellation too, including a fast move immediately before release.
        if (input && press->active && (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED)) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            press->dragged |= std::abs(point.x - press->origin.x) > 8 ||
                              std::abs(point.y - press->origin.y) > 8;
            if (code == LV_EVENT_RELEASED) {
                lv_area_t bounds;
                lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_current_target(e)), &bounds);
                press->dragged |= point.x < bounds.x1 || point.x > bounds.x2 ||
                                  point.y < bounds.y1 || point.y > bounds.y2;
            }
        }
        if (code == LV_EVENT_PRESS_LOST) press->active = false;
        if (code == LV_EVENT_CLICKED && press->active && !press->dragged &&
            awake_ && !swiped_ && !power_transition_ && press->generation == menu_input_generation_) {
            press->active = false;
            Emit(Action::Activity);
            fn();
        }
    });
    return o;
}

void WatchUi::Back(lv_obj_t* p, std::function<void()> cb) {
    // Borrow BAJI's light, translucent close-control surface while retaining
    // this board's explicit back action and existing touch target.
    const bool on_photo = page_ == Page::Chat;
    auto* b = Button(p, 60, 44, 36, 36, on_photo ? 0x000000 : 0xffffff, LV_RADIUS_CIRCLE,
                     cb ? std::move(cb) : [this] { GoBack(); });
    lv_obj_set_style_bg_opa(b, on_photo ? 160 : 20, 0);
    lv_obj_set_style_bg_opa(b, on_photo ? 192 : 35, LV_STATE_PRESSED);
    auto* arrow = CenteredIcon(b, "arrow-left", 18);
    lv_obj_set_style_image_opa(arrow, on_photo ? LV_OPA_COVER : 178, 0);
}

void WatchUi::PageTitle(const char* title, uint32_t accent) {
    Back(content_);
    auto* text = Text(content_, title, 104, 50, 152, 16, kText, 400, true, 24);
    SingleLine(text);
    Box(content_, 168, 80, 24, 2, accent, 1, 180);
}

lv_obj_t* WatchUi::ScrollList(lv_obj_t* parent, int x, int y, int width, int height) {
    auto* list = Box(parent, x, y, width, height, kBg);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    return list;
}

void WatchUi::EmptyState(lv_obj_t* parent, const char* icon, const char* title, const char* hint,
                         uint32_t accent, int y) {
    auto* tile = Box(parent, 156, y, 48, 48, BlendColor(kBg, accent, .15f), 16);
    Border(tile, accent, 40);
    CenteredIcon(tile, icon, 20, accent);
    Text(parent, title, 66, y + 62, 228, 14, kText, 400, true, 22);
    Text(parent, hint, 72, y + 91, 216, 12, kMuted, 400, true, 18);
}

void WatchUi::Switch(lv_obj_t* p, bool on, uint32_t tint, std::function<void()> cb) {
    auto* hit = Button(p, 0, 0, 44, 36, 0, 0, std::move(cb));
    lv_obj_set_style_bg_opa(hit, 0, 0);
    // Center the complete control in its row, including the touch target.
    // Parent content alignment also accounts for the row's border.
    lv_obj_align(hit, LV_ALIGN_RIGHT_MID, -10, 0);
    auto* bar = Box(hit, 0, 0, 40, 22, on ? tint : 0x34343e, 11);
    Border(bar, on ? tint : 0xffffff, on ? 140 : 25);
    lv_obj_center(bar);
    auto* thumb = Box(bar, 0, 0, 16, 16, on ? 0xffffff : 0xa4a4af, LV_RADIUS_CIRCLE);
    // The track has a 1px border: alignment gives equal 3px outer clearance,
    // unlike child y=3, which produced 4px above and 2px below the thumb.
    lv_obj_align(thumb, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
}

lv_obj_t* WatchUi::Roller(lv_obj_t* parent, int x, int y, int maximum, int value, uint32_t accent) {
    auto* selection = Box(parent, x, y + 44, 80, 44, BlendColor(kBg, accent, .16f), 13);
    Border(selection, accent, 42);
    auto* roller = lv_roller_create(parent);
    lv_obj_remove_style_all(roller);
    std::string options;
    for (int n = 0; n <= maximum; ++n) {
        if (n) options += '\n';
        options += Number(n);
    }
    lv_roller_set_options(roller, options.c_str(), LV_ROLLER_MODE_NORMAL);
    lv_obj_set_pos(roller, x, y);
    lv_obj_set_size(roller, 80, 132);
    lv_obj_set_style_text_font(roller, Font(20), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 44 - lv_font_get_line_height(Font(20)), LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, Font(32), LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(accent), LV_PART_SELECTED);
    lv_obj_set_style_text_letter_space(roller, -1, LV_PART_SELECTED);
    lv_roller_set_selected(roller, value, LV_ANIM_OFF);
    Bind(roller, [this](lv_event_t* event) {
        if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) Emit(Action::Activity);
    });
    Fade(parent, x, y, 80, 25, kBg, false, false);
    Fade(parent, x, y + 107, 80, 25, kBg, false, true);
    return roller;
}

void WatchUi::DrawTabs(bool countdown) {
    Back(content_);
    auto* bar = Box(content_, 106, 44, 172, 36, kSurface, 18);
    Border(bar, 0xffffff, 20);
    auto* alarm = Button(bar, 0, 0, 76, 30, countdown ? kSurface : 0x624924, 15,
                          [this] { Navigate(Page::Alarms); });
    // 2px inside the 1px border leaves 3px around the selected capsule.
    lv_obj_align(alarm, LV_ALIGN_LEFT_MID, 2, 0);
    auto* alarm_label = Text(alarm, "闹钟", 0, 0, 76, 14, countdown ? kMuted : 0xffdf9a,
                             400, true, 22);
    SingleLine(alarm_label);
    lv_obj_center(alarm_label);
    auto* timer = Button(bar, 0, 0, 87, 30, countdown ? 0x20564d : kSurface, 15,
                          [this] { Navigate(Page::Countdown); });
    lv_obj_align(timer, LV_ALIGN_RIGHT_MID, -2, 0);
    auto* timer_label = Text(timer, "倒计时", 0, 0, 87, 14, countdown ? 0xb8f7e9 : kMuted,
                             400, true, 22);
    SingleLine(timer_label);
    lv_obj_center(timer_label);
}

lv_obj_t* WatchUi::SettingsRow(lv_obj_t* parent, int y, const char* icon, const char* title,
                              uint32_t accent, std::function<void()> action) {
    auto* item = Button(parent, 0, y, 240, 60, kSurface, 14, std::move(action));
    Border(item, 0xffffff, 22);
    auto* tile = Box(item, 12, 16, 28, 28, BlendColor(kSurface, accent, .14f), 9);
    CenteredIcon(tile, icon, 18, accent);
    auto* label = Text(item, title, 54, 8, 128, 14, kText, 400, false, 22);
    SingleLine(label);
    return item;
}

void WatchUi::RefreshClock(const std::string& time) {
    if (clock_) {
        if (clock_minute_) {
            const auto hours = time.substr(0, 2), minutes = time.substr(3, 2);
            if (hours != lv_label_get_text(clock_)) lv_label_set_text(clock_, hours.c_str());
            if (minutes != lv_label_get_text(clock_minute_)) lv_label_set_text(clock_minute_, minutes.c_str());
            lv_obj_update_layout(clock_);
            lv_obj_update_layout(clock_colon_);
            lv_obj_update_layout(clock_minute_);
            int left = (360 - lv_obj_get_width(clock_) - lv_obj_get_width(clock_minute_) -
                        lv_obj_get_width(clock_colon_) - 4) / 2;
            lv_obj_set_x(clock_, left);
            lv_obj_set_x(clock_colon_, left + lv_obj_get_width(clock_) + 2);
            lv_obj_set_x(clock_minute_, left + lv_obj_get_width(clock_) + 4 + lv_obj_get_width(clock_colon_));
        } else if (time != lv_label_get_text(clock_)) lv_label_set_text(clock_, time.c_str());
    }
    if (date_) {
        std::string caption = snapshot_.settings.language ? "Awaiting time" : "时间待同步";
        if (snapshot_.time_valid) {
            const auto date = Date(snapshot_.now);
            constexpr const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            char formatted[48];
            if (snapshot_.settings.language)
                std::snprintf(formatted, sizeof(formatted), "%02d / %02d · %s", date.tm_mon + 1,
                              date.tm_mday, weekdays[date.tm_wday]);
            else
                std::snprintf(formatted, sizeof(formatted), "%d月%d日 星期%s", date.tm_mon + 1,
                              date.tm_mday, kDays[date.tm_wday]);
            caption = formatted;
        }
        if (caption != lv_label_get_text(date_)) SetLabelText(date_, caption.c_str());
    }
}

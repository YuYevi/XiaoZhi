#include "watch_services.h"

#include "application.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {
constexpr char kTag[] = "WatchServices";
constexpr char kNamespace[] = "watch";
constexpr int64_t kCatchUpSeconds = 300;
constexpr int64_t kUsPerSecond = 1000000;
constexpr int64_t kDaySeconds = 86400;
constexpr size_t kMaxBlobBytes = 4096;

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool ValidTitle(const std::string& title) {
    if (title.empty() || title.size() > WatchServices::kMaxTitleBytes) return false;
    for (unsigned char c : title) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool ValidSettings(const WatchSettings& settings) {
    return settings.screen_timeout_seconds <= 3600 &&
           settings.wallpaper_interval_seconds <= 3600 && settings.alarm_volume <= 100 && settings.language <= 1;
}

int64_t DayStart(int64_t when) { return when - when % kDaySeconds; }

// System time follows the native OTA convention: already shifted to local time.
int WeekdayBit(int64_t day) {
    return static_cast<int>((day / kDaySeconds + 3) % 7);  // 1970-01-01: Thursday
}

int64_t NextAlarm(const WatchAlarm& alarm, int64_t now, bool allow_current_minute) {
    const int64_t today = DayStart(now);
    for (int offset = 0; offset <= 7; ++offset) {
        const int64_t day = today + offset * kDaySeconds;
        const int64_t due = day + alarm.hour * 3600 + alarm.minute * 60;
        if (due < now - (allow_current_minute ? now % 60 : 0)) continue;
        if (!alarm.weekdays || (alarm.weekdays & (1 << WeekdayBit(day)))) return due;
    }
    return 0;
}

struct PersistentState {
    WatchSettings settings;
    uint32_t next_id = 1;
    std::vector<WatchAlarm> alarms;
    std::vector<WatchEvent> events;
};

void Put(std::vector<uint8_t>& data, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) data.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void PutString(std::vector<uint8_t>& data, const std::string& value) {
    Put(data, value.size(), 1);
    data.insert(data.end(), value.begin(), value.end());
}

uint32_t Checksum(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

std::vector<uint8_t> Encode(const PersistentState& state) {
    std::vector<uint8_t> data;
    data.reserve(2300);
    Put(data, 0x31544357, 4);  // WCT1, explicit format version; never serialize struct padding
    Put(data, state.next_id, 4);
    Put(data, state.settings.screen_timeout_seconds, 2);
    Put(data, state.settings.wallpaper_interval_seconds, 2);
    // Keep flag bit 0 reserved so previously saved settings retain their layout.
    Put(data, (state.settings.do_not_disturb ? 2 : 0) |
              (state.settings.show_clock ? 4 : 0) |
              (state.settings.power_save ? 8 : 0) |
              (state.settings.language == 1 ? 16 : 0), 1);
    Put(data, state.settings.alarm_volume, 1);
    Put(data, state.alarms.size(), 1);
    Put(data, state.events.size(), 1);
    for (const auto& alarm : state.alarms) {
        Put(data, alarm.id, 4); Put(data, alarm.hour, 1); Put(data, alarm.minute, 1);
        Put(data, alarm.weekdays, 1); Put(data, alarm.enabled, 1);
        Put(data, alarm.next_due, 8); Put(data, alarm.last_fired, 8);
        PutString(data, alarm.title);
    }
    for (const auto& event : state.events) {
        Put(data, event.id, 4); Put(data, event.due, 8);
        Put(data, (event.completed ? 1 : 0) | (event.notified ? 2 : 0), 1);
        PutString(data, event.title);
    }
    Put(data, Checksum(data.data(), data.size()), 4);
    return data;
}

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
    uint64_t Get(unsigned count) {
        if (offset_ + count > data_.size() - 4) { ok_ = false; return 0; }
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= uint64_t(data_[offset_++]) << (8 * i);
        return value;
    }
    std::string String() {
        const auto size = Get(1);
        if (!ok_ || size > WatchServices::kMaxTitleBytes || offset_ + size > data_.size() - 4) {
            ok_ = false; return {};
        }
        std::string result(reinterpret_cast<const char*>(data_.data() + offset_), size);
        offset_ += size;
        return result;
    }
    bool Done() const { return ok_ && offset_ == data_.size() - 4; }
private:
    const std::vector<uint8_t>& data_;
    size_t offset_ = 0;
    bool ok_ = true;
};

bool Decode(const std::vector<uint8_t>& data, PersistentState* state) {
    if (data.size() < 20 || data.size() > kMaxBlobBytes) return false;
    uint32_t stored_hash = 0;
    for (unsigned i = 0; i < 4; ++i) stored_hash |= uint32_t(data[data.size() - 4 + i]) << (8 * i);
    if (stored_hash != Checksum(data.data(), data.size() - 4)) return false;
    Reader reader(data);
    if (reader.Get(4) != 0x31544357) return false;
    PersistentState value;
    value.next_id = reader.Get(4);
    value.settings.screen_timeout_seconds = reader.Get(2);
    value.settings.wallpaper_interval_seconds = reader.Get(2);
    const auto flags = reader.Get(1);
    // The retired flag bit 0 is intentionally ignored for existing records.
    value.settings.do_not_disturb = flags & 2;
    value.settings.show_clock = flags & 4;
    value.settings.power_save = flags & 8;
    value.settings.language = (flags & 16) ? 1 : 0;
    value.settings.alarm_volume = reader.Get(1);
    const auto alarm_count = reader.Get(1), event_count = reader.Get(1);
    if (alarm_count > WatchServices::kMaxAlarms || event_count > WatchServices::kMaxEvents ||
        !ValidSettings(value.settings) || !value.next_id) return false;
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < alarm_count; ++i) {
        WatchAlarm alarm;
        alarm.id = reader.Get(4); alarm.hour = reader.Get(1); alarm.minute = reader.Get(1);
        alarm.weekdays = reader.Get(1); alarm.enabled = reader.Get(1);
        alarm.next_due = reader.Get(8); alarm.last_fired = reader.Get(8);
        alarm.title = reader.String();
        if (!alarm.id || alarm.id >= value.next_id || alarm.hour > 23 || alarm.minute > 59 ||
            alarm.weekdays > 127 || !ValidTitle(alarm.title) ||
            (alarm.next_due && !WatchServices::IsTimeValid(alarm.next_due)) ||
            (alarm.last_fired && !WatchServices::IsTimeValid(alarm.last_fired))) return false;
        ids.push_back(alarm.id);
        value.alarms.push_back(std::move(alarm));
    }
    for (size_t i = 0; i < event_count; ++i) {
        WatchEvent event;
        event.id = reader.Get(4); event.due = reader.Get(8);
        const auto event_flags = reader.Get(1);
        event.completed = event_flags & 1; event.notified = event_flags & 2;
        event.title = reader.String();
        if (!event.id || event.id >= value.next_id || !WatchServices::IsTimeValid(event.due) ||
            !ValidTitle(event.title)) return false;
        ids.push_back(event.id);
        value.events.push_back(std::move(event));
    }
    std::sort(ids.begin(), ids.end());
    if (!reader.Done() || std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return false;
    *state = std::move(value);
    return true;
}

std::string FormatTime(int64_t timestamp) {
    time_t value = static_cast<time_t>(timestamp);
    tm parts{};
    gmtime_r(&value, &parts);
    char output[32];
    strftime(output, sizeof(output), "%Y-%m-%d %H:%M", &parts);
    return output;
}

std::string JsonString(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') result += '\\';
        if (c < 0x20) { result += ' '; continue; }
        result += static_cast<char>(c);
    }
    return result + '"';
}
}  // namespace

struct WatchServices::Impl {
    mutable std::mutex mutex;
    PersistentState state;
    bool initialized = false;
    bool storage_blocked = false;
    bool tools_registered = false;
    uint8_t active_slot = 1;
    uint32_t revision = 0;
    std::string storage_error;
    esp_timer_handle_t timer = nullptr;
    std::atomic<bool> tick_pending{false};
    std::function<void(const std::vector<WatchReminder>&)> callback;
    std::vector<WatchReminder> reminders;
    std::vector<WatchReminder> snoozed;
    int64_t snooze_deadline_us = 0;
    uint32_t next_token = 1;
    WatchCountdown countdown;
    int64_t countdown_deadline_us = 0;
    int64_t countdown_remaining_us = 0;
    int64_t save_retry_after_us = 0;
    std::map<uint32_t, int64_t> runtime_fired;

    bool Save(const PersistentState& candidate, std::string* error) {
        if (!initialized || storage_blocked) return Fail(error, storage_error.empty() ? "提醒存储尚未初始化" : storage_error);
        nvs_handle_t handle = 0;
        esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
        const uint8_t next_slot = active_slot ^ 1;
        if (result == ESP_OK) {
            const auto blob = Encode(candidate);
            result = nvs_set_blob(handle, next_slot ? "state1" : "state0", blob.data(), blob.size());
            if (result == ESP_OK) result = nvs_commit(handle);
            // Only publish the completed copy. On power loss/failure the old blob remains intact.
            if (result == ESP_OK) result = nvs_set_u8(handle, "active", next_slot);
            if (result == ESP_OK) result = nvs_commit(handle);
            nvs_close(handle);
        }
        if (result != ESP_OK) {
            storage_error = std::string("保存失败: ") + esp_err_to_name(result);
            ESP_LOGE(kTag, "%s", storage_error.c_str());
            ++revision;
            return Fail(error, storage_error);
        }
        active_slot = next_slot;
        state = candidate;
        storage_error.clear();
        ++revision;
        return true;
    }
};

WatchServices& WatchServices::GetInstance() {
    static WatchServices instance;
    return instance;
}

WatchServices::WatchServices() : impl_(new Impl) {}
WatchServices::~WatchServices() {
    if (impl_->timer) { esp_timer_stop(impl_->timer); esp_timer_delete(impl_->timer); }
}

bool WatchServices::Initialize(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->initialized) return !impl_->storage_blocked || Fail(error, impl_->storage_error);
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        impl_->initialized = true;
        return true;
    }
    if (result == ESP_OK) {
        uint8_t selected = 1;
        result = nvs_get_u8(handle, "active", &selected);
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;  // interrupted first save: no published state
        else if (result == ESP_OK) {
            if (selected > 1) result = ESP_ERR_INVALID_STATE;
            else {
                size_t size = 0;
                result = nvs_get_blob(handle, selected ? "state1" : "state0", nullptr, &size);
                if (result == ESP_OK && (size < 20 || size > kMaxBlobBytes)) result = ESP_ERR_INVALID_SIZE;
                if (result == ESP_OK) {
                    std::vector<uint8_t> blob(size);
                    result = nvs_get_blob(handle, selected ? "state1" : "state0", blob.data(), &size);
                    if (result == ESP_OK && !Decode(blob, &impl_->state)) result = ESP_ERR_INVALID_STATE;
                    if (result == ESP_OK) impl_->active_slot = selected;
                }
            }
        }
        nvs_close(handle);
    }
    impl_->initialized = true;
    if (result != ESP_OK) {
        impl_->storage_blocked = true;
        impl_->storage_error = std::string("读取提醒失败: ") + esp_err_to_name(result);
        return Fail(error, impl_->storage_error);
    }
    ESP_LOGI(kTag, "Loaded %u alarms and %u events", unsigned(impl_->state.alarms.size()), unsigned(impl_->state.events.size()));
    return true;
}

bool WatchServices::Start(std::string* error) {
    // Even if storage is damaged, relative timers and a visible storage error remain available.
    Initialize(error);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->timer) return true;
    esp_timer_create_args_t args{};
    args.callback = [](void* arg) {
        auto* self = static_cast<WatchServices*>(arg);
        if (!self->impl_->tick_pending.exchange(true)) {
            Application::GetInstance().Schedule([self]() {
                self->Tick();
                self->impl_->tick_pending = false;
            });
        }
    };
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "watch_clock";
    args.skip_unhandled_events = true;
    esp_err_t result = esp_timer_create(&args, &impl_->timer);
    if (result == ESP_OK) result = esp_timer_start_periodic(impl_->timer, kUsPerSecond);
    if (result != ESP_OK) {
        if (impl_->timer) esp_timer_delete(impl_->timer);
        impl_->timer = nullptr;
        return Fail(error, std::string("提醒服务启动失败: ") + esp_err_to_name(result));
    }
    return true;
}

bool WatchServices::IsTimeValid(int64_t value) {
    return value >= 1704067200LL && value < 4102444800LL;  // 2024-01-01 .. 2099-12-31
}

bool WatchServices::MakeLocalTime(int year, int month, int day, int hour, int minute, int64_t* result) {
    if (!result || year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month - 1] + ((month == 2 && leap) ? 1 : 0)) return false;
    // Civil date -> days since 1970; independent of the process TZ environment.
    const int y = year - (month <= 2);
    const int era = y / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    *result = (int64_t(era) * 146097 + doe - 719468) * kDaySeconds + hour * 3600 + minute * 60;
    return true;
}

WatchSnapshot WatchServices::Snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    WatchSnapshot snapshot;
    snapshot.settings = impl_->state.settings;
    snapshot.alarms = impl_->state.alarms;
    snapshot.events = impl_->state.events;
    snapshot.reminders = impl_->reminders;
    snapshot.countdown = impl_->countdown;
    if (snapshot.countdown.active) {
        const int64_t remaining = snapshot.countdown.paused ? impl_->countdown_remaining_us :
            std::max<int64_t>(0, impl_->countdown_deadline_us - esp_timer_get_time());
        snapshot.countdown.remaining_seconds = (remaining + kUsPerSecond - 1) / kUsPerSecond;
    }
    snapshot.now = time(nullptr);
    snapshot.time_valid = IsTimeValid(snapshot.now);
    snapshot.revision = impl_->revision;
    snapshot.storage_error = impl_->storage_error;
    std::sort(snapshot.events.begin(), snapshot.events.end(), [](const auto& a, const auto& b) { return a.due < b.due; });
    return snapshot;
}

bool WatchServices::SaveSettings(const WatchSettings& settings, std::string* error) {
    if (!ValidSettings(settings)) return Fail(error, "设置数值超出范围");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto state = impl_->state;
    state.settings = settings;
    return impl_->Save(state, error);
}

bool WatchServices::ResetWatchData(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->Save(PersistentState{}, error)) return false;
    impl_->reminders.clear();
    impl_->snoozed.clear();
    impl_->runtime_fired.clear();
    impl_->countdown = {};
    impl_->countdown_remaining_us = 0;
    impl_->countdown_deadline_us = 0;
    impl_->save_retry_after_us = 0;
    return true;
}

bool WatchServices::SaveAlarm(const WatchAlarm& input, uint32_t* assigned_id, std::string* error) {
    if (input.hour > 23 || input.minute > 59 || input.weekdays > 127 || !ValidTitle(input.title))
        return Fail(error, "闹钟时间或标题无效（标题最多60字节）");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto state = impl_->state;
    auto item = std::find_if(state.alarms.begin(), state.alarms.end(), [&](const auto& alarm) { return alarm.id == input.id; });
    WatchAlarm alarm = input;
    bool reset_due = true;
    if (input.id) {
        if (item == state.alarms.end()) return Fail(error, "闹钟已不存在");
        reset_due = item->hour != input.hour || item->minute != input.minute ||
                    item->weekdays != input.weekdays || (!item->enabled && input.enabled);
        alarm.last_fired = item->last_fired;
        alarm.next_due = item->next_due;
    } else {
        if (state.alarms.size() >= kMaxAlarms) return Fail(error, "最多保存8个闹钟");
        if (state.next_id == UINT32_MAX) return Fail(error, "提醒编号已耗尽");
        alarm.id = state.next_id++;
        alarm.last_fired = 0;
    }
    if (reset_due) {
        const int64_t now = time(nullptr);
        alarm.next_due = alarm.enabled && IsTimeValid(now) ? NextAlarm(alarm, now, false) : 0;
        if (IsTimeValid(now)) alarm.last_fired = std::max(alarm.last_fired, now - 1);
        // Editing a fired alarm must not make a backward clock correction ring it again.
    }
    if (input.id) *item = alarm;
    else state.alarms.push_back(alarm);
    if (!impl_->Save(state, error)) return false;
    if (assigned_id) *assigned_id = alarm.id;
    return true;
}

bool WatchServices::DeleteAlarm(uint32_t id, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto state = impl_->state;
    auto item = std::find_if(state.alarms.begin(), state.alarms.end(), [id](const auto& alarm) { return alarm.id == id; });
    if (item == state.alarms.end()) return Fail(error, "闹钟已不存在");
    state.alarms.erase(item);
    if (!impl_->Save(state, error)) return false;
    impl_->runtime_fired.erase(id);
    return true;
}

bool WatchServices::SaveEvent(const WatchEvent& input, uint32_t* assigned_id, std::string* error) {
    if (!ValidTitle(input.title) || !IsTimeValid(input.due)) return Fail(error, "日程日期或标题无效（标题最多60字节）");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto state = impl_->state;
    auto item = std::find_if(state.events.begin(), state.events.end(), [&](const auto& event) { return event.id == input.id; });
    WatchEvent event = input;
    if (input.id) {
        if (item == state.events.end()) return Fail(error, "日程已不存在");
        event.notified = item->due == input.due ? item->notified : false;
    } else {
        if (state.events.size() >= kMaxEvents) return Fail(error, "最多保存16条日程");
        if (state.next_id == UINT32_MAX) return Fail(error, "提醒编号已耗尽");
        event.id = state.next_id++;
        event.notified = false;
    }
    if (input.id) *item = event;
    else state.events.push_back(event);
    if (!impl_->Save(state, error)) return false;
    if (assigned_id) *assigned_id = event.id;
    return true;
}

bool WatchServices::DeleteEvent(uint32_t id, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto state = impl_->state;
    auto item = std::find_if(state.events.begin(), state.events.end(), [id](const auto& event) { return event.id == id; });
    if (item == state.events.end()) return Fail(error, "日程已不存在");
    state.events.erase(item);
    if (!impl_->Save(state, error)) return false;
    impl_->runtime_fired.erase(id);
    return true;
}

bool WatchServices::SetLocalTime(int year, int month, int day, int hour, int minute, std::string* error) {
    int64_t when = 0;
    if (!MakeLocalTime(year, month, day, hour, minute, &when)) return Fail(error, "日期或时间无效");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    timeval tv{};
    tv.tv_sec = when;
    if (settimeofday(&tv, nullptr) != 0) return Fail(error, "设置系统时间失败");
    ++impl_->revision;
    return true;
}

bool WatchServices::StartCountdown(uint32_t seconds, std::string* error) {
    if (!seconds || seconds > 86400) return Fail(error, "倒计时范围为1秒至24小时");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->countdown = {true, false, seconds, seconds};
    impl_->countdown_remaining_us = int64_t(seconds) * kUsPerSecond;
    impl_->countdown_deadline_us = esp_timer_get_time() + impl_->countdown_remaining_us;
    ++impl_->revision;
    return true;
}

bool WatchServices::PauseCountdown(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->countdown.active || impl_->countdown.paused) return Fail(error, "没有运行中的倒计时");
    impl_->countdown_remaining_us = std::max<int64_t>(0, impl_->countdown_deadline_us - esp_timer_get_time());
    if (!impl_->countdown_remaining_us) return Fail(error, "倒计时已到时");
    impl_->countdown.paused = true;
    ++impl_->revision;
    return true;
}

bool WatchServices::ResumeCountdown(std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->countdown.active || !impl_->countdown.paused) return Fail(error, "没有暂停的倒计时");
    impl_->countdown_deadline_us = esp_timer_get_time() + impl_->countdown_remaining_us;
    impl_->countdown.paused = false;
    ++impl_->revision;
    return true;
}

void WatchServices::CancelCountdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->countdown = {};
    ++impl_->revision;
}

void WatchServices::DismissReminders() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->reminders.clear();
    ++impl_->revision;
}

bool WatchServices::SnoozeReminders(uint32_t seconds, std::string* error) {
    if (!seconds || seconds > 3600) return Fail(error, "稍后提醒范围为1秒至1小时");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->reminders.empty()) return Fail(error, "没有正在响铃的提醒");
    // Merge reminders already snoozed so a newly arriving event cannot discard them.
    for (auto reminder : impl_->reminders) {
        if (std::none_of(impl_->snoozed.begin(), impl_->snoozed.end(), [&](const auto& item) { return item.token == reminder.token; }))
            impl_->snoozed.push_back(std::move(reminder));
    }
    impl_->reminders.clear();
    impl_->snooze_deadline_us = esp_timer_get_time() + int64_t(seconds) * kUsPerSecond;
    ++impl_->revision;
    return true;
}

void WatchServices::SetReminderCallback(std::function<void(const std::vector<WatchReminder>&)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

void WatchServices::Tick() {
    std::vector<WatchReminder> due_reminders;
    std::function<void(const std::vector<WatchReminder>&)> callback;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const int64_t monotonic = esp_timer_get_time();
        const int64_t now = time(nullptr);
        auto add_reminder = [&](WatchReminderKind kind, uint32_t id, const std::string& title, bool audible) {
            due_reminders.push_back({impl_->next_token++, kind, id, title, audible});
        };
        if (impl_->countdown.active && !impl_->countdown.paused && monotonic >= impl_->countdown_deadline_us) {
            impl_->countdown = {};
            add_reminder(WatchReminderKind::Countdown, 0, "倒计时结束", true);
        }
        if (!impl_->snoozed.empty() && monotonic >= impl_->snooze_deadline_us) {
            for (auto reminder : impl_->snoozed) {
                reminder.audible = reminder.kind != WatchReminderKind::Event || !impl_->state.settings.do_not_disturb;
                due_reminders.push_back(std::move(reminder));
            }
            impl_->snoozed.clear();
        }
        if (IsTimeValid(now)) {
            auto state = impl_->state;
            bool changed = false;
            for (auto& alarm : state.alarms) {
                if (!alarm.enabled) continue;
                int64_t due = alarm.next_due;
                if (alarm.weekdays) {
                    // Check today's/yesterday's occurrence, including a catch-up just after midnight.
                    due = 0;
                    for (int days_back = 1; days_back >= 0; --days_back) {
                        const int64_t day = DayStart(now) - days_back * kDaySeconds;
                        const int64_t candidate = day + alarm.hour * 3600 + alarm.minute * 60;
                        if ((alarm.weekdays & (1 << WeekdayBit(day))) && candidate <= now) due = candidate;
                    }
                } else if (!due) {
                    due = NextAlarm(alarm, now, true);
                    alarm.next_due = due;
                    changed = true;
                }
                const int64_t fired = std::max(alarm.last_fired, impl_->runtime_fired[alarm.id]);
                if (due && due <= now && due > alarm.last_fired) {
                    alarm.last_fired = due;
                    impl_->runtime_fired[alarm.id] = due;
                    if (!alarm.weekdays) alarm.enabled = false;
                    changed = true;
                    if (due > fired && now - due <= kCatchUpSeconds)
                        add_reminder(WatchReminderKind::Alarm, alarm.id, alarm.title, true);
                }
                if (alarm.weekdays) {
                    const int64_t next = NextAlarm(alarm, now + 1, false);
                    if (next != alarm.next_due) { alarm.next_due = next; changed = true; }
                }
            }
            for (auto& event : state.events) {
                if (event.completed || event.notified || event.due > now) continue;
                event.notified = true;
                changed = true;
                if (event.due > impl_->runtime_fired[event.id]) {
                    impl_->runtime_fired[event.id] = event.due;
                    if (now - event.due <= kCatchUpSeconds)
                        add_reminder(WatchReminderKind::Event, event.id, event.title, !state.settings.do_not_disturb);
                }
            }
            if (changed && monotonic >= impl_->save_retry_after_us) {
                // A failed flash write never prevents an alarm sounding. runtime_fired prevents
                // duplicate rings this boot; the persistent update is retried on a later tick.
                if (!impl_->Save(state, nullptr)) impl_->save_retry_after_us = monotonic + 30 * kUsPerSecond;
                else impl_->save_retry_after_us = 0;
            }
        }
        if (!due_reminders.empty()) {
            for (const auto& reminder : due_reminders) {
                // Replace an unacknowledged older occurrence, then append so the UI's last
                // token changes when the same daily alarm rings again the next morning.
                impl_->reminders.erase(std::remove_if(impl_->reminders.begin(), impl_->reminders.end(), [&](const auto& old) {
                    return old.kind == reminder.kind && old.source_id == reminder.source_id;
                }), impl_->reminders.end());
                if (impl_->reminders.size() >= 32) impl_->reminders.erase(impl_->reminders.begin());
                impl_->reminders.push_back(reminder);
            }
            ++impl_->revision;
            callback = impl_->callback;
        }
    }
    if (callback && !due_reminders.empty()) callback(due_reminders);
}

void WatchServices::RegisterMcpTools() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->tools_registered) return;
    impl_->tools_registered = true;
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.watch.get_status",
        "Read the watch local time validity, saved alarms and calendar events. Weekday bits are Monday=1 through Sunday=64. "
        "The clock already represents local time, do not add a timezone offset. Query before changing existing reminders.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            auto snapshot = Snapshot();
            std::string result = "{\"time_valid\":" + std::string(snapshot.time_valid ? "true" : "false") +
                ",\"local_time\":" + JsonString(snapshot.time_valid ? FormatTime(snapshot.now) : "待校时") + ",\"alarms\":[";
            for (size_t i = 0; i < snapshot.alarms.size(); ++i) {
                const auto& alarm = snapshot.alarms[i];
                if (i) result += ',';
                result += "{\"id\":" + std::to_string(alarm.id) + ",\"hour\":" + std::to_string(alarm.hour) +
                    ",\"minute\":" + std::to_string(alarm.minute) + ",\"weekdays\":" + std::to_string(alarm.weekdays) +
                    ",\"enabled\":" + (alarm.enabled ? "true" : "false") + ",\"title\":" + JsonString(alarm.title) + "}";
            }
            result += "],\"events\":[";
            for (size_t i = 0; i < snapshot.events.size(); ++i) {
                const auto& event = snapshot.events[i];
                if (i) result += ',';
                result += "{\"id\":" + std::to_string(event.id) + ",\"local_time\":" + JsonString(FormatTime(event.due)) +
                    ",\"title\":" + JsonString(event.title) + ",\"completed\":" + (event.completed ? "true" : "false") + "}";
            }
            return result + "],\"countdown_seconds\":" + std::to_string(snapshot.countdown.remaining_seconds) +
                ",\"storage_error\":" + JsonString(snapshot.storage_error) + "}";
        });
    mcp.AddTool("self.watch.save_alarm",
        "Persist a local watch alarm. Ask for missing time/repetition details; never invent them. id=0 creates, otherwise updates. "
        "weekdays=0 means the next occurrence once; bits Monday=1 Tuesday=2 Wednesday=4 Thursday=8 Friday=16 Saturday=32 Sunday=64. "
        "Maximum 8 alarms, title at most 60 UTF-8 bytes. Confirm saved only if this tool succeeds. No power-off alarm.",
        PropertyList({Property("hour", kPropertyTypeInteger, 0, 23), Property("minute", kPropertyTypeInteger, 0, 59),
            Property("weekdays", kPropertyTypeInteger, 0, 0, 127), Property("title", kPropertyTypeString, std::string("闹钟")),
            Property("enabled", kPropertyTypeBoolean, true), Property("id", kPropertyTypeInteger, 0, 0, INT32_MAX)}),
        [this](const PropertyList& props) -> ReturnValue {
            WatchAlarm alarm;
            alarm.id = props["id"].value<int>(); alarm.hour = props["hour"].value<int>();
            alarm.minute = props["minute"].value<int>(); alarm.weekdays = props["weekdays"].value<int>();
            alarm.title = props["title"].value<std::string>(); alarm.enabled = props["enabled"].value<bool>();
            std::string error;
            uint32_t id = 0;
            if (!SaveAlarm(alarm, &id, &error)) throw std::runtime_error(error);
            return std::string("已保存闹钟 id=") + std::to_string(id) + " " + std::to_string(alarm.hour) + ":" +
                (alarm.minute < 10 ? "0" : "") + std::to_string(alarm.minute) + " weekdays=" + std::to_string(alarm.weekdays) +
                (Snapshot().time_valid ? "" : "；当前时间未校准，校时后才能准确提醒");
        });
    mcp.AddTool("self.watch.delete_alarm", "Delete an existing alarm after identifying its id using get_status.",
        PropertyList({Property("id", kPropertyTypeInteger, 1, INT32_MAX)}), [this](const PropertyList& props) -> ReturnValue {
            std::string error;
            if (!DeleteAlarm(props["id"].value<int>(), &error)) throw std::runtime_error(error);
            return true;
        });
    mcp.AddTool("self.watch.save_event",
        "Persist a calendar event in local date/time. Ask the user for missing date, time or title. id=0 creates; existing id updates. "
        "Maximum 16 events, title at most 60 UTF-8 bytes. Confirm saved only after this tool succeeds. completed marks an event done.",
        PropertyList({Property("year", kPropertyTypeInteger, 2024, 2099), Property("month", kPropertyTypeInteger, 1, 12),
            Property("day", kPropertyTypeInteger, 1, 31), Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 59), Property("title", kPropertyTypeString),
            Property("id", kPropertyTypeInteger, 0, 0, INT32_MAX), Property("completed", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& props) -> ReturnValue {
            WatchEvent event;
            if (!MakeLocalTime(props["year"].value<int>(), props["month"].value<int>(), props["day"].value<int>(),
                props["hour"].value<int>(), props["minute"].value<int>(), &event.due)) throw std::runtime_error("日程日期无效");
            event.id = props["id"].value<int>(); event.title = props["title"].value<std::string>();
            event.completed = props["completed"].value<bool>();
            uint32_t id = 0;
            std::string error;
            if (!SaveEvent(event, &id, &error)) throw std::runtime_error(error);
            return std::string("已保存日程 id=") + std::to_string(id) + " " + FormatTime(event.due) + " " + event.title +
                (Snapshot().time_valid ? "" : "；当前时间未校准，校时后才能准确提醒");
        });
    mcp.AddTool("self.watch.delete_event", "Delete a saved calendar event after identifying its id using get_status.",
        PropertyList({Property("id", kPropertyTypeInteger, 1, INT32_MAX)}), [this](const PropertyList& props) -> ReturnValue {
            std::string error;
            if (!DeleteEvent(props["id"].value<int>(), &error)) throw std::runtime_error(error);
            return true;
        });
    mcp.AddTool("self.watch.countdown",
        "Operate a relative countdown that works before clock synchronization. action=start/pause/resume/cancel. "
        "For start require the user's duration in seconds, 1..86400. It continues across pages but does not survive a power loss.",
        PropertyList({Property("action", kPropertyTypeString), Property("seconds", kPropertyTypeInteger, 0, 0, 86400)}),
        [this](const PropertyList& props) -> ReturnValue {
            const auto action = props["action"].value<std::string>();
            std::string error;
            bool result = false;
            if (action == "start") result = StartCountdown(props["seconds"].value<int>(), &error);
            else if (action == "pause") result = PauseCountdown(&error);
            else if (action == "resume") result = ResumeCountdown(&error);
            else if (action == "cancel") { CancelCountdown(); result = true; }
            else error = "未知倒计时操作";
            if (!result) throw std::runtime_error(error);
            return true;
        });
}

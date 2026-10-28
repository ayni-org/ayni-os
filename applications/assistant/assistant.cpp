#include "policy_support.h"
#include <algorithm>
namespace ayni::applications::assistant {
namespace {
constexpr uint32_t parameter_version = 2, header_bytes = 4, id_bytes = 8, bits_per_byte = 8;
constexpr uint32_t version_at = 0, kind_at = 1, source_count_at = 2, origin_at = 3;
constexpr uint32_t minutes_per_day = 1440, seconds_per_minute = 60;
bool valid(const Parameters &p) {
    if (p.text.size() > text_bytes_max || p.sources.size() > memory_sources_max ||
        static_cast<uint32_t>(p.kind) > static_cast<uint32_t>(GoalKind::scene_observe) ||
        static_cast<uint32_t>(p.origin) > static_cast<uint32_t>(Origin::model) ||
        (p.origin == Origin::model) != (p.operation_id != 0) ||
        (p.origin == Origin::model && p.kind == GoalKind::memory_organize)) {
        return false;
    }
    if (p.kind == GoalKind::reminder) {
        return !p.text.empty() && p.sources.empty();
    }
    if (!p.text.empty() || (p.kind == GoalKind::memory_organize) != !p.sources.empty()) {
        return false;
    }
    for (size_t at = 0; at < p.sources.size(); ++at) {
        if (!p.sources[at] ||
            std::find(p.sources.begin(), p.sources.begin() + static_cast<std::ptrdiff_t>(at),
                      p.sources[at]) != p.sources.begin() + static_cast<std::ptrdiff_t>(at)) {
            return false;
        }
    }
    return true;
}
void put_id(Encoded &out, uint64_t id) {
    for (uint32_t byte = 0; byte < id_bytes; ++byte) {
        out.bytes[out.size++] = static_cast<uint8_t>(id >> (byte * bits_per_byte));
    }
}
uint64_t get_id(const uint8_t *data, uint32_t &at) {
    uint64_t id = 0;
    for (uint32_t byte = 0; byte < id_bytes; ++byte) {
        id |= static_cast<uint64_t>(data[at++]) << (byte * bits_per_byte);
    }
    return id;
}
// Exact ASCII case folding only; other bytes, including UTF-8, compare unchanged.
bool same_phrase(const std::string &input, const char *phrase) {
    return std::equal(input.begin(), input.end(), phrase, phrase + std::strlen(phrase),
                      [](char a, char b) {
                          return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
                      });
}
bool strip_suffix(std::string &input, const char *suffix) {
    const auto size = std::strlen(suffix);
    if (input.size() <= size || input.compare(input.size() - size, size, suffix) != 0) {
        return false;
    }
    input.resize(input.size() - size);
    return true;
}
bool strip_prefix(std::string &input, const char *prefix) {
    const auto size = std::strlen(prefix);
    if (input.size() < size || input.compare(0, size, prefix) != 0) {
        return false;
    }
    input.erase(0, size);
    return true;
}
// Whole UTF-8 sequences: trimming a byte set would cut into CJK punctuation such as "。".
constexpr char ideographic_space[] = "\xE3\x80\x80"; // U+3000, typed by CJK input methods.
} // namespace
ayni_status_t detail::objects(const ayni_kernel_t *k, ayni_object_kind_t kind,
                              std::vector<uint64_t> &ids) {
    uint32_t count = 0;
    auto status = ayni_list_objects(k, kind, nullptr, 0, &count);
    if (status != AYNI_OK && status != AYNI_BUFFER_TOO_SMALL) {
        return status;
    }
    ids.resize(count);
    return ayni_list_objects(k, kind, ids.data(), count, &count);
}
const char *goal_type(GoalKind kind) {
    switch (kind) {
    case GoalKind::reminder:
        return "assistant.reminder";
    case GoalKind::device_check:
        return "assistant.device";
    case GoalKind::memory_organize:
        return "assistant.organize";
    case GoalKind::scene_observe:
        return "assistant.observe";
    }
    return "";
}
// Application parameter format 2: version/kind/source-count/origin, the LE operation ID for
// model origin only, LE source IDs, then the UTF-8 body. Format 1 data is refused (ADR-0072).
ayni_status_t encode(const Parameters &p, Encoded &out) {
    if (!valid(p)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Encoded next;
    next.bytes[version_at] = parameter_version;
    next.bytes[kind_at] = static_cast<uint8_t>(p.kind);
    next.bytes[source_count_at] = static_cast<uint8_t>(p.sources.size());
    next.bytes[origin_at] = static_cast<uint8_t>(p.origin);
    next.size = header_bytes;
    if (p.origin == Origin::model) {
        put_id(next, p.operation_id);
    }
    for (const auto id : p.sources) {
        put_id(next, id);
    }
    std::memcpy(next.bytes.data() + next.size, p.text.data(), p.text.size());
    next.size += static_cast<uint32_t>(p.text.size());
    out = next;
    return AYNI_OK;
}
ayni_status_t decode(ayni_bytes_v1 input, Parameters &out) {
    if (!input.data || input.size < header_bytes || input.size > parameter_bytes_max ||
        input.data[version_at] != parameter_version ||
        input.data[origin_at] > static_cast<uint8_t>(Origin::model) ||
        input.data[source_count_at] > memory_sources_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    Parameters next;
    next.kind = static_cast<GoalKind>(input.data[kind_at]);
    next.origin = static_cast<Origin>(input.data[origin_at]);
    const uint32_t ids = input.data[source_count_at] + (next.origin == Origin::model ? 1U : 0U);
    if (input.size < header_bytes + ids * id_bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t at = header_bytes;
    if (next.origin == Origin::model) {
        next.operation_id = get_id(input.data, at);
    }
    for (uint32_t source = 0; source < input.data[source_count_at]; ++source) {
        next.sources.push_back(get_id(input.data, at));
    }
    next.text.assign(reinterpret_cast<const char *>(input.data + at), input.size - at);
    if (!valid(next)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = std::move(next);
    return AYNI_OK;
}
bool stop_request(const std::string &input) {
    auto phrase = input;
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const auto *tail : {" ", "\t", "\r", "\n", ideographic_space, "!", ".", "！", "。"}) {
            stripped = stripped || strip_suffix(phrase, tail);
        }
    }
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const auto *head : {" ", "\t", "\r", "\n", ideographic_space}) {
            stripped = stripped || strip_prefix(phrase, head);
        }
    }
    for (const auto *stop : {"停", "停下", "停止", "stop"}) {
        if (same_phrase(phrase, stop)) {
            return true;
        }
    }
    return false;
}
ayni_status_t QuietHours::validate() const {
    if (!enabled) {
        return AYNI_OK;
    }
    if (start_minute >= minutes_per_day || end_minute >= minutes_per_day ||
        start_minute == end_minute) {
        return AYNI_INVALID_ARGUMENT;
    }
    return detail::check_timezone(detail::text(timezone));
}
ayni_status_t QuietHours::allows(ayni_utc_sample_v1 sample, bool &out) const {
    auto status = validate();
    if (status != AYNI_OK) {
        return status;
    }
    if (!enabled) {
        out = true;
        return AYNI_OK;
    }
    if (sample.struct_size != sizeof(sample) || sample.trust != AYNI_UTC_TRUSTED ||
        sample.utc_s < 0 || sample.utc_s > AYNI_CALENDAR_SECONDS_MAX) {
        out = false;
        return AYNI_OK;
    }
    int64_t local = 0;
    status = ayni_calendar_local_time({timezone.data(), static_cast<uint32_t>(timezone.size())},
                                      sample.utc_s, &local);
    if (status != AYNI_OK) {
        return status;
    }
    const auto minute = static_cast<uint32_t>((local / seconds_per_minute) % minutes_per_day);
    const bool quiet = start_minute < end_minute ? minute >= start_minute && minute < end_minute
                                                 : minute >= start_minute || minute < end_minute;
    out = !quiet;
    return AYNI_OK;
}
Presentation presentation(const ayni_call_info_v1 &call) {
    if (call.phase == AYNI_CALL_RESULT_UNKNOWN || call.outcome == AYNI_CALL_ABANDONED ||
        (call.phase == AYNI_CALL_FINISHED && call.effect == AYNI_CALL_EFFECT_UNKNOWN)) {
        return Presentation::unknown;
    }
    if (call.phase == AYNI_CALL_FINISHED) {
        return call.outcome == AYNI_CALL_COMPLETED && call.status == AYNI_OK &&
                       call.effect == AYNI_CALL_EFFECT_COMPLETE
                   ? Presentation::presented
                   : Presentation::failed;
    }
    return call.phase == AYNI_CALL_QUEUED ? Presentation::available : Presentation::presenting;
}
ayni_status_t project(const ayni_kernel_t *kernel, Activity &out) {
    Activity activity;
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    auto status = ayni_get_kernel_info(kernel, &info);
    if (status != AYNI_OK) {
        return status;
    }
    if (info.lifecycle != AYNI_LIFECYCLE_RUNNING) {
        activity.primary =
            info.lifecycle == AYNI_LIFECYCLE_RESTRICTED ? State::needs_attention : State::offline;
        out = activity;
        return AYNI_OK;
    }
    ayni_pause_group_info_v1 group{};
    group.struct_size = sizeof(group);
    status = ayni_get_pause_group(kernel, autonomous_group, &group);
    if (status != AYNI_OK) {
        return status;
    }
    State result = group.paused ? State::paused : State::available;
    activity.autonomy_paused = group.paused != 0;
    std::vector<uint64_t> ids;
    status = detail::objects(kernel, AYNI_OBJECT_GOAL, ids);
    if (status != AYNI_OK) {
        return status;
    }
    for (const auto id : ids) {
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        status = ayni_get_goal(kernel, id, &goal);
        if (status != AYNI_OK) {
            return status;
        }
        if (goal.lifecycle == AYNI_GOAL_PROPOSED) {
            result = State::awaiting_authority;
            ++activity.awaiting_authority;
        }
    }
    status = detail::objects(kernel, AYNI_OBJECT_TASK, ids);
    if (status != AYNI_OK) {
        return status;
    }
    for (const auto id : ids) {
        ayni_task_info_v1 task{};
        task.struct_size = sizeof(task);
        status = ayni_get_task(kernel, id, &task);
        if (status != AYNI_OK) {
            return status;
        }
        if (task.lifecycle == AYNI_TASK_PAUSED && task.hold_status != AYNI_OK) {
            result = State::needs_attention;
            ++activity.needs_attention;
            continue;
        }
        if (task.lifecycle == AYNI_TASK_READY || task.lifecycle == AYNI_TASK_RUNNING) {
            ++activity.processing;
            if (result == State::available || result == State::observing) {
                result = State::processing;
            }
        } else if (task.lifecycle == AYNI_TASK_WAITING) {
            ++activity.observing;
            if (result == State::available) {
                result = State::observing;
            }
        }
    }
    activity.primary = result;
    out = activity;
    return AYNI_OK;
}
} // namespace ayni::applications::assistant

#include <algorithm>
#include <ayni/applications/assistant.h>
#include <cstring>
namespace ayni::applications::assistant {
namespace {
constexpr uint32_t parameter_version = 1, header_bytes = 4, id_bytes = 8, bits_per_byte = 8;
constexpr uint32_t version_at = 0, kind_at = 1, source_count_at = 2, reserved_at = 3;
constexpr uint32_t minutes_per_day = 1440, seconds_per_minute = 60;
bool valid(const Parameters &p) {
    if (p.text.size() > text_bytes_max || p.sources.size() > memory_sources_max ||
        static_cast<uint32_t>(p.kind) > static_cast<uint32_t>(GoalKind::scene_observe)) {
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
ayni_status_t objects(const ayni_kernel_t *k, ayni_object_kind_t kind, std::vector<uint64_t> &ids) {
    uint32_t count = 0;
    auto status = ayni_list_objects(k, kind, nullptr, 0, &count);
    if (status != AYNI_OK && status != AYNI_BUFFER_TOO_SMALL) {
        return status;
    }
    ids.resize(count);
    return ayni_list_objects(k, kind, ids.data(), count, &count);
}
} // namespace
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
// Application parameter format 1: version/kind/source-count/reserved, LE IDs, UTF-8 body.
ayni_status_t encode(const Parameters &p, Encoded &out) {
    if (!valid(p)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Encoded next;
    next.bytes[version_at] = parameter_version;
    next.bytes[kind_at] = static_cast<uint8_t>(p.kind);
    next.bytes[source_count_at] = static_cast<uint8_t>(p.sources.size());
    next.size = header_bytes;
    for (const auto id : p.sources) {
        for (uint32_t byte = 0; byte < id_bytes; ++byte) {
            next.bytes[next.size++] = static_cast<uint8_t>(id >> (byte * bits_per_byte));
        }
    }
    std::memcpy(next.bytes.data() + next.size, p.text.data(), p.text.size());
    next.size += static_cast<uint32_t>(p.text.size());
    out = next;
    return AYNI_OK;
}
ayni_status_t decode(ayni_bytes_v1 input, Parameters &out) {
    if (!input.data || input.size < header_bytes || input.size > parameter_bytes_max ||
        input.data[version_at] != parameter_version || input.data[reserved_at] != 0 ||
        input.data[source_count_at] > memory_sources_max ||
        input.size < header_bytes + input.data[source_count_at] * id_bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    Parameters next;
    next.kind = static_cast<GoalKind>(input.data[kind_at]);
    uint32_t at = header_bytes;
    for (uint32_t source = 0; source < input.data[source_count_at]; ++source) {
        uint64_t id = 0;
        for (uint32_t byte = 0; byte < id_bytes; ++byte) {
            id |= static_cast<uint64_t>(input.data[at++]) << (byte * bits_per_byte);
        }
        next.sources.push_back(id);
    }
    next.text.assign(reinterpret_cast<const char *>(input.data + at), input.size - at);
    if (!valid(next)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = std::move(next);
    return AYNI_OK;
}
uint64_t route(const std::string &input, uint64_t a, uint64_t b, std::string &body) {
    body = input;
    if (input.size() > 3 && input[0] == '@' && input[2] == ' ' &&
        (input[1] == 'a' || input[1] == 'b')) {
        body = input.substr(3);
        return input[1] == 'a' ? a : b;
    }
    return input == "check" || input == "检查" ? a : b;
}
ayni_status_t QuietHours::validate() const {
    if (!enabled) {
        return AYNI_OK;
    }
    if (start_minute >= minutes_per_day || end_minute >= minutes_per_day ||
        start_minute == end_minute) {
        return AYNI_INVALID_ARGUMENT;
    }
    int64_t local = 0;
    // Mid-range instant validates the zone without rejecting a legitimate west offset at epoch 0.
    constexpr int64_t validation_utc_s = 86400;
    return ayni_calendar_local_time({timezone.data(), static_cast<uint32_t>(timezone.size())},
                                    validation_utc_s, &local);
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
    status = objects(kernel, AYNI_OBJECT_GOAL, ids);
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
    status = objects(kernel, AYNI_OBJECT_TASK, ids);
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
std::vector<Suggestion> merge_suggestions(const std::vector<Suggestion> &input) {
    std::vector<Suggestion> out;
    for (const auto &item : input) {
        auto same = std::find_if(out.begin(), out.end(), [&](const auto &row) {
            return row.key == item.key && row.value == item.value;
        });
        if (same != out.end()) {
            same->agents |= item.agents;
            continue;
        }
        out.push_back(item);
    }
    for (auto &item : out) {
        item.conflict = std::any_of(out.begin(), out.end(), [&](const auto &other) {
            return item.key == other.key && item.value != other.value;
        });
    }
    return out;
}
} // namespace ayni::applications::assistant

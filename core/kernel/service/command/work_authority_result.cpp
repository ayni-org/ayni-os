#include "kernel/service/command/work_authority_result.h"

#include "authority/grant/grant_table.h"
#include "durability/batch/record_body.h"
#include "foundation/kernel_id.h"

#include <cstring>

namespace ayni::kernel {

WorkAuthorityResult copy_work_authority_result(const work::GoalAuthorityRecord &record) {
    WorkAuthorityResult result{};
    result.transition_id = record.stamp.transition_id;
    result.binding = record.binding;
    result.count = record.count;
    for (uint32_t at = 0; at < result.count; ++at) {
        result.references[at] = record.requirements[at].dependency;
    }
    return result;
}

bool work_authority_result_matches(const work::GoalAuthorityRecord &record, ayni_bytes_v1 bytes) {
    const auto result = copy_work_authority_result(record);
    const auto size = work_authority_result_size(result.count);
    if (bytes.data == nullptr || bytes.size != size) {
        return false;
    }
    std::array<uint8_t, work_authority_result_size(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX)> expected{};
    write_work_authority_result(result, expected.data());
    return std::memcmp(expected.data(), bytes.data, size) == 0;
}

// Compare a validated result with the current generation. Pauses may advance
// its stamp, but cannot change the original choices or their establishment time.
bool retains_work_authority_result(const work::GoalAuthorityRecord &record,
                                   const WorkAuthorityResult &result) {
    if (record.binding.established_seq < result.binding.established_seq ||
        (record.stamp.commit_seq == result.binding.established_seq
             ? result.transition_id != record.stamp.transition_id
             : result.transition_id >= record.stamp.transition_id)) {
        return false;
    }
    if (record.binding.established_seq > result.binding.established_seq) {
        return true; // Only a later explicit binding may have different choices (ADR-0038).
    }
    if (record.count != result.count ||
        record.binding.parent.goal_id != result.binding.parent.goal_id ||
        record.binding.parent.commit_seq != result.binding.parent.commit_seq ||
        record.binding.input.incarnation != result.binding.input.incarnation ||
        record.binding.input.through != result.binding.input.through) {
        return false;
    }
    for (uint32_t at = 0; at < result.count; ++at) {
        const auto &selected = record.requirements[at].dependency;
        if (selected.grant_id != result.references[at].grant_id ||
            selected.revision != result.references[at].revision) {
            return false;
        }
    }
    return true;
}

bool work_authority_result_within(const WorkAuthorityResult &result, uint64_t transition_limit,
                                  uint64_t boot_epoch_limit) {
    if (result.transition_id > transition_limit ||
        result.binding.input.incarnation > boot_epoch_limit ||
        foundation::id_boot_epoch(result.binding.parent.goal_id) > boot_epoch_limit) {
        return false;
    }
    for (uint32_t at = 0; at < result.count; ++at) {
        if (foundation::id_boot_epoch(result.references[at].grant_id) > boot_epoch_limit) {
            return false;
        }
    }
    return true;
}

bool valid_work_authority_result(const WorkAuthorityResult &result, uint64_t goal_id) {
    if (result.count > result.references.size() ||
        !work::valid_goal_binding(result.binding, goal_id, result.binding.parent.goal_id,
                                  {result.transition_id, result.binding.established_seq})) {
        return false;
    }
    for (uint32_t at = 0; at < result.count; ++at) {
        if (!work::valid_selected_authority_reference(result.references[at]) ||
            result.references[at].revision != authority::grant_active_revision) {
            return false;
        }
    }
    return true;
}

void write_work_authority_result(const WorkAuthorityResult &result, uint8_t *body) {
    durability::BodyWriter writer(body, work_authority_result_size(result.count));
    writer.u64(result.transition_id);
    writer.u64(result.binding.established_seq);
    writer.u64(result.binding.parent.goal_id);
    writer.u64(result.binding.parent.commit_seq);
    writer.u64(result.binding.input.incarnation);
    writer.u64(result.binding.input.through);
    writer.u32(result.count);
    for (uint32_t at = 0; at < result.count; ++at) {
        writer.u64(result.references[at].grant_id);
        writer.u32(result.references[at].revision);
    }
}

// Decode the configured result independently of current work/catalog state;
// later reauthorization cannot reinterpret an earlier command's evidence.
bool read_work_authority_result(ayni_bytes_v1 bytes, uint64_t goal_id, WorkAuthorityResult &out) {
    if (bytes.data == nullptr || bytes.size < work_authority_result_fixed_size ||
        bytes.size > work_authority_result_size(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX)) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    WorkAuthorityResult result{};
    result.transition_id = reader.u64();
    result.binding.established_seq = reader.u64();
    result.binding.parent.goal_id = reader.u64();
    result.binding.parent.commit_seq = reader.u64();
    result.binding.input.incarnation = reader.u64();
    result.binding.input.through = reader.u64();
    result.count = reader.u32();
    if (result.count > result.references.size()) {
        return false;
    }
    for (uint32_t at = 0; at < result.count; ++at) {
        result.references[at] = {reader.u64(), reader.u32()};
    }
    if (!reader.ok() || !valid_work_authority_result(result, goal_id)) {
        return false;
    }
    out = result;
    return true;
}

} // namespace ayni::kernel

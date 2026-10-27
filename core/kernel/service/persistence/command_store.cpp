// Exact owner command storage, retry-window order and retained snapshot proof (ADR-0059).
// Journal partner verification remains in WorkReplay; no live Session is reconstructed.
#include "authority/grant/grant_records.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/service/command/command_records.h"
#include "kernel/service/memory/memory_permissions.h"
#include "kernel/service/model/store_limits.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
namespace ayni::kernel {
namespace {
const StoreKind kinds[] = {{command_record_type, exact_command_record_version}};
// records.md COMMAND_MAPPING snapshot suffix: original commit_seq, little endian.
constexpr uint32_t command_tail_size = sizeof(uint64_t);
bool knows(uint16_t, uint16_t version, WorkRecordContext context) {
    return knows_command_record(version) &&
           (version != expired_command_record_version || context == WorkRecordContext::snapshot);
}
uint32_t capacity(const KernelCapacities &c) {
    return authority::PrincipalRegistry::capacity * c.command_capacity;
}
uint32_t record_max(const KernelCapacities &c) {
    return command_record_max(command_limits(c).canonical_max_bytes, c.memory_dependency_capacity) +
           command_tail_size;
}
uint32_t journal_max(const KernelCapacities &capacities) {
    return record_bytes(command_record_max(command_limits(capacities).canonical_max_bytes,
                                           capacities.memory_dependency_capacity));
}
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    const auto *mapping = model.commands.mapped_in(stamp.commit_seq);
    if (mapping == nullptr) {
        return true;
    }
    const auto size = command_record_size_for(*mapping);
    auto *at = writer.add_record_body({command_record_type, command_record_version_for(*mapping),
                                       durability::record_required, size});
    return at != nullptr &&
           write_command_record(*mapping, model.commands.floor(mapping->principal_id),
                                model.commands.canonical(*mapping), at, size,
                                model.commands.authorization_result(*mapping));
}
// The table keeps no order; preserve original acceptance order and eviction floors (ADR-0027).
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    bool found = false;
    uint64_t first = UINT64_MAX;
    for (uint32_t index = 0; index < model.commands.size(); ++index) {
        const auto &mapping = model.commands.at(index);
        if (mapping.commit_seq > cursor.order && mapping.commit_seq < first) {
            first = mapping.commit_seq;
            cursor.index = index;
            found = true;
        }
    }
    return found;
}
StoreRecord snapshot_record(const WorkModel &model, const StoreCursor &cursor) {
    const auto index = cursor.index;
    const auto &mapping = model.commands.at(index);
    return {command_record_version_for(mapping),
            command_record_size_for(mapping) + command_tail_size};
}
void snapshot_write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    const auto &mapping = model.commands.at(index);
    const auto base_size = command_record_size_for(mapping);
    static_cast<void>(write_command_record(mapping, model.commands.floor(mapping.principal_id),
                                           model.commands.canonical(mapping), body, base_size,
                                           model.commands.authorization_result(mapping)));
    foundation::store_le64(body + base_size, mapping.commit_seq);
}
void snapshot_advance(const WorkModel &model, StoreCursor &cursor) {
    cursor.order = model.commands.at(cursor.index).commit_seq;
}
bool matches_retained_revocation(const CommandMapping &mapping, const authority::Grant &grant) {
    if (mapping.effect == AYNI_INTENT_EFFECT_TARGET_GONE ||
        grant.created_stamp.commit_seq >= mapping.commit_seq ||
        grant.stamp.commit_seq > mapping.commit_seq || grant.revision != mapping.object.revision) {
        return false;
    }
    if (mapping.effect == AYNI_INTENT_EFFECT_CHANGED) {
        // Cleanup changes its own stamp only. A retained explicit revoke
        // result therefore still identifies this target's terminal fact.
        return grant.lifecycle == AYNI_GRANT_REVOKED &&
               authority::is_revocation_root(grant.grant_id, grant.revoked_by_root_id) &&
               grant.stamp.commit_seq == mapping.commit_seq;
    }
    return mapping.effect == AYNI_INTENT_EFFECT_NO_CHANGE &&
           grant.stamp.commit_seq < mapping.commit_seq && authority::terminal(grant);
}
// Snapshot compaction removes the original state/command adjacency. Recheck
// its durable owner and every fact still provable from a retained Task (ADR-0043).
bool matches_task_limit_command(const WorkModel &model,
                                const durability::SnapshotMetadata &metadata,
                                const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    TaskLimitCommand command{};
    const auto *binding = model.principals.binding();
    if (!read_task_limit_command(canonical, command) || binding == nullptr ||
        mapping.principal_id != binding->principal_id ||
        mapping.commit_seq <= binding->stamp.commit_seq ||
        !foundation::is_kernel_id(mapping.object.id) ||
        foundation::id_boot_epoch(mapping.object.id) > metadata.boot_epoch) {
        return false;
    }
    uint32_t index = 0;
    if (!model.tasks.find(mapping.object.id, index)) {
        // A terminal Task may have given way; its exact retry window still survives.
        return true;
    }
    const auto &task = model.tasks.at(index);
    if (task.commit_seq < mapping.commit_seq || task.revision < mapping.object.revision ||
        task.model_calls.limit < command.limit) {
        return false;
    }
    return task.commit_seq != mapping.commit_seq ||
           (task.revision == mapping.object.revision && task.model_calls.limit == command.limit &&
            work::TaskTable::is_live(task.lifecycle) && task.model_calls.used < command.limit &&
            !task.model_calls.blocked);
}
// A retained retry result may precede current state; at the same commit it
// must describe exactly that owner's transition, not merely a monotone bound.
bool matches_target_command(const WorkModel &model, const durability::SnapshotMetadata &metadata,
                            const CommandMapping &mapping) {
    const auto *binding = model.principals.binding();
    if (binding == nullptr || mapping.principal_id != binding->principal_id ||
        mapping.commit_seq <= binding->stamp.commit_seq ||
        !foundation::is_kernel_id(mapping.object.id) ||
        foundation::id_boot_epoch(mapping.object.id) > metadata.boot_epoch) {
        return false;
    }
    uint32_t index = 0;
    if (mapping.kind == AYNI_INTENT_CALL_QUERY_RESULT) {
        if (!model.calls.find(mapping.object.id, index)) {
            return true; // A resolved call can give way while its command receipt remains.
        }
        const auto &state = model.calls.state(index);
        if (model.calls.query_revision(index) < mapping.object.revision ||
            state.latest.transition.commit_seq < mapping.commit_seq ||
            state.created.transition.commit_seq >= mapping.commit_seq) {
            return false;
        }
        return state.latest.transition.commit_seq != mapping.commit_seq ||
               (model.calls.query_revision(index) == mapping.object.revision && state.query &&
                state.query->phase == work::CallQuery::Phase::requested &&
                state.query->requested.transition.commit_seq == mapping.commit_seq);
    }
    if (!model.tasks.find(mapping.object.id, index)) {
        // Terminal Tasks may give way while their original command receipts remain.
        return true;
    }
    const auto &task = model.tasks.at(index);
    if (task.commit_seq < mapping.commit_seq || task.revision < mapping.object.revision) {
        return false;
    }
    // A live selected call was necessarily accepted before the owner could resume its wait.
    if (task.commit_seq != mapping.commit_seq) {
        return true;
    }
    uint32_t call_index = 0;
    return task.revision == mapping.object.revision && work::TaskTable::is_live(task.lifecycle) &&
           task.call.end == work::TaskCall::End::none && task.hold_status == AYNI_OK &&
           task.hold_reason == AYNI_REASON_NONE && model.calls.find(task.call.id, call_index) &&
           model.calls.state(call_index).created.transition.commit_seq < mapping.commit_seq;
}
// Retired Grants leave exact receipts intact; retained Grants must still prove their first result.
bool matches_grant_command(const WorkModel &model, const durability::SnapshotMetadata &metadata,
                           const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    if (mapping.kind == AYNI_INTENT_GRANT_CREATE || mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        const auto *binding = model.principals.binding();
        if (binding == nullptr || mapping.principal_id != binding->principal_id ||
            mapping.commit_seq <= binding->stamp.commit_seq ||
            !foundation::is_kernel_id(mapping.object.id) ||
            foundation::id_boot_epoch(mapping.object.id) > metadata.boot_epoch) {
            return false;
        }
        uint32_t index = 0;
        if (model.grants.find(mapping.object.id, index)) {
            const auto &retained = model.grants.at(index);
            const bool matches =
                mapping.kind == AYNI_INTENT_GRANT_CREATE
                    ? WorkReplay::matches_grant_creation(mapping, canonical, retained,
                                                         model.grants.source_domain(index),
                                                         model.grants.scope(index))
                    : matches_retained_revocation(mapping, retained);
            if (!matches) {
                return false;
            }
        }
    }
    return true;
}
// A destroyed canonical request cannot be re-proven, so the explicit expiry format requires
// a later durable privacy fact and the same historical management owner.
durability::BatchCheck restore_expired(WorkModel &model,
                                       const durability::SnapshotMetadata &metadata,
                                       const CommandMapping &mapping, uint64_t floor) {
    // Canonical proof has deliberately been destroyed. Only an authenticated historical
    // owner identity covered by a later retained privacy pass may use this explicit format.
    const auto *owner = model.principals.binding();
    if (owner == nullptr || owner->principal_id != mapping.principal_id ||
        mapping.commit_seq <= owner->stamp.commit_seq ||
        mapping.commit_seq >= model.privacy.record().stamp.commit_seq ||
        !foundation::is_kernel_id(mapping.object.id) ||
        foundation::id_boot_epoch(mapping.object.id) > metadata.boot_epoch) {
        return durability::BatchCheck::corrupt;
    }
    model.release_command_sources(mapping.principal_id, mapping.commit_seq);
    return snapshot_restore_check(model.commands.restore_checked(mapping, floor));
}
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t version, const uint8_t *body, uint32_t size, uint16_t,
                                     StoreReadCursor &) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    ayni_bytes_v1 canonical{};
    ayni_bytes_v1 result_bytes{};
    if (size < command_tail_size ||
        !read_command_record(version, body, size - command_tail_size, mapping, floor, canonical,
                             &result_bytes)) {
        return durability::BatchCheck::corrupt;
    }
    mapping.commit_seq = foundation::load_le64(body + size - command_tail_size);
    if (mapping.commit_seq <= model.commands.last_added_seq() ||
        mapping.commit_seq > metadata.covers_commit_seq) {
        return durability::BatchCheck::corrupt;
    }
    if (!WorkReplay(model).matches_command_sources(mapping, metadata.boot_epoch)) {
        return durability::BatchCheck::corrupt;
    }
    if (mapping.sources.count > model.memories.limits().dependencies) {
        return durability::BatchCheck::no_room;
    }
    if (mapping.privacy_expired) {
        return restore_expired(model, metadata, mapping, floor);
    }
    if (!matches_grant_command(model, metadata, mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (is_work_authority_command(mapping.kind) &&
        (foundation::id_boot_epoch(mapping.object.id) > metadata.boot_epoch ||
         !(mapping.kind == AYNI_INTENT_GOAL_APPROVE
               ? WorkReplay(model).matches_goal_approval(mapping, canonical, result_bytes,
                                                         metadata.max_transition_id,
                                                         metadata.boot_epoch)
               : WorkReplay(model).matches_goal_reauthorization(mapping, canonical, result_bytes,
                                                                metadata.max_transition_id,
                                                                metadata.boot_epoch)))) {
        return durability::BatchCheck::corrupt;
    }
    if (mapping.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT &&
        !matches_task_limit_command(model, metadata, mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (is_memory_command(mapping.kind) &&
        !WorkReplay(model).matches_memory_command(mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (is_context_command(mapping.kind) &&
        !WorkReplay(model).matches_context_command(mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (mapping.kind == AYNI_INTENT_PAUSE_GROUP_SET &&
        !WorkReplay(model).matches_pause_group_command(mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED &&
        !WorkReplay(model).matches_scene_command(mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (is_target_command(mapping.kind) && !matches_target_command(model, metadata, mapping)) {
        return durability::BatchCheck::corrupt;
    }
    model.release_command_sources(mapping.principal_id, mapping.commit_seq);
    return snapshot_restore_check(
        model.commands.restore_checked(mapping, floor, canonical, result_bytes));
}
} // namespace
const PersistentStore command_store{command_record_type,
                                    kinds,
                                    1,
                                    knows,
                                    StorePosition::after_cleanup,
                                    capacity,
                                    record_max,
                                    journal_max,
                                    journal,
                                    snapshot_next,
                                    snapshot_record,
                                    snapshot_write,
                                    snapshot_advance,
                                    snapshot_read};
// Complete source closure is stored with the receipt because its original Goal may give way.
// Live permission is intentionally not required to restore historical content pending cleanup.
bool WorkReplay::matches_command_sources(const CommandMapping &mapping,
                                         uint64_t boot_epoch_limit) const {
    return valid_command_sources(mapping) &&
           memory_dependencies_history(model_, mapping.sources, boot_epoch_limit);
}
} // namespace ayni::kernel

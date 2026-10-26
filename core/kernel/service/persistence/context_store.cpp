// Context records pair their exact owner commands; cleanup effects use the privacy record instead.
#include "foundation/kernel_id.h"
#include "kernel/service/memory/context_service.h"
#include "kernel/service/memory/memory_permissions.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include "knowledge/context/context_records.h"
namespace ayni::kernel {
namespace {
using knowledge::ContextAction;
using knowledge::ContextRecord;
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == knowledge::context_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    return context_capacity(c);
}
uint32_t record_max(const KernelCapacities &c) {
    return knowledge::context_record_size(c.memory_dependency_capacity);
}
uint32_t journal_max(const KernelCapacities &c) {
    return capacity(c) == 0 ? 0 : record_bytes(record_max(c));
}
const StoreKind kinds[] = {{knowledge::context_record_type, always_required}};
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    // Privacy progress is the sole journal effect for derived-source invalidation; emitting
    // a management record as well would require an owner command that never occurred.
    if (model.privacy.record().stamp.commit_seq == stamp.commit_seq &&
        model.privacy.record().effect == work::PrivacyEffect::context) {
        return true;
    }
    uint32_t count = 0;
    for (uint32_t index = 0; index < model.contexts.size(); ++index) {
        const auto &record = model.contexts.at(index);
        if (record.stamp.commit_seq != stamp.commit_seq) {
            continue;
        }
        if (++count > 1) {
            return false;
        }
        auto *body =
            writer.add_record_body({knowledge::context_record_type,
                                    knowledge::context_record_version, durability::record_required,
                                    knowledge::context_record_size(record.dependencies.count)});
        if (body == nullptr) {
            return false;
        }
        knowledge::write_context_record(record, body);
    }
    return true;
}
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    return cursor.index < model.contexts.size();
}
StoreRecord snapshot_record(const WorkModel &model, uint32_t index) {
    return {knowledge::context_record_version,
            knowledge::context_record_size(model.contexts.at(index).dependencies.count)};
}
void snapshot_write(const WorkModel &model, uint32_t index, uint8_t *body) {
    knowledge::write_context_record(model.contexts.at(index), body);
}
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size) {
    // Rows are decoded before cross-store consistency, but duplicate anchors and absent owners
    // can already be rejected without depending on later memories or provenance footers.
    ContextRecord record{};
    uint32_t duplicate = 0;
    if (!knowledge::read_context_record(body, size, record) ||
        !within_snapshot(record.stamp, metadata) ||
        foundation::id_boot_epoch(record.id) > metadata.boot_epoch ||
        model.contexts.find(record.id, duplicate) ||
        model.agents.find_instance(record.owner_instance_id) == nullptr) {
        return durability::BatchCheck::corrupt;
    }
    const auto result = model.contexts.restore(record, model.memories.limits().dependencies);
    return result == AYNI_OK                  ? durability::BatchCheck::ok
           : result == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                              : durability::BatchCheck::corrupt;
}
// Recover the one possible management operation from the exact before/after facts. No implicit
// adoption of old file text is allowed; publication proves its original completed Task lineage.
bool transition_command(const WorkModel &model, const ContextRecord &next, ContextCommand &out) {
    uint32_t index = 0;
    if (!model.contexts.find(next.id, index)) {
        if (next.revision != 1 || next.generation != 0 || next.last_turn != 0 ||
            next.pending.action != ContextAction::none || next.dependencies.count != 0) {
            return false;
        }
        out.kind = AYNI_INTENT_CONTEXT_OPEN;
        out.owner_instance_id = next.owner_instance_id;
        return true;
    }
    const auto &old = model.contexts.at(index);
    if (old.owner_instance_id != next.owner_instance_id) {
        return false;
    }
    out.context_id = old.id;
    out.generation = old.generation;
    out.expected_revision = old.revision;
    // An acknowledgement changes only the stamp and pending action, never source history.
    if (next.pending.action == ContextAction::none) {
        if (old.pending.action == ContextAction::none || next.revision != old.revision ||
            next.generation != old.generation || next.last_turn != old.last_turn ||
            !knowledge::same_dependencies(next.dependencies, old.dependencies)) {
            return false;
        }
        out.kind = AYNI_INTENT_CONTEXT_ACK;
        return true;
    }
    if (old.generation == UINT64_MAX || next.generation != old.generation + 1 ||
        old.pending.action > ContextAction::publish) {
        return false;
    }
    // A new file generation is justified by one original completed Task; arbitrary owner text
    // cannot acquire trusted source metadata through a publish request.
    if (next.pending.action == ContextAction::publish) {
        uint32_t task = 0;
        knowledge::MemoryDependencies sources{};
        if (old.pending.action != ContextAction::none || next.revision != old.revision ||
            old.last_turn == UINT64_MAX || next.last_turn != old.last_turn + 1 ||
            !model.tasks.find(next.pending.task_id, task) ||
            model.tasks.at(task).commit_seq >= next.stamp.commit_seq ||
            !task_context_sources(model, task, old, sources) ||
            !knowledge::same_dependencies(next.dependencies, sources)) {
            return false;
        }
        out.kind = AYNI_INTENT_CONTEXT_PUBLISH;
        out.task_id = next.pending.task_id;
        out.turn_id = next.last_turn;
        return true;
    }
    if (old.revision == UINT32_MAX || next.revision != old.revision + 1 ||
        next.last_turn != old.last_turn) {
        return false;
    }
    out.kind = AYNI_INTENT_CONTEXT_FORGET;
    out.action = next.pending.action;
    out.turn_id = next.pending.turn_id;
    return true;
}
bool same_command(const ContextCommand &a, const ContextCommand &b) {
    return a.kind == b.kind && a.expected_revision == b.expected_revision &&
           a.owner_instance_id == b.owner_instance_id && a.context_id == b.context_id &&
           a.generation == b.generation && a.task_id == b.task_id && a.turn_id == b.turn_id &&
           a.action == b.action;
}
} // namespace
const PersistentStore context_store{knowledge::context_record_type,
                                    kinds,
                                    1,
                                    knows,
                                    StorePosition::after_work,
                                    capacity,
                                    record_max,
                                    journal_max,
                                    journal,
                                    snapshot_next,
                                    snapshot_record,
                                    snapshot_write,
                                    advance_store_index,
                                    snapshot_read};

durability::RecordUse WorkReplay::restore_context(const uint8_t *body, uint32_t size,
                                                  foundation::TransitionStamp stamp,
                                                  uint64_t boot_epoch) {
    using durability::RecordUse;
    ContextRecord record{};
    ContextCommand expected{};
    const auto *owner = model_.principals.binding();
    if (batch_seq_ == stamp.commit_seq || !pairing_.complete() || owner == nullptr ||
        owner->stamp.commit_seq >= stamp.commit_seq ||
        !knowledge::read_context_record(body, size, record) ||
        record.stamp.commit_seq != stamp.commit_seq ||
        record.stamp.transition_id != stamp.transition_id ||
        foundation::id_boot_epoch(record.id) > boot_epoch ||
        model_.agents.find_instance(record.owner_instance_id) == nullptr ||
        !transition_command(model_, record, expected)) {
        return RecordUse::corrupt;
    }
    if (expected.kind == AYNI_INTENT_CONTEXT_OPEN &&
        foundation::id_boot_epoch(record.id) != boot_epoch) {
        return RecordUse::corrupt;
    }
    uint32_t index = 0;
    knowledge::MemoryDependencies released{};
    if (model_.contexts.find(record.id, index)) {
        released = model_.contexts.at(index).dependencies;
    }
    const auto status = model_.contexts.restore(record, model_.memories.limits().dependencies);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? RecordUse::no_room : RecordUse::corrupt;
    }
    model_.release_content_references(released, stamp.commit_seq);
    pairing_.context = BatchPairing::ContextPair{record.id, expected};
    return RecordUse::applied;
}
bool WorkReplay::pair_context_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    ContextCommand command{};
    if (!pairing_.context || mapping.object.id != pairing_.context->object_id ||
        !read_context_command(canonical, command) ||
        !same_command(command, pairing_.context->expected) ||
        !matches_context_command(mapping, canonical)) {
        return false;
    }
    pairing_.context->command_seen = true;
    return true;
}
// Exact receipts survive later file changes, but never retarget another owner or a future version.
bool WorkReplay::matches_context_command(const CommandMapping &mapping,
                                         ayni_bytes_v1 canonical) const {
    ContextCommand command{};
    uint32_t index = 0;
    const auto *owner = model_.principals.binding();
    if (!read_context_command(canonical, command) || owner == nullptr ||
        mapping.principal_id != owner->principal_id ||
        mapping.commit_seq <= owner->stamp.commit_seq ||
        !model_.contexts.find(mapping.object.id, index)) {
        return false;
    }
    const auto &row = model_.contexts.at(index);
    if (mapping.object.revision > row.revision || command.generation > row.generation ||
        row.stamp.commit_seq < mapping.commit_seq ||
        (command.kind == AYNI_INTENT_CONTEXT_OPEN &&
         command.owner_instance_id != row.owner_instance_id)) {
        return false;
    }
    // Later changes keep the original retry receipt; exact equality applies only at its commit.
    if (row.stamp.commit_seq != mapping.commit_seq) {
        return true;
    }
    switch (command.kind) {
    case AYNI_INTENT_CONTEXT_OPEN:
        return row.revision == 1 && row.generation == 0 && row.last_turn == 0 &&
               row.pending.action == ContextAction::none;
    case AYNI_INTENT_CONTEXT_PUBLISH:
        return row.revision == command.expected_revision &&
               row.generation == command.generation + 1 &&
               row.pending.action == ContextAction::publish &&
               row.pending.task_id == command.task_id && row.last_turn == command.turn_id;
    case AYNI_INTENT_CONTEXT_FORGET:
        return row.revision == command.expected_revision + 1 &&
               row.generation == command.generation + 1 && row.pending.action == command.action &&
               row.pending.turn_id == command.turn_id;
    case AYNI_INTENT_CONTEXT_ACK:
        return row.revision == command.expected_revision && row.generation == command.generation &&
               row.pending.action == ContextAction::none;
    default:
        return false;
    }
}
} // namespace ayni::kernel

namespace ayni::kernel {
// Stable anchors are never reclaimed. Snapshot and journal publication share these identity,
// source-history and pending-Task checks, independent of the current external file contents.
bool WorkReplay::contexts_consistent(uint64_t boot_epoch_limit) const {
    const auto *owner = model_.principals.binding();
    for (uint32_t index = 0; index < model_.contexts.size(); ++index) {
        const auto &row = model_.contexts.at(index);
        uint32_t other = 0;
        if (owner == nullptr || row.stamp.commit_seq <= owner->stamp.commit_seq ||
            foundation::id_boot_epoch(row.id) > boot_epoch_limit ||
            model_.agents.find_instance(row.owner_instance_id) == nullptr ||
            model_.goals.find(row.id, other) || model_.tasks.find(row.id, other) ||
            model_.grants.find(row.id, other) || model_.calls.find(row.id, other) ||
            model_.memories.find(row.id, other) || model_.agents.find_instance(row.id) != nullptr ||
            !memory_dependencies_history(model_, row.dependencies, boot_epoch_limit)) {
            return false;
        }
        for (uint32_t scene = 0; scene < model_.scenes.state_count(); ++scene) {
            if (model_.scenes.state(scene).object_id == row.id) {
                return false;
            }
        }
        if (row.pending.action == knowledge::ContextAction::publish) {
            if (!model_.tasks.find(row.pending.task_id, other)) {
                return false;
            }
            auto before = row;
            --before.generation;
            knowledge::MemoryDependencies sources{};
            if (!task_context_sources(model_, other, before, sources) ||
                !knowledge::same_dependencies(sources, row.dependencies)) {
                return false;
            }
        }
    }
    return true;
}
} // namespace ayni::kernel

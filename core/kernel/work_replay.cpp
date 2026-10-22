#include "kernel/work_replay.h"

#include "authority/grant/grant_records.h"
#include "authority/principal/owner_binding_records.h"
#include "kernel/command/command_records.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

using durability::RecordUse;

template <typename Restore> RecordUse use_of(Restore restore) {
    switch (restore) {
    case Restore::restored:
        return RecordUse::applied;
    case Restore::no_room:
        return RecordUse::no_room;
    case Restore::corrupt:
        break;
    }
    return RecordUse::corrupt;
}

} // namespace

RecordUse WorkReplay::on_record(void *context, const durability::BatchIdentity &batch,
                                const durability::RecordHeader &header, const uint8_t *body) {
    return static_cast<WorkReplay *>(context)->apply(batch, header, body);
}

bool WorkReplay::binding_matches() const {
    if (model_.principals.binding_known()) {
        return model_.principals.binding_matches();
    }
    const authority::Principal *owner = model_.principals.owner();
    for (uint32_t index = 0; index < model_.goals.size(); ++index) {
        const work::GoalOrigin &origin = model_.goals.at(index).origin;
        if (origin.kind == AYNI_ORIGIN_USER &&
            (owner == nullptr || origin.principal_id != owner->principal_id ||
             origin.binding_revision != owner->binding_revision)) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.commands.size(); ++index) {
        if (owner == nullptr || model_.commands.at(index).principal_id != owner->principal_id) {
            return false;
        }
    }
    return true;
}

// Discovery must enforce ownership's header rule even when a snapshot
// covers the record and its body will not be replayed.
RecordUse WorkReplay::classify_header(const durability::RecordHeader &header) {
    if ((header.type == authority::owner_binding_record_type ||
         header.type == authority::grant_record_type ||
         header.type == authority::grant_state_record_type ||
         (header.type == command_record_type && header.version >= exact_command_record_version)) &&
        (header.flags & durability::record_required) == 0U) {
        return RecordUse::corrupt;
    }
    if (header.type == command_record_type && header.version == exact_command_record_version) {
        return RecordUse::applied;
    }
    if (header.version != 1U) {
        return RecordUse::unknown;
    }
    switch (header.type) {
    case authority::owner_binding_record_type:
    case authority::grant_record_type:
    case authority::grant_state_record_type:
    case work::instance_record_type:
    case work::goal_record_type:
    case work::goal_state_record_type:
    case work::task_record_type:
    case work::task_state_record_type:
    case work::task_steps_record_type:
    case work::task_checkpoint_record_type:
    case command_record_type:
        return RecordUse::applied;
    default:
        return RecordUse::unknown;
    }
}

RecordUse WorkReplay::apply(const durability::BatchIdentity &batch,
                            const durability::RecordHeader &header, const uint8_t *body) {
    // A batch that created a goal or a task without its state ended before
    // this record.
    if (batch.commit_seq != batch_seq_) {
        if (!complete()) {
            return RecordUse::corrupt;
        }
        batch_seq_ = batch.commit_seq;
    }
    const foundation::TransitionStamp stamp{batch.transition_id, batch.commit_seq};
    const uint32_t size = header.length;
    const RecordUse use = classify_header(header);
    if (use != RecordUse::applied) {
        return use;
    }
    switch (header.type) {
    case authority::owner_binding_record_type:
        return restore_owner_binding(body, size, stamp);
    case authority::grant_record_type:
        return restore_grant(body, size, stamp);
    case authority::grant_state_record_type:
        return restore_grant_state(body, size, stamp);
    case work::instance_record_type:
        return restore_instance(body, size, stamp);
    case work::goal_record_type:
        return restore_goal(body, size, stamp);
    case work::goal_state_record_type:
        return restore_goal_state(body, size, stamp);
    case work::task_record_type:
        return restore_task(body, size, stamp);
    case work::task_state_record_type:
        return restore_task_state(body, size, stamp);
    case work::task_steps_record_type:
        return restore_task_steps(body, size, stamp);
    case work::task_checkpoint_record_type:
        return restore_task_checkpoint(body, size, stamp);
    case command_record_type:
        return restore_command(header.version, body, size, batch.commit_seq);
    default:
        return RecordUse::unknown;
    }
}

static_assert(authority::owner_binding_record_version == 1U &&
                  work::instance_record_version == 1U && work::goal_record_version == 1U &&
                  work::goal_state_record_version == 1U && work::task_record_version == 1U &&
                  work::task_state_record_version == 1U && work::task_steps_record_version == 1U &&
                  work::task_checkpoint_record_version == 1U && command_record_version == 1U &&
                  authority::grant_record_version == 1U &&
                  authority::grant_state_record_version == 1U && exact_command_record_version == 2U,
              "classify_header/apply know work v1 and exact command v2");

RecordUse WorkReplay::restore_owner_binding(const uint8_t *body, uint32_t size,
                                            foundation::TransitionStamp stamp) {
    authority::OwnerBindingRecord record{};
    if (!authority::read_owner_binding_record(body, size, record) ||
        !model_.principals.restore_binding(record, stamp)) {
        return RecordUse::corrupt;
    }
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_instance(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp) {
    work::InstanceRecord record{};
    if (!work::read_instance_record(body, size, record)) {
        return RecordUse::corrupt;
    }
    return use_of(model_.agents.restore(record.agent_id, record.instance_id, record.revision,
                                        record.lifecycle, stamp));
}

RecordUse WorkReplay::restore_goal(const uint8_t *body, uint32_t size,
                                   foundation::TransitionStamp stamp) {
    work::GoalRecord record{};
    // A goal is created for an instance that has started.
    if (pending_goal_id_ != 0U || !work::read_goal_record(body, size, record) ||
        model_.agents.find_instance(record.goal.owner_instance_id) == nullptr) {
        return RecordUse::corrupt;
    }
    record.goal.goal_type_index = work::GoalTable::no_index;
    uint32_t index = 0;
    const work::GoalTable::Restore restore =
        model_.goals.restore(record.goal, record.goal_id, record.replaced_goal_id, stamp, index);
    if (restore != work::GoalTable::Restore::restored) {
        return use_of(restore);
    }
    // The goal that gave way took its tasks, which name the place, with it
    // (work::insert_goal).
    if (record.replaced_goal_id != 0U) {
        model_.tasks.remove_goal(index);
    }
    if (model_.goals.at(index).depth != record.depth) {
        return RecordUse::corrupt;
    }
    pending_goal_id_ = record.goal_id;
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_goal_state(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::GoalStateRecord record{};
    uint32_t index = 0;
    if (!work::read_goal_state_record(body, size, record) ||
        !model_.goals.find(record.goal_id, index)) {
        return RecordUse::corrupt;
    }
    if (record.goal_id == pending_goal_id_) {
        pending_goal_id_ = 0;
    }
    return use_of(model_.goals.restore_state(index, record.state, stamp));
}

RecordUse WorkReplay::restore_task(const uint8_t *body, uint32_t size,
                                   foundation::TransitionStamp stamp) {
    work::TaskRecord record{};
    uint32_t goal_index = 0;
    if (pending_task_id_ != 0U || !work::read_task_record(body, size, record) ||
        !model_.goals.find(record.task.goal_id, goal_index) ||
        model_.goals.at(goal_index).owner_instance_id != record.task.owner_instance_id) {
        return RecordUse::corrupt;
    }
    record.task.goal_index = goal_index;
    const work::TaskTable::Restore restore =
        model_.tasks.restore(record.task, record.task_id, record.replaced_task_id, stamp);
    if (restore == work::TaskTable::Restore::restored) {
        pending_task_id_ = record.task_id;
    }
    return use_of(restore);
}

RecordUse WorkReplay::restore_task_state(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::TaskStateRecord record{};
    uint32_t index = 0;
    if (!work::read_task_state_record(body, size, record) ||
        !model_.tasks.find(record.task_id, index)) {
        return RecordUse::corrupt;
    }
    if (record.task_id == pending_task_id_) {
        pending_task_id_ = 0;
    }
    return use_of(model_.tasks.restore_state(index, record.state, stamp));
}

RecordUse WorkReplay::restore_task_steps(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::TaskTable &tasks = model_.tasks;
    uint64_t task_id = 0;
    uint32_t index = work::TaskTable::no_index;
    work::TaskTable::Restore failure = work::TaskTable::Restore::corrupt;
    const bool read =
        work::read_task_steps_record(body, size, task_id, [&](uint32_t step, ayni_string_v1 key) {
            if (index == work::TaskTable::no_index && !tasks.find(task_id, index)) {
                return false;
            }
            const work::TaskTable::Restore restore = tasks.restore_step(index, step, key, stamp);
            if (restore != work::TaskTable::Restore::restored) {
                failure = restore;
                return false;
            }
            return true;
        });
    return read ? RecordUse::applied : use_of(failure);
}

RecordUse WorkReplay::restore_task_checkpoint(const uint8_t *body, uint32_t size,
                                              foundation::TransitionStamp stamp) {
    work::TaskCheckpointRecord record{};
    uint32_t index = 0;
    if (!work::read_task_checkpoint_record(body, size, record) ||
        !model_.tasks.find(record.task_id, index)) {
        return RecordUse::corrupt;
    }
    return use_of(model_.tasks.restore_checkpoint(index, record.writer_id, record.writer_version,
                                                  record.state, stamp));
}

RecordUse WorkReplay::restore_grant(const uint8_t *body, uint32_t size,
                                    foundation::TransitionStamp stamp) {
    authority::GrantRecord record{};
    const auto *binding = model_.principals.binding();
    if (pending_grant_id_ != 0U || model_.commands.mapped_in(stamp.commit_seq) != nullptr ||
        !authority::read_grant_record(body, size, record) || binding == nullptr ||
        binding->principal_id != record.grant.owner_principal_id ||
        binding->binding_revision != record.grant.owner_binding_revision ||
        binding->stamp.commit_seq >= stamp.commit_seq ||
        binding->stamp.transition_id >= stamp.transition_id ||
        model_.agents.find_instance(record.grant.subject_instance_id) == nullptr) {
        return RecordUse::corrupt;
    }
    const auto restored = model_.grants.restore_create(record, stamp);
    if (restored == authority::GrantTable::Restore::restored) {
        pending_grant_id_ = record.grant_id;
    }
    return use_of(restored);
}

RecordUse WorkReplay::restore_grant_state(const uint8_t *body, uint32_t size,
                                          foundation::TransitionStamp stamp) {
    authority::GrantStateRecord record{};
    if (!authority::read_grant_state_record(body, size, record)) {
        return RecordUse::corrupt;
    }
    return use_of(model_.grants.restore_state(record, stamp));
}

// An absent principal's retry window is still durable state. Keeping it
// prevents compaction from turning an old accepted command into a new one.
RecordUse WorkReplay::restore_command(uint16_t version, const uint8_t *body, uint32_t size,
                                      uint64_t commit_seq) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    ayni_bytes_v1 canonical{};
    if (model_.commands.mapped_in(commit_seq) != nullptr ||
        !read_command_record(version, body, size, mapping, floor, canonical)) {
        return RecordUse::corrupt;
    }
    mapping.commit_seq = commit_seq;
    if (mapping.kind == AYNI_INTENT_GRANT_CREATE) {
        uint32_t index = 0;
        if (pending_grant_id_ == 0U || mapping.object.id != pending_grant_id_ ||
            !model_.grants.find(pending_grant_id_, index)) {
            return RecordUse::corrupt;
        }
        const auto &grant = model_.grants.at(index);
        if (!matches_grant_creation(mapping, canonical, grant, model_.grants.source_domain(index),
                                    model_.grants.scope(index))) {
            return RecordUse::corrupt;
        }
        pending_grant_id_ = 0;
    }
    return use_of(model_.commands.restore_checked(mapping, floor, canonical));
}

bool WorkReplay::matches_grant_creation(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                        const authority::Grant &grant, ayni_bytes_v1 domain,
                                        ayni_bytes_v1 scope) {
    AuthorityCommandRecord request{};
    // C parents have creation revision 1 while ACTIVE; later terminal
    // revisions must not rewrite the original command's expected revision.
    return read_authority_command(canonical, request) &&
           grant.created_stamp.commit_seq == mapping.commit_seq &&
           mapping.principal_id == grant.owner_principal_id &&
           mapping.object.id == grant.grant_id &&
           grant.capability_id.equals(request.capability_id) &&
           grant.capability_version == request.capability_version &&
           grant.subject_instance_id == request.subject_instance_id &&
           grant.parent_grant_id == request.parent_grant_id && grant.lifetime == request.lifetime &&
           grant.duration_us == request.duration_us &&
           grant.allow_delegation == ((request.flags & AYNI_GRANT_ALLOW_DELEGATION) != 0U) &&
           request.expected_revision == (grant.parent_grant_id == 0U ? 0U : 1U) &&
           authority_command_scope_matches(request, domain, scope);
}

// Ending a goal ends its descendants and every task it reaches, and
// stopping an instance ends every goal it owns, in the same transition
// (ADR-0018, ADR-0025 decision 5); so no stored transition leaves a live
// goal under a terminal parent or a stopped owner, or a live task under a
// terminal goal.
bool WorkReplay::consistent(uint64_t boot_epoch_limit) const {
    if (!model_.grants.consistent()) {
        return false;
    }
    const auto *binding = model_.principals.binding();
    for (uint32_t index = 0; index < model_.grants.size(); ++index) {
        const auto &grant = model_.grants.at(index);
        if (binding == nullptr || binding->principal_id != grant.owner_principal_id ||
            binding->binding_revision != grant.owner_binding_revision ||
            model_.agents.find_instance(grant.subject_instance_id) == nullptr ||
            (grant.grant_id >> 32U) > boot_epoch_limit ||
            (grant.subject_instance_id >> 32U) > boot_epoch_limit) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.goals.size(); ++index) {
        const work::Goal &goal = model_.goals.at(index);
        if (!work::GoalTable::is_live(goal.lifecycle)) {
            continue;
        }
        const work::AgentInstance *owner = model_.agents.find_instance(goal.owner_instance_id);
        if ((goal.parent_index != work::GoalTable::no_parent &&
             !work::GoalTable::is_live(model_.goals.at(goal.parent_index).lifecycle)) ||
            owner == nullptr || owner->lifecycle == AYNI_AGENT_STOPPED) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.tasks.size(); ++index) {
        const work::Task &task = model_.tasks.at(index);
        if (work::TaskTable::is_live(task.lifecycle) &&
            !work::GoalTable::is_live(model_.goals.at(task.goal_index).lifecycle)) {
            return false;
        }
    }
    return true;
}

void WorkReplay::finish(uint64_t now_us) {
    model_.agents.fault_unregistered();
    model_.goals.resolve_types([this](const catalog::CatalogId &id, uint32_t version,
                                      ayni_trigger_kind_t trigger, uint32_t &out_index) {
        const catalog::GoalTypeRegistry::Lookup type =
            model_.goal_types.find({id.data(), id.size()}, version);
        switch (type.kind) {
        case catalog::GoalTypeRegistry::Lookup::Kind::found:
            out_index = type.index;
            // A goal with a trigger derives tasks, and the type no longer
            // has a strategy to decide them.
            return trigger != AYNI_TRIGGER_NONE &&
                           model_.goal_types.at(type.index).strategy_index ==
                               catalog::GoalType::no_strategy
                       ? AYNI_REASON_STRATEGY_CHANGED
                       : AYNI_REASON_NONE;
        case catalog::GoalTypeRegistry::Lookup::Kind::unknown_version:
            return AYNI_REASON_GOAL_TYPE_VERSION;
        case catalog::GoalTypeRegistry::Lookup::Kind::unknown_id:
            break;
        }
        return AYNI_REASON_GOAL_TYPE_UNKNOWN;
    });
    model_.grants.resolve_catalog(model_.capabilities);
    for (uint32_t index = 0; index < model_.grants.size(); ++index) {
        const auto *subject =
            model_.agents.find_instance(model_.grants.at(index).subject_instance_id);
        if (subject == nullptr ||
            work::AgentTable::effective_lifecycle(*subject) == AYNI_AGENT_STOPPED ||
            work::AgentTable::effective_lifecycle(*subject) == AYNI_AGENT_FAULTED) {
            model_.grants.hold_restored(index, AYNI_REASON_GRANT_SUBJECT);
        }
    }
    hold_tasks();
    hold_unconfirmed_work();
    model_.goals.reanchor(now_us);
    model_.tasks.reanchor(now_us);
    model_.due_work_changed = true;
}

// The strategy that decides a task is its goal type's, and only the
// strategy that wrote a checkpoint, at that version, can read it
// (C-POLICY-010).
void WorkReplay::hold_tasks() {
    work::TaskTable &tasks = model_.tasks;
    for (uint32_t index = 0; index < tasks.size(); ++index) {
        const work::Task &task = tasks.at(index);
        if (!work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        const work::Goal &goal = model_.goals.at(task.goal_index);
        if (work::GoalTable::is_held(goal)) {
            tasks.hold_restored(index,
                                goal.hold_reason == AYNI_REASON_GOAL_TYPE_UNKNOWN
                                    ? AYNI_NOT_FOUND
                                    : AYNI_UNSUPPORTED_VERSION,
                                goal.hold_reason);
            continue;
        }
        const catalog::GoalType &type = model_.goal_types.at(goal.goal_type_index);
        if (type.strategy_index == catalog::GoalType::no_strategy) {
            // A goal without a trigger derives no task, and one with a
            // trigger is held above, so only a log this library did not
            // write gets here; hold rather than ask a strategy that is not
            // there.
            tasks.hold_restored(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
            continue;
        }
        const catalog::Strategy &strategy = model_.strategies.at(type.strategy_index);
        if (task.checkpoint_writer_version == 0U) {
            continue;
        }
        if (strategy.version != task.checkpoint_writer_version ||
            !strategy.id.equals(
                {task.checkpoint_writer_id.data(), task.checkpoint_writer_id.size()})) {
            tasks.hold_restored(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
        } else if (task.checkpoint_size > strategy.max_checkpoint_bytes) {
            tasks.hold_restored(index, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT);
        }
    }
}

// Compatibility failures remain the primary reason for a hold. Otherwise
// legacy work has no durable authorization evidence and must not advance.
void WorkReplay::hold_unconfirmed_work() {
    for (uint32_t index = 0; index < model_.goals.size(); ++index) {
        model_.goals.hold_restored(index, AYNI_REASON_LEGACY_UNCONFIRMED);
    }
    for (uint32_t index = 0; index < model_.tasks.size(); ++index) {
        if (!work::TaskTable::is_held(model_.tasks.at(index))) {
            model_.tasks.hold_restored(index, AYNI_DENIED, AYNI_REASON_LEGACY_UNCONFIRMED);
        }
    }
}

void WorkReplay::discard() {
    model_.principals.drop_restored_binding();
    model_.agents.drop_restored();
    model_.goals.clear();
    model_.tasks.clear();
    model_.commands.clear();
    model_.grants.clear();
    batch_seq_ = 0;
    pending_goal_id_ = 0;
    pending_task_id_ = 0;
    pending_grant_id_ = 0;
}

void WorkReplay::discard_for_reload() {
    discard();
    model_.sessions.clear();
    model_.due_work_changed = false;
    model_.next_due_work_us = work::no_time;
    model_.searched_with_id = true;
    model_.ids = foundation::IdAllocator{};
    model_.transitions = TransitionClock{};
}

} // namespace ayni::kernel

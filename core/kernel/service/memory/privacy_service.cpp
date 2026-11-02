#include "kernel/service/memory/privacy_service.h"
namespace ayni::kernel {
namespace {
using work::ContentState;
using work::PrivacyEffect;
using work::PrivacyPhase;
using work::ProvenanceKind;
bool retained_leaf(const void *context, const knowledge::MemoryDependency &dependency) {
    uint32_t index = 0;
    return dependency.kind == knowledge::DependencyKind::memory ||
           static_cast<const WorkModel *>(context)->contexts.current_reference(dependency, index);
}
bool retained_dependency(const void *context, const knowledge::MemoryDependency &dependency) {
    if (dependency.kind == knowledge::DependencyKind::memory) {
        return true;
    }
    const auto &model = *static_cast<const WorkModel *>(context);
    uint32_t index = 0;
    return model.contexts.current_reference(dependency, index) &&
           context_sources_retained(model, index);
}
bool invalid_provenance(const WorkModel &model, const work::ProvenanceRecord *record) {
    return record != nullptr && (record->content != ContentState::available ||
                                 !content_sources_retained(model, record->dependencies));
}
const work::ProvenanceRecord *goal_sources(const WorkModel &model, uint32_t index) {
    return model.provenance.find(ProvenanceKind::goal, index, model.goals.at(index).goal_id);
}
const work::ProvenanceRecord *task_sources(const WorkModel &model, uint32_t index) {
    const auto &task = model.tasks.at(index);
    return model.provenance.find(ProvenanceKind::task, task.slot, task.task_id);
}
bool task_invalid(const WorkModel &model, uint32_t index) {
    return invalid_provenance(model, task_sources(model, index)) ||
           invalid_provenance(model, goal_sources(model, model.tasks.at(index).goal_index));
}
bool erased(const work::ProvenanceRecord *record) {
    return record != nullptr && record->content == ContentState::erased;
}
// Only private-content command families lose their canonical bytes. Grant and unrelated receipts
// preserve their independent exact retry contract and the principal's existing floor.
bool command_invalid(const WorkModel &model, const CommandMapping &mapping) {
    if (mapping.privacy_expired) {
        return false;
    }
    uint32_t index = 0;
    if (is_memory_command(mapping.kind) && model.memories.find(mapping.object.id, index)) {
        const auto &memory = model.memories.at(index);
        return memory.revision != mapping.object.revision ||
               memory.state != knowledge::MemoryState::live ||
               !content_sources_retained(model, memory.dependencies);
    }
    return !content_sources_retained(model, mapping.sources) ||
           (mapping.object.kind == AYNI_OBJECT_GOAL && model.goals.find(mapping.object.id, index) &&
            erased(goal_sources(model, index)));
}
// The cursor names a stable Kernel ID (command phase: original acceptance sequence), never a
// mutable row index. This bounded selector is identical in runtime and journal replay.
struct Candidate {
    uint64_t cursor = 0;
    PrivacyEffect effect = PrivacyEffect::none;
    void consider(uint64_t id, uint64_t after, PrivacyEffect change) {
        if (id > after && (cursor == 0 || id < cursor)) {
            cursor = id;
            effect = change;
        }
    }
};
Candidate memory_candidate(const WorkModel &model, uint64_t after, PrivacyPhase phase) {
    Candidate result;
    for (uint32_t index = 0; index < model.memories.size(); ++index) {
        const auto &row = model.memories.at(index);
        const bool scrubbed = row.state == knowledge::MemoryState::scrubbed;
        const bool invalid = row.state != knowledge::MemoryState::live ||
                             !content_sources_retained(model, row.dependencies);
        const auto effect =
            phase == PrivacyPhase::purge
                ? (scrubbed ? PrivacyEffect::remove_memory : PrivacyEffect::none)
                : (!scrubbed && invalid ? PrivacyEffect::memory : PrivacyEffect::none);
        result.consider(row.id, after, effect);
    }
    return result;
}
Candidate work_candidate(const WorkModel &model, uint64_t after, PrivacyPhase phase) {
    Candidate result;
    if (phase == PrivacyPhase::goals) {
        for (uint32_t index = 0; index < model.goals.size(); ++index) {
            const auto *sources = goal_sources(model, index);
            result.consider(model.goals.at(index).goal_id, after,
                            !erased(sources) && invalid_provenance(model, sources)
                                ? PrivacyEffect::goal
                                : PrivacyEffect::none);
        }
    } else {
        for (uint32_t index = 0; index < model.tasks.size(); ++index) {
            result.consider(model.tasks.at(index).task_id, after,
                            !erased(task_sources(model, index)) && task_invalid(model, index)
                                ? PrivacyEffect::task
                                : PrivacyEffect::none);
        }
    }
    return result;
}
Candidate candidate(const WorkModel &model, PrivacyPhase phase, uint64_t after) {
    if (phase == PrivacyPhase::memories || phase == PrivacyPhase::purge) {
        return memory_candidate(model, after, phase);
    }
    if (phase == PrivacyPhase::goals || phase == PrivacyPhase::tasks) {
        return work_candidate(model, after, phase);
    }
    Candidate result;
    if (phase == PrivacyPhase::contexts) {
        for (uint32_t index = 0; index < model.contexts.size(); ++index) {
            const auto &row = model.contexts.at(index);
            const bool invalid = row.pending.action <= knowledge::ContextAction::publish &&
                                 !context_sources_retained(model, index);
            result.consider(row.id, after, invalid ? PrivacyEffect::context : PrivacyEffect::none);
        }
    } else if (phase == PrivacyPhase::calls) {
        for (uint32_t index = 0; index < model.calls.capacity(); ++index) {
            if (!model.calls.occupied(index)) {
                continue;
            }
            uint32_t task = 0;
            const auto &id = model.calls.identity(index);
            const bool invalid = !model.calls.state(index).content_erased &&
                                 model.tasks.find(id.task_id, task) && task_invalid(model, task);
            result.consider(id.call_id, after, invalid ? PrivacyEffect::call : PrivacyEffect::none);
        }
    } else if (phase == PrivacyPhase::commands) {
        for (uint32_t index = 0; index < model.commands.size(); ++index) {
            const auto &mapping = model.commands.at(index);
            result.consider(mapping.commit_seq, after,
                            command_invalid(model, mapping) ? PrivacyEffect::command
                                                            : PrivacyEffect::none);
        }
    }
    return result;
}
void erase_work(WorkModel &model, ProvenanceKind kind, uint32_t index,
                foundation::TransitionStamp stamp) {
    const auto *record =
        kind == ProvenanceKind::goal ? goal_sources(model, index) : task_sources(model, index);
    if (record != nullptr) {
        model.release_content_references(record->dependencies, stamp.commit_seq);
    }
    if (kind == ProvenanceKind::goal) {
        model.provenance.erase(kind, index, model.goals.at(index).goal_id, stamp);
        model.goals.erase_parameters(index);
    } else {
        const auto &task = model.tasks.at(index);
        model.provenance.erase(kind, task.slot, task.task_id, stamp);
        model.tasks.erase_checkpoint(index);
    }
}
} // namespace
bool content_sources_retained(const WorkModel &model,
                              const knowledge::MemoryDependencies &dependencies) {
    return model.memories.dependencies_current(dependencies, UINT64_MAX,
                                               {&model, retained_dependency});
}
bool context_sources_retained(const WorkModel &model, uint32_t index) {
    const auto &record = model.contexts.at(index);
    return record.pending.action <= knowledge::ContextAction::publish &&
           model.memories.dependencies_current(record.dependencies, UINT64_MAX,
                                               {&model, retained_leaf});
}
// Exhaustion cannot advance the cursor or consume a transition. The query exposes this permanent
// capacity failure while the invalidation barrier and incomplete cleanup remain in force.
ayni_status_t privacy_progress_status(const WorkModel &model, uint64_t generation) {
    const auto &record = model.privacy.record();
    if (record.phase == PrivacyPhase::files &&
        generation > UINT64_MAX - work::PrivacyCleanup::snapshot_passes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (record.phase == PrivacyPhase::contexts && !model.privacy.source_rescan_required()) {
        const auto next = candidate(model, record.phase, record.cursor);
        uint32_t index = 0;
        if (next.effect == PrivacyEffect::context && model.contexts.find(next.cursor, index) &&
            (model.contexts.at(index).revision == UINT32_MAX ||
             model.contexts.at(index).generation == UINT64_MAX)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
    return AYNI_OK;
}
// Phase gates (files acknowledged, erased transport released, two completed rotations) belong
// to runtime. This owner selects only deterministic model effects and explicit progress facts.
bool next_privacy_step(const WorkModel &model, uint64_t generation, work::PrivacyRecord &out) {
    if (!model.privacy.pending() || privacy_progress_status(model, generation) != AYNI_OK) {
        return false;
    }
    const auto &old = model.privacy.record();
    if (old.phase == PrivacyPhase::compact && generation < old.snapshot_generation) {
        return false;
    }
    // A Context revision change can invalidate an earlier Memory or Context through another
    // anchor. Reach a fixed point before erasing work; each rewind removes one source set.
    if (model.privacy.source_rescan_required()) {
        out = {old.request_seq, PrivacyPhase::memories, PrivacyEffect::none, 0, 0, {}};
        return true;
    }
    const auto next = candidate(model, old.phase, old.cursor);
    out = old;
    out.stamp = {};
    out.cursor = next.cursor;
    out.effect = next.effect;
    if (next.cursor != 0) {
        return true;
    }
    out.phase = static_cast<PrivacyPhase>(static_cast<uint32_t>(old.phase) + 1);
    if (out.phase == PrivacyPhase::compact) {
        out.snapshot_generation = generation + work::PrivacyCleanup::snapshot_passes;
    }
    return true;
}
// Every branch delegates erasure to the representation's owner. The progress record is the sole
// journal effect, while snapshots retain each owner's explicit erased representation.
bool apply_privacy_step(WorkModel &model, const work::PrivacyRecord &record, uint64_t boot_epoch,
                        bool replay) {
    uint32_t index = 0;
    switch (record.effect) {
    case PrivacyEffect::none:
        break;
    case PrivacyEffect::memory:
        if (!model.memories.find(record.cursor, index)) {
            return false;
        }
        model.release_content_references(model.memories.at(index).dependencies,
                                         record.stamp.commit_seq);
        model.memories.scrub(index, record.stamp);
        break;
    case PrivacyEffect::goal:
        if (!model.goals.find(record.cursor, index)) {
            return false;
        }
        erase_work(model, ProvenanceKind::goal, index, record.stamp);
        break;
    case PrivacyEffect::task:
        if (!model.tasks.find(record.cursor, index)) {
            return false;
        }
        erase_work(model, ProvenanceKind::task, index, record.stamp);
        break;
    case PrivacyEffect::call:
        if (!model.calls.find(record.cursor, index) ||
            model.calls.erase_content(index, {boot_epoch, record.stamp}, !replay) != AYNI_OK) {
            return false;
        }
        break;
    case PrivacyEffect::command:
        for (uint32_t command = 0; command < model.commands.size(); ++command) {
            const auto &mapping = model.commands.at(command);
            if (mapping.commit_seq == record.cursor) {
                model.release_content_references(mapping.sources, record.stamp.commit_seq);
                break;
            }
        }
        if (!model.commands.expire_content(record.cursor)) {
            return false;
        }
        break;
    case PrivacyEffect::remove_memory:
        if (!model.memories.find(record.cursor, index) ||
            model.memories.at(index).state != knowledge::MemoryState::scrubbed) {
            return false;
        }
        model.memories.remove_scrubbed(index);
        break;
    case PrivacyEffect::context:
        if (!model.contexts.find(record.cursor, index) ||
            model.contexts.at(index).revision == UINT32_MAX ||
            model.contexts.at(index).generation == UINT64_MAX) {
            return false;
        }
        model.release_content_references(model.contexts.at(index).dependencies,
                                         record.stamp.commit_seq);
        model.contexts.erase(index, knowledge::ContextAction::erase_derived, 0, record.stamp);
        break;
    }
    model.privacy.restore_validated(record);
    return true;
}
bool advance_privacy(WorkModel &model, uint64_t generation) {
    work::PrivacyRecord next{};
    if (!next_privacy_step(model, generation, next)) {
        return false;
    }
    next.stamp = model.transitions.next();
    return apply_privacy_step(model, next, model.ids.boot_epoch());
}
} // namespace ayni::kernel

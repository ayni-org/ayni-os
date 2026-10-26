#include "kernel/service/memory/memory_decision.h"
#include "catalog/abi_checks.h"
#include <cstring>
namespace ayni::kernel {
namespace {
using namespace knowledge;
}
MemoryDecision::MemoryDecision(WorkModel &model, uint32_t task_index, uint64_t durable_seq,
                               OwnerClock clock, uint8_t *staging)
    : model_(model), task_index_(task_index), durable_seq_(durable_seq), clock_(clock),
      staging_(staging) {
    const auto &task = model_.tasks.at(task_index_);
    const auto *goal =
        model_.provenance.find(work::ProvenanceKind::goal, task.goal_index, task.goal_id);
    const auto *previous =
        model_.provenance.find(work::ProvenanceKind::task, task.slot, task.task_id);
    constexpr std::size_t inherited_sources = 2;
    const std::array<const work::ProvenanceRecord *, inherited_sources> sources{goal, previous};
    for (const auto *source : sources) {
        if (source == nullptr) {
            continue;
        }
        if (source->content == work::ContentState::erased) {
            failure_ = AYNI_EXPIRED;
            break;
        }
        failure_ = merge_dependencies(dependencies_, source->dependencies,
                                      model_.memories.limits().dependencies);
        if (failure_ != AYNI_OK) {
            break;
        }
    }
}
void MemoryDecision::retain_export(MemoryDependency &dependency) const {
    for (uint32_t i = 0; i < dependencies_.count; ++i) {
        const auto &old = dependencies_.items[i];
        if (same_memory_read(old, dependency)) {
            dependency.egress = old.egress;
            return;
        }
    }
}
// Every ancestor gets an explicit current read by this actor; its original graph still keeps
// the permissions under which it was derived. Sharing a summary grants no implicit source access.
ayni_status_t MemoryDecision::capture(uint32_t index, MemoryPurpose purpose) {
    auto staged = dependencies_;
    const auto actor = model_.tasks.at(task_index_).owner_instance_id;
    const MemoryAccess access{model_, clock_.now_us(), durable_seq_};
    std::array<uint64_t, memory_dependency_max> queue{};
    uint32_t count = 1, next = 0;
    queue[0] = model_.memories.at(index).id;
    while (next < count) {
        uint32_t source_index = 0;
        if (!model_.memories.find(queue[next++], source_index) ||
            !memory_current(access, source_index)) {
            return AYNI_EXPIRED;
        }
        const auto &record = model_.memories.at(source_index);
        MemoryDependency ref{
            DependencyKind::memory, record.id, record.revision, actor, purpose, {}, {}};
        const auto allowed = memory_read_permission(access, actor, source_index, purpose, ref.read);
        if (allowed != AYNI_OK) {
            return allowed;
        }
        retain_export(ref);
        const auto added = add_dependency(staged, ref, model_.memories.limits().dependencies);
        if (added != AYNI_OK) {
            return added;
        }
        for (uint32_t d = 0; d < record.dependencies.count; ++d) {
            const auto &source = record.dependencies.items[d];
            if (source.kind == DependencyKind::context) {
                const auto status =
                    add_dependency(staged, source, model_.memories.limits().dependencies);
                if (status != AYNI_OK) {
                    return status;
                }
                continue;
            }
            bool seen = false;
            for (uint32_t i = 0; i < count; ++i) {
                seen = seen || queue[i] == source.id;
            }
            if (!seen) {
                if (count == queue.size()) {
                    return AYNI_CAPACITY_EXCEEDED;
                }
                queue[count++] = source.id;
            }
        }
    }
    dependencies_ = staged;
    return AYNI_OK;
}
// Shape and output capacity checks precede capture; all accepted content/metadata exposure records
// sources.
ayni_status_t MemoryDecision::read(void *context, uint64_t id, ayni_memory_purpose_t purpose,
                                   ayni_memory_info_v1 *out, char *body, uint32_t capacity,
                                   uint32_t *out_bytes) {
    if (context == nullptr || out == nullptr || out_bytes == nullptr || body == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<MemoryDecision *>(context);
    if (!self.active_ || !self.model_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    if (self.failure_ != AYNI_OK) {
        return self.failure_;
    }
    const auto shape = catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    uint32_t index = 0;
    if (!self.model_.memories.find(id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto status = self.capture(index, static_cast<MemoryPurpose>(purpose));
    if (status != AYNI_OK) {
        self.failure_ = status;
        return status;
    }
    const auto text = self.model_.memories.body(index);
    if (capacity < text.size) {
        *out_bytes = text.size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    const MemoryAccess final_access{self.model_, self.clock_.now_us(), self.durable_seq_};
    if (!memory_dependencies_current(final_access, self.dependencies_)) {
        self.failure_ = AYNI_EXPIRED;
        return self.failure_;
    }
    const auto info = memory_info(final_access, index);
    if (info.usable == 0) {
        self.failure_ = AYNI_EXPIRED;
        return self.failure_;
    }
    std::memcpy(body, text.data, text.size);
    *out = info;
    *out_bytes = text.size;
    return AYNI_OK;
}
ayni_status_t MemoryDecision::validate() const {
    if (failure_ != AYNI_OK) {
        return failure_;
    }
    return memory_dependencies_current({model_, clock_.now_us(), durable_seq_}, dependencies_)
               ? AYNI_OK
               : AYNI_EXPIRED;
}
void MemoryDecision::publish(foundation::TransitionStamp stamp) {
    if (dependencies_.count == 0 && !proposal_) {
        return;
    }
    const auto &task = model_.tasks.at(task_index_);
    model_.provenance.assign(work::ProvenanceKind::task, task.slot, task.task_id, dependencies_,
                             stamp);
}

// Searching content is a read even when it does not match. Capture each examined authorized
// source before matching; only publish the complete bounded page after a fresh final check.
ayni_status_t MemoryDecision::search(void *context, const ayni_memory_search_v1 *query,
                                     ayni_memory_purpose_t raw_purpose, ayni_memory_info_v1 *out,
                                     uint32_t capacity, uint32_t *out_count) {
    if (context == nullptr || query == nullptr || out_count == nullptr ||
        (capacity != 0 && out == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<MemoryDecision *>(context);
    if (!self.active_ || !self.model_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    if (self.failure_ != AYNI_OK) {
        return self.failure_;
    }
    const auto form = memory_search_form(*query);
    if (form != AYNI_OK) {
        return form;
    }
    const auto purpose = static_cast<MemoryPurpose>(raw_purpose);
    if (purpose != MemoryPurpose::context && purpose != MemoryPurpose::organize) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto actor = self.model_.tasks.at(self.task_index_).owner_instance_id;
    const auto space = static_cast<MemoryNamespace>(query->name_space);
    if (purpose == MemoryPurpose::organize && space == MemoryNamespace::private_agent &&
        query->namespace_id != actor) {
        self.failure_ = AYNI_DENIED;
        return self.failure_;
    }
    std::array<uint32_t, memory_dependency_max> selected{};
    uint32_t count = 0;
    uint64_t after = query->after_id;
    const auto &memories = self.model_.memories;
    while (count < capacity) {
        uint32_t index = memories.size();
        for (uint32_t i = 0; i < memories.size(); ++i) {
            const auto &row = memories.at(i);
            if (row.id > after && row.space == space && row.namespace_id == query->namespace_id &&
                (index == memories.size() || row.id < memories.at(index).id)) {
                index = i;
            }
        }
        if (index == memories.size()) {
            break;
        }
        after = memories.at(index).id;
        const MemoryAccess access{self.model_, self.clock_.now_us(), self.durable_seq_};
        MemoryGrantRef permission{};
        if (!memory_current(access, index) ||
            memory_read_permission(access, actor, index, purpose, permission) != AYNI_OK) {
            continue;
        }
        const auto captured = self.capture(index, purpose);
        if (captured != AYNI_OK) {
            self.failure_ = captured;
            return captured;
        }
        if (memories.matches(index, query->keyword, query->tag)) {
            selected[count++] = index;
        }
    }
    const MemoryAccess final_access{self.model_, self.clock_.now_us(), self.durable_seq_};
    if (!memory_dependencies_current(final_access, self.dependencies_)) {
        self.failure_ = AYNI_EXPIRED;
        return self.failure_;
    }
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = memory_info(final_access, selected[i]);
    }
    *out_count = count;
    return AYNI_OK;
}

ayni_status_t MemoryDecision::propose(void *context, const ayni_memory_proposal_v1 *request) {
    if (context == nullptr || request == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<MemoryDecision *>(context);
    if (!self.active_ || !self.model_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    if (self.failure_ != AYNI_OK) {
        return self.failure_;
    }
    const auto status = self.stage(*request);
    if (status != AYNI_OK) {
        self.failure_ = status;
    }
    return status;
}
// Copy candidate text before the caller's nested views expire. No policy-supplied source graph
// or confirmed bit exists: Kernel derives both from this exact decision (ADR-0060).
ayni_status_t MemoryDecision::stage(const ayni_memory_proposal_v1 &request) {
    const auto shape =
        catalog::check_struct_size(request.struct_size, sizeof(request), sizeof(request));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (proposal_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (request.reserved != 0 ||
        (request.source != AYNI_MEMORY_MODEL && request.source != AYNI_MEMORY_DERIVED)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Proposal proposal{};
    if (!proposal.step.assign(request.step_key)) {
        return AYNI_INVALID_ARGUMENT;
    }
    proposal.target_id = request.memory_id;
    proposal.expected_revision = request.expected_revision;
    auto &value = proposal.value;
    value.namespace_id = model_.tasks.at(task_index_).owner_instance_id;
    value.space = MemoryNamespace::private_agent;
    value.source = static_cast<MemorySource>(request.source);
    value.source_call_id = request.source_call_id;
    value.body = request.body;
    if (!read_memory_labels(request.tags, request.tag_count, request.keywords,
                            request.keyword_count, value.labels)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto valid = check_memory_value(value, model_.memories.limits());
    if (valid != AYNI_OK) {
        return valid;
    }
    std::memcpy(staging_, request.body.data, request.body.size);
    value.body = {reinterpret_cast<const char *>(staging_), request.body.size};
    proposal_ = proposal;
    return AYNI_OK;
}
// One new step, complete source set and target CAS are reserved before the publication stamp.
ayni_status_t MemoryDecision::check_proposal(const ayni_policy_intent_v1 &intent) {
    const auto &task = model_.tasks.at(task_index_);
    if (!proposal_ || !proposal_->step.equals({intent.step_key, intent.step_key_size})) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &proposal = *proposal_;
    if (model_.step_taken(task_index_, {proposal.step.data(), proposal.step.size()})) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (model_.accepted_step_count(task_index_) >= model_.tasks.step_capacity() ||
        (proposal.target_id == 0 && !model_.ids.available())) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    proposal.value.dependencies = dependencies_;
    if (!memory_origin_matches(model_, task.task_id, proposal.value.source,
                               proposal.value.source_call_id, durable_seq_)) {
        return AYNI_DENIED;
    }
    if (!memory_dependencies_current({model_, clock_.now_us(), durable_seq_}, dependencies_,
                                     proposal.target_id)) {
        return AYNI_EXPIRED;
    }
    return model_.memories.check_write(proposal.target_id, proposal.expected_revision,
                                       proposal.value, proposal.plan);
}
bool MemoryDecision::accept(foundation::TransitionStamp stamp) {
    auto &proposal = *proposal_;
    uint64_t id = proposal.target_id;
    if (id == 0 && !model_.ids.allocate(id)) {
        return false;
    }
    if (proposal.target_id != 0) {
        model_.release_content_references(model_.memories.at(proposal.plan.index).dependencies,
                                          stamp.commit_seq);
    }
    model_.memories.install(proposal.plan, id, proposal.value, stamp);
    return true;
}
} // namespace ayni::kernel

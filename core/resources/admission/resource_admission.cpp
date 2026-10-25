#include "resources/admission/resource_admission.h"

#include "foundation/deadline.h"

#include <new>

namespace ayni::resources {
namespace {
uint64_t elapsed(uint64_t now_us, uint64_t start_us) {
    return now_us >= start_us ? now_us - start_us : 0U;
}
} // namespace

std::size_t ResourceAdmission::state_storage_size(uint32_t capacity) {
    return sizeof(State) * capacity;
}
std::size_t ResourceAdmission::request_storage_size(uint32_t capacity) {
    return foundation::BoundedArray<Entry>::storage_size(capacity);
}

ResourceAdmission::ResourceAdmission(const catalog::ResourceRegistry &registry, void *states,
                                     uint32_t resource_capacity, void *requests,
                                     uint32_t request_capacity)
    : registry_(registry), states_(static_cast<State *>(states)),
      requests_(requests, request_capacity) {
    static_assert(alignof(State) <= storage_alignment && alignof(Entry) <= storage_alignment,
                  "caller blocks align all admission objects");
    for (uint32_t index = 0; index < resource_capacity; ++index) {
        new (states_ + index) State{};
    }
}

ayni_status_t ResourceAdmission::set_health(uint32_t index, ayni_resource_health_t health,
                                            bool &changed) {
    if (index >= registry_.size() ||
        (health != AYNI_RESOURCE_ONLINE && health != AYNI_RESOURCE_OFFLINE &&
         health != AYNI_RESOURCE_FAULTED)) {
        return AYNI_INVALID_ARGUMENT;
    }
    changed = states_[index].health != health;
    states_[index].health = health;
    return AYNI_OK;
}

ayni_resource_info_v1 ResourceAdmission::info(uint32_t index) const {
    const auto &state = states_[index];
    const auto &resource = registry_.at(index);
    ayni_resource_state_t effective = AYNI_RESOURCE_UNAVAILABLE;
    if (state.health == AYNI_RESOURCE_ONLINE) {
        effective = state.quarantine.call_id != 0U ? AYNI_RESOURCE_QUARANTINED
                    : state.used != 0U             ? AYNI_RESOURCE_HELD
                                                   : AYNI_RESOURCE_FREE;
    }
    ayni_resource_info_v1 info{sizeof(ayni_resource_info_v1),
                               effective,
                               state.health,
                               resource.capacity,
                               state.used,
                               resource.flags,
                               state.quarantine.call_id,
                               state.quarantine.generation,
                               resource.component_count,
                               {},
                               resource.conflict_domain.snapshot()};
    for (uint32_t at = 0; at < resource.component_count; ++at) {
        info.components[at] = resource.components[at].snapshot();
    }
    return info;
}

// Validate before consuming a ticket/slot, including the complete resource
// group. Repeated indices are rejected, so accounting cannot hide an overclaim.
ayni_status_t ResourceAdmission::validate(const ResourceRequest &request) const {
    if (request.call_id == 0U || request.instance_id == 0U || request.priority >= Priority::count ||
        request.count > resource_group_max ||
        (request.on_wait != WaitAction::pause && request.on_wait != WaitAction::skip &&
         request.on_wait != WaitAction::wait) ||
        ((request.on_wait == WaitAction::wait) != (request.wait_us == no_deadline))) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < request.count; ++index) {
        const auto &units = request.resources[index];
        if (units.index >= registry_.size() || units.units == 0U ||
            units.units > registry_.at(units.index).capacity ||
            (index != 0U && request.resources[index - 1U].index >= units.index)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    std::array<uint32_t, resource_group_max> indices{};
    for (uint32_t at = 0; at < request.count; ++at) {
        indices[at] = request.resources[at].index;
    }
    return registry_.compatible_group(indices.data(), request.count) ? AYNI_OK
                                                                     : AYNI_INVALID_ARGUMENT;
}

// Accept a complete bounded group or leave both queue and ticket unchanged;
// enqueueing does not acquire units or move fairness cursors (C-RES-002/004).
ayni_status_t ResourceAdmission::enqueue(const ResourceRequest &request, uint64_t now_us) {
    const auto status = validate(request);
    if (status != AYNI_OK || now_us == no_deadline) {
        return status != AYNI_OK ? status : AYNI_INVALID_ARGUMENT;
    }
    uint32_t existing = 0;
    if (find(request.call_id, existing)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!has_room()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const Entry entry{request, last_ticket_ + 1U, now_us,
                      request.on_wait == WaitAction::wait
                          ? no_deadline
                          : foundation::deadline(now_us, request.wait_us),
                      Phase::waiting};
    // Capacity was checked under the single owner; push cannot fail (C-RES-002).
    requests_.push_back(entry);
    ++last_ticket_;
    return AYNI_OK;
}

bool ResourceAdmission::find(uint64_t call_id, uint32_t &index) const {
    for (uint32_t candidate = 0; candidate < requests_.size(); ++candidate) {
        if (requests_[candidate].request.call_id == call_id) {
            index = candidate;
            return true;
        }
    }
    return false;
}

bool ResourceAdmission::head(uint32_t index) const {
    const auto &entry = requests_[index];
    for (uint32_t other = 0; other < requests_.size(); ++other) {
        const auto &candidate = requests_[other];
        if (candidate.phase == Phase::waiting && candidate.ticket < entry.ticket &&
            candidate.request.instance_id == entry.request.instance_id &&
            candidate.request.priority == entry.request.priority) {
            return false;
        }
    }
    return true;
}

// Return the first blocking resource in canonical order. A health report is
// independent of occupancy and cannot clear unconfirmed stop (C-RES-008).
WaitReason ResourceAdmission::availability(const Entry &entry, uint32_t &index) const {
    for (uint32_t at = 0; at < entry.request.count; ++at) {
        const auto &units = entry.request.resources[at];
        const auto &state = states_[units.index];
        index = units.index;
        if (state.health == AYNI_RESOURCE_OFFLINE) {
            return WaitReason::offline;
        }
        if (state.health == AYNI_RESOURCE_FAULTED) {
            return WaitReason::faulted;
        }
        if (state.quarantine.call_id != 0U) {
            return WaitReason::quarantined;
        }
        // Subtraction avoids overflow even when capacity is UINT32_MAX.
        if (units.units > registry_.at(units.index).capacity - state.used) {
            return WaitReason::occupied;
        }
    }
    // A different resource in the domain also excludes this request. Quarantine
    // survives release and health changes, so it blocks the whole conflict domain.
    for (uint32_t at = 0; at < entry.request.count; ++at) {
        for (uint32_t other = 0; other < registry_.size(); ++other) {
            if (registry_.conflict(entry.request.resources[at].index, other) &&
                (states_[other].used != 0U || states_[other].quarantine.call_id != 0U)) {
                index = other;
                return states_[other].quarantine.call_id != 0U ? WaitReason::quarantined
                                                               : WaitReason::conflict;
            }
        }
    }
    index = no_index;
    return WaitReason::available;
}

uint32_t ResourceAdmission::expired(uint64_t now_us) const {
    uint32_t selected = no_index;
    for (uint32_t index = 0; index < requests_.size(); ++index) {
        const auto &entry = requests_[index];
        if (entry.phase == Phase::waiting && entry.deadline_us != no_deadline &&
            entry.deadline_us <= now_us &&
            (selected == no_index || entry.ticket < requests_[selected].ticket)) {
            selected = index;
        }
    }
    return selected;
}

// One round-robin cursor per priority. An unavailable Agent head may be skipped
// without letting that same Agent's later request overtake it (ADR-0042).
uint32_t ResourceAdmission::eligible(Priority priority) const {
    uint32_t after_cursor = no_index;
    uint32_t wrap = no_index;
    const auto last = last_instance_[static_cast<std::size_t>(priority)];
    for (uint32_t index = 0; index < requests_.size(); ++index) {
        const auto &entry = requests_[index];
        uint32_t resource = no_index;
        if (entry.phase != Phase::waiting || entry.request.priority != priority || !head(index) ||
            availability(entry, resource) != WaitReason::available) {
            continue;
        }
        const auto instance = entry.request.instance_id;
        if (wrap == no_index || instance < requests_[wrap].request.instance_id) {
            wrap = index;
        }
        if (instance > last &&
            (after_cursor == no_index || instance < requests_[after_cursor].request.instance_id)) {
            after_cursor = index;
        }
    }
    return after_cursor != no_index ? after_cursor : wrap;
}

AdmissionChoice ResourceAdmission::choice(uint32_t index, uint64_t now_us) const {
    const auto &entry = requests_[index];
    return {entry.request.call_id, elapsed(now_us, entry.queued_us), entry.request.on_wait,
            entry.deadline_us != no_deadline && entry.deadline_us <= now_us};
}

bool ResourceAdmission::next(uint64_t now_us, AdmissionChoice &out) const {
    const auto overdue = expired(now_us);
    if (overdue != no_index) {
        out = choice(overdue, now_us);
        return true;
    }
    // Expiry consumes no resources; lower priorities may use resources that
    // no eligible higher-priority request can use (work-conserving selection).
    for (std::size_t priority = 0; priority < last_instance_.size(); ++priority) {
        const auto selected = eligible(static_cast<Priority>(priority));
        if (selected != no_index) {
            out = choice(selected, now_us);
            return true;
        }
    }
    return false;
}

// Reselect immediately before the all-or-none change. Reusing a logical call
// still produces a different claim, fencing delayed release/stop confirmations.
bool ResourceAdmission::acquire(uint64_t call_id, uint64_t now_us, ResourceClaim &out) {
    AdmissionChoice selected{};
    uint32_t index = 0;
    if (!next(now_us, selected) || selected.expired || selected.call_id != call_id ||
        !find(call_id, index)) {
        return false;
    }
    auto &entry = requests_[index];
    for (uint32_t at = 0; at < entry.request.count; ++at) {
        const auto &units = entry.request.resources[at];
        states_[units.index].used += units.units;
    }
    entry.phase = Phase::held;
    last_instance_[static_cast<std::size_t>(entry.request.priority)] = entry.request.instance_id;
    if (selected.waited_us > longest_wait_us_) {
        longest_wait_us_ = selected.waited_us;
    }
    out = {call_id, entry.ticket};
    return true;
}

bool ResourceAdmission::remove_waiter(uint64_t call_id, uint64_t now_us) {
    uint32_t index = 0;
    if (!find(call_id, index) || requests_[index].phase != Phase::waiting) {
        return false;
    }
    const auto waited = elapsed(now_us, requests_[index].queued_us);
    if (waited > longest_wait_us_) {
        longest_wait_us_ = waited;
    }
    requests_.remove_at(index);
    return true;
}

// Return units of this exact acquisition; actuator reuse remains fenced until
// the same claim's stop fact is consumed, irrespective of logical-call retries.
bool ResourceAdmission::executable(ResourceClaim claim) const {
    uint32_t index = 0;
    if (!find(claim.call_id, index) || requests_[index].ticket != claim.generation ||
        requests_[index].phase != Phase::held) {
        return false;
    }
    const auto &request = requests_[index].request;
    for (uint32_t at = 0; at < request.count; ++at) {
        const auto &state = states_[request.resources[at].index];
        if (state.health != AYNI_RESOURCE_ONLINE || state.quarantine.call_id != 0U) {
            return false;
        }
    }
    return true;
}

bool ResourceAdmission::release(ResourceClaim claim) {
    uint32_t index = 0;
    if (!find(claim.call_id, index) || requests_[index].phase != Phase::held ||
        requests_[index].ticket != claim.generation) {
        return false;
    }
    const auto &entry = requests_[index];
    for (uint32_t at = 0; at < entry.request.count; ++at) {
        const auto &units = entry.request.resources[at];
        states_[units.index].used -= units.units;
        // End of an actuator claim does not prove physical stop (C-RES-008).
        if ((registry_.at(units.index).flags & AYNI_RESOURCE_ACTUATOR) != 0U) {
            states_[units.index].quarantine = claim;
        }
    }
    requests_.remove_at(index);
    return true;
}

bool ResourceAdmission::confirm_stopped(uint32_t index, ResourceClaim claim) {
    if (index >= registry_.size() || claim.call_id == 0U ||
        states_[index].quarantine.call_id != claim.call_id ||
        states_[index].quarantine.generation != claim.generation) {
        return false;
    }
    states_[index].quarantine = {};
    return true;
}

bool ResourceAdmission::waiting(uint64_t call_id, uint64_t now_us, WaitReason &reason,
                                uint32_t &resource_index, uint64_t &waited_us) const {
    uint32_t index = 0;
    if (!find(call_id, index) || requests_[index].phase != Phase::waiting) {
        return false;
    }
    resource_index = no_index;
    reason = head(index) ? availability(requests_[index], resource_index) : WaitReason::fifo;
    waited_us = elapsed(now_us, requests_[index].queued_us);
    return true;
}

uint64_t ResourceAdmission::next_deadline_us() const {
    uint64_t next = no_deadline;
    for (uint32_t index = 0; index < requests_.size(); ++index) {
        const auto &entry = requests_[index];
        if (entry.phase == Phase::waiting && entry.deadline_us < next) {
            next = entry.deadline_us;
        }
    }
    return next;
}

ayni_resource_queue_info_v1 ResourceAdmission::queue_info(uint64_t now_us) const {
    ayni_resource_queue_info_v1 info{sizeof(info), requests_.capacity(), 0, 0, longest_wait_us_};
    for (uint32_t at = 0; at < requests_.size(); ++at) {
        const auto &entry = requests_[at];
        if (entry.phase == Phase::held) {
            ++info.held;
        } else {
            ++info.waiting;
            const auto waited = elapsed(now_us, entry.queued_us);
            if (waited > info.longest_wait_us) {
                info.longest_wait_us = waited;
            }
        }
    }
    return info;
}

} // namespace ayni::resources

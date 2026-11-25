#include "kernel/service/reflex/reflex_table.h"

#include "catalog/abi_checks.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/value_encoding.h"
#include "foundation/integer_affine.h"
#include "foundation/little_endian.h"
#include "work/call/call_input.h"

#include <cstring>
#include <new>

namespace ayni::kernel {
namespace {

ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// Object-only paths have identical field/subtree semantics in admitted schema and value trees.
// Lists are deliberately excluded: one binding always names exactly one fixed-width scalar.
template <class Node>
uint32_t object_path(const Node *nodes, uint32_t count,
                     const ayni_reflex_param_binding_v1 &binding) {
    uint32_t index = 0;
    for (uint32_t depth = 0; depth < binding.path_count; ++depth) {
        if (nodes[index].kind != AYNI_VALUE_OBJECT) {
            return count;
        }
        uint32_t child = index + 1U;
        while (child < nodes[index].subtree_end && nodes[child].field_id != binding.path[depth]) {
            child = nodes[child].subtree_end;
        }
        if (child == nodes[index].subtree_end) {
            return count;
        }
        index = child;
    }
    return index;
}

// Reject malformed bindings before retaining caller pointers or allocating any rule slot.
ayni_status_t copy_parameters(const ayni_reflex_rule_v1 &raw, ReflexRuleEntry &out) {
    if (raw.param_binding_count > AYNI_REFLEX_PARAM_BINDING_MAX || raw.param_binding_reserved ||
        ((raw.param_binding_count == 0U) != (raw.param_bindings == nullptr)) ||
        (raw.param_binding_count && raw.action != AYNI_REFLEX_PREEMPT_CALL)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < raw.param_binding_count; ++at) {
        const auto &binding = raw.param_bindings[at];
        if (binding.condition_index >= raw.condition_count || !binding.path_count ||
            binding.path_count > AYNI_REFLEX_PARAM_PATH_MAX ||
            binding.input_min > binding.input_max || binding.scale_den <= 0 ||
            binding.output_min > binding.output_max) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t path = 0; path < AYNI_REFLEX_PARAM_PATH_MAX; ++path) {
            if ((path < binding.path_count) != (binding.path[path] != 0U)) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
        out.parameters[at].binding = binding;
    }
    out.parameter_count = raw.param_binding_count;
    return AYNI_OK;
}

// Validates rule header, trigger mode, flags, Scene pair and action-specific shape once
// before reserving a slot or encoding input bytes (ADR-0112).
ayni_status_t decode_rule_header(const ayni_reflex_rule_v1 &raw, ReflexRuleEntry &out) {
    if (const auto shape = catalog::check_struct_size(raw.struct_size, sizeof(raw), sizeof(raw));
        shape != AYNI_OK) {
        return shape;
    }
    if (raw.version == 0U || raw.reserved != 0U || raw.priority > ReflexTable::priority_max ||
        (raw.flags & ~ReflexTable::allowed_flags) != 0U || !out.rule_id.assign(raw.rule_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const bool soft_stop = raw.action == AYNI_REFLEX_SOFT_STOP_RESOURCE;
    const bool preempt_call = raw.action == AYNI_REFLEX_PREEMPT_CALL;
    if (!soft_stop && !preempt_call) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (raw.trigger_mode != AYNI_REFLEX_TRIGGER_IN_RANGE &&
        !(raw.trigger_mode == AYNI_REFLEX_TRIGGER_STALE_OR_GAP && soft_stop)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (raw.scene_id.size != 0U) {
        if (raw.scene_version == 0U || !out.scene_id.assign(raw.scene_id)) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else if (raw.scene_version != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (raw.condition_count == 0U || raw.condition_count > AYNI_REFLEX_CONDITION_COUNT_MAX ||
        raw.conditions == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < raw.condition_count; ++at) {
        const auto status = catalog::decode_condition(raw.conditions[at], out.conditions[at]);
        if (status != AYNI_OK) {
            return status;
        }
        for (uint32_t prev = 0; prev < at; ++prev) {
            if (out.conditions[prev].source.equals(view(out.conditions[at].source))) {
                return AYNI_INVALID_ARGUMENT; // Duplicate observation source in one rule.
            }
        }
    }
    if (soft_stop) {
        if (!out.resource_id.assign(raw.resource_id) || raw.owner_agent_id != 0U ||
            raw.binding_id.size != 0U || raw.binding_version != 0U || raw.input.node_count != 0U ||
            raw.input.nodes != nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else {
        if (raw.resource_id.size != 0U || raw.owner_agent_id == 0U ||
            !out.binding_id.assign(raw.binding_id) || raw.binding_version == 0U ||
            raw.input.node_count == 0U || raw.input.nodes == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    out.version = raw.version;
    out.action = raw.action;
    out.trigger_mode = raw.trigger_mode;
    out.flags = raw.flags;
    out.priority = raw.priority;
    out.cooldown_us = raw.cooldown_us;
    out.scene_version = raw.scene_version;
    out.condition_count = raw.condition_count;
    out.owner_agent_id = raw.owner_agent_id;
    out.binding_version = raw.binding_version;
    return copy_parameters(raw, out);
}

// Startup-only: each mapping has one I64 payload and an entire output interval within schema.
ayni_status_t resolve_parameters(ReflexRuleEntry &rule, const catalog::DecodedValue &value,
                                 const catalog::DecodedSchema &schema) {
    for (uint32_t at = 0; at < rule.parameter_count; ++at) {
        auto &parameter = rule.parameters[at];
        const auto &binding = parameter.binding;
        const auto value_index = object_path(value.nodes.data(), value.count, binding);
        const auto schema_index = object_path(schema.nodes.data(), schema.count, binding);
        if (value_index == value.count || schema_index == schema.count ||
            schema.nodes[schema_index].kind != AYNI_VALUE_I64 ||
            binding.output_min < schema.nodes[schema_index].min_i64 ||
            binding.output_max > schema.nodes[schema_index].max_i64 ||
            catalog::admitted_i64_offset(value.view(), value_index, parameter.byte_offset) !=
                AYNI_OK ||
            parameter.byte_offset > rule.input_size ||
            rule.input_size - parameter.byte_offset < sizeof(int64_t)) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t previous = 0; previous < at; ++previous) {
            if (rule.parameters[previous].byte_offset == parameter.byte_offset) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    return AYNI_OK;
}

} // namespace

std::size_t ReflexTable::rule_storage_size(uint32_t capacity, uint32_t input_bytes) {
    if (capacity == 0U) {
        return 0U;
    }
    // Reserve one extra input stride for in-place schema normalization at ayni_start().
    return sizeof(ReflexRuleEntry) * capacity +
           static_cast<std::size_t>(capacity + 1U) * input_bytes;
}

std::size_t ReflexTable::event_storage_size(uint32_t event_capacity) {
    return sizeof(ayni_reflex_event_v1) * event_capacity;
}

ReflexTable::ReflexTable(void *rule_storage, uint32_t rule_capacity, uint32_t input_bytes,
                         void *event_storage, uint32_t event_capacity)
    : rules_(rule_capacity == 0U ? nullptr : static_cast<ReflexRuleEntry *>(rule_storage)),
      inputs_(rule_capacity == 0U ? nullptr : reinterpret_cast<uint8_t *>(rules_ + rule_capacity)),
      events_(event_capacity == 0U ? nullptr : static_cast<ayni_reflex_event_v1 *>(event_storage)),
      rule_capacity_(rule_capacity), input_stride_(rule_capacity == 0U ? 0U : input_bytes),
      event_capacity_(event_capacity) {
    static_assert(alignof(ReflexRuleEntry) <= storage_alignment, "reflex rule arena alignment");
    static_assert(alignof(ayni_reflex_event_v1) <= storage_alignment,
                  "reflex event arena alignment");
    for (uint32_t at = 0; at < rule_capacity_; ++at) {
        new (rules_ + at) ReflexRuleEntry{};
    }
    for (uint32_t at = 0; at < event_capacity_; ++at) {
        new (events_ + at) ayni_reflex_event_v1{};
    }
}

uint8_t *ReflexTable::input_slot(uint32_t index) const {
    return inputs_ + static_cast<std::size_t>(index) * input_stride_;
}

ayni_bytes_v1 ReflexTable::input(uint32_t index) const {
    const auto size = rules_[index].input_size;
    return size == 0U ? ayni_bytes_v1{} : ayni_bytes_v1{input_slot(index), size};
}

ayni_bytes_v1
ReflexTable::projected_input(uint32_t index,
                             const std::array<int64_t, AYNI_REFLEX_CONDITION_COUNT_MAX> &values) {
    const auto &rule = rules_[index];
    if (!rule.parameter_count) {
        return input(index);
    }
    auto *scratch = input_slot(rule_capacity_);
    std::memcpy(scratch, input_slot(index), rule.input_size);
    for (uint32_t at = 0; at < rule.parameter_count; ++at) {
        const auto &parameter = rule.parameters[at];
        const auto &binding = parameter.binding;
        const auto mapped =
            foundation::map_reflex_i64(values[binding.condition_index], binding.input_min,
                                       binding.input_max, binding.scale_num, binding.scale_den,
                                       binding.offset, binding.output_min, binding.output_max);
        foundation::store_le64(scratch + parameter.byte_offset, static_cast<uint64_t>(mapped));
    }
    return {scratch, rule.input_size};
}

bool ReflexTable::find(ayni_string_v1 rule_id, uint32_t &out_index) const {
    for (uint32_t at = 0; at < rule_count_; ++at) {
        if (rules_[at].rule_id.equals(rule_id)) {
            out_index = at;
            return true;
        }
    }
    return false;
}

// Copies a validated rule and its raw PREEMPT_CALL value tree into the arena.
// Duplicate rule IDs fail before capacity exhaustion so caller mistakes are explicit.
ayni_status_t ReflexTable::add_rule(const ayni_reflex_rule_v1 &raw) {
    ReflexRuleEntry entry{};
    if (const auto status = decode_rule_header(raw, entry); status != AYNI_OK) {
        return status;
    }
    uint32_t duplicate = 0;
    if (find(view(entry.rule_id), duplicate)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (rule_count_ >= rule_capacity_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (entry.action == AYNI_REFLEX_PREEMPT_CALL) {
        uint32_t encoded_size = 0;
        const auto encoded =
            catalog::encode_value(raw.input, input_slot(rule_count_), input_stride_, encoded_size);
        if (encoded != AYNI_OK) {
            return encoded == AYNI_BUFFER_TOO_SMALL ? AYNI_CAPACITY_EXCEEDED : encoded;
        }
        // Reflex inputs are static startup constants and cannot retain host body references.
        ayni_body_ref_v1 body_probe{};
        if (catalog::value_bodies({input_slot(rule_count_), encoded_size}, &body_probe, 1U) != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        entry.input_size = encoded_size;
    }
    rules_[rule_count_] = entry;
    order_[rule_count_] = rule_count_;
    ++rule_count_;
    return AYNI_OK;
}

// Normalizes and re-encodes one PREEMPT_CALL rule's value against its resolved business contract,
// verifying physical envelope validity if the binding carries CALL_EFFECT_PHYSICAL (ADR-0112).
ayni_status_t ReflexTable::normalize_rule_input(uint32_t index, foundation::Workspace &workspace,
                                                const Catalogs &catalogs,
                                                const catalog::BindingLinks &links) {
    auto &rule = rules_[index];
    const auto &contract = links.contract;
    auto raw_lease = workspace.make<catalog::DecodedValue>();
    auto proof_lease = workspace.make<catalog::DecodedBusiness>();
    if (!raw_lease || !proof_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto status = catalog::decode_value(workspace, input(index), *raw_lease);
    if (status == AYNI_OK) {
        status =
            catalog::read_admitted_business(workspace, contract.business_contract, *proof_lease);
    }
    if (status != AYNI_OK) {
        return status;
    }
    uint8_t *scratch = input_slot(rule_capacity_);
    uint32_t normalized_size = 0;
    status =
        work::encode_business_input_with_proof(workspace, *proof_lease, contract, raw_lease->view(),
                                               scratch, input_stride_, normalized_size);
    if (status != AYNI_OK) {
        std::memset(scratch, 0, input_stride_);
        return status;
    }
    std::memcpy(input_slot(index), scratch, normalized_size);
    std::memset(input_slot(index) + normalized_size, 0, input_stride_ - normalized_size);
    std::memset(scratch, 0, input_stride_);
    rule.input_size = normalized_size;
    ayni_body_ref_v1 body_probe{};
    if (catalog::value_bodies(input(index), &body_probe, 1U) != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (rule.parameter_count != 0U) {
        // Offsets describe the canonical bytes after normalization, including defaults.
        status = catalog::decode_value(workspace, input(index), *raw_lease);
        if (status == AYNI_OK) {
            status = resolve_parameters(rule, *raw_lease, proof_lease->input);
        }
        if (status != AYNI_OK) {
            return status;
        }
    }
    if ((contract.effects & catalog::call_effect_physical) != 0U) {
        uint32_t exec_idx = 0;
        if (!catalogs.execution.find_binding(view(rule.binding_id), rule.binding_version,
                                             exec_idx)) {
            return AYNI_UNSUPPORTED;
        }
        auto view_lease = workspace.make<work::CallInputView>();
        if (!view_lease) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const auto domain = catalogs.capabilities.domain_view(links.capability);
        if (work::read_call_input_with_proof(workspace, *proof_lease, domain, input(index),
                                             contract, *view_lease) != AYNI_OK) {
            return AYNI_INVALID_ARGUMENT;
        }
        ayni_control_envelope_v1 envelope{};
        if (catalog::execution_envelope_for_value(catalogs.execution.binding(exec_idx),
                                                  proof_lease->view(), view_lease->value.view(),
                                                  view_lease->arguments, envelope) != AYNI_OK) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return AYNI_OK;
}

// Resolves all catalog, Scene, resource, agent and binding references for rule index at ayni_start.
ayni_status_t ReflexTable::validate_rule_links(uint32_t index, foundation::Workspace &workspace,
                                               const Catalogs &catalogs,
                                               const work::AgentTable &agents,
                                               const authority::Scenes &scenes) {
    auto &rule = rules_[index];
    for (uint32_t at = 0; at < rule.condition_count; ++at) {
        const auto &cond = rule.conditions[at];
        uint32_t source_index = 0;
        if (!catalogs.observation_sources.find(view(cond.source), source_index)) {
            return AYNI_NOT_FOUND;
        }
        const auto &source = catalogs.observation_sources.at(source_index);
        if (source.version != cond.version) {
            return AYNI_UNSUPPORTED_VERSION;
        }
        // Only trusted MEASURED sources may drive local safety/control reflexes (ADR-0112).
        if (source.origin != AYNI_OBSERVATION_MEASURED ||
            (source.kind == AYNI_OBSERVATION_BOOL && (cond.minimum < 0 || cond.maximum > 1))) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    if (rule.scene_id.size() != 0U) {
        uint32_t scene_index = 0;
        if (!scenes.find_definition(view(rule.scene_id), scene_index)) {
            return AYNI_NOT_FOUND;
        }
        if (scenes.definition(scene_index).version != rule.scene_version) {
            return AYNI_UNSUPPORTED_VERSION;
        }
        rule.scene_index = scene_index;
    }
    if (rule.action == AYNI_REFLEX_SOFT_STOP_RESOURCE) {
        uint32_t resource_index = 0;
        if (!catalogs.resources.find(view(rule.resource_id), resource_index)) {
            return AYNI_NOT_FOUND;
        }
        rule.resource_index = resource_index;
        return AYNI_OK;
    }
    if (agents.find_agent(rule.owner_agent_id) == nullptr) {
        return AYNI_NOT_FOUND;
    }
    uint32_t binding_index = 0;
    if (!catalogs.bindings.find(view(rule.binding_id), rule.binding_version, binding_index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &binding = catalogs.bindings.at(binding_index);
    catalog::BindingLinks links{};
    const auto resolved = catalog::BindingRegistry::resolve(
        binding, catalogs.capabilities, catalogs.call_capabilities, catalogs.providers,
        catalogs.resources, links);
    if (resolved != AYNI_OK) {
        return resolved;
    }
    if (const auto dispatchable = catalogs.dispatchable(binding, links); dispatchable != AYNI_OK) {
        return dispatchable;
    }
    if (!links.contract.is_ephemeral() || links.contract.subscription.has_value() ||
        links.contract.category != catalog::CallClass::tool) {
        return AYNI_INVALID_ARGUMENT;
    }
    rule.binding_index = binding_index;
    return normalize_rule_input(index, workspace, catalogs, links);
}

void ReflexTable::sort_priority_order() {
    for (uint32_t at = 0; at < rule_count_; ++at) {
        order_[at] = at;
    }
    // Stable insertion sort: descending priority, preserving registration order on ties.
    for (uint32_t i = 1; i < rule_count_; ++i) {
        const uint32_t key = order_[i];
        uint32_t j = i;
        while (j > 0U && rules_[order_[j - 1U]].priority < rules_[key].priority) {
            order_[j] = order_[j - 1U];
            --j;
        }
        order_[j] = key;
    }
}

ayni_status_t ReflexTable::validate_all(foundation::Workspace &workspace, const Catalogs &catalogs,
                                        const work::AgentTable &agents,
                                        const authority::Scenes &scenes) {
    for (uint32_t at = 0; at < rule_count_; ++at) {
        if (const auto status = validate_rule_links(at, workspace, catalogs, agents, scenes);
            status != AYNI_OK) {
            return status;
        }
    }
    sort_priority_order();
    return AYNI_OK;
}

void ReflexTable::begin_boot() {
    for (uint32_t at = 0; at < rule_count_; ++at) {
        auto &rule = rules_[at];
        rule.enabled = (rule.flags & AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT) != 0U;
        rule.latched_active = false;
        rule.has_fired = false;
        rule.last_fired_us = 0;
        rule.fire_count = 0;
        rule.denied_count = 0;
        rule.active_call_id = 0;
    }
    event_head_ = 0;
    event_count_ = 0;
    event_sequence_ = 0;
    dropped_total_ = 0;
}

ayni_status_t ReflexTable::set_enabled(ayni_string_v1 rule_id, uint32_t enabled) {
    if (enabled > 1U) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t index = 0;
    if (!find(rule_id, index)) {
        return AYNI_NOT_FOUND;
    }
    rules_[index].enabled = enabled != 0U;
    if (!rules_[index].enabled) {
        rules_[index].latched_active = false; // Re-enabling starts cleanly on the next edge.
    }
    return AYNI_OK;
}

ayni_status_t ReflexTable::info(ayni_string_v1 rule_id, ayni_reflex_info_v1 &out) const {
    uint32_t index = 0;
    if (!find(rule_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &rule = rules_[index];
    out = {sizeof(out),        rule.version,           rule.action,
           rule.trigger_mode,  rule.enabled ? 1U : 0U, rule.latched_active ? 1U : 0U,
           rule.last_fired_us, rule.fire_count,        rule.denied_count,
           rule.active_call_id};
    return AYNI_OK;
}

void ReflexTable::push_event(ayni_reflex_event_v1 event) {
    if (event_capacity_ == 0U) {
        return;
    }
    event.struct_size = sizeof(event);
    event.reserved = 0;
    event.sequence = ++event_sequence_;
    if (event_count_ == event_capacity_) {
        event_head_ = (event_head_ + 1U) % event_capacity_;
        --event_count_;
        ++dropped_total_;
    }
    event.dropped_before = dropped_total_;
    events_[(event_head_ + event_count_++) % event_capacity_] = event;
}

ayni_status_t ReflexTable::take_event(ayni_reflex_event_v1 &out) {
    if (event_count_ == 0U) {
        return AYNI_NOT_FOUND;
    }
    out = events_[event_head_];
    events_[event_head_] = {};
    event_head_ = (event_head_ + 1U) % event_capacity_;
    --event_count_;
    return AYNI_OK;
}

} // namespace ayni::kernel

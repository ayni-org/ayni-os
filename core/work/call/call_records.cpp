#include "work/call/call_records.h"
#include "catalog/capability/call_contract_encoding.h"

#include "work/action/action_records.h"

#include <array>

namespace ayni::work {
namespace {
void write_id(const catalog::CatalogId &id, foundation::BodyWriter &writer) {
    writer.bytes(id.data(), id.size());
}

bool read_id(foundation::BodyReader &reader, catalog::CatalogId &id) {
    uint32_t size = 0;
    const auto *bytes = reader.bytes(catalog::CatalogId::max_size, size);
    return size == 0U || id.assign({reinterpret_cast<const char *>(bytes), size});
}

// Finite subscription facts never store a monotonic lease deadline.
void write_subscription(const CallRecord &record, foundation::BodyWriter &writer) {
    const auto &spec = *record.contract.subscription;
    const auto &fact = *record.state.subscription;
    write_id(spec.source, writer);
    writer.u32(spec.source_version);
    writer.u32(spec.lease_ms);
    writer.u32(static_cast<uint32_t>(fact.phase));
    writer.u32(static_cast<uint32_t>(fact.desired));
    writer.u32(static_cast<uint32_t>(fact.attachment));
    writer.u64(fact.generation);
    writer.u64(fact.lease_sequence);
    writer.u32(static_cast<uint32_t>(fact.control_status));
    writer.u32(fact.control_reason);
}
bool read_subscription(foundation::BodyReader &reader, CallRecord &record) {
    catalog::SubscriptionContract spec{};
    SubscriptionFact fact{};
    if (!read_id(reader, spec.source)) {
        return false;
    }
    spec.source_version = reader.u32();
    spec.lease_ms = reader.u32();
    fact.phase = static_cast<SubscriptionPhase>(reader.u32());
    fact.desired = static_cast<SubscriptionDesired>(reader.u32());
    fact.attachment = static_cast<SubscriptionAttachment>(reader.u32());
    fact.generation = reader.u64();
    fact.lease_sequence = reader.u64();
    fact.control_status = static_cast<ayni_status_t>(reader.u32());
    fact.control_reason = reader.u32();
    record.contract.subscription = spec;
    record.state.subscription = fact;
    return reader.valid();
}
void write_state(const CallState &state, foundation::BodyWriter &writer) {
    write_action_stamp(state.created, writer);
    write_action_stamp(state.latest, writer);
    writer.u64(state.attempt.boot_epoch);
    writer.u64(state.attempt.request_id);
    writer.u64(state.progress_sequence);
    writer.u32(static_cast<uint32_t>(state.cancel_cause));
    writer.u32(state.cancel_result);
    writer.u32(state.outcome);
    writer.u32(static_cast<uint32_t>(state.status));
    writer.u32(state.reason);
    const auto query = state.query.value_or(CallQuery{});
    writer.u32(query.generation);
    writer.u32(static_cast<uint32_t>(query.phase));
    write_action_stamp(query.requested, writer);
    writer.u64(query.attempt.boot_epoch);
    writer.u64(query.attempt.request_id);
    writer.u32(static_cast<uint32_t>(query.status));
    writer.u32(query.reason);
    writer.u32(query.conclusion);
    writer.u32(state.model_usage.kind);
    writer.u32(state.model_usage.reserved);
    writer.u64(state.model_usage.input_tokens);
    writer.u64(state.model_usage.output_tokens);
    writer.u64(state.model_usage.total_tokens);
}

CallState read_state(foundation::BodyReader &reader) {
    CallState state{};
    state.created = read_action_stamp(reader);
    state.latest = read_action_stamp(reader);
    state.attempt.boot_epoch = reader.u64();
    state.attempt.request_id = reader.u64();
    state.progress_sequence = reader.u64();
    state.cancel_cause = static_cast<CallCancelCause>(reader.u32());
    state.cancel_result = reader.u32();
    state.outcome = reader.u32();
    state.status = static_cast<ayni_status_t>(reader.u32());
    state.reason = reader.u32();
    CallQuery query{};
    query.generation = reader.u32();
    query.phase = static_cast<CallQuery::Phase>(reader.u32());
    query.requested = read_action_stamp(reader);
    query.attempt = {reader.u64(), reader.u64()};
    query.status = static_cast<ayni_status_t>(reader.u32());
    query.reason = reader.u32();
    query.conclusion = reader.u32();
    state.model_usage = {reader.u32(), reader.u32(), reader.u64(), reader.u64(), reader.u64()};
    // Absent optional values have one all-zero encoding; noncanonical tails
    // become an invalid present value for the shared intrinsic validator.
    if (query.generation != 0U || static_cast<uint32_t>(query.phase) != 0U ||
        query.requested.boot_epoch != 0U || query.requested.transition.transition_id != 0U ||
        query.requested.transition.commit_seq != 0U || query.attempt.boot_epoch != 0U ||
        query.attempt.request_id != 0U || query.status != AYNI_OK ||
        query.reason != AYNI_REASON_NONE || query.conclusion != 0U) {
        state.query = query;
    }
    return state;
}
} // namespace

uint32_t call_record_max(CallTable::Limits limits) {
    return call_record_fixed_size + call_record_id_count * catalog::CatalogId::max_size +
           catalog::resource_group_max *
               (call_record_resource_fixed_size + catalog::CatalogId::max_size) +
           action_state_record_max_size + limits.domain_bytes + limits.input_bytes +
           limits.result_bytes + limits.business_bytes + execution_record_max() +
           call_subscription_fixed_bytes + catalog::CatalogId::max_size;
}

uint32_t call_record_size(const CallRecord &record) {
    const auto &id = record.identity;
    uint32_t size = call_record_fixed_size + id.step_key.size() + id.binding_id.size() +
                    id.capability_id.size() + id.provider_id.size() + id.endpoint.size() +
                    id.destination.size() + record.domain.size + record.input.size +
                    record.result.size + record.contract.business_contract.size;
    for (uint32_t at = 0; at < record.contract.resource_count; ++at) {
        size += call_record_resource_fixed_size + record.contract.resources[at].id.size();
    }
    return size +
           (record.state.subscription
                ? call_subscription_fixed_bytes + record.contract.subscription->source.size()
                : 0) +
           (record.action ? action_state_record_size(*record.action) : 0U) +
           (record.state.execution ? execution_record_size(*record.state.execution) : 0U);
}

// The enclosing call owns the original action association; the action body uses
// its existing codec, so standalone inspection and embedded storage cannot drift.
void write_call_record(const CallRecord &record, uint8_t *body) {
    foundation::BodyWriter writer(body, call_record_size(record));
    const auto &id = record.identity;
    writer.u64(id.call_id);
    writer.u64(id.operation_id);
    writer.u64(id.task_id);
    writer.u64(id.owner_instance_id);
    writer.u32(id.binding_version);
    writer.u32(id.capability_version);
    writer.u32(id.provider_version);
    write_id(id.step_key, writer);
    write_id(id.binding_id, writer);
    write_id(id.capability_id, writer);
    write_id(id.provider_id, writer);
    write_id(id.endpoint, writer);
    write_id(id.destination, writer);
    catalog::write_execution_contract(record.contract, writer);
    write_state(record.state, writer);
    writer.bytes(record.domain.data, record.domain.size);
    writer.bytes(record.input.data, record.input.size);
    writer.bytes(record.result.data, record.result.size);
    std::array<uint8_t, action_state_record_max_size> action{};
    const uint32_t action_size = record.action ? action_state_record_size(*record.action) : 0U;
    if (record.action) {
        write_action_state_record(*record.action, action.data());
    }
    writer.bytes(action.data(), action_size);
    writer.u32(record.state.execution ? 1U : 0U);
    writer.u32(record.state.content_erased ? 1U : 0U);
    write_action_stamp(record.state.content_erased.value_or(ActionStamp{}), writer);
    writer.u32(static_cast<uint32_t>(record.result_kind));
    if (record.state.execution) {
        write_execution(*record.state.execution, writer);
    }
    if (record.state.subscription) {
        write_subscription(record, writer);
    }
    writer.bytes(record.contract.business_contract.data, record.contract.business_contract.size);
    writer.u64(record.identity.authorization.grant_id);
    writer.u32(record.identity.authorization.revision);
    writer.u64(record.identity.model_context.boot_epoch);
    for (const auto word : record.identity.model_context.visible) {
        writer.u64(word);
    }
}

// This reader does not consult this boot's catalog. Old input is interpreted
// only with its saved domain; replay separately proves Task and stamp relations.
bool read_call_record(const uint8_t *body, uint32_t size, CallRecord &out) {
    if (body == nullptr || size < call_record_fixed_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    CallRecord record{};
    auto &id = record.identity;
    id.call_id = reader.u64();
    id.operation_id = reader.u64();
    id.task_id = reader.u64();
    id.owner_instance_id = reader.u64();
    id.binding_version = reader.u32();
    id.capability_version = reader.u32();
    id.provider_version = reader.u32();
    if (!read_id(reader, id.step_key) || !read_id(reader, id.binding_id) ||
        !read_id(reader, id.capability_id) || !read_id(reader, id.provider_id) ||
        !read_id(reader, id.endpoint) || !read_id(reader, id.destination) ||
        !catalog::read_execution_contract(reader, record.contract)) {
        return false;
    }
    record.state = read_state(reader);
    record.domain.data = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, record.domain.size);
    record.input.data = reader.bytes(AYNI_CALL_INPUT_BYTES_MAX, record.input.size);
    record.result.data = reinterpret_cast<const char *>(
        reader.bytes(AYNI_CALL_RESULT_BYTES_MAX, record.result.size));
    uint32_t action_size = 0;
    const auto *action = reader.bytes(action_state_record_max_size, action_size);
    if (action_size != 0U) {
        ActionFact fact{};
        if (!read_action_state_record(action, action_size, fact)) {
            return false;
        }
        record.action = fact;
    }
    const auto has_execution = reader.u32();
    const auto erased = reader.u32();
    const auto erasure_stamp = read_action_stamp(reader);
    record.result_kind = static_cast<CallResultKind>(reader.u32());
    if (has_execution > 1 || erased > 1 ||
        (erased == 0 &&
         (erasure_stamp.boot_epoch != 0 || erasure_stamp.transition.transition_id != 0 ||
          erasure_stamp.transition.commit_seq != 0))) {
        return false;
    }
    if (erased != 0) {
        record.state.content_erased = erasure_stamp;
    }
    if (has_execution != 0) {
        ExecutionFact execution{};
        if (!read_execution(reader, execution)) {
            return false;
        }
        record.state.execution = execution;
    }
    if (record.contract.form == catalog::CallForm::subscription &&
        !read_subscription(reader, record)) {
        return false;
    }
    record.contract.business_contract = reader.bytes(AYNI_BUSINESS_CONTRACT_BYTES_MAX);
    record.identity.authorization = {reader.u64(), reader.u32()};
    record.identity.model_context.boot_epoch = reader.u64();
    for (auto &word : record.identity.model_context.visible) {
        word = reader.u64();
    }
    if (!reader.ok() || !CallTable::valid_record(record)) {
        return false;
    }
    out = record;
    return true;
}
} // namespace ayni::work

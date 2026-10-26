#include "work/call/call_records.h"

#include "work/action/action_records.h"

#include <array>

namespace ayni::work {
namespace {
void write_id(const catalog::CatalogId &id, durability::BodyWriter &writer) {
    writer.bytes(id.data(), id.size());
}

bool read_id(durability::BodyReader &reader, catalog::CatalogId &id) {
    uint32_t size = 0;
    const auto *bytes = reader.bytes(catalog::CatalogId::max_size, size);
    return size == 0U || id.assign({reinterpret_cast<const char *>(bytes), size});
}

// Contract field order is representation v1, independent of native ABI layout.
void write_contract(const catalog::CallContract &contract, durability::BodyWriter &writer) {
    writer.u32(static_cast<uint32_t>(contract.form));
    writer.u32(static_cast<uint32_t>(contract.category));
    writer.u32(contract.effects);
    writer.u32(static_cast<uint32_t>(contract.idempotency));
    writer.u32(static_cast<uint32_t>(contract.query));
    writer.u32(static_cast<uint32_t>(contract.cancel));
    writer.u32(contract.dedupe_window_ms);
    writer.u32(contract.timeout_ms);
    writer.u32(contract.input_format);
    writer.u32(contract.result_format);
    writer.u32(contract.input_text_limit);
    writer.u32(contract.result_text_limit);
    writer.u32(contract.resource_count);
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        write_id(contract.resources[at].id, writer);
        writer.u32(contract.resources[at].units);
    }
}

// Bounds are checked before indexing; semantic validation is shared with registration.
bool read_contract(durability::BodyReader &reader, catalog::CallContract &contract) {
    contract.form = static_cast<catalog::CallForm>(reader.u32());
    contract.category = static_cast<catalog::CallClass>(reader.u32());
    contract.effects = reader.u32();
    contract.idempotency = static_cast<catalog::CallIdempotency>(reader.u32());
    contract.query = static_cast<catalog::QueryAuthority>(reader.u32());
    contract.cancel = static_cast<catalog::CancelMode>(reader.u32());
    contract.dedupe_window_ms = reader.u32();
    contract.timeout_ms = reader.u32();
    contract.input_format = reader.u32();
    contract.result_format = reader.u32();
    contract.input_text_limit = reader.u32();
    contract.result_text_limit = reader.u32();
    contract.resource_count = reader.u32();
    if (contract.resource_count > catalog::resource_group_max) {
        return false;
    }
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        if (!read_id(reader, contract.resources[at].id)) {
            return false;
        }
        contract.resources[at].units = reader.u32();
    }
    return true;
}

void write_state(const CallState &state, durability::BodyWriter &writer) {
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
}

CallState read_state(durability::BodyReader &reader) {
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
           limits.result_bytes + execution_record_max(); // A v4 kind never joins execution.
}

uint32_t call_record_size(const CallRecord &record) {
    const auto &id = record.identity;
    uint32_t size = call_record_fixed_size + id.step_key.size() + id.binding_id.size() +
                    id.capability_id.size() + id.provider_id.size() + id.endpoint.size() +
                    id.destination.size() + record.domain.size + record.input.size +
                    record.result.size;
    for (uint32_t at = 0; at < record.contract.resource_count; ++at) {
        size += call_record_resource_fixed_size + record.contract.resources[at].id.size();
    }
    return size + (record.action ? action_state_record_size(*record.action) : 0U) +
           (record.state.execution ? execution_record_size(*record.state.execution) : 0U) +
           (call_record_version_for(record) == call_tool_calls_record_version
                ? call_result_kind_size
                : 0U);
}

// The enclosing call owns the original action association; the action body uses
// its existing codec, so standalone inspection and embedded storage cannot drift.
void write_call_record(const CallRecord &record, uint8_t *body) {
    durability::BodyWriter writer(body, call_record_size(record));
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
    write_contract(record.contract, writer);
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
    if (record.state.execution) {
        write_execution(*record.state.execution, writer);
    }
    if (call_record_version_for(record) == call_tool_calls_record_version) {
        writer.u32(static_cast<uint32_t>(record.result_kind));
    }
}

uint16_t call_record_version_for(const CallRecord &record) {
    if (record.state.execution) {
        return call_execution_record_version;
    }
    if (record.contract.result_format == catalog::call_tool_calls_result_format) {
        return call_tool_calls_record_version;
    }
    return record.state.outcome == AYNI_CALL_SKIPPED ? call_skipped_record_version
                                                     : call_record_version;
}

// This reader does not consult this boot's catalog. Old input is interpreted
// only with its saved domain; replay separately proves Task and stamp relations.
bool read_call_record(const uint8_t *body, uint32_t size, CallRecord &out, uint16_t version) {
    if (body == nullptr || size < call_record_fixed_size ||
        (version != call_record_version && version != call_skipped_record_version &&
         version != call_execution_record_version && version != call_tool_calls_record_version)) {
        return false;
    }
    durability::BodyReader reader(body, size);
    CallRecord record{};
    record.result_kind = CallResultKind::text;
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
        !read_contract(reader, record.contract)) {
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
    if (version == call_execution_record_version) {
        ExecutionFact execution{};
        if (!read_execution(reader, execution)) {
            return false;
        }
        record.state.execution = execution;
    }
    if (version == call_tool_calls_record_version) {
        record.result_kind = static_cast<CallResultKind>(reader.u32());
    }
    if (!reader.ok() || version != call_record_version_for(record) ||
        !CallTable::valid_record(record)) {
        return false;
    }
    out = record;
    return true;
}
} // namespace ayni::work

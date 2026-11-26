#include "work/operation/operation_records.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/call_contract_encoding.h"
#include "work/action/action_records.h"
#include <array>
namespace ayni::work {
namespace {
void write_id(const catalog::CatalogId &id, foundation::BodyWriter &writer) {
    writer.bytes(id.data(), id.size());
}
bool read_id(foundation::BodyReader &reader, catalog::CatalogId &id) {
    const auto value = reader.text(catalog::CatalogId::max_size);
    return reader.valid() && (value.size == 0 || id.assign(value));
}
void write_identity(const OperationIdentity &id, foundation::BodyWriter &writer) {
    writer.u64(id.call_id);
    writer.u64(id.operation_id);
    writer.u64(id.task_id);
    writer.u64(id.owner_instance_id);
    writer.u32(id.binding_version);
    writer.u32(id.capability_version);
    writer.u32(id.provider_version);
    write_id(id.binding_id, writer);
    write_id(id.capability_id, writer);
    write_id(id.provider_id, writer);
    write_id(id.endpoint, writer);
    write_id(id.destination, writer);
}
bool read_identity(foundation::BodyReader &reader, OperationIdentity &id) {
    id.call_id = reader.u64();
    id.operation_id = reader.u64();
    id.task_id = reader.u64();
    id.owner_instance_id = reader.u64();
    id.binding_version = reader.u32();
    id.capability_version = reader.u32();
    id.provider_version = reader.u32();
    return read_id(reader, id.binding_id) && read_id(reader, id.capability_id) &&
           read_id(reader, id.provider_id) && read_id(reader, id.endpoint) &&
           read_id(reader, id.destination);
}
void write_query(const CallQuery &query, foundation::BodyWriter &writer) {
    writer.u32(query.generation);
    writer.u32(static_cast<uint32_t>(query.phase));
    write_action_stamp(query.requested, writer);
    writer.u64(query.attempt.boot_epoch);
    writer.u64(query.attempt.request_id);
    writer.u32(static_cast<uint32_t>(query.status));
    writer.u32(query.reason);
    writer.u32(query.conclusion);
}
CallQuery read_query(foundation::BodyReader &reader) {
    CallQuery query{};
    query.generation = reader.u32();
    query.phase = static_cast<CallQuery::Phase>(reader.u32());
    query.requested = read_action_stamp(reader);
    query.attempt = {reader.u64(), reader.u64()};
    query.status = static_cast<ayni_status_t>(reader.u32());
    query.reason = reader.u32();
    query.conclusion = reader.u32();
    return query;
}
void write_options(const OperationRecord &record, foundation::BodyWriter &writer) {
    writer.u32(record.state.execution_stop ? 1U : 0U);
    if (record.state.execution_stop) {
        const auto &stop = *record.state.execution_stop;
        writer.u32(stop.phase);
        writer.u32(stop.stop_cause);
        writer.u32(stop.target);
    }
    writer.u32(record.state.query ? 1U : 0U);
    if (record.state.query) {
        write_query(*record.state.query, writer);
    }
    if (record.contract.subscription) {
        const auto &sub = *record.contract.subscription;
        write_id(sub.source, writer);
        writer.u32(sub.source_version);
        writer.u32(sub.lease_ms);
    }
}
bool read_options(foundation::BodyReader &reader, OperationRecord &record) {
    const auto stop = reader.u32();
    if (stop > 1) {
        return false;
    }
    if (stop) {
        record.state.execution_stop =
            ExecutionStopSummary{reader.u32(), reader.u32(), reader.u32()};
    }
    const auto query = reader.u32();
    if (query > 1) {
        return false;
    }
    if (query) {
        record.state.query = read_query(reader);
    }
    if (record.contract.form == catalog::CallForm::subscription) {
        catalog::SubscriptionContract sub{};
        if (!read_id(reader, sub.source)) {
            return false;
        }
        sub.source_version = reader.u32();
        sub.lease_ms = reader.u32();
        record.contract.subscription = sub;
    }
    return reader.valid();
}
} // namespace
uint32_t operation_record_max(OperationLedger::Limits limits) {
    constexpr uint32_t route_id_count = 5;
    return operation_record_fixed_bytes + route_id_count * catalog::CatalogId::max_size +
           catalog::resource_group_max *
               (foundation::string_field_size(catalog::CatalogId::max_size) +
                static_cast<uint32_t>(sizeof(uint32_t))) +
           limits.business_bytes + action_state_record_max_size + operation_stop_bytes +
           operation_query_bytes + operation_subscription_bytes + catalog::CatalogId::max_size;
}
uint32_t operation_record_size(const OperationRecord &record) {
    const auto &id = record.identity;
    return operation_record_fixed_bytes + id.binding_id.size() + id.capability_id.size() +
           id.provider_id.size() + id.endpoint.size() + id.destination.size() +
           catalog::execution_contract_size(record.contract) -
           catalog::execution_contract_fixed_bytes + record.contract.business_contract.size +
           action_state_record_size(record.action) +
           (record.state.execution_stop ? operation_stop_bytes : 0U) +
           (record.state.query ? operation_query_bytes : 0U) +
           (record.contract.subscription
                ? operation_subscription_bytes + record.contract.subscription->source.size()
                : 0U);
}
// Reuses catalog's complete execution codec and ActionRecord's sole fact codec.
void write_operation_record(const OperationRecord &record, uint8_t *body) {
    foundation::BodyWriter writer(body, operation_record_size(record));
    write_identity(record.identity, writer);
    catalog::write_execution_contract(record.contract, writer);
    const auto &state = record.state;
    write_action_stamp(state.created, writer);
    write_action_stamp(state.archived, writer);
    write_action_stamp(state.latest, writer);
    writer.u64(state.attempt.boot_epoch);
    writer.u64(state.attempt.request_id);
    writer.u32(state.outcome);
    writer.u32(static_cast<uint32_t>(state.status));
    writer.u32(state.reason);
    writer.u32(state.revision);
    writer.bytes(record.contract.business_contract.data, record.contract.business_contract.size);
    std::array<uint8_t, action_state_record_max_size> action{};
    write_action_state_record(record.action, action.data());
    writer.bytes(action.data(), action_state_record_size(record.action));
    write_options(record, writer);
}
// All framing/optional markers are canonical; failures preserve the caller's previous value.
bool read_operation_record(foundation::Workspace &workspace, const uint8_t *body, uint32_t size,
                           OperationRecord &out) {
    if (!body || size < operation_record_fixed_bytes) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    OperationRecord record{};
    if (!read_identity(reader, record.identity) ||
        !catalog::read_execution_contract(reader, record.contract)) {
        return false;
    }
    auto &state = record.state;
    state.created = read_action_stamp(reader);
    state.archived = read_action_stamp(reader);
    state.latest = read_action_stamp(reader);
    state.attempt = {reader.u64(), reader.u64()};
    state.outcome = reader.u32();
    state.status = static_cast<ayni_status_t>(reader.u32());
    state.reason = reader.u32();
    state.revision = reader.u32();
    record.contract.business_contract = reader.bytes(AYNI_BUSINESS_CONTRACT_BYTES_MAX);
    const auto action = reader.bytes(action_state_record_max_size);
    if (!read_action_state_record(action.data, action.size, record.action) ||
        !read_options(reader, record) || !reader.ok() || !OperationLedger::valid_record(record)) {
        return false;
    }
    auto decoded = workspace.make<catalog::DecodedBusiness>();
    if (!decoded ||
        catalog::decode_business_proof(workspace, record.contract.business_contract, *decoded) !=
            AYNI_OK ||
        !catalog::bound_call_contract(decoded->execution, record.contract)) {
        return false;
    }
    out = record;
    return true;
}
void write_operation_change(const OperationChange &change, uint8_t *body) {
    foundation::BodyWriter writer(body, operation_state_bytes);
    writer.u64(change.call_id);
    writer.u32(static_cast<uint32_t>(change.kind));
    writer.u32(change.revision);
    writer.u32(change.generation);
    writer.u32(change.outcome);
    writer.u32(static_cast<uint32_t>(change.status));
    writer.u32(change.reason);
    writer.u32(static_cast<uint32_t>(change.effect));
    writer.u32(change.conclusion);
    writer.u32(0); // records.md: reserved word, canonical zero.
    writer.u64(change.attempt.boot_epoch);
    writer.u64(change.attempt.request_id);
    write_action_stamp(change.stamp, writer);
}
bool read_operation_change(const uint8_t *body, uint32_t size, OperationChange &out) {
    if (!body || size != operation_state_bytes) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    OperationChange change{};
    change.call_id = reader.u64();
    change.kind = static_cast<OperationChange::Kind>(reader.u32());
    change.revision = reader.u32();
    change.generation = reader.u32();
    change.outcome = reader.u32();
    change.status = static_cast<ayni_status_t>(reader.u32());
    change.reason = reader.u32();
    change.effect = static_cast<ActionEffect>(reader.u32());
    change.conclusion = reader.u32();
    const auto reserved = reader.u32();
    change.attempt = {reader.u64(), reader.u64()};
    change.stamp = read_action_stamp(reader);
    if (!reader.ok() || reserved || change.call_id == 0 || change.revision == 0 ||
        change.kind < OperationChange::Kind::query_requested ||
        change.kind > OperationChange::Kind::acknowledged || change.effect < ActionEffect::none ||
        change.effect > ActionEffect::unknown || !change.stamp.boot_epoch ||
        !change.stamp.transition.commit_seq || !change.stamp.transition.transition_id) {
        return false;
    }
    out = change;
    return true;
}
} // namespace ayni::work

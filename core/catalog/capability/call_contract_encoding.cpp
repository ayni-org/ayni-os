#include "catalog/capability/call_contract_encoding.h"

namespace ayni::catalog {
namespace {
void write_id(const CatalogId &id, foundation::BodyWriter &writer) {
    writer.bytes(id.data(), id.size());
}
bool read_id(foundation::BodyReader &reader, CatalogId &id) {
    uint32_t size = 0;
    const auto *bytes = reader.bytes(CatalogId::max_size, size);
    return size != 0U && id.assign({reinterpret_cast<const char *>(bytes), size});
}
} // namespace

// Contract field order is representation v1, independent of native ABI layout.
void write_execution_contract(const CallContract &contract, foundation::BodyWriter &writer) {
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
bool read_execution_contract(foundation::BodyReader &reader, CallContract &contract) {
    contract.form = static_cast<CallForm>(reader.u32());
    contract.category = static_cast<CallClass>(reader.u32());
    contract.effects = reader.u32();
    contract.idempotency = static_cast<CallIdempotency>(reader.u32());
    contract.query = static_cast<QueryAuthority>(reader.u32());
    contract.cancel = static_cast<CancelMode>(reader.u32());
    contract.dedupe_window_ms = reader.u32();
    contract.timeout_ms = reader.u32();
    contract.input_format = reader.u32();
    contract.result_format = reader.u32();
    contract.input_text_limit = reader.u32();
    contract.result_text_limit = reader.u32();
    contract.resource_count = reader.u32();
    if (contract.resource_count > resource_group_max) {
        return false;
    }
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        if (!read_id(reader, contract.resources[at].id)) {
            return false;
        }
        contract.resources[at].units = reader.u32();
    }
    return reader.valid();
}

uint32_t execution_contract_size(const CallContract &contract) {
    uint32_t size = execution_contract_fixed_bytes;
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        size += foundation::string_field_size(contract.resources[at].id.size()) +
                static_cast<uint32_t>(sizeof(uint32_t));
    }
    return size;
}
} // namespace ayni::catalog

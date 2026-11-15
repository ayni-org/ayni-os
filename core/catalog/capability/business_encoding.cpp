#include "catalog/capability/business_encoding.h"
#include "catalog/capability/workspace.h"

#include <cstring>

namespace ayni::catalog {
namespace {
constexpr uint32_t no_subscription = 0;
constexpr uint32_t has_subscription = 1;

void write_subscription(const CallContract &execution, foundation::BodyWriter &writer) {
    writer.u32(execution.subscription ? has_subscription : no_subscription);
    if (execution.subscription) {
        writer.text({execution.subscription->source.data(), execution.subscription->source.size()});
        writer.u32(execution.subscription->source_version);
        writer.u32(execution.subscription->lease_ms);
    }
}

bool read_subscription(foundation::BodyReader &reader, CallContract &execution) {
    const uint32_t marker = reader.u32();
    if (marker == has_subscription) {
        SubscriptionContract spec;
        const auto source = reader.text(CatalogId::max_size);
        if (!spec.source.assign(source)) {
            return false;
        }
        spec.source_version = reader.u32();
        spec.lease_ms = reader.u32();
        execution.subscription = spec;
    }
    return reader.valid() && (marker == no_subscription || marker == has_subscription);
}

// The accepted field mapping is a set; sort by stable authority identity.
ayni_status_t write_projections(const ayni_business_contract_v1 &business,
                                foundation::BodyWriter &writer) {
    FieldOrder order{};
    for (uint32_t at = 0; at < business.projection_count; ++at) {
        // Admission already proved distinct complete IDs. Reuse the field
        // insertion mechanism; the branch cannot fail for the admitted input.
        const bool inserted = insert_field_order(business.projections, at, order);
        if (!inserted) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    writer.u32(business.projection_count);
    for (uint32_t at = 0; at < business.projection_count; ++at) {
        const auto &projection = business.projections[order[at]];
        writer.u32(projection.field_id);
        writer.u32(projection.flags);
        writer.u32(projection.scale);
        writer.u32(projection.path_count);
        writer.text(projection.unit);
        for (uint32_t depth = 0; depth < projection.path_count; ++depth) {
            writer.u32(projection.path[depth]);
        }
    }
    return AYNI_OK;
}

bool read_projections(foundation::BodyReader &reader, DecodedBusiness &out) {
    out.count = reader.u32();
    if (out.count > out.projections.size()) {
        return false;
    }
    uint32_t previous = 0;
    for (uint32_t at = 0; at < out.count; ++at) {
        auto &projection = out.projections[at];
        projection.field_id = reader.u32();
        projection.flags = reader.u32();
        projection.scale = reader.u32();
        projection.path_count = reader.u32();
        projection.unit = reader.text(CatalogId::max_size);
        if (projection.field_id <= previous || projection.path_count == 0U ||
            projection.path_count > AYNI_VALUE_DEPTH_MAX) {
            return false;
        }
        previous = projection.field_id;
        for (uint32_t depth = 0; depth < projection.path_count; ++depth) {
            projection.path[depth] = reader.u32();
        }
    }
    return reader.valid();
}
} // namespace

// Compile complete immutable semantics before touching the caller's output.
// A canonical schema proof is produced once per definition, not per list row.
ayni_status_t encode_business(ayni::foundation::Workspace &workspace, ayni_bytes_v1 domain,
                              const CallContract &execution,
                              const ayni_business_contract_v1 &business, uint8_t *out,
                              uint32_t capacity, uint32_t &size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    ProjectionShape shape;
    const auto admitted = validate_projection(workspace, domain, business, shape);
    if (admitted != AYNI_OK) {
        return admitted;
    }
    return encode_admitted_business(workspace, execution, business, out, capacity, size);
}

ayni_status_t encode_admitted_business(ayni::foundation::Workspace &workspace,
                                       const CallContract &execution,
                                       const ayni_business_contract_v1 &business, uint8_t *out,
                                       uint32_t capacity, uint32_t &size) {
    if ((execution.result_format == call_text_result_format ||
         execution.result_format == call_tool_calls_result_format) &&
        business.result.nodes[0].kind != AYNI_VALUE_TEXT) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto proof_lease = workspace.make<std::array<uint8_t, business_encoding_bytes_max>>();
    if (!proof_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &proof = *proof_lease;
    auto schema_lease = workspace.make<std::array<uint8_t, schema_encoding_bytes_max>>();
    if (!schema_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &schema = *schema_lease;
    foundation::BodyWriter writer(proof.data(), static_cast<uint32_t>(proof.size()));
    writer.u32(business_encoding_version);
    write_execution_contract(execution, writer);
    write_subscription(execution, writer);
    for (const auto *definition : {&business.input, &business.result}) {
        uint32_t bytes = 0;
        const auto encoded = encode_validated_schema(workspace, *definition, schema.data(),
                                                     static_cast<uint32_t>(schema.size()), bytes);
        if (encoded != AYNI_OK) {
            return encoded;
        }
        writer.bytes(schema.data(), bytes);
    }
    const auto projected = write_projections(business, writer);
    if (projected != AYNI_OK) {
        return projected;
    }
    if (!writer.valid()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (writer.position() > capacity) {
        size = writer.position();
        return AYNI_BUFFER_TOO_SMALL;
    }
    std::memcpy(out, proof.data(), writer.position());
    size = writer.position();
    return AYNI_OK;
}

namespace {
ayni_status_t check_business_header(ayni_bytes_v1 body) {
    if (body.data == nullptr || body.size < sizeof(uint32_t) ||
        body.size > business_encoding_bytes_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    foundation::BodyReader reader(body);
    return reader.u32() == business_encoding_version ? AYNI_OK : AYNI_UNSUPPORTED_VERSION;
}
using SchemaReader = ayni_status_t (*)(ayni::foundation::Workspace &, ayni_bytes_v1,
                                       DecodedSchema &);
// The layout is read once here. External/replay callers validate each schema; a live immutable
// owner uses its already admitted bytes. No caller can supply a replacement catalog or domain.
ayni_status_t read_business_body(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body,
                                 DecodedBusiness &value, SchemaReader schema_reader) {
    foundation::BodyReader reader(body);
    static_cast<void>(reader.u32()); // Both callers admitted the header before taking scratch.
    if (!read_execution_contract(reader, value.execution) ||
        !read_subscription(reader, value.execution)) {
        return AYNI_INVALID_ARGUMENT;
    }
    CallContract normalized{};
    if (normalize_call_contract(value.execution, normalized) != AYNI_OK ||
        !same_call_contract(value.execution, normalized)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (auto *schema : {&value.input, &value.result}) {
        const auto bytes = reader.bytes(schema_encoding_bytes_max);
        if (!reader.valid()) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto decoded = schema_reader(workspace, bytes, *schema);
        if (decoded != AYNI_OK) {
            return decoded;
        }
    }
    if (!read_projections(reader, value) || !reader.ok()) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.execution.business_contract = body;
    return AYNI_OK;
}
} // namespace

ayni_status_t decode_business(ayni::foundation::Workspace &workspace, ayni_bytes_v1 domain,
                              ayni_bytes_v1 body, DecodedBusiness &out) {
    const auto header = check_business_header(body);
    if (header != AYNI_OK) {
        return header;
    }
    auto value_lease = workspace.make<DecodedBusiness>();
    if (!value_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &value = *value_lease;
    const auto read = read_business_body(workspace, body, value, decode_schema);
    if (read != AYNI_OK) {
        return read;
    }
    if ((value.execution.result_format == call_text_result_format ||
         value.execution.result_format == call_tool_calls_result_format) &&
        value.result.nodes[0].kind != AYNI_VALUE_TEXT) {
        return AYNI_INVALID_ARGUMENT;
    }
    ProjectionShape shape;
    const auto admitted = validate_projection_schemas_admitted(domain, value.view(), shape);
    if (admitted != AYNI_OK) {
        return admitted;
    }
    out = value;
    return AYNI_OK;
}

ayni_status_t read_admitted_business(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body,
                                     DecodedBusiness &out) {
    const auto header = check_business_header(body);
    if (header != AYNI_OK) {
        return header;
    }
    auto candidate = workspace.make<DecodedBusiness>();
    if (!candidate) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto status = read_business_body(workspace, body, *candidate, read_admitted_schema);
    if (status == AYNI_OK) {
        out = *candidate;
    }
    return status;
}

ayni_status_t store_business_contract(ayni::foundation::Workspace &workspace,
                                      const CallContract &execution,
                                      const ayni_business_contract_v1 *definition, uint8_t *out,
                                      uint32_t capacity, ayni_bytes_v1 &stored) {
    if (definition != nullptr) {
        if (execution.input_format != call_value_input_format) {
            return AYNI_INVALID_ARGUMENT;
        }
        uint32_t size = 0;
        const auto status =
            encode_admitted_business(workspace, execution, *definition, out, capacity, size);
        if (status != AYNI_OK) {
            return status == AYNI_BUFFER_TOO_SMALL ? AYNI_CAPACITY_EXCEEDED : status;
        }
        stored = {out, size};
        return AYNI_OK;
    }
    const auto proof = execution.business_contract;
    if (proof.size == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (proof.data == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (proof.size > capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    std::memcpy(out, proof.data, proof.size);
    stored = {out, proof.size};
    return AYNI_OK;
}
} // namespace ayni::catalog

extern "C" ayni_status_t
ayni_business_contract_read(const ayni_workspace_v1 *region, ayni_bytes_v1 original_domain,
                            ayni_bytes_v1 original_proof, void *context,
                            ayni_status_t (*receive)(void *, const ayni_business_contract_v1 *)) {
    const auto workspace_status =
        ayni::foundation::Workspace::check(region, ayni::catalog::business_workspace_size());
    if (workspace_status != AYNI_OK) {
        return workspace_status;
    }
    ayni::foundation::Workspace workspace(region->data, region->byte_capacity);

    if (receive == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto decoded_lease = workspace.make<ayni::catalog::DecodedBusiness>();
    if (!decoded_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &decoded = *decoded_lease;
    const auto status =
        ayni::catalog::decode_business(workspace, original_domain, original_proof, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    const auto view = decoded.view();
    const auto received = receive(context, &view);
    return received <= AYNI_OK ? received : AYNI_INVALID_ARGUMENT;
}

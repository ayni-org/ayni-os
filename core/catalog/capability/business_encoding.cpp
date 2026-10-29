#include "catalog/capability/business_encoding.h"

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
ayni_status_t encode_business(ayni_bytes_v1 domain, const CallContract &execution,
                              const ayni_business_contract_v1 &business, uint8_t *out,
                              uint32_t capacity, uint32_t &size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    ProjectionShape shape;
    const auto admitted = validate_projection(domain, business, shape);
    if (admitted != AYNI_OK) {
        return admitted;
    }
    return encode_admitted_business(execution, business, out, capacity, size);
}

ayni_status_t encode_admitted_business(const CallContract &execution,
                                       const ayni_business_contract_v1 &business, uint8_t *out,
                                       uint32_t capacity, uint32_t &size) {
    if ((execution.result_format == call_text_result_format ||
         execution.result_format == call_tool_calls_result_format) &&
        business.result.nodes[0].kind != AYNI_VALUE_TEXT) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::array<uint8_t, business_encoding_bytes_max> proof{};
    std::array<uint8_t, schema_encoding_bytes_max> schema{};
    foundation::BodyWriter writer(proof.data(), static_cast<uint32_t>(proof.size()));
    writer.u32(business_encoding_version);
    write_execution_contract(execution, writer);
    write_subscription(execution, writer);
    for (const auto *definition : {&business.input, &business.result}) {
        uint32_t bytes = 0;
        const auto encoded = encode_validated_schema(*definition, schema.data(),
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

ayni_status_t decode_business(ayni_bytes_v1 domain, ayni_bytes_v1 body, DecodedBusiness &out) {
    if (body.data == nullptr || body.size < sizeof(uint32_t) ||
        body.size > business_encoding_bytes_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    foundation::BodyReader reader(body);
    if (reader.u32() != business_encoding_version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    DecodedBusiness value;
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
        const auto decoded = decode_schema(bytes, *schema);
        if (decoded != AYNI_OK) {
            return decoded;
        }
    }
    if ((value.execution.result_format == call_text_result_format ||
         value.execution.result_format == call_tool_calls_result_format) &&
        value.result.nodes[0].kind != AYNI_VALUE_TEXT) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!read_projections(reader, value) || !reader.ok()) {
        return AYNI_INVALID_ARGUMENT;
    }
    ProjectionShape shape;
    const auto admitted = validate_projection_schemas_admitted(domain, value.view(), shape);
    if (admitted != AYNI_OK) {
        return admitted;
    }
    value.execution.business_contract = body;
    out = value;
    return AYNI_OK;
}

ayni_status_t store_business_contract(const CallContract &execution,
                                      const ayni_business_contract_v1 *definition, uint8_t *out,
                                      uint32_t capacity, ayni_bytes_v1 &stored) {
    if (definition != nullptr) {
        if (execution.input_format != call_value_input_format) {
            return AYNI_INVALID_ARGUMENT;
        }
        uint32_t size = 0;
        const auto status = encode_admitted_business(execution, *definition, out, capacity, size);
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
ayni_business_contract_read(ayni_bytes_v1 original_domain, ayni_bytes_v1 original_proof,
                            void *context,
                            ayni_status_t (*receive)(void *, const ayni_business_contract_v1 *)) {
    if (receive == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::DecodedBusiness decoded;
    const auto status = ayni::catalog::decode_business(original_domain, original_proof, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    const auto view = decoded.view();
    const auto received = receive(context, &view);
    return received <= AYNI_OK ? received : AYNI_INVALID_ARGUMENT;
}

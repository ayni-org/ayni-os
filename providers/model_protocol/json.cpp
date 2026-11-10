#include "json.h"
namespace ayni::providers::model_protocol::detail {
namespace {
// Check depth before the parser creates a DOM. yyjson remains the syntax/UTF-8
// validator; braces inside escaped strings cannot increase the structural depth.
bool depth_valid(const char *data, size_t size, uint32_t maximum_depth) {
    uint32_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > maximum_depth) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    return depth == 0 && !quoted;
}

enum class ShapeRegion { body, response, usage };
ayni_status_t validate_shape(yyjson_val *value, ShapeRegion region, bool &invalid_usage) {
    if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) > json_container_max) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size_t i, count;
        yyjson_val *child;
        yyjson_arr_foreach(value, i, count, child) {
            const auto status = validate_shape(
                child, region == ShapeRegion::usage ? ShapeRegion::usage : ShapeRegion::body,
                invalid_usage);
            if (status != AYNI_OK) {
                return status;
            }
        }
    } else if (yyjson_is_obj(value)) {
        if (yyjson_obj_size(value) > AYNI_SCHEMA_NODE_COUNT_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size_t i, count;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, i, count, key, child) {
            const auto child_region =
                region == ShapeRegion::usage ||
                        (region == ShapeRegion::response && yyjson_equals_str(key, "usage"))
                    ? ShapeRegion::usage
                    : ShapeRegion::body;
            // Ambiguity in optional usage is local to accounting. Response and
            // argument identities remain strict; capacity checks still walk all data.
            if (yyjson_obj_getn(value, yyjson_get_str(key), yyjson_get_len(key)) != child) {
                if (child_region != ShapeRegion::usage) {
                    return AYNI_INVALID_ARGUMENT;
                }
                invalid_usage = true;
            }
            const auto status = validate_shape(child, child_region, invalid_usage);
            if (status != AYNI_OK) {
                return status;
            }
        }
    }
    return AYNI_OK;
}
} // namespace
Document parse(char *data, size_t size, char *pool, size_t pool_size, ayni_status_t &status,
               bool raw_numbers, bool *invalid_usage, uint32_t maximum_depth) {
    status = AYNI_INVALID_ARGUMENT;
    yyjson_alc allocator{};
    if (!depth_valid(data, size, maximum_depth) ||
        !yyjson_alc_pool_init(&allocator, pool, pool_size)) {
        return {nullptr, yyjson_doc_free};
    }
    Document doc(yyjson_read_opts(data, size, raw_numbers ? YYJSON_READ_NUMBER_AS_RAW : 0,
                                  &allocator, nullptr),
                 yyjson_doc_free);
    if (doc) {
        bool ignored = false;
        status = validate_shape(yyjson_doc_get_root(doc.get()),
                                invalid_usage ? ShapeRegion::response : ShapeRegion::body,
                                invalid_usage ? *invalid_usage : ignored);
        if (status != AYNI_OK) {
            doc.reset();
        }
    }
    return doc;
}

void write_json(ayni::json::Writer &out, yyjson_val *value) {
    if (yyjson_is_obj(value)) {
        out.raw("{");
        size_t at, count;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, at, count, key, child) {
            if (at) {
                out.raw(",");
            }
            out.string(
                ayni_string_v1{yyjson_get_str(key), static_cast<uint32_t>(yyjson_get_len(key))});
            out.raw(":");
            write_json(out, child);
        }
        out.raw("}");
    } else if (yyjson_is_arr(value)) {
        out.raw("[");
        size_t at, count;
        yyjson_val *child;
        yyjson_arr_foreach(value, at, count, child) {
            if (at) {
                out.raw(",");
            }
            write_json(out, child);
        }
        out.raw("]");
    } else if (yyjson_is_str(value)) {
        out.string(
            ayni_string_v1{yyjson_get_str(value), static_cast<uint32_t>(yyjson_get_len(value))});
    } else if (yyjson_is_raw(value)) {
        out.raw({yyjson_get_raw(value), yyjson_get_len(value)});
    } else if (yyjson_is_uint(value)) {
        out.unsigned_number(yyjson_get_uint(value));
    } else if (yyjson_is_sint(value)) {
        out.number(yyjson_get_sint(value));
    } else if (yyjson_is_real(value)) {
        out.real(yyjson_get_real(value));
    } else {
        out.raw(yyjson_is_true(value) ? "true" : yyjson_is_false(value) ? "false" : "null");
    }
}
} // namespace ayni::providers::model_protocol::detail

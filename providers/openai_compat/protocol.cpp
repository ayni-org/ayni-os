#include "protocol.h"
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <yyjson.h>

namespace ayni::providers::openai_compat {
namespace {
// Allocation-free JSON writer. Every append checks the remaining fixed buffer.
class Writer {
  public:
    explicit Writer(char *buffer) : data_(buffer) {}
    void raw(std::string_view value) {
        if (value.size() > request_json_max - used_) {
            ok_ = false;
            return;
        }
        std::memcpy(data_ + used_, value.data(), value.size());
        used_ += value.size();
    }
    void string(ayni_string_v1 value) {
        constexpr char hex[] = "0123456789abcdef";
        constexpr unsigned nibble_bits = 4;
        constexpr unsigned nibble_mask = 15;
        raw("\"");
        for (uint32_t at = 0; at < value.size; ++at) {
            const auto byte = static_cast<unsigned char>(value.data[at]);
            if (byte == '"' || byte == '\\') {
                raw("\\");
                raw({value.data + at, 1});
            } else if (byte < ' ') {
                const char escape[] = {
                    '\\', 'u', '0', '0', hex[byte >> nibble_bits], hex[byte & nibble_mask]};
                raw({escape, sizeof(escape)});
            } else {
                raw({value.data + at, 1});
            }
        }
        raw("\"");
    }
    void string(const std::string &value) {
        string(ayni_string_v1{value.data(), static_cast<uint32_t>(value.size())});
    }
    void number(int64_t value) {
        char buffer[std::numeric_limits<int64_t>::digits10 + 3];
        const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
        raw({buffer, static_cast<size_t>(written.ptr - buffer)});
    }
    void name(const char *prefix, uint32_t value) {
        raw("\"");
        raw(prefix);
        number(value);
        raw("\"");
    }
    bool finish(size_t &size) const {
        size = used_;
        return ok_;
    }

  private:
    char *data_;
    size_t used_ = 0;
    bool ok_ = true;
};

void field_schema(Writer &out, const ayni_authority_field_desc_v1 &field) {
    const auto &domain = field.domain;
    if (domain.kind == AYNI_AUTHORITY_I64) {
        out.raw("{\"type\":\"integer\",\"description\":");
        out.string(domain.unit);
        out.raw(",\"minimum\":");
        out.number(domain.values.integer.min_inclusive);
        out.raw(",\"maximum\":");
        out.number(domain.values.integer.max_inclusive);
    } else {
        out.raw("{\"type\":\"string\",\"enum\":[");
        for (uint32_t i = 0; i < domain.values.tokens.member_count; ++i) {
            if (i != 0) {
                out.raw(",");
            }
            out.string(domain.values.tokens.members[i]);
        }
        out.raw("]");
    }
    out.raw("}");
}

void tool_schema(Writer &out, const Tool &tool, uint32_t index) {
    const auto &view = tool.view;
    out.raw("{\"type\":\"function\",\"function\":{\"name\":");
    out.name("tool_", index);
    out.raw(",\"description\":");
    out.string(view.binding_id);
    out.raw(",\"parameters\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{");
    for (uint32_t i = 0; i < view.field_count; ++i) {
        out.name("field_", view.fields[i].field_id);
        out.raw(":");
        field_schema(out, view.fields[i]);
        out.raw(",");
    }
    out.raw("\"text\":{\"type\":\"string\",\"description\":\"UTF-8 body; maximum ");
    out.number(view.text_limit);
    out.raw(" bytes\"}},\"required\":[");
    for (uint32_t i = 0; i < view.field_count; ++i) {
        out.name("field_", view.fields[i].field_id);
        out.raw(",");
    }
    out.raw("\"text\"]}}}");
}

// Check depth before the parser creates a DOM. yyjson remains the syntax/UTF-8
// validator; braces inside escaped strings cannot increase the structural depth.
bool depth_valid(const char *data, size_t size) {
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
            if (++depth > json_depth_max) {
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

ayni_status_t validate_shape(yyjson_val *value) {
    if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) > json_container_max) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size_t i, count;
        yyjson_val *child;
        yyjson_arr_foreach(value, i, count, child) {
            const auto status = validate_shape(child);
            if (status != AYNI_OK) {
                return status;
            }
        }
    } else if (yyjson_is_obj(value)) {
        if (yyjson_obj_size(value) > json_container_max) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size_t i, count;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, i, count, key, child) {
            // Duplicate object names are ambiguous, including inside ignored metadata.
            if (yyjson_obj_getn(value, yyjson_get_str(key), yyjson_get_len(key)) != child) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto status = validate_shape(child);
            if (status != AYNI_OK) {
                return status;
            }
        }
    }
    return AYNI_OK;
}
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
Document parse(char *data, size_t size, char *pool, size_t pool_size, ayni_status_t &status) {
    status = AYNI_INVALID_ARGUMENT;
    yyjson_alc allocator{};
    if (!depth_valid(data, size) || !yyjson_alc_pool_init(&allocator, pool, pool_size)) {
        return {nullptr, yyjson_doc_free};
    }
    Document doc(yyjson_read_opts(data, size, 0, &allocator, nullptr), yyjson_doc_free);
    if (doc) {
        status = validate_shape(yyjson_doc_get_root(doc.get()));
        if (status != AYNI_OK) {
            doc.reset();
        }
    }
    return doc;
}

bool named(yyjson_val *value, const char *prefix, uint32_t id) {
    constexpr size_t name_capacity = 32;
    char name[name_capacity];
    const auto prefix_size = std::strlen(prefix);
    std::memcpy(name, prefix, prefix_size);
    const auto number = std::to_chars(name + prefix_size, name + sizeof(name), id);
    return yyjson_equals_strn(value, name, static_cast<size_t>(number.ptr - name));
}
yyjson_val *field_value(yyjson_val *object, uint32_t id) {
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, i, count, key, value) {
        if (named(key, "field_", id)) {
            return value;
        }
    }
    return nullptr;
}
bool decode_field(yyjson_val *value, const ayni_authority_field_desc_v1 &offered,
                  ayni_scope_field_v1 &field, ayni_string_v1 &token) {
    field = {sizeof(field), offered.field_id, offered.domain};
    auto &domain = field.allowed;
    if (domain.kind == AYNI_AUTHORITY_I64) {
        if (!yyjson_is_int(value) ||
            (yyjson_is_uint(value) && yyjson_get_uint(value) > static_cast<uint64_t>(INT64_MAX))) {
            return false;
        }
        const auto integer = yyjson_get_sint(value);
        if (integer < domain.values.integer.min_inclusive ||
            integer > domain.values.integer.max_inclusive) {
            return false;
        }
        domain.values.integer = {integer, integer};
        return true;
    }
    for (uint32_t at = 0; at < domain.values.tokens.member_count; ++at) {
        const auto candidate = domain.values.tokens.members[at];
        if (yyjson_equals_strn(value, candidate.data, candidate.size)) {
            token = candidate;
            domain.values.tokens = {&token, 1, 0};
            return true;
        }
    }
    return false;
}
bool copy_body(yyjson_val *value, uint32_t limit, Answer &answer, ayni_string_v1 &out) {
    if (!yyjson_is_str(value) || yyjson_get_len(value) > limit - answer.used) {
        return false;
    }
    const size_t size = yyjson_get_len(value);
    std::memcpy(answer.body.get() + answer.used, yyjson_get_str(value), size);
    out = {answer.body.get() + answer.used, static_cast<uint32_t>(size)};
    answer.used += size;
    return true;
}
// Decode one exact offered function; no JSON view survives this helper.
ayni_status_t decode_tool(yyjson_val *call, const Tool *tools, uint32_t tool_count,
                          uint32_t result_limit, size_t at, Answer &answer, char *pool,
                          size_t pool_size) {
    ayni_status_t parsed = AYNI_OK;
    if (!yyjson_equals_str(yyjson_obj_get(call, "type"), "function")) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto *function = yyjson_obj_get(call, "function");
    const Tool *tool = nullptr;
    for (uint32_t i = 0; i < tool_count; ++i) {
        if (named(yyjson_obj_get(function, "name"), "tool_", i)) {
            tool = &tools[i];
            break;
        }
    }
    auto *args = yyjson_obj_get(function, "arguments");
    if (!tool || !yyjson_is_str(args)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto arguments = parse(const_cast<char *>(yyjson_get_str(args)), yyjson_get_len(args), pool,
                           pool_size, parsed);
    if (!arguments) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto *object = yyjson_doc_get_root(arguments.get());
    const auto &view = tool->view;
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != view.field_count + 1U) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t f = 0; f < view.field_count; ++f) {
        if (!decode_field(field_value(object, view.fields[f].field_id), view.fields[f],
                          answer.fields[at][f], answer.tokens[at][f])) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    auto *body = yyjson_obj_get(object, "text");
    if (!yyjson_is_str(body) || yyjson_get_len(body) > view.text_limit) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &decoded = answer.calls[at];
    decoded = {sizeof(decoded),
               view.binding_version,
               view.binding_id,
               {sizeof(ayni_scope_v1), view.field_count, answer.fields[at].data()},
               {}};
    if (!copy_body(body, result_limit, answer, decoded.text)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return AYNI_OK;
}
} // namespace

bool Tool::copy(const ayni_tool_v1 &source) {
    if (source.field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX) {
        return false;
    }
    view = source;
    size_t used = 0;
    const auto save = [&](ayni_string_v1 value, ayni_string_v1 &out) {
        if (value.size > AYNI_CATALOG_ID_MAX_BYTES || (value.size && !value.data)) {
            return false;
        }
        if (value.size) {
            std::memcpy(strings.data() + used, value.data, value.size);
        }
        out = {strings.data() + used, value.size};
        used += value.size;
        return true;
    };
    if (!save(source.binding_id, view.binding_id) ||
        !save(source.capability_id, view.capability_id)) {
        return false;
    }
    for (uint32_t i = 0; i < view.field_count; ++i) {
        auto &domain = view.fields[i].domain;
        if (!save(domain.unit, domain.unit)) {
            return false;
        }
        if (domain.kind == AYNI_AUTHORITY_I64) {
            continue;
        }
        if ((domain.kind != AYNI_AUTHORITY_ENUM && domain.kind != AYNI_AUTHORITY_IDENTIFIER) ||
            !domain.values.tokens.members || domain.values.tokens.member_count == 0 ||
            domain.values.tokens.member_count > AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX) {
            return false;
        }
        for (uint32_t j = 0; j < domain.values.tokens.member_count; ++j) {
            if (!save(domain.values.tokens.members[j], view.tokens[i][j])) {
                return false;
            }
        }
        domain.values.tokens.members = view.tokens[i];
    }
    return true;
}

void Answer::fail(ayni_status_t status, ayni_reason_t reason) {
    result.outcome = AYNI_CALL_FAILED;
    result.status = status;
    result.reason = reason;
    // Invalid model output does not establish whether egress/billing occurred.
    result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    result.result = {};
    result.tool_call_count = 0;
    result.tool_calls = nullptr;
}
Protocol::Protocol(size_t response_limit)
    : pool_size_(yyjson_read_max_memory_usage(response_limit, 0)),
      outer_pool_(new char[pool_size_]), arguments_pool_(new char[pool_size_]) {}

bool Protocol::encode(const Config &config, ayni_string_v1 input, const Tool *tools,
                      uint32_t tool_count, char *buffer, size_t &size) const {
    Writer out(buffer);
    out.raw("{\"model\":");
    out.string(config.model);
    out.raw(",\"max_tokens\":");
    out.number(config.max_output_tokens);
    out.raw(",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":");
    out.string(input);
    out.raw("}]");
    if (tool_count) {
        out.raw(",\"tools\":[");
        for (uint32_t i = 0; i < tool_count; ++i) {
            if (i) {
                out.raw(",");
            }
            tool_schema(out, tools[i], i);
        }
        out.raw("]");
    }
    out.raw("}");
    return out.finish(size);
}

// Only an explicitly finished singleton choice is business evidence. Protocol
// metadata can be ignored after bounded validation, but arguments must be exact.
void Protocol::decode(char *buffer, size_t size, const Tool *tools, uint32_t tool_count,
                      uint32_t result_limit, Answer &answer) {
    answer.used = 0;
    answer.fail(AYNI_INVALID_ARGUMENT, AYNI_REASON_MODEL_OUTPUT);
    ayni_status_t parsed = AYNI_OK;
    auto doc = parse(buffer, size, outer_pool_.get(), pool_size_, parsed);
    if (!doc) {
        answer.fail(parsed, AYNI_REASON_MODEL_OUTPUT);
        return;
    }
    auto *choices = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "choices");
    if (!yyjson_is_arr(choices) || yyjson_arr_size(choices) != 1) {
        return;
    }
    auto *choice = yyjson_arr_get_first(choices);
    auto *finish = yyjson_obj_get(choice, "finish_reason");
    auto *message = yyjson_obj_get(choice, "message");
    if (!yyjson_equals_str(yyjson_obj_get(message, "role"), "assistant")) {
        return;
    }
    auto *calls = yyjson_obj_get(message, "tool_calls");
    auto *content = yyjson_obj_get(message, "content");
    const auto count = yyjson_arr_size(calls);
    if (calls && !yyjson_is_null(calls) && !yyjson_is_arr(calls)) {
        return;
    }
    if (count == 0) {
        if (!yyjson_equals_str(finish, "stop") ||
            !copy_body(content, result_limit, answer, answer.result.result)) {
            return;
        }
    } else {
        if (!yyjson_equals_str(finish, "tool_calls") ||
            (content && !yyjson_is_null(content) && !yyjson_equals_str(content, ""))) {
            return;
        }
        if (count > AYNI_CALL_TOOL_COUNT_MAX) {
            answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return;
        }
        for (size_t at = 0; at < count; ++at) {
            const auto status =
                decode_tool(yyjson_arr_get(calls, at), tools, tool_count, result_limit, at, answer,
                            arguments_pool_.get(), pool_size_);
            if (status != AYNI_OK) {
                answer.fail(status, AYNI_REASON_MODEL_OUTPUT);
                return;
            }
        }
        answer.result.tool_call_count = static_cast<uint32_t>(count);
        answer.result.tool_calls = answer.calls.data();
    }
    answer.result.outcome = AYNI_CALL_COMPLETED;
    answer.result.status = AYNI_OK;
    answer.result.reason = AYNI_REASON_NONE;
    answer.result.effect = AYNI_CALL_EFFECT_COMPLETE;
}
} // namespace ayni::providers::openai_compat

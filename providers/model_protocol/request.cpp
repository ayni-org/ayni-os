#include "json.h"
#include <algorithm>
#include <ayni/providers/model_protocol.h>
#include <cstring>
#include <new>
#include <yyjson.h>

namespace ayni::providers::model_protocol {
namespace {
using json::Writer;
constexpr uint32_t definition_depth_max = envelope_depth_max;
constexpr size_t extra_bytes_max = 4096;
constexpr std::string_view continuation_codec = "openai.responses.output.v1";
constexpr std::string_view chat_selection_codec = "openai.chat.selection.v1";
ayni_string_v1 view(std::string_view value) {
    return {value.data(), static_cast<uint32_t>(value.size())};
}
// A DOM always dies before its allocator. All protocol JSON, including preloaded schemas,
// passes the same duplicate-key, UTF-8 and bounded-shape validator.
struct Document {
    Buffer pool;
    detail::Document doc{nullptr, yyjson_doc_free};
    ayni_status_t read(std::string_view bytes, uint32_t depth = json_depth_max,
                       bool raw_numbers = false) {
        const auto size = yyjson_read_max_memory_usage(bytes.size(), 0);
        if (!pool.resize(size)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        ayni_status_t status = AYNI_OK;
        doc =
            detail::parse(const_cast<char *>(bytes.data()), bytes.size(), pool.data(), size, status,
                          raw_numbers, nullptr, depth,
                          depth == definition_depth_max ? envelope_array_max : json_container_max);
        return status;
    }
    yyjson_val *root() const { return doc ? yyjson_doc_get_root(doc.get()) : nullptr; }
};
bool matches(yyjson_val *value, std::string_view expected) {
    return yyjson_is_str(value) &&
           std::string_view(yyjson_get_str(value), yyjson_get_len(value)) == expected;
}
ayni_status_t write_extra(Writer &out, std::string_view input) {
    if (input.empty()) {
        return AYNI_OK;
    }
    if (input.size() > extra_bytes_max) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Document doc;
    auto status = doc.read(input);
    if (status != AYNI_OK) {
        return status;
    }
    auto *root = doc.root();
    if (!yyjson_is_obj(root)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (const auto key : {"model", "messages", "input", "tools", "stream", "stream_options",
                           "max_tokens", "max_completion_tokens", "max_output_tokens", "store",
                           "include", "instructions", "previous_response_id", "conversation"}) {
        if (yyjson_obj_get(root, key)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    size_t at, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, at, count, key, value) {
        out.raw(",");
        out.string(view({yyjson_get_str(key), yyjson_get_len(key)}));
        out.raw(":");
        detail::write_json(out, value);
    }
    return AYNI_OK;
}
void write_function(Writer &out, const RequestOptions &options, std::string_view name,
                    std::string_view description, yyjson_val *parameters,
                    std::string_view fixed_parameters = {}) {
    out.raw(options.api == Api::responses ? "{\"type\":\"function\",\"name\":"
                                          : "{\"type\":\"function\",\"function\":{\"name\":");
    out.string(name);
    out.raw(",\"description\":");
    out.string(description);
    // Ayni schemas have optional fields and oneOf branches. Responses otherwise defaults to
    // strict:true and rewrites their semantics. Explicit non-strict preserves the contract.
    out.raw(",\"strict\":false,\"parameters\":");
    if (parameters) {
        detail::write_json(out, parameters);
    } else {
        out.raw(fixed_parameters);
    }
    out.raw(options.api == Api::responses ? "}" : "}}");
}
ayni_status_t write_tools(Writer &out, const RequestOptions &options, ayni_string_v1 definitions) {
    if (!definitions.size) {
        // Preserve the legacy Chat bytes for existing SDK estimates and consumers.
        if (options.api == Api::chat_completions) {
            model::write_functions(out);
            return AYNI_OK;
        }
        out.raw("[");
        for (size_t at = 0; at < model::functions.size(); ++at) {
            if (at) {
                out.raw(",");
            }
            const auto &function = model::functions[at];
            write_function(out, options, function.name, function.description, nullptr,
                           function.parameters);
        }
        out.raw("]");
        return AYNI_OK;
    }
    Document doc;
    const auto parsed = doc.read({definitions.data, definitions.size}, definition_depth_max);
    if (parsed != AYNI_OK) {
        return parsed;
    }
    auto *root = doc.root();
    if (!yyjson_is_arr(root) || yyjson_arr_size(root) > model::native_function_count) {
        return AYNI_INVALID_ARGUMENT;
    }
    out.raw("[");
    size_t at, count;
    yyjson_val *item;
    yyjson_arr_foreach(root, at, count, item) {
        auto *function = yyjson_obj_get(item, "function");
        auto *name = yyjson_obj_get(function, "name");
        auto *description = yyjson_obj_get(function, "description");
        auto *parameters = yyjson_obj_get(function, "parameters");
        if (!matches(yyjson_obj_get(item, "type"), "function") || !yyjson_is_str(name) ||
            !yyjson_get_len(name) || yyjson_get_len(name) > AYNI_CATALOG_ID_MAX_BYTES ||
            !yyjson_is_str(description) || !yyjson_is_obj(parameters) ||
            yyjson_obj_size(item) != 2 || yyjson_obj_size(function) != 3) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (size_t before = 0; before < at; ++before) {
            if (matches(yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(root, before), "function"),
                                       "name"),
                        {yyjson_get_str(name), yyjson_get_len(name)})) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
        if (at) {
            out.raw(",");
        }
        write_function(out, options, {yyjson_get_str(name), yyjson_get_len(name)},
                       {yyjson_get_str(description), yyjson_get_len(description)}, parameters);
    }
    out.raw("]");
    return AYNI_OK;
}
// Compare argument meaning without changing its wire lexemes. Invalid source is compared
// literally, so a malformed proposal still gets its matching local error (ADR-0085).
bool same_arguments(std::string_view left, std::string_view right) {
    Document a, b;
    if (a.read(left, json_depth_max, true) != AYNI_OK ||
        b.read(right, json_depth_max, true) != AYNI_OK) {
        return left == right;
    }
    return yyjson_equals(a.root(), b.root());
}
ayni_status_t read_selection(Document &doc, const RequestOptions &options,
                             const model::Continuation &state, std::string_view codec) {
    if (std::string_view(state.codec.data, state.codec.size) != codec) {
        return AYNI_UNSUPPORTED;
    }
    const auto parsed = doc.read({state.data.data, state.data.size});
    if (parsed != AYNI_OK) {
        return parsed;
    }
    auto *root = doc.root();
    return matches(yyjson_obj_get(root, "model"), options.model) &&
                   matches(yyjson_obj_get(root, "scope"), options.continuation_scope)
               ? AYNI_OK
               : AYNI_REVISION_CONFLICT;
}
ayni_status_t write_chat(Writer &out, const RequestOptions &options, ayni_string_v1 input,
                         model::Completeness completeness) {
    bool first = true;
    out.raw("[");
    const auto status = model::visit_messages(
        input,
        [&](const model::Message &message) {
            if (message.continuation.codec.size) {
                Document selection;
                const auto checked =
                    read_selection(selection, options, message.continuation, chat_selection_codec);
                if (checked != AYNI_OK) {
                    return checked;
                }
                if (yyjson_obj_size(selection.root()) != 2) {
                    return AYNI_INVALID_ARGUMENT;
                }
            }
            if (!first) {
                out.raw(",");
            }
            first = false;
            if (message.role == model::Role::calls) {
                model::write_calls_message(out, message);
            } else {
                model::write_text_message(out, message);
            }
            return AYNI_OK;
        },
        completeness, model::Continuations::canonical);
    out.raw("]");
    return status;
}
ayni_status_t write_continuation(Writer &out, const RequestOptions &options,
                                 const model::Message &message, bool &first) {
    Document doc;
    const auto checked = read_selection(doc, options, message.continuation, continuation_codec);
    if (checked != AYNI_OK) {
        return checked;
    }
    auto *root = doc.root();
    auto *items = yyjson_obj_get(root, "output");
    if (!yyjson_is_arr(items)) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t calls = 0;
    std::string text;
    size_t at, count;
    yyjson_val *item;
    yyjson_arr_foreach(items, at, count, item) {
        auto *type = yyjson_obj_get(item, "type");
        if (matches(type, "function_call")) {
            if (calls == message.count) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto &call = message.functions[calls++];
            auto *args = yyjson_obj_get(item, "arguments");
            if (!matches(yyjson_obj_get(item, "call_id"), {call.id.data, call.id.size}) ||
                !matches(yyjson_obj_get(item, "name"), {call.name.data, call.name.size}) ||
                !yyjson_is_str(args) ||
                !same_arguments({yyjson_get_str(args), yyjson_get_len(args)},
                                {call.arguments.data, call.arguments.size})) {
                return AYNI_INVALID_ARGUMENT;
            }
        } else if (matches(type, "message")) {
            if (!detail::append_response_text(item, text)) {
                return AYNI_INVALID_ARGUMENT;
            }
        } else if (!detail::valid_reasoning(item)) {
            return AYNI_UNSUPPORTED;
        }
    }
    if (calls != message.count ||
        text != std::string_view(message.text.data ? message.text.data : "", message.text.size)) {
        return AYNI_INVALID_ARGUMENT;
    }
    yyjson_arr_foreach(items, at, count, item) {
        if (!first) {
            out.raw(",");
        }
        first = false;
        detail::write_json(out, item);
    }
    return AYNI_OK;
}
void write_reply(Writer &out, Api api, ayni_string_v1 id, ayni_string_v1 content) {
    out.raw(api == Api::responses ? "{\"type\":\"function_call_output\",\"call_id\":"
                                  : "{\"role\":\"tool\",\"tool_call_id\":");
    out.string(id);
    out.raw(api == Api::responses ? ",\"output\":" : ",\"content\":");
    out.string(content);
    out.raw("}");
}
ayni_status_t write_responses(Writer &out, const RequestOptions &options, ayni_string_v1 input,
                              model::Completeness completeness) {
    bool first = true;
    out.raw("[");
    const auto status = model::visit_messages(
        input,
        [&](const model::Message &message) {
            if (message.continuation.codec.size) {
                return write_continuation(out, options, message, first);
            }
            const auto next = [&] {
                if (!first) {
                    out.raw(",");
                }
                first = false;
            };
            if (message.role == model::Role::calls) {
                if (message.text.size) {
                    next();
                    model::Message text = message;
                    text.role = model::Role::assistant;
                    model::write_text_message(out, text);
                }
                for (uint32_t at = 0; at < message.count; ++at) {
                    const auto &call = message.functions[at];
                    next();
                    out.raw("{\"type\":\"function_call\",\"call_id\":");
                    out.string(call.id);
                    out.raw(",\"name\":");
                    out.string(call.name);
                    out.raw(",\"arguments\":");
                    out.string(call.arguments);
                    out.raw("}");
                }
            } else {
                next();
                if (message.role == model::Role::tool) {
                    write_reply(out, options.api, message.tool_id, message.text);
                } else {
                    model::write_text_message(out, message);
                }
            }
            return AYNI_OK;
        },
        completeness, model::Continuations::canonical);
    out.raw("]");
    return status;
}
} // namespace
std::string_view path(Api api) {
    return api == Api::responses ? "/responses" : "/chat/completions";
}
ayni_status_t Protocol::write_context(Writer &out, const RequestOptions &options,
                                      ayni_string_v1 input, model::Mode mode,
                                      model::Completeness completeness) const {
    ayni_string_v1 messages{}, definitions{};
    auto status = model::split_request(input, messages, definitions);
    if (status != AYNI_OK) {
        return status;
    }
    std::string framed;
    if (std::string_view(messages.data, messages.size).substr(0, model::messages_prefix.size()) !=
        model::messages_prefix) {
        ayni_string_v1 instructions{}, data{};
        status = model::split_instructions(messages, instructions, data);
        if (status != AYNI_OK) {
            return status;
        }
        const model::Writer::Grow grow = [](void *context, uint32_t size) {
            auto &value = *static_cast<std::string *>(context);
            value.resize(size);
            return value.data();
        };
        model::Writer writer(&framed, grow, 0);
        writer.prefix();
        if (instructions.size) {
            writer.text(model::Role::system, instructions);
        }
        writer.text(model::Role::user, data);
        if (!writer.finish(messages)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
    out.raw(options.api == Api::responses ? "\"input\":" : "\"messages\":");
    status = options.api == Api::responses ? write_responses(out, options, messages, completeness)
                                           : write_chat(out, options, messages, completeness);
    if (status != AYNI_OK) {
        return status;
    }
    if (mode == model::Mode::assistant) {
        out.raw(",\"tools\":");
        return write_tools(out, options, definitions);
    }
    return AYNI_OK;
}
ayni_status_t Protocol::write_request(Writer &out, const RequestOptions &options,
                                      ayni_string_v1 input) const {
    if (options.model.empty() || !options.max_output_tokens ||
        (options.api != Api::chat_completions && options.api != Api::responses) ||
        (options.chat_token_limit != ChatTokenLimit::max_tokens &&
         options.chat_token_limit != ChatTokenLimit::max_completion_tokens) ||
        (options.reply_encoding != model::ReplyEncoding::text &&
         options.reply_encoding != model::ReplyEncoding::framed) ||
        (options.api == Api::responses && options.reply_encoding != model::ReplyEncoding::framed) ||
        (options.reply_encoding == model::ReplyEncoding::framed &&
         options.continuation_scope.empty()) ||
        (options.result_format != AYNI_CALL_RESULT_TEXT &&
         options.result_format != AYNI_CALL_RESULT_TOOL_CALLS)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out.raw("{\"model\":");
    out.string(options.model);
    out.raw(options.api == Api::responses ? ",\"max_output_tokens\":"
            : options.chat_token_limit == ChatTokenLimit::max_completion_tokens
                ? ",\"max_completion_tokens\":"
                : ",\"max_tokens\":");
    out.number(options.max_output_tokens);
    out.raw(options.stream ? ",\"stream\":true," : ",\"stream\":false,");
    if (options.api == Api::responses) {
        out.raw("\"store\":false,\"include\":[\"reasoning.encrypted_content\"],");
    } else if (options.stream) {
        out.raw("\"stream_options\":{\"include_usage\":true},");
    }
    auto status =
        write_context(out, options, input,
                      options.result_format == AYNI_CALL_RESULT_TOOL_CALLS ? model::Mode::assistant
                                                                           : model::Mode::text);
    if (status != AYNI_OK) {
        return status;
    }
    status = write_extra(out, options.extra_parameters);
    out.raw("}");
    return status;
}
ayni_status_t Protocol::encode(const RequestOptions &options, ayni_string_v1 input,
                               std::string &out) const {
    std::string encoded;
    auto writer = Writer::growing(&encoded, [](void *context, size_t minimum) {
        auto &value = *static_cast<std::string *>(context);
        if (minimum > value.max_size()) {
            return Writer::Buffer{};
        }
        value.resize(std::max(minimum, value.capacity()));
        return Writer::Buffer{value.data(), value.size()};
    });
    const auto status = write_request(writer, options, input);
    size_t size = 0;
    if (status != AYNI_OK || !writer.finish(size)) {
        return status != AYNI_OK ? status : AYNI_CAPACITY_EXCEEDED;
    }
    encoded.resize(size);
    out = std::move(encoded);
    return AYNI_OK;
}
model::Projection Protocol::projection(RequestOptions &options) {
    return {&options,
            [](void *context, Writer &out, ayni_string_v1 input, model::Mode mode,
               model::Completeness complete) {
#if defined(__cpp_exceptions)
                try {
#endif
                    return Protocol{}.write_context(out, *static_cast<RequestOptions *>(context),
                                                    input, mode, complete);
#if defined(__cpp_exceptions)
                } catch (const std::bad_alloc &) {
                    return AYNI_CAPACITY_EXCEEDED;
                }
#endif
            },
            [](void *context, Writer &out, ayni_string_v1 id, ayni_string_v1 content) {
                out.raw(",");
                write_reply(out, static_cast<RequestOptions *>(context)->api, id, content);
            },
            // Both Chat Completions and Responses append a matching comma-prefixed reply item
            // for each Role::tool message under fixed RequestOptions.
            true};
}
ayni_status_t encode_answer(const RequestOptions &options, Answer &answer, std::string &out) {
    if (answer.result.outcome != AYNI_CALL_COMPLETED) {
        return AYNI_INVALID_STATE;
    }
    ayni_string_v1 assistant = answer.result.result;
    if (options.reply_encoding == model::ReplyEncoding::framed) {
        answer.reply.clear();
        const model::Writer::Grow grow = [](void *context, uint32_t size) {
            auto &value = *static_cast<std::string *>(context);
            value.resize(size);
            return value.data();
        };
        model::Writer writer(&answer.reply, grow, 0);
        const model::Continuation state =
            answer.continuation.empty()
                ? model::Continuation{}
                : model::Continuation{view(options.api == Api::responses ? continuation_codec
                                                                         : chat_selection_codec),
                                      view(answer.continuation)};
        model::write_reply_frame(writer, assistant, state);
        if (!writer.finish(assistant)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    } else if (!answer.continuation.empty()) {
        return AYNI_UNSUPPORTED;
    }
    if (!answer.result.tool_call_count) {
        out.assign(assistant.data, assistant.size);
        return AYNI_OK;
    }
    uint32_t bytes = 0;
    auto status = ayni_tool_calls_write(answer.calls.data(), answer.result.tool_call_count,
                                        assistant, nullptr, 0, &bytes);
    if (status != AYNI_BUFFER_TOO_SMALL) {
        return status;
    }
    out.resize(bytes);
    return ayni_tool_calls_write(answer.calls.data(), answer.result.tool_call_count, assistant,
                                 reinterpret_cast<uint8_t *>(out.data()), bytes, &bytes);
}
} // namespace ayni::providers::model_protocol

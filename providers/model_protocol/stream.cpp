#include "json.h"
#include <algorithm>
#include <array>
#include <ayni/providers/model_protocol.h>
#include <new>
#include <yyjson.h>

namespace ayni::providers::model_protocol {
namespace {
using json::Writer;
void erase(std::string &value) {
    volatile char *at = value.data();
    for (size_t index = 0; index < value.size(); ++index) {
        at[index] = 0;
    }
    value.clear();
}
bool is(yyjson_val *value, const char *text) {
    return yyjson_equals_str(value, text);
}
// Optional Chat delta fields may be omitted or explicitly null between fragments.
// Required identities are still checked on the assembled final response.
bool has_value(yyjson_val *value) {
    return value && !yyjson_is_null(value);
}
std::string source(yyjson_val *value) {
    auto measured = Writer::measuring();
    detail::write_json(measured, value);
    size_t bytes = 0;
    if (!measured.finish(bytes)) {
        return {};
    }
    std::string text(bytes, '\0');
    Writer writer(text.data(), bytes);
    detail::write_json(writer, value);
    return writer.finish(bytes) ? text : std::string{};
}
} // namespace
struct Stream::Impl {
    RequestOptions options;
    size_t limit;
    Preview preview;
    ayni_status_t status = AYNI_OK;
    std::string line, event, event_type, content, finish_reason, usage, final_response;
    bool cr = false, done = false, terminal = false, invalid_usage = false;
    struct Call {
        bool present = false;
        std::string id, name, arguments;
        void clear() {
            erase(id);
            erase(name);
            erase(arguments);
        }
    };
    std::array<Call, AYNI_CALL_TOOL_COUNT_MAX> calls;
    struct Item {
        bool present = false, complete = false, arguments_seen = false;
        std::string id, arguments, final;
        void clear() {
            erase(id);
            erase(arguments);
            erase(final);
        }
    };
    std::array<Item, json_container_max> items;
    bool items_seen = false;
    Impl(const RequestOptions &value, size_t bytes, Preview callback)
        : options(value), limit(bytes), preview(callback) {}
    ~Impl() {
        for (auto *text :
             {&line, &event, &event_type, &content, &finish_reason, &usage, &final_response}) {
            erase(*text);
        }
        for (auto &call : calls) {
            call.clear();
        }
        for (auto &item : items) {
            item.clear();
        }
    }
    bool append(std::string &target, std::string_view value) {
        if (value.size() > limit - target.size()) {
            status = AYNI_CAPACITY_EXCEEDED;
            return false;
        }
        if (target.capacity() < target.size() + value.size()) {
            std::string next;
            next.reserve(
                std::min(limit, std::max(target.size() + value.size(), target.capacity() * 2)));
            next.assign(target);
            erase(target);
            target.swap(next);
        }
        target.append(value);
        return true;
    }
    bool string(yyjson_val *value, std::string &target, bool show = false) {
        if (!yyjson_is_str(value)) {
            status = AYNI_INVALID_ARGUMENT;
            return false;
        }
        const std::string_view bytes(yyjson_get_str(value), yyjson_get_len(value));
        if (!append(target, bytes)) {
            return false;
        }
        if (show && preview.text && !bytes.empty()) {
            preview.text(preview.context, bytes);
        }
        return true;
    }
    void chat(yyjson_val *root) {
        if (done || terminal) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        auto *statistics = yyjson_obj_get(root, "usage");
        if (statistics && !yyjson_is_null(statistics)) {
            usage = source(statistics);
        }
        auto *choices = yyjson_obj_get(root, "choices");
        if (!yyjson_is_arr(choices) || yyjson_arr_size(choices) > 1) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        if (!yyjson_arr_size(choices)) {
            return;
        } // Optional terminal usage chunk.
        auto *choice = yyjson_arr_get_first(choices);
        if (!finish_reason.empty() || !yyjson_is_uint(yyjson_obj_get(choice, "index")) ||
            yyjson_get_uint(yyjson_obj_get(choice, "index")) != 0) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        auto *delta = yyjson_obj_get(choice, "delta");
        if (!yyjson_is_obj(delta)) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        auto *role = yyjson_obj_get(delta, "role");
        if (has_value(role) && !is(role, "assistant")) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        for (const auto *field : {"content", "refusal"}) {
            auto *text = yyjson_obj_get(delta, field);
            if (text && !yyjson_is_null(text) && !string(text, content, true)) {
                return;
            }
        }
        auto *tool_calls = yyjson_obj_get(delta, "tool_calls");
        if (has_value(tool_calls) && !yyjson_is_arr(tool_calls)) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        size_t at, count;
        yyjson_val *call;
        yyjson_arr_foreach(tool_calls, at, count, call) {
            auto *index = yyjson_obj_get(call, "index");
            if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= calls.size()) {
                status = AYNI_CAPACITY_EXCEEDED;
                return;
            }
            auto &target = calls[static_cast<size_t>(yyjson_get_uint(index))];
            target.present = true;
            auto *type = yyjson_obj_get(call, "type");
            if (has_value(type) && !is(type, "function")) {
                status = AYNI_UNSUPPORTED;
                return;
            }
            auto *id = yyjson_obj_get(call, "id");
            if (has_value(id) && !string(id, target.id)) {
                return;
            }
            auto *function = yyjson_obj_get(call, "function");
            if (has_value(function) && !yyjson_is_obj(function)) {
                status = AYNI_INVALID_ARGUMENT;
                return;
            }
            auto *name = yyjson_obj_get(function, "name");
            auto *args = yyjson_obj_get(function, "arguments");
            if (has_value(name) && !string(name, target.name)) {
                return;
            }
            if (has_value(args) && !string(args, target.arguments)) {
                return;
            }
        }
        auto *finish = yyjson_obj_get(choice, "finish_reason");
        if (finish && !yyjson_is_null(finish)) {
            if (!is(finish, "stop") && !is(finish, "tool_calls")) {
                status = AYNI_INVALID_ARGUMENT;
                return;
            }
            string(finish, finish_reason);
        }
    }
    bool same_item(std::string &saved, yyjson_val *value) {
        Buffer pool;
        const auto bytes = yyjson_read_max_memory_usage(saved.size(), 0);
        if (!pool.resize(bytes)) {
            status = AYNI_CAPACITY_EXCEEDED;
            return false;
        }
        ayni_status_t parsed = AYNI_OK;
        auto doc = detail::parse(saved.data(), saved.size(), pool.data(), bytes, parsed, false,
                                 nullptr, envelope_depth_max, envelope_array_max);
        if (!doc) {
            status = parsed;
            return false;
        }
        return yyjson_equals(yyjson_doc_get_root(doc.get()), value);
    }
    void responses(yyjson_val *root) {
        auto *type = yyjson_obj_get(root, "type");
        if (!yyjson_is_str(type) || terminal) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        if (!event_type.empty() && !is(type, event_type.c_str())) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        if (is(type, "response.failed") || is(type, "response.incomplete") || is(type, "error")) {
            status = AYNI_RESULT_UNKNOWN;
            return;
        }
        if (is(type, "response.completed")) {
            auto *response = yyjson_obj_get(root, "response");
            auto *output = yyjson_obj_get(response, "output");
            if (!is(yyjson_obj_get(response, "status"), "completed") || !yyjson_is_arr(output)) {
                status = AYNI_INVALID_ARGUMENT;
                return;
            }
            if (items_seen) {
                const auto count = yyjson_arr_size(output);
                if (count > items.size()) {
                    status = AYNI_CAPACITY_EXCEEDED;
                    return;
                }
                for (size_t at = 0; at < items.size(); ++at) {
                    if ((at < count && (!items[at].complete ||
                                        !same_item(items[at].final, yyjson_arr_get(output, at)))) ||
                        (at >= count && items[at].present)) {
                        status = AYNI_INVALID_ARGUMENT;
                        return;
                    }
                }
            }
            final_response = source(response);
            terminal = true;
            return;
        }
        if (is(type, "response.output_text.delta")) {
            std::string text;
            string(yyjson_obj_get(root, "delta"), text, true);
            erase(text);
            return;
        }
        const bool added = is(type, "response.output_item.added"),
                   complete = is(type, "response.output_item.done"),
                   arguments = is(type, "response.function_call_arguments.delta"),
                   arguments_done = is(type, "response.function_call_arguments.done");
        if (!added && !complete && !arguments && !arguments_done) {
            return;
        }
        auto *index = yyjson_obj_get(root, "output_index");
        if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= items.size()) {
            status = AYNI_CAPACITY_EXCEEDED;
            return;
        }
        auto &target = items[static_cast<size_t>(yyjson_get_uint(index))];
        items_seen = true;
        if (added) {
            if (target.present) {
                status = AYNI_INVALID_ARGUMENT;
                return;
            }
            target.present = true;
            auto *item = yyjson_obj_get(root, "item");
            if (!string(yyjson_obj_get(item, "id"), target.id)) {
                return;
            }
            auto *args = yyjson_obj_get(item, "arguments");
            if (args) {
                string(args, target.arguments);
                target.arguments_seen = true;
            }
            return;
        }
        if (!target.present || target.complete) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        if (arguments || arguments_done) {
            if (!is(yyjson_obj_get(root, "item_id"), target.id.c_str())) {
                status = AYNI_INVALID_ARGUMENT;
                return;
            }
            if (arguments) {
                string(yyjson_obj_get(root, "delta"), target.arguments);
                target.arguments_seen = true;
            } else if (!is(yyjson_obj_get(root, "arguments"), target.arguments.c_str())) {
                status = AYNI_INVALID_ARGUMENT;
            }
            return; // arguments.done alone never completes a response.
        }
        auto *item = yyjson_obj_get(root, "item");
        if (!is(yyjson_obj_get(item, "id"), target.id.c_str()) ||
            (target.arguments_seen &&
             !is(yyjson_obj_get(item, "arguments"), target.arguments.c_str()))) {
            status = AYNI_INVALID_ARGUMENT;
            return;
        }
        target.complete = true;
        target.final = source(item);
    }
    void dispatch() {
        if (event.empty()) {
            event_type.clear();
            return;
        }
        if (options.api == Api::chat_completions && event == "[DONE]") {
            if (done || finish_reason.empty()) {
                status = AYNI_INVALID_ARGUMENT;
            }
            done = true;
            erase(event);
            event_type.clear();
            return;
        }
        Buffer pool;
        const auto bytes = yyjson_read_max_memory_usage(event.size(), 0);
        if (!pool.resize(bytes)) {
            status = AYNI_CAPACITY_EXCEEDED;
            return;
        }
        ayni_status_t parsed = AYNI_OK;
        bool usage_error = false;
        auto doc = detail::parse(event.data(), event.size(), pool.data(), bytes, parsed, false,
                                 &usage_error, envelope_depth_max, envelope_array_max);
        invalid_usage = invalid_usage || usage_error;
        if (!doc) {
            status = parsed;
            return;
        }
        auto *root = yyjson_doc_get_root(doc.get());
        if (options.api == Api::responses) {
            responses(root);
        } else {
            chat(root);
        }
        doc.reset();
        erase(event);
        event_type.clear();
    }
    void end_line() {
        if (line.empty()) {
            dispatch();
            return;
        }
        const auto colon = line.find(':');
        const auto key = std::string_view(line).substr(0, colon);
        auto value = colon == std::string::npos ? std::string_view{}
                                                : std::string_view(line).substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        if (key == "data") {
            if (!event.empty()) {
                append(event, "\n");
            }
            append(event, value);
        } else if (key == "event") {
            event_type.assign(value);
        }
        erase(line);
    }
    std::string chat_response() {
        const auto write = [&](Writer &out) {
            out.raw("{\"choices\":[{\"finish_reason\":");
            out.string(finish_reason);
            out.raw(",\"message\":{\"role\":\"assistant\",\"content\":");
            out.string(content);
            out.raw(",\"tool_calls\":[");
            bool first = true;
            bool missing = false;
            for (const auto &call : calls) {
                if (!call.present) {
                    missing = true;
                    continue;
                }
                if (missing) {
                    status = AYNI_INVALID_ARGUMENT;
                    return;
                }
                if (!first) {
                    out.raw(",");
                }
                first = false;
                out.raw("{\"id\":");
                out.string(call.id);
                out.raw(",\"type\":\"function\",\"function\":{\"name\":");
                out.string(call.name);
                out.raw(",\"arguments\":");
                out.string(call.arguments);
                out.raw("}}");
            }
            out.raw("]}}]");
            if (!usage.empty()) {
                out.raw(",\"usage\":");
                out.raw(usage);
            }
            out.raw("}");
        };
        auto measure = Writer::measuring();
        write(measure);
        size_t bytes = 0;
        if (!measure.finish(bytes) || bytes > limit) {
            status = AYNI_CAPACITY_EXCEEDED;
            return {};
        }
        std::string result(bytes, '\0');
        Writer out(result.data(), bytes);
        write(out);
        if (!out.finish(bytes)) {
            status = AYNI_CAPACITY_EXCEEDED;
        }
        return result;
    }
};
Stream::Stream(const RequestOptions &options, size_t limit, Preview preview)
    : impl_(new(std::nothrow) Impl(options, limit, preview)) {}
Stream::~Stream() = default;
ayni_status_t Stream::status() const {
    return impl_ ? impl_->status : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t Stream::feed(std::string_view bytes) {
    if (!impl_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &self = *impl_;
    if (self.status != AYNI_OK) {
        return self.status;
    }
#if defined(__cpp_exceptions)
    try {
#endif
        for (char byte : bytes) {
            if (self.cr && byte == '\n') {
                self.cr = false;
                continue;
            }
            self.cr = byte == '\r';
            if (byte == '\r' || byte == '\n') {
                self.end_line();
            } else {
                self.append(self.line, {&byte, 1});
            }
            if (self.status != AYNI_OK) {
                break;
            }
        }
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc &) {
        self.status = AYNI_CAPACITY_EXCEEDED;
    }
#endif
    return self.status;
}
void Stream::finish(Answer &answer) {
    if (!impl_) {
        answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
        return;
    }
    auto &self = *impl_;
    if (self.status != AYNI_OK || !self.line.empty() || !self.event.empty() ||
        !(self.options.api == Api::responses ? self.terminal : self.done)) {
        answer.fail(self.status == AYNI_OK ? AYNI_RESULT_UNKNOWN : self.status,
                    AYNI_REASON_MODEL_OUTPUT);
        return;
    }
#if defined(__cpp_exceptions)
    try {
#endif
        std::string response =
            self.options.api == Api::responses ? self.final_response : self.chat_response();
        if (self.status == AYNI_OK) {
            Protocol{}.decode(self.options, response.data(), response.size(), answer);
        } else {
            answer.fail(self.status, AYNI_REASON_MODEL_OUTPUT);
        }
        if (self.invalid_usage) {
            answer.result.model_usage = {AYNI_MODEL_USAGE_INVALID, 0, 0, 0, 0};
        }
        erase(response);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc &) {
        answer.fail(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
    }
#endif
}
} // namespace ayni::providers::model_protocol

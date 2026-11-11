// Synthetic public wire examples for ADR-0098; never contains private/live data.
#pragma once
#include <ayni/providers/model_protocol.h>
#include <cstdlib>
#include <string>
#include <yyjson.h>
namespace ayni::test::model_wire {
using providers::model_protocol::Api;
inline std::string response_items(bool tools) {
    return tools
               ? R"([{"type":"reasoning","id":"rs_1","summary":[],"encrypted_content":"opaque-fixture"},{"type":"message","id":"msg_1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"你好","annotations":[]}]},{"type":"function_call","id":"fc_item_1","call_id":"call_1","name":"ayni_call_capability","arguments":"{}","status":"completed"}])"
               : R"([{"type":"message","id":"msg_1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"你好","annotations":[]}]}])";
}
inline std::string responses_answer(bool tools) {
    return "{\"status\":\"completed\",\"output\":" + response_items(tools) +
           R"(,"usage":{"input_tokens":30,"output_tokens":4,"total_tokens":34}})";
}
inline std::string sse_event(std::string_view event, std::string_view data) {
    return "event: " + std::string(event) + "\r\ndata: " + std::string(data) + "\r\n\r\n";
}
inline std::string stream_answer(Api api, bool tools) {
    if (api == Api::responses) {
        auto items = response_items(tools);
        auto *doc = yyjson_read(items.data(), items.size(), 0);
        std::string events = ": fixture comment\r\n\r\n";
        size_t at, count;
        yyjson_val *item;
        yyjson_arr_foreach(yyjson_doc_get_root(doc), at, count, item) {
            char *json = yyjson_val_write(item, 0, nullptr);
            const std::string add =
                "{\"type\":\"response.output_item.added\",\"output_index\":" + std::to_string(at) +
                ",\"item\":" + json + "}";
            const std::string done =
                "{\"type\":\"response.output_item.done\",\"output_index\":" + std::to_string(at) +
                ",\"item\":" + json + "}";
            free(json);
            events += sse_event("response.output_item.added", add);
            events += sse_event("response.output_item.done", done);
        }
        yyjson_doc_free(doc);
        events += sse_event("response.output_text.delta",
                            R"({"type":"response.output_text.delta","delta":"你好"})");
        events += sse_event("response.completed", "{\"type\":\"response.completed\",\"response\":" +
                                                      responses_answer(tools) + "}");
        return events;
    }
    const auto finish = tools ? "tool_calls" : "stop";
    std::string events = "data: "
                         "{\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":"
                         "\"你\"},\"finish_reason\":null}]}\r\n\r\n";
    events += "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"好\"";
    if (tools) {
        events +=
            R"(,"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"ayni_call_capability","arguments":"{"}}])";
    }
    events += "},\"finish_reason\":null}]}\r\n\r\n";
    events += "data: {\"choices\":[{\"index\":0,\"delta\":{";
    if (tools) {
        events += R"("tool_calls":[{"index":0,"function":{"arguments":"}"}}])";
    }
    events += "},\"finish_reason\":\"" + std::string(finish) + "\"}]}\r\n\r\n";
    // Multiple data lines form one JSON event, including a usage-only final chunk.
    events += "data: {\"choices\":[],\n";
    events +=
        "data: \"usage\":{\"prompt_tokens\":30,\"completion_tokens\":4,\"total_tokens\":34}}\n\n";
    events += "data: [DONE]\r\n\r\n";
    return events;
}
} // namespace ayni::test::model_wire

#include "harness.h"
#include <ayni/values_json.hpp>

namespace ayni::strategies::detail {
namespace {
// Render while the decoded registered contract is borrowed. Only owned JSON escapes this
// callback; no schema pointer or directory view survives a decide (ADR-0098).
template <class Write> ayni_status_t render(std::string &value, Write write) {
    auto measured = json::Writer::measuring();
    write(measured);
    size_t size = 0;
    if (!measured.finish(size)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    value.resize(size);
    json::Writer out(value.data(), size);
    write(out);
    return out.finish(size) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t branch(Workspace &workspace, const ayni_capability_detail_v1 &detail,
                     std::string &value) {
    return ::ayni::detail::with_workspace(
        workspace, ayni_business_workspace_requirements, [&](const ayni_workspace_v1 *region) {
            struct State {
                const ayni_capability_detail_v1 &detail;
                std::string &value;
            } captured{detail, value};
            return ayni_business_contract_read(
                region, detail.original_domain, detail.business_contract, &captured,
                [](void *context, const ayni_business_contract_v1 *business) {
                    auto &state = *static_cast<State *>(context);
                    const auto &selected = state.detail;
                    return render(state.value, [&](json::Writer &out) {
                        out.raw("{\"type\":\"object\",\"description\":\"");
                        for (auto part : {selected.info.summary, selected.usage.purpose,
                                          selected.usage.text_usage, selected.usage.result_usage,
                                          selected.usage.examples}) {
                            out.escaped(part);
                            out.raw("\\n");
                        }
                        for (uint32_t at = 0; at < selected.usage.field_count; ++at) {
                            out.unsigned_number(selected.usage.fields[at].field_id);
                            out.raw(": ");
                            out.escaped(selected.usage.fields[at].description);
                            out.raw("\\n");
                        }
                        out.raw("Result schema: ");
                        // The result schema is real registry data too; put it in a typed schema
                        // annotation, not a second hand-maintained device description.
                        out.raw("\",\"x-ayni-result-schema\":");
                        json::write_schema(out, business->result);
                        out.raw(",\"properties\":{\"binding_id\":{\"type\":\"string\",\"enum\":[");
                        out.string(selected.info.binding_id);
                        out.raw("]},\"binding_version\":{\"type\":\"integer\",\"enum\":[");
                        out.unsigned_number(selected.info.binding_version);
                        out.raw("]},\"operation\":{\"type\":\"string\",\"enum\":[");
                        if (selected.info.form != AYNI_CALL_SUBSCRIPTION) {
                            out.raw("\"invoke\"");
                        }
                        if (selected.info.form != AYNI_CALL_UNARY) {
                            if (selected.info.form != AYNI_CALL_SUBSCRIPTION) {
                                out.raw(",");
                            }
                            out.raw("\"start\"");
                        }
                        out.raw("]},\"arguments\":");
                        json::write_schema(out, business->input);
                        out.raw("},\"required\":[\"binding_id\",\"binding_version\",\"arguments\"");
                        if (selected.info.form == AYNI_CALL_SUBSCRIPTION) {
                            out.raw(",\"operation\"");
                        }
                        out.raw("],\"additionalProperties\":false}");
                    });
                });
        });
}
} // namespace
ayni_status_t preload_tools(Workspace &workspace, const ayni_task_snapshot_v1 &snapshot,
                            std::string &definitions, std::map<std::string, std::string> &cache) {
    if (!snapshot.list_capabilities || !snapshot.inspect_capability) {
        return AYNI_UNSUPPORTED;
    }
    std::string branches;
    ayni_capability_cursor_v1 cursor{}, next{};
    std::array<ayni_capability_info_v1, AYNI_CAPABILITY_PAGE_MAX> items{};
    uint32_t total = 0;
    do {
        uint32_t count = 0;
        auto status = snapshot.list_capabilities(snapshot.call_context, &cursor, items.data(),
                                                 AYNI_CAPABILITY_PAGE_MAX, &count, &next);
        if (status != AYNI_OK) {
            return status;
        }
        if (count > items.size() || count > AYNI_GOAL_TYPE_TOOL_COUNT_MAX - total ||
            (next.boot_epoch && (!count || next.position <= cursor.position))) {
            return AYNI_INVALID_ARGUMENT;
        }
        total += count;
        for (uint32_t at = 0; at < count; ++at) {
            const auto &info = items[at];
            const std::string cache_key = std::string(info.binding_id.data, info.binding_id.size) +
                                          ":" + std::to_string(info.binding_version);
            auto cached = cache.find(cache_key);
            if (cached != cache.end()) {
                if (!branches.empty()) {
                    branches += ',';
                }
                branches += cached->second;
                continue; // Only rendering is cached. Listing and dispatch authorization stay live.
            }
            ayni_capability_detail_v1 detail{};
            detail.struct_size = sizeof(detail);
            status = snapshot.inspect_capability(snapshot.call_context, items[at].binding_id,
                                                 items[at].binding_version, &detail);
            if (status != AYNI_OK) {
                return status;
            }
            std::string item;
            status = branch(workspace, detail, item);
            if (status != AYNI_OK) {
                return status;
            }
            if (!branches.empty()) {
                branches += ',';
            }
            if (cache.size() < AYNI_GOAL_TYPE_TOOL_COUNT_MAX) {
                cache.emplace(cache_key, item);
            }
            branches += item;
        }
        cursor = next;
    } while (cursor.boot_epoch);
    if (!branches.empty()) {
        branches += ',';
    }
    // Existing Task-scoped operation references retain their read/wait/cancel/close semantics.
    branches +=
        R"({"type":"object","properties":{"operation":{"type":"string","enum":["read","cancel","close"]},"operation_ref":{"type":"string"}},"required":["operation","operation_ref"],"additionalProperties":false},)";
    branches +=
        R"({"type":"object","properties":{"operation":{"type":"string","enum":["wait"]},"operation_ref":{"type":"string"},"after":{"type":"string"},"timeout_ms":{"type":"integer","minimum":1,"maximum":30000}},"required":["operation","operation_ref","after","timeout_ms"],"additionalProperties":false})";
    definitions =
        R"([{"type":"function","function":{"name":"ayni_call_capability","description":"Call a currently available exact binding using its registered arguments. invoke is the default; start reports acceptance, not completion. read/wait/cancel/close use this Task's operation_ref. Never retry UNKNOWN. Directory visibility does not authorize arbitrary arguments.","parameters":{"type":"object","oneOf":[)";
    definitions += branches;
    definitions += "]}}}]";
    return AYNI_OK;
}
} // namespace ayni::strategies::detail

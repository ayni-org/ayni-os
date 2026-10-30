#include "catalog/goal_type/tool_policy.h"
namespace ayni::catalog {
bool ToolPolicy::valid() const {
    if (authorization > AYNI_TOOLS_ON_DEMAND || count > tools.size()) {
        return false;
    }
    uint32_t shell_count = 0;
    for (uint32_t at = 0; at < count; ++at) {
        const auto &tool = tools[at];
        if (!tool.binding_id.size() || !tool.binding_version || tool.role > AYNI_TOOL_ROLE_SHELL) {
            return false;
        }
        shell_count += tool.role == AYNI_TOOL_ROLE_SHELL ? 1U : 0U;
        for (uint32_t before = 0; before < at; ++before) {
            if (tools[before].binding_id.equals({tool.binding_id.data(), tool.binding_id.size()})) {
                return false;
            }
        }
    }
    return shell_count <= 1;
}
bool ToolPolicy::operator==(const ToolPolicy &other) const {
    if (authorization != other.authorization || count != other.count) {
        return false;
    }
    for (uint32_t at = 0; at < count; ++at) {
        const auto &left = tools[at];
        const auto &right = other.tools[at];
        if (!left.binding_id.equals({right.binding_id.data(), right.binding_id.size()}) ||
            left.binding_version != right.binding_version || left.role != right.role) {
            return false;
        }
    }
    return true;
}
bool ToolPolicy::contains(ayni_string_v1 id, uint32_t version) const {
    for (uint32_t at = 0; at < count; ++at) {
        if (tools[at].binding_version == version && tools[at].binding_id.equals(id)) {
            return true;
        }
    }
    return false;
}
uint32_t tool_policy_size(const ToolPolicy &policy) {
    uint32_t size = tool_policy_header_bytes;
    for (uint32_t at = 0; at < policy.count; ++at) {
        size += tool_reference_fixed_bytes + policy.tools[at].binding_id.size();
    }
    return size;
}
void write_tool_policy(const ToolPolicy &policy, foundation::BodyWriter &out) {
    out.u32(policy.authorization);
    out.u32(policy.count);
    for (uint32_t at = 0; at < policy.count; ++at) {
        const auto &tool = policy.tools[at];
        out.u32(tool.binding_version);
        out.u32(tool.role);
        out.text({tool.binding_id.data(), tool.binding_id.size()});
    }
}
bool read_tool_policy(foundation::BodyReader &input, ToolPolicy &out) {
    ToolPolicy candidate;
    candidate.authorization = input.u32();
    candidate.count = input.u32();
    if (candidate.count > candidate.tools.size()) {
        return false;
    }
    for (uint32_t at = 0; at < candidate.count; ++at) {
        auto &tool = candidate.tools[at];
        tool.binding_version = input.u32();
        tool.role = input.u32();
        if (!tool.binding_id.assign(input.text(CatalogId::max_size))) {
            return false;
        }
    }
    if (!input.valid() || !candidate.valid()) {
        return false;
    }
    out = candidate;
    return true;
}
} // namespace ayni::catalog

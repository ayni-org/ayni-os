// Shared diagnostic JSON for Native fixture artifacts; independent of storage/wire codecs.
#ifndef AYNI_RUNTIME_FIXTURE_JSON_H
#define AYNI_RUNTIME_FIXTURE_JSON_H
#include <ayni/ayni.h>
#include <iomanip>
#include <sstream>
#include <string>
namespace ayni::runtime::fixtures {
inline std::string quoted(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (char byte : value) {
        const auto ch = static_cast<unsigned char>(byte);
        if (ch == '"' || ch == '\\') {
            out << '\\' << static_cast<char>(ch);
        } else if (ch < ' ') {
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch)
                << std::dec;
        } else {
            out << static_cast<char>(ch);
        }
    }
    return out.str() + '"';
}
// Capture the actual public configuration, including zeros that select automatic
// bounds. This is diagnostic serialization, independent of storage/wire formats.
inline std::string configuration_json(const ayni_config_v1 &config) {
    std::string result = "{";
    auto field = [&](const char *name, uint64_t value) {
        if (result.size() > 1) {
            result += ',';
        }
        result += quoted(name) + ":" + quoted(std::to_string(value));
    };
#define AYNI_FIXTURE_CONFIG_FIELD(name) field(#name, config.name)
    AYNI_FIXTURE_CONFIG_FIELD(struct_size);
    AYNI_FIXTURE_CONFIG_FIELD(abi_major);
    AYNI_FIXTURE_CONFIG_FIELD(abi_minor);
    AYNI_FIXTURE_CONFIG_FIELD(ingress_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(event_body_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(completion_slot_count);
    AYNI_FIXTURE_CONFIG_FIELD(storage_buffer_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(storage_log_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(storage_write_interval_us);
    AYNI_FIXTURE_CONFIG_FIELD(agent_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(goal_type_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(goal_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(goal_parameter_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(session_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(goal_depth_max);
    AYNI_FIXTURE_CONFIG_FIELD(goal_children_max);
    AYNI_FIXTURE_CONFIG_FIELD(task_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(strategy_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(policy_intent_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(task_step_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(task_checkpoint_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(command_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(capability_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(authority_scope_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(grant_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(grant_delegation_depth_max);
    AYNI_FIXTURE_CONFIG_FIELD(goal_requirement_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(resource_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(resource_request_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(task_model_call_hard_limit);
    AYNI_FIXTURE_CONFIG_FIELD(call_capability_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(provider_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(binding_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(call_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(call_input_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(call_result_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(device_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(component_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(observation_source_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(scene_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(executor_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(execution_binding_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(memory_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(memory_body_max_bytes);
    AYNI_FIXTURE_CONFIG_FIELD(memory_dependency_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(observation_ingress_capacity);
    AYNI_FIXTURE_CONFIG_FIELD(business_contract_max_bytes);
#undef AYNI_FIXTURE_CONFIG_FIELD
    return result + '}';
}
} // namespace ayni::runtime::fixtures
#endif

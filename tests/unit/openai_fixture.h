// Shared public-call harness for offline protocol tests and the opt-in live probe.
#ifndef AYNI_TEST_OPENAI_FIXTURE_H
#define AYNI_TEST_OPENAI_FIXTURE_H
// C-PROV-002/004/006/008/011 and ADR-0054/0055, through registration and public calls.
#include "loopback_http.h"
#include "mock_display_schema.h"
#include "work_node.h"
#include <array>
#include <ayni/directory.h>
#include <ayni/model_messages.hpp>
#include <ayni/providers/mock.h>
#include <ayni/providers/openai_compat.h>
#include <ayni/values.hpp>
#include <cstdlib>
#include <functional>
#include <thread>

namespace ayni::test {
using namespace ayni::test;
using ayni::providers::openai_compat::Config;
using ayni::providers::openai_compat::Provider;
constexpr uint32_t result_limit = 512;
constexpr uint32_t destination_field = AYNI_MODEL_DESTINATION_FIELD_ID;
ayni_authority_value_set_v1 tokens(ayni_authority_value_kind_t kind, const ayni_string_v1 *values,
                                   uint32_t count) {
    ayni_authority_value_set_v1 out{};
    out.struct_size = sizeof(out);
    out.kind = kind;
    out.values.tokens = {values, count, 0};
    return out;
}
class OpenaiFixture : public ::testing::Test {
  protected:
    void start(const std::string &body, int http = 200, unsigned delay_ms = 0,
               uint32_t response_limit = 4096, uint32_t timeout_ms = 1500,
               const std::string &headers = {}) {
        server = std::make_unique<LoopbackHttp>(body, http, delay_ms, headers);
        ASSERT_EQ(setenv("AYNI_PROTOCOL_FIXTURE_KEY", "fixture-only-key", 1), 0);
        Config config;
        config.base_url = server->url();
        config.model = "fixture-model";
        config.key_environment = "AYNI_PROTOCOL_FIXTURE_KEY";
        config.text_max_bytes = result_limit;
        config.request_capacity = 1;
        config.response_max_bytes = response_limit;
        config.timeout_ms = timeout_ms;
        config.result_format = model_result_format;
        initialize(config);
    }
    void initialize(const Config &config) {
        ASSERT_EQ(provider.configure(config), AYNI_OK);
        ASSERT_EQ(provider.describe(descriptor), AYNI_OK);
        display.input[1].min_i64 = 5;
        display.input[1].max_i64 = 10;
        ASSERT_EQ(presenter.configure({ayni::providers::mock::Variant::a,
                                       ayni::providers::mock::Service::action,
                                       1,
                                       64,
                                       {},
                                       &display.business,
                                       MockDisplaySchema::body_field}),
                  AYNI_OK);
        ASSERT_EQ(presenter.describe(tool_descriptor), AYNI_OK);
        auto &c = work.node().config();
        c.call_capacity = 4;
        c.storage_buffer_bytes = 0;
        c.call_capability_capacity = 2;
        c.provider_capacity = 2;
        c.binding_capacity = 2;
        c.call_input_max_bytes = 640;
        c.call_result_max_bytes = result_limit;
        c.resource_capacity = 2;
        c.resource_request_capacity = 4;
        work.node().on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            return decide(snapshot, output);
        };
        work.run();
        for (const auto *offer : {descriptor.capabilities, tool_descriptor.capabilities}) {
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             offer->capability_id,
                                             work.instance(agent_a),
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = ++command;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        }
        pump(200);
    }
    void register_profile(ayni_kernel_t *kernel) {
        auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        auto strategy = strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        const auto destination = descriptor.destination_id;
        const ayni_authority_field_desc_v1 egress{
            sizeof(egress), destination_field, AYNI_AUTHORITY_EGRESS_DESTINATION, 0,
            tokens(AYNI_AUTHORITY_IDENTIFIER, &destination, 1)};
        const ayni_capability_desc_v1 model{
            sizeof(model), 1, descriptor.capabilities->capability_id, 1, 0, &egress};
        ASSERT_EQ(ayni_register_capability(kernel, &model), AYNI_OK);
        ayni_authority_value_set_v1 integer{};
        integer.struct_size = sizeof(integer);
        integer.kind = AYNI_AUTHORITY_I64;
        integer.unit = text("second");
        integer.values.integer = {5, 10};
        const ayni_string_v1 voices[] = {text("calm"), text("bright")};
        const ayni_authority_field_desc_v1 fields[] = {
            {sizeof(ayni_authority_field_desc_v1), 1, AYNI_AUTHORITY_PARAMETER, 0, integer},
            {sizeof(ayni_authority_field_desc_v1), 2, AYNI_AUTHORITY_PARAMETER, 0,
             tokens(AYNI_AUTHORITY_ENUM, voices, 2)}};
        const ayni_capability_desc_v1 tool{
            sizeof(tool), 1, tool_descriptor.capabilities->capability_id, 2, 0, fields};
        ASSERT_EQ(ayni_register_capability(kernel, &tool), AYNI_OK);
        for (const auto *desc : {&descriptor, &tool_descriptor}) {
            const auto resource_id = desc->capabilities->contract.resources[0].resource_id;
            const ayni_resource_desc_v1 resource{sizeof(resource),
                                                 AYNI_RESOURCE_SHARED,
                                                 resource_id,
                                                 desc == &descriptor ? model_capacity : 1U,
                                                 0,
                                                 0,
                                                 0,
                                                 nullptr,
                                                 {}};
            ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
            ASSERT_EQ(ayni_set_resource_health(kernel, resource_id, AYNI_RESOURCE_ONLINE), AYNI_OK);
        }
        for (const auto *desc : {&descriptor, &tool_descriptor}) {
            ASSERT_EQ(ayni_register_call_capability(kernel, desc->capabilities), AYNI_OK);
            ASSERT_EQ(ayni_register_provider(kernel, desc), AYNI_OK);
        }
        for (const auto *desc : {&descriptor, &tool_descriptor}) {
            const ayni_binding_v1 binding{sizeof(binding),
                                          1,
                                          desc == &descriptor ? text("plan") : text("show"),
                                          desc->capabilities->capability_id,
                                          1,
                                          1,
                                          desc->provider_id,
                                          desc->endpoint_id,
                                          desc->destination_id};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        }
        if (usage_available) {
            ayni_tool_v1 key{};
            key.struct_size = sizeof(key);
            key.binding_id = text("show");
            key.binding_version = 1;
            key.capability_id = tool_descriptor.capabilities->capability_id;
            key.capability_version = 1;
            ayni_tool_usage_v1 usage{};
            usage.struct_size = sizeof(usage);
            adoption_status = lookup_usage(this, &key, &usage);
            if (adoption_status == AYNI_OK) {
                adoption_status =
                    ayni_register_capability_usage(kernel, text("Fixture display."), &usage);
            }
        }
        static const ayni_tool_ref_v1 tools[] = {{text("show"), 1, 0}};
        auto goal = goal_type_desc("assist", 1, nullptr, this);
        goal.tool_count = offer_tools ? 1U : 0U;
        goal.tools = offer_tools ? tools : nullptr;
        goal.export_requirements = [](void *ctx, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
            const auto &self = *static_cast<OpenaiFixture *>(ctx);
            for (const auto *desc : {&self.descriptor, &self.tool_descriptor}) {
                const ayni_goal_requirement_v1 requirement{sizeof(requirement),
                                                           1,
                                                           desc->capabilities->capability_id,
                                                           {sizeof(ayni_scope_v1), 0, nullptr}};
                const auto status = sink->emit(sink->ctx, &requirement);
                if (status != AYNI_OK) {
                    return status;
                }
            }
            return AYNI_OK;
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &goal), AYNI_OK);
    }
    // ADR-0082/AP-01: a mock device extension describes its declared identity,
    // rather than copying arbitrary identities from the authoritative input.
    static ayni_status_t lookup_usage(void *context, const ayni_tool_v1 *tool,
                                      ayni_tool_usage_v1 *out) {
        auto &self = *static_cast<OpenaiFixture *>(context);
        ++self.usage_lookups;
        EXPECT_EQ(std::this_thread::get_id(), self.owner_thread);
        if (self.usage_status != AYNI_OK) {
            return self.usage_status;
        }
        EXPECT_EQ(string_of(tool->binding_id), "show");
        EXPECT_EQ(tool->binding_version, 1U);
        ayni_tool_usage_v1 usage{
            sizeof(usage),
            1,
            text("show"),
            1,
            self.tool_descriptor.capabilities->capability_id,
            {self.usage_purpose.data(), static_cast<uint32_t>(self.usage_purpose.size())},
            text("Text to show on the mock display; empty text clears it."),
            text("Completion confirms simulated display only; no physical device is attached."),
            text("seconds=7, voice=calm, text=hello"),
            2,
            0,
            self.usage_fields.data()};
        if (self.change_usage) {
            self.change_usage(usage);
        }
        *out = usage;
        return AYNI_OK;
    }
    ayni_status_t decide(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
        ayni_task_call_result_v1 result{};
        result.struct_size = sizeof(result);
        const auto read = snapshot.call_result(snapshot.call_context, text("plan"), &result);
        if (read == AYNI_NOT_FOUND) {
            ayni::ValueBuilder<2> value;
            value.object();
            value.text(AYNI_MODEL_BODY_FIELD_ID,
                       {prompt.data(), static_cast<uint32_t>(prompt.size())});
            value.end();
            ayni_call_request_v1 request{sizeof(request),
                                         AYNI_POLICY_REQUEST_MODEL,
                                         1,
                                         0,
                                         text("plan"),
                                         text("plan"),
                                         {},
                                         {},
                                         {},
                                         AYNI_CALL_MODE_FOREGROUND,
                                         0};
            if (value.finish(request.value) != AYNI_OK) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto status = output.request_call(output.call_context, &request);
            EXPECT_EQ(status, AYNI_OK);
            return status;
        }
        if (read != AYNI_OK) {
            return read;
        }
        seen_result = result;
        reply = string_of(result.text);
        if (result.tool_call_count) {
            ayni_task_tool_call_v1 proposed{};
            proposed.struct_size = sizeof(proposed);
            const auto status =
                snapshot.call_tool(snapshot.call_context, text("plan"), 0, &proposed);
            if (status != AYNI_OK) {
                return status;
            }
            std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> source{};
            std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
            std::array<char, 4096> bytes{};
            ayni_value_buffer_v1 value{nodes.data(), static_cast<uint32_t>(nodes.size()), 0,
                                       bytes.data(), static_cast<uint32_t>(bytes.size()), 0};
            auto &native = proposed.call;
            proposal_error = AYNI_INVALID_ARGUMENT;
            if (native.input_kind == AYNI_TOOL_INPUT_ARGUMENTS &&
                std::string_view(native.function_name.data, native.function_name.size) ==
                    model::call_function) {
                const auto tree = native.input.arguments;
                uint32_t argument = UINT32_MAX;
                bool binding = false, version = false;
                if (tree.nodes[0].kind == AYNI_ARGUMENT_OBJECT) {
                    for (uint32_t at = 1; at < tree.node_count; at = tree.nodes[at].subtree_end) {
                        const auto &node = tree.nodes[at];
                        const std::string_view name(node.name.data, node.name.size);
                        if (name == "binding_id") {
                            binding = node.kind == AYNI_ARGUMENT_STRING &&
                                      string_of(node.value.text) == "show";
                        }
                        if (name == "binding_version") {
                            version = node.kind == AYNI_ARGUMENT_NUMBER &&
                                      string_of(node.value.text) == "1";
                        }
                        if (name == "arguments") {
                            argument = at;
                        }
                    }
                }
                if (binding && version && argument != UINT32_MAX) {
                    const uint32_t count = tree.nodes[argument].subtree_end - argument;
                    for (uint32_t at = 0; at < count; ++at) {
                        source[at] = tree.nodes[argument + at];
                        source[at].subtree_end -= argument;
                    }
                    source[0].name = {};
                    const ayni_arguments_v1 args{source.data(), count};
                    proposal_error =
                        ayni_arguments_normalize(&display.business.input, &args, &value);
                }
            } else if (native.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
                proposal_error = AYNI_NOT_FOUND;
            }
            if (proposal_error != AYNI_OK) {
                auto &intent = output.intents[0];
                intent = {};
                intent.struct_size = sizeof(intent);
                intent.kind = AYNI_POLICY_REPORT_COMPLETION;
                intent.expected_revision = snapshot.revision;
                output.intent_count = 1;
                return AYNI_OK;
            }
            ayni::ValueReader input;
            if (input.read({nodes.data(), value.node_count}) != AYNI_OK) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto *seconds = input.field(1);
            const auto *voice = input.field(2);
            const auto *body = input.field(MockDisplaySchema::body_field);
            if (!seconds || !voice || !body) {
                return AYNI_INVALID_ARGUMENT;
            }
            proposal = std::string("show|") + string_of(body->value.text) + "|" +
                       std::to_string(seconds->value.integer) + "second|" +
                       string_of(voice->value.text);
            if (execute_proposal) {
                ayni_task_call_result_v1 device{};
                device.struct_size = sizeof(device);
                const auto device_status =
                    snapshot.call_result(snapshot.call_context, text("show"), &device);
                if (device_status == AYNI_NOT_FOUND) {
                    const ayni_call_request_v1 request{sizeof(request),
                                                       AYNI_POLICY_REQUEST_CALL,
                                                       1,
                                                       0,
                                                       text("show"),
                                                       text("show"),
                                                       {},
                                                       {},
                                                       {nodes.data(), value.node_count},
                                                       AYNI_CALL_MODE_FOREGROUND,
                                                       0};
                    return output.request_call(output.call_context, &request);
                }
                if (device_status != AYNI_OK) {
                    return device_status;
                }
                device_result = device;
                device_reply = string_of(device.text);
            }
        }
        const bool completed = result.outcome == AYNI_CALL_COMPLETED &&
                               (!execute_proposal || result.tool_call_count == 0 ||
                                device_result.outcome == AYNI_CALL_COMPLETED);
        return report(completed ? AYNI_POLICY_REPORT_COMPLETION : AYNI_POLICY_REPORT_FAILURE,
                      snapshot, output);
    }
    uint64_t create() {
        const auto result = work.create(
            WorkNode::create_body(work.instance(agent_a), 0, "assist", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(result.status, AYNI_OK);
        last_goal = result.result.object.id;
        return last_goal;
    }
    void pump(unsigned rounds = 1) {
        for (unsigned i = 0; i < rounds; ++i) {
            if (work.node().lifecycle() == AYNI_LIFECYCLE_STOPPED) {
                return;
            }
            work.node().sim().complete_storage();
            EXPECT_EQ(presenter.advance(), AYNI_OK);
            work.node().sim().advance_us(1000);
            work.node().step();
        }
    }
    void wait_done(unsigned timeout_seconds = 4) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        while (std::chrono::steady_clock::now() < end) {
            pump();
            const auto goal = work.goal(last_goal);
            const auto task =
                goal.last_task_id ? work.task(goal.last_task_id) : ayni_task_info_v1{};
            const bool ended = task.lifecycle == AYNI_TASK_PAUSED ||
                               task.lifecycle == AYNI_TASK_CANCELLED ||
                               work.node().lifecycle() == AYNI_LIFECYCLE_STOPPED;
            if ((seen_result.outcome && (!execute_proposal || device_result.outcome)) ||
                (ended && provider.pending() == 0)) {
                pump(30);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto goal = work.goal(last_goal);
        const auto task = goal.last_task_id ? work.task(goal.last_task_id) : ayni_task_info_v1{};
        FAIL() << "provider did not complete; goal=" << goal.lifecycle
               << " wait=" << goal.wait_reason << " task=" << task.lifecycle
               << " provider=" << provider.status() << " pending=" << provider.pending()
               << " strategy calls=" << work.strategy().seen.size();
    }
    void wait_request() {
        for (unsigned i = 0; i < 1000 && server->requests() == 0; ++i) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(server->requests(), 1U);
    }
    void TearDown() override {
        if (work.kernel()) {
            ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
            for (unsigned i = 0; i < 4000 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED;
                 ++i) {
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
            if (descriptor.ctx) {
                // ADR-0057/C-HOST-018: a consumed terminal post may still be
                // returning through Host wake. Keep contexts until destroy.
                auto status = provider.quiesce();
                for (unsigned i = 0; status == AYNI_BUSY && i < 1000; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    status = provider.quiesce();
                }
                ASSERT_EQ(status, AYNI_OK);
            }
            ASSERT_EQ(work.node().destroy(), AYNI_OK);
        }
        if (descriptor.ctx) {
            EXPECT_EQ(provider.pending(), 0U);
            EXPECT_EQ(provider.close(), AYNI_OK);
        }
        if (tool_descriptor.ctx) {
            EXPECT_EQ(presenter.close(), AYNI_OK);
        }
        unsetenv("AYNI_PROTOCOL_FIXTURE_KEY");
    }
    std::unique_ptr<LoopbackHttp> server;
    MockDisplaySchema display;
    Provider provider;
    ayni::providers::mock::Provider presenter;
    WorkNode work;
    ayni_provider_v1 descriptor{}, tool_descriptor{};
    ayni_task_call_result_v1 seen_result{};
    ayni_task_call_result_v1 device_result{};
    bool execute_proposal = false;
    std::string device_reply;
    std::string reply, proposal;
    std::string prompt = "say hello";
    uint64_t command = 0, last_goal = 0;
    uint32_t model_capacity = 1;
    uint32_t model_result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    bool usage_available = true;
    bool offer_tools = true; // A tool-capable model may receive a request without tools.
    uint32_t usage_lookups = 0;
    ayni_status_t usage_status = AYNI_OK;
    std::thread::id owner_thread = std::this_thread::get_id();
    std::string usage_purpose = "Show text on the mock device display for a bounded duration.";
    // Deliberately reverse explanation order: pairing uses IDs, not array positions.
    std::array<ayni_tool_field_usage_v1, 2> usage_fields{
        {{sizeof(ayni_tool_field_usage_v1), 2, text("Presentation style of the mock display.")},
         {sizeof(ayni_tool_field_usage_v1), 1, text("How long to display the text.")}}};
    ayni_status_t adoption_status = AYNI_NOT_FOUND;
    ayni_status_t proposal_error = AYNI_OK;
    std::function<void(ayni_tool_usage_v1 &)> change_usage;
};

} // namespace ayni::test
#endif

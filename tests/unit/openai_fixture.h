// Shared public-call harness for offline protocol tests and the opt-in live probe.
#ifndef AYNI_TEST_OPENAI_FIXTURE_H
#define AYNI_TEST_OPENAI_FIXTURE_H
// C-PROV-002/004/006/008/011 and ADR-0054/0055, through registration and public calls.
#include "loopback_http.h"
#include "work_node.h"
#include <ayni/providers/mock.h>
#include <ayni/providers/openai_compat.h>
#include <cstdlib>

namespace ayni::test {
using namespace ayni::test;
using ayni::providers::openai_compat::Config;
using ayni::providers::openai_compat::Provider;
constexpr uint32_t result_limit = 512;
constexpr uint32_t destination_field = 3;
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
        initialize(config);
    }
    void initialize(const Config &config) {
        ASSERT_EQ(provider.configure(config), AYNI_OK);
        ASSERT_EQ(provider.describe(descriptor), AYNI_OK);
        ASSERT_EQ(presenter.configure({ayni::providers::mock::Variant::a,
                                       ayni::providers::mock::Service::action, 1, 64}),
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
        c.resource_capacity = 1;
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
        const auto resource_id = tool_descriptor.capabilities->contract.resources[0].resource_id;
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, resource_id, 1, 0, 0, 0, nullptr, {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource_id, AYNI_RESOURCE_ONLINE), AYNI_OK);
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
        static const ayni_tool_ref_v1 tools[] = {{text("show"), 1, 0}};
        auto goal = goal_type_desc("assist", 1, nullptr, this);
        goal.tool_count = 1;
        goal.tools = tools;
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
    ayni_status_t decide(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
        ayni_task_call_result_v1 result{};
        result.struct_size = sizeof(result);
        const auto read = snapshot.call_result(snapshot.call_context, text("plan"), &result);
        if (read == AYNI_NOT_FOUND) {
            const auto destination = descriptor.destination_id;
            const ayni_scope_field_v1 field{sizeof(field), destination_field,
                                            tokens(AYNI_AUTHORITY_IDENTIFIER, &destination, 1)};
            const ayni_call_request_v1 request{
                sizeof(request),
                AYNI_POLICY_REQUEST_MODEL,
                1,
                0,
                text("plan"),
                text("plan"),
                {sizeof(ayni_scope_v1), 1, &field},
                {prompt.data(), static_cast<uint32_t>(prompt.size())}};
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
            proposal = string_of(proposed.call.binding_id) + "|" + string_of(proposed.call.text);
            for (uint32_t i = 0; i < proposed.call.arguments.field_count; ++i) {
                const auto &field = proposed.call.arguments.fields[i].allowed;
                proposal += "|" + (field.kind == AYNI_AUTHORITY_I64
                                       ? std::to_string(field.values.integer.min_inclusive) +
                                             string_of(field.unit)
                                       : string_of(field.values.tokens.members[0]));
            }
        }
        return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                            : AYNI_POLICY_REPORT_FAILURE,
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
            if (seen_result.outcome || (ended && provider.pending() == 0)) {
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
    Provider provider;
    ayni::providers::mock::Provider presenter;
    WorkNode work;
    ayni_provider_v1 descriptor{}, tool_descriptor{};
    ayni_task_call_result_v1 seen_result{};
    std::string reply, proposal;
    std::string prompt = "say hello";
    uint64_t command = 0, last_goal = 0;
};

} // namespace ayni::test
#endif

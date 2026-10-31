#include "workspace_fixture.h"
// Public normal shell lifecycle, ADR-0085: explicit Grant, canonical input,
// real bounded worker execution, structured post and joined shutdown.
#include "work_node.h"

#include <ayni/providers/shell.h>

#include <chrono>
#include <thread>

namespace {
using namespace ayni::test;

// The same trusted deployment is exercised with and without its explicit
// unrestricted-shell Grant. Missing authority creates a proposal without I/O.
TEST(KernelShell, explicit_authority_controls_real_pipeline_and_structured_result_delivery) {
    for (const bool authorized : {false, true}) {
        SCOPED_TRACE(authorized);
        ayni::providers::shell::Provider shell;
        ayni::providers::shell::Config shell_config;
        shell_config.working_directory = "/tmp";
        shell_config.command_max_bytes = 128;
        shell_config.output_max_bytes = 128;
        shell_config.timeout_ms = 1000;
        ASSERT_EQ(shell.configure(shell_config), AYNI_OK);
        ayni_provider_v1 provider{};
        ayni_capability_desc_v1 authority{};
        ASSERT_EQ(shell.describe(provider), AYNI_OK);
        ASSERT_EQ(shell.describe_authority(authority), AYNI_OK);
        ASSERT_NE(provider.capabilities[0].business, nullptr);
        ASSERT_EQ(ayni_schema_validate(::ayni::test::value_workspace(),
                                       &provider.capabilities[0].business->input),
                  AYNI_OK);
        ASSERT_EQ(ayni_schema_validate(::ayni::test::value_workspace(),
                                       &provider.capabilities[0].business->result),
                  AYNI_OK);
        WorkNode work;
        auto &config = work.node().config();
        config.call_capacity = 4;
        config.call_capability_capacity = 1;
        config.call_input_max_bytes = 512;
        config.call_result_max_bytes = 1024;
        config.provider_capacity = 1;
        config.binding_capacity = 1;
        config.goal_type_capacity = 1;
        config.resource_capacity = 1;
        config.resource_request_capacity = 4;
        config.storage_buffer_bytes = 0;
        work.node().on_create([&](auto *kernel) {
            const auto owner = owner_desc();
            EXPECT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            const auto agent = agent_desc(agent_a);
            EXPECT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            const auto strategy =
                strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
            EXPECT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
            auto type = goal_type_desc("shell", 1);
            type.export_requirements = [](void *, ayni_bytes_v1, const auto *sink) {
                const ayni_goal_requirement_v1 need{
                    sizeof(need), 1, text("system.shell"), {sizeof(ayni_scope_v1), 0, nullptr}};
                return sink->emit(sink->ctx, &need);
            };
            EXPECT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
            EXPECT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
            const ayni_resource_desc_v1 resource{sizeof(resource),
                                                 AYNI_RESOURCE_SHARED,
                                                 text("shell.slot"),
                                                 1,
                                                 0,
                                                 0,
                                                 0,
                                                 nullptr,
                                                 {}};
            EXPECT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
            EXPECT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                      AYNI_OK);
            EXPECT_EQ(ayni_register_call_capability(kernel, provider.capabilities), AYNI_OK);
            EXPECT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
            const ayni_binding_v1 binding{sizeof(binding),
                                          1,
                                          text("shell.binding"),
                                          text("system.shell"),
                                          1,
                                          1,
                                          provider.provider_id,
                                          provider.endpoint_id,
                                          provider.destination_id,
                                          0,
                                          0,
                                          nullptr};
            EXPECT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        });
        std::string output, error, encoding;
        int64_t exit_code = -1;
        work.strategy().answer = [&](const auto &snapshot, auto &policy) {
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            const auto read =
                snapshot.call_result(snapshot.call_context, text("pipeline"), &result);
            if (read == AYNI_OK) {
                std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
                uint32_t count = 0;
                EXPECT_EQ(ayni_value_read(::ayni::test::value_workspace(), result.encoded_value,
                                          nodes.data(), static_cast<uint32_t>(nodes.size()),
                                          &count),
                          AYNI_OK);
                if (count == 7U) {
                    exit_code = nodes[2].value.integer;
                    encoding = string_of(nodes[4].value.text);
                    output = string_of(nodes[5].value.text);
                    error = string_of(nodes[6].value.text);
                }
                return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                    : AYNI_POLICY_REPORT_FAILURE,
                              snapshot, policy);
            }
            if (read != AYNI_NOT_FOUND) {
                return read;
            }
            std::array<ayni_value_node_v1, 2> nodes{};
            nodes[0].kind = AYNI_VALUE_OBJECT;
            nodes[0].subtree_end = 2;
            nodes[1].kind = AYNI_VALUE_TEXT;
            nodes[1].field_id = 1;
            nodes[1].subtree_end = 2;
            nodes[1].value.text = text("printf hi | /usr/bin/tr a-z A-Z; printf ERR >&2; exit 7");
            ayni_call_request_v1 request{};
            request.struct_size = sizeof(request);
            request.kind = AYNI_POLICY_REQUEST_CALL;
            request.binding_version = 1;
            request.binding_id = text("shell.binding");
            request.step_key = text("pipeline");
            request.value = {nodes.data(), static_cast<uint32_t>(nodes.size())};
            return policy.request_call(policy.call_context, &request);
        };
        work.run();
        if (authorized) {
            ayni_grant_create_v1 grant{};
            grant.struct_size = sizeof(grant);
            grant.capability_version = 1;
            grant.capability_id = text("system.shell");
            grant.subject_instance_id = work.instance(agent_a);
            grant.lifetime = AYNI_GRANT_PERMANENT;
            grant.scope = {sizeof(ayni_scope_v1), 0, nullptr};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = 100;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        }
        for (uint32_t at = 0; at < 20U; ++at) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(10000);
            work.node().step();
        }
        const auto create =
            WorkNode::create_body(work.instance(agent_a), 0, "shell", 1, {}, AYNI_TRIGGER_ONCE);
        const auto goal = work.create(create);
        ASSERT_EQ(goal.status, AYNI_OK);
        // ADR-0085: await the real worker's terminal rather than assuming that
        // 500 simulated advances cover its 1s execution budget under sanitizers.
        // The bounded harness wait does not change the Provider's timeout or result checks.
        constexpr auto completion_wait = std::chrono::seconds(5);
        const auto completion_deadline = std::chrono::steady_clock::now() + completion_wait;
        while (std::chrono::steady_clock::now() < completion_deadline) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(1000);
            work.node().step();
            ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
            const auto state = work.goal(goal.result.object.id).lifecycle;
            if (state == AYNI_GOAL_PROPOSED || state == AYNI_GOAL_SATISFIED) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (authorized) {
            EXPECT_EQ(work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_SATISFIED);
            EXPECT_EQ(exit_code, 7);
            EXPECT_EQ(encoding, "base64");
            EXPECT_EQ(output, "SEk=");
            EXPECT_EQ(error, "RVJS");
        } else {
            EXPECT_EQ(work.goal(goal.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
            EXPECT_TRUE(output.empty());
        }
        EXPECT_EQ(shell.status(), AYNI_OK);
        ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
        work.node().drain();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
        auto joined = shell.quiesce();
        for (uint32_t at = 0; at < 200U && joined == AYNI_BUSY; ++at) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            joined = shell.quiesce();
        }
        ASSERT_EQ(joined, AYNI_OK);
        ASSERT_EQ(work.node().destroy(), AYNI_OK);
        EXPECT_EQ(shell.close(), AYNI_OK);
    }
}
} // namespace

#include "strategy_workspace_fixture.h"
// Public ABI integration for ADR-0056, with deterministic local Provider receipts.
#ifndef AYNI_TEST_STRATEGIES_FIXTURE_H
#define AYNI_TEST_STRATEGIES_FIXTURE_H
#include "function_proposal.h"
#include "work_node.h"
#include <ayni/providers/software/work.h>
#include <ayni/strategies/builtin.h>
#include <ayni/values.hpp>
#include <ayni/values_json.hpp>
#include <chrono>

namespace ayni::test {
namespace policies = ayni::strategies;
class StrategiesFixture : public ::testing::Test {
  protected:
    struct Pending {
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        std::string capability;
        std::string text;
        uint64_t operation;
        bool ended = false;
    };
    bool action_tool = false;
    bool software_tools = false;
    ayni::providers::software::WorkProvider software;
    uint32_t tool_alias_count = 0;
    policies::HistoryPort history_port{};
    ayni_model_usage_v1 service_usage{AYNI_MODEL_USAGE_REPORTED, 0, 100, 10, 110};
    void SetUp() override {
        steps[0] = {text("inspect"),
                    1,
                    {AYNI_POLICY_REQUEST_CALL, text("echo"), 1, empty_scope(), text("first")}};
        steps[1] = {text("notify"),
                    1,
                    {AYNI_POLICY_REQUEST_CALL, text("echo"), 1, empty_scope(), text("second")}};
        rule = {text("routine"), 1, steps.data(), 2};
        ASSERT_EQ(rules.configure({text("rules"), 1, nullptr, 0}, &rule, 1), AYNI_OK);
        rules_desc = rules.describe();
        policies::ToolLoopConfig cfg;
        cfg.identity = {text("loop"), 1, nullptr, 0};
        cfg.model = {AYNI_POLICY_REQUEST_MODEL, text("model"), 1, empty_scope(),
                     text("Use tools if needed.")};
        cfg.summary = {AYNI_POLICY_REQUEST_MODEL, text("summary"), 1, empty_scope(), {}};
        cfg.context_window_tokens = 5000;
        cfg.output_reserve_tokens = 1000;
        cfg.history = history_port;
        ASSERT_EQ(loop.configure(cfg, ::ayni::test::loop_workspace()), AYNI_OK);
        auto &c = work.node().config();
        c.storage_buffer_bytes = 0;
        c.goal_parameter_max_bytes = policies::state_bytes_max;
        c.task_checkpoint_max_bytes = policies::state_bytes_max;
        c.task_step_capacity = 64;
        c.task_capacity = 8;
        c.strategy_capacity = 2;
        c.call_capacity = 24;
        c.call_input_max_bytes = policies::prompt_bytes_max + 36;
        c.call_result_max_bytes = 4096;
        c.call_capability_capacity = software_tools ? 5 : 3;
        c.capability_capacity = software_tools ? 5 : 3;
        c.provider_capacity = software_tools ? 2 : 1;
        c.binding_capacity = (software_tools ? 5 : 3) + tool_alias_count;
        c.goal_type_capacity = 2;
        c.resource_capacity = software_tools ? 2 : 1;
        if (software_tools) {
            c.observation_source_capacity = 1;
            ayni::providers::software::WorkConfig config;
            config.clock_context = this;
            config.now_us = [](void *context) {
                return static_cast<StrategiesFixture *>(context)->work.node().sim().now_us();
            };
            ASSERT_EQ(software.configure(config), AYNI_OK);
        }
        c.resource_request_capacity = 4;
        work.node().on_create([this](ayni_kernel_t *kernel) { profile(kernel); });
        work.run();
        for (const char *id : {"model", "summary", "echo", "software.sum", "software.events"}) {
            if (!software_tools && std::strncmp(id, "software.", 9) == 0) {
                continue;
            }
            const ayni_grant_create_v1 grant{
                sizeof(grant),        1, text(id), work.instance(agent_a), 0,
                AYNI_GRANT_PERMANENT, 0, 0,        empty_scope()};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = ++command;
            const auto accepted = work.submit(intent);
            ASSERT_EQ(accepted.status, AYNI_OK);
            if (std::strcmp(id, "echo") == 0) {
                echo_grant = accepted.result.object.id;
            }
        }
        pump();
        conversation.id = 1;
        conversation.owner_instance_id = work.instance(agent_a);
    }
    static ayni_scope_v1 empty_scope() { return {sizeof(ayni_scope_v1), 0, nullptr}; }
    void profile(ayni_kernel_t *kernel) {
        auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        ASSERT_EQ(ayni_register_strategy(kernel, &rules_desc), AYNI_OK);
        auto model_strategy = loop.describe();
        ASSERT_EQ(ayni_register_strategy(kernel, &model_strategy), AYNI_OK);
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        const ayni_call_resource_v1 slot{resource.resource_id, 1, 0};
        std::array<ayni_call_capability_v1, 3> offers{};
        const char *ids[] = {"model", "summary", "echo"};
        for (uint32_t at = 0; at < offers.size(); ++at) {
            const auto id = text(ids[at]);
            const ayni_capability_desc_v1 cap{sizeof(cap), 1, id, 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);
            auto &offer = offers[at];
            offer.struct_size = sizeof(offer);
            offer.version = 1;
            offer.capability_id = id;
            const bool model = at != 2;
            offer.contract = {sizeof(ayni_call_contract_v1),
                              !model && action_tool ? AYNI_CALL_ACTION : AYNI_CALL_UNARY,
                              model ? AYNI_CALL_MODEL : AYNI_CALL_TOOL,
                              0,
                              AYNI_CALL_IDEMPOTENT,
                              AYNI_CALL_QUERY_NONE,
                              AYNI_CALL_CANCEL_REQUEST,
                              0,
                              1000000,
                              AYNI_CALL_INPUT_VALUE,
                              at == 0 ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT,
                              policies::prompt_bytes_max + 36,
                              4096,
                              model ? 1U : 0U,
                              model ? &slot : nullptr,
                              nullptr};
            auto &schema = input_schemas[at];
            schema[0].kind = model ? AYNI_VALUE_OBJECT : AYNI_VALUE_TEXT;
            schema[0].subtree_end = model ? 2U : 1U;
            if (model) {
                schema[1].kind = AYNI_VALUE_TEXT;
                schema[1].field_id = AYNI_MODEL_BODY_FIELD_ID;
                schema[1].subtree_end = 2;
                schema[1].flags = AYNI_SCHEMA_REQUIRED;
                schema[1].name = text("body");
                schema[1].max_count = policies::prompt_bytes_max;
            } else {
                schema[0].max_count = policies::prompt_bytes_max;
            }
            auto &result_schema = result_schemas[at];
            result_schema.kind = AYNI_VALUE_TEXT;
            result_schema.subtree_end = 1;
            result_schema.max_count = 4096;
            business[at] = {{schema.data(), model ? 2U : 1U}, {&result_schema, 1}, 0, 0, nullptr};
            if (!model && structured_tool) {
                auto &input = input_schemas[at];
                input = {};
                input[0].kind = AYNI_VALUE_OBJECT;
                input[0].subtree_end = 4;
                input[1].kind = AYNI_VALUE_LIST;
                input[1].field_id = 7;
                input[1].subtree_end = 4;
                input[1].flags = AYNI_SCHEMA_REQUIRED;
                input[1].name = text("items");
                input[1].min_count = 1;
                input[1].max_count = 2;
                input[2].kind = AYNI_VALUE_OBJECT;
                input[2].subtree_end = 4;
                input[3].kind = AYNI_VALUE_DECIMAL;
                input[3].field_id = 3;
                input[3].subtree_end = 4;
                input[3].flags = AYNI_SCHEMA_REQUIRED;
                input[3].name = text("amount");
                input[3].unit = text("dollar");
                input[3].max_i64 = 100000;
                input[3].scale = 2;
                auto &output = structured_result_schema;
                output[0].kind = AYNI_VALUE_OBJECT;
                output[0].subtree_end = 3;
                output[1].kind = AYNI_VALUE_U64;
                output[1].field_id = 1;
                output[1].subtree_end = 2;
                output[1].flags = AYNI_SCHEMA_REQUIRED;
                output[1].name = text("total");
                output[1].max_u64 = UINT64_MAX;
                output[2].kind = AYNI_VALUE_DECIMAL;
                output[2].field_id = 2;
                output[2].subtree_end = 3;
                output[2].flags = AYNI_SCHEMA_REQUIRED;
                output[2].name = text("price");
                output[2].unit = text("dollar");
                output[2].max_i64 = 100000;
                output[2].scale = 2;
                business[at].input = {input.data(), 4};
                business[at].result = {output.data(), 3};
                offer.contract.result_format = AYNI_CALL_RESULT_VALUE;
            }
            offer.business = &business[at];
            ASSERT_EQ(ayni_register_call_capability(kernel, &offer), AYNI_OK);
        }
        ayni_provider_v1 provider{};
        provider.struct_size = sizeof(provider);
        provider.version = 1;
        provider.provider_id = text("scripted.io");
        provider.endpoint_id = text("local");
        provider.ctx = this;
        provider.capability_count = static_cast<uint32_t>(offers.size());
        provider.capabilities = offers.data();
        provider.submit = [](void *context, const ayni_provider_request_v1 *request,
                             ayni_provider_completion_t *completion) {
            auto &self = *static_cast<StrategiesFixture *>(context);
            const bool model = string_of(request->capability_id) != "echo";
            std::string copied;
            if (!model && self.structured_tool) {
                std::array<char, 4096> bytes{};
                ayni::json::Writer json(bytes.data(), bytes.size());
                const auto status =
                    ayni::json::write_value(json, self.business[2].input, request->value);
                size_t size = 0;
                if (status != AYNI_OK || !json.finish(size)) {
                    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
                }
                copied.assign(bytes.data(), size);
            } else {
                ValueReader reader;
                ayni_string_v1 body{};
                if (reader.read(request->value) != AYNI_OK ||
                    !ValueReader::text(
                        model ? reader.field(AYNI_MODEL_BODY_FIELD_ID) : reader.root(), body)) {
                    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
                }
                copied = string_of(body);
            }
            self.pending.push_back({request->attempt, completion, string_of(request->capability_id),
                                    copied, request->operation_id, false});
            return AYNI_SUBMIT_ACCEPTED;
        };
        provider.cancel = [](void *context, ayni_attempt_ref_v1 attempt) {
            auto &self = *static_cast<StrategiesFixture *>(context);
            for (uint32_t at = 0; at < self.pending.size(); ++at) {
                const auto &p = self.pending[at];
                if (!p.ended && p.attempt.request_id == attempt.request_id &&
                    p.attempt.boot_epoch == attempt.boot_epoch) {
                    self.post(at, {}, AYNI_CALL_CANCELLED, AYNI_OK);
                }
            }
            return AYNI_SUBMIT_ACCEPTED;
        };
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        for (const auto &offer : offers) {
            const ayni_binding_v1 binding{sizeof(binding),
                                          1,
                                          offer.capability_id,
                                          offer.capability_id,
                                          1,
                                          1,
                                          provider.provider_id,
                                          provider.endpoint_id,
                                          {},
                                          0,
                                          0,
                                          nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        }
        const ayni_tool_usage_v1 usage{sizeof(usage),
                                       1,
                                       text("echo"),
                                       1,
                                       text("echo"),
                                       text("Echo input."),
                                       text("Native input."),
                                       text("Echo output."),
                                       text("Echo hello."),
                                       0,
                                       0,
                                       nullptr};
        ASSERT_EQ(ayni_register_capability_usage(kernel, text("Echo."), &usage), AYNI_OK);
        std::vector<std::string> aliases;
        std::vector<ayni_tool_ref_v1> allowed;
        aliases.reserve(tool_alias_count);
        for (uint32_t at = 0; at < tool_alias_count; ++at) {
            aliases.push_back("tool." + std::to_string(at));
        }
        for (const auto &alias : aliases) {
            const ayni_string_v1 id{alias.data(), static_cast<uint32_t>(alias.size())};
            const ayni_binding_v1 binding{sizeof(binding),
                                          1,
                                          id,
                                          text("echo"),
                                          1,
                                          1,
                                          provider.provider_id,
                                          provider.endpoint_id,
                                          {},
                                          0,
                                          0,
                                          nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
            auto adopted = usage;
            adopted.binding_id = id;
            ASSERT_EQ(ayni_register_capability_usage(kernel, text("Echo alias."), &adopted),
                      AYNI_OK);
            allowed.push_back({id, 1, AYNI_TOOL_ROLE_CAPABILITY});
        }
        if (software_tools) {
            register_software(kernel);
            allowed.push_back({text("software.sum"), 1, AYNI_TOOL_ROLE_CAPABILITY});
            allowed.push_back({text("software.events"), 1, AYNI_TOOL_ROLE_CAPABILITY});
        }
        if (allowed.empty()) {
            allowed.push_back({text("echo"), 1, AYNI_TOOL_ROLE_CAPABILITY});
        }
        for (const char *type : {"routine", "chat"}) {
            const bool chat = std::strcmp(type, "chat") == 0;
            auto goal =
                goal_type_desc(type, 1, chat ? +[](void *, ayni_bytes_v1 parameters) {
                    return policies::Conversation::validate_turn(
                        const_cast<ayni_workspace_v1 *>(::ayni::test::conversation_workspace()), parameters);
                } : nullptr);
            goal.strategy_id = chat ? model_strategy.strategy_id : rules_desc.strategy_id;
            goal.ctx = chat ? this : nullptr;
            goal.tool_authorization = chat ? AYNI_TOOLS_ON_DEMAND : AYNI_TOOLS_REQUIRED;
            goal.tool_count = chat ? static_cast<uint32_t>(allowed.size()) : 0U;
            goal.tools = chat ? allowed.data() : nullptr;
            goal.export_requirements = [](void *context, ayni_bytes_v1,
                                          const ayni_goal_requirement_sink_v1 *sink) {
                for (const char *id : {"model", "summary", "echo"}) {
                    if (context && std::strcmp(id, "echo") == 0) {
                        continue;
                    }
                    const ayni_goal_requirement_v1 req{sizeof(req), 1, text(id), empty_scope()};
                    const auto status = sink->emit(sink->ctx, &req);
                    if (status != AYNI_OK) {
                        return status;
                    }
                }
                return AYNI_OK;
            };
            ASSERT_EQ(ayni_register_goal_type(kernel, &goal), AYNI_OK);
        }
    }
    // ADR-0086: an actual cooperative software backend uses only public registration ports.
    void register_software(ayni_kernel_t *kernel) {
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, text("software.slot"), 4, 0, 0, 0, nullptr, {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ayni_observation_source_v1 source{};
        ASSERT_EQ(software.describe_source(source), AYNI_OK);
        ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
        ayni_provider_v1 provider{};
        ASSERT_EQ(software.describe(provider), AYNI_OK);
        for (uint32_t at = 0; at < provider.capability_count; ++at) {
            const auto &offer = provider.capabilities[at];
            const ayni_capability_desc_v1 capability{
                sizeof(capability), 1, offer.capability_id, 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &capability), AYNI_OK);
            ASSERT_EQ(ayni_register_call_capability(kernel, &offer), AYNI_OK);
        }
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        for (uint32_t at = 0; at < provider.capability_count; ++at) {
            const auto id = provider.capabilities[at].capability_id;
            const ayni_binding_v1 binding{
                sizeof(binding),      1,  id, id, 1,      1, provider.provider_id,
                provider.endpoint_id, {}, 0,  0,  nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
            const ayni_tool_usage_v1 usage{sizeof(usage),
                                           1,
                                           id,
                                           1,
                                           id,
                                           text("Software work."),
                                           text("Native input."),
                                           text("Native result."),
                                           text("Start or invoke with Schema input."),
                                           0,
                                           0,
                                           nullptr};
            ASSERT_EQ(ayni_register_capability_usage(kernel, text("Cooperative software."), &usage),
                      AYNI_OK);
        }
    }
    void pump(uint32_t count = 160) {
        for (uint32_t at = 0; at < count; ++at) {
            work.node().step();
            work.node().sim().complete_storage();
            work.node().sim().advance_us(1000);
        }
    }
    uint64_t start(const char *type = "chat", const char *input = "hello",
                   uint32_t model_limit = AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT) {
        policies::Encoded parameters;
        if (std::strcmp(type, "chat") == 0) {
            EXPECT_EQ(policies::Conversation::begin(::ayni::test::conversation_workspace(),
                                                    conversation, text(input), parameters),
                      AYNI_OK);
        }
        auto body = WorkNode::create_body(work.instance(agent_a), 0, type, 1, parameters.view(),
                                          AYNI_TRIGGER_ONCE);
        const auto made = work.create(body);
        EXPECT_EQ(made.status, AYNI_OK);
        pump();
        const auto task = work.goal(made.result.object.id).last_task_id;
        if (model_limit > AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT) {
            const ayni_task_raise_model_limit_v1 raise{sizeof(raise), model_limit, task};
            auto intent =
                work.intent(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &raise, work.task(task).revision);
            intent.command_id = ++command;
            EXPECT_EQ(work.submit(intent).status, AYNI_OK);
            pump();
        }
        return task;
    }
    void post(uint32_t index, ayni_string_v1 text_result = {},
              ayni_call_outcome_t outcome = AYNI_CALL_COMPLETED, ayni_status_t status = AYNI_OK,
              const ayni_tool_call_v1 *tools = nullptr, uint32_t count = 0) {
        ASSERT_LT(index, pending.size());
        auto &p = pending[index];
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.attempt = p.attempt;
        result.outcome = outcome;
        result.status = status;
        if (p.capability == "model" || p.capability == "summary") {
            result.model_usage = service_usage;
        }
        result.effect = outcome == AYNI_CALL_ABANDONED   ? AYNI_CALL_EFFECT_UNKNOWN
                        : outcome == AYNI_CALL_COMPLETED ? AYNI_CALL_EFFECT_COMPLETE
                                                         : AYNI_CALL_EFFECT_NONE;
        result.result = text_result;
        result.tool_call_count = count;
        result.tool_calls = tools;
        ASSERT_EQ(ayni_provider_post(p.completion, &result), AYNI_OK);
        p.ended = true;
    }
    void propose(uint32_t at, uint32_t count = 2, const char *binding = "echo") {
        ayni_argument_node_v1 argument{};
        argument.kind = AYNI_ARGUMENT_STRING;
        argument.subtree_end = 1;
        argument.value.text = text("tool body");
        FunctionProposal proposal(text(binding), {&argument, 1});
        auto call = proposal.call();
        std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> tools{};
        std::array<std::string, AYNI_CALL_TOOL_COUNT_MAX> ids{};
        for (uint32_t index = 0; index < count; ++index) {
            ids[index] = "fixture_" + std::to_string(at) + "_" + std::to_string(index);
            tools[index] = call;
            tools[index].protocol_id = {ids[index].data(),
                                        static_cast<uint32_t>(ids[index].size())};
        }
        post(at, {}, AYNI_CALL_COMPLETED, AYNI_OK, tools.data(), count);
        pump();
    }
    uint64_t start_until_budget_summary() {
        // ADR-0085: short histories do not compact at a fixed round count.
        const std::string question(policies::turn_text_max, 'q');
        const std::string answer(policies::turn_text_max, 'a');
        constexpr uint32_t search_turns = 8;
        for (uint32_t at = 0; at < search_turns; ++at) {
            if (at == policies::retained_rounds_max) {
                conversation.last_usage = {AYNI_MODEL_USAGE_REPORTED, 0, 3100, 20, 3120};
            }
            const auto task = start("chat", question.c_str());
            if (pending.empty()) {
                return 0;
            }
            const auto call = static_cast<uint32_t>(pending.size() - 1U);
            if (pending[call].capability == "summary") {
                return task;
            }
            EXPECT_EQ(pending[call].capability, "model");
            post(call, {answer.data(), static_cast<uint32_t>(answer.size())});
            pump();
            finish(task);
        }
        return 0;
    }
    void finish(uint64_t task) {
        policies::Encoded state;
        ASSERT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                            policies::state_bytes_max, &state.size),
                  AYNI_OK);
        policies::ConversationRecord candidate;
        ASSERT_EQ(policies::Conversation::finish(::ayni::test::conversation_workspace(),
                                                 conversation, state.view(), candidate),
                  AYNI_OK);
        // Simulated successful outer-store commit; until here originals remain.
        conversation = candidate;
    }
    void TearDown() override {
        if (!work.kernel() || work.node().lifecycle() == AYNI_LIFECYCLE_STOPPED) {
            return;
        }
        for (uint32_t at = 0; at < pending.size(); ++at) {
            if (!pending[at].ended) {
                post(at, {}, AYNI_CALL_CANCELLED, AYNI_OK);
            }
        }
        ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
        if (software_tools) {
            for (uint32_t at = 0; at < 160 && software.pending(); ++at) {
                pump(1);
                ASSERT_EQ(software.advance(), AYNI_OK);
            }
            ASSERT_EQ(software.pending(), 0U);
        }
        work.node().drain();
    }
    std::array<std::array<ayni_schema_node_v1, 4>, 3> input_schemas{};
    std::array<ayni_schema_node_v1, 3> structured_result_schema{};
    bool structured_tool = false;
    std::array<ayni_schema_node_v1, 3> result_schemas{};
    std::array<ayni_business_contract_v1, 3> business{};
    WorkNode work;
    policies::Rules rules;
    policies::ToolLoop loop;
    std::array<policies::RuleStep, 2> steps{};
    policies::Rule rule;
    ayni_strategy_v1 rules_desc{};
    std::vector<Pending> pending;
    policies::ConversationRecord conversation;
    uint64_t command = 600;
    uint64_t echo_grant = 0;
};
} // namespace ayni::test
#endif

// Public ABI integration for ADR-0056, with deterministic local Provider receipts.
#ifndef AYNI_TEST_STRATEGIES_FIXTURE_H
#define AYNI_TEST_STRATEGIES_FIXTURE_H
#include "work_node.h"
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
        ASSERT_EQ(loop.configure(cfg), AYNI_OK);
        auto &c = work.node().config();
        c.storage_buffer_bytes = 0;
        c.goal_parameter_max_bytes = policies::state_bytes_max;
        c.task_checkpoint_max_bytes = policies::state_bytes_max;
        c.task_step_capacity = 64;
        c.task_capacity = 8;
        c.strategy_capacity = 2;
        c.call_capacity = 24;
        c.call_input_max_bytes = policies::prompt_bytes_max + 36;
        c.call_result_max_bytes = 1024;
        c.call_capability_capacity = 3;
        c.capability_capacity = 3;
        c.provider_capacity = 1;
        c.binding_capacity = 3;
        c.goal_type_capacity = 2;
        c.resource_capacity = 1;
        c.resource_request_capacity = 4;
        work.node().on_create([this](ayni_kernel_t *kernel) { profile(kernel); });
        work.run();
        for (const char *id : {"model", "summary", "echo"}) {
            const ayni_grant_create_v1 grant{
                sizeof(grant),        1, text(id), work.instance(agent_a), 0,
                AYNI_GRANT_PERMANENT, 0, 0,        empty_scope()};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = ++command;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
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
                              AYNI_CALL_UNARY,
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
                              1024,
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
            result_schema.max_count = 1024;
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
                std::array<char, 1024> bytes{};
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
                                          {}};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        }
        const ayni_tool_ref_v1 allowed{text("echo"), 1, 0};
        for (const char *type : {"routine", "chat"}) {
            const bool chat = std::strcmp(type, "chat") == 0;
            auto goal =
                goal_type_desc(type, 1, chat ? policies::Conversation::validate_turn : nullptr);
            goal.strategy_id = chat ? model_strategy.strategy_id : rules_desc.strategy_id;
            goal.tool_count = chat ? 1U : 0U;
            goal.tools = chat ? &allowed : nullptr;
            goal.export_requirements = [](void *, ayni_bytes_v1,
                                          const ayni_goal_requirement_sink_v1 *sink) {
                for (const char *id : {"model", "summary", "echo"}) {
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
    void pump(uint32_t count = 160) {
        for (uint32_t at = 0; at < count; ++at) {
            work.node().step();
            work.node().sim().complete_storage();
            work.node().sim().advance_us(1000);
        }
    }
    uint64_t start(const char *type = "chat", const char *input = "hello") {
        policies::Encoded parameters;
        if (std::strcmp(type, "chat") == 0) {
            EXPECT_EQ(policies::Conversation::begin(conversation, text(input), parameters),
                      AYNI_OK);
        }
        const auto body = WorkNode::create_body(work.instance(agent_a), 0, type, 1,
                                                parameters.view(), AYNI_TRIGGER_ONCE);
        const auto made = work.create(body);
        EXPECT_EQ(made.status, AYNI_OK);
        pump();
        return work.goal(made.result.object.id).last_task_id;
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
        ValueBuilder<1> value;
        value.text(0, text("tool body"));
        ayni_tool_call_v1 call{};
        call.struct_size = sizeof(call);
        call.binding_version = 1;
        call.binding_id = text(binding);
        call.input_kind = AYNI_TOOL_INPUT_VALUE;
        ASSERT_EQ(value.finish(call.input.value), AYNI_OK);
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
    void finish(uint64_t task) {
        policies::Encoded state;
        ASSERT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                            policies::state_bytes_max, &state.size),
                  AYNI_OK);
        policies::ConversationRecord candidate;
        ASSERT_EQ(policies::Conversation::finish(conversation, state.view(), candidate), AYNI_OK);
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
};
} // namespace ayni::test
#endif

#include "workspace_fixture.h"
// Mock tools service, ADR-0054: a deterministic TOOL_CALLS model whose
// proposal a strategy requests from a second Mock, through public ABI only.
#include "mock_display_schema.h"
#include "work_node.h"
#include <ayni/providers/mock.h>
#include <ayni/values.hpp>
#include <limits>

namespace {
using namespace ayni::test;
using ayni::providers::mock::Provider;
using ayni::providers::mock::Service;
using ayni::providers::mock::Variant;

constexpr uint32_t seconds_field = 1;
constexpr uint32_t voice_field = 2;

class MockTools : public ::testing::Test {
  protected:
    MockDisplaySchema display;
    std::vector<int64_t> received_amounts;
    static constexpr uint32_t body_field = MockDisplaySchema::body_field;
    virtual void extend_schema() {}
    virtual uint32_t model_budget() const { return 512; }
    void SetUp() override {
        extend_schema();
        ASSERT_EQ(planner.configure({Variant::a, Service::tools, 1, model_budget()}), AYNI_OK);
        ASSERT_EQ(planner.describe(planner_desc), AYNI_OK);
        ASSERT_EQ(presenter.configure(
                      {Variant::b, Service::action, 1, 64, {}, &display.business, body_field}),
                  AYNI_OK);
        ASSERT_EQ(presenter.describe(presenter_desc), AYNI_OK);
        EXPECT_EQ(string_of(planner_desc.provider_id), "mock.a.tools");
        EXPECT_EQ(planner_desc.capabilities->contract.result_format, AYNI_CALL_RESULT_TOOL_CALLS);
        auto &config = work.node().config();
        config.call_capacity = 4;
        config.storage_buffer_bytes = 0;
        config.call_capability_capacity = 2;
        config.provider_capacity = 2;
        config.binding_capacity = 2;
        config.call_input_max_bytes = 384; // The 128-byte text and its framing.
        config.call_result_max_bytes = model_budget();
        config.resource_capacity = 2;
        config.resource_request_capacity = 4;
        work.node().on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            return decide(snapshot, output);
        };
        work.run();
        for (const auto *offer : {planner_desc.capabilities, presenter_desc.capabilities}) {
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
        pump(12);
    }
    void TearDown() override {
        if (work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED) {
            ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
            for (unsigned turn = 0; turn < 100 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED;
                 ++turn) {
                pump(1);
                ASSERT_EQ(planner.advance(), AYNI_OK);
                ASSERT_EQ(presenter.advance(), AYNI_OK);
            }
            EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
        }
        ASSERT_EQ(work.node().destroy(), AYNI_OK);
        EXPECT_EQ(planner.close(), AYNI_OK);
        EXPECT_EQ(presenter.close(), AYNI_OK);
    }

    static ayni_authority_value_set_v1 values(ayni_authority_value_kind_t kind, const char *unit,
                                              int64_t min, int64_t max,
                                              const ayni_string_v1 *members, uint32_t count) {
        ayni_authority_value_set_v1 set{};
        set.struct_size = sizeof(set);
        set.kind = kind;
        if (kind == AYNI_AUTHORITY_I64) {
            set.unit = text(unit);
            set.values.integer = {min, max};
        } else {
            set.values.tokens = {members, count, 0};
        }
        return set;
    }

    void register_profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        const auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        static const ayni_string_v1 voices[] = {text("bright"), text("calm")};
        const ayni_authority_field_desc_v1 fields[] = {
            {sizeof(ayni_authority_field_desc_v1), seconds_field, AYNI_AUTHORITY_PARAMETER, 0,
             values(AYNI_AUTHORITY_I64, "second", 1, 60, nullptr, 0)},
            {sizeof(ayni_authority_field_desc_v1), voice_field, AYNI_AUTHORITY_PARAMETER, 0,
             values(AYNI_AUTHORITY_ENUM, nullptr, 0, 0, voices, 2)},
        };
        const ayni_capability_desc_v1 authorities[] = {
            {sizeof(ayni_capability_desc_v1), 1, planner_desc.capabilities->capability_id, 0, 0,
             nullptr},
            {sizeof(ayni_capability_desc_v1), 1, presenter_desc.capabilities->capability_id, 2, 0,
             fields},
        };
        for (const auto &authority : authorities) {
            ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        }
        for (const auto *desc : {&planner_desc, &presenter_desc}) {
            const auto &offer = *desc->capabilities;
            const ayni_resource_desc_v1 resource{sizeof(resource),
                                                 AYNI_RESOURCE_SHARED,
                                                 offer.contract.resources[0].resource_id,
                                                 1,
                                                 0,
                                                 0,
                                                 0,
                                                 nullptr,
                                                 {}};
            ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
            ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                      AYNI_OK);
            ASSERT_EQ(ayni_register_call_capability(kernel, &offer), AYNI_OK);
            ASSERT_EQ(ayni_register_provider(kernel, desc), AYNI_OK);
        }
        const ayni_binding_v1 bindings[] = {
            {sizeof(ayni_binding_v1),
             1,
             text("mock.plan"),
             planner_desc.capabilities->capability_id,
             1,
             1,
             planner_desc.provider_id,
             planner_desc.endpoint_id,
             {},
             0,
             0,
             nullptr},
            {sizeof(ayni_binding_v1),
             1,
             text("mock.show"),
             presenter_desc.capabilities->capability_id,
             1,
             1,
             presenter_desc.provider_id,
             presenter_desc.endpoint_id,
             {},
             0,
             0,
             nullptr},
        };
        for (const auto &binding : bindings) {
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        }
        static const ayni_tool_ref_v1 tools[] = {{text("mock.show"), 1, 0}};
        auto goal_type = goal_type_desc("assist", 1, nullptr, this);
        goal_type.tool_count = 1;
        goal_type.tools = tools;
        goal_type.export_requirements = [](void *context, ayni_bytes_v1,
                                           const ayni_goal_requirement_sink_v1 *sink) {
            const auto &self = *static_cast<MockTools *>(context);
            const ayni_goal_requirement_v1 plan{sizeof(ayni_goal_requirement_v1),
                                                1,
                                                self.planner_desc.capabilities->capability_id,
                                                {sizeof(ayni_scope_v1), 0, nullptr}};
            static const ayni_string_v1 calm = text("calm");
            const ayni_scope_field_v1 ceiling[] = {
                {sizeof(ayni_scope_field_v1), seconds_field,
                 values(AYNI_AUTHORITY_I64, "second", 5, 10, nullptr, 0)},
                {sizeof(ayni_scope_field_v1), voice_field,
                 values(AYNI_AUTHORITY_ENUM, nullptr, 0, 0, &calm, 1)},
            };
            const ayni_goal_requirement_v1 show{sizeof(ayni_goal_requirement_v1),
                                                1,
                                                self.presenter_desc.capabilities->capability_id,
                                                {sizeof(ayni_scope_v1), 2, ceiling}};
            const auto status = sink->emit(sink->ctx, &plan);
            return status != AYNI_OK ? status : sink->emit(sink->ctx, &show);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &goal_type), AYNI_OK);
    }

    // Plans with the tools Mock, then requests its first proposal from the presenter.
    ayni_status_t decide(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
        ayni_task_call_result_v1 result{};
        result.struct_size = sizeof(result);
        if (snapshot.call_result(snapshot.call_context, text("act"), &result) == AYNI_OK) {
            acted = string_of(result.text);
            return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                : AYNI_POLICY_REPORT_FAILURE,
                          snapshot, output);
        }
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
                                         text("mock.plan"),
                                         {},
                                         {},
                                         {},
                                         AYNI_CALL_MODE_FOREGROUND,
                                         0};
            if (value.finish(request.value) != AYNI_OK) {
                return AYNI_INVALID_ARGUMENT;
            }
            return output.request_call(output.call_context, &request);
        }
        if (read != AYNI_OK) {
            return read;
        }
        planned = result;
        planned_text = string_of(result.text);
        if (result.outcome != AYNI_CALL_COMPLETED || result.tool_call_count == 0U) {
            return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                : AYNI_POLICY_REPORT_FAILURE,
                          snapshot, output);
        }
        ayni_task_tool_call_v1 proposed{};
        proposed.struct_size = sizeof(proposed);
        const auto status = snapshot.call_tool(snapshot.call_context, text("plan"), 0, &proposed);
        if (status != AYNI_OK) {
            return status;
        }
        const auto &call = proposed.call;
        std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> source{};
        std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
        std::array<char, 4096> bytes{};
        ayni_value_buffer_v1 value{nodes.data(), static_cast<uint32_t>(nodes.size()), 0,
                                   bytes.data(), static_cast<uint32_t>(bytes.size()), 0};
        proposal_error = AYNI_NOT_FOUND;
        if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS &&
            string_of(call.function_name) == "ayni_call_capability") {
            const auto tree = call.input.arguments;
            uint32_t argument = UINT32_MAX;
            bool binding = false;
            if (tree.nodes[0].kind == AYNI_ARGUMENT_OBJECT) {
                for (uint32_t at = 1; at < tree.node_count; at = tree.nodes[at].subtree_end) {
                    const auto &node = tree.nodes[at];
                    if (string_of(node.name) == "binding_id") {
                        binding = node.kind == AYNI_ARGUMENT_STRING &&
                                  string_of(node.value.text) == "mock.show";
                    }
                    if (string_of(node.name) == "arguments") {
                        argument = at;
                    }
                }
            }
            if (binding && argument != UINT32_MAX) {
                const auto count = tree.nodes[argument].subtree_end - argument;
                for (uint32_t at = 0; at < count; ++at) {
                    source[at] = tree.nodes[argument + at];
                    source[at].subtree_end -= argument;
                }
                source[0].name = {};
                const ayni_arguments_v1 args{source.data(), count};
                proposal_error = ayni_arguments_normalize(::ayni::test::value_workspace(),
                                                          &display.business.input, &args, &value);
            }
        }
        if (proposal_error != AYNI_OK) {
            return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
        }
        ayni_value_v1 normalized{nodes.data(), value.node_count};
        ayni::ValueReader input;
        if (input.read(normalized) != AYNI_OK) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (const auto *amounts = input.field(3)) {
            received_amounts.clear();
            for (uint32_t at = static_cast<uint32_t>(amounts - normalized.nodes) + 1U;
                 at < amounts->subtree_end; ++at) {
                received_amounts.push_back(normalized.nodes[at].value.decimal.coefficient);
            }
        }
        const auto *seconds = input.field(seconds_field);
        const auto *voice = input.field(voice_field);
        const auto *body = input.field(body_field);
        if (!seconds || !voice || !body) {
            return AYNI_INVALID_ARGUMENT;
        }
        proposal = std::string("mock.show|") + string_of(body->value.text) + "|" +
                   std::to_string(seconds->value.integer) + "second|" +
                   string_of(voice->value.text);
        const ayni_call_request_v1 request{sizeof(request), AYNI_POLICY_REQUEST_CALL,  1,  0,
                                           text("act"),     text("mock.show"),         {}, {},
                                           normalized,      AYNI_CALL_MODE_FOREGROUND, 0};
        return output.request_call(output.call_context, &request);
    }

    uint64_t create() {
        const auto created = work.create(
            WorkNode::create_body(work.instance(agent_a), 0, "assist", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(created.status, AYNI_OK);
        return created.result.object.id;
    }
    void pump(unsigned turns) {
        for (unsigned at = 0; at < turns; ++at) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(10000);
            work.node().step();
        }
    }
    void drive(unsigned turns) {
        for (unsigned at = 0; at < turns; ++at) {
            ASSERT_EQ(planner.advance(), AYNI_OK);
            ASSERT_EQ(presenter.advance(), AYNI_OK);
            pump(2);
        }
    }
    ayni_task_lifecycle_t run(const std::string &input) {
        prompt = input;
        const auto goal = create();
        pump(20);
        drive(20);
        return work.task(work.goal(goal).last_task_id).lifecycle;
    }

    Provider planner;
    Provider presenter;
    WorkNode work; // Destroyed before the Provider contexts if a test exits early.
    ayni_provider_v1 planner_desc{};
    ayni_provider_v1 presenter_desc{};
    uint64_t command = 0;
    std::string prompt;
    ayni_task_call_result_v1 planned{};
    ayni_status_t proposal_error = AYNI_OK;
    std::string planned_text;
    std::string proposal;
    std::string acted;
};

TEST_F(MockTools, generic_function_arguments_run_through_original_schema_and_authority) {
    EXPECT_EQ(run(R"(call:mock.show json:{"text":"hello","voice":"calm","seconds":5})"),
              AYNI_TASK_COMPLETED);
    EXPECT_EQ(planned.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(planned.tool_call_count, 1U);
    EXPECT_EQ(planned_text, "");
    // ADR-0085: deterministic source values still pass the ordinary whole-Grant boundary.
    EXPECT_EQ(proposal, "mock.show|hello|5second|calm");
    EXPECT_EQ(acted, "hello");
    EXPECT_EQ(planner.executions(), 1U);
    EXPECT_EQ(presenter.executions(), 1U);
}

TEST_F(MockTools, input_without_call_words_is_echoed_as_text) {
    EXPECT_EQ(run("just text"), AYNI_TASK_COMPLETED);
    EXPECT_EQ(planned.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(planned.tool_call_count, 0U);
    EXPECT_EQ(planned_text, "just text");
    EXPECT_EQ(proposal, "");
    EXPECT_EQ(presenter.executions(), 0U);
}

TEST_F(MockTools, unknown_binding_is_a_normal_function_refusal_without_business_execution) {
    EXPECT_EQ(run("call:mock.plan hello"), AYNI_TASK_COMPLETED);
    EXPECT_EQ(planned.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(planned.tool_call_count, 1U);
    EXPECT_EQ(proposal_error, AYNI_NOT_FOUND);
    EXPECT_EQ(presenter.executions(), 0U);
}
TEST_F(MockTools, body_over_the_tool_limit_is_refused_after_known_model_completion) {
    const auto input = std::string(R"(call:mock.show json:{"text":")") + std::string(65, 'a') +
                       R"(","voice":"calm","seconds":5})";
    EXPECT_EQ(run(input), AYNI_TASK_COMPLETED);
    EXPECT_EQ(planned.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(proposal_error, AYNI_OK);
    EXPECT_EQ(presenter.executions(), 0U);
}

TEST_F(MockTools, list_over_the_result_limit_fails_as_model_output) {
    std::string input;
    for (uint32_t at = 0; at < AYNI_CALL_TOOL_COUNT_MAX; ++at) {
        input += "call:mock.show ";
    }
    EXPECT_EQ(run(input + "hi"), AYNI_TASK_FAILED);
    EXPECT_EQ(planned.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(planned.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(planned.reason, AYNI_REASON_MODEL_OUTPUT);
}
// ADR-0084: the same deterministic adapter transports named JSON for any original
// Schema; authority is still checked after normalization by Core.
TEST_F(MockTools, named_json_arguments_use_original_schema_and_current_authority) {
    EXPECT_EQ(run(R"(call:mock.show json:{"text":"hello","voice":"calm","seconds":7})"),
              AYNI_TASK_COMPLETED);
    EXPECT_EQ(acted, "hello");
    EXPECT_EQ(presenter.executions(), 1U);
}
class NestedMockTools : public MockTools {
    uint32_t model_budget() const override { return 512; }
    void extend_schema() override {
        display.input[0].subtree_end = 6;
        auto &list = display.input[4];
        list.kind = AYNI_VALUE_LIST;
        list.field_id = 3;
        list.subtree_end = 6;
        list.flags = AYNI_SCHEMA_REQUIRED;
        list.name = text("amounts");
        list.max_count = 4;
        auto &item = display.input[5];
        item.kind = AYNI_VALUE_DECIMAL;
        item.subtree_end = 6;
        item.scale = 2;
        item.max_i64 = 100;
        display.business.input.node_count = 6;
    }
};
TEST_F(NestedMockTools, named_json_keeps_nested_list_order_duplicates_and_exact_decimals) {
    EXPECT_EQ(
        run(R"(call:mock.show json:{"seconds":7,"voice":"calm","text":"hello","amounts":["0.10","0.20","0.10"]})"),
        AYNI_TASK_COMPLETED);
    EXPECT_EQ(acted, "hello");
    EXPECT_EQ(received_amounts, (std::vector<int64_t>{10, 20, 10}));
    EXPECT_EQ(presenter.executions(), 1U);
}
} // namespace

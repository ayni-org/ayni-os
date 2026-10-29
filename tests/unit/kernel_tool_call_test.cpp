// Typed tool calls through the public ABI, ADR-0054: the GoalType allow list
// cut to the Task's authority, the tool view given at submit, receipt checks
// and a strategy requesting a proposed call like any other. Normal scenarios
// with explicit sim storage timing; no fault injection.
#include "work_node.h"

#include <ayni/calls.h>
#include <ayni/values.hpp>

namespace {
using namespace ayni::test;

constexpr uint32_t minutes_field = 1;
constexpr uint32_t mode_field = 2;
constexpr uint32_t target_field = 3;

// What a Provider saw of one offered tool, copied before submit returned.
struct ToolSeen {
    std::string binding;
    std::string capability;
    uint32_t text_limit = 0;
    std::vector<uint32_t> ids;
    std::vector<uint32_t> usages;
    std::vector<std::pair<int64_t, int64_t>> ranges;
    std::vector<std::vector<std::string>> tokens;
};

class ToolCalls : public ::testing::Test {
  protected:
    struct Pending {
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        std::string endpoint;
        std::string text;
        std::vector<ToolSeen> tools;
        std::vector<uint32_t> fields;   // Field ids of the delivered arguments.
        std::vector<int64_t> integers;  // I64 values in field order.
        std::vector<std::string> words; // Token values in field order.
        const void *tool_context;
        ayni_status_t (*tool)(const void *, uint32_t, ayni_tool_v1 *);
        bool ended = false;
    };

    void SetUp() override {
        configure();
        work.run();
        for (const char *capability : {"planner", "timer.set"}) {
            const ayni_grant_create_v1 body{sizeof(body),
                                            1,
                                            text(capability),
                                            work.instance(agent_a),
                                            0,
                                            AYNI_GRANT_PERMANENT,
                                            0,
                                            0,
                                            {sizeof(ayni_scope_v1), 0, nullptr}};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &body);
            intent.command_id = ++command;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        }
        pump(20);
    }
    void configure() {
        auto &config = work.node().config();
        config.call_capacity = 8;
        config.task_capacity = 8; // Held Tasks keep their slots.
        config.call_input_max_bytes = 256;
        config.call_result_max_bytes = 512;
        config.call_capability_capacity = 4;
        config.capability_capacity = 4;
        config.provider_capacity = 2;
        config.binding_capacity = 4;
        config.goal_type_capacity = 2;
        config.resource_capacity = 1;
        config.observation_source_capacity = 1;
        config.resource_request_capacity = 4;
        config.storage_buffer_bytes = 0;
        work.node().on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            return decide(snapshot, output);
        };
    }
    void TearDown() override {
        if (work.node().lifecycle() == AYNI_LIFECYCLE_STOPPED) {
            return;
        }
        for (std::size_t at = 0; at < pending.size(); ++at) {
            if (!pending[at].ended) {
                post_text(at, AYNI_CALL_CANCELLED, AYNI_CALL_EFFECT_NONE, {});
            }
        }
        ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
        work.node().drain();
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    }

    static ayni_authority_value_set_v1 integers(int64_t min, int64_t max) {
        ayni_authority_value_set_v1 values{};
        values.struct_size = sizeof(values);
        values.kind = AYNI_AUTHORITY_I64;
        values.unit = text("minute");
        values.values.integer = {min, max};
        return values;
    }
    static ayni_authority_value_set_v1 tokens(ayni_authority_value_kind_t kind,
                                              const ayni_string_v1 *members, uint32_t count) {
        ayni_authority_value_set_v1 values{};
        values.struct_size = sizeof(values);
        values.kind = kind;
        values.values.tokens = {members, count, 0};
        return values;
    }

    void register_profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        const auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);

        // The timer may go to two endpoints; its Binding reaches only one.
        static const ayni_string_v1 modes[] = {text("loud"), text("quiet")};
        static const ayni_string_v1 targets[] = {text("local.timer"), text("other.timer")};
        const ayni_authority_field_desc_v1 timer_fields[] = {
            {sizeof(ayni_authority_field_desc_v1), minutes_field, AYNI_AUTHORITY_PARAMETER, 0,
             integers(1, 120)},
            {sizeof(ayni_authority_field_desc_v1), mode_field, AYNI_AUTHORITY_PARAMETER, 0,
             tokens(AYNI_AUTHORITY_ENUM, modes, 2)},
            {sizeof(ayni_authority_field_desc_v1), target_field, AYNI_AUTHORITY_EXECUTION_TARGET, 0,
             tokens(AYNI_AUTHORITY_IDENTIFIER, targets, 2)},
        };
        const ayni_capability_desc_v1 authorities[] = {
            {sizeof(ayni_capability_desc_v1), 1, text("planner"), 0, 0, nullptr},
            {sizeof(ayni_capability_desc_v1), 1, text("timer.set"), 3, 0, timer_fields},
            {sizeof(ayni_capability_desc_v1), 1, text("lamp.off"), 0, 0, nullptr},
            {sizeof(ayni_capability_desc_v1), 1, text("lamp.watch"), 0, 0, nullptr},
        };
        for (const auto &authority : authorities) {
            ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        }
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        const ayni_call_resource_v1 units{resource.resource_id, 1, 0};
        // ADR-0063: the allow-list rejection uses a valid subscription contract,
        // so it still proves model tools exclude subscriptions, not malformed offers.
        const ayni_observation_source_v1 source{sizeof(source),
                                                1,
                                                text("lamp.sensor"),
                                                AYNI_OBSERVATION_BOOL,
                                                AYNI_OBSERVATION_MEASURED,
                                                text("bool"),
                                                100};
        ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
        const ayni_subscription_contract_v1 subscription{sizeof(subscription), 1, source.source_id,
                                                         AYNI_SUBSCRIPTION_LEASE_MS_DEFAULT, 0};
        auto contract = [&](ayni_call_class_t category, uint32_t result_format,
                            uint32_t input_limit, uint32_t result_limit,
                            ayni_call_form_t form = AYNI_CALL_UNARY) {
            const bool model = category == AYNI_CALL_MODEL;
            return ayni_call_contract_v1{sizeof(ayni_call_contract_v1),
                                         form,
                                         category,
                                         0,
                                         AYNI_CALL_IDEMPOTENT,
                                         AYNI_CALL_QUERY_NONE,
                                         form == AYNI_CALL_SUBSCRIPTION ? AYNI_CALL_CANCEL_CONFIRMED
                                                                        : AYNI_CALL_CANCEL_REQUEST,
                                         0,
                                         30000,
                                         AYNI_CALL_INPUT_VALUE,
                                         result_format,
                                         input_limit,
                                         result_limit,
                                         model ? 1U : 0U,
                                         model ? &units : nullptr,
                                         form == AYNI_CALL_SUBSCRIPTION ? &subscription : nullptr};
        };
        // The planner's own result bound is below the 512-byte result lane.
        ayni_call_capability_v1 planner{
            sizeof(ayni_call_capability_v1), 1, text("planner"),
            contract(AYNI_CALL_MODEL, AYNI_CALL_RESULT_TOOL_CALLS, 88, typed_tool ? 512U : 192U),
            nullptr};
        ayni_call_capability_v1 offers[] = {
            {sizeof(ayni_call_capability_v1), 1, text("timer.set"),
             contract(AYNI_CALL_TOOL, AYNI_CALL_RESULT_TEXT, 16, 32), nullptr},
            {sizeof(ayni_call_capability_v1), 1, text("lamp.off"),
             contract(AYNI_CALL_TOOL, AYNI_CALL_RESULT_TEXT, 16, 32), nullptr},
            {sizeof(ayni_call_capability_v1), 1, text("lamp.watch"),
             contract(AYNI_CALL_TOOL, AYNI_CALL_RESULT_TEXT, 16, 32, AYNI_CALL_SUBSCRIPTION),
             nullptr},
        };
        std::array<ayni_schema_node_v1, 5> business_nodes{};
        business_nodes[0].kind = AYNI_VALUE_OBJECT;
        business_nodes[0].subtree_end = 5;
        const std::array<ayni_string_v1, 4> names{text("minutes"), text("mode"), text("target"),
                                                  text("text")};
        for (uint32_t at = 1; at < business_nodes.size(); ++at) {
            business_nodes[at].field_id = at;
            business_nodes[at].subtree_end = at + 1U;
            business_nodes[at].flags = AYNI_SCHEMA_REQUIRED;
            business_nodes[at].name = names[at - 1U];
        }
        business_nodes[1].kind = AYNI_VALUE_I64;
        business_nodes[1].unit = text("minute");
        business_nodes[1].min_i64 = 1;
        business_nodes[1].max_i64 = 120;
        business_nodes[2].kind = AYNI_VALUE_ENUM;
        business_nodes[2].choice_count = 2;
        business_nodes[2].choices = modes;
        business_nodes[3].kind = AYNI_VALUE_IDENTIFIER;
        business_nodes[3].choice_count = 2;
        business_nodes[3].choices = targets;
        business_nodes[4].kind = AYNI_VALUE_TEXT;
        business_nodes[4].max_count = 16;
        ayni_value_node_v1 default_node{};
        default_node.kind = AYNI_VALUE_TEXT;
        default_node.subtree_end = 1;
        default_node.value.text = text("tea");
        const ayni_value_v1 default_value{&default_node, 1};
        if (default_body) {
            business_nodes[4].flags = 0;
            business_nodes[4].default_value = &default_value;
        }
        ayni_schema_node_v1 result_node{};
        result_node.kind = AYNI_VALUE_TEXT;
        result_node.subtree_end = 1;
        result_node.max_count = 32;
        std::array<ayni_authority_projection_v1, 3> paths{};
        for (uint32_t at = 0; at < paths.size(); ++at) {
            paths[at].field_id = at + 1U;
            paths[at].path_count = 1;
            paths[at].path[0] = at + 1U;
        }
        paths[0].unit = text("minute");
        const ayni_business_contract_v1 business{
            {business_nodes.data(), 5}, {&result_node, 1}, 3, 0, paths.data()};
        offers[0].contract.input_text_limit = typed_input_limit;
        offers[0].business = &business;
        std::array<ayni_schema_node_v1, 2> planner_schema{};
        planner_schema[0].kind = planner_schema[1].kind = AYNI_VALUE_TEXT;
        planner_schema[0].subtree_end = planner_schema[1].subtree_end = 1;
        planner_schema[0].max_count = 64;
        planner_schema[1].max_count = planner.contract.result_text_limit;
        const ayni_business_contract_v1 planner_business{
            {planner_schema.data(), 1}, {&planner_schema[1], 1}, 0, 0, nullptr};
        planner.business = &planner_business;
        std::array<ayni_schema_node_v1, 2> lamp_schema{};
        lamp_schema[0].kind = lamp_schema[1].kind = AYNI_VALUE_TEXT;
        lamp_schema[0].subtree_end = lamp_schema[1].subtree_end = 1;
        lamp_schema[0].max_count = 16;
        lamp_schema[1].max_count = 32;
        const ayni_business_contract_v1 lamp_business{
            {lamp_schema.data(), 1}, {&lamp_schema[1], 1}, 0, 0, nullptr};
        offers[1].contract.input_text_limit = offers[2].contract.input_text_limit = 40;
        offers[1].business = offers[2].business = &lamp_business;
        if (typed_tool) {
            offers[0].contract.input_format = AYNI_CALL_INPUT_VALUE;
            offers[0].contract.result_format = AYNI_CALL_RESULT_VALUE;
            offers[0].contract.input_text_limit = typed_input_limit;
            offers[0].contract.result_text_limit = 64;
            offers[0].business = &business;
        }
        ASSERT_EQ(ayni_register_call_capability(kernel, &planner), AYNI_OK);
        for (const auto &offer : offers) {
            ASSERT_EQ(ayni_register_call_capability(kernel, &offer), AYNI_OK);
        }
        ayni_provider_v1 provider{};
        provider.struct_size = sizeof(provider);
        provider.version = 1;
        provider.provider_id = text("planner.provider");
        provider.endpoint_id = text("planner");
        provider.ctx = this;
        provider.capability_count = 1;
        provider.capabilities = &planner;
        provider.submit = submit;
        provider.cancel = cancel;
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        provider.provider_id = text("tools.provider");
        provider.endpoint_id = text("local.timer");
        provider.capability_count = 3;
        provider.capabilities = offers;
        provider.renew = [](void *, const ayni_subscription_renew_v1 *) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        };
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        const ayni_binding_v1 bindings[] = {
            {sizeof(ayni_binding_v1),
             1,
             text("planner"),
             text("planner"),
             1,
             1,
             text("planner.provider"),
             text("planner"),
             {}},
            {sizeof(ayni_binding_v1),
             1,
             text("timer"),
             text("timer.set"),
             1,
             1,
             text("tools.provider"),
             text("local.timer"),
             {}},
            {sizeof(ayni_binding_v1),
             1,
             text("lamp"),
             text("lamp.off"),
             1,
             1,
             text("tools.provider"),
             text("local.timer"),
             {}},
            {sizeof(ayni_binding_v1),
             1,
             text("watch"),
             text("lamp.watch"),
             1,
             1,
             text("tools.provider"),
             text("local.timer"),
             {}},
        };
        for (const auto &binding : bindings) {
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        }

        auto goal_type = goal_type_desc("assist", 1);
        goal_type.tool_count = static_cast<uint32_t>(listed.size());
        goal_type.tools = listed.data();
        goal_type.export_requirements = [](void *, ayni_bytes_v1,
                                           const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 planner_need{sizeof(ayni_goal_requirement_v1),
                                                        1,
                                                        text("planner"),
                                                        {sizeof(ayni_scope_v1), 0, nullptr}};
            static const ayni_string_v1 quiet = text("quiet");
            const ayni_scope_field_v1 ceiling[] = {
                {sizeof(ayni_scope_field_v1), minutes_field, integers(1, 30)},
                {sizeof(ayni_scope_field_v1), mode_field, tokens(AYNI_AUTHORITY_ENUM, &quiet, 1)},
            };
            const ayni_goal_requirement_v1 timer_need{sizeof(ayni_goal_requirement_v1),
                                                      1,
                                                      text("timer.set"),
                                                      {sizeof(ayni_scope_v1), 2, ceiling}};
            const auto status = sink->emit(sink->ctx, &planner_need);
            return status != AYNI_OK ? status : sink->emit(sink->ctx, &timer_need);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &goal_type), AYNI_OK);
    }

    static ayni_submit_result_t submit(void *ctx, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<ToolCalls *>(ctx);
        Pending call{request->attempt,
                     completion,
                     string_of(request->endpoint_id),
                     string_of(request->text),
                     {},
                     {},
                     {},
                     {},
                     request->tool_context,
                     request->tool};
        if (request->value.node_count != 0U) {
            for (uint32_t at = 1; at < request->value.node_count; ++at) {
                const auto &node = request->value.nodes[at];
                if (node.field_id == 4U && node.kind == AYNI_VALUE_TEXT) {
                    call.text = string_of(node.value.text);
                }
            }
        }
        for (uint32_t at = 0; at < request->arguments.field_count; ++at) {
            const auto &field = request->arguments.fields[at];
            call.fields.push_back(field.field_id);
            if (field.allowed.kind == AYNI_AUTHORITY_I64) {
                call.integers.push_back(field.allowed.values.integer.min_inclusive);
            } else {
                call.words.push_back(string_of(field.allowed.values.tokens.members[0]));
            }
        }
        for (uint32_t index = 0; index < request->tool_count; ++index) {
            ayni_tool_v1 view{};
            // C-VER-004: submit-time tool views share the public ABI size
            // rule; malformed headers cannot partially publish offered metadata.
            view.struct_size = 0;
            view.text_limit = 99;
            EXPECT_EQ(request->tool(request->tool_context, index, &view), AYNI_INVALID_ARGUMENT);
            EXPECT_EQ(view.text_limit, 99U);
            view.struct_size = sizeof(view) + 1;
            EXPECT_EQ(request->tool(request->tool_context, index, &view), AYNI_UNSUPPORTED_VERSION);
            EXPECT_EQ(view.text_limit, 99U);
            EXPECT_EQ(request->tool(nullptr, index, &view), AYNI_INVALID_ARGUMENT);
            EXPECT_EQ(request->tool(request->tool_context, index, nullptr), AYNI_INVALID_ARGUMENT);
            view.struct_size = sizeof(view);
            EXPECT_EQ(request->tool(request->tool_context, index, &view), AYNI_OK);
            ToolSeen seen;
            seen.binding = string_of(view.binding_id);
            seen.capability = string_of(view.capability_id);
            seen.text_limit = view.text_limit;
            for (uint32_t at = 0; at < view.field_count; ++at) {
                const auto &field = view.fields[at];
                seen.ids.push_back(field.field_id);
                seen.usages.push_back(field.usage);
                seen.ranges.emplace_back(field.domain.values.integer.min_inclusive,
                                         field.domain.values.integer.max_inclusive);
                std::vector<std::string> words;
                if (field.domain.kind != AYNI_AUTHORITY_I64) {
                    for (uint32_t member = 0; member < field.domain.values.tokens.member_count;
                         ++member) {
                        words.push_back(string_of(field.domain.values.tokens.members[member]));
                    }
                }
                seen.tokens.push_back(words);
            }
            call.tools.push_back(seen);
        }
        if (request->tool_count != 0U) {
            ayni_tool_v1 view{};
            view.struct_size = sizeof(view);
            EXPECT_EQ(request->tool(request->tool_context, request->tool_count, &view),
                      AYNI_INVALID_ARGUMENT);
        }
        self.pending.push_back(call);
        return AYNI_SUBMIT_ACCEPTED;
    }
    static ayni_submit_result_t cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<ToolCalls *>(ctx);
        for (std::size_t at = 0; at < self.pending.size(); ++at) {
            if (self.pending[at].attempt.request_id == attempt.request_id &&
                !self.pending[at].ended) {
                self.post_text(at, AYNI_CALL_CANCELLED, AYNI_CALL_EFFECT_NONE, {});
            }
        }
        return AYNI_SUBMIT_ACCEPTED;
    }

    // Plans with the model, then requests the first proposed tool call as is.
    ayni_status_t decide(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
        ayni_task_call_result_v1 act{};
        act.struct_size = sizeof(act);
        if (snapshot.call_result(snapshot.call_context, text("act"), &act) == AYNI_OK) {
            acted = string_of(act.text);
            return report(act.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                             : AYNI_POLICY_REPORT_FAILURE,
                          snapshot, output);
        }
        ayni_task_call_result_v1 plan{};
        plan.struct_size = sizeof(plan);
        const auto read = snapshot.call_result(snapshot.call_context, text("plan"), &plan);
        if (read == AYNI_NOT_FOUND) {
            ayni::ValueBuilder<1> input;
            input.text(0, text("set a tea timer"));
            ayni_call_request_v1 request{sizeof(request),
                                         AYNI_POLICY_REQUEST_MODEL,
                                         1,
                                         0,
                                         text("plan"),
                                         text("planner"),
                                         {},
                                         {},
                                         {}};
            const auto built = input.finish(request.value);
            if (built != AYNI_OK) {
                return built;
            }
            return output.request_call(output.call_context, &request);
        }
        if (read != AYNI_OK) {
            return read;
        }
        planned = plan;
        if (plan.outcome != AYNI_CALL_COMPLETED || plan.tool_call_count == 0U) {
            return report(plan.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                              : AYNI_POLICY_REPORT_FAILURE,
                          snapshot, output);
        }
        ayni_task_tool_call_v1 proposed{};
        proposed.struct_size = sizeof(proposed);
        EXPECT_EQ(snapshot.call_tool(snapshot.call_context, text("plan"), plan.tool_call_count,
                                     &proposed),
                  AYNI_NOT_FOUND);
        if (typed_tool) {
            // ADR-0084: a successful public read overwrites stale tagged payload fields.
            proposed.call.input_reserved = 99;
            proposed.call.arguments.field_count = 1;
            proposed.call.text = text("old");
        }
        const auto status = snapshot.call_tool(snapshot.call_context, text("plan"), 0, &proposed);
        if (status != AYNI_OK) {
            return status;
        }
        if (typed_tool) {
            EXPECT_EQ(proposed.call.input_reserved, 0U);
            EXPECT_EQ(proposed.call.arguments.field_count, 0U);
            EXPECT_EQ(proposed.call.text.size, 0U);
            uint32_t bytes = 0;
            EXPECT_EQ(ayni_tool_calls_measure(&proposed.call, 1, &bytes), AYNI_OK);
        }
        proposal_binding = string_of(proposed.call.binding_id);
        proposal_fields = proposed.call.input.value.node_count - 1U;
        proposal_protocol = string_of(proposed.call.protocol_id);
        if (hold_after_plan) {
            return AYNI_UNSUPPORTED; // Holds the Task with its call retained.
        }
        ayni_call_request_v1 request{sizeof(request),
                                     AYNI_POLICY_REQUEST_CALL,
                                     proposed.call.binding_version,
                                     0,
                                     text("act"),
                                     proposed.call.binding_id,
                                     {},
                                     {},
                                     proposed.call.input.value};
        if (proposed.call.input_kind == AYNI_TOOL_INPUT_VALUE) {
            request.value = proposed.call.input.value;
        }
        return output.request_call(output.call_context, &request);
    }

    uint64_t create() {
        const auto created = work.create(
            WorkNode::create_body(work.instance(agent_a), 0, "assist", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(created.status, AYNI_OK);
        return created.result.object.id;
    }
    void pump(unsigned rounds) {
        for (unsigned round = 0; round < rounds; ++round) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(10000);
            work.node().step();
            ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        }
    }
    void post_text(std::size_t at, ayni_call_outcome_t outcome, ayni_call_effect_t effect,
                   ayni_string_v1 body) {
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = outcome;
        result.attempt = pending[at].attempt;
        result.status = outcome == AYNI_CALL_ABANDONED ? AYNI_RESULT_UNKNOWN : AYNI_OK;
        result.effect = effect;
        result.result = body;
        ASSERT_EQ(ayni_provider_post(pending[at].completion, &result), AYNI_OK);
        pending[at].ended = true;
    }
    // One proposal with every timer field; the test adjusts it before posting.
    struct Proposal {
        std::array<ayni_value_node_v1, 5> nodes{};
        ayni_tool_call_v1 call{};
        Proposal() {
            nodes[0].kind = AYNI_VALUE_OBJECT;
            nodes[0].subtree_end = static_cast<uint32_t>(nodes.size());
            for (uint32_t at = 1; at < nodes.size(); ++at) {
                nodes[at].field_id = at;
                nodes[at].subtree_end = at + 1;
            }
            nodes[1].kind = AYNI_VALUE_I64;
            nodes[1].value.integer = 10;
            nodes[2].kind = AYNI_VALUE_ENUM;
            nodes[2].value.text = text("quiet");
            nodes[3].kind = AYNI_VALUE_IDENTIFIER;
            nodes[3].value.text = text("local.timer");
            nodes[4].kind = AYNI_VALUE_TEXT;
            nodes[4].value.text = text("tea");
            call.struct_size = sizeof(call);
            call.binding_version = 1;
            call.binding_id = text("timer");
            call.input_kind = AYNI_TOOL_INPUT_VALUE;
            call.protocol_id = text("call_fixture");
            call.input.value = {nodes.data(), static_cast<uint32_t>(nodes.size())};
        }
    };
    ayni_provider_result_v1 completed(std::size_t at, const ayni_tool_call_v1 *calls,
                                      uint32_t count) const {
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = AYNI_CALL_COMPLETED;
        result.attempt = pending[at].attempt;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        result.tool_call_count = count;
        result.tool_calls = calls;
        return result;
    }
    ayni_call_info_v1 call_info(uint64_t call) {
        ayni_call_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_call(work.kernel(), call, &info), AYNI_OK);
        return info;
    }

    bool typed_tool = false;
    bool default_body = false;
    uint32_t typed_input_limit = 256;
    std::string proposal_protocol;
    WorkNode work;
    // The lamp is listed but its Goal never asks for it: it is not offered.
    std::vector<ayni_tool_ref_v1> listed{{text("timer"), 1, 0}, {text("lamp"), 1, 0}};
    std::vector<Pending> pending;
    uint64_t command = 0;
    ayni_task_call_result_v1 planned{};
    std::string proposal_binding;
    uint32_t proposal_fields = 0;
    std::string acted;
    bool hold_after_plan = false;
};

class NamedToolCalls : public ToolCalls {
  protected:
    void SetUp() override {
        typed_tool = true;
        ToolCalls::SetUp();
    }
    void post_named(ayni_string_v1 number, ayni_string_v1 field) {
        std::array<ayni_argument_node_v1, 5> nodes{};
        nodes[0].kind = AYNI_ARGUMENT_OBJECT;
        nodes[0].subtree_end = 5;
        for (uint32_t at = 1; at < nodes.size(); ++at) {
            nodes[at].subtree_end = at + 1U;
            nodes[at].kind = AYNI_ARGUMENT_STRING;
        }
        nodes[1].name = text("target");
        nodes[1].value.text = text("local.timer");
        nodes[2].kind = AYNI_ARGUMENT_NUMBER;
        nodes[2].name = field;
        nodes[2].value.text = number;
        nodes[3].name = text("mode");
        nodes[3].value.text = text("quiet");
        nodes[4].name = text("text");
        nodes[4].value.text = text("tea");
        ayni_tool_call_v1 proposal{};
        proposal.struct_size = sizeof(proposal);
        proposal.binding_version = 1;
        proposal.binding_id = text("timer");
        proposal.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        proposal.protocol_id = text("call_Original123");
        const uint32_t count = default_body ? 4U : static_cast<uint32_t>(nodes.size());
        nodes[0].subtree_end = count;
        proposal.input.arguments = {nodes.data(), count};
        const auto result = completed(0, &proposal, 1);
        ASSERT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_OK);
        pending[0].ended = true;
    }
};

// ADR-0084/0085: the worker's named tree is copied, then the owner resolves the
// original Schema before the strategy submits the canonical native value.
TEST_F(NamedToolCalls, named_receipt_keeps_protocol_id_and_dispatches_the_same_normalized_values) {
    const auto goal = create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    char number[] = "10";
    post_named({number, 2}, text("minutes"));
    number[0] = '9';
    pump(20);
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(proposal_protocol, "call_Original123");
    EXPECT_EQ(pending[1].integers, std::vector<int64_t>{10});
    EXPECT_EQ(pending[1].text, "tea");
    ayni_value_node_v1 node{};
    node.kind = AYNI_VALUE_TEXT;
    node.subtree_end = 1;
    node.value.text = text("set");
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = pending[1].attempt;
    result.outcome = AYNI_CALL_COMPLETED;
    result.effect = AYNI_CALL_EFFECT_COMPLETE;
    result.value = {&node, 1};
    ASSERT_EQ(ayni_provider_post(pending[1].completion, &result), AYNI_OK);
    pending[1].ended = true;
    pump(20);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
}

// ADR-0084: normalized proposal and direct request share the capability byte
// budget; omitted defaults still consume the complete portable input size.
class ToolDefaultLimit : public NamedToolCalls, public ::testing::WithParamInterface<uint32_t> {
  protected:
    void SetUp() override {
        default_body = true;
        typed_input_limit = GetParam();
        NamedToolCalls::SetUp();
    }
};
TEST_P(ToolDefaultLimit, default_expansion_obeys_the_tools_own_input_bound) {
    const auto goal = create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    post_named(text("10"), text("minutes"));
    pump(20);
    constexpr uint32_t complete_input_bytes = 107;
    if (GetParam() == complete_input_bytes) {
        ASSERT_EQ(pending.size(), 2U);
        EXPECT_EQ(pending[1].text, "tea");
        EXPECT_EQ(planned.outcome, AYNI_CALL_COMPLETED);
    } else {
        EXPECT_EQ(pending.size(), 1U);
        const auto task = work.task(work.goal(goal).last_task_id);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task.hold_status, AYNI_RESULT_UNKNOWN);
        EXPECT_EQ(call_info(task.logical_call_id).reason, AYNI_REASON_PROVIDER_RESULT);
    }
}
INSTANTIATE_TEST_SUITE_P(Bytes, ToolDefaultLimit, ::testing::Values(107U, 106U));

// ADR-0084: a valid source shape is not a valid business input. Schema failure
// after successful post consumes the released borrow and retains UNKNOWN.
TEST_F(NamedToolCalls, integer_schema_refuses_exponent_lexeme_without_dispatching_the_tool) {
    const auto goal = create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    post_named(text("1e1"), text("minutes"));
    pump(20);
    EXPECT_EQ(pending.size(), 1U);
    const auto task = work.task(work.goal(goal).last_task_id);
    EXPECT_EQ(task.hold_status, AYNI_RESULT_UNKNOWN);
    const auto info = call_info(task.logical_call_id);
    EXPECT_EQ(info.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(info.reason, AYNI_REASON_PROVIDER_RESULT);
}

TEST_F(ToolCalls, model_call_is_offered_only_listed_tools_within_the_goal_ceiling) {
    create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    ASSERT_EQ(pending[0].endpoint, "planner");
    ASSERT_EQ(pending[0].tools.size(), 1U);
    const auto &tool = pending[0].tools[0];
    EXPECT_EQ(tool.binding, "timer");
    EXPECT_EQ(tool.capability, "timer.set");
    EXPECT_EQ(tool.text_limit, 256U);
    ASSERT_EQ(tool.ids, (std::vector<uint32_t>{minutes_field, mode_field, target_field}));
    EXPECT_EQ(tool.usages[2], AYNI_AUTHORITY_EXECUTION_TARGET);
    EXPECT_EQ(tool.ranges[0], std::make_pair(int64_t{1}, int64_t{30}));
    EXPECT_EQ(tool.tokens[1], std::vector<std::string>{"quiet"});
    EXPECT_EQ(tool.tokens[2], std::vector<std::string>{"local.timer"});
    // The view is a submit-time borrow; afterwards the callback refuses.
    ayni_tool_v1 late{};
    late.struct_size = sizeof(late);
    EXPECT_EQ(pending[0].tool(pending[0].tool_context, 0, &late), AYNI_INVALID_STATE);
}

TEST_F(ToolCalls, strategy_requests_a_proposed_call_through_the_ordinary_checks) {
    const auto goal = create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    Proposal proposal;
    const auto result = completed(0, &proposal.call, 1);
    ASSERT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_OK);
    pending[0].ended = true;
    proposal.call.binding_id = text("lamp"); // Copied by the post already.
    pump(20);
    EXPECT_EQ(planned.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(planned.tool_call_count, 1U);
    EXPECT_EQ(planned.text.size, 0U);
    EXPECT_EQ(proposal_binding, "timer");
    EXPECT_EQ(proposal_fields, 4U);
    const auto plan = call_info(planned.logical_call_id);
    EXPECT_EQ(plan.tool_call_count, 1U);
    EXPECT_EQ(plan.result_bytes, 0U);
    uint32_t copied = 9;
    EXPECT_EQ(ayni_copy_call_result(work.kernel(), planned.logical_call_id, nullptr, 0, &copied),
              AYNI_OK);
    EXPECT_EQ(copied, 0U);

    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].endpoint, "local.timer");
    EXPECT_EQ(pending[1].text, "tea");
    EXPECT_TRUE(pending[1].tools.empty());
    EXPECT_EQ(pending[1].fields, (std::vector<uint32_t>{minutes_field, mode_field, target_field}));
    EXPECT_EQ(pending[1].integers, std::vector<int64_t>{10});
    EXPECT_EQ(pending[1].words, (std::vector<std::string>{"quiet", "local.timer"}));
    // A text-format call takes no list at all.
    Proposal again;
    const auto listed_reply = completed(1, &again.call, 1);
    EXPECT_EQ(ayni_provider_post(pending[1].completion, &listed_reply), AYNI_INVALID_ARGUMENT);
    post_text(1, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("set"));
    pump(20);
    EXPECT_EQ(acted, "set");
    EXPECT_EQ(work.task(work.goal(goal).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
}

TEST_F(ToolCalls, malformed_lists_are_refused_on_post_and_keep_the_borrow) {
    create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    Proposal proposal;
    auto result = completed(0, &proposal.call, 1);
    result.result = text("and text");
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_INVALID_ARGUMENT);
    result = completed(0, &proposal.call, 1);
    result.outcome = AYNI_CALL_FAILED;
    result.status = AYNI_INVALID_ARGUMENT;
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_INVALID_ARGUMENT);
    result = completed(0, &proposal.call, 1);
    result.tool_reserved = 1;
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_INVALID_ARGUMENT);
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX + 1U> many{};
    many.fill(proposal.call);
    result = completed(0, many.data(), static_cast<uint32_t>(many.size()));
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_INVALID_ARGUMENT);
    result = completed(0, nullptr, 1);
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_INVALID_ARGUMENT);
    // Two calls fit the lane but not the planner's own result bound.
    std::array<ayni_tool_call_v1, 2> two{{proposal.call, proposal.call}};
    two[1].protocol_id = text("call_two");
    result = completed(0, two.data(), 2);
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_CAPACITY_EXCEEDED);
    proposal.nodes[1].kind = AYNI_VALUE_BOOL;
    proposal.nodes[1].value.boolean = 2;
    result = completed(0, &proposal.call, 1);
    EXPECT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_INVALID_ARGUMENT);
    pump(5);
    EXPECT_EQ(planned.outcome, 0U);
    proposal.nodes[1].kind = AYNI_VALUE_I64;
    proposal.nodes[1].value.integer = 10;
    ASSERT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_OK);
    pending[0].ended = true;
    pump(20);
    EXPECT_EQ(planned.tool_call_count, 1U);
}

// The Provider was told what it may propose; anything else is invalid evidence,
// not a model failure the strategy could take for a decision: the call ends
// UNKNOWN and the Task is held like any other unknown call.
TEST_F(ToolCalls, unoffered_tools_and_values_outside_the_offer_end_unknown) {
    const std::vector<std::function<void(Proposal &)>> breaks = {
        [](Proposal &p) { p.call.binding_id = text("lamp"); },
        [](Proposal &p) { p.nodes[1].value.integer = 45; },
        [](Proposal &p) { p.nodes[2].value.text = text("loud"); },
        [](Proposal &p) { p.nodes[3].value.text = text("other.timer"); },
        [](Proposal &p) {
            p.call.input.value.node_count = 4;
            p.nodes[0].subtree_end = 4;
        },
        [](Proposal &p) { p.nodes[4].value.text = text("seventeen bytes!!"); },
        [](Proposal &p) { p.call.binding_version = 2; },
    };
    for (std::size_t at = 0; at < breaks.size(); ++at) {
        SCOPED_TRACE(at);
        const auto goal = create();
        pump(20);
        ASSERT_EQ(pending.size(), at + 1U);
        Proposal proposal;
        breaks[at](proposal);
        const auto result = completed(at, &proposal.call, 1);
        ASSERT_EQ(ayni_provider_post(pending[at].completion, &result), AYNI_OK);
        pending[at].ended = true;
        pump(20);
        EXPECT_EQ(planned.outcome, 0U);
        const auto task = work.task(work.goal(goal).last_task_id);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task.hold_status, AYNI_RESULT_UNKNOWN);
        const auto plan = call_info(task.logical_call_id);
        EXPECT_EQ(plan.outcome, AYNI_CALL_ABANDONED);
        EXPECT_EQ(plan.status, AYNI_RESULT_UNKNOWN);
        EXPECT_EQ(plan.reason, AYNI_REASON_PROVIDER_RESULT);
        EXPECT_EQ(plan.tool_call_count, 0U);
    }
}

// The list is part of the durable call row (v4) and is decoded again on restart.
TEST_F(ToolCalls, proposed_calls_survive_restart_with_the_call_row) {
    hold_after_plan = true;
    create();
    pump(20);
    ASSERT_EQ(pending.size(), 1U);
    Proposal proposal;
    const auto result = completed(0, &proposal.call, 1);
    ASSERT_EQ(ayni_provider_post(pending[0].completion, &result), AYNI_OK);
    pending[0].ended = true;
    pump(20);
    ASSERT_EQ(proposal_binding, "timer");
    const auto call = planned.logical_call_id;
    ASSERT_EQ(call_info(call).tool_call_count, 1U);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    const auto restored = call_info(call);
    EXPECT_EQ(restored.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(restored.tool_call_count, 1U);
    EXPECT_EQ(restored.result_bytes, 0U);
}

// The allow list is checked in two steps: its shape at registration, and at
// ayni_start that each entry is an ordinary unary or action TOOL Binding.
class ToolAllowList : public ToolCalls {
  protected:
    void SetUp() override { configure(); }
    void TearDown() override {}
};

TEST_F(ToolAllowList, registration_checks_each_reference) {
    work.node().create();
    ayni_tool_ref_v1 refs[AYNI_GOAL_TYPE_TOOL_COUNT_MAX + 1U] = {};
    const char *ids[] = {"t.a", "t.b", "t.c", "t.d", "t.e", "t.f", "t.g", "t.h", "t.i"};
    for (uint32_t at = 0; at <= AYNI_GOAL_TYPE_TOOL_COUNT_MAX; ++at) {
        refs[at] = {text(ids[at]), 1, 0};
    }
    auto type = goal_type_desc("other", 1);
    const auto refused = [&](auto change) {
        auto copy = type;
        ayni_tool_ref_v1 edited[2] = {refs[0], refs[1]};
        copy.tool_count = 2;
        copy.tools = edited;
        change(copy, edited);
        return ayni_register_goal_type(work.kernel(), &copy) == AYNI_INVALID_ARGUMENT;
    };
    EXPECT_TRUE(refused([&](auto &copy, auto *) {
        copy.tool_count = 9;
        copy.tools = refs;
    }));
    EXPECT_TRUE(refused([](auto &copy, auto *) { copy.tool_reserved = 1; }));
    EXPECT_TRUE(refused([](auto &copy, auto *) { copy.tools = nullptr; }));
    EXPECT_TRUE(refused([](auto &, auto *edited) { edited[1].binding_version = 0; }));
    EXPECT_TRUE(refused([](auto &, auto *edited) { edited[1].reserved = 1; }));
    EXPECT_TRUE(refused([](auto &, auto *edited) { edited[1].binding_id = text("Not.Id"); }));
    EXPECT_TRUE(refused([](auto &, auto *edited) { edited[1].binding_id = text("t.a"); }));
    // Distinct ids at different versions are the duplicate case too.
    EXPECT_TRUE(refused([](auto &, auto *edited) {
        edited[1] = edited[0];
        edited[1].binding_version = 2;
    }));
    type.tool_count = AYNI_GOAL_TYPE_TOOL_COUNT_MAX;
    type.tools = refs;
    type.tool_reserved = 0;
    EXPECT_EQ(ayni_register_goal_type(work.kernel(), &type), AYNI_OK);
    // An empty list needs no array: only the full registry refuses it.
    auto empty = goal_type_desc("assist", 2);
    empty.tools = nullptr;
    EXPECT_EQ(ayni_register_goal_type(work.kernel(), &empty), AYNI_CAPACITY_EXCEEDED);
}

struct StartCase {
    const char *binding;
    uint32_t version;
    ayni_status_t status;
};

class ToolStart : public ToolAllowList, public ::testing::WithParamInterface<StartCase> {};

TEST_P(ToolStart, start_needs_each_tool_to_be_an_ordinary_tool_binding) {
    listed = {{text("timer"), 1, 0}, {text(GetParam().binding), GetParam().version, 0}};
    work.node().create();
    EXPECT_EQ(ayni_start(work.kernel()), GetParam().status);
    EXPECT_EQ(work.node().lifecycle(), GetParam().status == AYNI_OK ? AYNI_LIFECYCLE_RECOVERING
                                                                    : AYNI_LIFECYCLE_CONFIGURING);
}

INSTANTIATE_TEST_SUITE_P(Cases, ToolStart,
                         ::testing::Values(StartCase{"lamp", 1, AYNI_OK},
                                           StartCase{"lamp", 2, AYNI_NOT_FOUND},
                                           StartCase{"absent", 1, AYNI_NOT_FOUND},
                                           StartCase{"planner", 1, AYNI_INVALID_ARGUMENT},
                                           StartCase{"watch", 1, AYNI_INVALID_ARGUMENT}),
                         [](const ::testing::TestParamInfo<StartCase> &start_case) {
                             return std::string(start_case.param.binding) + "_v" +
                                    std::to_string(start_case.param.version);
                         });
} // namespace

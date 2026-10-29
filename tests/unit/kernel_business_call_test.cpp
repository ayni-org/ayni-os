// Public normal typed-call scenarios, ADR-0084: one normalized input, exact
// decimal operands, one complete saved authority and copied structured results.
#include "kernel_test_probe.h"
#include "work/call/call_records.h"
#include "work_node.h"

#include <ayni/calls.h>

namespace {
using namespace ayni::test;

class BusinessCall : public ::testing::Test {
  protected:
    void SetUp() override {
        auto &config = work.node().config();
        config.call_capacity = 4;
        config.task_capacity = 4;
        config.call_input_max_bytes = 512;
        config.call_result_max_bytes = 512;
        config.call_capability_capacity = 1;
        config.provider_capacity = 1;
        config.binding_capacity = 1;
        config.goal_type_capacity = 1;
        config.resource_request_capacity = 4;
        config.storage_buffer_bytes = 0;
        work.node().on_create([this](auto *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            const auto read = snapshot.call_result(snapshot.call_context, text("batch"), &result);
            if (read == AYNI_OK) {
                if (result.encoded_value.size != 0U) {
                    std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
                    uint32_t count = 0;
                    EXPECT_EQ(ayni_value_read(result.encoded_value, nodes.data(),
                                              static_cast<uint32_t>(nodes.size()), &count),
                              AYNI_OK);
                    if (count == 3U) {
                        result_label = string_of(nodes[2].value.text);
                    }
                }
                return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                    : AYNI_POLICY_REPORT_FAILURE,
                              snapshot, output);
            }
            if (read != AYNI_NOT_FOUND) {
                return read;
            }
            std::array<ayni_value_node_v1, 4> nodes{};
            nodes[0].kind = AYNI_VALUE_OBJECT;
            nodes[0].subtree_end = 4;
            nodes[1].kind = AYNI_VALUE_LIST;
            nodes[1].field_id = 7;
            nodes[1].subtree_end = 4;
            for (uint32_t at = 0; at < values.size(); ++at) {
                nodes[at + 2U].kind = AYNI_VALUE_DECIMAL;
                nodes[at + 2U].subtree_end = at + 3U;
                nodes[at + 2U].value.decimal = {values[at], 2, 0};
            }
            ayni_call_request_v1 request{};
            request.struct_size = sizeof(request);
            request.kind = AYNI_POLICY_REQUEST_CALL;
            request.binding_version = 1;
            request.binding_id = text("batch.binding");
            request.step_key = text("batch");
            request.value = {nodes.data(), static_cast<uint32_t>(nodes.size())};
            const auto staged = output.request_call(output.call_context, &request);
            nodes[2].value.decimal.coefficient = -1; // staging must copy the canonical input.
            return staged;
        };
        work.run();
        for (const int64_t lower : {0, 1000}) {
            const auto field = scope_field(lower, lower + 1000);
            ayni_grant_create_v1 create{};
            create.struct_size = sizeof(create);
            create.capability_id = text("batch");
            create.capability_version = 1;
            create.subject_instance_id = work.instance(agent_a);
            create.lifetime = AYNI_GRANT_PERMANENT;
            create.scope = {sizeof(ayni_scope_v1), 1, &field};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &create);
            intent.command_id = ++command;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        }
        pump();
    }

    static ayni_scope_field_v1 scope_field(int64_t low, int64_t high) {
        ayni_scope_field_v1 field{};
        field.struct_size = sizeof(field);
        field.field_id = 1;
        field.allowed.struct_size = sizeof(field.allowed);
        field.allowed.kind = AYNI_AUTHORITY_I64;
        field.allowed.unit = text("cent");
        field.allowed.values.integer = {low, high};
        return field;
    }

    void register_profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        const auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        auto type = goal_type_desc("batch", 1);
        type.export_requirements = [](void *, ayni_bytes_v1, const auto *sink) {
            for (const int64_t lower : {0, 1000}) {
                const auto field = scope_field(lower, lower + 1000);
                const ayni_goal_requirement_v1 requirement{
                    sizeof(requirement), 1, text("batch"), {sizeof(ayni_scope_v1), 1, &field}};
                const auto status = sink->emit(sink->ctx, &requirement);
                if (status != AYNI_OK) {
                    return status;
                }
            }
            return AYNI_OK;
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
        const auto allowed = scope_field(0, 2000);
        const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                                 allowed.allowed};
        const ayni_capability_desc_v1 authority{sizeof(authority), 1, text("batch"), 1, 0, &field};
        ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        std::array<ayni_schema_node_v1, 3> input{};
        input[0].kind = AYNI_VALUE_OBJECT;
        input[0].subtree_end = 3;
        input[1].kind = AYNI_VALUE_LIST;
        input[1].field_id = 7;
        input[1].subtree_end = 3;
        input[1].flags = AYNI_SCHEMA_REQUIRED;
        input[1].name = text("prices");
        input[1].min_count = 1;
        input[1].max_count = 2;
        input[2].kind = AYNI_VALUE_DECIMAL;
        input[2].subtree_end = 3;
        input[2].unit = text("yuan");
        input[2].scale = 2;
        input[2].max_i64 = 2000;
        std::array<ayni_schema_node_v1, 3> result{};
        result[0].kind = AYNI_VALUE_OBJECT;
        result[0].subtree_end = 3;
        result[1].kind = AYNI_VALUE_I64;
        result[1].field_id = 1;
        result[1].subtree_end = 2;
        result[1].flags = AYNI_SCHEMA_REQUIRED;
        result[1].name = text("count");
        result[1].max_i64 = 2;
        result[2].kind = AYNI_VALUE_TEXT;
        result[2].field_id = 2;
        result[2].subtree_end = 3;
        result[2].name = text("label");
        result[2].max_count = 16;
        ayni_value_node_v1 default_node{};
        default_node.kind = AYNI_VALUE_TEXT;
        default_node.subtree_end = 1;
        default_node.value.text = text("done");
        const ayni_value_v1 default_value{&default_node, 1};
        result[2].default_value = &default_value;
        ayni_authority_projection_v1 path{};
        path.field_id = 1;
        path.path_count = 2;
        path.path[0] = 7;
        path.path[1] = AYNI_PROJECTION_LIST_ITEM;
        path.unit = text("cent");
        path.scale = 2;
        const ayni_business_contract_v1 business{
            {input.data(), 3}, {result.data(), 3}, 1, 0, &path};
        ayni_call_capability_v1 capability{};
        capability.struct_size = sizeof(capability);
        capability.version = 1;
        capability.capability_id = text("batch");
        capability.contract = {sizeof(capability.contract),
                               AYNI_CALL_UNARY,
                               AYNI_CALL_TOOL,
                               0,
                               AYNI_CALL_IDEMPOTENT,
                               AYNI_CALL_QUERY_NONE,
                               AYNI_CALL_CANCEL_NONE,
                               0,
                               30000,
                               AYNI_CALL_INPUT_VALUE,
                               AYNI_CALL_RESULT_VALUE,
                               512,
                               512,
                               0,
                               nullptr,
                               nullptr};
        capability.business = &business;
        ASSERT_EQ(ayni_register_call_capability(kernel, &capability), AYNI_OK);
        ayni_provider_v1 provider{};
        provider.struct_size = sizeof(provider);
        provider.version = 1;
        provider.provider_id = text("batch.provider");
        provider.endpoint_id = text("batch.endpoint");
        provider.ctx = this;
        provider.capability_count = 1;
        provider.capabilities = &capability;
        provider.submit = [](void *ctx, const auto *request, auto *completion) {
            auto &self = *static_cast<BusinessCall *>(ctx);
            ++self.submissions;
            EXPECT_EQ(request->value.node_count, 4U);
            EXPECT_EQ(request->value.nodes[2].value.decimal.coefficient, self.values[0]);
            EXPECT_EQ(request->value.nodes[3].value.decimal.coefficient, self.values[1]);
            std::array<ayni_value_node_v1, 2> answer{};
            answer[0].kind = AYNI_VALUE_OBJECT;
            answer[0].subtree_end = 2;
            answer[1].kind = AYNI_VALUE_I64;
            answer[1].field_id = 1;
            answer[1].subtree_end = 2;
            answer[1].value.integer = self.malformed ? 3 : 2;
            ayni_provider_result_v1 posted{};
            posted.struct_size = sizeof(posted);
            posted.attempt = request->attempt;
            posted.outcome = AYNI_CALL_COMPLETED;
            posted.effect = AYNI_CALL_EFFECT_COMPLETE;
            posted.value = {answer.data(), static_cast<uint32_t>(answer.size())};
            EXPECT_EQ(ayni_provider_post(completion, &posted), AYNI_OK);
            answer[1].value.integer = -1; // successful post copied all node bytes.
            return AYNI_SUBMIT_ACCEPTED;
        };
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("batch.binding"),
                                      text("batch"),
                                      1,
                                      1,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {}};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        input[2].scale = 3; // registered proofs must own the original scale/defaults.
    }

    void pump() {
        for (uint32_t at = 0; at < 40U; ++at) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(10000);
            work.node().step();
            ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        }
    }
    uint64_t start() {
        const auto body =
            WorkNode::create_body(work.instance(agent_a), 0, "batch", 1, {}, AYNI_TRIGGER_ONCE);
        const auto created = work.create(body);
        EXPECT_EQ(created.status, AYNI_OK);
        pump();
        return created.result.object.id;
    }
    WorkNode work;
    std::array<int64_t, 2> values{500, 700};
    uint64_t command = 100;
    uint32_t submissions = 0;
    bool malformed = false;
    std::string result_label;
};

// ADR-0084: normalization/copying preserves exact coefficients through dispatch
// and fills the original result default on the owner after terminal post.
TEST_F(BusinessCall, copied_business_values_and_result_defaults_complete_the_same_task) {
    const auto goal = start();
    EXPECT_EQ(submissions, 1U) << "task hold "
                               << work.task(work.goal(goal).last_task_id).hold_status << "/"
                               << work.task(work.goal(goal).last_task_id).hold_reason;
    EXPECT_EQ(result_label, "done");
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
}

// ADR-0084 / privacy boundary, white-box body ownership: source invalidation
// may discard a late structured result without changing actual outcome/effect,
// inventing an erasure stamp, or leaving an undecodable persisted record.
TEST_F(BusinessCall, unavailable_structured_body_keeps_a_valid_original_call_fact) {
    start();
    auto &calls = ayni::kernel::KernelTestProbe::work(work.kernel()).calls;
    uint32_t index = 0;
    while (index < calls.capacity() && !calls.occupied(index)) {
        ++index;
    }
    ASSERT_LT(index, calls.capacity());
    const auto call_id = calls.identity(index).call_id;
    calls.discard_unusable_result(index);
    const auto saved = calls.record(index);
    ASSERT_TRUE(ayni::work::CallTable::valid_record(saved));
    EXPECT_EQ(saved.result_kind, ayni::work::CallResultKind::unavailable);
    EXPECT_EQ(saved.state.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(calls.effect(index), ayni::work::ActionEffect::complete);
    EXPECT_FALSE(saved.state.content_erased);
    EXPECT_GT(saved.input.size, 0U);
    std::vector<uint8_t> body(ayni::work::call_record_size(saved));
    ayni::work::write_call_record(saved, body.data());
    ayni::work::CallRecord decoded{};
    ASSERT_TRUE(
        ayni::work::read_call_record(body.data(), static_cast<uint32_t>(body.size()), decoded));
    const ayni::work::CallTable::Limits limits{1, 512, 512, 512, 4096};
    std::vector<uint64_t> arena(ayni::work::CallTable::storage_size(limits) / sizeof(uint64_t) +
                                1U);
    ayni::work::CallTable restored(arena.data(), limits);
    ASSERT_EQ(restored.restore_validated(decoded), AYNI_OK);
    EXPECT_EQ(restored.record(0).result_kind, ayni::work::CallResultKind::unavailable);
    uint32_t size = 99;
    EXPECT_EQ(ayni_copy_call_result(work.kernel(), call_id, nullptr, 0, &size), AYNI_EXPIRED);
    EXPECT_EQ(size, 99U);
}

// ADR-0084 decision 5: two separately valid Grants cannot be unioned per item.
TEST_F(BusinessCall, a_list_split_across_two_saved_grants_is_denied_without_dispatch) {
    values = {500, 1500};
    start();
    EXPECT_EQ(submissions, 0U);
}

// ADR-0084 decision 5: a narrower candidate matching the first item does not
// prevent a later complete requirement from covering the entire request.
TEST_F(BusinessCall, a_later_complete_requirement_can_cover_the_whole_list) {
    values = {1000, 1500};
    const auto goal = start();
    EXPECT_EQ(submissions, 1U) << "task hold "
                               << work.task(work.goal(goal).last_task_id).hold_status << "/"
                               << work.task(work.goal(goal).last_task_id).hold_reason;
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
}

// ADR-0084: accepted structural receipt is consumed as unknown evidence if its
// schema fails; a released producer borrow is never required to post again.
TEST_F(BusinessCall, schema_invalid_structured_terminal_does_not_complete_the_goal) {
    malformed = true;
    const auto goal = start();
    EXPECT_EQ(submissions, 1U) << "task hold "
                               << work.task(work.goal(goal).last_task_id).hold_status << "/"
                               << work.task(work.goal(goal).last_task_id).hold_reason;
    EXPECT_TRUE(result_label.empty());
    EXPECT_NE(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
}
} // namespace

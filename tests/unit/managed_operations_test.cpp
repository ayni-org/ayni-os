// C-OP-001/002/003/004, ADR-0086: public fixed-tool round trips with real bounded software
// execution.
#include "strategies_fixture.h"
#include <ayni/operation_json.hpp>
namespace {
using namespace ayni::test;
class ManagedOperations : public StrategiesFixture {
  protected:
    void SetUp() override {
        software_tools = true;
        StrategiesFixture::SetUp();
    }
    // Closed argument objects are built explicitly; all views survive the synchronous post copy.
    void command(uint32_t model,
                 std::initializer_list<std::pair<const char *, std::string>> fields) {
        std::array<ayni_argument_node_v1, 8> nodes{};
        nodes[0].kind = AYNI_ARGUMENT_OBJECT;
        nodes[0].subtree_end = static_cast<uint32_t>(fields.size()) + 1;
        uint32_t at = 1;
        for (const auto &field : fields) {
            auto &node = nodes[at];
            node.name = text(field.first);
            const bool numeric = std::strcmp(field.first, "binding_version") == 0 ||
                                 std::strcmp(field.first, "timeout_ms") == 0 ||
                                 (std::strcmp(field.first, "arguments") == 0 && field.second != "");
            node.kind = numeric ? AYNI_ARGUMENT_NUMBER : AYNI_ARGUMENT_STRING;
            node.subtree_end = ++at;
            node.value.text = {field.second.data(), static_cast<uint32_t>(field.second.size())};
        }
        const std::string protocol = "managed_" + std::to_string(model);
        ayni_tool_call_v1 call{};
        call.struct_size = sizeof(call);
        call.protocol_id = {protocol.data(), static_cast<uint32_t>(protocol.size())};
        call.function_name = {ayni::model::call_function.data(),
                              static_cast<uint32_t>(ayni::model::call_function.size())};
        call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        call.input.arguments = {nodes.data(), at};
        post(model, {}, AYNI_CALL_COMPLETED, AYNI_OK, &call, 1);
        pump();
    }
    static std::string token(const std::string &body, const char *prefix) {
        const auto at = body.rfind(prefix);
        if (at == std::string::npos) {
            return {};
        }
        const auto end = body.find_first_not_of("0123456789:of", at);
        return body.substr(at, end - at);
    }
    uint64_t management_command = 10000;
    ayni_status_t task_change(ayni_intent_kind_t kind, uint64_t task) {
        const ayni_task_target_v1 target{sizeof(target), 0, task};
        auto intent = work.intent(kind, &target, work.task(task).revision);
        intent.command_id = ++management_command;
        const auto status = work.submit(intent).status;
        pump();
        return status;
    }
    void compute(uint32_t count = 1) {
        for (uint32_t at = 0; at < count; ++at) {
            ASSERT_EQ(software.advance(), AYNI_OK);
            pump();
        }
    }
};
TEST_F(ManagedOperations, action_progress_wait_and_original_typed_result) {
    const auto task = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "512"}});
    ASSERT_EQ(pending.size(), 2U);
    const auto reference = token(pending[1].text, "o1:");
    ASSERT_FALSE(reference.empty());
    EXPECT_EQ(software.pending(), 1U);
    // Accepting a second action must not create another delivery.
    command(1, {{"operation", "invoke"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "2"}});
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_EQ(software.pending(), 1U);
    command(2, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 4U);
    const auto after = token(pending.back().text, "f1:");
    command(3, {{"operation", "wait"},
                {"operation_ref", reference},
                {"after", after},
                {"timeout_ms", "30000"}});
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_WAITING);
    const auto models = pending.size();
    pump();
    EXPECT_EQ(pending.size(), models);
    compute();
    ASSERT_EQ(pending.size(), models + 1U);
    compute(4);
    command(static_cast<uint32_t>(models), {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), models + 2U);
    EXPECT_NE(pending.back().text.find("131328"), std::string::npos);
    EXPECT_EQ(software.pending(), 0U);
    post(static_cast<uint32_t>(pending.size() - 1U), text("Computed."));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(ManagedOperations, finite_wait_times_out_without_cancelling_then_explicit_cancel) {
    const auto task = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "512"}});
    ASSERT_EQ(pending.size(), 2U);
    const auto reference = token(pending[1].text, "o1:");
    command(1, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 3U);
    const auto after = token(pending.back().text, "f1:");
    command(2, {{"operation", "wait"},
                {"operation_ref", reference},
                {"after", after},
                {"timeout_ms", "1000"}});
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_WAITING);
    work.node().sim().advance_us(1000000);
    pump();
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_EQ(software.pending(), 1U);
    command(3, {{"operation", "cancel"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 5U);
    compute();
    EXPECT_EQ(software.pending(), 0U);
    command(4, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 6U);
    post(5, text("Cancelled."));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(ManagedOperations, final_text_closes_original_task_operations_and_reference_is_scoped) {
    const auto first = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "512"}});
    ASSERT_EQ(pending.size(), 2U);
    const auto reference = token(pending[1].text, "o1:");
    post(1, text("Finished this turn."));
    pump();
    compute();
    EXPECT_EQ(work.task(first).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(software.pending(), 0U);
    finish(first);
    const auto second = start();
    ASSERT_EQ(pending.size(), 3U);
    command(2, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_NE(pending.back().text.find("unavailable"), std::string::npos);
    post(3, text("Done."));
    pump();
    EXPECT_EQ(work.task(second).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(ManagedOperations, software_subscription_sample_is_readable_then_close_invalidates_context) {
    const auto task = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.events"},
                {"binding_version", "1"},
                {"arguments", ""}});
    ASSERT_EQ(pending.size(), 2U);
    const auto reference = token(pending[1].text, "o1:");
    compute();
    work.node().sim().advance_us(1000000);
    compute();
    command(1, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_NE(pending.back().text.find("software.events"), std::string::npos);
    command(2, {{"operation", "close"}, {"operation_ref", reference}});
    compute();
    EXPECT_EQ(software.pending(), 0U);
    // No additional model request may reuse the now-closed current sample as evidence.
    EXPECT_NE(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}

// ADR-0086: old observed progress must be returned immediately, never swallowed by wait.
TEST_F(ManagedOperations, progress_before_wait_returns_immediately_and_future_frontier_is_refused) {
    start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "512"}});
    ASSERT_EQ(pending.size(), 2U);
    const auto reference = token(pending.back().text, "o1:");
    command(1, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 3U);
    const auto after = token(pending.back().text, "f1:");
    compute();
    command(2, {{"operation", "wait"},
                {"operation_ref", reference},
                {"after", after},
                {"timeout_ms", "1000"}});
    ASSERT_EQ(pending.size(), 4U);
    ayni_operation_frontier_v1 future{};
    const auto current = token(pending.back().text, "f1:");
    ASSERT_TRUE(ayni::operations::frontier(text(current.c_str()), future));
    ++future.sequence;
    char bytes[ayni::operations::frontier_bytes];
    ayni_string_v1 encoded{};
    ASSERT_TRUE(ayni::operations::frontier(future, bytes, encoded));
    command(3, {{"operation", "wait"},
                {"operation_ref", reference},
                {"after", string_of(encoded)},
                {"timeout_ms", "1000"}});
    ASSERT_EQ(pending.size(), 5U);
    EXPECT_NE(pending.back().text.find("Control refused"), std::string::npos);
    EXPECT_EQ(software.pending(), 1U);
}
TEST_F(ManagedOperations, action_and_subscription_progress_do_not_end_the_foreground_model) {
    const auto task = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "512"}});
    ASSERT_EQ(pending.size(), 2U);
    command(1, {{"operation", "start"},
                {"binding_id", "software.events"},
                {"binding_version", "1"},
                {"arguments", ""}});
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_EQ(software.pending(), 2U);
    const auto foreground = work.task(task).logical_call_id;
    compute(5);
    EXPECT_EQ(software.pending(), 1U);
    EXPECT_FALSE(pending.back().ended);
    EXPECT_EQ(pending.size(), 3U);
    EXPECT_EQ(work.task(task).logical_call_id, foreground);
    post(2, text("Done."));
    pump();
    compute();
    EXPECT_EQ(software.pending(), 0U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(ManagedOperations,
       sample_wait_rebuild_keeps_current_reference_and_next_task_discards_sample_history) {
    const auto task = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.events"},
                {"binding_version", "1"},
                {"arguments", ""}});
    ASSERT_EQ(pending.size(), 2U);
    const auto reference = token(pending.back().text, "o1:");
    compute();
    work.node().sim().advance_us(1000000);
    compute();
    command(1, {{"operation", "read"}, {"operation_ref", reference}});
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_NE(pending.back().text.find("\"usable\":true"), std::string::npos);
    const auto after = token(pending.back().text, "f1:");
    command(2, {{"operation", "wait"},
                {"operation_ref", reference},
                {"after", after},
                {"timeout_ms", "30000"}});
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_WAITING);
    work.node().sim().advance_us(1000000);
    compute();
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_NE(pending.back().text.find(reference), std::string::npos);
    EXPECT_NE(pending.back().text.find("Known operation facts"), std::string::npos);
    post(3, text("Latest sample analyzed."));
    pump();
    compute();
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    finish(task);
    EXPECT_GT(conversation.disclosure.sample_count, 0U);
    start();
    ASSERT_EQ(pending.size(), 5U);
    EXPECT_EQ(pending.back().text.find("\"usable\":true"), std::string::npos);
    EXPECT_EQ(pending.back().text.find("Latest sample analyzed."), std::string::npos);
}
// C-RES-004 / C-OP-001: preserve existing same-Agent FIFO when the action cannot acquire resources.
TEST_F(ManagedOperations, queued_action_preserves_agent_fifo_and_owner_can_cancel_the_task) {
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("software.slot"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    const auto task = start();
    command(0, {{"operation", "start"},
                {"binding_id", "software.sum"},
                {"binding_version", "1"},
                {"arguments", "512"}});
    EXPECT_EQ(pending.size(), 1U); // Next MODEL is queued behind this Agent's earlier action.
    EXPECT_EQ(software.pending(), 0U);
    EXPECT_NE(work.task(task).logical_call_id, 0U);
    ASSERT_EQ(task_change(AYNI_INTENT_TASK_CANCEL, task), AYNI_OK);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(software.pending(), 0U);
}
TEST(OperationTokens, exact_round_trip_rejects_overflow_leading_zeros_and_malformed_null_view) {
    const ayni_operation_ref_v1 original{UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 2};
    char bytes[ayni::operations::reference_bytes];
    ayni_string_v1 encoded{};
    ASSERT_TRUE(ayni::operations::reference(original, bytes, encoded));
    ayni_operation_ref_v1 decoded{};
    ASSERT_TRUE(ayni::operations::reference(encoded, decoded));
    EXPECT_EQ(decoded.operation_id, original.operation_id);
    EXPECT_FALSE(ayni::operations::reference(text("o1:01:2:3"), decoded));
    EXPECT_FALSE(ayni::operations::reference(text("o1:18446744073709551616:2:3"), decoded));
    EXPECT_FALSE(ayni::operations::reference(ayni_string_v1{nullptr, 8}, decoded));
}
// C-OP-004: source identities and the complete I64 domain survive ordinary JSON consumers.
TEST(OperationTokens, copied_sample_preserves_full_width_identity_time_and_integer_value) {
    ayni_operation_info_v1 info{};
    info.reference = {UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 2};
    info.sample.struct_size = sizeof(info.sample);
    info.sample.usable = 1;
    info.sample.kind = AYNI_OBSERVATION_I64;
    info.sample.value = INT64_MIN;
    info.sample.reference.boot_epoch = UINT64_MAX;
    info.sample.reference.generation = UINT64_MAX;
    info.sample.reference.sequence = UINT64_MAX;
    info.sample.reference.subscription_id = UINT64_MAX;
    info.sample.captured_at_us = UINT64_MAX;
    std::array<char, 2048> bytes{};
    ayni::json::Writer json(bytes.data(), bytes.size());
    ayni::operations::write(json, info);
    size_t size = 0;
    ASSERT_TRUE(json.finish(size));
    const std::string body(bytes.data(), size);
    for (const auto key :
         {"boot_epoch", "generation", "sequence", "subscription_id", "captured_at_us"}) {
        EXPECT_NE(body.find(std::string("\"") + key + "\":\"18446744073709551615\""),
                  std::string::npos);
    }
    EXPECT_NE(body.find("\"value\":\"-9223372036854775808\""), std::string::npos);
}
} // namespace

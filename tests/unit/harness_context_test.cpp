#include "strategy_workspace_fixture.h"
// ADR-0085: normal context-budget fallback and chunked result handling.
#include "strategies_fixture.h"
#include <ayni/ayni.hpp>
#include <ayni/call_result_json.hpp>
#include <memory>
namespace {
using namespace ayni::test;
// The first ToolLoop body write is not yet durable; every later one is.
struct DeferOnce {
    const ayni_body_store_v1 *inner = nullptr;
    bool deferred = false;
    std::map<uint64_t, uint32_t> reads;
    ayni_body_store_v1 store{sizeof(ayni_body_store_v1),
                             0,
                             this,
                             write,
                             read,
                             release,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr};
    static ayni_status_t write(void *context, uint64_t scope, ayni_bytes_v1 bytes,
                               ayni_body_ref_v1 *out) {
        auto &self = *static_cast<DeferOnce *>(context);
        if (!self.deferred) {
            self.deferred = true;
            return AYNI_NOT_READY;
        }
        return self.inner->write(self.inner->context, scope, bytes, out);
    }
    static ayni_status_t read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                              uint32_t capacity) {
        auto &self = *static_cast<DeferOnce *>(context);
        ++self.reads[ref->id];
        const auto *inner = self.inner;
        return inner->read(inner->context, ref, out, capacity);
    }
    static void release(void *context, const ayni_body_ref_v1 *ref) {
        const auto *inner = static_cast<DeferOnce *>(context)->inner;
        inner->release(inner->context, ref);
    }
};
class ContextHarness : public StrategiesFixture {
  protected:
    ContextHarness() {
        large_tool_result = true;
        configure_loop = [](policies::ToolLoopConfig &config) {
            config.body_progress = {nullptr, [](void *, uint64_t) { return 1U; }};
        };
        defer.inner = bodies.store();
        body_store = &defer.store;
    }
    void TearDown() override {
        StrategiesFixture::TearDown();
        EXPECT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    }
    void expect_original(const std::string &original) {
        uint32_t count = 0;
        ASSERT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_CALL, nullptr, 0, &count),
                  AYNI_BUFFER_TOO_SMALL);
        std::vector<uint64_t> ids(count);
        ASSERT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_CALL, ids.data(), count, &count),
                  AYNI_OK);
        bool found = false;
        for (const auto id : ids) {
            auto call = ayni::initialized<ayni_call_info_v1>();
            ASSERT_EQ(ayni_get_call(work.kernel(), id, &call), AYNI_OK);
            if (call.operation_id != pending[1].operation) {
                continue;
            }
            found = true;
            EXPECT_EQ(call.outcome, AYNI_CALL_COMPLETED);
            EXPECT_EQ(call.effect, AYNI_CALL_EFFECT_COMPLETE);
            std::string bytes(original.size(), '\0');
            uint32_t size = 0;
            ASSERT_EQ(ayni_copy_call_result(work.kernel(), id, bytes.data(),
                                            static_cast<uint32_t>(bytes.size()), &size),
                      AYNI_OK);
            if (call.body.id) {
                ASSERT_EQ(policies::Conversation::text(*bodies.store(), call.body, bytes), AYNI_OK);
            } else {
                bytes.resize(size);
            }
            EXPECT_EQ(bytes, original);
        }
        EXPECT_TRUE(found);
    }
    DeferOnce defer;
};
TEST_F(ContextHarness, body_write_not_yet_durable_does_not_pause_or_repeat_a_model_call) {
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    post(0, text("done"));
    pump();
    finish(task);
    EXPECT_TRUE(defer.deferred);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_NE(conversation.history.id, 0U);
}
TEST_F(ContextHarness, oversized_executed_result_uses_chunk_summaries_and_keeps_known_facts) {
    const auto task = start("chat", "one operation", 32);
    propose(0, 1);
    const std::string original(4000, '\x01');
    post(1, {original.data(), static_cast<uint32_t>(original.size())});
    pump();
    uint32_t summaries = 0;
    while (pending.back().capability == "summary") {
        ASSERT_LT(summaries++, 24U);
        post(static_cast<uint32_t>(pending.size() - 1), text("tentative result summary"));
        pump();
    }
    EXPECT_GT(summaries, 1U);
    // C-POLICY-013: multiple summary chunks share one original projection, not N full reads.
    ayni_body_ref_v1 source{};
    ASSERT_EQ(bodies.store()->write(
                  bodies.store()->context, conversation.id,
                  {reinterpret_cast<const uint8_t *>(original.data()), uint32_t(original.size())},
                  &source),
              AYNI_OK);
    EXPECT_EQ(defer.reads[source.id], 1U);
    ASSERT_EQ(pending.back().capability, "model");
    EXPECT_NE(pending.back().text.find("unconfirmed_summary"), std::string::npos);
    EXPECT_NE(pending.back().text.find("result_ref"), std::string::npos);
    EXPECT_EQ(std::count_if(pending.begin(), pending.end(),
                            [](const auto &p) { return p.capability == "echo"; }),
              1);
    expect_original(original);
    post(static_cast<uint32_t>(pending.size() - 1), text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
// C-OP-001/004: managed read archives/summarizes the original business result exactly once.
class ManagedResultHarness : public ContextHarness {
  protected:
    void SetUp() override {
        action_tool = true;
        ContextHarness::SetUp();
    }
    void function(uint32_t model,
                  std::initializer_list<std::pair<const char *, std::string>> fields) {
        std::array<ayni_argument_node_v1, 8> nodes{};
        nodes[0].kind = AYNI_ARGUMENT_OBJECT;
        nodes[0].subtree_end = static_cast<uint32_t>(fields.size()) + 1;
        uint32_t at = 1;
        for (const auto &field : fields) {
            auto &node = nodes[at];
            node.name = text(field.first);
            node.kind = std::strcmp(field.first, "binding_version") == 0 ? AYNI_ARGUMENT_NUMBER
                                                                         : AYNI_ARGUMENT_STRING;
            node.subtree_end = ++at;
            node.value.text = {field.second.data(), static_cast<uint32_t>(field.second.size())};
        }
        ayni_tool_call_v1 call{};
        call.struct_size = sizeof(call);
        call.protocol_id = text("managed_result");
        call.function_name = text("ayni_call_capability");
        call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        call.input.arguments = {nodes.data(), at};
        post(model, {}, AYNI_CALL_COMPLETED, AYNI_OK, &call, 1);
        pump();
    }
};
TEST_F(ManagedResultHarness,
       managed_read_preserves_reference_and_original_large_result_through_summary) {
    const auto task = start("chat", "one managed operation", 32);
    function(0, {{"operation", "start"},
                 {"binding_id", "echo"},
                 {"binding_version", "1"},
                 {"arguments", "body"}});
    ASSERT_EQ(pending.size(), 3U);
    ASSERT_EQ(pending[1].capability, "echo");
    ASSERT_EQ(pending[2].capability, "model");
    const auto begin = pending[2].text.find("o1:");
    ASSERT_NE(begin, std::string::npos);
    const auto end = pending[2].text.find_first_not_of("o:0123456789", begin);
    const auto reference = pending[2].text.substr(begin, end - begin);
    const std::string original(4000, '\x01');
    post(1, {original.data(), static_cast<uint32_t>(original.size())});
    pump();
    function(2, {{"operation", "read"}, {"operation_ref", reference}});
    uint32_t summaries = 0;
    while (pending.back().capability == "summary") {
        ASSERT_LT(summaries++, 24U);
        post(static_cast<uint32_t>(pending.size() - 1), text("tentative managed result summary"));
        pump();
    }
    EXPECT_GT(summaries, 1U);
    // C-POLICY-013: multiple summary chunks share one original projection, not N full reads.
    ayni_body_ref_v1 source{};
    ASSERT_EQ(bodies.store()->write(
                  bodies.store()->context, conversation.id,
                  {reinterpret_cast<const uint8_t *>(original.data()), uint32_t(original.size())},
                  &source),
              AYNI_OK);
    EXPECT_EQ(defer.reads[source.id], 1U);
    ASSERT_EQ(pending.back().capability, "model");
    EXPECT_NE(pending.back().text.find(reference), std::string::npos);
    EXPECT_NE(pending.back().text.find("result_ref"), std::string::npos);
    EXPECT_EQ(std::count_if(pending.begin(), pending.end(),
                            [](const auto &call) { return call.capability == "echo"; }),
              1);
    post(static_cast<uint32_t>(pending.size() - 1), text("Done."));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}

TEST_F(ContextHarness, declared_summary_failure_pauses_without_changing_the_executed_result) {
    const auto task = start("chat", "one operation", 32);
    propose(0, 1);
    const std::string original(4000, '\x01');
    post(1, {original.data(), static_cast<uint32_t>(original.size())});
    pump();
    ASSERT_EQ(pending.back().capability, "summary");
    post(static_cast<uint32_t>(pending.size() - 1), {}, AYNI_CALL_FAILED, AYNI_NOT_READY);
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_status, AYNI_NOT_READY);
    expect_original(original);
    EXPECT_EQ(std::count_if(pending.begin(), pending.end(),
                            [](const auto &p) { return p.capability == "echo"; }),
              1);
}
// ADR-0085: browsing metadata never binds a Grant; queued egress rechecks its source.
TEST_F(StrategiesFixture, unused_metadata_revocation_cancels_queued_model_egress) {
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    std::array<ayni_argument_node_v1, 3> arguments{};
    arguments[0].kind = AYNI_ARGUMENT_OBJECT;
    arguments[0].subtree_end = 3;
    arguments[1].kind = AYNI_ARGUMENT_STRING;
    arguments[1].name = text("binding_id");
    arguments[1].subtree_end = 2;
    arguments[1].value.text = text("echo");
    arguments[2].kind = AYNI_ARGUMENT_NUMBER;
    arguments[2].name = text("binding_version");
    arguments[2].subtree_end = 3;
    arguments[2].value.text = text("1");
    ayni_tool_call_v1 inspect{};
    inspect.struct_size = sizeof(inspect);
    inspect.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
    inspect.function_name = text("ayni_inspect_capability");
    inspect.protocol_id = text("inspect-echo");
    inspect.input.arguments = {arguments.data(), static_cast<uint32_t>(arguments.size())};
    post(0, {}, AYNI_CALL_COMPLETED, AYNI_OK, &inspect, 1);
    pump();
    ASSERT_EQ(pending.size(), 1U); // The metadata-bearing next Model Call is resource-queued.
    std::array<uint64_t, 24> calls{};
    uint32_t count = 0;
    ASSERT_EQ(
        ayni_list_objects(work.kernel(), AYNI_OBJECT_CALL, calls.data(), calls.size(), &count),
        AYNI_OK);
    ASSERT_EQ(count, 2U);
    uint64_t queued = 0;
    for (uint32_t at = 0; at < count; ++at) {
        ayni_call_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_call(work.kernel(), calls[at], &info), AYNI_OK);
        if (!info.outcome) {
            queued = calls[at];
            EXPECT_EQ(info.attempt.request_id, 0U);
        }
    }
    ASSERT_NE(queued, 0U);
    ayni_task_tool_authority_v1 bound{};
    bound.struct_size = sizeof(bound);
    EXPECT_EQ(ayni_get_task_tool_authority(work.kernel(), task, 0, &bound), AYNI_NOT_FOUND);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, echo_grant};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump();
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump();
    ayni_call_info_v1 cancelled{};
    cancelled.struct_size = sizeof(cancelled);
    ASSERT_EQ(ayni_get_call(work.kernel(), queued, &cancelled), AYNI_OK);
    EXPECT_EQ(cancelled.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(cancelled.effect, AYNI_CALL_EFFECT_NONE);
    // Core can reserve a bookkeeping attempt before Provider submit. Observed
    // operations, rather than a terminal attempt field, prove no original egress.
    for (const auto &submitted : pending) {
        EXPECT_NE(submitted.operation, cancelled.operation_id);
    }
    EXPECT_EQ(ayni_get_task_tool_authority(work.kernel(), task, 0, &bound), AYNI_NOT_FOUND);
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].text.find("Echo input."), std::string::npos);
    post(1, text("Current directory rebuilt."));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(StrategiesFixture, absent_service_usage_uses_a_labelled_character_estimate) {
    service_usage = {};
    const auto task = start("chat", "你好 abc");
    post(0, text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(conversation.last_usage.kind, AYNI_MODEL_USAGE_NONE);
    EXPECT_GT(conversation.last_request_characters, 0U);
    EXPECT_EQ(conversation.last_input_estimate,
              ayni::model::estimate_tokens(conversation.last_request_characters));
}
TEST(ContextEstimate, counts_unicode_characters_and_rounds_the_selected_ratio_up) {
    EXPECT_EQ(ayni::model::estimate_tokens(4), 3U);
    EXPECT_EQ(ayni::model::estimate_tokens(5), 4U);
    char frame[256];
    ayni::model::Writer messages(frame, sizeof(frame));
    messages.prefix();
    messages.text(ayni::model::Role::user, {"中文😀", 10});
    ayni_string_v1 body{};
    ASSERT_TRUE(messages.finish(body));
    auto projected = ayni::json::Writer::measuring();
    ASSERT_EQ(ayni::model::write_context(projected, body, ayni::model::Mode::text), AYNI_OK);
    size_t bytes = 0;
    ASSERT_TRUE(projected.finish(bytes));
    EXPECT_EQ(bytes - projected.characters(), 7U); // Two three-byte and one four-byte code points.
}
} // namespace

namespace {
class SmallResultBudget : public ContextHarness {
  protected:
    SmallResultBudget() { limits.tool_result_max_tokens = 600; }
};
TEST_F(SmallResultBudget, SummaryEnvelopeStillObeysThePerToolResultBudget) {
    // ADR-0098: a normal summary within O can exceed R; keep the completed original and hold.
    const auto task = start("chat", "one operation", 32);
    propose(0, 1);
    const std::string original(2200, 'r');
    post(1, {original.data(), uint32_t(original.size())});
    pump();
    ASSERT_EQ(pending.back().capability, "summary");
    const std::string summary(1200, 's'); // 900 estimated tokens fits O=1000, exceeds R=600.
    uint32_t chunks = 0;
    while (work.task(task).lifecycle != AYNI_TASK_PAUSED && chunks++ < 16) {
        ASSERT_EQ(pending.back().capability, "summary");
        post(uint32_t(pending.size() - 1), {summary.data(), uint32_t(summary.size())});
        pump();
    }
    EXPECT_EQ(work.task(task).hold_status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(std::count_if(pending.begin(), pending.end(),
                            [](const auto &p) { return p.capability == "echo"; }),
              1);
    expect_original(original);
}
} // namespace

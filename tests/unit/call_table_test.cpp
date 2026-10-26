// White-box call ownership scenarios, C-CALL-002/008 and ADR-0045.
// Kernel B2/B4 assembly and Provider I/O are exercised separately.
#include "work/call/call_records.h"

#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
using namespace ayni::work;
constexpr uint64_t boot = 3;

ayni::catalog::CatalogId id(const char *value) {
    ayni::catalog::CatalogId result;
    EXPECT_TRUE(result.assign({value, static_cast<uint32_t>(std::strlen(value))}));
    return result;
}
ActionStamp stamp(uint64_t sequence) {
    return {boot, {sequence, sequence}};
}

class CallTableTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ayni::foundation::IdAllocator ids;
        ids.start(boot);
        ASSERT_TRUE(ids.allocate(identity.owner_instance_id));
        ASSERT_TRUE(ids.allocate(identity.task_id));
        ASSERT_TRUE(ids.allocate(identity.call_id));
        ASSERT_TRUE(ids.allocate(identity.operation_id));
        identity.step_key = id("echo.once");
        identity.binding_id = id("mock.echo");
        identity.binding_version = 1;
        identity.capability_id = id("echo");
        identity.capability_version = 1;
        identity.provider_id = id("mock");
        identity.provider_version = 1;
        identity.endpoint = id("echo.endpoint");
        contract.form = ayni::catalog::CallForm::unary;
        contract.category = ayni::catalog::CallClass::tool;
        contract.idempotency = ayni::catalog::CallIdempotency::idempotent;
        contract.query = ayni::catalog::QueryAuthority::none;
        contract.cancel = ayni::catalog::CancelMode::request;
        contract.timeout_ms = 30000;
        contract.input_format = AYNI_CALL_INPUT_FIELDS_TEXT;
        contract.result_format = AYNI_CALL_RESULT_TEXT;
        contract.input_text_limit = 16;
        contract.result_text_limit = 16;
        const CallInput input{{sizeof(ayni_scope_v1), 0, nullptr}, {"question", 8}};
        ASSERT_EQ(encode_call_input({domain.data(), static_cast<uint32_t>(domain.size())}, input,
                                    contract.input_text_limit, input_bytes.data(),
                                    static_cast<uint32_t>(input_bytes.size()), input_size),
                  AYNI_OK);
    }
    void insert() {
        ASSERT_EQ(calls.insert(identity, contract,
                               {domain.data(), static_cast<uint32_t>(domain.size())},
                               {input_bytes.data(), input_size}, stamp(1), index),
                  AYNI_OK);
    }
    void dispatch() {
        insert();
        ASSERT_EQ(calls.prepare(index, stamp(2)), AYNI_OK);
        ASSERT_EQ(calls.begin_dispatch(index, {boot, 5}, stamp(3), 2), AYNI_OK);
        ASSERT_TRUE(calls.claim_dispatch(index, boot, 3));
    }
    static constexpr CallTable::Limits limits{1, 32, 64, 16};
    std::vector<uint64_t> storage =
        std::vector<uint64_t>(CallTable::storage_size(limits) / sizeof(uint64_t) + 1);
    CallTable calls{storage.data(), limits};
    CallIdentity identity{};
    ayni::catalog::CallContract contract{};
    std::array<uint8_t, 4> domain{{1, 0, 0, 0}};
    std::array<uint8_t, 64> input_bytes{};
    uint32_t input_size = 0;
    uint32_t index = CallTable::no_index;
};

// ADR-0047/records.md: the Core queue outcome requires v2 but retains the v1 row layout.
TEST_F(CallTableTest, skipped_wait_encodes_non_delivery_and_does_not_allow_a_dispatched_attempt) {
    insert();
    ASSERT_EQ(calls.skip_before_dispatch(index, stamp(2)), AYNI_OK);
    const auto saved = calls.record(index);
    ASSERT_TRUE(CallTable::valid_record(saved));
    const auto version = call_record_version_for(saved);
    EXPECT_EQ(version, call_skipped_record_version);
    std::vector<uint8_t> bytes(call_record_size(saved));
    write_call_record(saved, bytes.data());
    CallRecord decoded{};
    ASSERT_TRUE(
        read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded, version));
    EXPECT_EQ(decoded.state.outcome, AYNI_CALL_SKIPPED);
    EXPECT_EQ(decoded.state.reason, AYNI_REASON_RESOURCE_WAIT);
    EXPECT_EQ(decoded.state.attempt.request_id, 0U);
    EXPECT_FALSE(decoded.action.has_value());
    EXPECT_FALSE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded,
                                  call_record_version));
    calls.clear();
    dispatch();
    EXPECT_EQ(calls.skip_before_dispatch(index, stamp(4)), AYNI_INVALID_STATE);
    EXPECT_EQ(calls.state(index).outcome, 0U);
}

TEST_F(CallTableTest, accepted_input_is_owned_and_step_identity_cannot_be_accepted_twice) {
    insert();
    input_bytes.fill(0);
    CallInputView view{};
    ASSERT_EQ(read_call_input(calls.domain(index), calls.input(index), 16, view), AYNI_OK);
    EXPECT_EQ(std::string(view.text.data, view.text.size), "question");
    uint32_t found = CallTable::no_index;
    EXPECT_TRUE(calls.find_step(identity.task_id, {"echo.once", 9}, found));
    EXPECT_EQ(found, index);
    EXPECT_FALSE(calls.has_room());
    EXPECT_EQ(calls.action(index), nullptr);
    EXPECT_FALSE(calls.can_retire_task(identity.task_id, UINT64_MAX));
    EXPECT_EQ(
        calls.insert(identity, contract, calls.domain(index), calls.input(index), stamp(1), found),
        AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(calls.size(), 1U);
}

TEST_F(CallTableTest, both_durable_barriers_precede_the_only_dispatch_claim) {
    insert();
    EXPECT_FALSE(calls.claim_dispatch(index, boot, UINT64_MAX));
    ASSERT_EQ(calls.prepare(index, stamp(2)), AYNI_OK);
    EXPECT_EQ(calls.begin_dispatch(index, {boot, 5}, stamp(3), 1), AYNI_BUSY);
    EXPECT_EQ(calls.state(index).attempt.request_id, 0U);
    ASSERT_EQ(calls.begin_dispatch(index, {boot, 5}, stamp(3), 2), AYNI_OK);
    EXPECT_FALSE(calls.claim_dispatch(index, boot, 2));
    EXPECT_TRUE(calls.claim_dispatch(index, boot, 3));
    EXPECT_FALSE(calls.claim_dispatch(index, boot, 3));
}

TEST_F(CallTableTest, result_and_step_facts_wait_for_durability_before_retirement) {
    dispatch();
    ASSERT_EQ(calls.progress(index, 1, stamp(4)), AYNI_OK);
    char text[] = "answer";
    const CallResult completed{
        AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {text, 6}};
    ASSERT_EQ(calls.finish(index, completed, stamp(5)), AYNI_OK);
    text[0] = 'x';
    EXPECT_EQ(std::string(calls.result(index).data, calls.result(index).size), "answer");
    EXPECT_EQ(calls.action(index)->phase, ActionPhase::completed);
    EXPECT_FALSE(calls.can_retire_task(identity.task_id, 4));
    EXPECT_TRUE(calls.can_retire_task(identity.task_id, 5));
    EXPECT_EQ(calls.finish(index, completed, stamp(6)), AYNI_INVALID_STATE);
    calls.retire_task(identity.task_id);
    EXPECT_EQ(calls.size(), 0U);
    EXPECT_TRUE(calls.has_room());
}

TEST_F(CallTableTest, cancellation_control_does_not_replace_terminal_effect_evidence) {
    dispatch();
    ASSERT_EQ(calls.request_cancel(index, CallCancelCause::user, stamp(4)), AYNI_OK);
    ASSERT_EQ(calls.cancel_response(index, AYNI_CALL_CANCEL_UNKNOWN, stamp(5)), AYNI_OK);
    EXPECT_EQ(calls.state(index).outcome, 0U);
    const CallResult cancelled{
        AYNI_CALL_CANCELLED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::partial, {nullptr, 0}};
    ASSERT_EQ(calls.finish(index, cancelled, stamp(6)), AYNI_OK);
    EXPECT_EQ(calls.state(index).cancel_result, AYNI_CALL_CANCEL_UNKNOWN);
    EXPECT_EQ(calls.action(index)->phase, ActionPhase::cancelled);
    EXPECT_EQ(calls.action(index)->effect, ActionEffect::partial);
    EXPECT_EQ(calls.action(index)->evidence, ActionEvidence::provider_result);
}

TEST_F(CallTableTest, queued_cancellation_never_creates_a_possible_dispatch) {
    insert();
    ASSERT_EQ(calls.cancel_before_dispatch(index, stamp(2)), AYNI_OK);
    EXPECT_EQ(calls.action(index), nullptr);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(calls.prepare(index, stamp(3)), AYNI_INVALID_STATE);
    EXPECT_TRUE(calls.can_retire_task(identity.task_id, 2));
}

TEST_F(CallTableTest, abandoned_outcome_retains_identity_and_cannot_be_retried_or_reclaimed) {
    dispatch();
    const CallResult unknown{AYNI_CALL_ABANDONED,
                             AYNI_RESULT_UNKNOWN,
                             AYNI_REASON_NONE,
                             ActionEffect::unknown,
                             {nullptr, 0}};
    ASSERT_EQ(calls.finish(index, unknown, stamp(4)), AYNI_OK);
    EXPECT_EQ(calls.action(index)->phase, ActionPhase::result_unknown);
    EXPECT_FALSE(calls.can_retire_task(identity.task_id, UINT64_MAX));
    EXPECT_EQ(calls.begin_dispatch(index, {boot, 6}, stamp(5), 4), AYNI_INVALID_STATE);
    EXPECT_FALSE(calls.claim_dispatch(index, boot, UINT64_MAX));
    EXPECT_EQ(calls.state(index).attempt.request_id, 5U);
}

TEST_F(CallTableTest, invalid_or_oversized_result_preserves_pending_call_and_action) {
    dispatch();
    CallResult result{AYNI_CALL_COMPLETED,
                      AYNI_OK,
                      AYNI_REASON_NONE,
                      ActionEffect::complete,
                      {"a result longer than sixteen bytes", 20}};
    EXPECT_EQ(calls.finish(index, result, stamp(4)), AYNI_CAPACITY_EXCEEDED);
    result.text = {"\xff", 1};
    EXPECT_EQ(calls.finish(index, result, stamp(4)), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(calls.state(index).outcome, 0U);
    EXPECT_EQ(calls.action(index)->phase, ActionPhase::dispatching);
    result.text = {nullptr, 0};
    EXPECT_EQ(calls.finish(index, result, stamp(4)), AYNI_OK);
}
// C-CALL-002/C-COMMIT-007: normal full-row encoding retains the original
// canonical body and action association, without serializing a submit claim.
TEST_F(CallTableTest, full_record_codec_roundtrips_acceptance_barrier_and_terminal_text) {
    insert();
    const auto roundtrip = [&] {
        const auto record = calls.record(index);
        std::vector<uint8_t> bytes(call_record_size(record));
        write_call_record(record, bytes.data());
        CallRecord decoded{};
        EXPECT_TRUE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
        EXPECT_EQ(decoded.identity.call_id, identity.call_id);
        EXPECT_EQ(decoded.state.latest.transition.commit_seq,
                  record.state.latest.transition.commit_seq);
        EXPECT_EQ(decoded.state.outcome, record.state.outcome);
        EXPECT_EQ(std::string(decoded.result.data, decoded.result.size),
                  std::string(record.result.data, record.result.size));
        EXPECT_EQ(decoded.action.has_value(), record.action.has_value());
    };
    roundtrip();
    ASSERT_EQ(calls.prepare(index, stamp(2)), AYNI_OK);
    roundtrip();
    ASSERT_EQ(calls.begin_dispatch(index, {boot, 5}, stamp(3), 2), AYNI_OK);
    roundtrip();
    ASSERT_TRUE(calls.claim_dispatch(index, boot, 3));
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {"answer", 6}},
            stamp(4)),
        AYNI_OK);
    roundtrip();
}

// C-CALL-008: classified failure does not resolve explicitly unknown effects.
TEST_F(CallTableTest, failed_attempt_with_unknown_effect_retains_its_ledger) {
    dispatch();
    ASSERT_EQ(calls.finish(
                  index,
                  {AYNI_CALL_FAILED, AYNI_NOT_READY, AYNI_REASON_NONE, ActionEffect::unknown, {}},
                  stamp(4)),
              AYNI_OK);
    EXPECT_FALSE(calls.can_retire_task(identity.task_id, UINT64_MAX));
}

// Record boundary: no truncation or partially decoded identity escapes.
TEST_F(CallTableTest, short_or_trailing_record_bytes_preserve_output) {
    insert();
    const auto record = calls.record(index);
    std::vector<uint8_t> bytes(call_record_size(record) + 1);
    write_call_record(record, bytes.data());
    CallRecord decoded{};
    decoded.identity.call_id = 42;
    EXPECT_FALSE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_FALSE(read_call_record(bytes.data(), call_record_fixed_size - 1, decoded));
    EXPECT_EQ(decoded.identity.call_id, 42U);
}

// ADR-0045: full query records retain independent attempts and exact original
// operation identity through request, barrier and known-result publication.
TEST_F(CallTableTest,
       reconciliation_record_codec_preserves_query_generation_and_execution_identity) {
    contract.query = ayni::catalog::QueryAuthority::authoritative;
    dispatch();
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, ActionEffect::unknown, {}},
            stamp(4)),
        AYNI_OK);
    const auto roundtrip = [&] {
        const auto record = calls.record(index);
        std::vector<uint8_t> bytes(call_record_size(record));
        write_call_record(record, bytes.data());
        CallRecord decoded{};
        ASSERT_TRUE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
        ASSERT_TRUE(decoded.state.query);
        EXPECT_EQ(decoded.state.attempt.request_id, 5U);
        EXPECT_EQ(decoded.identity.operation_id, identity.operation_id);
        EXPECT_EQ(decoded.state.query->generation, 1U);
        EXPECT_EQ(decoded.state.query->requested.transition.commit_seq, 5U);
        EXPECT_EQ(decoded.state.query->phase, record.state.query->phase);
        EXPECT_EQ(decoded.state.query->attempt.request_id, record.state.query->attempt.request_id);
    };
    ASSERT_EQ(calls.request_query(index, stamp(5)), AYNI_OK);
    roundtrip();
    ASSERT_EQ(calls.begin_query(index, {boot, 6}, stamp(6)), AYNI_OK);
    roundtrip();
    ASSERT_EQ(
        calls.reconcile(
            index, AYNI_QUERY_RESULT_FOUND,
            {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {"answer", 6}},
            stamp(7)),
        AYNI_OK);
    roundtrip();
    EXPECT_FALSE(calls.claim_dispatch(index, boot, UINT64_MAX));
    EXPECT_FALSE(calls.can_retire_task(identity.task_id, 6));
    EXPECT_TRUE(calls.can_retire_task(identity.task_id, 7));
}

// C-CALL-008: saved selection matching covers identities as well as the offer's
// execution shape; an equal contract cannot silently retarget accepted work.
TEST_F(CallTableTest, saved_selection_rejects_changed_provider_capability_or_destination) {
    const auto original = identity.binding();
    EXPECT_TRUE(ayni::catalog::BindingRegistry::same_selection(original, identity.binding()));
    auto replacement = original;
    replacement.provider = id("other.provider");
    EXPECT_FALSE(ayni::catalog::BindingRegistry::same_selection(original, replacement));
    replacement = original;
    ++replacement.provider_version;
    EXPECT_FALSE(ayni::catalog::BindingRegistry::same_selection(original, replacement));
    replacement = original;
    replacement.capability = id("other.capability");
    EXPECT_FALSE(ayni::catalog::BindingRegistry::same_selection(original, replacement));
    replacement = original;
    replacement.endpoint = id("other.endpoint");
    EXPECT_FALSE(ayni::catalog::BindingRegistry::same_selection(original, replacement));
    replacement = original;
    replacement.destination = id("cloud");
    EXPECT_FALSE(ayni::catalog::BindingRegistry::same_selection(original, replacement));
}

// ADR-0050: stale dependencies reject derived use without sanitizing invalid original evidence.
TEST_F(CallTableTest, stale_query_preserves_effect_but_rejects_invalid_evidence_before_filtering) {
    contract.query = ayni::catalog::QueryAuthority::authoritative;
    dispatch();
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, ActionEffect::unknown, {}},
            stamp(4)),
        AYNI_OK);
    ASSERT_EQ(calls.request_query(index, stamp(5)), AYNI_OK);
    ASSERT_EQ(calls.begin_query(index, {boot, 6}, stamp(6)), AYNI_OK);
    CallResult result{
        AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {"old", 3}};
    EXPECT_EQ(calls.reconcile(index, AYNI_QUERY_RESULT_FOUND, result, stamp(7),
                              ResultDependencies::stale),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_ABANDONED);
    result.effect = ActionEffect::complete;
    result.text = {"\xff", 1};
    EXPECT_EQ(calls.reconcile(index, AYNI_QUERY_RESULT_FOUND, result, stamp(7),
                              ResultDependencies::stale),
              AYNI_INVALID_ARGUMENT);
    result.text = {"old", 3};
    ASSERT_EQ(calls.reconcile(index, AYNI_QUERY_RESULT_FOUND, result, stamp(7),
                              ResultDependencies::stale),
              AYNI_OK);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(calls.state(index).status, AYNI_STALE);
    EXPECT_EQ(calls.action(index)->effect, ActionEffect::complete);
    EXPECT_EQ(calls.result(index).size, 0U);
    EXPECT_TRUE(CallTable::valid_record(calls.record(index)));
}
} // namespace

// ADR-0054: a TOOL_CALLS completion keeps its typed list as the result body,
// and every row of such a contract is v4 so old readers refuse it explicitly.
TEST_F(CallTableTest, tool_call_list_result_is_retained_and_round_trips_as_v4) {
    contract.category = ayni::catalog::CallClass::model;
    contract.result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    contract.result_text_limit = 64;
    constexpr CallTable::Limits tool_limits{1, 32, 64, 64};
    std::vector<uint64_t> tool_storage(CallTable::storage_size(tool_limits) / sizeof(uint64_t) + 1);
    CallTable table{tool_storage.data(), tool_limits};
    ASSERT_EQ(table.insert(identity, contract,
                           {domain.data(), static_cast<uint32_t>(domain.size())},
                           {input_bytes.data(), input_size}, stamp(1), index),
              AYNI_OK);
    EXPECT_EQ(call_record_version_for(table.record(index)), call_tool_calls_record_version);
    ASSERT_EQ(table.prepare(index, stamp(2)), AYNI_OK);
    ASSERT_EQ(table.begin_dispatch(index, {boot, 5}, stamp(3), 2), AYNI_OK);
    ASSERT_TRUE(table.claim_dispatch(index, boot, 3));

    const ayni_tool_call_v1 call{
        sizeof(call), 1, {"lamp.off", 8}, {sizeof(ayni_scope_v1), 0, nullptr}, {"now", 3}};
    uint32_t size = 0;
    ASSERT_EQ(measure_tool_calls(&call, 1, size), AYNI_OK);
    std::vector<uint8_t> list(size);
    write_tool_calls(&call, 1, list.data(), size);
    const ayni_string_v1 body{reinterpret_cast<const char *>(list.data()), size};
    EXPECT_EQ(table.finish(index,
                           {AYNI_CALL_FAILED, AYNI_INVALID_ARGUMENT, AYNI_REASON_MODEL_OUTPUT,
                            ActionEffect::none, body, CallResultKind::tool_calls},
                           stamp(4)),
              AYNI_INVALID_ARGUMENT);
    const std::vector<uint8_t> broken(list.begin(), list.end() - 1);
    EXPECT_EQ(table.finish(index,
                           {AYNI_CALL_COMPLETED,
                            AYNI_OK,
                            AYNI_REASON_NONE,
                            ActionEffect::complete,
                            {reinterpret_cast<const char *>(broken.data()), size - 1U},
                            CallResultKind::tool_calls},
                           stamp(4)),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(table.state(index).outcome, 0U);
    ASSERT_EQ(table.finish(index,
                           {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete,
                            body, CallResultKind::tool_calls},
                           stamp(4)),
              AYNI_OK);
    EXPECT_EQ(table.result(index).size, 0U);
    ASSERT_EQ(table.tool_calls(index).size, size);
    EXPECT_EQ(tool_call_count(table.tool_calls(index)), 1U);

    const auto record = table.record(index);
    EXPECT_EQ(record.result_kind, CallResultKind::tool_calls);
    ASSERT_EQ(call_record_version_for(record), call_tool_calls_record_version);
    std::vector<uint8_t> bytes(call_record_size(record));
    write_call_record(record, bytes.data());
    CallRecord decoded{};
    EXPECT_FALSE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    ASSERT_TRUE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded,
                                 call_tool_calls_record_version));
    EXPECT_EQ(decoded.result_kind, CallResultKind::tool_calls);
    std::vector<uint64_t> restored_storage(tool_storage.size());
    CallTable restored{restored_storage.data(), tool_limits};
    ASSERT_EQ(restored.restore_validated(decoded), AYNI_OK);
    ASSERT_TRUE(restored.find(identity.call_id, index));
    EXPECT_EQ(restored.result(index).size, 0U);
    EXPECT_EQ(tool_call_count(restored.tool_calls(index)), 1U);
    // An invalid kind tail or a list in a text contract is refused, not reinterpreted.
    ayni::foundation::store_le32(bytes.data() + bytes.size() - 4U, 3U);
    EXPECT_FALSE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded,
                                  call_tool_calls_record_version));
    auto text_record = record;
    text_record.contract.result_format = AYNI_CALL_RESULT_TEXT;
    text_record.contract.category = ayni::catalog::CallClass::tool;
    EXPECT_FALSE(CallTable::valid_record(text_record));
}

// A text contract never accepts a tool list, nor a list over its result bound.
TEST_F(CallTableTest, tool_call_list_needs_its_contract_and_fits_its_bound) {
    const ayni_tool_call_v1 call{
        sizeof(call), 1, {"lamp.off", 8}, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    uint32_t size = 0;
    ASSERT_EQ(measure_tool_calls(&call, 1, size), AYNI_OK);
    ASSERT_GT(size, contract.result_text_limit);
    std::vector<uint8_t> list(size);
    write_tool_calls(&call, 1, list.data(), size);
    const CallResult result{AYNI_CALL_COMPLETED,
                            AYNI_OK,
                            AYNI_REASON_NONE,
                            ActionEffect::complete,
                            {reinterpret_cast<const char *>(list.data()), size},
                            CallResultKind::tool_calls};
    dispatch();
    EXPECT_EQ(calls.finish(index, result, stamp(4)), AYNI_INVALID_ARGUMENT);
    calls.clear();
    contract.category = ayni::catalog::CallClass::model;
    contract.result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    dispatch();
    EXPECT_EQ(calls.finish(index, result, stamp(4)), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(calls.state(index).outcome, 0U);
}

#include "kernel/service/persistence/work_replay.h"
// White-box call ownership scenarios, C-CALL-002/008 and ADR-0045.
// Kernel B2/B4 assembly and Provider I/O are exercised separately.
#include "work/call/call_records.h"

#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <initializer_list>
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
    std::vector<uint8_t> encoded() const {
        const auto record = calls.record(index);
        std::vector<uint8_t> bytes(call_record_size(record));
        write_call_record(record, bytes.data());
        return bytes;
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
    std::vector<uint8_t> bytes(call_record_size(saved));
    write_call_record(saved, bytes.data());
    CallRecord decoded{};
    ASSERT_TRUE(
        read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.state.outcome, AYNI_CALL_SKIPPED);
    EXPECT_EQ(decoded.state.reason, AYNI_REASON_RESOURCE_WAIT);
    EXPECT_EQ(decoded.state.attempt.request_id, 0U);
    EXPECT_FALSE(decoded.action.has_value());
    EXPECT_EQ(ayni::kernel::WorkReplay::classify_header(
                  {call_record_type, static_cast<uint16_t>(call_record_version + 1),
                   ayni::durability::record_required, 0}),
              ayni::durability::RecordUse::unknown);
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

// C-CALL-002: insertion owns intrinsic identity checks. Invalid identities
// cannot consume the only slot or publish a caller's output index.
TEST_F(CallTableTest, invalid_admission_identity_and_stamps_leave_the_slot_unclaimed) {
    ayni::foundation::IdAllocator allocator;
    allocator.start(boot + 1);
    uint64_t future = 0;
    ASSERT_TRUE(allocator.allocate(future));
    const auto invalid = {
        +[](CallIdentity &v) { v.call_id = 0; },
        +[](CallIdentity &v) { v.operation_id = 0; },
        +[](CallIdentity &v) { v.task_id = 0; },
        +[](CallIdentity &v) { v.owner_instance_id = 0; },
        +[](CallIdentity &v) { v.call_id = v.operation_id; },
        +[](CallIdentity &v) { v.call_id = v.task_id; },
        +[](CallIdentity &v) { v.operation_id = v.task_id; },
        +[](CallIdentity &v) { v.step_key = {}; },
        +[](CallIdentity &v) { v.binding_id = {}; },
        +[](CallIdentity &v) { v.binding_version = 0; },
        +[](CallIdentity &v) { v.capability_id = {}; },
        +[](CallIdentity &v) { v.capability_version = 0; },
        +[](CallIdentity &v) { v.provider_id = {}; },
        +[](CallIdentity &v) { v.provider_version = 0; },
        +[](CallIdentity &v) { v.endpoint = {}; },
        +[](CallIdentity &v) { v.destination = id("unexpected"); },
    };
    const auto reject = [&](const CallIdentity &candidate, ActionStamp when) {
        constexpr uint32_t unpublished = 99;
        uint32_t output = unpublished;
        EXPECT_EQ(calls.insert(candidate, contract,
                               {domain.data(), static_cast<uint32_t>(domain.size())},
                               {input_bytes.data(), input_size}, when, output),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(output, unpublished);
        EXPECT_EQ(calls.size(), 0U);
        EXPECT_TRUE(calls.has_room());
    };
    for (const auto change : invalid) {
        auto candidate = identity;
        change(candidate);
        reject(candidate, stamp(1));
    }
    for (auto member : {&CallIdentity::call_id, &CallIdentity::operation_id, &CallIdentity::task_id,
                        &CallIdentity::owner_instance_id}) {
        auto candidate = identity;
        candidate.*member = future;
        reject(candidate, stamp(1));
    }
    auto when = stamp(1);
    when.transition.transition_id = 0;
    reject(identity, when);
    when = stamp(1);
    when.transition.commit_seq = 0;
    reject(identity, when);
    insert();
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

// C-CALL-008: a terminal's outcome/status/effect/body form one fact. Reject
// inconsistent Provider evidence before changing either the Call or action ledger.
TEST_F(CallTableTest, inconsistent_terminal_results_preserve_the_complete_pending_record) {
    dispatch();
    const auto before = encoded();
    const CallResult valid{
        AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {}};
    const auto invalid = {
        CallResult{AYNI_CALL_PROGRESS, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {}},
        CallResult{
            AYNI_CALL_COMPLETED, AYNI_NOT_READY, AYNI_REASON_NONE, ActionEffect::complete, {}},
        CallResult{
            AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_PROVIDER_RESULT, ActionEffect::complete, {}},
        CallResult{AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {}},
        CallResult{AYNI_CALL_FAILED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {}},
        CallResult{
            AYNI_CALL_FAILED, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, ActionEffect::unknown, {}},
        CallResult{AYNI_CALL_CANCELLED, AYNI_NOT_READY, AYNI_REASON_NONE, ActionEffect::none, {}},
        CallResult{
            AYNI_CALL_CANCELLED, AYNI_OK, AYNI_REASON_PROVIDER_RESULT, ActionEffect::none, {}},
        CallResult{AYNI_CALL_ABANDONED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::unknown, {}},
        CallResult{
            AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, ActionEffect::none, {}},
        CallResult{AYNI_CALL_ABANDONED,
                   AYNI_RESULT_UNKNOWN,
                   AYNI_REASON_NONE,
                   ActionEffect::unknown,
                   {"unproven", 8}},
        CallResult{AYNI_CALL_FAILED,
                   AYNI_NOT_READY,
                   AYNI_REASON_NONE,
                   static_cast<ActionEffect>(UINT32_MAX),
                   {}},
        CallResult{AYNI_CALL_COMPLETED,
                   AYNI_OK,
                   AYNI_REASON_NONE,
                   ActionEffect::complete,
                   {},
                   static_cast<CallResultKind>(UINT32_MAX)},
    };
    for (const auto &result : invalid) {
        SCOPED_TRACE(::testing::Message() << result.outcome << '/' << result.status << '/'
                                          << static_cast<uint32_t>(result.effect));
        EXPECT_EQ(calls.finish(index, result, stamp(4)), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(encoded(), before);
    }
    ASSERT_EQ(calls.finish(index, valid, stamp(4)), AYNI_OK);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_COMPLETED);
}

// C-CALL-002/008: valid states cannot advance with stale stamps, duplicate
// control, a missing action/attempt, or an outcome from the wrong control lane.
TEST_F(CallTableTest, rejected_lifecycle_operations_preserve_acceptance_and_action_facts) {
    insert();
    const auto reject = [&](auto operation, ayni_status_t expected = AYNI_INVALID_STATE) {
        const auto before = encoded();
        EXPECT_EQ(operation(), expected);
        EXPECT_EQ(encoded(), before);
    };
    reject([&] { return calls.progress(index, 1, stamp(2)); });
    reject([&] { return calls.rejected_not_retained(index, stamp(2)); });
    reject([&] { return calls.begin_dispatch(index, {boot, 1}, stamp(2), 1); });
    reject([&] { return calls.prepare(index, stamp(1)); });
    auto stale = stamp(2);
    --stale.boot_epoch;
    reject([&] { return calls.prepare(index, stale); });
    stale = stamp(2);
    stale.transition.transition_id = 1;
    reject([&] { return calls.prepare(index, stale); });
    stale = stamp(2);
    stale.transition.commit_seq = 1;
    reject([&] { return calls.prepare(index, stale); });
    ASSERT_EQ(calls.prepare(index, stamp(2)), AYNI_OK);
    reject([&] { return calls.prepare(index, stamp(3)); });
    reject([&] { return calls.progress(index, 1, stamp(3)); });
    reject([&] { return calls.begin_dispatch(index, {boot, 0}, stamp(3), 2); });
    reject([&] { return calls.begin_dispatch(index, {boot + 1, 1}, stamp(3), 2); });
    ASSERT_EQ(calls.begin_dispatch(index, {boot, 1}, stamp(3), 2), AYNI_OK);
    ASSERT_TRUE(calls.claim_dispatch(index, boot, 3));
    reject([&] { return calls.cancel_response(index, AYNI_CALL_CANNOT_CANCEL, stamp(4)); });
    reject([&] { return calls.progress(index, 0, stamp(4)); });
    reject([&] { return calls.request_cancel(index, CallCancelCause::none, stamp(4)); },
           AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(calls.request_cancel(index, CallCancelCause::deadline, stamp(4)), AYNI_OK);
    reject([&] { return calls.request_cancel(index, CallCancelCause::shutdown, stamp(5)); });
    reject([&] { return calls.cancel_response(index, AYNI_CALL_CANCELLED, stamp(5)); },
           AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(calls.cancel_response(index, AYNI_CALL_CANNOT_CANCEL, stamp(5)), AYNI_OK);
    reject([&] { return calls.cancel_response(index, AYNI_CALL_CANCEL_UNKNOWN, stamp(6)); });
    ASSERT_EQ(calls.progress(index, 1, stamp(6)), AYNI_OK);
    reject([&] { return calls.progress(index, 1, stamp(7)); });
    const CallResult terminal{
        AYNI_CALL_CANCELLED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {}};
    ASSERT_EQ(calls.finish(index, terminal, stamp(7)), AYNI_OK);
    reject([&] { return calls.progress(index, 2, stamp(8)); });
    reject([&] { return calls.request_cancel(index, CallCancelCause::user, stamp(8)); });
    reject([&] { return calls.cancel_response(index, AYNI_CALL_CANCEL_UNKNOWN, stamp(8)); });
    reject([&] { return calls.rejected_not_retained(index, stamp(8)); });
    reject([&] { return calls.cancel_before_dispatch(index, stamp(8)); });
    ASSERT_EQ(calls.erase_content(index, stamp(8)), AYNI_OK);
    reject([&] { return calls.erase_content(index, stamp(9)); });
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

// ADR-0060: erasure is independent of execution outcome and has an explicit REQUIRED codec.
TEST_F(CallTableTest, erased_completed_call_retains_action_facts_and_restores_without_plaintext) {
    dispatch();
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {"answer", 6}},
            stamp(4)),
        AYNI_OK);
    ASSERT_EQ(calls.erase_content(index, stamp(5)), AYNI_OK);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(calls.action(index)->phase, ActionPhase::completed);
    EXPECT_EQ(calls.input(index).size, 0U);
    EXPECT_EQ(calls.result(index).size, 0U);
    const auto *begin = reinterpret_cast<const char *>(storage.data());
    const auto *end = begin + storage.size() * sizeof(uint64_t);
    for (const std::string marker : {"question", "answer"}) {
        EXPECT_EQ(std::search(begin, end, marker.begin(), marker.end()), end);
    }
    const auto record = calls.record(index);
    ASSERT_TRUE(CallTable::valid_record(record));
    EXPECT_TRUE(record.state.content_erased.has_value());
    std::vector<uint8_t> encoded(call_record_size(record));
    write_call_record(record, encoded.data());
    CallRecord decoded{};
    ASSERT_TRUE(read_call_record(encoded.data(), static_cast<uint32_t>(encoded.size()), decoded));
    std::vector<uint64_t> restored_storage(storage.size());
    CallTable restored(restored_storage.data(), limits);
    ASSERT_EQ(restored.restore_validated(decoded), AYNI_OK);
    uint32_t restored_index = 0;
    ASSERT_TRUE(restored.find(identity.call_id, restored_index));
    EXPECT_EQ(restored.state(restored_index).outcome, AYNI_CALL_COMPLETED);
    ASSERT_TRUE(restored.state(restored_index).content_erased.has_value());
    EXPECT_EQ(restored.state(restored_index).content_erased->transition.commit_seq, 5U);
    EXPECT_EQ(restored.input(restored_index).size, 0U);
}
// A late real completion still advances the ledger, but cannot refill an erased content region.
TEST_F(CallTableTest, late_completion_cannot_refill_erased_call_bodies) {
    dispatch();
    ASSERT_EQ(calls.erase_content(index, stamp(4)), AYNI_OK);
    ASSERT_EQ(calls.finish(index,
                           {AYNI_CALL_COMPLETED,
                            AYNI_OK,
                            AYNI_REASON_NONE,
                            ActionEffect::complete,
                            {"late answer", 11}},
                           stamp(5)),
              AYNI_OK);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(calls.result(index).size, 0U);
    EXPECT_EQ(calls.input(index).size, 0U);
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
    EXPECT_EQ(table.record(index).contract.result_format, AYNI_CALL_RESULT_TOOL_CALLS);
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
    ASSERT_EQ(record.contract.result_format, AYNI_CALL_RESULT_TOOL_CALLS);
    std::vector<uint8_t> bytes(call_record_size(record));
    write_call_record(record, bytes.data());
    CallRecord decoded{};
    EXPECT_EQ(ayni::kernel::WorkReplay::classify_header(
                  {call_record_type, static_cast<uint16_t>(call_record_version + 1),
                   ayni::durability::record_required, 0}),
              ayni::durability::RecordUse::unknown);
    ASSERT_TRUE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.result_kind, CallResultKind::tool_calls);
    std::vector<uint64_t> restored_storage(tool_storage.size());
    CallTable restored{restored_storage.data(), tool_limits};
    ASSERT_EQ(restored.restore_validated(decoded), AYNI_OK);
    ASSERT_TRUE(restored.find(identity.call_id, index));
    EXPECT_EQ(restored.result(index).size, 0U);
    EXPECT_EQ(tool_call_count(restored.tool_calls(index)), 1U);
    // An invalid kind tail or a list in a text contract is refused, not reinterpreted.
    ayni::foundation::store_le32(bytes.data() + bytes.size() - 4U, 3U);
    EXPECT_FALSE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    auto text_record = record;
    text_record.contract.result_format = AYNI_CALL_RESULT_TEXT;
    text_record.contract.category = ayni::catalog::CallClass::tool;
    EXPECT_FALSE(CallTable::valid_record(text_record));
    // ADR-0060: the typed list and its kind are erased, while the real completion remains.
    ASSERT_EQ(table.erase_content(index, stamp(5)), AYNI_OK);
    const auto erased = table.record(index);
    ASSERT_TRUE(erased.state.content_erased.has_value());
    bytes.resize(call_record_size(erased));
    write_call_record(erased, bytes.data());
    ASSERT_TRUE(read_call_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.result.size, 0U);
    EXPECT_EQ(decoded.result_kind, CallResultKind::text);
    EXPECT_EQ(decoded.state.outcome, AYNI_CALL_COMPLETED);
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

// ADR-0060: a conclusive query retains the real outcome after erasure, never its late body.
TEST_F(CallTableTest, authoritative_query_after_erasure_keeps_outcome_without_refilling_content) {
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
    ASSERT_EQ(calls.erase_content(index, stamp(7)), AYNI_OK);
    ASSERT_EQ(calls.reconcile(index, AYNI_QUERY_RESULT_FOUND,
                              {AYNI_CALL_COMPLETED,
                               AYNI_OK,
                               AYNI_REASON_NONE,
                               ActionEffect::complete,
                               {"late answer", 11}},
                              stamp(8)),
              AYNI_OK);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(calls.result(index).size, 0U);
    EXPECT_EQ(calls.input(index).size, 0U);
    EXPECT_TRUE(CallTable::valid_record(calls.record(index)));
}

namespace {
// White-box, C-CALL-002/008 and C-STORE-007/009: decoded facts must pass the same
// intrinsic validator before any row is installed. Each case breaks one rule
// of a row produced through the real call lifecycle, with a valid control first.
struct InvalidCallFact {
    const char *scenario;
    void (*change)(CallRecord &);
};
void expect_invalid_call_facts(const CallRecord &valid,
                               std::initializer_list<InvalidCallFact> cases) {
    ASSERT_TRUE(CallTable::valid_record(valid));
    for (const auto &test : cases) {
        SCOPED_TRACE(test.scenario);
        auto candidate = valid;
        test.change(candidate);
        EXPECT_FALSE(CallTable::valid_record(candidate));
    }
}
uint64_t first_id_in_boot(uint64_t epoch) {
    ayni::foundation::IdAllocator allocator;
    allocator.start(epoch);
    uint64_t result = 0;
    EXPECT_TRUE(allocator.allocate(result));
    return result;
}
} // namespace

TEST_F(CallTableTest, restored_pending_facts_reject_invalid_identity_history_and_delivery_shapes) {
    insert();
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"zero call", [](auto &r) { r.identity.call_id = 0; }},
            {"zero operation", [](auto &r) { r.identity.operation_id = 0; }},
            {"zero task", [](auto &r) { r.identity.task_id = 0; }},
            {"zero owner", [](auto &r) { r.identity.owner_instance_id = 0; }},
            {"empty step", [](auto &r) { r.identity.step_key = {}; }},
            {"empty binding", [](auto &r) { r.identity.binding_id = {}; }},
            {"zero binding version", [](auto &r) { r.identity.binding_version = 0; }},
            {"empty capability", [](auto &r) { r.identity.capability_id = {}; }},
            {"zero capability version", [](auto &r) { r.identity.capability_version = 0; }},
            {"empty provider", [](auto &r) { r.identity.provider_id = {}; }},
            {"zero provider version", [](auto &r) { r.identity.provider_version = 0; }},
            {"empty endpoint", [](auto &r) { r.identity.endpoint = {}; }},
            {"future call identity",
             [](auto &r) { r.identity.call_id = first_id_in_boot(boot + 1); }},
            {"zero creation boot", [](auto &r) { r.state.created.boot_epoch = 0; }},
            {"zero creation sequence", [](auto &r) { r.state.created.transition.commit_seq = 0; }},
            {"zero creation transition",
             [](auto &r) { r.state.created.transition.transition_id = 0; }},
            {"latest boot regressed", [](auto &r) { --r.state.latest.boot_epoch; }},
            {"latest sequence regressed", [](auto &r) { --r.state.latest.transition.commit_seq; }},
            {"latest transition regressed",
             [](auto &r) { --r.state.latest.transition.transition_id; }},
            {"latest boot exhausted", [](auto &r) { r.state.latest.boot_epoch = UINT64_MAX; }},
            {"undeclared destination", [](auto &r) { r.identity.destination = id("elsewhere"); }},
            {"missing egress destination",
             [](auto &r) { r.contract.effects |= ayni::catalog::call_effect_egress; }},
            {"invalid form",
             [](auto &r) { r.contract.form = static_cast<ayni::catalog::CallForm>(0); }},
            {"zero deadline", [](auto &r) { r.contract.timeout_ms = 0; }},
            {"missing input", [](auto &r) { r.input = {}; }},
            {"missing schema", [](auto &r) { r.domain = {}; }},
            {"attempt boot without request", [](auto &r) { r.state.attempt.boot_epoch = boot; }},
            {"attempt request without boot", [](auto &r) { r.state.attempt.request_id = 1; }},
            {"old attempt boot", [](auto &r) { r.state.attempt = {boot - 1, 1}; }},
            {"future attempt boot", [](auto &r) { r.state.attempt = {boot + 1, 1}; }},
            {"invalid cancel cause",
             [](auto &r) { r.state.cancel_cause = static_cast<CallCancelCause>(UINT32_MAX); }},
            {"cancel result without intent",
             [](auto &r) { r.state.cancel_result = AYNI_CALL_CANNOT_CANCEL; }},
            {"cancel result without dispatch",
             [](auto &r) {
                 r.state.cancel_cause = CallCancelCause::user;
                 r.state.cancel_result = AYNI_CALL_CANNOT_CANCEL;
             }},
            {"unproved progress", [](auto &r) { r.state.progress_sequence = 1; }},
            {"pending error", [](auto &r) { r.state.status = AYNI_NOT_READY; }},
            {"pending reason", [](auto &r) { r.state.reason = AYNI_REASON_PROVIDER_RESULT; }},
            {"pending result body", [](auto &r) { r.result = {"early", 5}; }},
            {"completed without action", [](auto &r) { r.state.outcome = AYNI_CALL_COMPLETED; }},
            {"failed without action",
             [](auto &r) {
                 r.state.outcome = AYNI_CALL_FAILED;
                 r.state.status = AYNI_NOT_READY;
             }},
            {"unknown outcome code", [](auto &r) { r.state.outcome = UINT32_MAX; }},
            {"physical effect without permit facts",
             [](auto &r) { r.contract.effects |= ayni::catalog::call_effect_physical; }},
            {"subscription fact without contract", [](auto &r) { r.state.subscription.emplace(); }},
        });
}

TEST_F(CallTableTest,
       restored_dispatch_facts_require_matching_action_identity_and_original_stamps) {
    dispatch();
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"missing action after dispatch", [](auto &r) { r.action.reset(); }},
            {"mismatched action operation", [](auto &r) { ++r.action->identity.operation_id; }},
            {"mismatched action call", [](auto &r) { ++r.action->identity.logical_call_id; }},
            {"mismatched action binding",
             [](auto &r) { r.action->identity.binding_id = id("other"); }},
            {"mismatched action version", [](auto &r) { ++r.action->identity.binding_version; }},
            {"preparation equals acceptance",
             [](auto &r) { r.action->prepared = r.state.created; }},
            {"preparation before acceptance",
             [](auto &r) { r.action->prepared.transition.commit_seq = 0; }},
            {"action exceeds call state",
             [](auto &r) { ++r.action->latest.transition.commit_seq; }},
            {"prepared action has attempt",
             [](auto &r) {
                 r.action->phase = ActionPhase::prepared;
                 r.action->latest = r.action->prepared;
             }},
            {"dispatch has no attempt", [](auto &r) { r.state.attempt = {}; }},
            {"invalid cancel result",
             [](auto &r) {
                 r.state.cancel_cause = CallCancelCause::user;
                 r.state.cancel_result = AYNI_CALL_COMPLETED;
             }},
            {"premature completed outcome", [](auto &r) { r.state.outcome = AYNI_CALL_COMPLETED; }},
            {"premature failed outcome",
             [](auto &r) {
                 r.state.outcome = AYNI_CALL_FAILED;
                 r.state.status = AYNI_NOT_READY;
             }},
            {"premature cancelled outcome", [](auto &r) { r.state.outcome = AYNI_CALL_CANCELLED; }},
            {"premature abandoned outcome",
             [](auto &r) {
                 r.state.outcome = AYNI_CALL_ABANDONED;
                 r.state.status = AYNI_RESULT_UNKNOWN;
             }},
        });
}

TEST_F(CallTableTest, completed_record_requires_success_and_terminal_action_proof) {
    dispatch();
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {"done", 4}},
            stamp(4)),
        AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"completed error status", [](auto &r) { r.state.status = AYNI_NOT_READY; }},
            {"completed reason", [](auto &r) { r.state.reason = AYNI_REASON_PROVIDER_RESULT; }},
            {"missing completed proof",
             [](auto &r) {
                 r.action.reset();
                 r.state.attempt = {};
             }},
            {"pending state with completed action",
             [](auto &r) {
                 r.state.outcome = 0;
                 r.result = {};
             }},
            {"failed state with success status",
             [](auto &r) { r.state.outcome = AYNI_CALL_FAILED; }},
            {"failed state with unknown status",
             [](auto &r) {
                 r.state.outcome = AYNI_CALL_FAILED;
                 r.state.status = AYNI_RESULT_UNKNOWN;
             }},
            {"cancelled state with completed action",
             [](auto &r) { r.state.outcome = AYNI_CALL_CANCELLED; }},
            {"abandoned state with success status",
             [](auto &r) { r.state.outcome = AYNI_CALL_ABANDONED; }},
            {"abandoned body",
             [](auto &r) {
                 r.state.outcome = AYNI_CALL_ABANDONED;
                 r.state.status = AYNI_RESULT_UNKNOWN;
             }},
            {"erased row retains input", [](auto &r) { r.state.content_erased = stamp(5); }},
        });
}

TEST_F(CallTableTest, erased_record_cannot_regain_bodies_or_lose_its_explicit_erasure_order) {
    dispatch();
    ASSERT_EQ(calls.erase_content(index, stamp(4)), AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"erasure equals creation", [](auto &r) { r.state.content_erased = r.state.created; }},
            {"erasure predates creation", [](auto &r) { r.state.content_erased = stamp(0); }},
            {"erasure boot exhausted",
             [](auto &r) { r.state.content_erased->boot_epoch = UINT64_MAX; }},
            {"plaintext input resurrected",
             [](auto &r) { r.input = {reinterpret_cast<const uint8_t *>("x"), 1}; }},
            {"plaintext result resurrected", [](auto &r) { r.result = {"x", 1}; }},
            {"erased tool body tag", [](auto &r) { r.result_kind = CallResultKind::tool_calls; }},
            {"erased schema malformed", [](auto &r) { r.domain.size = 1; }},
        });
}

// White-box, C-CALL-008/010: a management query retains its own command and
// attempt history. Corrupted evidence cannot turn UNKNOWN into a fresh retry.
TEST_F(CallTableTest, restored_queries_require_separate_attempts_and_ordered_command_evidence) {
    contract.query = ayni::catalog::QueryAuthority::authoritative;
    dispatch();
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, ActionEffect::unknown, {}},
            stamp(4)),
        AYNI_OK);
    ASSERT_EQ(calls.request_query(index, stamp(5)), AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"zero query generation", [](auto &r) { r.state.query->generation = 0; }},
            {"exhausted query generation", [](auto &r) { r.state.query->generation = UINT32_MAX; }},
            {"query undeclared",
             [](auto &r) { r.contract.query = ayni::catalog::QueryAuthority::none; }},
            {"query boot before creation",
             [](auto &r) { r.state.query->requested.boot_epoch = boot - 1; }},
            {"query boot after latest",
             [](auto &r) { r.state.query->requested.boot_epoch = boot + 1; }},
            {"query sequence equals creation",
             [](auto &r) {
                 r.state.query->requested.transition.commit_seq =
                     r.state.created.transition.commit_seq;
             }},
            {"query transition equals creation",
             [](auto &r) {
                 r.state.query->requested.transition.transition_id =
                     r.state.created.transition.transition_id;
             }},
            {"query sequence after latest",
             [](auto &r) { ++r.state.query->requested.transition.commit_seq; }},
            {"query transition after latest",
             [](auto &r) { ++r.state.query->requested.transition.transition_id; }},
            {"requested phase has attempt", [](auto &r) { r.state.query->attempt = {boot, 6}; }},
            {"requested phase has status", [](auto &r) { r.state.query->status = AYNI_NOT_READY; }},
            {"requested phase has reason",
             [](auto &r) { r.state.query->reason = AYNI_REASON_PROVIDER_RESULT; }},
            {"requested phase has conclusion",
             [](auto &r) { r.state.query->conclusion = AYNI_QUERY_RESULT_FOUND; }},
            {"unknown query phase",
             [](auto &r) { r.state.query->phase = static_cast<CallQuery::Phase>(0); }},
        });
    ASSERT_EQ(calls.begin_query(index, {boot, 6}, stamp(6)), AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"dispatch has no query attempt", [](auto &r) { r.state.query->attempt = {}; }},
            {"query attempt has no boot", [](auto &r) { r.state.query->attempt.boot_epoch = 0; }},
            {"query attempt has no request",
             [](auto &r) { r.state.query->attempt.request_id = 0; }},
            {"query attempt boot too old", [](auto &r) { --r.state.query->attempt.boot_epoch; }},
            {"query attempt boot too new", [](auto &r) { ++r.state.query->attempt.boot_epoch; }},
            {"query reuses execution attempt",
             [](auto &r) { r.state.query->attempt = r.state.attempt; }},
            {"dispatch query has error", [](auto &r) { r.state.query->status = AYNI_NOT_READY; }},
            {"dispatch query has reason",
             [](auto &r) { r.state.query->reason = AYNI_REASON_PROVIDER_RESULT; }},
            {"dispatch query has conclusion",
             [](auto &r) { r.state.query->conclusion = AYNI_QUERY_RESULT_FOUND; }},
        });
    ASSERT_EQ(calls.end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, stamp(7)), AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"unknown query claims success", [](auto &r) { r.state.query->status = AYNI_OK; }},
            {"found conclusion lacks execution result",
             [](auto &r) {
                 r.state.query->conclusion = AYNI_QUERY_RESULT_FOUND;
                 r.state.query->status = AYNI_OK;
             }},
        });
    ASSERT_EQ(calls.request_query(index, stamp(8)), AYNI_OK);
    ASSERT_EQ(calls.begin_query(index, {boot, 7}, stamp(9)), AYNI_OK);
    ASSERT_EQ(calls.reconcile(
                  index, AYNI_QUERY_RESULT_FOUND,
                  {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {}},
                  stamp(10)),
              AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"weak lookup cannot conclude",
             [](auto &r) { r.contract.query = ayni::catalog::QueryAuthority::best_effort; }},
            {"finished lookup lacks attempt", [](auto &r) { r.state.query->attempt = {}; }},
            {"finished lookup has error", [](auto &r) { r.state.query->status = AYNI_NOT_READY; }},
            {"finished lookup has reason",
             [](auto &r) { r.state.query->reason = AYNI_REASON_PROVIDER_RESULT; }},
            {"nonacceptance lacks tombstone proof",
             [](auto &r) { r.state.query->conclusion = AYNI_QUERY_RESULT_NOT_ACCEPTED; }},
            {"finished lookup unknown code",
             [](auto &r) { r.state.query->conclusion = UINT32_MAX; }},
        });
}

// White-box, ADR-0063: confirmed leases require the generation's dispatch
// proof. Valid raw subscription facts alone cannot prove the Call lifecycle.
TEST_F(CallTableTest, restored_subscription_rows_pair_contract_generation_and_dispatch_proof) {
    contract.form = ayni::catalog::CallForm::subscription;
    contract.cancel = ayni::catalog::CancelMode::confirmed;
    contract.subscription = ayni::catalog::SubscriptionContract{id("sample"), 1, 30000};
    dispatch();
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"missing lease facts", [](auto &r) { r.state.subscription.reset(); }},
            {"future generation boot",
             [](auto &r) { r.state.subscription->generation = first_id_in_boot(boot + 1); }},
            {"generation predates operation", [](auto &r) { --r.state.subscription->generation; }},
            {"starting lease has cancellation",
             [](auto &r) { r.state.cancel_cause = CallCancelCause::user; }},
            {"stopping lease lacks cancellation",
             [](auto &r) { r.state.subscription->phase = SubscriptionPhase::stopping; }},
            {"expired lease lacks cancellation",
             [](auto &r) { r.state.subscription->phase = SubscriptionPhase::expired; }},
            {"stopped lease has no terminal",
             [](auto &r) { r.state.subscription->phase = SubscriptionPhase::stopped; }},
        });
    ASSERT_EQ(calls.confirm_subscription(index, 1, stamp(4)), AYNI_OK);
    expect_invalid_call_facts(
        calls.record(index),
        {
            {"active lease has cancellation",
             [](auto &r) { r.state.cancel_cause = CallCancelCause::user; }},
            {"active lease lacks action",
             [](auto &r) {
                 r.action.reset();
                 r.state.attempt = {};
             }},
            {"active lease lacks attempt", [](auto &r) { r.state.attempt = {}; }},
            {"active lease has only prepared action",
             [](auto &r) {
                 r.action->phase = ActionPhase::prepared;
                 r.action->latest = r.action->prepared;
                 r.state.attempt = {};
             }},
            {"active lease has only dispatch barrier",
             [](auto &r) {
                 r.action->phase = ActionPhase::dispatching;
                 r.action->latest = stamp(3);
             }},
        });
}

// White-box, C-STORE-007/009 / C-CALL-002: individually legal facts cannot
// replace an accepted call's identity, normalized contract or canonical input.
TEST_F(CallTableTest, replay_rejects_legal_facts_that_retarget_an_existing_call) {
    insert();
    const auto before = calls.record(index);
    const InvalidCallFact changes[] = {
        {"different operation", [](auto &r) { ++r.identity.operation_id; }},
        {"different task", [](auto &r) { r.identity.task_id += 10; }},
        {"different owner", [](auto &r) { r.identity.owner_instance_id += 10; }},
        {"different step", [](auto &r) { r.identity.step_key = id("other"); }},
        {"different binding", [](auto &r) { r.identity.binding_id = id("other"); }},
        {"different binding version", [](auto &r) { ++r.identity.binding_version; }},
        {"different capability", [](auto &r) { r.identity.capability_id = id("other"); }},
        {"different capability version", [](auto &r) { ++r.identity.capability_version; }},
        {"different provider", [](auto &r) { r.identity.provider_id = id("other"); }},
        {"different provider version", [](auto &r) { ++r.identity.provider_version; }},
        {"different endpoint", [](auto &r) { r.identity.endpoint = id("other"); }},
        {"different timeout", [](auto &r) { ++r.contract.timeout_ms; }},
        {"different query authority",
         [](auto &r) { r.contract.query = ayni::catalog::QueryAuthority::authoritative; }},
        {"different input limit", [](auto &r) { ++r.contract.input_text_limit; }},
        {"different result limit", [](auto &r) { --r.contract.result_text_limit; }},
        {"different original creation", [](auto &r) { r.state.created = stamp(2); }},
    };
    for (const auto &test : changes) {
        SCOPED_TRACE(test.scenario);
        auto candidate = before;
        candidate.state.latest = stamp(3);
        test.change(candidate);
        ASSERT_TRUE(CallTable::valid_record(candidate));
        EXPECT_EQ(calls.restore_validated(candidate), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(calls.identity(index).operation_id, identity.operation_id);
        EXPECT_EQ(calls.state(index).latest.transition.commit_seq, 1U);
        EXPECT_EQ(calls.size(), 1U);
    }
    auto candidate = before;
    candidate.state.latest = stamp(3);
    std::array<uint8_t, 64> replacement{};
    uint32_t replacement_size = 0;
    ASSERT_EQ(encode_call_input(before.domain,
                                {{sizeof(ayni_scope_v1), 0, nullptr}, {"changed", 7}},
                                contract.input_text_limit, replacement.data(), replacement.size(),
                                replacement_size),
              AYNI_OK);
    candidate.input = {replacement.data(), replacement_size};
    ASSERT_TRUE(CallTable::valid_record(candidate));
    EXPECT_EQ(calls.restore_validated(candidate), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(calls.state(index).latest.transition.commit_seq, 1U);
    EXPECT_EQ(calls.restore_validated(before), AYNI_INVALID_ARGUMENT);
    // A valid strictly later row with the original identity/input is installable.
    candidate = before;
    candidate.state.latest = stamp(2);
    ASSERT_TRUE(CallTable::valid_record(candidate));
    EXPECT_EQ(calls.restore_validated(candidate), AYNI_OK);
}

// White-box, ADR-0063 / C-CALL-008: known subscription closure does not allow
// rewriting the original result; only a separately allocated generation reopens.
TEST_F(CallTableTest, replay_preserves_known_subscription_result_until_a_new_generation) {
    contract.form = ayni::catalog::CallForm::subscription;
    contract.cancel = ayni::catalog::CancelMode::confirmed;
    contract.subscription = ayni::catalog::SubscriptionContract{id("sample"), 1, 30000};
    dispatch();
    ASSERT_EQ(calls.confirm_subscription(index, 1, stamp(4)), AYNI_OK);
    ASSERT_EQ(calls.stop_subscription(index, CallCancelCause::shutdown, SubscriptionDesired::open,
                                      stamp(5)),
              AYNI_OK);
    ASSERT_EQ(calls.finish(index,
                           {AYNI_CALL_CANCELLED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {}},
                           stamp(6)),
              AYNI_OK);
    const auto stopped = calls.record(index);
    ASSERT_TRUE(CallTable::valid_record(stopped));
    const InvalidCallFact changes[] = {
        {"different progress", [](auto &r) { ++r.state.progress_sequence; }},
        {"different result body", [](auto &r) { r.result = {"changed", 7}; }},
        {"different lease phase",
         [](auto &r) { r.state.subscription->phase = SubscriptionPhase::failed; }},
        {"different lease acknowledgement",
         [](auto &r) { ++r.state.subscription->lease_sequence; }},
        {"different measured effect", [](auto &r) { r.action->effect = ActionEffect::partial; }},
        {"different action terminal stamp", [](auto &r) { r.action->latest = stamp(7); }},
    };
    for (const auto &test : changes) {
        SCOPED_TRACE(test.scenario);
        auto candidate = stopped;
        candidate.state.latest = stamp(7);
        test.change(candidate);
        ASSERT_TRUE(CallTable::valid_record(candidate));
        EXPECT_EQ(calls.restore_validated(candidate), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(calls.state(index).latest.transition.commit_seq, 6U);
        EXPECT_EQ(calls.state(index).subscription->lease_sequence, 1U);
        EXPECT_EQ(calls.result(index).size, 0U);
    }
    std::vector<uint64_t> other_storage(storage.size());
    CallTable restored(other_storage.data(), limits);
    ASSERT_EQ(restored.restore_validated(stopped), AYNI_OK);
    const auto generation = identity.operation_id + 1;
    ASSERT_EQ(calls.restart_subscription(index, generation, stamp(7)), AYNI_OK);
    const auto reopened = calls.record(index);
    ASSERT_TRUE(CallTable::valid_record(reopened));
    EXPECT_EQ(restored.restore_validated(reopened), AYNI_OK);
    EXPECT_EQ(restored.state(index).subscription->generation, generation);
    EXPECT_EQ(restored.state(index).outcome, 0U);
    EXPECT_EQ(restored.state(index).attempt.request_id, 0U);
    auto queried_generation = reopened;
    queried_generation.state.query =
        CallQuery{1, CallQuery::Phase::requested, queried_generation.state.latest};
    EXPECT_FALSE(CallTable::valid_record(queried_generation));
}

// White-box, C-CALL-008 / ADR-0063: a legal new-generation row from a known
// stop must not replace a different history whose old operation remains UNKNOWN.
TEST_F(CallTableTest, replay_cannot_reopen_an_unknown_subscription_with_a_legal_new_generation) {
    contract.form = ayni::catalog::CallForm::subscription;
    contract.cancel = ayni::catalog::CancelMode::confirmed;
    contract.subscription = ayni::catalog::SubscriptionContract{id("sample"), 1, 30000};
    insert();
    const auto accepted = calls.record(index);
    ASSERT_TRUE(CallTable::valid_record(accepted));
    ASSERT_EQ(calls.prepare(index, stamp(2)), AYNI_OK);
    ASSERT_EQ(calls.begin_dispatch(index, {boot, 5}, stamp(3), 2), AYNI_OK);
    ASSERT_TRUE(calls.claim_dispatch(index, boot, 3));
    ASSERT_EQ(
        calls.finish(
            index,
            {AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, ActionEffect::unknown, {}},
            stamp(4)),
        AYNI_OK);
    const auto unknown = calls.record(index);
    ASSERT_TRUE(CallTable::valid_record(unknown));
    ASSERT_TRUE(calls.unresolved(index));
    const auto generation = identity.operation_id + 1;
    EXPECT_EQ(calls.restart_subscription(index, generation, stamp(5)), AYNI_INVALID_STATE);

    std::vector<uint64_t> alternate_storage(storage.size());
    CallTable alternate(alternate_storage.data(), limits);
    ASSERT_EQ(alternate.restore_validated(accepted), AYNI_OK);
    ASSERT_EQ(alternate.prepare(index, stamp(2)), AYNI_OK);
    ASSERT_EQ(alternate.begin_dispatch(index, {boot, 5}, stamp(3), 2), AYNI_OK);
    ASSERT_TRUE(alternate.claim_dispatch(index, boot, 3));
    ASSERT_EQ(alternate.stop_subscription(index, CallCancelCause::shutdown,
                                          SubscriptionDesired::open, stamp(4)),
              AYNI_OK);
    ASSERT_EQ(alternate.finish(
                  index, {AYNI_CALL_CANCELLED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {}},
                  stamp(5)),
              AYNI_OK);
    ASSERT_EQ(alternate.restart_subscription(index, generation, stamp(6)), AYNI_OK);
    const auto reopened = alternate.record(index);
    ASSERT_TRUE(CallTable::valid_record(reopened));
    EXPECT_EQ(calls.restore_validated(reopened), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(calls.state(index).outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(calls.state(index).latest.transition.commit_seq,
              unknown.state.latest.transition.commit_seq);
    EXPECT_EQ(calls.state(index).subscription->generation, unknown.state.subscription->generation);
    EXPECT_TRUE(calls.unresolved(index));
    EXPECT_FALSE(calls.can_retire_task(identity.task_id, UINT64_MAX));
}

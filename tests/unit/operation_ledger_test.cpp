// White-box ADR-0115: independent arena, scalar reconciliation, atomic admission and codecs.
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "work/action/action_records.h"
#include "work/operation/operation_records.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>
namespace {
using namespace ayni::work;
constexpr uint64_t boot = 3;
ActionStamp stamp(uint64_t seq) {
    return {boot, {seq, seq}};
}
ayni::catalog::CatalogId id(const char *text) {
    ayni::catalog::CatalogId value;
    EXPECT_TRUE(value.assign({text, static_cast<uint32_t>(std::strlen(text))}));
    return value;
}
class OperationLedgerTest : public ::testing::Test {
  protected:
    static constexpr OperationLedger::Limits limits{1, 64};
    std::vector<uint64_t> arena =
        std::vector<uint64_t>(OperationLedger::storage_size(limits) / sizeof(uint64_t) + 1);
    OperationLedger ledger{arena.data(), limits};
    CallIdentity identity{};
    ayni::catalog::CallContract contract{};
    CallState state{};
    std::optional<ActionRecord> action;
    std::array<uint8_t, 4> proof{{1, 2, 3, 4}};
    void SetUp() override {
        ayni::foundation::IdAllocator ids;
        ids.start(boot);
        ASSERT_TRUE(ids.allocate(identity.owner_instance_id));
        ASSERT_TRUE(ids.allocate(identity.task_id));
        ASSERT_TRUE(ids.allocate(identity.call_id));
        ASSERT_TRUE(ids.allocate(identity.operation_id));
        identity.step_key = id("step");
        identity.binding_id = id("binding");
        identity.binding_version = 1;
        identity.capability_id = id("cap");
        identity.capability_version = 1;
        identity.provider_id = id("provider");
        identity.provider_version = 1;
        identity.endpoint = id("endpoint");
        contract.form = ayni::catalog::CallForm::unary;
        contract.category = ayni::catalog::CallClass::tool;
        contract.idempotency = ayni::catalog::CallIdempotency::not_idempotent;
        contract.query = ayni::catalog::QueryAuthority::authoritative;
        contract.cancel = ayni::catalog::CancelMode::request;
        contract.timeout_ms = 1000;
        contract.input_format = AYNI_CALL_INPUT_VALUE;
        contract.result_format = AYNI_CALL_RESULT_TEXT;
        contract.business_contract = {proof.data(), static_cast<uint32_t>(proof.size())};
        state.created = stamp(1);
        state.latest = stamp(4);
        state.attempt = {boot, 1};
        state.outcome = AYNI_CALL_ABANDONED;
        state.status = AYNI_RESULT_UNKNOWN;
        ASSERT_EQ(
            ActionRecord::prepare({identity.operation_id, identity.call_id, identity.binding_id, 1},
                                  stamp(2), action),
            AYNI_OK);
        ASSERT_EQ(action->begin_dispatch(stamp(3)), AYNI_OK);
        ASSERT_TRUE(action->claim_dispatch(boot, 3));
        ASSERT_EQ(action->abandon(stamp(4)), AYNI_OK);
    }
    void admit() {
        ASSERT_EQ(ledger.check_admit_from_call(identity, contract, state, action->fact(), stamp(5)),
                  AYNI_OK);
        ledger.admit_checked_call(identity, contract, state, action->fact(), stamp(5));
    }
};
TEST_F(OperationLedgerTest, DisabledConsumesNoArenaAndCapacityNeverEvictsUnknown) {
    EXPECT_EQ(OperationLedger::storage_size({0, 4096}), 0U);
    admit();
    EXPECT_FALSE(ledger.has_room(1));
    EXPECT_EQ(ledger.check_admit_from_call(identity, contract, state, action->fact(), stamp(6)),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(ledger.size(), 1U);
}
TEST_F(OperationLedgerTest,
       RejectsPendingEphemeralUnstoppedAndNonmonotonicHandoffsWithoutMutation) {
    for (const auto bad : {ActionStamp{boot - 1, {6, 6}}, stamp(4), ActionStamp{boot, {4, 6}},
                           ActionStamp{boot, {6, 4}}}) {
        EXPECT_EQ(ledger.check_admit_from_call(identity, contract, state, action->fact(), bad),
                  AYNI_INVALID_STATE);
    }
    state.ephemeral = true;
    EXPECT_EQ(ledger.check_admit_from_call(identity, contract, state, action->fact(), stamp(5)),
              AYNI_INVALID_STATE);
    state.ephemeral = false;
    state.query = CallQuery{1, CallQuery::Phase::requested, stamp(4)};
    EXPECT_EQ(ledger.check_admit_from_call(identity, contract, state, action->fact(), stamp(5)),
              AYNI_INVALID_STATE);
    state.query.reset();
    state.execution.emplace();
    state.execution->phase = AYNI_EXECUTION_DISPATCHING;
    EXPECT_EQ(ledger.check_admit_from_call(identity, contract, state, action->fact(), stamp(5)),
              AYNI_INVALID_STATE);
    EXPECT_EQ(ledger.size(), 0U);
}
TEST_F(OperationLedgerTest, OwnsCompleteProofAndRejectsDuplicateIdsInSnapshot) {
    admit();
    proof.fill(0);
    EXPECT_EQ(ledger.contract(0).business_contract.data[1], 2U);
    const auto row = ledger.record(0);
    EXPECT_EQ(ledger.restore_snapshot_entry(row), AYNI_INVALID_STATE);
    EXPECT_TRUE(ledger.contains_identity(identity.operation_id));
}
TEST_F(OperationLedgerTest, OwnerQueryUsesIndependentAttemptAndRemovesOnlyKnownAuthorityResult) {
    admit();
    EXPECT_EQ(ledger.check_query(0, 2), AYNI_REVISION_CONFLICT);
    ASSERT_EQ(ledger.request_query(0, stamp(6)), AYNI_OK);
    EXPECT_EQ(ledger.check_acknowledge(0, 2), AYNI_INVALID_STATE);
    EXPECT_EQ(ledger.begin_query(0, state.attempt, stamp(7)), AYNI_INVALID_STATE);
    ASSERT_EQ(ledger.begin_query(0, {boot, 2}, stamp(7)), AYNI_OK);
    EXPECT_EQ(ledger.reconcile_and_remove(
                  0, AYNI_QUERY_RESULT_FOUND,
                  {AYNI_CALL_FAILED, AYNI_NOT_READY, AYNI_REASON_NONE, ActionEffect::unknown, {}},
                  stamp(8)),
              AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(ledger.reconcile_and_remove(
                  0, AYNI_QUERY_RESULT_FOUND,
                  {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::complete, {}},
                  stamp(8)),
              AYNI_OK);
    EXPECT_EQ(ledger.size(), 0U);
    EXPECT_FALSE(ledger.has_durable_audit(7));
    PendingAudit audit{};
    ASSERT_TRUE(ledger.take_durable_audit(8, audit));
    EXPECT_FALSE(audit.acknowledged);
    EXPECT_EQ(audit.call_id, identity.call_id);
}
TEST_F(OperationLedgerTest, AcknowledgementHasCASAndDurableAuditBackpressureWithoutExtraRows) {
    contract.query = ayni::catalog::QueryAuthority::none;
    admit();
    EXPECT_EQ(ledger.check_query(0, 1), AYNI_UNSUPPORTED);
    EXPECT_EQ(ledger.check_acknowledge(0, 2), AYNI_REVISION_CONFLICT);
    ASSERT_EQ(ledger.acknowledge_and_remove(0, stamp(6)), AYNI_OK);
    EXPECT_TRUE(ledger.audit_queue_full());
    EXPECT_EQ(ledger.size(), 0U);
    admit();
    EXPECT_EQ(ledger.check_acknowledge(0, 1), AYNI_BUSY);
    PendingAudit audit{};
    EXPECT_FALSE(ledger.take_durable_audit(5, audit));
    ASSERT_TRUE(ledger.take_durable_audit(6, audit));
    EXPECT_TRUE(audit.acknowledged);
    EXPECT_EQ(ledger.check_acknowledge(0, 1), AYNI_OK);
    ledger.clear();
    EXPECT_FALSE(ledger.has_durable_audit(UINT64_MAX));
}
TEST_F(OperationLedgerTest, QueryFailureRetainsOriginalUnknownAndAllowsANewGeneration) {
    admit();
    ASSERT_EQ(ledger.request_query(0, stamp(6)), AYNI_OK);
    ASSERT_EQ(ledger.end_query(0, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_NONE, stamp(7)), AYNI_OK);
    EXPECT_EQ(ledger.state(0).outcome, AYNI_CALL_ABANDONED);
    ASSERT_EQ(ledger.request_query(0, stamp(8)), AYNI_OK);
    EXPECT_EQ(ledger.state(0).revision, 3U);
}
TEST_F(OperationLedgerTest, FullSnapshotAndFixedDeltaRoundtripRejectMalformedFraming) {
    admit();
    auto row = ledger.record(0);
    std::vector<uint8_t> bytes(operation_record_size(row));
    write_operation_record(row, bytes.data());
    OperationRecord decoded{};
    ASSERT_TRUE(read_operation_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.identity.call_id, identity.call_id);
    EXPECT_EQ(decoded.contract.business_contract.size, proof.size());
    EXPECT_FALSE(
        read_operation_record(bytes.data(), static_cast<uint32_t>(bytes.size() - 1), decoded));
    ASSERT_EQ(ledger.request_query(0, stamp(6)), AYNI_OK);
    std::array<uint8_t, operation_state_bytes> delta{};
    write_operation_change(*ledger.journal_change(), delta.data());
    OperationChange change{};
    ASSERT_TRUE(read_operation_change(delta.data(), delta.size(), change));
    EXPECT_EQ(change.kind, OperationChange::Kind::query_requested);
    ledger.clear();
    ASSERT_EQ(ledger.restore_snapshot_entry(row), AYNI_OK);
    ASSERT_EQ(ledger.restore_journal_change(change), AYNI_OK);
    EXPECT_EQ(ledger.state(0).revision, 2U);
    constexpr uint32_t reserved_offset = 40;
    ayni::foundation::store_le32(delta.data() + reserved_offset, 1);
    EXPECT_FALSE(read_operation_change(delta.data(), delta.size(), change));
}
} // namespace

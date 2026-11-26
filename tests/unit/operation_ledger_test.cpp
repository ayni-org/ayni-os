// White-box ADR-0115: independent arena, scalar reconciliation, atomic admission and codecs.
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "text_business_fixture.h"
#include "work/action/action_records.h"
#include "work/operation/operation_records.h"
#include "workspace_fixture.h"
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
    static constexpr OperationLedger::Limits limits{1, 4096};
    std::vector<uint64_t> arena =
        std::vector<uint64_t>(OperationLedger::storage_size(limits) / sizeof(uint64_t) + 1);
    OperationLedger ledger{arena.data(), limits};
    CallIdentity identity{};
    ayni::catalog::CallContract contract{};
    CallState state{};
    std::optional<ActionRecord> action;
    std::array<uint8_t, 4096> proof{};
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
        contract.input_text_limit = 64;
        contract.result_text_limit = 16;
        ayni::test::TextBusiness business(16, 16);
        const std::array<uint8_t, 4> domain{{1, 0, 0, 0}};
        uint32_t size = 0;
        ASSERT_EQ(ayni::catalog::encode_business(
                      ayni::test::codec_workspace(), {domain.data(), domain.size()}, contract,
                      business.definition, proof.data(), static_cast<uint32_t>(proof.size()), size),
                  AYNI_OK);
        contract.business_contract = {proof.data(), size};
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
// ADR-0115: byte-proof strides may be odd without misaligning metadata or aliasing another row.
TEST_F(OperationLedgerTest, OddProofStrideKeepsTwoEntriesAndAuditObligationsIndependent) {
    admit();
    constexpr OperationLedger::Limits odd_limits{2, 4097};
    std::vector<uint64_t> storage(OperationLedger::storage_size(odd_limits) / sizeof(uint64_t) + 1);
    OperationLedger odd(storage.data(), odd_limits);
    auto row = ledger.record(0);
    ASSERT_EQ(odd.restore_snapshot_entry(row), AYNI_OK);
    row.identity.call_id += 10;
    row.identity.operation_id += 10;
    row.action.identity.logical_call_id = row.identity.call_id;
    row.action.identity.operation_id = row.identity.operation_id;
    ASSERT_EQ(odd.restore_snapshot_entry(row), AYNI_OK);
    ASSERT_EQ(odd.acknowledge_and_remove(0, stamp(6)), AYNI_OK);
    EXPECT_EQ(std::memcmp(odd.contract(1).business_contract.data, proof.data(),
                          contract.business_contract.size),
              0);
    ASSERT_EQ(odd.acknowledge_and_remove(1, stamp(7)), AYNI_OK);
    PendingAudit first{}, second{};
    ASSERT_TRUE(odd.take_durable_audit(7, first));
    ASSERT_TRUE(odd.take_durable_audit(7, second));
    EXPECT_EQ(first.call_id, identity.call_id);
    EXPECT_EQ(second.call_id, row.identity.call_id);
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
    const auto original = proof;
    proof.fill(0);
    EXPECT_EQ(std::memcmp(ledger.contract(0).business_contract.data, original.data(),
                          ledger.contract(0).business_contract.size),
              0);
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
    ASSERT_TRUE(read_operation_record(ayni::test::codec_workspace(), bytes.data(),
                                      static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.identity.call_id, identity.call_id);
    EXPECT_EQ(decoded.contract.business_contract.size, contract.business_contract.size);
    EXPECT_FALSE(read_operation_record(ayni::test::codec_workspace(), bytes.data(),
                                       static_cast<uint32_t>(bytes.size() - 1), decoded));
    ASSERT_EQ(ledger.request_query(0, stamp(6)), AYNI_OK);
    std::array<uint8_t, operation_state_bytes> delta{};
    write_operation_change(*ledger.journal_change(), delta.data());
    OperationChange change{};
    ASSERT_TRUE(read_operation_change(delta.data(), delta.size(), change));
    EXPECT_EQ(change.kind, OperationChange::Kind::query_requested);
    ledger.clear();
    ASSERT_EQ(ledger.restore_snapshot_entry(decoded), AYNI_OK);
    ASSERT_EQ(ledger.restore_journal_change(change), AYNI_OK);
    EXPECT_EQ(ledger.state(0).revision, 2U);
    constexpr uint32_t reserved_offset = 40;
    ayni::foundation::store_le32(delta.data() + reserved_offset, 1);
    EXPECT_FALSE(read_operation_change(delta.data(), delta.size(), change));
}
} // namespace
namespace {
// C-CALL-016: snapshot scalar combinations obey the same original Call evidence rules.
TEST_F(OperationLedgerTest, SnapshotRejectsContradictoryOutcomeStopRouteAndProof) {
    admit();
    const auto row = ledger.record(0);
    auto changed = row;
    changed.state.outcome = AYNI_CALL_COMPLETED;
    changed.state.status = AYNI_OK;
    EXPECT_FALSE(OperationLedger::valid_record(changed));
    changed = row;
    changed.contract.effects = ayni::catalog::call_effect_physical;
    EXPECT_FALSE(OperationLedger::valid_record(changed));
    changed = row;
    changed.identity.destination = id("unexpected");
    EXPECT_FALSE(OperationLedger::valid_record(changed));
    changed = row;
    changed.state.created = changed.action.prepared;
    EXPECT_FALSE(OperationLedger::valid_record(changed));
    std::vector<uint8_t> bytes(operation_record_size(row));
    write_operation_record(row, bytes.data());
    // The immutable proof buffer is independently framed and checked against the saved contract.
    OperationRecord decoded{};
    ASSERT_TRUE(read_operation_record(ayni::test::codec_workspace(), bytes.data(),
                                      static_cast<uint32_t>(bytes.size()), decoded));
    const auto offset = static_cast<size_t>(decoded.contract.business_contract.data - bytes.data());
    bytes[offset] = 0;
    EXPECT_FALSE(read_operation_record(ayni::test::codec_workspace(), bytes.data(),
                                       static_cast<uint32_t>(bytes.size()), decoded));
}
// C-CALL-016: an untrusted retained proof must validate its embedded canonical schemas.
TEST_F(OperationLedgerTest, SnapshotRejectsMalformedSchemaInsideOtherwiseValidProof) {
    admit();
    const auto row = ledger.record(0);
    std::vector<uint8_t> bytes(operation_record_size(row));
    write_operation_record(row, bytes.data());
    OperationRecord decoded{};
    ASSERT_TRUE(read_operation_record(ayni::test::codec_workspace(), bytes.data(),
                                      static_cast<uint32_t>(bytes.size()), decoded));
    ayni::foundation::BodyReader reader(decoded.contract.business_contract);
    EXPECT_EQ(reader.u32(), ayni::catalog::business_encoding_version);
    ayni::catalog::CallContract execution{};
    ASSERT_TRUE(ayni::catalog::read_execution_contract(reader, execution));
    ASSERT_EQ(reader.u32(), 0U); // No subscription in this unary fixture.
    const auto input = reader.bytes(ayni::catalog::schema_encoding_bytes_max);
    ASSERT_TRUE(reader.valid());
    constexpr size_t root_subtree_end_offset = 4 * sizeof(uint32_t);
    const auto offset = static_cast<size_t>(input.data - bytes.data()) + root_subtree_end_offset;
    ayni::foundation::store_le32(bytes.data() + offset, 0);
    EXPECT_FALSE(read_operation_record(ayni::test::codec_workspace(), bytes.data(),
                                       static_cast<uint32_t>(bytes.size()), decoded));
}
} // namespace

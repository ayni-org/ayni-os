// C-HVAL-009 / ADR-0104: normal asynchronous cold reads prepare immutable dependencies
// before projecting saved tool results. No I/O failure or recovery is injected.
#include "../../strategies/builtin/history.h"
#include "../../strategies/builtin/history_result.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/field_domain.h"
#include "workspace_fixture.h"
#include <ayni/ayni.hpp>
#include <ayni/model_messages.hpp>
#include <ayni/runtime/common/memory_bodies.h>
#include <gtest/gtest.h>
#include <set>

namespace {
using namespace ayni::strategies::detail;
using ayni::strategies::BodyRef;
constexpr uint32_t result_count = 20, text_limit = 64, proof_capacity = 4096;
constexpr size_t history_capacity = 32768;
struct ColdBodies {
    ayni::runtime::MemoryBodies memory;
    std::set<uint64_t> available, views;
    uint64_t proof = 0;
    uint32_t early_proof_reads = 0, reads = 0;
    static ayni_status_t read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                              uint32_t capacity) {
        auto &self = *static_cast<ColdBodies *>(context);
        ++self.reads;
        if (ref->id == self.proof && !std::includes(self.available.begin(), self.available.end(),
                                                    self.views.begin(), self.views.end())) {
            ++self.early_proof_reads;
        }
        if (self.available.insert(ref->id).second) {
            return AYNI_NOT_READY; // One ordinary asynchronous completion per new reference.
        }
        const auto *store = self.memory.store();
        return store->read(store->context, ref, out, capacity);
    }
};
TEST(HistoryLoading, cold_dependencies_are_prepared_before_saved_results_are_projected) {
    ColdBodies bodies;
    const auto &store = *bodies.memory.store();
    std::array<uint8_t, proof_capacity> domain{}, proof{};
    uint32_t domain_size = 0, proof_size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(nullptr, 0, domain.data(), domain.size(), domain_size),
              AYNI_OK);
    ayni_schema_node_v1 schema{};
    schema.kind = AYNI_VALUE_TEXT;
    schema.subtree_end = 1;
    schema.max_count = text_limit;
    const ayni_business_contract_v1 business{{&schema, 1}, {&schema, 1}, 0, 0, nullptr};
    ayni::catalog::CallContract contract{};
    contract.form = ayni::catalog::CallForm::unary;
    contract.category = ayni::catalog::CallClass::tool;
    contract.idempotency = ayni::catalog::CallIdempotency::idempotent;
    contract.query = ayni::catalog::QueryAuthority::none;
    contract.cancel = ayni::catalog::CancelMode::none;
    contract.timeout_ms = 1000;
    contract.input_format = AYNI_CALL_INPUT_VALUE;
    contract.result_format = AYNI_CALL_RESULT_TEXT;
    contract.input_text_limit = text_limit;
    contract.result_text_limit = text_limit;
    const ayni_bytes_v1 domain_view{domain.data(), domain_size};
    ASSERT_EQ(ayni::catalog::encode_business(ayni::test::codec_workspace(), domain_view, contract,
                                             business, proof.data(), proof.size(), proof_size),
              AYNI_OK);
    Turn turn;
    turn.record.id = 1;
    std::string history(history_capacity, '\0');
    ayni::model::Writer messages(history.data(), history.size());
    messages.prefix();
    for (uint32_t index = 0; index < result_count; ++index) {
        const auto text = std::string("result ") + std::to_string(index);
        auto call = ayni::initialized<ayni_task_call_result_v1>();
        call.logical_call_id = call.operation_id = index + 1U;
        call.outcome = AYNI_CALL_COMPLETED;
        call.result_kind = AYNI_CALL_RESULT_TEXT;
        call.original_domain = domain_view;
        call.business_contract = {proof.data(), proof_size};
        ASSERT_EQ(
            store.stage(store.context, turn.record.id,
                        {reinterpret_cast<const uint8_t *>(text.data()), uint32_t(text.size())},
                        &call.body),
            AYNI_OK);
        BodyRef view{};
        ASSERT_EQ(store_result_view(store, turn.record.id, call, view), AYNI_OK);
        std::string json;
        ASSERT_EQ(load_result_view(store, view, json, nullptr), AYNI_OK);
        const auto id = std::to_string(index + 1U);
        turn.result_views[id] = view;
        bodies.views.insert(view.id);
        messages.tool({id.data(), uint32_t(id.size())}, {json.data(), uint32_t(json.size())});
    }
    BodyRef proof_ref{};
    ASSERT_EQ(store.stage(store.context, turn.record.id, {proof.data(), proof_size}, &proof_ref),
              AYNI_OK);
    bodies.proof = proof_ref.id;
    ayni_string_v1 encoded{};
    ASSERT_TRUE(messages.finish(encoded));
    history.resize(encoded.size);
    turn.history = history;
    ASSERT_EQ(store_history(store, turn), AYNI_OK);
    auto cold = store;
    cold.context = &bodies;
    cold.read = ColdBodies::read;
    std::string restored = "unchanged until complete";
    auto status = AYNI_NOT_READY;
    constexpr uint32_t attempt_limit = result_count * 4;
    for (uint32_t attempt = 0; attempt < attempt_limit && status == AYNI_NOT_READY; ++attempt) {
        status = load_history(cold, turn.record.history, restored);
        if (status == AYNI_NOT_READY) {
            EXPECT_EQ(restored, "unchanged until complete");
        }
    }
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(restored, history);
    EXPECT_EQ(bodies.early_proof_reads, 0U);
    // This fixed normal fixture previously made 1711 reads. Leave headroom above
    // the measured 305 while rejecting repeated prefix reconstruction.
    constexpr uint32_t read_call_budget = 400;
    EXPECT_LE(bodies.reads, read_call_budget);
}
} // namespace

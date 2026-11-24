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
constexpr uint32_t history_capacity = 32768;
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
    ayni::model::Writer messages(history.data(), history_capacity);
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
    // C-POLICY-013: the operation shares verified bytes, including the common proof.
    // The old dependency-preparation path made 305 reads in this same fixture.
    constexpr uint32_t read_call_budget = 120;
    EXPECT_LE(bodies.reads, read_call_budget);
    bodies.reads = 0;
    ASSERT_EQ(load_history(cold, turn.record.history, restored), AYNI_OK);
    EXPECT_EQ(restored, history);
    // One ready pass reads only the distinct root/views/values and shared domain/proof.
    constexpr uint32_t fixed_shared_bodies = 3;
    EXPECT_EQ(bodies.reads, result_count * 2U + fixed_shared_bodies);
}
// C-POLICY-013: synchronous views stay stable, but a later operation checks the store anew.
TEST(HistoryLoading, one_operation_reads_each_identity_once_and_the_next_observes_release) {
    ColdBodies observed;
    auto store = *observed.memory.store();
    store.context = &observed;
    store.read = ColdBodies::read;
    BodyRef first{}, second{};
    const auto *original = observed.memory.store();
    constexpr char content[] = "immutable bytes", other[] = "another body";
    ASSERT_EQ(original->write(original->context, 1,
                              {reinterpret_cast<const uint8_t *>(content), sizeof(content) - 1U},
                              &first),
              AYNI_OK);
    ASSERT_EQ(original->write(original->context, 1,
                              {reinterpret_cast<const uint8_t *>(other), sizeof(other) - 1U},
                              &second),
              AYNI_OK);
    observed.available = {first.id, second.id};
    {
        BodyReader reader(store);
        BodyReader::Body one, again, two;
        ASSERT_EQ(reader.read(first, one), AYNI_OK);
        ASSERT_EQ(reader.read(second, two), AYNI_OK);
        ASSERT_EQ(reader.read(first, again), AYNI_OK);
        EXPECT_EQ(*one, content);
        EXPECT_EQ(*two, other);
        EXPECT_EQ(one.get(), again.get());
        EXPECT_EQ(observed.reads, 2U);
        auto conflicting = first;
        ++conflicting.size;
        EXPECT_EQ(reader.read(conflicting, again), AYNI_STORAGE_FAILED);
    }
    original->release(original->context, &first);
    BodyReader next(store);
    BodyReader::Body absent;
    EXPECT_EQ(next.read(first, absent), AYNI_NOT_FOUND);
}
// C-POLICY-013: independent large results stream through preparation; only the fixed
// small-body cache remains, and digest-correct invalid text still fails its own boundary.
TEST(HistoryLoading, large_bodies_do_not_accumulate_in_the_operation_cache) {
    ColdBodies observed;
    const auto &store = *observed.memory.store();
    constexpr uint32_t large_count = 20, large_bytes = 64U * 1024U;
    std::string bytes(large_bytes, 'x');
    std::vector<BodyRef> refs;
    for (uint32_t index = 0; index < large_count; ++index) {
        bytes.front() = static_cast<char>('A' + index);
        BodyRef ref{};
        ASSERT_EQ(store.write(store.context, 1,
                              {reinterpret_cast<const uint8_t *>(bytes.data()), large_bytes}, &ref),
                  AYNI_OK);
        refs.push_back(ref);
        observed.available.insert(ref.id);
    }
    auto counted = store;
    counted.context = &observed;
    counted.read = ColdBodies::read;
    BodyReader reader(counted);
    ASSERT_EQ(reader.prepare(refs), AYNI_OK);
    EXPECT_EQ(observed.reads, large_count);
    EXPECT_EQ(reader.retained_bytes(), 0U);
    std::string result;
    ASSERT_EQ(reader.copy_text(refs.back(), result), AYNI_OK);
    // C-POLICY-013: exceeding 16 KiB trades a second read of the selected body
    // for releasing all twenty 64 KiB payloads during dependency preparation.
    EXPECT_EQ(observed.reads, large_count + 1U);
    EXPECT_EQ(result, bytes);
    EXPECT_EQ(reader.retained_bytes(), 0U);
    constexpr uint8_t invalid_utf8 = 0xff;
    BodyRef invalid{};
    ASSERT_EQ(store.write(store.context, 1, {&invalid_utf8, 1}, &invalid), AYNI_OK);
    observed.available.insert(invalid.id);
    EXPECT_EQ(reader.copy_text(invalid, result), AYNI_INVALID_ARGUMENT);
    EXPECT_LE(reader.retained_bytes(), BodyReader::cache_bytes);
}
// C-POLICY-013: a long list of tiny bodies also obeys the entry bound; misses stream.
TEST(HistoryLoading, small_body_reuse_stops_at_the_entry_limit_without_rejecting_the_graph) {
    ColdBodies observed;
    const auto *original = observed.memory.store();
    std::vector<BodyRef> refs;
    for (size_t index = 0; index <= BodyReader::cache_entries; ++index) {
        const auto text = std::to_string(index);
        BodyRef ref{};
        ASSERT_EQ(original->write(original->context, 1,
                                  {reinterpret_cast<const uint8_t *>(text.data()),
                                   static_cast<uint32_t>(text.size())},
                                  &ref),
                  AYNI_OK);
        refs.push_back(ref);
        observed.available.insert(ref.id);
    }
    auto port = *original;
    port.context = &observed;
    port.read = ColdBodies::read;
    BodyReader reader(port);
    ASSERT_EQ(reader.prepare(refs), AYNI_OK);
    const auto reads = observed.reads;
    BodyReader::Body first, last;
    ASSERT_EQ(reader.read(refs.front(), first), AYNI_OK);
    EXPECT_EQ(observed.reads, reads);
    ASSERT_EQ(reader.read(refs.back(), last), AYNI_OK);
    EXPECT_EQ(observed.reads, reads + 1U);
    EXPECT_LE(reader.retained_bytes(), BodyReader::cache_bytes);
}
// C-POLICY-013: one immutable model group is decoded once across decisions; each
// decision still obtains the current Core fact before it can reuse private proposals.
TEST(HistoryLoading, prepared_proposals_share_owned_nodes_but_requery_the_current_call) {
    ColdBodies observed;
    const auto *original = observed.memory.store();
    ayni_argument_node_v1 node{};
    node.kind = AYNI_ARGUMENT_STRING;
    node.subtree_end = 1;
    node.value.text = {"argument", 8};
    auto call = ayni::initialized<ayni_tool_call_v1>();
    call.function_name = {"tool", 4};
    call.protocol_id = {"id", 2};
    call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
    call.input.arguments = {&node, 1};
    uint32_t size = 0;
    ASSERT_EQ(ayni_tool_calls_measure(&call, 1, {}, &size), AYNI_OK);
    std::vector<uint8_t> encoded(size);
    ASSERT_EQ(ayni_tool_calls_write(&call, 1, {}, encoded.data(), size, &size), AYNI_OK);
    struct Source {
        ayni_task_call_result_v1 result = ayni::initialized<ayni_task_call_result_v1>();
        mutable uint32_t queries = 0;
    } source;
    auto &fact = source.result;
    fact.logical_call_id = fact.requires_seq = 1;
    fact.outcome = AYNI_CALL_COMPLETED;
    fact.tool_call_count = 1;
    ASSERT_EQ(original->write(original->context, 1, {encoded.data(), size}, &fact.body), AYNI_OK);
    observed.available.insert(fact.body.id);
    auto port = *original;
    port.context = &observed;
    port.read = ColdBodies::read;
    auto snapshot = ayni::initialized<ayni_task_snapshot_v1>();
    snapshot.call_context = &source;
    snapshot.call_result = [](const void *context, ayni_string_v1, ayni_task_call_result_v1 *out) {
        auto &value = *static_cast<const Source *>(context);
        ++value.queries;
        *out = value.result;
        return AYNI_OK;
    };
    ayni::Workspace owner;
    ASSERT_EQ(owner.create(ayni_tool_calls_workspace_requirements), AYNI_OK);
    Workspace workspace(owner.get()->data, owner.get()->byte_capacity);
    PreparedProposals prepared;
    ModelResult first, second;
    ASSERT_EQ(model_result(workspace, port, snapshot, {}, first, ayni::model::ReplyEncoding::text,
                           &prepared),
              AYNI_OK);
    ASSERT_EQ(model_result(workspace, port, snapshot, {}, second, ayni::model::ReplyEncoding::text,
                           &prepared),
              AYNI_OK);
    EXPECT_EQ(source.queries, 2U);
    EXPECT_EQ(observed.reads, 1U);
    EXPECT_EQ(first.proposals.get(), second.proposals.get());
    const auto proposal = second.proposals->calls.front().view();
    const auto text = proposal.input.arguments.nodes[0].value.text;
    EXPECT_EQ(std::string(text.data, text.size), "argument");
    ++fact.requires_seq; // A different admitted fact never reuses the old parse.
    ModelResult changed;
    ASSERT_EQ(model_result(workspace, port, snapshot, {}, changed, ayni::model::ReplyEncoding::text,
                           &prepared),
              AYNI_OK);
    EXPECT_NE(changed.proposals.get(), first.proposals.get());
    EXPECT_EQ(observed.reads, 2U);

    // A decoded checkpoint cursor must also fit this particular immutable list,
    // even when it is below the global eight-proposal bound (C-POLICY-007).
    Turn turn;
    turn.record.id = 1;
    turn.record.owner_instance_id = 2;
    turn.input = "current input";
    turn.phase = TurnPhase::tools;
    turn.tool_index = 2;
    ayni::strategies::Encoded checkpoint;
    ASSERT_EQ(write_turn(turn, *original, checkpoint.bytes.data(), checkpoint.bytes.size(),
                         checkpoint.size),
              AYNI_OK);
    ayni::strategies::ToolLoopConfig config;
    config.identity.id = {"loop", 4};
    config.model = {
        AYNI_POLICY_REQUEST_MODEL, {"model", 5}, 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    config.summary = config.model;
    config.limits = {32768, 1024, 8192, 4096};
    config.bodies = original;
    ayni::Workspace loop_memory;
    ASSERT_EQ(loop_memory.create(ayni::strategies::ToolLoop::workspace_requirements), AYNI_OK);
    ayni::strategies::ToolLoop loop;
    ASSERT_EQ(loop.configure(config, loop_memory.get()), AYNI_OK);
    snapshot.task_id = 1;
    snapshot.owner_instance_id = 2;
    snapshot.goal_parameters = checkpoint.view();
    snapshot.capability_context = [](const void *, const ayni_capability_context_v1 *,
                                     ayni_capability_context_v1 *out) {
        *out = {};
        out->boot_epoch = 1;
        out->owner_instance_id = 2;
        out->goal_type_version = out->goal_type_size = 1;
        out->goal_type_id[0] = 'x';
        return AYNI_OK;
    };
    auto output = ayni::initialized<ayni_policy_output_v1>();
    const auto policy = loop.describe();
    EXPECT_EQ(policy.decide(policy.ctx, &snapshot, &output), AYNI_INVALID_ARGUMENT);
}
// C-POLICY-013: checkpoints keep original round bodies; persisting a new index
// must not resubmit old user/answer text to the body store.
TEST(HistoryLoading, writing_retained_round_metadata_does_not_restage_its_text) {
    ayni::runtime::MemoryBodies memory;
    const auto &store = *memory.store();
    Turn turn;
    turn.record.id = 1;
    turn.record.owner_instance_id = 2;
    turn.record.last_round = 1;
    turn.input = "current input";
    BodyRef user{}, answer{};
    ASSERT_EQ(save_body(store, 1, {"old user", 8}, user), AYNI_OK);
    ASSERT_EQ(save_body(store, 1, {"old answer", 10}, answer), AYNI_OK);
    turn.rounds.push_back({1, user, answer});
    struct Writes {
        const ayni_body_store_v1 *store;
        std::vector<std::string> texts;
    } writes{&store, {}};
    auto port = store;
    port.context = &writes;
    port.write = [](void *context, uint64_t scope, ayni_bytes_v1 bytes, BodyRef *out) {
        auto &value = *static_cast<Writes *>(context);
        value.texts.emplace_back(reinterpret_cast<const char *>(bytes.data), bytes.size);
        return value.store->write(value.store->context, scope, bytes, out);
    };
    ayni::strategies::Encoded checkpoint;
    ASSERT_EQ(
        write_turn(turn, port, checkpoint.bytes.data(), checkpoint.bytes.size(), checkpoint.size),
        AYNI_OK);
    EXPECT_EQ(std::count(writes.texts.begin(), writes.texts.end(), "old user"), 0);
    EXPECT_EQ(std::count(writes.texts.begin(), writes.texts.end(), "old answer"), 0);
    std::vector<ayni::strategies::Round> rounds;
    ASSERT_EQ(ayni::strategies::Conversation::rounds(store, turn.record, rounds), AYNI_OK);
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds.front().user, "old user");
    EXPECT_EQ(rounds.front().answer, "old answer");
    writes.texts.clear();
    ASSERT_EQ(
        write_turn(turn, port, checkpoint.bytes.data(), checkpoint.bytes.size(), checkpoint.size),
        AYNI_OK);
    EXPECT_TRUE(
        writes.texts.empty()); // C-POLICY-013: unchanged owned text needs no new store call.
}
// C-POLICY-013: progress and the published summary share one body; changing text invalidates it.
TEST(HistoryLoading, summary_replacement_reuses_progress_body_and_invalidates_only_changed_text) {
    ayni::runtime::MemoryBodies memory;
    const auto &store = *memory.store();
    struct Writes {
        const ayni_body_store_v1 *store;
        std::map<std::string, uint32_t> texts;
    } writes{&store, {}};
    auto port = store;
    port.context = &writes;
    port.write = [](void *ctx, uint64_t scope, ayni_bytes_v1 bytes, BodyRef *out) {
        auto &s = *static_cast<Writes *>(ctx);
        ++s.texts[std::string(reinterpret_cast<const char *>(bytes.data), bytes.size)];
        return s.store->write(s.store->context, scope, bytes, out);
    };
    Turn turn;
    turn.record.id = 1;
    turn.record.owner_instance_id = 2;
    turn.record.summary_first = turn.record.summary_last = turn.record.last_round = 1;
    turn.input = "input";
    turn.history_summary.set_text({"first summary", 13});
    turn.summary_change = SummaryChange::replacement;
    ayni::strategies::Encoded checkpoint;
    ASSERT_EQ(
        write_turn(turn, port, checkpoint.bytes.data(), checkpoint.bytes.size(), checkpoint.size),
        AYNI_OK);
    const auto first = turn.record.summary;
    EXPECT_EQ(first.id, turn.history_summary.reference.id);
    EXPECT_EQ(writes.texts["first summary"], 1U);
    turn.history_summary.set_text({"second summary", 14});
    EXPECT_EQ(turn.history_summary.reference.id, 0U);
    turn.summary_change = SummaryChange::replacement;
    ASSERT_EQ(
        write_turn(turn, port, checkpoint.bytes.data(), checkpoint.bytes.size(), checkpoint.size),
        AYNI_OK);
    EXPECT_NE(turn.record.summary.id, first.id);
    EXPECT_EQ(turn.record.summary.id, turn.history_summary.reference.id);
    EXPECT_EQ(writes.texts["second summary"], 1U);
    EXPECT_EQ(writes.texts["input"], 1U);
}
// C-POLICY-013: source matching precedes rendering. Index-only collection checks the
// immutable identity without interpreting these test-only opaque proof bytes as a Schema.
TEST(HistoryLoading, result_source_requires_the_same_kind_body_and_original_proofs) {
    ayni::runtime::MemoryBodies memory;
    const auto &store = *memory.store();
    auto call = ayni::initialized<ayni_task_call_result_v1>();
    call.logical_call_id = call.operation_id = call.requires_seq = 1;
    call.outcome = AYNI_CALL_COMPLETED;
    call.effect = AYNI_CALL_EFFECT_COMPLETE;
    call.result_kind = AYNI_CALL_RESULT_TEXT;
    call.text = {"first", 5};
    constexpr uint8_t domain[] = {1}, proof[] = {2}, other[] = {3};
    call.original_domain = {domain, sizeof(domain)};
    call.business_contract = {proof, sizeof(proof)};
    BodyRef reference{};
    ASSERT_EQ(store_result_view(store, 1, call, reference), AYNI_OK);
    const auto check = [&](const ayni_task_call_result_v1 &source) {
        std::string ignored;
        std::vector<BodyRef> refs;
        return load_result_view(store, reference, ignored, &refs, &source);
    };
    EXPECT_EQ(check(call), AYNI_OK);
    auto changed = call;
    changed.text = {"other", 5};
    EXPECT_EQ(check(changed), AYNI_REVISION_CONFLICT);
    changed = call;
    changed.result_kind = AYNI_CALL_RESULT_VALUE;
    EXPECT_EQ(check(changed), AYNI_REVISION_CONFLICT);
    changed = call;
    changed.original_domain = {other, sizeof(other)};
    EXPECT_EQ(check(changed), AYNI_REVISION_CONFLICT);
    changed = call;
    changed.business_contract = {other, sizeof(other)};
    EXPECT_EQ(check(changed), AYNI_REVISION_CONFLICT);
}
// ADR-0113: unshared small tool arguments and continuations (<= 512 B) stay inline in AYNDXI1
// instead of spawning individual FileBodies files, while payloads > 512 B remain external refs.
TEST(HistoryLoading, small_unshared_payloads_inline_into_history_index_and_round_trip) {
    struct CountingStore {
        ayni::runtime::MemoryBodies memory;
        uint32_t writes = 0;
        static ayni_status_t write(void *ctx, uint64_t scope, ayni_bytes_v1 bytes,
                                   ayni_body_ref_v1 *out) {
            auto &self = *static_cast<CountingStore *>(ctx);
            ++self.writes;
            const auto *inner = self.memory.store();
            return inner->write(inner->context, scope, bytes, out);
        }
        static ayni_status_t read(void *ctx, const ayni_body_ref_v1 *ref, uint8_t *out,
                                  uint32_t cap) {
            const auto *inner = static_cast<CountingStore *>(ctx)->memory.store();
            return inner->read(inner->context, ref, out, cap);
        }
        static void release(void *ctx, const ayni_body_ref_v1 *ref) {
            const auto *inner = static_cast<CountingStore *>(ctx)->memory.store();
            inner->release(inner->context, ref);
        }
    } counted;
    ayni_body_store_v1 port{sizeof(port),
                            0,
                            &counted,
                            CountingStore::write,
                            CountingStore::read,
                            CountingStore::release,
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr};

    // Turn with 3 small tool proposals (<= 512 B arguments) + short continuation: only
    // history_index itself is written (1 body write instead of 5).
    std::string history(8192, '\0');
    ayni::model::Writer writer(history.data(), static_cast<uint32_t>(history.size()));
    writer.prefix();
    ayni::model::Function small_calls[3]{
        {{"c1", 2}, {"motor.run", 9}, {"{\"m1\":60,\"m2\":60}", 17}},
        {{"c2", 2}, {"speaker.say", 11}, {"{\"text\":\"hi\"}", 13}},
        {{"c3", 2}, {"motor.stop", 10}, {"{}", 2}},
    };
    writer.calls(3, {"ok", 2}, {{"cont", 4}, {"tok-123", 7}});
    for (const auto &fn : small_calls) {
        writer.function(fn);
    }
    ayni_string_v1 encoded{};
    ASSERT_TRUE(writer.finish(encoded));
    history.resize(encoded.size);

    Turn turn;
    turn.record.id = 7;
    turn.history = history;
    ASSERT_EQ(store_history(port, turn), AYNI_OK);
    EXPECT_EQ(counted.writes, 1U); // Only the AYNDXI1 history_index body itself.

    std::string restored;
    ASSERT_EQ(load_history(port, turn.record.history, restored), AYNI_OK);
    EXPECT_EQ(restored, history);

    // Adding a large argument (> 512 B) externalizes that argument + updates history_index (+2).
    const std::string large_arg(600, 'a');
    std::string second_history(8192, '\0');
    ayni::model::Writer writer2(second_history.data(),
                                static_cast<uint32_t>(second_history.size()));
    writer2.prefix();
    ayni::model::Function large_call{
        {"c4", 2}, {"big.tool", 8}, {large_arg.data(), static_cast<uint32_t>(large_arg.size())}};
    writer2.calls(1);
    writer2.function(large_call);
    ASSERT_TRUE(writer2.finish(encoded));
    second_history.resize(encoded.size);

    counted.writes = 0;
    Turn turn2;
    turn2.record.id = 8;
    turn2.history = second_history;
    ASSERT_EQ(store_history(port, turn2), AYNI_OK);
    EXPECT_EQ(counted.writes, 2U); // Large argument body + history_index.
    ASSERT_EQ(load_history(port, turn2.record.history, restored), AYNI_OK);
    EXPECT_EQ(restored, second_history);
}
} // namespace

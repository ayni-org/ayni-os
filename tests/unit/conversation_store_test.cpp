#include "strategy_workspace_fixture.h"
// ADR-0096: the Native SQLite body store, its collection and privacy replacement.
#include "../../runtime/native_node/conversation_store.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/field_domain.h"
#include "temp_directory.h"
#include <ayni/call_result_json.hpp>
#include <ayni/model_messages.hpp>
#include <chrono>
#include <sqlite3.h>
#include <thread>
namespace {
using ayni::runtime::ConversationStore;
using ayni::runtime::SavedConversation;
using ayni::strategies::BodyRef;
class ConversationDatabase : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(store.open(directory.path().c_str(), state), AYNI_NOT_FOUND);
        state.context_id = 3;
        state.record.id = 1;
        state.record.owner_instance_id = 2;
        commit();
    }
    void TearDown() override { store.close(); }
    void commit(ConversationStore::Erasure erase = ConversationStore::Erasure::none,
                uint64_t turn = 0) {
        ASSERT_EQ(store.save(state, erase, turn), AYNI_OK);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        ayni_status_t result = AYNI_BUSY;
        while (!store.poll(result) && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(result, AYNI_OK);
    }
    template <class Action> ayni_status_t await(Action action) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto status = action();
        while (status == AYNI_NOT_READY && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            status = action();
        }
        return status;
    }
    int64_t count(const char *table) {
        sqlite3 *db = nullptr;
        EXPECT_EQ(sqlite3_open_v2(directory.file("conversation.sqlite3").c_str(), &db,
                                  SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        sqlite3_stmt *query = nullptr;
        const auto text = std::string("SELECT count(*) FROM ") + table;
        EXPECT_EQ(sqlite3_prepare_v2(db, text.c_str(), -1, &query, nullptr), SQLITE_OK);
        EXPECT_EQ(sqlite3_step(query), SQLITE_ROW);
        const auto result = sqlite3_column_int64(query, 0);
        sqlite3_finalize(query);
        sqlite3_close(db);
        return result;
    }
    BodyRef write(const std::string &text, uint64_t scope = 1) {
        BodyRef ref{};
        const auto &bodies = *store.waiting_bodies();
        EXPECT_EQ(bodies.write(bodies.context, scope,
                               {reinterpret_cast<const uint8_t *>(text.data()),
                                static_cast<uint32_t>(text.size())},
                               &ref),
                  AYNI_OK);
        return ref;
    }
    ayni_status_t read(const BodyRef &ref, std::string &out) {
        return ayni::strategies::Conversation::text(*store.bodies(), ref, out);
    }
    ayni::test::TempDirectory directory;
    ConversationStore store;
    SavedConversation state;
};
TEST_F(ConversationDatabase, large_bodies_round_trip_once_and_survive_reopen) {
    const std::string large(200 * 1024, 'h');
    const auto &bodies = *store.bodies();
    BodyRef ref{};
    const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(large.data()),
                              static_cast<uint32_t>(large.size())};
    // Asynchronous writes answer NOT_READY until the worker committed the row.
    EXPECT_EQ(bodies.write(bodies.context, 1, bytes, &ref), AYNI_NOT_READY);
    ASSERT_EQ(await([&] { return bodies.write(bodies.context, 1, bytes, &ref); }), AYNI_OK);
    EXPECT_EQ(write(large).id, ref.id); // Identical bytes in one scope are one body.
    state.record.history = ref;
    commit();
    EXPECT_EQ(count("bodies"), 1);
    store.close();
    SavedConversation reopened;
    ASSERT_EQ(store.open(directory.path().c_str(), reopened), AYNI_OK);
    EXPECT_EQ(reopened.record.history.id, ref.id);
    std::string loaded;
    ASSERT_EQ(read(reopened.record.history, loaded), AYNI_OK);
    EXPECT_EQ(loaded, large);
    ayni::strategies::Encoded encoded;
    ASSERT_EQ(ayni::strategies::Conversation::begin(::ayni::test::conversation_workspace(),
                                                    *store.waiting_bodies(), reopened.record,
                                                    {"next", 4}, encoded),
              AYNI_OK);
    EXPECT_LE(encoded.size, ayni::strategies::encoded_bytes_max); // References only.
}
TEST_F(ConversationDatabase, idle_save_collects_bodies_the_record_no_longer_names) {
    const auto first = write("first history");
    state.record.history = first;
    commit();
    const auto intermediate = write("intermediate");
    state.pending_goal = 9; // A turn in flight keeps every body of the conversation.
    commit();
    EXPECT_EQ(count("bodies"), 2);
    const auto second = write("second history");
    state.record.history = second;
    state.pending_goal = 0;
    commit();
    EXPECT_EQ(count("bodies"), 1);
    std::string loaded;
    EXPECT_EQ(read(first, loaded), AYNI_NOT_FOUND);
    EXPECT_EQ(read(intermediate, loaded), AYNI_NOT_FOUND);
    ASSERT_EQ(read(second, loaded), AYNI_OK);
    EXPECT_EQ(loaded, "second history");
    const auto other = write("another scope", 7); // Other scopes belong to their own owners.
    commit();
    EXPECT_EQ(count("bodies"), 2);
    store.bodies()->release(store.bodies()->context, &other);
    commit();
    EXPECT_EQ(count("bodies"), 1);
}
// A durable reference may still name a deleted body, so its identity is never handed out again.
TEST_F(ConversationDatabase, released_newest_identity_is_not_reused_after_reopen) {
    write("kept", 7);
    const auto newest = write("released", 7);
    commit();
    store.bodies()->release(store.bodies()->context, &newest);
    commit();
    EXPECT_EQ(count("bodies"), 1);
    store.close();
    SavedConversation reopened;
    ASSERT_EQ(store.open(directory.path().c_str(), reopened), AYNI_OK);
    EXPECT_GT(write("after reopen", 7).id, newest.id);
}
TEST_F(ConversationDatabase, erasing_the_conversation_removes_every_body) {
    state.record.summary_first = 1;
    state.record.summary_last = 1;
    state.record.last_round = 1;
    state.record.summary = write("private generated summary");
    state.record.history = write("private generated history");
    commit();
    EXPECT_EQ(count("bodies"), 2);
    ASSERT_EQ(ayni::strategies::Conversation::erase(::ayni::test::conversation_workspace(),
                                                    state.record, state.record),
              AYNI_OK);
    commit(ConversationStore::Erasure::all);
    EXPECT_EQ(count("bodies"), 0);
    EXPECT_EQ(count("body_data"), 0); // Privacy ACK includes physical shared payload deletion.
}
// ADR-0104: SQLite stores one payload for independent scope references, including normal reopen.
TEST_F(ConversationDatabase, staged_aliases_share_one_payload_and_release_independently) {
    const auto *bodies = store.bodies();
    const std::string value = "shared staged value";
    BodyRef call{}, chat{};
    ASSERT_EQ(bodies->stage(bodies->context, 7,
                            {reinterpret_cast<const uint8_t *>(value.data()),
                             static_cast<uint32_t>(value.size())},
                            &call),
              AYNI_OK);
    std::string observed;
    ASSERT_EQ(read(call, observed), AYNI_OK);
    EXPECT_EQ(observed, value);
    EXPECT_EQ(count("body_data"), 0);
    ASSERT_EQ(bodies->share(bodies->context, 8, &call, &chat), AYNI_OK);
    ASSERT_EQ(await([&] { return bodies->confirm(bodies->context, &call); }), AYNI_OK);
    ASSERT_EQ(await([&] { return bodies->confirm(bodies->context, &chat); }), AYNI_OK);
    EXPECT_EQ(count("body_data"), 1);
    bodies->release(bodies->context, &call);
    store.close();
    SavedConversation reopened;
    ASSERT_EQ(store.open(directory.path().c_str(), reopened), AYNI_OK);
    ASSERT_EQ(read(chat, observed), AYNI_OK);
    EXPECT_EQ(observed, value);
    store.bodies()->release(store.bodies()->context, &chat);
    store.close();
    EXPECT_EQ(count("body_data"), 0);
}
// C-SDK-007 / ADR-0105: the one-pass worker stores exactly the original result projection.
TEST_F(ConversationDatabase, archived_text_result_matches_fixed_projection_and_keeps_one_row) {
    constexpr uint32_t proof_capacity = 4096, text_limit = 64, timeout_ms = 1000;
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
    contract.timeout_ms = timeout_ms;
    contract.input_format = AYNI_CALL_INPUT_VALUE;
    contract.result_format = AYNI_CALL_RESULT_TEXT;
    contract.input_text_limit = contract.result_text_limit = text_limit;
    const ayni_bytes_v1 original_domain{domain.data(), domain_size};
    ASSERT_EQ(ayni::catalog::encode_business(::ayni::test::codec_workspace(), original_domain,
                                             contract, business, proof.data(), proof.size(),
                                             proof_size),
              AYNI_OK);
    constexpr uint64_t task = 3, call = 5, operation = 6, sequence = 7;
    ayni_task_call_result_v1 result{};
    result.struct_size = sizeof(result);
    result.logical_call_id = call;
    result.operation_id = operation;
    result.requires_seq = sequence;
    result.outcome = AYNI_CALL_COMPLETED;
    result.result_kind = AYNI_CALL_RESULT_TEXT;
    constexpr char answer[] = u8"完成\n\"exact\"";
    result.text = {answer, sizeof(answer) - 1U};
    result.original_domain = original_domain;
    result.business_contract = {proof.data(), proof_size};
    const auto history = store.history_port();
    ayni::strategies::ArchivedResult archived{};
    ASSERT_EQ(await([&] {
                  return history.archive(history.context, task, state.record, result, archived);
              }),
              AYNI_OK);
    constexpr size_t output_bytes = 2048;
    std::array<char, output_bytes> bytes{};
    ayni::json::Writer writer(bytes.data(), bytes.size());
    ASSERT_EQ(ayni::json::write_call_result(::ayni::test::result_workspace(), writer, result,
                                            archived.reference),
              AYNI_OK);
    size_t size = 0;
    ASSERT_TRUE(writer.finish(size));
    std::string observed;
    ASSERT_EQ(history.result_chunk(history.context, task, archived.reference, 0, observed,
                                   archived.json_bytes),
              AYNI_OK);
    EXPECT_EQ(observed, std::string(bytes.data(), size));
    EXPECT_EQ(archived.json_bytes, size);
    EXPECT_EQ(count("results"), 1);
}
} // namespace

#include "strategy_workspace_fixture.h"
// ADR-0096: the Native SQLite body store, its collection and privacy replacement.
#include "../../runtime/native_node/conversation_store.h"
#include "temp_directory.h"
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
}
} // namespace

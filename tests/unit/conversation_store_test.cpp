#include "strategy_workspace_fixture.h"
// ADR-0085: normal Native SQLite append, paging, references and privacy replacement.
#include "../../runtime/native_node/conversation_store.h"
#include "temp_directory.h"
#include <ayni/model_messages.hpp>
#include <chrono>
#include <sqlite3.h>
#include <thread>
namespace {
using ayni::runtime::ConversationStore;
using ayni::runtime::SavedConversation;
using History = ayni::strategies::Text<ayni::strategies::history_bytes_max>;
class ConversationDatabase : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(store.open(directory.path().c_str(), state), AYNI_NOT_FOUND);
        state.context_id = 3;
        state.record.id = 1;
        state.record.owner_instance_id = 2;
        commit();
        port = store.history_port();
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
    void append(History &history, const std::string &text,
                ayni::model::Role role = ayni::model::Role::user) {
        ayni::model::Writer writer(history.bytes.data() + history.size,
                                   static_cast<uint32_t>(history.bytes.size()) - history.size);
        if (!history.size) {
            writer.prefix();
        }
        writer.text(role, {text.data(), static_cast<uint32_t>(text.size())});
        ayni_string_v1 result{};
        ASSERT_TRUE(writer.finish(result));
        history.size += result.size;
    }
    void save_history(uint64_t turn) {
        uint64_t reference = 0;
        ASSERT_EQ(await([&] { return port.save(port.context, 4, turn, state.record, reference); }),
                  AYNI_OK);
        ASSERT_NE(reference, 0U);
        state.record.history_ref = reference;
    }
    ayni::test::TempDirectory directory;
    ConversationStore store;
    SavedConversation state;
    ayni::strategies::HistoryPort port;
};
TEST_F(ConversationDatabase, appends_messages_once_and_reads_views_across_sql_pages) {
    constexpr int first_page_messages = 80;
    for (int at = 0; at < first_page_messages; ++at) {
        append(state.record.history, "message-" + std::to_string(at));
    }
    save_history(1);
    const auto first = state.record.history_ref;
    append(state.record.history, "new-message");
    save_history(2);
    EXPECT_NE(state.record.history_ref, first);
    EXPECT_EQ(count("messages"), first_page_messages + 1);
    EXPECT_EQ(count("views"), 2);
    History loaded;
    ASSERT_EQ(await([&] { return port.load(port.context, 4, state.record, loaded); }), AYNI_OK);
    EXPECT_EQ(std::string(loaded.bytes.data(), loaded.size),
              std::string(state.record.history.bytes.data(), state.record.history.size));
    ayni::strategies::Encoded encoded;
    ASSERT_EQ(ayni::strategies::Conversation::begin(::ayni::test::conversation_workspace(),
                                                    state.record, {"next", 4}, encoded),
              AYNI_OK);
    EXPECT_LT(encoded.size, 1024U); // The external reference excludes complete message history.
    commit();
}
TEST_F(ConversationDatabase, generation_erasure_rejects_old_cache_reads_and_late_writes) {
    append(state.record.history, "private-generated-body", ayni::model::Role::context);
    save_history(1);
    const auto old = state.record;
    History loaded;
    ASSERT_EQ(await([&] { return port.load(port.context, 4, old, loaded); }), AYNI_OK);
    ASSERT_EQ(ayni::strategies::Conversation::erase(::ayni::test::conversation_workspace(),
                                                    state.record, state.record),
              AYNI_OK);
    commit(ConversationStore::Erasure::all);
    EXPECT_EQ(port.load(port.context, 4, old, loaded), AYNI_EXPIRED);
    uint64_t reference = 0;
    EXPECT_EQ(port.save(port.context, 4, 1, old, reference), AYNI_EXPIRED);
    EXPECT_EQ(count("messages"), 0);
    EXPECT_EQ(count("views"), 0);
}
TEST_F(ConversationDatabase, deleting_one_turn_preserves_other_independent_user_messages) {
    append(state.record.history, "keep-user");
    append(state.record.history, "derived-answer", ayni::model::Role::assistant);
    save_history(1);
    append(state.record.history, "forget-user");
    save_history(2);
    ASSERT_EQ(ayni::strategies::Conversation::erase(::ayni::test::conversation_workspace(),
                                                    state.record, state.record),
              AYNI_OK);
    commit(ConversationStore::Erasure::turn, 2);
    EXPECT_EQ(count("messages"), 1);
    EXPECT_EQ(count("views"), 0);
}
} // namespace

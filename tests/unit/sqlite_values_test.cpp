#include "strategy_workspace_fixture.h"
// ADR-0108: SQLite implements the same value/body ports as other Session backends.
#include "../../runtime/native_node/sqlite_values.h"
#include "temp_directory.h"
#include <atomic>
#include <ayni/ayni.hpp>
#include <ayni/runtime/conversation/file_bodies.h>
#include <chrono>
#include <sqlite3.h>
#include <thread>
namespace {
using ayni::runtime::FileBodies;
using ayni::runtime::SqliteValues;
using ayni::strategies::BodyRef;
using namespace ayni::hosts::storage;
class SqliteValuesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(store.open(directory.path().c_str()), AYNI_OK);
        configure_bodies();
        record.id = 3;
        record.owner_instance_id = 2;
        commit();
    }
    void TearDown() override {
        drain();
        store.close();
    }
    void configure_bodies() {
        bodies = std::make_unique<FileBodies>();
        ASSERT_EQ(bodies->configure(store.files(), 16U * 1024U * 1024U), AYNI_OK);
        bodies->set_shared_call_results();
    }
    void reopen() {
        drain();
        store.close();
        ASSERT_EQ(store.open(directory.path().c_str()), AYNI_OK);
        configure_bodies();
    }
    template <class Action> ayni_status_t await(Action action) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto status = action();
        while ((status == AYNI_NOT_READY || status == AYNI_BUSY) &&
               std::chrono::steady_clock::now() < end) {
            if (bodies) {
                FileBodies::ready(bodies.get(), 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            status = action();
        }
        return status;
    }
    FileValueResult value(FileValueRequest request) {
        struct Completion {
            FileValueResult result;
            std::atomic<bool> done{false};
        } completion;
        request.key = {record.owner_instance_id, record.id};
        request.context = &completion;
        request.complete = [](void *context, const FileValueResult &result) noexcept {
            auto &self = *static_cast<Completion *>(context);
            self.result = result;
            self.done.store(true, std::memory_order_release);
        };
        const auto port = store.files();
        EXPECT_EQ(await([&] {
                      return port.submit(port.context, &request) == AYNI_SUBMIT_ACCEPTED
                                 ? AYNI_OK
                                 : AYNI_BUSY;
                  }),
                  AYNI_OK);
        EXPECT_EQ(await([&] {
                      return completion.done.load(std::memory_order_acquire) ? AYNI_OK
                                                                             : AYNI_NOT_READY;
                  }),
                  AYNI_OK);
        return completion.result;
    }
    void commit() {
        ayni::strategies::Encoded encoded;
        ASSERT_EQ(ayni::strategies::Conversation::encode(ayni::test::conversation_workspace(),
                                                         record, encoded),
                  AYNI_OK);
        FileValueRequest request;
        request.kind = FileValueRequest::Kind::write;
        request.previous_generation = version;
        request.generation = version + 1U;
        request.data = encoded.bytes.data();
        request.size = encoded.size;
        const auto result = value(request);
        ASSERT_EQ(result.status, AYNI_OK);
        version = result.generation;
    }
    void drain() {
        auto port = bodies->collection();
        ASSERT_EQ(await([&] {
                      return port.ready(port.context, 0) ? port.status(port.context)
                                                         : AYNI_NOT_READY;
                  }),
                  AYNI_OK);
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
    BodyRef write(const std::string &text, uint64_t scope = 3) {
        BodyRef ref{};
        const auto *port = bodies->store();
        EXPECT_EQ(await([&] {
                      return port->write(
                          port->context, scope,
                          {reinterpret_cast<const uint8_t *>(text.data()), uint32_t(text.size())},
                          &ref);
                  }),
                  AYNI_OK);
        return ref;
    }
    ayni_status_t read(const BodyRef &ref, std::string &out) {
        return await(
            [&] { return ayni::strategies::Conversation::text(*bodies->store(), ref, out); });
    }
    ayni::test::TempDirectory directory;
    SqliteValues store;
    std::unique_ptr<FileBodies> bodies;
    ayni::strategies::ConversationRecord record;
    uint64_t version = 0;
};
// C-SDK-009: the common value CAS preserves an exact root across normal close/reopen.
TEST_F(SqliteValuesTest, large_bodies_and_root_survive_normal_reopen) {
    const std::string large(200U * 1024U, 'h');
    const auto ref = write(large);
    EXPECT_EQ(write(large).id, ref.id);
    record.history = ref;
    commit();
    reopen();
    ayni::strategies::Encoded encoded;
    FileValueRequest request;
    request.buffer = encoded.bytes.data();
    request.size = encoded.bytes.size();
    const auto result = value(request);
    ASSERT_EQ(result.status, AYNI_OK);
    EXPECT_EQ(result.generation, version);
    ayni::strategies::ConversationRecord reopened;
    ASSERT_EQ(ayni::strategies::Conversation::decode(ayni::test::conversation_workspace(),
                                                     {encoded.bytes.data(), result.size}, reopened),
              AYNI_OK);
    EXPECT_EQ(reopened.history.id, ref.id);
    std::string loaded;
    ASSERT_EQ(read(ref, loaded), AYNI_OK);
    EXPECT_EQ(loaded, large);
}
// C-SDK-009/011: Session, rather than a second SQLite controller, selects the retained graph.
TEST_F(SqliteValuesTest, scope_collection_deletes_only_unretained_bodies_and_shared_payloads) {
    const auto first = write("first");
    const auto second = write("second");
    const auto other = write("other scope", 7);
    auto collection = bodies->collection();
    collection.collect(collection.context, 3, {second});
    drain();
    std::string loaded;
    EXPECT_EQ(read(first, loaded), AYNI_NOT_FOUND);
    EXPECT_EQ(read(second, loaded), AYNI_OK);
    EXPECT_EQ(read(other, loaded), AYNI_OK);
    collection.collect(collection.context, 3, {});
    drain();
    EXPECT_EQ(read(other, loaded), AYNI_OK);
}
// C-HVAL-009: aliases own separate rows but share one immutable payload, including reopen.
TEST_F(SqliteValuesTest, staged_aliases_share_one_payload_and_release_independently) {
    const auto *port = bodies->store();
    BodyRef source{}, alias{};
    constexpr uint8_t text[] = "shared body";
    ASSERT_EQ(port->stage(port->context, 7, {text, sizeof(text) - 1U}, &source), AYNI_OK);
    ASSERT_EQ(port->share(port->context, 8, &source, &alias), AYNI_OK);
    ASSERT_EQ(await([&] { return port->confirm(port->context, &source); }), AYNI_OK);
    ASSERT_EQ(await([&] { return port->confirm(port->context, &alias); }), AYNI_OK);
    port->release(port->context, &source);
    reopen();
    std::string loaded;
    ASSERT_EQ(read(alias, loaded), AYNI_OK);
    EXPECT_EQ(loaded, "shared body");
    bodies->store()->release(bodies->store()->context, &alias);
    drain();
    EXPECT_EQ(count("state"), 1);
}
// C-HVAL-009: confirmed removal remains observable after ordinary reopen.
TEST_F(SqliteValuesTest, released_reference_is_absent_after_normal_reopen) {
    write("kept", 7);
    const auto last = write("released", 7);
    bodies->store()->release(bodies->store()->context, &last);
    reopen();
    std::string text;
    EXPECT_EQ(read(last, text), AYNI_NOT_FOUND);
    EXPECT_EQ(read(write("new", 7), text), AYNI_OK);
}
// C-SDK-009: a stale writer cannot replace a committed root; reads observe the retained version.
TEST_F(SqliteValuesTest, stale_value_cas_is_refused_and_the_body_cache_has_a_finite_budget) {
    constexpr uint8_t replacement = 0;
    FileValueRequest request;
    request.kind = FileValueRequest::Kind::write;
    request.generation = version + 1U;
    request.data = &replacement;
    request.size = 1;
    EXPECT_EQ(value(request).status, AYNI_STALE);
    const std::string too_large(17U * 1024U * 1024U, 'x');
    BodyRef ref{};
    const auto *port = bodies->store();
    EXPECT_EQ(port->stage(
                  port->context, 3,
                  {reinterpret_cast<const uint8_t *>(too_large.data()), uint32_t(too_large.size())},
                  &ref),
              AYNI_CAPACITY_EXCEEDED);
}
} // namespace

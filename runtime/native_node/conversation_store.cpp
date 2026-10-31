#include "conversation_store.h"
#include <ayni/call_result_json.hpp>
#include <ayni/model_messages.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sqlite3.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
namespace ayni::runtime {
namespace {
constexpr char filename[] = "conversation.sqlite3";
constexpr int application_id = 0x41594E49;
constexpr int database_version = 2; // Conversation format 4 includes Task-scoped sample provenance.
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement statement(sqlite3 *db, const char *sql) {
    sqlite3_stmt *out = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &out, nullptr) != SQLITE_OK) {
        return {nullptr, sqlite3_finalize};
    }
    return {out, sqlite3_finalize};
}
bool sql(sqlite3 *db, const char *text) {
    return sqlite3_exec(db, text, nullptr, nullptr, nullptr) == SQLITE_OK;
}
int pragma(sqlite3 *db, const char *text) {
    auto query = statement(db, text);
    return query && sqlite3_step(query.get()) == SQLITE_ROW ? sqlite3_column_int(query.get(), 0)
                                                            : -1;
}
bool private_regular(int fd) {
    struct stat st {};
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & 077) == 0 && st.st_nlink == 1;
}
} // namespace
ConversationStore::~ConversationStore() {
    close();
}
// Reject legacy state before creating the new database; no implicit migration.
ayni_status_t ConversationStore::open(const char *directory, SavedConversation &state) {
    if (directory_ >= 0) {
        return AYNI_INVALID_STATE;
    }
    if (!owner_workspace_.get()) {
        auto status = owner_workspace_.create(strategies::Conversation::workspace_requirements);
        if (status == AYNI_OK) {
            status = worker_workspace_.create(strategies::Conversation::workspace_requirements);
        }
        if (status == AYNI_OK) {
            status = result_workspace_.create(json::call_result_workspace_requirements);
        }
        if (status != AYNI_OK) {
            owner_workspace_.close();
            worker_workspace_.close();
            result_workspace_.close();
            return status;
        }
    }
    directory_ = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_ < 0) {
        return AYNI_STORAGE_FAILED;
    }
    struct stat legacy {};
    if (fstatat(directory_, "conversation.bin", &legacy, AT_SYMLINK_NOFOLLOW) == 0) {
        close();
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (errno != ENOENT) {
        close();
        return AYNI_STORAGE_FAILED;
    }
    bool created = false;
    int fd = openat(directory_, filename, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        fd = openat(directory_, filename, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        created = fd >= 0;
    }
    if (fd < 0 || !private_regular(fd)) {
        if (fd >= 0) {
            ::close(fd);
        }
        close();
        return AYNI_STORAGE_FAILED;
    }
    ::close(fd);
    std::error_code path_error;
    const auto path = std::filesystem::canonical(directory, path_error) / filename;
    if (path_error) {
        close();
        return AYNI_STORAGE_FAILED;
    }
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
                        nullptr) != SQLITE_OK) {
        close();
        return AYNI_STORAGE_FAILED;
    }
    if (!created && (pragma(database_, "PRAGMA application_id") != application_id ||
                     pragma(database_, "PRAGMA user_version") != database_version)) {
        close();
        return AYNI_UNSUPPORTED_VERSION;
    }
    const char *schema =
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
        "PRAGMA secure_delete=ON; PRAGMA trusted_schema=OFF;"
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS state(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "context INTEGER NOT NULL,pending INTEGER NOT NULL,body BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation "
        "INTEGER NOT NULL,"
        "owner INTEGER NOT NULL,generation INTEGER NOT NULL,turn INTEGER NOT NULL,role INTEGER NOT "
        "NULL,body BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS views(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation "
        "INTEGER NOT NULL,"
        "owner INTEGER NOT NULL,generation INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS view_messages(view_id INTEGER NOT NULL REFERENCES views(id) ON "
        "DELETE CASCADE,"
        "ordinal INTEGER NOT NULL,message_id INTEGER NOT NULL REFERENCES messages(id),"
        "PRIMARY KEY(view_id,ordinal));"
        "CREATE TABLE IF NOT EXISTS results(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation "
        "INTEGER NOT NULL,"
        "owner INTEGER NOT NULL,generation INTEGER NOT NULL,task INTEGER NOT NULL,call INTEGER NOT "
        "NULL,"
        "sequence INTEGER NOT NULL,operation INTEGER NOT NULL,outcome INTEGER NOT NULL,status "
        "INTEGER NOT NULL,"
        "reason INTEGER NOT NULL,effect INTEGER NOT NULL,text BLOB NOT NULL,value BLOB NOT NULL,"
        "domain BLOB NOT NULL,proof BLOB NOT NULL,json BLOB NOT NULL,usage_kind INTEGER NOT NULL,"
        "input_tokens INTEGER NOT NULL,output_tokens INTEGER NOT NULL,total_tokens INTEGER NOT "
        "NULL,"
        "UNIQUE(conversation,owner,generation,call,sequence));"
        "PRAGMA application_id=1096371785; PRAGMA user_version=2; COMMIT;";
    // The application id is independent from Core journal/schema and message-frame versions.
    if (!sql(database_, schema)) {
        close();
        return AYNI_STORAGE_FAILED;
    }
    const auto result = read(state);
    if (result != AYNI_OK && result != AYNI_NOT_FOUND) {
        close();
        return result;
    }
    if (result == AYNI_OK) {
        conversation_ = state.record.id;
        owner_ = state.record.owner_instance_id;
        generation_ = state.record.generation;
    }
    try {
        worker_ = std::thread([this] { run(); });
    } catch (const std::system_error &) {
        close();
        return AYNI_CAPACITY_EXCEEDED;
    }
    return result;
}
ayni_status_t ConversationStore::read(SavedConversation &state) {
    auto query = statement(database_, "SELECT context,pending,body FROM state WHERE singleton=1");
    if (!query) {
        return AYNI_STORAGE_FAILED;
    }
    const auto result = sqlite3_step(query.get());
    if (result == SQLITE_DONE) {
        return AYNI_NOT_FOUND;
    }
    if (result != SQLITE_ROW) {
        return AYNI_STORAGE_FAILED;
    }
    SavedConversation candidate;
    candidate.context_id = static_cast<uint64_t>(sqlite3_column_int64(query.get(), 0));
    candidate.pending_goal = static_cast<uint64_t>(sqlite3_column_int64(query.get(), 1));
    const auto bytes = sqlite3_column_bytes(query.get(), 2);
    if (!candidate.context_id || bytes < 0 || static_cast<uint32_t>(bytes) > file_size_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = strategies::Conversation::decode(
        worker_workspace_.get(),
        {static_cast<const uint8_t *>(sqlite3_column_blob(query.get(), 2)),
         static_cast<uint32_t>(bytes)},
        candidate.record);
    if (status == AYNI_OK) {
        state = candidate;
    }
    return status;
}
ayni_status_t ConversationStore::save(const SavedConversation &state, Erasure erasure,
                                      uint64_t turn) {
    if (!state.context_id) {
        return AYNI_INVALID_ARGUMENT;
    }
    strategies::Encoded encoded;
    const auto status =
        strategies::Conversation::encode(owner_workspace_.get(), state.record, encoded);
    if (status != AYNI_OK) {
        return status;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_) {
        return AYNI_INVALID_STATE;
    }
    if (phase_ != Phase::idle) {
        return AYNI_BUSY;
    }
    bytes_.fill(0);
    std::memcpy(bytes_.data(), encoded.bytes.data(), encoded.size);
    size_ = encoded.size;
    pending_ = state;
    erasure_ = erasure;
    erased_turn_ = turn;
    phase_ = Phase::queued;
    wake_.notify_one();
    return AYNI_OK;
}
// The privacy coordinator publishes its erasure ACK only after this transaction.
// Generation replacement removes every obsolete raw message and immutable view.
ayni_status_t ConversationStore::write() {
    if (!sql(database_, "BEGIN IMMEDIATE")) {
        return AYNI_STORAGE_FAILED;
    }
    const auto finish = [&](bool okay) {
        if (okay && sql(database_, "COMMIT")) {
            return AYNI_OK;
        }
        sql(database_, "ROLLBACK");
        return AYNI_STORAGE_FAILED;
    };
    for (const char *text :
         {"DELETE FROM views WHERE conversation=? AND owner=? AND generation<>?",
          "DELETE FROM results WHERE conversation=? AND owner=? AND generation<>?"}) {
        auto erase = statement(database_, text);
        if (!erase) {
            return finish(false);
        }
        sqlite3_bind_int64(erase.get(), 1, static_cast<sqlite3_int64>(pending_.record.id));
        sqlite3_bind_int64(erase.get(), 2,
                           static_cast<sqlite3_int64>(pending_.record.owner_instance_id));
        sqlite3_bind_int64(erase.get(), 3, static_cast<sqlite3_int64>(pending_.record.generation));
        if (sqlite3_step(erase.get()) != SQLITE_DONE) {
            return finish(false);
        }
    }
    // Independent user statements retain their original turn identity. Erasing
    // one source discards all derived content while preserving other user turns.
    const bool keep_users = erasure_ == Erasure::derived || erasure_ == Erasure::turn;
    const auto bind_scope = [&](sqlite3_stmt *command) {
        sqlite3_bind_int64(command, 1, static_cast<sqlite3_int64>(pending_.record.id));
        sqlite3_bind_int64(command, 2,
                           static_cast<sqlite3_int64>(pending_.record.owner_instance_id));
        sqlite3_bind_int64(command, 3, static_cast<sqlite3_int64>(pending_.record.generation));
    };
    auto erase = statement(
        database_, keep_users
                       ? "DELETE FROM messages WHERE conversation=? AND owner=? AND generation<>? "
                         "AND (role<>? OR turn=?)"
                       : "DELETE FROM messages WHERE conversation=? AND owner=? AND generation<>?");
    if (!erase) {
        return finish(false);
    }
    bind_scope(erase.get());
    if (keep_users) {
        sqlite3_bind_int(erase.get(), 4, static_cast<int>(model::Role::user));
        sqlite3_bind_int64(
            erase.get(), 5,
            static_cast<sqlite3_int64>(erasure_ == Erasure::turn ? erased_turn_ : 0));
    }
    if (sqlite3_step(erase.get()) != SQLITE_DONE) {
        return finish(false);
    }
    if (keep_users) {
        auto retain = statement(
            database_, "UPDATE messages SET generation=? WHERE conversation=? AND owner=?");
        if (!retain) {
            return finish(false);
        }
        sqlite3_bind_int64(retain.get(), 1, static_cast<sqlite3_int64>(pending_.record.generation));
        sqlite3_bind_int64(retain.get(), 2, static_cast<sqlite3_int64>(pending_.record.id));
        sqlite3_bind_int64(retain.get(), 3,
                           static_cast<sqlite3_int64>(pending_.record.owner_instance_id));
        if (sqlite3_step(retain.get()) != SQLITE_DONE) {
            return finish(false);
        }
    }
    auto write = statement(database_,
                           "INSERT INTO state VALUES(1,?,?,?) ON CONFLICT(singleton) DO UPDATE SET "
                           "context=excluded.context,pending=excluded.pending,body=excluded.body");
    if (!write) {
        return finish(false);
    }
    sqlite3_bind_int64(write.get(), 1, static_cast<sqlite3_int64>(pending_.context_id));
    sqlite3_bind_int64(write.get(), 2, static_cast<sqlite3_int64>(pending_.pending_goal));
    sqlite3_bind_blob(write.get(), 3, bytes_.data(), static_cast<int>(size_), SQLITE_STATIC);
    return finish(sqlite3_step(write.get()) == SQLITE_DONE);
}
void ConversationStore::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] {
            return phase_ == Phase::queued || phase_ == Phase::stopping ||
                   loading_.phase == Phase::queued || storing_.phase == Phase::queued ||
                   archiving_.phase == Phase::queued;
        });
        if (phase_ == Phase::stopping) {
            return;
        }
        HistoryJob *job = loading_.phase == Phase::queued   ? &loading_
                          : storing_.phase == Phase::queued ? &storing_
                                                            : nullptr;
        const bool archive = !job && archiving_.phase == Phase::queued;
        if (archive) {
            archiving_.phase = Phase::writing;
        } else if (job) {
            job->phase = Phase::writing;
        } else {
            phase_ = Phase::writing;
        }
        lock.unlock();
        ayni_status_t result = AYNI_INTERNAL;
        try {
            result = job == &loading_   ? read_history(*job)
                     : job == &storing_ ? write_history(*job)
                     : archive          ? write_result()
                                        : write();
        } catch (const std::bad_alloc &) {
            sql(database_, "ROLLBACK");
            result = AYNI_CAPACITY_EXCEEDED;
        }
        lock.lock();
        if (archive) {
            archiving_.status = result;
            archiving_.phase = Phase::done;
        } else if (job) {
            job->status = result;
            job->phase = Phase::done;
        } else {
            status_ = result;
            phase_ = Phase::done;
            if (result == AYNI_OK) {
                conversation_ = pending_.record.id;
                owner_ = pending_.record.owner_instance_id;
                generation_ = pending_.record.generation;
                // The ACK covers managed copies and jobs queued during erasure.
                for (auto *cached : {&loading_, &storing_}) {
                    if (cached->record.id != conversation_ ||
                        cached->record.owner_instance_id != owner_ ||
                        cached->record.generation != generation_) {
                        *cached = {};
                    }
                }
                if (archiving_.conversation != conversation_ || archiving_.owner != owner_ ||
                    archiving_.generation != generation_) {
                    archiving_ = {};
                }
            }
        }
        wake_.notify_all();
    }
}
bool ConversationStore::poll(ayni_status_t &status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != Phase::done) {
        return false;
    }
    status = status_;
    bytes_.fill(0);
    size_ = 0;
    pending_ = {};
    phase_ = Phase::idle;
    return true;
}
void ConversationStore::close() {
    if (worker_.joinable()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] {
                return phase_ != Phase::queued && phase_ != Phase::writing &&
                       loading_.phase != Phase::queued && loading_.phase != Phase::writing &&
                       storing_.phase != Phase::queued && storing_.phase != Phase::writing &&
                       archiving_.phase != Phase::queued && archiving_.phase != Phase::writing;
            });
            phase_ = Phase::stopping;
        }
        wake_.notify_one();
        worker_.join();
    }
    if (database_) {
        sqlite3_close_v2(database_);
    }
    database_ = nullptr;
    if (directory_ >= 0) {
        ::close(directory_);
    }
    directory_ = -1;
    bytes_.fill(0);
    size_ = 0;
    pending_ = {};
    loading_ = {};
    storing_ = {};
    archiving_ = {};
    phase_ = Phase::idle;
}
} // namespace ayni::runtime

#include "sqlite_values.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
namespace ayni::runtime {
namespace {
constexpr char filename[] = "conversation.sqlite3";
constexpr int application_id = 0x41594E49;
constexpr int synchronous_extra = 3; // SQLite EXTRA synchronizes DELETE journal directory removal.
constexpr int database_version = 5;  // Shared value CAS/body ports only (ADR-0108).
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
SqliteValues::~SqliteValues() {
    close();
}
// Refuse unsupported storage before Core starts; no reader, migration or automatic erase.
ayni_status_t SqliteValues::open(const char *directory) {
    if (directory_ >= 0) {
        return AYNI_INVALID_STATE;
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
    const auto schema =
        std::string(
            "PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; PRAGMA foreign_keys=ON;"
            "PRAGMA secure_delete=ON; PRAGMA trusted_schema=OFF; BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS state(owner INTEGER NOT NULL,object INTEGER NOT NULL,"
            "generation INTEGER NOT NULL,body BLOB NOT NULL,PRIMARY KEY(owner,object));"
            "PRAGMA application_id=") +
        std::to_string(application_id) +
        "; PRAGMA user_version=" + std::to_string(database_version) + "; COMMIT;";
    if (!sql(database_, schema.c_str())) {
        close();
        return AYNI_STORAGE_FAILED;
    }
    auto journal = statement(database_, "PRAGMA journal_mode");
    if (!journal || sqlite3_step(journal.get()) != SQLITE_ROW ||
        std::strcmp(reinterpret_cast<const char *>(sqlite3_column_text(journal.get(), 0)),
                    "delete") ||
        pragma(database_, "PRAGMA synchronous") != synchronous_extra ||
        pragma(database_, "PRAGMA secure_delete") != 1) {
        journal.reset();
        close();
        return AYNI_STORAGE_FAILED;
    }
    journal.reset();
    try {
        phase_ = State::idle;
        worker_ = std::thread([this] { run(); });
    } catch (const std::system_error &) {
        close();
        return AYNI_CAPACITY_EXCEEDED;
    }
    return AYNI_OK;
}
// One worker borrows value bytes; the bounded queue holds metadata only. Callers keep buffers alive
// through completion; SQLite's EXTRA commit is the same FileValuePort durability boundary.
ayni_submit_result_t SqliteValues::submit(void *context,
                                          const hosts::storage::FileValueRequest *request) {
    if (!context || !request || !hosts::storage::valid_file_value_request(*request)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    auto &self = *static_cast<SqliteValues *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (!self.database_ || self.phase_ == State::closed || self.phase_ == State::stopping ||
        self.queued_ == queue_capacity) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.pending_[self.queued_++] = *request;
    self.wake_.notify_one();
    return AYNI_SUBMIT_ACCEPTED;
}
hosts::storage::FileValueResult
SqliteValues::value(const hosts::storage::FileValueRequest &request) {
    using Kind = hosts::storage::FileValueRequest::Kind;
    hosts::storage::FileValueResult out;
    auto query =
        statement(database_, "SELECT generation,body FROM state WHERE owner=? AND object=?");
    if (!query) {
        return out;
    }
    const auto bind_key = [&](sqlite3_stmt *q) {
        sqlite3_bind_int64(q, 1, static_cast<sqlite3_int64>(request.key.owner_instance_id));
        sqlite3_bind_int64(q, 2, static_cast<sqlite3_int64>(request.key.object_id));
    };
    bind_key(query.get());
    const auto found = sqlite3_step(query.get());
    if (found != SQLITE_ROW && found != SQLITE_DONE) {
        return out;
    }
    const bool exists = found == SQLITE_ROW;
    const auto generation =
        exists ? static_cast<uint64_t>(sqlite3_column_int64(query.get(), 0)) : 0;
    const auto size = exists ? sqlite3_column_bytes(query.get(), 1) : 0;
    if (size < 0 || (exists && !generation)) {
        out.status = AYNI_INVALID_ARGUMENT;
        return out;
    }
    out.generation = generation;
    out.size = static_cast<uint32_t>(size);
    if (request.kind == Kind::read) {
        out.status = !exists                   ? AYNI_NOT_FOUND
                     : out.size > request.size ? AYNI_BUFFER_TOO_SMALL
                                               : AYNI_OK;
        if (out.status == AYNI_OK && size) {
            std::memcpy(request.buffer, sqlite3_column_blob(query.get(), 1), out.size);
        }
        return out;
    }
    // Exact retry reconfirms the same version/bytes; it cannot overwrite another generation.
    const bool same =
        exists && generation == request.generation && out.size == request.size &&
        (!size || std::memcmp(sqlite3_column_blob(query.get(), 1), request.data, out.size) == 0);
    if (request.kind == Kind::write && !same && generation != request.previous_generation) {
        out.status = AYNI_STALE;
        return out;
    }
    query.reset(); // Finish the read before beginning the writer transaction.
    if (!sql(database_, "BEGIN IMMEDIATE")) {
        return out;
    }
    auto write = statement(database_,
                           request.kind == Kind::remove
                               ? "DELETE FROM state WHERE owner=? AND object=?"
                               : "INSERT INTO state(owner,object,generation,body) VALUES(?,?,?,?) "
                                 "ON CONFLICT(owner,object) "
                                 "DO UPDATE SET generation=excluded.generation,body=excluded.body");
    bool okay = static_cast<bool>(write);
    if (okay) {
        bind_key(write.get());
        if (request.kind == Kind::write) {
            sqlite3_bind_int64(write.get(), 3, static_cast<sqlite3_int64>(request.generation));
            sqlite3_bind_blob(write.get(), 4,
                              request.size ? request.data : static_cast<const void *>(""),
                              static_cast<int>(request.size), SQLITE_STATIC);
        }
        okay = sqlite3_step(write.get()) == SQLITE_DONE;
    }
    // A failed COMMIT may have reached durable media. Never let a later rollback
    // attempt turn that uncertainty into permission to ACK or replay (C-HVAL-002).
    out.status = !okay                      ? AYNI_STORAGE_FAILED
                 : sql(database_, "COMMIT") ? AYNI_OK
                                            : AYNI_RESULT_UNKNOWN;
    if (out.status != AYNI_OK) {
        sql(database_, "ROLLBACK");
    } else {
        out.generation = request.generation;
        out.size = request.size;
    }
    return out;
}
void SqliteValues::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return queued_ != 0U || phase_ == State::stopping; });
        if (phase_ == State::stopping) {
            return;
        }
        phase_ = State::working;
        size_t selected = 0;
        for (size_t index = 0; index < queued_; ++index) {
            if (pending_[index].priority == hosts::storage::FileValueRequest::Priority::normal) {
                selected = index;
                break;
            }
        }
        const auto request = pending_[selected];
        for (size_t index = selected + 1U; index < queued_; ++index) {
            pending_[index - 1U] = pending_[index];
        }
        pending_[--queued_] = {};
        lock.unlock();
        const auto result = value(request);
        request.complete(request.context, result);
        lock.lock();
        phase_ = State::idle;
        wake_.notify_all();
    }
}
void SqliteValues::close() {
    if (worker_.joinable()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return phase_ == State::idle && queued_ == 0U; });
            phase_ = State::stopping;
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
    pending_ = {};
    queued_ = 0;
    phase_ = State::closed;
}
} // namespace ayni::runtime

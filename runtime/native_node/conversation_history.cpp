// Native body store (ADR-0096): one SQLite row per immutable body, cached in memory.
#include "conversation_store.h"
#include <algorithm>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <vector>
namespace ayni::runtime {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *out = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &out, nullptr) != SQLITE_OK) {
        return {nullptr, sqlite3_finalize};
    }
    return {out, sqlite3_finalize};
}
void integer(sqlite3_stmt *statement, int index, uint64_t value) {
    sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value));
}
} // namespace
strategies::HistoryPort ConversationStore::history_port() {
    return {this, history_ready, archive_result, result_chunk};
}
// Wakes a held Task once no body batch, archive or record write is outstanding.
uint32_t ConversationStore::history_ready(void *context, uint64_t) {
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    for (const auto phase : {self.phase_, self.archiving_.phase}) {
        if (phase == Phase::queued || phase == Phase::writing) {
            return 0;
        }
    }
    return self.inserts_.empty() ? 1U : 0U;
}
ayni_status_t ConversationStore::body_write(void *context, uint64_t scope, ayni_bytes_v1 body,
                                            ayni_body_ref_v1 *out) {
    if (!context || !out || (body.size && !body.data)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    const auto digest = ayni_body_digest(body);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (!self.database_) {
        return AYNI_INVALID_STATE;
    }
    for (auto at = self.bodies_.begin(); at != self.bodies_.end(); ++at) {
        auto &stored = at->second;
        if (stored.scope != scope || stored.digest != digest || stored.bytes.size() != body.size ||
            (body.size && std::memcmp(stored.bytes.data(), body.data, body.size) != 0)) {
            continue;
        }
        if (stored.state == Body::State::failed) {
            self.bodies_.erase(at);
            return AYNI_STORAGE_FAILED;
        }
        if (stored.state == Body::State::pending) {
            return AYNI_NOT_READY;
        }
        *out = {at->first, digest, body.size, 0};
        return AYNI_OK;
    }
    const auto id = self.next_body_++;
    self.bodies_[id] = {scope, digest,
                        std::string(reinterpret_cast<const char *>(body.data), body.size),
                        Body::State::pending};
    self.inserts_.push_back(id);
    self.wake_.notify_all();
    return AYNI_NOT_READY;
}
// Owner-thread callers outside decide may wait for the worker's commit.
ayni_status_t ConversationStore::body_write_waiting(void *context, uint64_t scope,
                                                    ayni_bytes_v1 body, ayni_body_ref_v1 *out) {
    auto status = body_write(context, scope, body, out);
    if (status != AYNI_NOT_READY) {
        return status;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    {
        std::unique_lock<std::mutex> lock(self.mutex_);
        self.durable_.wait(lock, [&] {
            if (!self.database_) {
                return true;
            }
            for (const auto &[id, stored] : self.bodies_) {
                if (stored.state == Body::State::pending && stored.scope == scope &&
                    stored.bytes.size() == body.size &&
                    (!body.size || std::memcmp(stored.bytes.data(), body.data, body.size) == 0)) {
                    return false;
                }
            }
            return true;
        });
    }
    return body_write(context, scope, body, out);
}
ayni_status_t ConversationStore::body_read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                                           uint32_t capacity) {
    if (!context || !ref || !ref->id || (capacity && !out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    const auto found = self.bodies_.find(ref->id);
    if (found == self.bodies_.end() || found->second.state == Body::State::failed) {
        return AYNI_NOT_FOUND;
    }
    const auto &bytes = found->second.bytes;
    if (bytes.size() > capacity) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (!bytes.empty()) {
        std::memcpy(out, bytes.data(), bytes.size());
    }
    return AYNI_OK;
}
// Releasers call this only after no durable state names the body (bodies.h).
void ConversationStore::body_release(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !ref || !ref->id) {
        return;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (self.bodies_.erase(ref->id) != 0U) {
        self.deletes_.push_back(ref->id);
        self.wake_.notify_all();
    }
}
// Startup only: every live body enters the cache, so later reads need no SQL.
ayni_status_t ConversationStore::load_bodies() {
    auto query = prepare(database_, "SELECT id,scope,digest,body FROM bodies ORDER BY id");
    if (!query) {
        return AYNI_STORAGE_FAILED;
    }
    int result = SQLITE_OK;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        const auto id = static_cast<uint64_t>(sqlite3_column_int64(query.get(), 0));
        const auto size = sqlite3_column_bytes(query.get(), 3);
        if (!id || size < 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        Body body{static_cast<uint64_t>(sqlite3_column_int64(query.get(), 1)),
                  static_cast<uint64_t>(sqlite3_column_int64(query.get(), 2)),
                  std::string(static_cast<const char *>(sqlite3_column_blob(query.get(), 3)),
                              static_cast<size_t>(size)),
                  Body::State::durable};
        const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(body.bytes.data()),
                                  static_cast<uint32_t>(body.bytes.size())};
        if (ayni_body_digest(bytes) != body.digest) {
            return AYNI_STORAGE_FAILED;
        }
        bodies_[id] = std::move(body);
        next_body_ = id + 1U;
    }
    if (result != SQLITE_DONE) {
        return AYNI_STORAGE_FAILED;
    }
    // AUTOINCREMENT keeps the highest identity ever stored, so a deleted body's identity is
    // never handed out again, even when it was the newest.
    auto highest = prepare(database_, "SELECT seq FROM sqlite_sequence WHERE name='bodies'");
    if (!highest) {
        return AYNI_STORAGE_FAILED;
    }
    result = sqlite3_step(highest.get());
    if (result == SQLITE_ROW) {
        next_body_ = std::max<uint64_t>(
            next_body_, static_cast<uint64_t>(sqlite3_column_int64(highest.get(), 0)) + 1U);
        result = sqlite3_step(highest.get());
    }
    return result == SQLITE_DONE ? AYNI_OK : AYNI_STORAGE_FAILED;
}
// Worker only: one transaction inserts pending bodies, then removes released ones.
ayni_status_t ConversationStore::write_bodies(std::vector<std::pair<uint64_t, Body>> &inserts,
                                              std::vector<uint64_t> &deletes) {
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AYNI_STORAGE_FAILED;
    }
    const auto fail = [&] {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AYNI_STORAGE_FAILED;
    };
    for (const auto &[id, body] : inserts) {
        auto insert = prepare(
            database_, "INSERT OR REPLACE INTO bodies(id,scope,digest,body) VALUES(?,?,?,?)");
        if (!insert) {
            return fail();
        }
        integer(insert.get(), 1, id);
        integer(insert.get(), 2, body.scope);
        integer(insert.get(), 3, body.digest);
        sqlite3_bind_blob(insert.get(), 4, body.bytes.empty() ? "" : body.bytes.data(),
                          static_cast<int>(body.bytes.size()), SQLITE_STATIC);
        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            return fail();
        }
    }
    for (const auto id : deletes) {
        auto erase = prepare(database_, "DELETE FROM bodies WHERE id=?");
        if (!erase) {
            return fail();
        }
        integer(erase.get(), 1, id);
        if (sqlite3_step(erase.get()) != SQLITE_DONE) {
            return fail();
        }
    }
    return sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK ? AYNI_OK
                                                                                     : fail();
}
} // namespace ayni::runtime

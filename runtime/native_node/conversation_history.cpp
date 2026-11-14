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
ayni_status_t ConversationStore::body_stage(void *context, uint64_t scope, ayni_bytes_v1 body,
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
    if (self.body_status_ != AYNI_OK) {
        return self.body_status_;
    }
    std::shared_ptr<const std::string> shared;
    uint64_t content = 0;
    size_t bytes = 0;
    std::vector<uint64_t> counted;
    for (const auto &[id, stored] : self.bodies_) {
        if (std::find(counted.begin(), counted.end(), stored.content) == counted.end()) {
            counted.push_back(stored.content);
            bytes += stored.bytes->size();
        }
        if (stored.digest != digest || stored.bytes->size() != body.size ||
            (body.size && std::memcmp(stored.bytes->data(), body.data, body.size))) {
            continue;
        }
        if (stored.state == Body::State::failed) {
            return AYNI_STORAGE_FAILED;
        }
        if (stored.scope == scope) {
            *out = {id, digest, body.size, 0};
            return AYNI_OK;
        }
        shared = stored.bytes;
        content = stored.content;
    }
    if (self.bodies_.size() == body_reference_limit ||
        (!shared && (body.size > body_cache_limit || bytes > body_cache_limit - body.size))) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto id = self.next_body_++;
    if (!shared) {
        shared = std::make_shared<const std::string>(
            body.size ? reinterpret_cast<const char *>(body.data) : "", body.size);
        content = id;
    }
    self.bodies_[id] = {scope, digest, std::move(shared), content, Body::State::staged};
    *out = {id, digest, body.size, 0};
    return AYNI_OK;
}
ayni_status_t ConversationStore::body_confirm(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !ref || !ref->id) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (self.body_status_ != AYNI_OK) {
        return self.body_status_;
    }
    const auto found = self.bodies_.find(ref->id);
    if (found == self.bodies_.end()) {
        return AYNI_NOT_FOUND;
    }
    if (found->second.state == Body::State::staged) {
        found->second.state = Body::State::pending;
        self.inserts_.push_back(ref->id);
        self.wake_.notify_all();
    }
    return found->second.state == Body::State::durable  ? AYNI_OK
           : found->second.state == Body::State::failed ? AYNI_STORAGE_FAILED
                                                        : AYNI_NOT_READY;
}
ayni_status_t ConversationStore::body_write(void *context, uint64_t scope, ayni_bytes_v1 body,
                                            ayni_body_ref_v1 *out) {
    if (!out) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_body_ref_v1 staged{};
    const auto status = body_stage(context, scope, body, &staged);
    if (status != AYNI_OK) {
        return status;
    }
    const auto confirmed = body_confirm(context, &staged);
    if (confirmed == AYNI_OK) {
        *out = staged;
    }
    return confirmed;
}
ayni_status_t ConversationStore::body_share(void *context, uint64_t scope,
                                            const ayni_body_ref_v1 *source, ayni_body_ref_v1 *out) {
    if (!context || !source || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    std::shared_ptr<const std::string> bytes;
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto found = self.bodies_.find(source->id);
        if (found == self.bodies_.end()) {
            return AYNI_NOT_FOUND;
        }
        bytes = found->second.bytes;
    }
    return body_stage(
        context, scope,
        {reinterpret_cast<const uint8_t *>(bytes->data()), static_cast<uint32_t>(bytes->size())},
        out);
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
                    stored.bytes->size() == body.size &&
                    (!body.size || std::memcmp(stored.bytes->data(), body.data, body.size) == 0)) {
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
    if (self.body_status_ != AYNI_OK) {
        return self.body_status_;
    }
    if (found == self.bodies_.end()) {
        return AYNI_NOT_FOUND;
    }
    if (found->second.state == Body::State::failed) {
        return AYNI_STORAGE_FAILED;
    }
    const auto &bytes = *found->second.bytes;
    if (bytes.size() > capacity) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (!bytes.empty()) {
        std::memcpy(out, bytes.data(), bytes.size());
    }
    return AYNI_OK;
}
void ConversationStore::body_release_temporary(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !ref) {
        return;
    }
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    const auto found = self.bodies_.find(ref->id);
    if (found != self.bodies_.end() && found->second.state == Body::State::staged) {
        self.bodies_.erase(found); // No SQL ownership was ever confirmed for this model input.
    }
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
    auto query = prepare(database_, "SELECT b.id,b.scope,b.digest,d.body,b.content FROM bodies b "
                                    "JOIN body_data d ON d.id=b.content ORDER BY b.id");
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
                  std::make_shared<const std::string>(
                      static_cast<const char *>(sqlite3_column_blob(query.get(), 3)),
                      static_cast<size_t>(size)),
                  static_cast<uint64_t>(sqlite3_column_int64(query.get(), 4)),
                  Body::State::durable};
        const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(body.bytes->data()),
                                  static_cast<uint32_t>(body.bytes->size())};
        if (ayni_body_digest(bytes) != body.digest) {
            return AYNI_STORAGE_FAILED;
        }
        for (const auto &[other, stored] : bodies_) {
            if (stored.content == body.content) {
                body.bytes = stored.bytes;
                break;
            }
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
        auto payload = prepare(database_, "INSERT OR IGNORE INTO body_data(id,body) VALUES(?,?)");
        if (!payload) {
            return fail();
        }
        integer(payload.get(), 1, body.content);
        sqlite3_bind_blob(payload.get(), 2, body.bytes->empty() ? "" : body.bytes->data(),
                          static_cast<int>(body.bytes->size()), SQLITE_STATIC);
        if (sqlite3_step(payload.get()) != SQLITE_DONE) {
            return fail();
        }
        auto insert = prepare(
            database_, "INSERT OR REPLACE INTO bodies(id,scope,digest,content) VALUES(?,?,?,?)");
        if (!insert) {
            return fail();
        }
        integer(insert.get(), 1, id);
        integer(insert.get(), 2, body.scope);
        integer(insert.get(), 3, body.digest);
        integer(insert.get(), 4, body.content);
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
    if (sqlite3_exec(database_,
                     "DELETE FROM body_data WHERE id NOT IN (SELECT content FROM bodies)", nullptr,
                     nullptr, nullptr) != SQLITE_OK) {
        return fail();
    }
    return sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK ? AYNI_OK
                                                                                     : fail();
}
} // namespace ayni::runtime

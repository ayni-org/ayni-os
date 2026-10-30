#include "conversation_store.h"
#include <ayni/model_messages.hpp>
#include <sqlite3.h>
#include <string>
#include <vector>
namespace ayni::runtime {
namespace {
using Record = strategies::ConversationRecord;
using History = strategies::Text<strategies::history_bytes_max>;
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
bool same_owner(const Record &a, const Record &b) {
    return a.id == b.id && a.owner_instance_id == b.owner_instance_id &&
           a.generation == b.generation;
}
bool same_history(const History &a, const History &b) {
    return a.size == b.size &&
           (!a.size || std::memcmp(a.bytes.data(), b.bytes.data(), a.size) == 0);
}
struct Frame {
    uint64_t id = 0;
    std::string body;
    uint32_t role = 0;
};
// One row per message, with length framing retained exactly. A partially handled
// tool group may be stored but is never projected to the model until complete.
bool frames(const History &history, std::vector<Frame> &out) {
    if (!history.size) {
        return true;
    }
    model::Reader reader(history.view());
    model::Message message;
    uint32_t start = history.size - reader.remaining_bytes();
    while (reader.next(message)) {
        const auto end = history.size - reader.remaining_bytes();
        out.push_back({0, std::string(history.bytes.data() + start, end - start),
                       static_cast<uint32_t>(message.role)});
        start = end;
    }
    return reader.finished();
}
// Keyset pages bound each SQL result set; the assembled working view has its own
// byte resource limit, separate from the database's uncapped long message history.
ayni_status_t read_frames(sqlite3 *db, const Record &record, std::vector<Frame> &out) {
    if (!record.history_ref) {
        return AYNI_OK;
    }
    auto owner = prepare(
        db, "SELECT 1 FROM views WHERE id=? AND conversation=? AND owner=? AND generation=?");
    if (!owner) {
        return AYNI_STORAGE_FAILED;
    }
    integer(owner.get(), 1, record.history_ref);
    integer(owner.get(), 2, record.id);
    integer(owner.get(), 3, record.owner_instance_id);
    integer(owner.get(), 4, record.generation);
    if (sqlite3_step(owner.get()) != SQLITE_ROW) {
        return AYNI_NOT_FOUND;
    }
    constexpr int page_rows = 64;
    uint64_t ordinal = 0;
    size_t total = model::messages_prefix.size();
    for (;;) {
        auto query = prepare(
            db, "SELECT v.ordinal,m.id,m.body,m.conversation,m.owner,m.generation FROM "
                "view_messages v LEFT JOIN messages m ON "
                "m.id=v.message_id WHERE v.view_id=? AND v.ordinal>=? ORDER BY v.ordinal LIMIT ?");
        if (!query) {
            return AYNI_STORAGE_FAILED;
        }
        integer(query.get(), 1, record.history_ref);
        integer(query.get(), 2, ordinal);
        sqlite3_bind_int(query.get(), 3, page_rows);
        int rows = 0, result = SQLITE_OK;
        while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
            if (static_cast<uint64_t>(sqlite3_column_int64(query.get(), 3)) != record.id ||
                static_cast<uint64_t>(sqlite3_column_int64(query.get(), 4)) !=
                    record.owner_instance_id ||
                static_cast<uint64_t>(sqlite3_column_int64(query.get(), 5)) != record.generation) {
                return AYNI_DENIED;
            }
            const auto index = static_cast<uint64_t>(sqlite3_column_int64(query.get(), 0));
            const auto size = sqlite3_column_bytes(query.get(), 2);
            if (index != ordinal || size < 0 ||
                static_cast<size_t>(size) > strategies::history_bytes_max - total) {
                return AYNI_INVALID_ARGUMENT;
            }
            out.push_back(
                {static_cast<uint64_t>(sqlite3_column_int64(query.get(), 1)),
                 std::string(static_cast<const char *>(sqlite3_column_blob(query.get(), 2)),
                             static_cast<size_t>(size))});
            total += static_cast<size_t>(size);
            ++ordinal;
            ++rows;
        }
        if (result != SQLITE_DONE) {
            return AYNI_STORAGE_FAILED;
        }
        if (rows < page_rows) {
            return AYNI_OK;
        }
    }
}
} // namespace
strategies::HistoryPort ConversationStore::history_port() {
    return {this, history_ready, history_load, history_save, archive_result, result_chunk};
}
uint32_t ConversationStore::history_ready(void *context, uint64_t) {
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    for (const auto phase :
         {self.phase_, self.loading_.phase, self.storing_.phase, self.archiving_.phase}) {
        if (phase == Phase::queued || phase == Phase::writing) {
            return 0;
        }
    }
    return 1;
}

ayni_status_t ConversationStore::history_load(void *context, uint64_t task, const Record &record,
                                              History &out) {
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto &job = self.loading_;
    if (record.id != self.conversation_ || record.owner_instance_id != self.owner_ ||
        record.generation != self.generation_) {
        return AYNI_EXPIRED;
    }
    if (job.phase == Phase::queued || job.phase == Phase::writing) {
        return AYNI_NOT_READY;
    }
    if (job.phase == Phase::done && same_owner(job.record, record) &&
        job.record.history_ref == record.history_ref) {
        if (job.status == AYNI_OK) {
            out = job.record.history;
        }
        return job.status;
    }
    job = {};
    job.task = task;
    job.record = record;
    job.record.history = {};
    job.phase = Phase::queued;
    self.wake_.notify_one();
    return AYNI_NOT_READY;
}
ayni_status_t ConversationStore::history_save(void *context, uint64_t task, uint64_t turn,
                                              const Record &record, uint64_t &reference) {
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto &job = self.storing_;
    if (record.id != self.conversation_ || record.owner_instance_id != self.owner_ ||
        record.generation != self.generation_) {
        return AYNI_EXPIRED;
    }
    if (job.phase == Phase::queued || job.phase == Phase::writing) {
        return AYNI_NOT_READY;
    }
    if (job.phase == Phase::done && job.task == task && job.turn == turn &&
        same_owner(job.record, record) && job.record.history_ref == record.history_ref &&
        same_history(job.record.history, record.history)) {
        if (job.status == AYNI_OK) {
            reference = job.reference;
        }
        return job.status;
    }
    job = {};
    job.task = task;
    job.turn = turn;
    job.record = record;
    job.phase = Phase::queued;
    self.wake_.notify_one();
    return AYNI_NOT_READY;
}
ayni_status_t ConversationStore::read_history(HistoryJob &job) {
    const auto current = current_namespace(job.record);
    if (current != AYNI_OK) {
        return current;
    }
    std::vector<Frame> source;
    const auto status = read_frames(database_, job.record, source);
    if (status != AYNI_OK) {
        return status;
    }
    model::Writer writer(job.record.history.bytes.data(), strategies::history_bytes_max);
    writer.prefix();
    for (const auto &frame : source) {
        writer.raw(std::string_view(frame.body));
    }
    ayni_string_v1 body{};
    if (!writer.finish(body)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    job.record.history.size = body.size;
    return AYNI_OK;
}
// Immutable views reuse the unchanged prefix/suffix message IDs. Only new frames
// append payload rows; compaction retains all original rows for paged inspection.
ayni_status_t ConversationStore::write_history(HistoryJob &job) {
    const auto current = current_namespace(job.record);
    if (current != AYNI_OK) {
        return current;
    }
    std::vector<Frame> before, after;
    auto status = read_frames(database_, job.record, before);
    if (status != AYNI_OK) {
        return status;
    }
    if (!frames(job.record.history, after)) {
        return AYNI_INVALID_ARGUMENT;
    }
    size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() &&
           before[prefix].body == after[prefix].body) {
        after[prefix].id = before[prefix].id;
        ++prefix;
    }
    if (prefix == before.size() && prefix == after.size()) {
        job.reference = job.record.history_ref;
        return AYNI_OK;
    }
    size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - suffix - 1].body == after[after.size() - suffix - 1].body) {
        after[after.size() - suffix - 1].id = before[before.size() - suffix - 1].id;
        ++suffix;
    }
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AYNI_STORAGE_FAILED;
    }
    const auto fail = [&] {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AYNI_STORAGE_FAILED;
    };
    auto view =
        prepare(database_, "INSERT INTO views(conversation,owner,generation) VALUES(?,?,?)");
    if (!view) {
        return fail();
    }
    integer(view.get(), 1, job.record.id);
    integer(view.get(), 2, job.record.owner_instance_id);
    integer(view.get(), 3, job.record.generation);
    if (sqlite3_step(view.get()) != SQLITE_DONE) {
        return fail();
    }
    const auto reference = static_cast<uint64_t>(sqlite3_last_insert_rowid(database_));
    for (size_t at = 0; at < after.size(); ++at) {
        auto &frame = after[at];
        if (!frame.id) {
            auto append = prepare(
                database_, "INSERT INTO messages(conversation,owner,generation,turn,role,body) "
                           "VALUES(?,?,?,?,?,?)");
            if (!append) {
                return fail();
            }
            integer(append.get(), 1, job.record.id);
            integer(append.get(), 2, job.record.owner_instance_id);
            integer(append.get(), 3, job.record.generation);
            integer(append.get(), 4, job.turn);
            integer(append.get(), 5, frame.role);
            sqlite3_bind_blob(append.get(), 6, frame.body.data(),
                              static_cast<int>(frame.body.size()), SQLITE_STATIC);
            if (sqlite3_step(append.get()) != SQLITE_DONE) {
                return fail();
            }
            frame.id = static_cast<uint64_t>(sqlite3_last_insert_rowid(database_));
        }
        auto link = prepare(database_, "INSERT INTO view_messages VALUES(?,?,?)");
        if (!link) {
            return fail();
        }
        integer(link.get(), 1, reference);
        integer(link.get(), 2, at);
        integer(link.get(), 3, frame.id);
        if (sqlite3_step(link.get()) != SQLITE_DONE) {
            return fail();
        }
    }
    if (sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail();
    }
    job.reference = reference;
    return AYNI_OK;
}
} // namespace ayni::runtime

#include "conversation_store.h"
#include <ayni/call_result_json.hpp>
#include <sqlite3.h>
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
void blob(sqlite3_stmt *statement, int index, ayni_bytes_v1 value) {
    sqlite3_bind_blob(statement, index,
                      value.size ? static_cast<const void *>(value.data)
                                 : static_cast<const void *>(""),
                      static_cast<int>(value.size), SQLITE_STATIC);
}
} // namespace
ayni_status_t ConversationStore::current_namespace(const strategies::ConversationRecord &record) {
    SavedConversation current;
    const auto status = read(current);
    if (status != AYNI_OK) {
        return status;
    }
    return current.record.id == record.id &&
                   current.record.owner_instance_id == record.owner_instance_id &&
                   current.record.generation == record.generation
               ? AYNI_OK
               : AYNI_EXPIRED;
}
// Owner thread copies every borrowed byte to the reserved lane before yielding.
// The result's Core sequence identifies an immutable original or query conclusion.
ayni_status_t ConversationStore::archive_result(void *context, uint64_t task,
                                                const strategies::ConversationRecord &record,
                                                const ayni_task_call_result_v1 &result,
                                                strategies::ArchivedResult &archived) {
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (record.id != self.conversation_ || record.owner_instance_id != self.owner_ ||
        record.generation != self.generation_) {
        return AYNI_EXPIRED;
    }
    auto &job = self.archiving_;
    if (job.phase == Phase::queued || job.phase == Phase::writing) {
        return AYNI_NOT_READY;
    }
    if (job.phase == Phase::done && job.task == task && job.conversation == record.id &&
        job.owner == record.owner_instance_id && job.generation == record.generation &&
        job.result.logical_call_id == result.logical_call_id &&
        job.result.requires_seq == result.requires_seq) {
        if (job.status == AYNI_OK) {
            archived = job.archived;
        }
        return job.status;
    }
    const uint64_t size = uint64_t{result.text.size} + result.encoded_value.size +
                          result.original_domain.size + result.business_contract.size;
    if (!result.outcome || !result.logical_call_id || !result.requires_seq || size > UINT32_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    job = {};
    job.task = task;
    job.conversation = record.id;
    job.owner = record.owner_instance_id;
    job.generation = record.generation;
    job.result = result;
    job.raw.resize(size);
    uint32_t offset = 0;
    const auto copy = [&](ayni_bytes_v1 source) -> ayni_bytes_v1 {
        auto *destination = job.raw.data() + offset;
        if (source.size) {
            std::memcpy(destination, source.data, source.size);
        }
        offset += source.size;
        return {destination, source.size};
    };
    const auto text = copy({reinterpret_cast<const uint8_t *>(result.text.data), result.text.size});
    job.result.text = {reinterpret_cast<const char *>(text.data), text.size};
    job.result.encoded_value = copy(result.encoded_value);
    job.result.original_domain = copy(result.original_domain);
    job.result.business_contract = copy(result.business_contract);
    job.phase = Phase::queued;
    self.wake_.notify_one();
    return AYNI_NOT_READY;
}
ayni_status_t ConversationStore::result_chunk(void *context, uint64_t task, uint64_t reference,
                                              uint32_t offset, std::string &out, uint32_t maximum) {
    auto &self = *static_cast<ConversationStore *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto &job = self.archiving_;
    if (job.phase != Phase::done || job.status != AYNI_OK || job.task != task ||
        job.archived.reference != reference || job.conversation != self.conversation_ ||
        job.owner != self.owner_ || job.generation != self.generation_) {
        return AYNI_EXPIRED;
    }
    if (offset > job.archived.json_bytes || !maximum) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t take = std::min(maximum, job.archived.json_bytes - offset);
    constexpr unsigned continuation_mask = 0xc0, continuation = 0x80;
    while (take && offset + take < job.archived.json_bytes &&
           (static_cast<unsigned char>(job.json[offset + take]) & continuation_mask) ==
               continuation) {
        --take;
    }
    if (!take && offset < job.archived.json_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out.assign(job.json.data() + offset, take);
    return AYNI_OK;
}
// Renders the job's result into json, sized by a measuring pass first.
ayni_status_t ConversationStore::render(uint64_t reference, size_t &size) {
    auto &job = archiving_;
    auto measured = json::Writer::measuring();
    auto status = json::write_call_result(result_workspace_.get(), measured, job.result, reference);
    if (status != AYNI_OK) {
        return status;
    }
    if (!measured.finish(size) || size > UINT32_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    job.json.assign(size, '\0');
    json::Writer writer(job.json.data(), size);
    status = json::write_call_result(result_workspace_.get(), writer, job.result, reference);
    size_t written = 0;
    return status != AYNI_OK                           ? status
           : writer.finish(written) && written == size ? AYNI_OK
                                                       : AYNI_CAPACITY_EXCEEDED;
}
// SQL owns complete original bytes/proofs and the exact rendered view. The
// transaction commits before a Core checkpoint may contain the result reference.
ayni_status_t ConversationStore::write_result() {
    auto &job = archiving_;
    strategies::ConversationRecord record;
    record.id = job.conversation;
    record.owner_instance_id = job.owner;
    record.generation = job.generation;
    const auto current = current_namespace(record);
    if (current != AYNI_OK) {
        return current;
    }
    auto prior = prepare(database_,
                         "SELECT id,json,text,value,domain,proof FROM results WHERE conversation=? "
                         "AND owner=? AND generation=? AND task=? AND call=? AND sequence=?");
    if (!prior) {
        return AYNI_STORAGE_FAILED;
    }
    integer(prior.get(), 1, job.conversation);
    integer(prior.get(), 2, job.owner);
    integer(prior.get(), 3, job.generation);
    integer(prior.get(), 4, job.task);
    integer(prior.get(), 5, job.result.logical_call_id);
    integer(prior.get(), 6, job.result.requires_seq);
    const auto found = sqlite3_step(prior.get());
    if (found == SQLITE_ROW) {
        const auto size = sqlite3_column_bytes(prior.get(), 1);
        if (size < 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        const std::array<ayni_bytes_v1, 4> originals{
            {{reinterpret_cast<const uint8_t *>(job.result.text.data), job.result.text.size},
             job.result.encoded_value,
             job.result.original_domain,
             job.result.business_contract}};
        int column = 2;
        for (const auto original : originals) {
            if (sqlite3_column_bytes(prior.get(), column) != static_cast<int>(original.size) ||
                (original.size && std::memcmp(sqlite3_column_blob(prior.get(), column),
                                              original.data, original.size))) {
                return AYNI_INVALID_ARGUMENT;
            }
            ++column;
        }
        const auto reference = static_cast<uint64_t>(sqlite3_column_int64(prior.get(), 0));
        size_t written = 0;
        const auto rendered = render(reference, written);
        if (rendered != AYNI_OK || written != static_cast<size_t>(size) ||
            (written &&
             std::memcmp(job.json.data(), sqlite3_column_blob(prior.get(), 1), written))) {
            return AYNI_INVALID_ARGUMENT;
        }
        job.archived = {reference, static_cast<uint32_t>(size)};
        return AYNI_OK;
    }
    if (found != SQLITE_DONE) {
        return AYNI_STORAGE_FAILED;
    }
    prior.reset();
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return AYNI_STORAGE_FAILED;
    }
    const auto fail = [&] {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        return AYNI_STORAGE_FAILED;
    };
    auto append = prepare(
        database_, "INSERT INTO "
                   "results(conversation,owner,generation,task,call,sequence,operation,outcome,"
                   "status,reason,effect,text,value,domain,proof,json,usage_kind,input_tokens,"
                   "output_tokens,total_tokens) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!append) {
        return fail();
    }
    integer(append.get(), 1, job.conversation);
    integer(append.get(), 2, job.owner);
    integer(append.get(), 3, job.generation);
    integer(append.get(), 4, job.task);
    integer(append.get(), 5, job.result.logical_call_id);
    integer(append.get(), 6, job.result.requires_seq);
    integer(append.get(), 7, job.result.operation_id);
    integer(append.get(), 8, job.result.outcome);
    sqlite3_bind_int(append.get(), 9, job.result.status);
    integer(append.get(), 10, job.result.reason);
    integer(append.get(), 11, job.result.effect);
    blob(append.get(), 12,
         {reinterpret_cast<const uint8_t *>(job.result.text.data), job.result.text.size});
    blob(append.get(), 13, job.result.encoded_value);
    blob(append.get(), 14, job.result.original_domain);
    blob(append.get(), 15, job.result.business_contract);
    blob(append.get(), 16, {});
    integer(append.get(), 17, job.result.model_usage.kind);
    integer(append.get(), 18, job.result.model_usage.input_tokens);
    integer(append.get(), 19, job.result.model_usage.output_tokens);
    integer(append.get(), 20, job.result.model_usage.total_tokens);
    if (sqlite3_step(append.get()) != SQLITE_DONE) {
        return fail();
    }
    const auto reference = static_cast<uint64_t>(sqlite3_last_insert_rowid(database_));
    size_t size = 0;
    const auto rendered = render(reference, size);
    if (rendered != AYNI_OK) {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        return rendered;
    }
    auto update = prepare(database_, "UPDATE results SET json=? WHERE id=?");
    if (!update) {
        return fail();
    }
    blob(update.get(), 1,
         {reinterpret_cast<const uint8_t *>(job.json.data()), static_cast<uint32_t>(size)});
    integer(update.get(), 2, reference);
    if (sqlite3_step(update.get()) != SQLITE_DONE ||
        sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail();
    }
    job.archived = {reference, static_cast<uint32_t>(size)};
    return AYNI_OK;
}
} // namespace ayni::runtime

#include <ayni/storage_format.h>

#include <ayni/hosts/sim.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ayni::hosts::sim {
namespace {

bool has_identity(const StoredBatch &batch, uint32_t writer_epoch, uint64_t transition_id,
                  uint64_t commit_seq) {
    ayni_storage_batch_identity_v1 identity{};
    return batch.commit_seq == commit_seq &&
           ayni_storage_read_batch_identity(batch.bytes.data(), batch.bytes.size(), &identity) &&
           identity.writer_epoch == writer_epoch && identity.transition_id == transition_id;
}

} // namespace

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &Host::monotonic_now_us;
    host.utc_now = &Host::utc_now;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    host.storage_append = &Host::storage_append;
    host.storage_read_prefix = &Host::storage_read_prefix;
    host.storage_read_log = &Host::storage_read_log;
    host.storage_fence_writers = &Host::storage_fence_writers;
    host.storage_write_snapshot = &Host::storage_write_snapshot;
    host.storage_read_snapshot = &Host::storage_read_snapshot;
    host.storage_trim_prefix = &Host::storage_trim_prefix;
    host.storage_free_bytes = &Host::storage_free_bytes;
    return host;
}

void Host::inject_fault(StoragePort port, StorageFault fault) {
    faults_[static_cast<std::size_t>(port)].push_back(fault);
}

std::size_t Host::complete_storage() {
    std::size_t accepted = 0;
    while (!pending_.empty()) {
        const Pending request = pending_.front();
        pending_.pop_front();
        const ayni_completion_result_v1 result = run(request);
        if (ayni_completion_post(request.completion, &result) == AYNI_OK) {
            ++accepted;
        }
    }
    return accepted;
}

void Host::power_cut(CutAppend append) {
    for (const Pending &request : pending_) {
        if (append == CutAppend::lost) {
            continue;
        }
        Pending persisted = request;
        persisted.fault = append == CutAppend::stored
                              ? std::nullopt
                              : std::optional<StorageFault>(StorageFault::short_write);
        if (request.port == StoragePort::append || request.port == StoragePort::write_snapshot) {
            static_cast<void>(run(persisted));
        }
    }
    pending_.clear();
}

std::optional<StorageFault> Host::next_fault(StoragePort port) {
    auto &queue = faults_[static_cast<std::size_t>(port)];
    if (queue.empty() && port == StoragePort::read_log) {
        return next_fault(StoragePort::read_prefix);
    }
    if (queue.empty()) {
        return std::nullopt;
    }
    const StorageFault fault = queue.front();
    queue.pop_front();
    ++faults_applied_;
    return fault;
}

void Host::utc_now(void *ctx, ayni_utc_sample_v1 *out) {
    auto &self = *static_cast<Host *>(ctx);
    self.note_call();
    *out = {sizeof(*out), self.utc_trust_, self.utc_s_};
}

uint64_t Host::monotonic_now_us(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    const uint64_t now = self.now_us_;
    self.now_us_ += self.advance_per_read_us_;
    return now;
}

void Host::wake(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    self.wake_count_.fetch_add(1);
}

void Host::ingress_lock(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    if (self.holder_.load() == std::this_thread::get_id()) {
        // A second lock on the same thread would deadlock; fail loudly instead.
        std::fputs("sim host: ingress lock taken twice by one thread\n", stderr);
        std::abort();
    }
    self.ingress_.lock();
    self.holder_.store(std::this_thread::get_id());
}

void Host::ingress_unlock(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    self.holder_.store(std::thread::id{});
    self.ingress_.unlock();
}

ayni_submit_result_t Host::storage_append(void *ctx, const ayni_storage_batch_v1 *batch,
                                          ayni_attempt_ref_v1 attempt,
                                          ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (batch == nullptr || completion == nullptr || batch->writer_epoch == 0U ||
        batch->encoded.data == nullptr || batch->encoded.size == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni_storage_batch_identity_v1 identity{};
    if (ayni_storage_read_batch_identity(batch->encoded.data, batch->encoded.size, &identity) &&
        (identity.writer_epoch != batch->writer_epoch || identity.commit_seq != batch->commit_seq ||
         identity.transition_id != batch->transition_id)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const Pending request{StoragePort::append,
                          self.next_fault(StoragePort::append),
                          attempt,
                          completion,
                          batch->commit_seq,
                          batch->encoded.data,
                          nullptr,
                          batch->encoded.size,
                          batch->writer_epoch,
                          batch->transition_id};
    return self.accept(request);
}

ayni_submit_result_t Host::storage_read_prefix(void *ctx, uint64_t from_commit_seq, uint8_t *buffer,
                                               uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (completion == nullptr || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const Pending request{StoragePort::read_prefix,
                          self.next_fault(StoragePort::read_prefix),
                          attempt,
                          completion,
                          from_commit_seq,
                          nullptr,
                          buffer,
                          buffer_size};
    return self.accept(request);
}

ayni_submit_result_t Host::storage_read_log(void *ctx, uint64_t cursor, uint8_t *buffer,
                                            uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (completion == nullptr || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return self.accept({StoragePort::read_log, self.next_fault(StoragePort::read_log), attempt,
                        completion, cursor, nullptr, buffer, buffer_size});
}

ayni_submit_result_t Host::storage_fence_writers(void *ctx, uint32_t minimum_epoch,
                                                 ayni_attempt_ref_v1 attempt,
                                                 ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (completion == nullptr || minimum_epoch == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return self.accept({StoragePort::fence, self.next_fault(StoragePort::fence), attempt,
                        completion, 0, nullptr, nullptr, 0, minimum_epoch});
}

ayni_submit_result_t Host::storage_write_snapshot(void *ctx, uint32_t writer_epoch, uint32_t slot,
                                                  uint64_t offset, ayni_bytes_v1 bytes,
                                                  ayni_attempt_ref_v1 attempt,
                                                  ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (completion == nullptr || slot > 1U || writer_epoch == 0U ||
        (bytes.data == nullptr && bytes.size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return self.accept({StoragePort::write_snapshot, self.next_fault(StoragePort::write_snapshot),
                        attempt, completion, offset, bytes.data, nullptr, bytes.size, writer_epoch,
                        0, slot});
}

ayni_submit_result_t Host::storage_read_snapshot(void *ctx, uint32_t slot, uint64_t offset,
                                                 uint8_t *buffer, uint32_t buffer_size,
                                                 ayni_attempt_ref_v1 attempt,
                                                 ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (completion == nullptr || slot > 1U || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return self.accept({StoragePort::read_snapshot, self.next_fault(StoragePort::read_snapshot),
                        attempt, completion, offset, nullptr, buffer, buffer_size, 0, 0, slot});
}

ayni_submit_result_t Host::storage_trim_prefix(void *ctx, uint32_t writer_epoch,
                                               uint64_t transition_id, uint64_t commit_seq,
                                               ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (completion == nullptr || writer_epoch == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return self.accept({StoragePort::trim_prefix, self.next_fault(StoragePort::trim_prefix),
                        attempt, completion, commit_seq, nullptr, nullptr, 0, writer_epoch,
                        transition_id});
}

ayni_status_t Host::storage_free_bytes(void *ctx, uint64_t *out_free_bytes) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    if (out_free_bytes == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint64_t used = self.used_bytes();
    for (const Pending &request : self.pending_) {
        if (request.port == StoragePort::append || request.port == StoragePort::write_snapshot) {
            used += request.size + (request.port == StoragePort::append ? 24U : 0U);
        }
    }
    *out_free_bytes = used >= self.storage_capacity_ ? 0U : self.storage_capacity_ - used;
    return AYNI_OK;
}

void Host::note_call() {
    if (holder_.load() == std::this_thread::get_id()) {
        calls_under_lock_.fetch_add(1);
    }
}

ayni_submit_result_t Host::accept(const Pending &request) {
    attempts_.push_back(request.attempt);
    if (request.fault == StorageFault::reject) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    pending_.push_back(request);
    return request.fault == StorageFault::accept_unknown ? AYNI_SUBMIT_ACCEPT_UNKNOWN
                                                         : AYNI_SUBMIT_ACCEPTED;
}

ayni_completion_result_v1 Host::run(const Pending &request) {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    if (request.fault == StorageFault::fail || request.fault == StorageFault::unavailable) {
        result.outcome =
            request.port == StoragePort::fence ? AYNI_FENCE_UNAVAILABLE : AYNI_WRITE_FAILED;
        return result;
    }
    if (request.fault == StorageFault::unknown_lost) {
        result.outcome =
            request.port == StoragePort::fence ? AYNI_FENCE_UNAVAILABLE : AYNI_WRITE_UNKNOWN;
        return result;
    }
    const bool mutation = request.port == StoragePort::append ||
                          request.port == StoragePort::write_snapshot ||
                          request.port == StoragePort::trim_prefix;
    if (mutation && request.writer_epoch < storage_.writer_epoch) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    if (mutation) {
        storage_.writer_epoch = request.writer_epoch;
    }
    switch (request.port) {
    case StoragePort::append: {
        if (request.fault == StorageFault::unknown_deferred) {
            late_appends_.push_back(
                {request.writer_epoch,
                 {request.commit_seq,
                  std::vector<uint8_t>(request.data, request.data + request.size)}});
            result.outcome = AYNI_WRITE_UNKNOWN;
            return result;
        }
        while (!storage_.log.empty() && storage_.log.back().torn) {
            storage_.log.pop_back();
        }
        for (const StoredBatch &batch : storage_.log) {
            if (batch.commit_seq != request.commit_seq) {
                continue;
            }
            ayni_storage_batch_identity_v1 identity{};
            const bool encoded =
                ayni_storage_read_batch_identity(request.data, request.size, &identity) != 0;
            if (!encoded || has_identity(batch, request.writer_epoch, request.transition_id,
                                         request.commit_seq)) {
                result.outcome =
                    batch.bytes.size() == request.size &&
                            std::equal(batch.bytes.begin(), batch.bytes.end(), request.data)
                        ? AYNI_WRITE_OK
                        : AYNI_WRITE_FAILED;
                return result;
            }
        }
        if (used_bytes() + request.size + 24U > storage_capacity_) {
            result.outcome = AYNI_WRITE_FAILED;
            return result;
        }
        const bool torn = request.fault == StorageFault::short_write;
        const uint32_t kept = torn ? request.size / 2U : request.size;
        storage_.log.push_back(
            {request.commit_seq, std::vector<uint8_t>(request.data, request.data + kept), torn});
        result.outcome = torn ? AYNI_WRITE_FAILED : AYNI_WRITE_OK;
        break;
    }
    case StoragePort::read_prefix:
    case StoragePort::read_log:
        result = read(request);
        break;
    case StoragePort::fence:
        if (storage_.writer_epoch == UINT32_MAX || request.writer_epoch == 0U) {
            result.outcome = AYNI_FENCE_UNAVAILABLE;
        } else {
            storage_.writer_epoch = std::max(request.writer_epoch, storage_.writer_epoch + 1U);
            result.outcome = AYNI_FENCE_EPOCH_INSTALLED;
            result.value = storage_.writer_epoch;
        }
        break;
    case StoragePort::read_snapshot:
    case StoragePort::write_snapshot:
        result = snapshot(request);
        break;
    case StoragePort::trim_prefix: {
        auto &log = storage_.log;
        const auto retained =
            std::find_if(log.begin(), log.end(), [&request](const StoredBatch &batch) {
                return !batch.torn && has_identity(batch, request.writer_epoch,
                                                   request.transition_id, request.commit_seq);
            });
        if (retained == log.end()) {
            result.outcome = AYNI_WRITE_FAILED;
        } else {
            log.erase(log.begin(), retained);
            result.outcome = AYNI_WRITE_OK;
        }
        break;
    }
    }
    if (request.fault == StorageFault::unknown_persisted) {
        result.outcome =
            request.port == StoragePort::fence ? AYNI_FENCE_UNAVAILABLE : AYNI_WRITE_UNKNOWN;
    }
    return result;
}

// The physical cursor includes every frame, even an older writer's frame
// with a lower commit_seq. Only the Kernel interprets those identities.
ayni_completion_result_v1 Host::read(const Pending &request) const {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    const bool physical = request.port == StoragePort::read_log;
    const uint64_t start = physical && request.commit_seq != 0U ? request.commit_seq - 1U : 0U;
    if (physical && start > storage_.log.size()) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    uint32_t written = 0;
    std::size_t index = static_cast<std::size_t>(start);
    for (; index < storage_.log.size(); ++index) {
        const StoredBatch &batch = storage_.log[index];
        if (batch.torn) {
            break;
        }
        if (!physical && batch.commit_seq < request.commit_seq) {
            continue;
        }
        if (batch.bytes.size() > request.size - written) {
            if (written == 0U) {
                result.outcome = AYNI_READ_BUFFER_TOO_SMALL;
                result.value = batch.bytes.size();
                return result;
            }
            break;
        }
        if (!batch.bytes.empty()) {
            std::memcpy(request.buffer + written, batch.bytes.data(), batch.bytes.size());
        }
        written += static_cast<uint32_t>(batch.bytes.size());
    }
    result.outcome = AYNI_WRITE_OK;
    result.value = written;
    result.next_cursor = physical ? index + 1U : 0U;
    return result;
}

ayni_completion_result_v1 Host::snapshot(const Pending &request) {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    if (request.slot > 1U) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    auto &bytes = storage_.snapshots[request.slot];
    const uint64_t offset = request.commit_seq;
    if (request.port == StoragePort::read_snapshot) {
        const uint32_t count =
            offset >= bytes.size()
                ? 0U
                : static_cast<uint32_t>(std::min<uint64_t>(request.size, bytes.size() - offset));
        if (count != 0U) {
            std::memcpy(request.buffer, bytes.data() + offset, count);
        }
        result.outcome = AYNI_WRITE_OK;
        result.value = count;
        return result;
    }
    constexpr uint64_t snapshot_limit = 8U * 1024U * 1024U;
    if (offset > snapshot_limit || request.size > snapshot_limit - offset) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    const uint64_t replacement_size = offset + request.size;
    const uint64_t used_without_slot = used_bytes() - bytes.size();
    if (replacement_size > storage_capacity_ ||
        used_without_slot > storage_capacity_ - replacement_size) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    if (offset <= bytes.size() && request.size <= bytes.size() - offset &&
        (request.size == 0U || std::equal(request.data, request.data + request.size,
                                          bytes.begin() + static_cast<std::ptrdiff_t>(offset)))) {
        result.outcome = AYNI_WRITE_OK;
        return result;
    }
    if (offset == 0U) {
        bytes.clear();
    } else if (offset > bytes.size() || offset + request.size < bytes.size() ||
               !std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end(),
                           request.data)) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    const bool torn = request.fault == StorageFault::short_write;
    const uint32_t kept = torn ? request.size / 2U : request.size;
    bytes.resize(static_cast<std::size_t>(offset + kept));
    if (kept != 0U) {
        std::memcpy(bytes.data() + offset, request.data, kept);
    }
    result.outcome = torn ? AYNI_WRITE_FAILED : AYNI_WRITE_OK;
    return result;
}

uint64_t Host::used_bytes() const {
    uint64_t used = 8;
    for (const StoredBatch &batch : storage_.log) {
        used += batch.bytes.size() + 24U;
    }
    for (const auto &slot : storage_.snapshots) {
        used += slot.size();
    }
    return used;
}

std::size_t Host::complete_late_storage() {
    std::size_t stored = 0;
    for (LateAppend &append : late_appends_) {
        if (append.writer_epoch < storage_.writer_epoch) {
            continue;
        }
        // A deferred write is still an append. A torn retry can only be
        // replaced at the tail, before the complete copy becomes visible.
        while (!storage_.log.empty() && storage_.log.back().torn) {
            storage_.log.pop_back();
        }
        const bool duplicate = std::any_of(
            storage_.log.begin(), storage_.log.end(), [&append](const StoredBatch &existing) {
                return existing.commit_seq == append.batch.commit_seq &&
                       existing.bytes == append.batch.bytes && !existing.torn;
            });
        if (!duplicate && used_bytes() + append.batch.bytes.size() + 24U <= storage_capacity_) {
            storage_.log.push_back(std::move(append.batch));
            ++stored;
        }
    }
    late_appends_.clear();
    return stored;
}

} // namespace ayni::hosts::sim

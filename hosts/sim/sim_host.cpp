#include <ayni/hosts/sim.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ayni::hosts::sim {

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &Host::monotonic_now_us;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    host.storage_append = &Host::storage_append;
    host.storage_read_prefix = &Host::storage_read_prefix;
    return host;
}

void Host::inject_fault(StoragePort port, StorageFault fault) {
    (port == StoragePort::append ? append_fault_ : read_fault_) = fault;
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
        if (request.port != StoragePort::append || append == CutAppend::lost) {
            continue;
        }
        const uint32_t kept = append == CutAppend::stored ? request.size : request.size / 2U;
        log_.push_back({request.commit_seq, std::vector<uint8_t>(request.data, request.data + kept),
                        append == CutAppend::torn});
    }
    pending_.clear();
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
    const Pending request{StoragePort::append,
                          std::exchange(self.append_fault_, std::nullopt),
                          attempt,
                          completion,
                          batch->commit_seq,
                          batch->encoded.data,
                          nullptr,
                          batch->encoded.size};
    return self.accept(request);
}

ayni_submit_result_t Host::storage_read_prefix(void *ctx, uint64_t from_commit_seq, uint8_t *buffer,
                                               uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    const Pending request{StoragePort::read_prefix,
                          std::exchange(self.read_fault_, std::nullopt),
                          attempt,
                          completion,
                          from_commit_seq,
                          nullptr,
                          buffer,
                          buffer_size};
    return self.accept(request);
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
    return AYNI_SUBMIT_ACCEPTED;
}

ayni_completion_result_v1 Host::run(const Pending &request) {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    if (request.fault == StorageFault::fail) {
        result.outcome = AYNI_WRITE_FAILED;
        return result;
    }
    if (request.fault == StorageFault::unknown_lost) {
        result.outcome = AYNI_WRITE_UNKNOWN;
        return result;
    }
    if (request.port == StoragePort::append) {
        while (!log_.empty() && log_.back().torn) {
            log_.pop_back();
        }
        log_.push_back(
            {request.commit_seq, std::vector<uint8_t>(request.data, request.data + request.size)});
        result.outcome = AYNI_WRITE_OK;
    } else {
        result = read(request);
    }
    if (request.fault == StorageFault::unknown_persisted) {
        result.outcome = AYNI_WRITE_UNKNOWN;
    }
    return result;
}

// Whole batches from the first one at or after from_commit_seq, as many as
// fit; a first batch that does not fit is a failed read.
ayni_completion_result_v1 Host::read(const Pending &request) const {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    uint32_t written = 0;
    for (const StoredBatch &batch : log_) {
        if (batch.torn) {
            break;
        }
        if (batch.commit_seq < request.commit_seq) {
            continue;
        }
        if (batch.bytes.size() > request.size - written) {
            if (written == 0U) {
                result.outcome = AYNI_WRITE_FAILED;
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
    return result;
}

} // namespace ayni::hosts::sim

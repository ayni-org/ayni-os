#include <algorithm>
#include <ayni/runtime/conversation/file_bodies.h>
#include <cstring>
namespace ayni::runtime {
namespace {
using hosts::storage::file_value_bytes_max;
using hosts::storage::FileValueRequest;
using hosts::storage::FileValueResult;
bool same(const std::string &stored, ayni_bytes_v1 body) {
    return stored.size() == body.size &&
           (!body.size || std::memcmp(stored.data(), body.data, body.size) == 0);
}
} // namespace
ayni_status_t FileBodies::configure(hosts::storage::FileValuePort files, uint64_t base) {
    if (!files.submit || !base || base > UINT64_MAX - chunks(UINT32_MAX)) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_ || !entries_.empty() || !removals_.empty()) {
        return AYNI_INVALID_STATE;
    }
    files_ = files;
    base_ = base;
    owner_ = std::this_thread::get_id();
    return AYNI_OK;
}
uint64_t FileBodies::identity(uint64_t scope, uint64_t digest) {
    auto mixed = digest ^ (scope * UINT64_C(0x9e3779b97f4a7c15));
    mixed ^= mixed >> 29U;
    return mixed ? mixed : 1U;
}
uint32_t FileBodies::chunks(uint32_t size) {
    return size ? (size - 1U) / file_value_bytes_max + 1U : 1U;
}
void FileBodies::wait(uint64_t id) {
    if (std::find(waiting_.begin(), waiting_.end(), id) == waiting_.end()) {
        waiting_.push_back(id);
    }
}
bool FileBodies::removing(uint64_t id) const {
    return std::any_of(removals_.begin(), removals_.end(),
                       [id](const auto &key) { return key.object_id == id; });
}
FileBodies::Pending *FileBodies::prepare(FileValueRequest request, uint64_t id, uint32_t chunk,
                                         uint32_t expected) {
    const bool removal = request.kind == FileValueRequest::Kind::remove;
    request.key = {base_ + chunk, id};
    request.complete = complete;
    pending_.push_back({this, id, chunk, expected, removal, request});
    auto *pending = &pending_.back();
    pending->request.context = pending;
    ++outstanding_;
    if (!removal) {
        ++entries_[id].outstanding;
    }
    return pending;
}
void FileBodies::flush() {
    if (std::this_thread::get_id() != owner_) {
        return;
    }
    std::vector<Pending *> requests;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Removals go first, so a body written again after its release keeps its new files.
        for (const auto &key : removals_) {
            FileValueRequest request;
            request.kind = FileValueRequest::Kind::remove;
            requests.push_back(prepare(request, key.object_id,
                                       static_cast<uint32_t>(key.owner_instance_id - base_), 0));
        }
        removals_.clear();
        for (const auto id : waiting_) {
            const auto found = entries_.find(id);
            if (found == entries_.end() || found->second.released ||
                (found->second.state != State::writing && found->second.state != State::loading)) {
                continue;
            }
            auto &entry = found->second;
            // The entry is not erased while it has outstanding requests, so its bytes stay put.
            auto *bytes = reinterpret_cast<uint8_t *>(entry.bytes.data());
            const auto size = static_cast<uint32_t>(entry.bytes.size());
            for (const auto count = chunks(size); entry.submitted < count; ++entry.submitted) {
                FileValueRequest request;
                const auto at = entry.submitted * file_value_bytes_max;
                request.size = std::min(file_value_bytes_max, size - at);
                uint32_t expected = 0;
                if (entry.state == State::writing) {
                    request.kind = FileValueRequest::Kind::write;
                    request.previous_generation = 0; // Create once; identical files confirm.
                    request.generation = 1;
                    request.data = size ? bytes + at : nullptr;
                } else {
                    request.kind = FileValueRequest::Kind::read;
                    request.buffer = size ? bytes + at : nullptr;
                    expected = request.size;
                }
                requests.push_back(prepare(request, id, entry.submitted, expected));
            }
        }
        waiting_.clear();
    }
    for (std::size_t at = 0; at < requests.size(); ++at) {
        if (files_.submit(files_.context, &requests[at]->request) != AYNI_SUBMIT_ACCEPTED) {
            withdraw(requests, at); // The queue keeps its order, so later requests wait too.
            return;
        }
    }
}
void FileBodies::withdraw(const std::vector<Pending *> &requests, std::size_t from) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto at = from; at < requests.size(); ++at) {
        auto *pending = requests[at];
        --outstanding_;
        if (pending->removal) {
            removals_.push_back(pending->request.key);
        } else if (const auto found = entries_.find(pending->id); found != entries_.end()) {
            auto &entry = found->second;
            --entry.outstanding;
            entry.submitted = std::min(entry.submitted, pending->chunk);
            if (entry.released && !entry.outstanding) {
                entries_.erase(found);
            } else {
                wait(pending->id);
            }
        }
        pending_.remove_if([pending](const Pending &item) { return &item == pending; });
    }
}
ayni_status_t FileBodies::write(void *context, uint64_t scope, ayni_bytes_v1 body,
                                ayni_body_ref_v1 *out) {
    if (!context || !out || (body.size && !body.data)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<FileBodies *>(context);
    const auto digest = ayni_body_digest(body);
    const auto id = identity(scope, digest);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.files_.submit) {
            return AYNI_INVALID_STATE;
        }
        const auto found = self.entries_.find(id);
        if (found == self.entries_.end() || found->second.state == State::evicted) {
            if (self.removing(id)) {
                return AYNI_NOT_READY; // Its old files are removed first.
            }
            if (found != self.entries_.end() &&
                (found->second.digest != digest || found->second.size != body.size)) {
                return AYNI_STORAGE_FAILED;
            }
            const auto room = self.reserve_cache(body.size);
            if (room != AYNI_OK) {
                return room;
            }
            auto &entry = self.entries_[id];
            entry.scope = scope;
            entry.digest = digest;
            entry.size = body.size;
            entry.state = State::writing;
            entry.submitted = 0;
            entry.bytes.assign(reinterpret_cast<const char *>(body.data), body.size);
            self.wait(id);
        } else {
            auto &entry = found->second;
            if (entry.released) {
                return AYNI_NOT_READY; // Erased once its requests finish, then written anew.
            }
            if (entry.state == State::failed) {
                if (!entry.outstanding) {
                    self.entries_.erase(found); // A later write tries again.
                }
                return AYNI_STORAGE_FAILED;
            }
            if (entry.state != State::loading) {
                if (entry.digest != digest || !same(entry.bytes, body)) {
                    return AYNI_STORAGE_FAILED; // Identity collision: fail closed, never alias.
                }
                entry.scope = entry.scope ? entry.scope : scope;
                if (entry.state == State::ready || entry.state == State::durable) {
                    entry.state = State::durable; // This caller has consumed the durable result.
                    *out = {id, digest, body.size, 0};
                    return AYNI_OK;
                }
            }
            self.wait(id);
        }
    }
    self.flush();
    return AYNI_NOT_READY;
}
ayni_status_t FileBodies::read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                               uint32_t capacity) {
    if (!context || !ref || !ref->id || (capacity && !out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.files_.submit) {
            return AYNI_INVALID_STATE;
        }
        const auto found = self.entries_.find(ref->id);
        if (found != self.entries_.end() &&
            (found->second.digest != ref->digest || found->second.size != ref->size)) {
            return AYNI_STORAGE_FAILED;
        }
        if (found == self.entries_.end() || found->second.state == State::evicted) {
            if (self.removing(ref->id)) {
                return AYNI_NOT_FOUND;
            }
            const auto room = self.reserve_cache(ref->size);
            if (room != AYNI_OK) {
                return room;
            }
            auto &entry = self.entries_[ref->id];
            entry.digest = ref->digest;
            entry.size = ref->size;
            entry.state = State::loading;
            entry.submitted = 0;
            entry.bytes.assign(ref->size, '\0');
            self.wait(ref->id);
        } else {
            auto &entry = found->second;
            if (entry.released) {
                return AYNI_NOT_FOUND;
            }
            if (entry.state == State::failed) {
                if (entry.outstanding) {
                    return AYNI_NOT_READY;
                }
                const auto failure = entry.failure;
                self.entries_.erase(found); // A later read loads again.
                return failure;
            }
            if (entry.state != State::loading) {
                if (entry.bytes.size() > capacity) {
                    return AYNI_BUFFER_TOO_SMALL;
                }
                if (!entry.bytes.empty()) {
                    std::memcpy(out, entry.bytes.data(), entry.bytes.size());
                }
                if (entry.state == State::ready) {
                    entry.state = State::durable; // The loaded result can now be reclaimed.
                }
                return AYNI_OK;
            }
            self.wait(ref->id);
        }
    }
    self.flush();
    return AYNI_NOT_READY;
}
void FileBodies::release(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !ref || !ref->id) {
        return;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto found = self.entries_.find(ref->id);
        if (found != self.entries_.end()) {
            if (found->second.outstanding) {
                found->second.released = true;
            } else {
                self.entries_.erase(found);
            }
        }
        // Submitted after any accepted write of the same body, so its files end up removed.
        for (uint32_t chunk = 0, count = chunks(ref->size); chunk < count; ++chunk) {
            self.removals_.push_back({self.base_ + chunk, ref->id});
        }
    }
    self.flush();
}
void FileBodies::collect(uint64_t scope, const std::vector<ayni_body_ref_v1> &keep) {
    std::vector<ayni_body_ref_v1> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[id, entry] : entries_) {
            bool kept = entry.scope != scope || entry.released;
            for (const auto &ref : keep) {
                kept = kept || ref.id == id;
            }
            if (!kept) {
                released.push_back({id, entry.digest, entry.size, 0});
            }
        }
    }
    for (const auto &ref : released) {
        release(this, &ref);
    }
}
uint32_t FileBodies::ready(void *context, uint64_t) {
    auto &self = *static_cast<FileBodies *>(context);
    self.flush();
    std::lock_guard<std::mutex> lock(self.mutex_);
    return self.outstanding_ || !self.waiting_.empty() || !self.removals_.empty() ? 0U : 1U;
}
size_t FileBodies::cached_bytes_locked() const {
    size_t total = 0;
    for (const auto &item : entries_) {
        total += item.second.bytes.size();
    }
    return total;
}
size_t FileBodies::cached_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_bytes_locked();
}
// All descriptor reads copy bytes, so only file requests borrow cached storage. Deterministic
// map order suffices for reclamation. Keep ownership metadata for scope collection even
// after the cached payload is gone (C-HVAL-005). Only explicit trim may abandon a ready
// result, so cancellation can reclaim it without enabling automatic retry/eviction loops.
ayni_status_t FileBodies::trim_locked(size_t target, Reclaim reclaim) {
    auto bytes = cached_bytes_locked();
    for (auto at = entries_.begin(); bytes > target && at != entries_.end(); ++at) {
        auto &entry = at->second;
        const bool reclaimable = entry.state == State::durable ||
                                 (reclaim == Reclaim::completed && entry.state == State::ready);
        if (reclaimable && !entry.outstanding) {
            bytes -= entry.bytes.size();
            std::string{}.swap(entry.bytes);
            entry.state = State::evicted;
        }
    }
    return bytes <= target ? AYNI_OK : AYNI_BUSY;
}
ayni_status_t FileBodies::trim_cache(size_t target) {
    std::lock_guard<std::mutex> lock(mutex_);
    return trim_locked(target, Reclaim::completed);
}
ayni_status_t FileBodies::set_cache_limit(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes) {
        const auto status = trim_locked(bytes, Reclaim::consumed);
        if (status != AYNI_OK) {
            return status;
        }
    }
    cache_limit_ = bytes;
    return AYNI_OK;
}
ayni_status_t FileBodies::reserve_cache(size_t bytes) {
    if (!cache_limit_) {
        return AYNI_OK;
    }
    if (bytes > cache_limit_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto status = trim_locked(cache_limit_ - bytes, Reclaim::consumed);
    return status == AYNI_OK ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
void FileBodies::complete(void *context, const FileValueResult &result) noexcept {
    auto *pending = static_cast<Pending *>(context);
    auto &self = *pending->self;
    std::lock_guard<std::mutex> lock(self.mutex_);
    --self.outstanding_;
    const auto found = pending->removal ? self.entries_.end() : self.entries_.find(pending->id);
    if (found != self.entries_.end()) {
        auto &entry = found->second;
        --entry.outstanding;
        const bool loading = entry.state == State::loading;
        if ((loading || entry.state == State::writing) &&
            (result.status != AYNI_OK || (loading && result.size != pending->expected))) {
            entry.state = State::failed;
            entry.failure =
                loading && result.status == AYNI_NOT_FOUND ? AYNI_NOT_FOUND : AYNI_STORAGE_FAILED;
        }
        const auto size = static_cast<uint32_t>(entry.bytes.size());
        if (entry.released && !entry.outstanding) {
            self.entries_.erase(found);
        } else if (!entry.outstanding && entry.submitted == chunks(size)) {
            const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(entry.bytes.data()), size};
            if (entry.state == State::writing) {
                entry.state = State::ready;
            } else if (entry.state == State::loading) {
                const bool verified = ayni_body_digest(bytes) == entry.digest;
                entry.state = verified ? State::ready : State::failed;
                entry.failure = verified ? AYNI_OK : AYNI_STORAGE_FAILED;
            }
        }
    }
    self.pending_.remove_if([pending](const Pending &item) { return &item == pending; });
}
} // namespace ayni::runtime

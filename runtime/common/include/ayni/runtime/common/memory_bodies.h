// In-memory body store (ADR-0096) for composition roots without a durable body backend:
// simulations, examples and tests. Writes are durable for the process lifetime as soon as they
// return, so write never answers NOT_READY. Thread-safe; the C descriptor borrows this object.
#ifndef AYNI_RUNTIME_COMMON_MEMORY_BODIES_H
#define AYNI_RUNTIME_COMMON_MEMORY_BODIES_H
#include <ayni/bodies.h>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ayni::runtime {
class MemoryBodies {
  public:
    MemoryBodies() = default;
    MemoryBodies(const MemoryBodies &) = delete;
    MemoryBodies &operator=(const MemoryBodies &) = delete;
    const ayni_body_store_v1 *store() const { return &store_; }
    // Deletes every body of scope that keep does not name.
    void collect(uint64_t scope, const std::vector<ayni_body_ref_v1> &keep) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto at = bodies_.begin(); at != bodies_.end();) {
            bool kept = at->second.scope != scope;
            for (const auto &ref : keep) {
                kept = kept || ref.id == at->first;
            }
            at = kept ? std::next(at) : bodies_.erase(at);
        }
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bodies_.size();
    }
    bool contains(const ayni_body_ref_v1 &ref) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bodies_.count(ref.id) != 0U;
    }

  private:
    struct Body {
        uint64_t scope = 0;
        uint64_t digest = 0;
        std::string bytes;
    };
    static ayni_status_t write(void *context, uint64_t scope, ayni_bytes_v1 body,
                               ayni_body_ref_v1 *out) {
        if (!context || !out || (body.size && !body.data)) {
            return AYNI_INVALID_ARGUMENT;
        }
        auto &self = *static_cast<MemoryBodies *>(context);
        const auto digest = ayni_body_digest(body);
        const std::string bytes(reinterpret_cast<const char *>(body.data), body.size);
        std::lock_guard<std::mutex> lock(self.mutex_);
        for (const auto &[id, stored] : self.bodies_) {
            if (stored.scope == scope && stored.digest == digest && stored.bytes == bytes) {
                *out = {id, digest, body.size, 0};
                return AYNI_OK;
            }
        }
        const auto id = ++self.next_;
        self.bodies_[id] = {scope, digest, bytes};
        *out = {id, digest, body.size, 0};
        return AYNI_OK;
    }
    static ayni_status_t read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                              uint32_t capacity) {
        if (!context || !ref || !ref->id || (capacity && !out)) {
            return AYNI_INVALID_ARGUMENT;
        }
        auto &self = *static_cast<MemoryBodies *>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto found = self.bodies_.find(ref->id);
        if (found == self.bodies_.end()) {
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
    static void release(void *context, const ayni_body_ref_v1 *ref) {
        if (!context || !ref) {
            return;
        }
        auto &self = *static_cast<MemoryBodies *>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.bodies_.erase(ref->id);
    }
    mutable std::mutex mutex_;
    std::map<uint64_t, Body> bodies_;
    uint64_t next_ = 0;
    ayni_body_store_v1 store_{sizeof(ayni_body_store_v1), 0, this, write, read, release};
};
} // namespace ayni::runtime
#endif

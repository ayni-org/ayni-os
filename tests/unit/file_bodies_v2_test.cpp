// ADR-0113: portable FileBodies AYNB format 2 verification:
// 1. Small payloads (<= 4096 B and 0 B) store inline inside the manifest file (1 write op,
//    0 chunk files, 1 cold read op, 1 remove op).
// 2. Optimistic previous_generation=0 create falls back cleanly to manifest_read + merge on
//    AYNI_STALE when an identical digest already exists on disk from another owner/boot.
// 3. Evicted inline bodies support cold reload and partial-release manifest rewrite under tight
//    cache budgets without CAPACITY_EXCEEDED.
// 4. Large payloads (> 4096 B) keep chunked storage, trim/reload, and clean deletion.
// 5. Legacy AYNB format 1 files are rejected with AYNI_UNSUPPORTED_VERSION and left untouched.

#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace ayni::hosts::storage;
using ayni::runtime::FileBodies;

class InMemoryFiles final : public Files {
  public:
    int begin(const char *) override { return 0; }
    void end() override { handles_.clear(); }
    int open(const char *name, Mode mode, int &handle) override {
        const std::string key(name);
        auto it = files_.find(key);
        if (mode == Mode::read_existing || mode == Mode::read_existing_no_follow) {
            if (it == files_.end()) {
                return ENOENT;
            }
        } else if (mode == Mode::replace) {
            files_[key].clear();
        } else {
            files_[key];
        }
        handle = next_handle_++;
        handles_[handle] = key;
        return 0;
    }
    void close(int handle) override { handles_.erase(handle); }
    int lock(int) override { return 0; }
    int read(int handle, uint64_t offset, uint8_t *data, std::size_t bytes,
             std::size_t &count) const override {
        count = 0;
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        const auto f = files_.find(h->second);
        if (f == files_.end() || offset >= f->second.size()) {
            return 0;
        }
        count = std::min<std::size_t>(bytes, f->second.size() - static_cast<std::size_t>(offset));
        std::memcpy(data, f->second.data() + offset, count);
        return 0;
    }
    int write(int handle, uint64_t offset, const uint8_t *data, std::size_t bytes) override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        auto &buf = files_[h->second];
        const auto end = static_cast<std::size_t>(offset) + bytes;
        if (buf.size() < end) {
            buf.resize(end);
        }
        if (bytes != 0U) {
            std::memcpy(buf.data() + offset, data, bytes);
        }
        return 0;
    }
    int size(int handle, uint64_t &bytes) const override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        const auto f = files_.find(h->second);
        bytes = f == files_.end() ? 0 : f->second.size();
        return 0;
    }
    int truncate(int handle, uint64_t bytes) override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        files_[h->second].resize(static_cast<std::size_t>(bytes));
        return 0;
    }
    int sync_file(int) override { return 0; }
    int sync_namespace() override { return 0; }
    int rename(const char *from, const char *to) override {
        auto it = files_.find(from);
        if (it == files_.end()) {
            return ENOENT;
        }
        files_[to] = std::move(it->second);
        files_.erase(it);
        return 0;
    }
    int remove(const char *name) override { return files_.erase(name) ? 0 : ENOENT; }
    int free_bytes(uint64_t &bytes) const override {
        bytes = 4U * 1024U * 1024U;
        return 0;
    }
    std::size_t file_count() const { return files_.size(); }
    const std::map<std::string, std::vector<uint8_t>> &snapshot() const { return files_; }

  private:
    std::map<std::string, std::vector<uint8_t>> files_;
    std::map<int, std::string> handles_;
    int next_handle_ = 1;
};

struct CountingFilePort {
    InMemoryFiles files;
    std::deque<FileValueRequest> queue;
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint32_t removes = 0;
    uint32_t chunk_ops = 0;

    FileValuePort port() { return {this, submit}; }

    void reset_counts() { reads = writes = removes = chunk_ops = 0; }

    static ayni_submit_result_t submit(void *ctx, const FileValueRequest *req) {
        auto &self = *static_cast<CountingFilePort *>(ctx);
        if (req->kind == FileValueRequest::Kind::read) {
            ++self.reads;
        } else if (req->kind == FileValueRequest::Kind::write) {
            ++self.writes;
        } else {
            ++self.removes;
        }
        if (req->role == FileValueRole::body_chunk) {
            ++self.chunk_ops;
        }
        self.queue.push_back(*req);
        return AYNI_SUBMIT_ACCEPTED;
    }

    void advance_one() {
        if (queue.empty()) {
            return;
        }
        const auto req = queue.front();
        queue.pop_front();
        const auto res = serve_file_value(files, req);
        req.complete(req.context, res);
    }

    template <class Done> bool until(FileBodies &bodies, Done done) {
        for (uint32_t step = 0; step < 5000; ++step) {
            if (done()) {
                return true;
            }
            FileBodies::progress(&bodies, 0);
            advance_one();
        }
        return done();
    }
};

void append_le(std::string &out, uint64_t value, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back(static_cast<char>(value >> (i * 8U)));
    }
}

uint64_t body_identity(uint64_t scope, uint64_t digest) {
    auto mixed = digest ^ (scope * UINT64_C(0x9e3779b97f4a7c15));
    mixed ^= mixed >> 29U;
    return mixed ? mixed : 1U;
}

// ADR-0113: small payloads (<= 4096 B and 0 B) persist in 1 FileValue write (optimistic create,
// 0 reads, 0 chunk files), cold-read in 1 op, and delete in 1 op leaving 0 files.
TEST(FileBodiesV2, inline_small_and_empty_bodies_use_single_op_write_read_and_remove) {
    CountingFilePort storage;
    FileBodies writer;
    ASSERT_EQ(writer.configure(storage.port(), 65536), AYNI_OK);
    const auto *store = writer.store();

    for (const std::string &payload :
         {std::string("hello AYNB v2"), std::string(4096, 'z'), std::string("")}) {
        storage.reset_counts();
        ayni_body_ref_v1 ref{};
        ayni_status_t status = AYNI_NOT_READY;
        ASSERT_EQ(store->stage(store->context, 11,
                               {payload.empty() ? nullptr
                                                : reinterpret_cast<const uint8_t *>(payload.data()),
                                static_cast<uint32_t>(payload.size())},
                               &ref),
                  AYNI_OK);
        ASSERT_TRUE(storage.until(writer, [&] {
            return (status = store->confirm(store->context, &ref)) != AYNI_NOT_READY;
        }));
        ASSERT_EQ(status, AYNI_OK);
        EXPECT_EQ(storage.writes, 1U);
        EXPECT_EQ(storage.reads, 0U);
        EXPECT_EQ(storage.chunk_ops, 0U);
        EXPECT_EQ(storage.files.file_count(), 1U);

        // Cold reopen reads payload in 1 FileValue read op.
        storage.reset_counts();
        FileBodies reader;
        ASSERT_EQ(reader.configure(storage.port(), 65536), AYNI_OK);
        const auto *rstore = reader.store();
        std::string loaded(payload.size(), '\0');
        ASSERT_TRUE(storage.until(reader, [&] {
            return (status = rstore->read(
                        rstore->context, &ref,
                        loaded.empty() ? nullptr : reinterpret_cast<uint8_t *>(loaded.data()),
                        static_cast<uint32_t>(loaded.size()))) != AYNI_NOT_READY;
        }));
        ASSERT_EQ(status, AYNI_OK);
        EXPECT_EQ(loaded, payload);
        EXPECT_EQ(storage.reads, 1U);
        EXPECT_EQ(storage.chunk_ops, 0U);

        // Single-op remove deletes the inline manifest and leaves zero files.
        storage.reset_counts();
        store->release(store->context, &ref);
        ASSERT_TRUE(storage.until(writer, [&] { return writer.idle(); }));
        EXPECT_EQ(storage.removes, 1U);
        EXPECT_EQ(storage.chunk_ops, 0U);
        EXPECT_EQ(storage.files.file_count(), 0U);
    }
}

// C-HVAL-009: a new RAM alias of an already saved tool result waits for confirmation.
TEST(FileBodiesV2, staging_a_shared_owner_does_not_rewrite_the_manifest) {
    CountingFilePort storage;
    FileBodies bodies;
    ASSERT_EQ(bodies.configure(storage.port(), 1024), AYNI_OK);
    const auto *store = bodies.store();
    const std::string payload = "tool result already owned by Core";
    const ayni_bytes_v1 view{reinterpret_cast<const uint8_t *>(payload.data()),
                             static_cast<uint32_t>(payload.size())};
    ayni_body_ref_v1 core{}, chat{};
    ASSERT_EQ(store->stage(store->context, 1, view, &core), AYNI_OK);
    ASSERT_TRUE(
        storage.until(bodies, [&] { return store->confirm(store->context, &core) == AYNI_OK; }));
    storage.reset_counts();

    ASSERT_EQ(store->share(store->context, 2, &core, &chat), AYNI_OK);
    EXPECT_TRUE(bodies.idle());
    FileBodies::progress(&bodies, 0);
    EXPECT_TRUE(storage.queue.empty());
    EXPECT_EQ(storage.writes, 0U);

    ASSERT_TRUE(
        storage.until(bodies, [&] { return store->confirm(store->context, &chat) == AYNI_OK; }));
    EXPECT_EQ(storage.writes, 1U);
}

// ADR-0113: optimistic previous_generation=0 create encountering AYNI_STALE falls back cleanly
// to manifest_read + merge + CAS manifest_write, retaining both independent owners.
TEST(FileBodiesV2, optimistic_create_recovers_on_stale_and_evicted_partial_release_rewrites) {
    CountingFilePort storage;
    FileBodies first;
    ASSERT_EQ(first.configure(storage.port(), 1024), AYNI_OK);
    const std::string text = "shared across boots";
    const ayni_bytes_v1 view{reinterpret_cast<const uint8_t *>(text.data()),
                             static_cast<uint32_t>(text.size())};

    ayni_body_ref_v1 ref1{};
    ayni_status_t status = AYNI_NOT_READY;
    ASSERT_EQ(first.store()->stage(first.store()->context, 1, view, &ref1), AYNI_OK);
    ASSERT_TRUE(storage.until(first, [&] {
        return (status = first.store()->confirm(first.store()->context, &ref1)) != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);

    // Second instance stages the same bytes under scope 2: optimistic gen=0->1 hits AYNI_STALE,
    // falls back to manifest_read, merges owners {1, 2}, and writes gen=1->2.
    FileBodies second;
    ASSERT_EQ(second.configure(storage.port(), 1024), AYNI_OK);
    ayni_body_ref_v1 ref2{};
    ASSERT_EQ(second.store()->stage(second.store()->context, 2, view, &ref2), AYNI_OK);
    ASSERT_TRUE(storage.until(second, [&] {
        return (status = second.store()->confirm(second.store()->context, &ref2)) != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(FileBodies::status(&second), AYNI_OK);

    // Evict inline payload and shrink cache below payload size, then release scope 1:
    // transient inline staging rewrites the manifest without hitting CAPACITY_EXCEEDED.
    ASSERT_EQ(second.trim_cache(0), AYNI_OK);
    ASSERT_EQ(second.set_cache_limit(8), AYNI_OK);
    second.store()->release(second.store()->context, &ref1);
    ASSERT_TRUE(storage.until(second, [&] { return second.idle(); }));
    EXPECT_EQ(FileBodies::status(&second), AYNI_OK);

    // Reopen verifies scope 1 is gone while scope 2 still reads exact bytes.
    FileBodies verify;
    ASSERT_EQ(verify.configure(storage.port(), 1024), AYNI_OK);
    std::string out(text.size(), '\0');
    ASSERT_TRUE(storage.until(verify, [&] {
        return (status = verify.store()->read(verify.store()->context, &ref2,
                                              reinterpret_cast<uint8_t *>(out.data()),
                                              static_cast<uint32_t>(out.size()))) != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(out, text);
    EXPECT_EQ(verify.store()->read(verify.store()->context, &ref1,
                                   reinterpret_cast<uint8_t *>(out.data()),
                                   static_cast<uint32_t>(out.size())),
              AYNI_NOT_FOUND);
}

// ADR-0113: bodies > 4096 B retain 4 KiB chunk files, cold reload, and clean removal.
TEST(FileBodiesV2, large_chunked_body_round_trips_and_deletes_all_chunks) {
    CountingFilePort storage;
    FileBodies bodies;
    ASSERT_EQ(bodies.configure(storage.port(), 32768), AYNI_OK);
    const std::string big(9000, 'L');
    ayni_body_ref_v1 ref{};
    ayni_status_t status = AYNI_NOT_READY;
    ASSERT_EQ(bodies.store()->stage(bodies.store()->context, 5,
                                    {reinterpret_cast<const uint8_t *>(big.data()),
                                     static_cast<uint32_t>(big.size())},
                                    &ref),
              AYNI_OK);
    ASSERT_TRUE(storage.until(bodies, [&] {
        return (status = bodies.store()->confirm(bodies.store()->context, &ref)) != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(storage.files.file_count(), 4U); // 1 manifest + 3 chunks (4096 + 4096 + 808).
    EXPECT_GT(storage.chunk_ops, 0U);

    ASSERT_EQ(bodies.trim_cache(0), AYNI_OK);
    std::string restored(big.size(), '\0');
    ASSERT_TRUE(storage.until(bodies, [&] {
        return (status = bodies.store()->read(
                    bodies.store()->context, &ref, reinterpret_cast<uint8_t *>(restored.data()),
                    static_cast<uint32_t>(restored.size()))) != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(restored, big);

    bodies.store()->release(bodies.store()->context, &ref);
    ASSERT_TRUE(storage.until(bodies, [&] { return bodies.idle(); }));
    EXPECT_EQ(storage.files.file_count(), 0U);
}

// ADR-0113: legacy AYNB format 1 manifests are explicitly rejected with AYNI_UNSUPPORTED_VERSION
// and preserved untouched on storage (no legacy reader or silent overwrite).
TEST(FileBodiesV2, legacy_aynb_v1_manifest_is_rejected_and_preserved_untouched) {
    CountingFilePort storage;
    const std::string payload = "legacy v1 payload";
    const ayni_bytes_v1 view{reinterpret_cast<const uint8_t *>(payload.data()),
                             static_cast<uint32_t>(payload.size())};
    const uint64_t digest = ayni_body_digest(view);
    const uint64_t content_id = body_identity(0, digest);
    const uint64_t owner_id = body_identity(1, digest);

    // Synthesize an exact AYNB format 1 manifest (32 B header + 16 B owner).
    std::string v1_manifest;
    append_le(v1_manifest, 0x424e5941u, 4); // AYNB
    append_le(v1_manifest, 1u, 4);          // legacy format = 1
    append_le(v1_manifest, payload.size(), 4);
    append_le(v1_manifest, 1u, 4);
    append_le(v1_manifest, digest, 8);
    append_le(v1_manifest, 0u, 8);
    append_le(v1_manifest, 1u, 8);
    append_le(v1_manifest, owner_id, 8);

    FileValueRequest seed{};
    seed.kind = FileValueRequest::Kind::write;
    seed.key = {FileBodies::default_base, content_id};
    seed.previous_generation = 0;
    seed.generation = 1;
    seed.data = reinterpret_cast<const uint8_t *>(v1_manifest.data());
    seed.size = static_cast<uint32_t>(v1_manifest.size());
    seed.complete = [](void *, const FileValueResult &) noexcept {};
    ASSERT_EQ(serve_file_value(storage.files, seed).status, AYNI_OK);
    const auto before = storage.files.snapshot();

    FileBodies bodies;
    ASSERT_EQ(bodies.configure(storage.port(), 4096), AYNI_OK);
    const ayni_body_ref_v1 ref{owner_id, digest, static_cast<uint32_t>(payload.size()), 0};
    std::string out(payload.size(), '\0');
    ayni_status_t status = AYNI_NOT_READY;
    ASSERT_TRUE(storage.until(bodies, [&] {
        return (status = bodies.store()->read(bodies.store()->context, &ref,
                                              reinterpret_cast<uint8_t *>(out.data()),
                                              static_cast<uint32_t>(out.size()))) != AYNI_NOT_READY;
    }));
    EXPECT_EQ(status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(FileBodies::status(&bodies), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(storage.files.snapshot(), before); // Old file preserved byte-for-byte.
}

} // namespace

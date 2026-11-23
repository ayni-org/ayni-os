#include <algorithm>
#include <ayni/runtime/conversation/file_bodies.h>
#include <cstring>
#include <iterator>
namespace ayni::runtime {
namespace {
using hosts::storage::FileValueRequest;
using hosts::storage::FileValueResult;
constexpr uint32_t byte_bits = 8;
constexpr uint32_t word_bytes = 4, wide_bytes = 8;
void integer(std::string &out, uint64_t value, uint32_t count) {
    for (uint32_t at = 0; at < count; ++at) {
        out.push_back(static_cast<char>(value));
        value >>= byte_bits;
    }
}
uint64_t take(const std::string &in, size_t &at, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t byte = 0; byte < count; ++byte) {
        value |= uint64_t{static_cast<unsigned char>(in[at++])} << (byte * byte_bits);
    }
    return value;
}
} // namespace
ayni_status_t FileBodies::configure(hosts::storage::FileValuePort files, size_t cache_bytes,
                                    uint64_t base) {
    if (!files.submit || !cache_bytes || !base || base > UINT64_MAX - chunks(UINT32_MAX) - 1U) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entries_.empty() || !pending_.empty()) {
        return AYNI_INVALID_STATE;
    }
    files_ = files;
    cache_limit_ = cache_bytes;
    base_ = base;
    owner_ = std::this_thread::get_id();
    scan_needed_ = true;
    background_writes_paused_ = false;
    return AYNI_OK;
}
uint64_t FileBodies::identity(uint64_t scope, uint64_t digest) {
    auto mixed = digest ^ (scope * UINT64_C(0x9e3779b97f4a7c15));
    constexpr uint32_t mix_shift = 29;
    mixed ^= mixed >> mix_shift;
    return mixed ? mixed : 1U;
}
uint32_t FileBodies::chunks(uint32_t size) {
    return size ? (size - 1U) / chunk_bytes + 1U : 1U;
}
bool FileBodies::valid_ref(const ayni_body_ref_v1 *ref) {
    return ref && ref->id && !ref->reserved;
}
bool FileBodies::owns(const Entry &entry, uint64_t id) {
    const auto has = [id](const auto &owner) { return owner.id == id; };
    return std::none_of(entry.releases.begin(), entry.releases.end(),
                        [id](const auto &release) { return release.id == id; }) &&
           (std::any_of(entry.owners.begin(), entry.owners.end(), has) ||
            std::any_of(entry.additions.begin(), entry.additions.end(), has));
}
bool FileBodies::conflicting(const ayni_body_ref_v1 &ref) const {
    const auto found = entries_.find(identity(0, ref.digest));
    return found != entries_.end() &&
           (found->second.digest != ref.digest || found->second.size != ref.size);
}
FileBodies::Entry *FileBodies::find(const ayni_body_ref_v1 &ref) {
    const auto found = entries_.find(identity(0, ref.digest));
    if (found == entries_.end() || found->second.digest != ref.digest ||
        found->second.size != ref.size) {
        return nullptr;
    }
    return &found->second;
}
FileBodies::Entry *FileBodies::metadata(const ayni_body_ref_v1 &ref) {
    auto *entry = find(ref);
    if (!entry && entries_.size() < entry_limit) {
        Entry pending;
        pending.digest = ref.digest;
        pending.size = ref.size;
        entry = &entries_.emplace(identity(0, ref.digest), std::move(pending)).first->second;
        scan_needed_ = true;
    }
    return entry;
}
ayni_status_t FileBodies::add_owner(Entry &entry, uint64_t scope, uint64_t id) {
    if (owns(entry, id)) {
        return AYNI_OK;
    }
    if (entry.deleting) {
        return AYNI_NOT_READY;
    }
    if (std::any_of(entry.releases.begin(), entry.releases.end(),
                    [id](const auto &release) { return release.id == id; })) {
        return AYNI_NOT_READY; // Finish the old alias's durable removal before recreating it.
    }
    auto &owners = entry.manifest_loaded ? entry.owners : entry.additions;
    if (owners.size() == owner_limit || entry.revision == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    owners.push_back({scope, id});
    entry.temporary.push_back(id);
    ++entry.revision;
    scan_needed_ = true;
    return AYNI_OK;
}
void FileBodies::drop_owner(Entry &entry, uint64_t id) {
    entry.temporary.erase(std::remove(entry.temporary.begin(), entry.temporary.end(), id),
                          entry.temporary.end());
    auto release = std::find_if(entry.releases.begin(), entry.releases.end(),
                                [id](const auto &item) { return item.id == id; });
    if (release == entry.releases.end()) {
        if (entry.manifest_loaded &&
            std::none_of(entry.owners.begin(), entry.owners.end(),
                         [id](const auto &owner) { return owner.id == id; })) {
            return; // Already-proved absence cannot acquire a new wait on unrelated revisions.
        }
        if (entry.releases.size() == owner_limit || entry.revision == UINT64_MAX) {
            failure_ = entry.failure = AYNI_CAPACITY_EXCEEDED;
            entry.state = State::failed;
            return; // A refused release must never be acknowledged as completed cleanup.
        }
        entry.releases.push_back({id, std::nullopt});
        scan_needed_ = true;
        release = std::prev(entry.releases.end());
    }
    if (entry.manifest_loaded && !release->revision) {
        scan_needed_ = true;
        apply_release(entry, *release);
        retire_releases(entry);
    }
}
bool FileBodies::apply_release(Entry &entry, Entry::Release &release) {
    const auto id = release.id;
    const auto owner = std::find_if(entry.owners.begin(), entry.owners.end(),
                                    [id](const auto &item) { return item.id == id; });
    if (owner != entry.owners.end()) {
        if (entry.revision == UINT64_MAX) {
            failure_ = entry.failure = AYNI_CAPACITY_EXCEEDED;
            entry.state = State::failed;
            return false;
        }
        entry.owners.erase(owner);
        ++entry.revision;
    }
    release.revision = entry.revision;
    const bool started = !entry.deleting && entry.owners.empty();
    entry.deleting = entry.owners.empty();
    if (started) {
        entry.chunk = 0;
    }
    return true;
}
void FileBodies::retire_releases(Entry &entry) {
    if (!entry.deleting) {
        entry.releases.erase(std::remove_if(entry.releases.begin(), entry.releases.end(),
                                            [&](const auto &release) {
                                                return release.revision &&
                                                       *release.revision <= entry.saved_revision;
                                            }),
                             entry.releases.end());
    } // Last-owner removal completes only after payload chunks and the manifest are deleted.
}
std::string FileBodies::encode_manifest(const Entry &entry) {
    std::string out;
    integer(out, manifest_magic, word_bytes);
    integer(out, manifest_format, word_bytes);
    integer(out, entry.size, word_bytes);
    integer(out, entry.owners.size(), word_bytes);
    integer(out, entry.digest, wide_bytes);
    integer(out, 0, wide_bytes);
    for (const auto &owner : entry.owners) {
        integer(out, owner.scope, wide_bytes);
        integer(out, owner.id, wide_bytes);
    }
    return out;
}
bool FileBodies::decode_manifest(Entry &entry, const std::string &bytes) {
    if (bytes.size() < manifest_header) {
        return false;
    }
    size_t at = 0;
    if (take(bytes, at, word_bytes) != manifest_magic ||
        take(bytes, at, word_bytes) != manifest_format ||
        take(bytes, at, word_bytes) != entry.size) {
        return false;
    }
    const auto count = take(bytes, at, word_bytes);
    if (!count || count > owner_limit || bytes.size() != manifest_header + count * owner_bytes ||
        take(bytes, at, wide_bytes) != entry.digest || take(bytes, at, wide_bytes)) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        const auto scope = take(bytes, at, wide_bytes), id = take(bytes, at, wide_bytes);
        if (id != identity(scope, entry.digest) ||
            std::any_of(entry.owners.begin(), entry.owners.end(),
                        [id](const auto &owner) { return owner.id == id; })) {
            return false;
        }
        entry.owners.push_back({scope, id});
    }
    return true;
}
// Stage accepts one immutable payload. All scope owners share those bytes and their files;
// manifest changes are confirmed separately, so an accepted tail can be used immediately.
ayni_status_t FileBodies::put(void *context, uint64_t scope, ayni_bytes_v1 bytes,
                              ayni_body_ref_v1 *out) {
    if (!context || !out || (bytes.size && !bytes.data)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<FileBodies *>(context);
    const auto digest = ayni_body_digest(bytes), id = identity(0, digest);
    ayni_status_t status = AYNI_OK;
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.files_.submit) {
            return AYNI_INVALID_STATE;
        }
        if (self.failure_ != AYNI_OK) {
            return self.failure_;
        }
        auto found = self.entries_.find(id);
        if (found != self.entries_.end() && found->second.state == State::failed &&
            found->second.failure == AYNI_NOT_FOUND && !found->second.staged) {
            self.entries_.erase(
                found); // A negative lookup owns no accepted bytes and cannot forbid recreation.
            found = self.entries_.end();
        }
        if (found == self.entries_.end()) {
            if (self.entries_.size() == entry_limit) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            const auto room = self.reserve_cache(bytes.size);
            if (room != AYNI_OK) {
                return room;
            }
            Entry entry;
            entry.digest = digest;
            entry.size = bytes.size;
            if (bytes.size) {
                entry.bytes.assign(reinterpret_cast<const char *>(bytes.data), bytes.size);
            }
            entry.staged = true;
            found = self.entries_.emplace(id, std::move(entry)).first;
        }
        auto &entry = found->second;
        if (entry.state == State::failed) {
            return entry.failure;
        }
        if (entry.deleting || entry.state == State::loading ||
            (entry.state == State::checking && !entry.staged)) {
            entry.awaited = true;
            self.scan_needed_ = true;
            status = AYNI_NOT_READY;
        } else if (entry.digest != digest || entry.size != bytes.size) {
            return AYNI_STORAGE_FAILED;
        } else {
            if (entry.state == State::evicted) {
                const auto room = self.reserve_cache(bytes.size);
                if (room != AYNI_OK) {
                    return room;
                }
                if (bytes.size) {
                    entry.bytes.assign(reinterpret_cast<const char *>(bytes.data), bytes.size);
                }
                entry.state = State::durable;
            }
            if (entry.bytes.size() != bytes.size ||
                (bytes.size && std::memcmp(entry.bytes.data(), bytes.data, bytes.size))) {
                return AYNI_STORAGE_FAILED; // Digest collision never aliases different local bytes.
            }
            const auto alias = identity(scope, digest);
            status = self.add_owner(entry, scope, alias);
            if (status == AYNI_OK) {
                *out = {alias, digest, bytes.size, 0};
            } else if (status == AYNI_NOT_READY) {
                entry.awaited = true;
                self.scan_needed_ = true;
            }
        }
    }
    if (status == AYNI_NOT_READY) {
        self.flush();
    }
    return status;
}
ayni_status_t FileBodies::stage(void *context, uint64_t scope, ayni_bytes_v1 body,
                                ayni_body_ref_v1 *out) {
    if (!context) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.files_.submit) {
            return AYNI_INVALID_STATE;
        }
    }
    return put(context, scope, body, out);
}
ayni_status_t FileBodies::write(void *context, uint64_t scope, ayni_bytes_v1 body,
                                ayni_body_ref_v1 *out) {
    if (!out) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_body_ref_v1 staged{};
    const auto status = put(context, scope, body, &staged);
    if (status != AYNI_OK) {
        return status;
    }
    const auto durable = confirm(context, &staged);
    if (durable == AYNI_NOT_READY) {
        auto &self = *static_cast<FileBodies *>(context);
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(self.mutex_);
            if (auto *entry = self.find(staged); entry && !entry->awaited) {
                entry->awaited = true;
                self.scan_needed_ = true;
                wake = true;
            }
        }
        if (wake) {
            self.flush();
        }
    }
    if (durable == AYNI_OK) {
        *out = staged;
    }
    return durable;
}
ayni_status_t FileBodies::share(void *context, uint64_t scope, const ayni_body_ref_v1 *ref,
                                ayni_body_ref_v1 *out) {
    if (!context || !valid_ref(ref) || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.failure_ != AYNI_OK) {
            return self.failure_;
        }
        if (self.conflicting(*ref)) {
            return AYNI_STORAGE_FAILED;
        }
        auto *entry = self.metadata(*ref);
        if (!entry) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        if (entry->state == State::failed) {
            return entry->failure;
        }
        if (entry->manifest_loaded || owns(*entry, ref->id)) {
            if (!owns(*entry, ref->id)) {
                return AYNI_NOT_FOUND;
            }
            const auto id = identity(scope, ref->digest);
            const auto status = self.add_owner(*entry, scope, id);
            if (status == AYNI_OK) {
                *out = {id, ref->digest, ref->size, 0};
                return AYNI_OK;
            }
            if (status != AYNI_NOT_READY) {
                return status;
            }
        }
        entry->awaited = true;
        self.scan_needed_ = true;
    }
    self.flush();
    return AYNI_NOT_READY;
}

ayni_status_t FileBodies::confirm(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !valid_ref(ref)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.conflicting(*ref)) {
            return AYNI_STORAGE_FAILED;
        }
        auto *entry = self.metadata(*ref);
        if (!entry) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        if (!entry->persist_requested) {
            entry->persist_requested = true;
            self.scan_needed_ = true;
        }
        entry->temporary.erase(
            std::remove(entry->temporary.begin(), entry->temporary.end(), ref->id),
            entry->temporary.end());
    }
    self.flush();
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (self.failure_ != AYNI_OK) {
        return self.failure_;
    }
    auto *entry = self.find(*ref);
    if (!entry) {
        return AYNI_NOT_FOUND;
    }
    if (entry->state == State::failed) {
        return entry->failure;
    }
    if (!owns(*entry, ref->id)) {
        return entry->manifest_loaded ? AYNI_NOT_FOUND : AYNI_NOT_READY;
    }
    if (entry->state == State::ready) {
        entry->state = State::durable;
    }
    return entry->manifest_loaded && entry->saved_revision == entry->revision &&
                   (entry->state == State::durable || entry->state == State::ready ||
                    entry->state == State::evicted)
               ? AYNI_OK
               : AYNI_NOT_READY;
}
// Reads of locally staged bytes are immediate. A reopened reference must first prove its
// durable ownership manifest; only then can the shared content be loaded and verified.
ayni_status_t FileBodies::read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                               uint32_t capacity) {
    if (!context || !valid_ref(ref) || (capacity && !out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (capacity < ref->size) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.conflicting(*ref)) {
            return AYNI_STORAGE_FAILED;
        }
        auto *entry = self.find(*ref);
        if (!entry) {
            if (!self.files_.submit) {
                return AYNI_INVALID_STATE;
            }
            if (self.entries_.size() == entry_limit) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            const auto room = self.reserve_cache(ref->size);
            if (room != AYNI_OK) {
                return room;
            }
            Entry loaded;
            loaded.digest = ref->digest;
            loaded.size = ref->size;
            loaded.load_payload = true;
            loaded.bytes.resize(
                ref->size); // Reserve the entire accepted load before later I/O can allocate.
            entry =
                &self.entries_.emplace(identity(0, ref->digest), std::move(loaded)).first->second;
            self.scan_needed_ = true;
        }
        if (entry->state == State::failed) {
            return entry->failure;
        }
        if (!entry->staged && !entry->load_payload && !entry->manifest_loaded) {
            const auto room = self.reserve_cache(entry->size);
            if (room != AYNI_OK) {
                return room;
            }
            entry->bytes.resize(entry->size);
            entry->load_payload = true;
            self.scan_needed_ = true;
        }
        if (entry->manifest_loaded && !owns(*entry, ref->id)) {
            return AYNI_NOT_FOUND;
        }
        if (entry->state == State::evicted) {
            const auto room = self.reserve_cache(entry->size);
            if (room != AYNI_OK) {
                return room;
            }
            entry->bytes.resize(entry->size);
            entry->state = State::loading;
            entry->chunk = 0;
            self.scan_needed_ = true;
        }
        if ((entry->staged && owns(*entry, ref->id)) || entry->state == State::ready ||
            entry->state == State::durable) {
            if (ref->size) {
                std::memcpy(out, entry->bytes.data(), ref->size);
            }
            if (entry->state == State::ready) {
                entry->state = State::durable;
            }
            return AYNI_OK;
        }
        entry->awaited = true;
        self.scan_needed_ = true;
    }
    self.flush();
    return AYNI_NOT_READY;
}
void FileBodies::release_temporary(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !valid_ref(ref)) {
        return;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        auto *entry = self.find(*ref);
        if (!entry || std::find(entry->temporary.begin(), entry->temporary.end(), ref->id) ==
                          entry->temporary.end()) {
            return;
        }
        if (!entry->manifest_loaded && !entry->pending && !entry->persist_requested &&
            entry->releases.empty() && !entry->deleting) {
            entry->additions.erase(
                std::remove_if(entry->additions.begin(), entry->additions.end(),
                               [&](const auto &owner) { return owner.id == ref->id; }),
                entry->additions.end());
            if (self.expired_.size() == entry_limit) {
                self.failure_ = AYNI_CAPACITY_EXCEEDED;
                return; // Preserve accepted content and report backpressure, never lose ownership.
            }
            self.expired_.insert(ref->id);
            entry->temporary.erase(
                std::remove(entry->temporary.begin(), entry->temporary.end(), ref->id),
                entry->temporary.end());
            if (entry->additions.empty()) {
                self.entries_.erase(identity(0, ref->digest));
            }
            return; // Removing RAM-only owners creates no persistence work.
        }
    }
    release(context, ref);
}
void FileBodies::release(void *context, const ayni_body_ref_v1 *ref) {
    if (!context || !valid_ref(ref)) {
        return;
    }
    auto &self = *static_cast<FileBodies *>(context);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.expired_.erase(ref->id)) {
            return;
        } // No file was ever created for this retired lease.
        if (self.conflicting(*ref)) {
            return;
        }
        auto *entry = self.metadata(*ref);
        if (entry) {
            self.drop_owner(*entry, ref->id);
        } else {
            self.failure_ = AYNI_CAPACITY_EXCEEDED;
        } // GC must not acknowledge a refused deletion.
    }
    self.flush();
}
void FileBodies::collect(uint64_t scope, const std::vector<ayni_body_ref_v1> &keep) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &[id, entry] : entries_) {
        const auto alias = identity(scope, entry.digest);
        if (owns(entry, alias) && std::none_of(keep.begin(), keep.end(), [alias](const auto &ref) {
                return ref.id == alias;
            })) {
            drop_owner(entry, alias);
        }
    }
}
// One outstanding 4 KiB quantum keeps cancellation and Core appends ahead of background
// payloads. No callback submits another request; the owner controls queue admission.
bool FileBodies::prepare(uint64_t id, Entry &entry) {
    if (entry.pending || entry.state == State::failed ||
        (entry.staged && !entry.persist_requested && !entry.deleting && entry.releases.empty())) {
        return false;
    }
    // Defer purely background persistence/cleanup during hot owner transitions while RAM
    // cache headroom remains, so Core commits do not queue behind non-blocking body writes.
    // Foreground waiters (awaited) and cache pressure above half the budget still advance.
    constexpr size_t pause_headroom_divisor = 2;
    if (background_writes_paused_ && !entry.awaited &&
        cached_bytes_locked() <= cache_limit_ / pause_headroom_divisor) {
        return false;
    }
    Pending pending;
    pending.self = this;
    pending.id = id;
    auto &request = pending.request;
    request.key = {base_, id};
    request.priority = FileValueRequest::Priority::background;
    request.complete = complete;
    using Role = hosts::storage::FileValueRole;
    request.role = Role::owner_manifest;
    if (!entry.manifest_loaded) {
        pending.operation = Operation::manifest_read;
        pending.buffer.resize(manifest_bytes);
    } else if (entry.deleting) {
        pending.operation = Operation::remove;
        request.kind = FileValueRequest::Kind::remove;
        if (entry.chunk < chunks(entry.size)) {
            request.role = Role::body_chunk;
            request.key.owner_instance_id += 1U + entry.chunk;
            request.logical_remove_bytes =
                std::min(chunk_bytes, entry.size - entry.chunk * chunk_bytes);
        } else {
            request.logical_remove_bytes = entry.saved_manifest_bytes;
        }
    } else if (entry.state == State::writing || entry.state == State::loading) {
        request.role = Role::body_chunk;
        const auto at = entry.chunk * chunk_bytes;
        request.size = std::min(chunk_bytes, entry.size - at);
        request.key.owner_instance_id += 1U + entry.chunk;
        if (entry.state == State::writing) {
            pending.operation = Operation::payload_write;
            request.kind = FileValueRequest::Kind::write;
            request.previous_generation = 0;
            request.generation = 1;
            request.data =
                request.size ? reinterpret_cast<const uint8_t *>(entry.bytes.data()) + at : nullptr;
        } else {
            pending.operation = Operation::payload_read;
            request.buffer =
                request.size ? reinterpret_cast<uint8_t *>(entry.bytes.data()) + at : nullptr;
        }
    } else if (entry.revision != entry.saved_revision) {
        if (entry.generation == UINT64_MAX) {
            entry.state = State::failed;
            failure_ = entry.failure = AYNI_CAPACITY_EXCEEDED;
            return false;
        }
        pending.operation = Operation::manifest_write;
        pending.revision = entry.revision;
        pending.buffer = encode_manifest(entry);
        request.kind = FileValueRequest::Kind::write;
        request.previous_generation = entry.generation;
        request.generation = entry.generation + 1U;
    } else {
        return false;
    }
    pending_.push_back(std::move(pending));
    auto &stored = pending_.back();
    stored.request.context = &stored;
    if (stored.operation == Operation::manifest_read) {
        stored.request.buffer = reinterpret_cast<uint8_t *>(stored.buffer.data());
        stored.request.size = static_cast<uint32_t>(stored.buffer.size());
    } else if (stored.operation == Operation::manifest_write) {
        stored.request.data = reinterpret_cast<const uint8_t *>(stored.buffer.data());
        stored.request.size = static_cast<uint32_t>(stored.buffer.size());
    }
    entry.pending = true;
    return true;
}
void FileBodies::flush() {
    if (std::this_thread::get_id() != owner_) {
        return;
    }
    Pending *request = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!scan_needed_ || !pending_.empty() || failure_ != AYNI_OK) {
            return;
        }
        for (auto &[id, entry] : entries_) {
            if (prepare(id, entry)) {
                request = &pending_.back();
                break;
            }
        }
        // A rejected submit retains work for the next owner tick.
        scan_needed_ = request != nullptr;
    }
    if (request && files_.submit(files_.context, &request->request) != AYNI_SUBMIT_ACCEPTED) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.at(request->id).pending = false;
        pending_.clear(); // Rejection retains neither buffers nor request; retry on owner progress.
    }
}
// Completion only updates the borrowed entry. An ownership update that raced the write stays
// dirty; it cannot confirm until a later CAS writes the new complete owner set.
void FileBodies::complete(void *context, const FileValueResult &result) noexcept {
    auto *pending = static_cast<Pending *>(context);
    auto &self = *pending->self;
    std::lock_guard<std::mutex> lock(self.mutex_);
    // This borrow may have exposed the next chunk, manifest update or deletion.
    self.scan_needed_ = true;
    auto &entry = self.entries_.at(pending->id);
    entry.pending = false;
    const bool missing =
        pending->operation == Operation::manifest_read && result.status == AYNI_NOT_FOUND;
    bool erased = false;
    if (result.status != AYNI_OK && !missing) {
        entry.state = State::failed;
        self.failure_ = entry.failure = result.status;
    } else if (pending->operation == Operation::manifest_read) {
        pending->buffer.resize(missing ? 0 : result.size);
        if (!missing && !self.decode_manifest(entry, pending->buffer)) {
            entry.state = State::failed;
            self.failure_ = entry.failure = AYNI_STORAGE_FAILED;
        } else if (missing && !entry.staged && entry.releases.empty()) {
            entry.state = State::failed;
            entry.failure = AYNI_NOT_FOUND;
            std::string{}.swap(
                entry.bytes); // Negative queries accepted no content and consume no quota.
        } else {
            entry.manifest_loaded = true;
            entry.generation = missing ? 0 : result.generation;
            entry.saved_manifest_bytes = missing ? 0 : result.size;
            // Merge local references into the durable owner set; repeated staging is idempotent.
            for (const auto &owner : entry.additions) {
                if (std::none_of(entry.owners.begin(), entry.owners.end(),
                                 [&](const auto &other) { return other.id == owner.id; })) {
                    entry.owners.push_back(owner);
                }
            }
            entry.additions.clear();
            if (entry.owners.size() > owner_limit) {
                entry.state = State::failed;
                self.failure_ = entry.failure = AYNI_CAPACITY_EXCEEDED;
            } else {
                for (auto &release : entry.releases) {
                    if (!self.apply_release(entry, release)) {
                        break;
                    }
                    if (missing) {
                        release.revision = entry.saved_revision;
                    } // A missing manifest already proves absence; shared staged bytes stay
                      // RAM-only.
                }
                self.retire_releases(entry);
                if (entry.owners.empty()) {
                    entry.deleting = true;
                }
                if (entry.state == State::failed) {
                    // Preserve capacity failure from applying the cold ownership removals.
                } else if (entry.staged) {
                    entry.state = missing ? State::writing : State::durable;
                } else if (!entry.deleting) {
                    entry.state = entry.load_payload ? State::loading : State::evicted;
                }
            }
        }
    } else if (pending->operation == Operation::manifest_write) {
        entry.generation = result.generation;
        entry.saved_manifest_bytes = pending->request.size;
        entry.saved_revision = pending->revision;
        self.retire_releases(entry);
    } else if (pending->operation == Operation::remove) {
        if (entry.chunk++ == chunks(entry.size)) {
            self.entries_.erase(pending->id);
            erased = true;
        }
    } else if (pending->operation == Operation::payload_read &&
               result.size != pending->request.size) {
        entry.state = State::failed;
        self.failure_ = entry.failure = AYNI_STORAGE_FAILED;
    } else if (entry.deleting) {
        entry.chunk = 0; // The finished borrow preceded deletion; remove every chunk from zero.
    } else if (++entry.chunk == chunks(entry.size)) {
        const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(entry.bytes.data()),
                                  entry.size};
        if (pending->operation == Operation::payload_read &&
            ayni_body_digest(bytes) != entry.digest) {
            entry.state = State::failed;
            self.failure_ = entry.failure = AYNI_STORAGE_FAILED;
        } else {
            entry.state = State::ready;
        }
    }
    if (!erased && entry.awaited) {
        const bool manifest_done = pending->operation == Operation::manifest_read &&
                                   entry.manifest_loaded && !entry.deleting &&
                                   entry.state != State::loading && entry.state != State::writing &&
                                   entry.releases.empty() && entry.saved_revision == entry.revision;
        const bool read_done =
            pending->operation == Operation::payload_read && entry.state == State::ready;
        const bool write_done = pending->operation == Operation::manifest_write &&
                                !entry.deleting && entry.saved_revision == entry.revision;
        if (entry.state == State::failed || manifest_done || read_done || write_done) {
            entry.awaited = false;
        }
    }
    self.pending_.clear();
}
ayni_status_t FileBodies::status(void *context) {
    auto &self = *static_cast<FileBodies *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    return self.failure_;
}
uint32_t FileBodies::ready(void *context, uint64_t) {
    auto &self = *static_cast<FileBodies *>(context);
    self.flush();
    return self.idle() ? 1U : 0U;
}
uint32_t FileBodies::progress(void *context, uint64_t) {
    static_cast<FileBodies *>(context)->flush();
    return 1U;
}
ayni_status_t FileBodies::collected(void *context, uint64_t scope) {
    auto &self = *static_cast<FileBodies *>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (self.failure_ != AYNI_OK) {
        return self.failure_;
    }
    for (const auto &[id, entry] : self.entries_) {
        const auto alias = identity(scope, entry.digest);
        if (std::any_of(entry.releases.begin(), entry.releases.end(),
                        [alias](const auto &release) { return release.id == alias; })) {
            return AYNI_BUSY;
        }
    }
    return AYNI_OK;
}
bool FileBodies::entry_pending(const Entry &entry) {
    return entry.state != State::failed &&
           (entry.persist_requested || !entry.staged || entry.deleting ||
            !entry.releases.empty()) &&
           (!entry.manifest_loaded || entry.deleting || entry.state == State::writing ||
            entry.state == State::loading || entry.saved_revision != entry.revision);
}
bool FileBodies::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.empty()) {
        return false;
    }
    if (failure_ != AYNI_OK) {
        return true; // flush refuses new I/O; explicit disposal may report the error after join.
    }
    for (const auto &[id, entry] : entries_) {
        if (entry_pending(entry)) {
            return false;
        }
    }
    return true;
}
FileBodies::Backlog FileBodies::backlog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Backlog out{};
    out.in_flight = !pending_.empty();
    for (const auto &[id, entry] : entries_) {
        if (!entry_pending(entry)) {
            if (entry.staged && !entry.persist_requested && !entry.deleting &&
                entry.releases.empty() && entry.state != State::durable &&
                entry.state != State::failed) {
                ++out.staged;
            }
            continue;
        }
        ++out.entries;
        if (entry.deleting || !entry.releases.empty()) {
            ++out.deletes;
        } else if (entry.load_payload || entry.state == State::loading ||
                   (!entry.staged && !entry.persist_requested)) {
            ++out.reads;
        } else {
            ++out.writes;
        }
    }
    return out;
}
void FileBodies::set_background_writes_paused(bool paused) {
    if (std::this_thread::get_id() != owner_) {
        return;
    }
    bool resume = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (background_writes_paused_ == paused) {
            return;
        }
        background_writes_paused_ = paused;
        if (!paused) {
            scan_needed_ = true;
            resume = true;
        }
    }
    if (resume) {
        flush();
    }
}
size_t FileBodies::cached_bytes_locked() const {
    size_t total = 0;
    for (const auto &[id, entry] : entries_) {
        total += entry.bytes.size();
    }
    return total;
}
size_t FileBodies::cached_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_bytes_locked();
}
ayni_status_t FileBodies::trim_locked(size_t target, bool completed) {
    auto bytes = cached_bytes_locked();
    for (auto &[id, entry] : entries_) {
        if (bytes <= target) {
            break;
        }
        // A retired entry has no reader and deletion uses metadata only. Reclaiming
        // its payload must retain the pending deletion even under queue backpressure.
        if (!entry.pending && (entry.deleting || (entry.saved_revision == entry.revision &&
                                                  (entry.state == State::durable ||
                                                   (completed && entry.state == State::ready))))) {
            bytes -= entry.bytes.size();
            std::string{}.swap(entry.bytes);
            entry.state = State::evicted;
            entry.staged = false;
        }
    }
    return bytes <= target ? AYNI_OK : AYNI_BUSY;
}
ayni_status_t FileBodies::trim_cache(size_t target) {
    std::lock_guard<std::mutex> lock(mutex_);
    return trim_locked(target, true);
}
ayni_status_t FileBodies::set_cache_limit(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (trim_locked(bytes, false) != AYNI_OK) {
        return AYNI_BUSY;
    }
    cache_limit_ = bytes;
    return AYNI_OK;
}
ayni_status_t FileBodies::reserve_cache(size_t bytes) {
    if (bytes > cache_limit_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return trim_locked(cache_limit_ - bytes, false) == AYNI_OK ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
} // namespace ayni::runtime

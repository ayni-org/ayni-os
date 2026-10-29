#include "kernel/service/persistence/work_snapshot.h"

#include "authority/principal/owner_binding_records.h"
#include "durability/batch/record_body.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/service/persistence/work_replay.h"
#include "work/authority/authority_cleanup_records.h"

namespace ayni::kernel {
namespace {

// Descriptor positions preserve the persisted order; both traversal directions use it.
SnapshotSection store_section(StorePosition position) {
    switch (position) {
    case StorePosition::instances:
        return SnapshotSection::instances;
    case StorePosition::goals:
        return SnapshotSection::goals;
    case StorePosition::tasks:
        return SnapshotSection::tasks;
    case StorePosition::before_work:
        return SnapshotSection::before_work;
    case StorePosition::after_work:
        return SnapshotSection::after_work;
    case StorePosition::after_cleanup:
        return SnapshotSection::after_cleanup;
    }
    return SnapshotSection::done;
}
bool is_store_section(SnapshotSection section) {
    return section == SnapshotSection::instances || section == SnapshotSection::goals ||
           section == SnapshotSection::tasks || section == SnapshotSection::before_work ||
           section == SnapshotSection::after_work || section == SnapshotSection::after_cleanup;
}

// Schema 2 Goals retain full creation stamps in v4 snapshots. Grants use
// their context-specific versions; exact authority commands retain v5 input
// and results (contracts/storage/records.md).
constexpr uint16_t snapshot_record_version = 1;
constexpr uint32_t record_prefix = static_cast<uint32_t>(durability::record_header_size);

// The tails after the log body (records.md). A stamp is a transition_id,
// then a commit_seq.
constexpr uint32_t stamp_size = snapshot_stamp_size;
// OWNER_BINDING: the stamp that first established this owner.
constexpr uint32_t owner_binding_size = authority::owner_binding_record_size + stamp_size;

uint32_t maximum(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

void count(SnapshotLayout &layout, uint32_t body) {
    layout.payload_length += record_prefix + body;
    ++layout.record_count;
}

} // namespace

uint32_t snapshot_record_max(const KernelCapacities &c) {
    uint32_t size = maximum(owner_binding_size, work::authority_cleanup_record_size);
    for (const auto *store : persistent_stores) {
        if (store->capacity(c) != 0) {
            size = maximum(size, store->record_max(c));
        }
    }
    return record_prefix + size;
}
uint64_t snapshot_payload_max(const KernelCapacities &c) {
    uint64_t size =
        record_bytes(owner_binding_size) + record_bytes(work::authority_cleanup_record_size);
    for (const auto *store : persistent_stores) {
        size += store->snapshot_max != nullptr
                    ? store->snapshot_max(c)
                    : uint64_t{store->capacity(c)} * record_bytes(store->record_max(c));
    }
    return size;
}
SnapshotLayout measure_snapshot(const WorkModel &model) {
    SnapshotLayout layout{};
    count(layout, work::authority_cleanup_record_size);
    if (model.principals.binding_known()) {
        count(layout, owner_binding_size);
    }
    for (const auto *store : persistent_stores) {
        StoreCursor cursor{};
        while (store->snapshot_next(model, cursor)) {
            count(layout, store->snapshot_record(model, cursor).size);
            store->snapshot_advance(model, cursor);
        }
    }
    return layout;
}
void WorkSnapshotWriter::reset() {
    section_ = Section::owner_binding;
    store_ = 0;
    store_cursor_ = {};
}
bool WorkSnapshotWriter::next_store_record(StorePosition position, uint16_t &type, uint32_t &size) {
    for (; store_ < persistent_store_count; ++store_, store_cursor_ = {}) {
        const PersistentStore &store = *persistent_stores[store_];
        if (store.position == position && store.snapshot_next(model_, store_cursor_)) {
            const auto shape = store.snapshot_record(model_, store_cursor_);
            type = shape.type == 0 ? store.record_type : shape.type;
            size = shape.size;
            return true;
        }
    }
    return false;
}
// Sections preserve schema 2 order; each descriptor owns its row/part traversal.
bool WorkSnapshotWriter::next_record(uint16_t &type, uint32_t &size) {
    for (;;) {
        if (section_ == Section::owner_binding) {
            if (model_.principals.binding_known()) {
                type = authority::owner_binding_record_type;
                size = owner_binding_size;
                return true;
            }
        } else if (section_ == Section::cleanup) {
            type = work::authority_cleanup_record_type;
            size = work::authority_cleanup_record_size;
            return true;
        } else if (section_ == Section::done) {
            return false;
        } else {
            StorePosition position = StorePosition::instances;
            switch (section_) {
            case Section::instances:
                position = StorePosition::instances;
                break;
            case Section::before_work:
                position = StorePosition::before_work;
                break;
            case Section::goals:
                position = StorePosition::goals;
                break;
            case Section::tasks:
                position = StorePosition::tasks;
                break;
            case Section::after_work:
                position = StorePosition::after_work;
                break;
            case Section::after_cleanup:
                position = StorePosition::after_cleanup;
                break;
            default:
                break;
            }
            if (next_store_record(position, type, size)) {
                return true;
            }
        }
        section_ = static_cast<Section>(static_cast<uint32_t>(section_) + 1U);
        store_ = 0;
        store_cursor_ = {};
    }
}
void WorkSnapshotWriter::write_record(uint16_t type, uint8_t *body) {
    if (is_store_section(section_)) {
        persistent_stores[store_]->snapshot_write(model_, store_cursor_, body);
        return;
    }
    if (type == authority::owner_binding_record_type) {
        const auto &binding = *model_.principals.binding();
        authority::write_owner_binding_record(binding, body);
        write_snapshot_stamp(body + authority::owner_binding_record_size,
                             {binding.stamp.transition_id, binding.stamp.commit_seq});
    } else {
        work::write_authority_cleanup_record(model_.authority_cleanup.record(), body);
    }
}
void WorkSnapshotWriter::advance_record(uint16_t) {
    if (is_store_section(section_)) {
        persistent_stores[store_]->snapshot_advance(model_, store_cursor_);
        return;
    }
    section_ = section_ == Section::owner_binding ? Section::instances : Section::after_cleanup;
    store_ = 0;
    store_cursor_ = {};
}
WorkSnapshotWriter::Page WorkSnapshotWriter::next(uint8_t *buffer, uint32_t capacity,
                                                  uint32_t &out_size) {
    out_size = 0;
    uint16_t type = 0;
    uint32_t size = 0;
    while (next_record(type, size)) {
        if (capacity - out_size < record_prefix || capacity - out_size - record_prefix < size) {
            return out_size == 0U ? Page::no_room : Page::page;
        }
        uint8_t *at = buffer + out_size;
        const uint16_t version =
            is_store_section(section_)
                ? persistent_stores[store_]->snapshot_record(model_, store_cursor_).version
                : snapshot_record_version;
        durability::write_record_header({type, version, durability::record_required, size}, at);
        write_record(type, at + record_prefix);
        out_size += record_prefix + size;
        advance_record(type);
    }
    return out_size == 0U ? Page::done : Page::page;
}
void WorkSnapshotReader::reset(const durability::SnapshotMetadata &metadata) {
    metadata_ = metadata;
    records_ = 0;
    section_ = Section::owner_binding;
    store_ = 0;
    store_read_ = {};
    cleanup_seen_ = false;
}
// Only binding and cleanup are coordinator-owned; all work records belong to stores.
bool WorkSnapshotReader::record_section(uint16_t type, Section &out) {
    if (type == authority::owner_binding_record_type) {
        out = Section::owner_binding;
        return true;
    }
    if (type == work::authority_cleanup_record_type) {
        out = Section::cleanup;
        return true;
    }
    return false;
}
durability::BatchCheck WorkSnapshotReader::apply(const durability::RecordHeader &header,
                                                 const uint8_t *body) {
    using durability::BatchCheck;
    if (records_ == metadata_.record_count ||
        (store_read_.expected_type != 0 && header.type != store_read_.expected_type)) {
        return BatchCheck::corrupt;
    }
    ++records_;
    if ((header.flags & ~durability::record_required) != 0) {
        return BatchCheck::unsupported;
    }
    const auto use = WorkReplay::classify_header(header, WorkRecordContext::snapshot);
    if (use == durability::RecordUse::corrupt) {
        return BatchCheck::corrupt;
    }
    // ADR-0082: no optional-extension compatibility path exists in schema 3.
    constexpr auto unknown = BatchCheck::unsupported;
    if (use != durability::RecordUse::applied) {
        return unknown;
    }
    Section section = section_;
    uint32_t store = 0;
    const bool is_store = find_persistent_store(header.type, store);
    if (is_store) {
        section = store_section(persistent_stores[store]->position);
    } else if (!record_section(header.type, section)) {
        return unknown;
    }
    const bool changes_family = section != section_ || (is_store && store != store_);
    if (section < section_ || (is_store && section == section_ && store < store_) ||
        (changes_family && !store_read_.complete())) {
        return BatchCheck::corrupt;
    }
    section_ = section;
    if (changes_family) {
        store_read_ = {};
    }
    if (is_store) {
        store_ = store;
        return persistent_stores[store]->snapshot_read(model_, metadata_, header.version, body,
                                                       header.length, header.type, store_read_);
    }
    return header.type == authority::owner_binding_record_type
               ? owner_binding(body, header.length)
               : authority_cleanup(body, header.length);
}
durability::BatchCheck WorkSnapshotReader::owner_binding(const uint8_t *body, uint32_t size) {
    authority::OwnerBindingRecord record{};
    if (size != owner_binding_size ||
        !authority::read_owner_binding_record(body, authority::owner_binding_record_size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp =
        read_snapshot_stamp(body + authority::owner_binding_record_size);
    return within_snapshot(stamp, metadata_) && model_.principals.restore_binding(record, stamp)
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}
durability::BatchCheck WorkSnapshotReader::authority_cleanup(const uint8_t *body, uint32_t size) {
    work::AuthorityCleanupRecord record{};
    if (cleanup_seen_ || !work::read_authority_cleanup_record(body, size, record) ||
        (record.stamp.commit_seq != 0U && !within_snapshot(record.stamp, metadata_)) ||
        foundation::id_boot_epoch(record.after_goal_id) > metadata_.boot_epoch ||
        foundation::id_boot_epoch(record.through_goal_id) > metadata_.boot_epoch ||
        foundation::id_boot_epoch(record.requested_through) > metadata_.boot_epoch) {
        return durability::BatchCheck::corrupt;
    }
    model_.authority_cleanup.restore_validated(record);
    cleanup_seen_ = true;
    return durability::BatchCheck::ok;
}
durability::BatchCheck WorkSnapshotReader::complete() const {
    return records_ == metadata_.record_count && cleanup_seen_ && store_read_.complete() &&
                   WorkReplay(model_).consistent(metadata_.boot_epoch)
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}
} // namespace ayni::kernel

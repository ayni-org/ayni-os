#include <ayni/providers/mock/media.h>
#include <cstring>
#include <new>
namespace ayni::providers::mock {
namespace {
constexpr uint32_t object_capacity_max = 64;
bool same(const ayni_catalog_name_v1 &name, ayni_string_v1 text) {
    return name.size == text.size && text.data != nullptr &&
           std::memcmp(name.data, text.data, name.size) == 0;
}
bool same(const ayni_catalog_name_v1 &left, const ayni_catalog_name_v1 &right) {
    return same(left, {right.data, right.size});
}
bool exact(const ayni_media_ref_v1 &left, const ayni_media_ref_v1 &right) {
    return left.media_id == right.media_id && left.boot_epoch == right.boot_epoch &&
           left.content_type == right.content_type &&
           left.owner_instance_id == right.owner_instance_id &&
           left.generation == right.generation && left.byte_size == right.byte_size &&
           left.valid_until_us == right.valid_until_us &&
           left.source_version == right.source_version && same(left.source_id, right.source_id) &&
           same(left.purpose, right.purpose);
}
} // namespace
struct MediaStore::Impl {
    struct Slot {
        bool present = false; // Exactly when reference and its byte region contain a live object.
        ayni_media_ref_v1 reference{}; // Whole identity, no pointer to caller memory.
    };
    MediaConfig config;               // Fixed allocation bounds.
    std::unique_ptr<Slot[]> slots;    // One identity per object.
    std::unique_ptr<uint8_t[]> bytes; // Fixed stride regions, private to this owner.
    uint64_t boot = 0;                // 0 before begin_boot.
    uint64_t next_id = 1;             // Monotone across boots; UINT64_MAX means exhausted.
    uint8_t *body(uint32_t index) const { return bytes.get() + index * config.object_max_bytes; }
    void clear(uint32_t index) {
        std::memset(body(index), 0, config.object_max_bytes);
        slots[index] = {};
    }
};
MediaStore::MediaStore() = default;
MediaStore::~MediaStore() {
    if (impl_) {
        for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
            impl_->clear(i);
        }
    }
}
ayni_status_t MediaStore::configure(MediaConfig config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if (config.object_capacity == 0 || config.object_capacity > object_capacity_max ||
        config.object_max_bytes == 0 || config.object_max_bytes > AYNI_MEDIA_BYTES_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::unique_ptr<Impl> created(new (std::nothrow) Impl{});
    if (!created) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    created->config = config;
    created->slots.reset(new (std::nothrow) Impl::Slot[config.object_capacity]);
    created->bytes.reset(new (std::nothrow)
                             uint8_t[config.object_capacity * config.object_max_bytes]{});
    if (!created->slots || !created->bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    impl_ = std::move(created);
    return AYNI_OK;
}
ayni_status_t MediaStore::begin_boot(uint64_t boot_epoch) {
    if (!impl_) {
        return AYNI_NOT_READY;
    }
    if (boot_epoch == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (boot_epoch < impl_->boot) {
        return AYNI_STALE;
    }
    if (boot_epoch != impl_->boot) {
        for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
            impl_->clear(i);
        }
        impl_->boot = boot_epoch;
    }
    return AYNI_OK;
}
// Allocation and validation complete before installing a slot; even exhausted
// identities never overwrite another object's bytes or modify the output.
ayni_status_t MediaStore::put(ayni_media_ref_v1 metadata, ayni_bytes_v1 bytes, uint64_t now_us,
                              ayni_media_ref_v1 &out) {
    if (!impl_ || impl_->boot == 0) {
        return AYNI_NOT_READY;
    }
    if (metadata.media_id != 0 || bytes.data == nullptr || bytes.size != metadata.byte_size) {
        return AYNI_INVALID_ARGUMENT;
    }
    metadata.media_id = impl_->next_id;
    if (const auto status = ayni_validate_media_reference(&metadata); status != AYNI_OK) {
        return status;
    }
    if (metadata.boot_epoch != impl_->boot) {
        return AYNI_STALE;
    }
    if (now_us >= metadata.valid_until_us) {
        return AYNI_EXPIRED;
    }
    if (bytes.size > impl_->config.object_max_bytes || impl_->next_id == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    expire(now_us);
    for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
        auto &slot = impl_->slots[i];
        if (!slot.present) {
            std::memcpy(impl_->body(i), bytes.data, bytes.size);
            slot.reference = metadata;
            slot.present = true;
            ++impl_->next_id;
            out = metadata;
            return AYNI_OK;
        }
    }
    return AYNI_CAPACITY_EXCEEDED;
}
// Validate the caller's whole identity before looking up bytes. A guessed ID or
// mutated metadata cannot broaden the access or retention of an existing object.
ayni_status_t MediaStore::read(const ayni_media_ref_v1 &reference, uint64_t owner,
                               ayni_string_v1 purpose, uint64_t now_us, uint8_t *out,
                               uint32_t capacity, uint32_t &size) const {
    const auto status = ayni_validate_media_reference(&reference);
    if (status != AYNI_OK) {
        return status;
    }
    if (!impl_ || impl_->boot == 0) {
        return AYNI_NOT_READY;
    }
    if (reference.boot_epoch != impl_->boot) {
        return AYNI_STALE;
    }
    if (owner != reference.owner_instance_id || !same(reference.purpose, purpose)) {
        return AYNI_DENIED;
    }
    if (now_us >= reference.valid_until_us) {
        return AYNI_EXPIRED;
    }
    for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
        if (impl_->slots[i].present && exact(reference, impl_->slots[i].reference)) {
            const auto needed = static_cast<uint32_t>(reference.byte_size);
            if (capacity < needed) {
                size = needed;
                return AYNI_BUFFER_TOO_SMALL;
            }
            if (out == nullptr) {
                return AYNI_INVALID_ARGUMENT;
            }
            std::memcpy(out, impl_->body(i), needed);
            size = needed;
            return AYNI_OK;
        }
    }
    return AYNI_NOT_FOUND;
}
void MediaStore::expire(uint64_t now_us) {
    if (!impl_) {
        return;
    }
    for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
        if (impl_->slots[i].present && now_us >= impl_->slots[i].reference.valid_until_us) {
            impl_->clear(i);
        }
    }
}
void MediaStore::release_generation(ayni_string_v1 source, uint64_t boot_epoch,
                                    uint64_t generation) {
    if (!impl_ || impl_->boot != boot_epoch) {
        return;
    }
    for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
        const auto &slot = impl_->slots[i];
        if (slot.present && slot.reference.generation == generation &&
            same(slot.reference.source_id, source)) {
            impl_->clear(i);
        }
    }
}
uint32_t MediaStore::occupied() const {
    uint32_t count = 0;
    if (impl_) {
        for (uint32_t i = 0; i < impl_->config.object_capacity; ++i) {
            count += impl_->slots[i].present ? 1U : 0U;
        }
    }
    return count;
}
} // namespace ayni::providers::mock

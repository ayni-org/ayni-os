// Bounded M1 media bytes; no capture, filesystem, network or authorization service.
#ifndef AYNI_PROVIDERS_MOCK_MEDIA_H
#define AYNI_PROVIDERS_MOCK_MEDIA_H
#include <ayni/media.h>
#include <memory>
namespace ayni::providers::mock {
// Fixed independent object/byte bounds; no allocation after configure.
struct MediaConfig {
    uint32_t object_capacity = 4;     // Concurrent references, 1..64.
    uint32_t object_max_bytes = 4096; // Per-slot bytes, 1..AYNI_MEDIA_BYTES_MAX.
};
// Single owner-thread test store. The trusted caller checks capture/read/egress
// permission separately; matching owner/purpose alone does not grant permission.
class MediaStore {
  public:
    MediaStore();
    ~MediaStore();
    MediaStore(const MediaStore &) = delete;
    MediaStore &operator=(const MediaStore &) = delete;
    // Bounded startup allocations; INVALID_ARGUMENT, INVALID_STATE or CAPACITY_EXCEEDED.
    ayni_status_t configure(MediaConfig config);
    // Positive strictly increasing boot; a new boot zeroes/reclaims all bytes.
    // Same boot is idempotent, an older boot returns STALE. No IDs are reused.
    ayni_status_t begin_boot(uint64_t boot_epoch);
    // metadata.media_id must be zero; other fields must satisfy the public schema
    // and current boot/TTL. Copy bytes and return assigned metadata on success only.
    // Full store or oversized object returns CAPACITY_EXCEEDED. Expired slots reclaim first.
    ayni_status_t put(ayni_media_ref_v1 metadata, ayni_bytes_v1 bytes, uint64_t now_us,
                      ayni_media_ref_v1 &out);
    // Copy complete exact reference for an explicitly matching owner/purpose.
    // No borrowed view; BUFFER_TOO_SMALL reports required bytes, no partial copy.
    // STALE boot, EXPIRED TTL, DENIED access, NOT_FOUND released/mismatched reference.
    ayni_status_t read(const ayni_media_ref_v1 &reference, uint64_t owner, ayni_string_v1 purpose,
                       uint64_t now_us, uint8_t *out, uint32_t capacity, uint32_t &size) const;
    // Zero/reclaim expired bytes and every slot of an explicitly closed generation.
    void expire(uint64_t now_us);
    void release_generation(ayni_string_v1 source, uint64_t boot_epoch, uint64_t generation);
    // Current occupied slots, including those awaiting explicit expire; zero unconfigured.
    uint32_t occupied() const;

  private:
    struct Impl;                 // Owns bounded slots and private byte arenas.
    std::unique_ptr<Impl> impl_; // Null before configure; all bytes destroyed with this store.
};
} // namespace ayni::providers::mock
#endif

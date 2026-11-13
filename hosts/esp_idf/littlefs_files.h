// Dedicated LittleFS volume. Raw API permits atomic rename; the adapter rebinds
// source tokens afterwards so later synced writes remain linked. VFS rejects open-file rename.
#ifndef AYNI_ESP_LITTLEFS_FILES_H
#define AYNI_ESP_LITTLEFS_FILES_H
#include <ayni/hosts/esp_idf/metrics.h>
#include <ayni/hosts/storage/files.h>
#include <esp_partition.h>
#define ESP_LITTLEFS_RAW_API
#include <array>
#include <cerrno>
#include <lfs.h>
namespace ayni::hosts::esp_idf {
class LittleFiles final : public storage::Files {
  public:
    explicit LittleFiles(bool initialize_blank) : initialize_blank_(initialize_blank) {}
    ~LittleFiles() override { end(); }
    LittleFiles(const LittleFiles &) = delete;
    LittleFiles &operator=(const LittleFiles &) = delete;
    int begin(const char *partition_label) override;
    void end() override;
    int open(const char *, Mode, int &) override;
    void close(int) override;
    int lock(int) override;
    int read(int, uint64_t, uint8_t *, std::size_t, std::size_t &) const override;
    int write(int, uint64_t, const uint8_t *, std::size_t) override;
    int size(int, uint64_t &) const override;
    int truncate(int, uint64_t) override;
    int sync_file(int) override;
    int sync_namespace() override;
    int rename(const char *, const char *) override;
    int remove(const char *) override;
    int free_bytes(uint64_t &) const override;
    int last_mount_error() const { return mount_error_; }
    // Same exclusive I/O-thread ownership as the file methods; no synchronization needed.
    FileIoMetrics metrics() const { return metrics_; }

  private:
    static constexpr std::size_t file_capacity = 4;
    static constexpr std::size_t path_bytes = 80;
    static constexpr std::size_t cache_bytes = 256;
    static constexpr std::size_t lookahead_bytes = 128;
    static constexpr uint32_t block_bytes = 4096;
    static constexpr int block_cycles = 128;
    static constexpr uint32_t transfer_unit_bytes = 16;
    static constexpr uint32_t minimum_volume_blocks = 4;
    static constexpr uint8_t erased_byte = 0xff;
    enum class State { free, open, detached };
    struct Slot {
        lfs_file_t file{};
        lfs_file_config config{};
        std::array<uint8_t, cache_bytes> cache{};
        State state = State::free;
        std::array<char, path_bytes> name{}; // Valid only while open; detached inode has no name.
        int access_flags = 0; // Reopen access only; excludes CREATE/TRUNC so data survives rename.
    };
    static int read_block(const lfs_config *, lfs_block_t, lfs_off_t, void *, lfs_size_t);
    static int program_block(const lfs_config *, lfs_block_t, lfs_off_t, const void *, lfs_size_t);
    static int erase_block(const lfs_config *, lfs_block_t);
    static int sync_medium(const lfs_config *);
    int check_blank(bool &blank) const;
    Slot *slot(int handle) const;
    void detach_name(const char *name);
    int namespace_result(int status);
    // These structures and caches live in internal RAM with the owning Storage.
    mutable lfs_t filesystem_{};
    mutable FileIoMetrics metrics_{};
    lfs_config config_{};
    mutable std::array<Slot, file_capacity> slots_{};
    std::array<uint8_t, cache_bytes> read_cache_{}, program_cache_{};
    alignas(uint32_t) std::array<uint8_t, lookahead_bytes> lookahead_{};
    const esp_partition_t *partition_ = nullptr; // Non-null only while owning a registry lease.
    enum class VolumeState { closed, mounted, poisoned };
    VolumeState volume_state_ = VolumeState::closed; // Mounted/poisoned own filesystem+lease.
    int unavailable() const { return volume_state_ == VolumeState::poisoned ? EIO : EBADF; }
    bool initialize_blank_; // Explicit startup permission, never format unknown nonblank media.
    int mount_error_ = 0;   // Last raw lfs result; file methods otherwise return errno causes.
};
} // namespace ayni::hosts::esp_idf
#endif

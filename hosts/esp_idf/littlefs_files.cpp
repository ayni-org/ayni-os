#include "littlefs_files.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <esp_littlefs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <limits>
namespace ayni::hosts::esp_idf {
namespace {
// File API wall time includes preemption and LittleFS work; this emits no logs or I/O.
class FileTimer {
  public:
    explicit FileTimer(uint64_t &total) : total_(total), start_(esp_timer_get_time()) {}
    ~FileTimer() { total_ += static_cast<uint64_t>(esp_timer_get_time() - start_); }

  private:
    uint64_t &total_;
    int64_t start_;
};
// Platform-local, bounded partition leases exclude all Ayni Hosts. Deployments
// must dedicate these partitions: external VFS/raw writers are outside this owner.
constexpr std::size_t volume_capacity = 2;
portMUX_TYPE volume_mutex = portMUX_INITIALIZER_UNLOCKED;
std::array<const esp_partition_t *, volume_capacity> volumes{};
int claim(const esp_partition_t *partition) {
    int result = ENOSPC;
    portENTER_CRITICAL(&volume_mutex);
    for (const auto *current : volumes) {
        if (current == partition) {
            result = EWOULDBLOCK;
            break;
        }
    }
    if (result != EWOULDBLOCK) {
        for (auto &current : volumes) {
            if (!current) {
                current = partition;
                result = 0;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&volume_mutex);
    return result;
}
void release(const esp_partition_t *partition) {
    portENTER_CRITICAL(&volume_mutex);
    for (auto &current : volumes) {
        if (current == partition) {
            current = nullptr;
        }
    }
    portEXIT_CRITICAL(&volume_mutex);
}
int cause(int status) {
    if (status >= 0) {
        return 0;
    }
    switch (status) {
    case LFS_ERR_NOENT:
        return ENOENT;
    case LFS_ERR_EXIST:
        return EEXIST;
    case LFS_ERR_NOSPC:
        return ENOSPC;
    case LFS_ERR_NOMEM:
        return ENOMEM;
    case LFS_ERR_INVAL:
        return EINVAL;
    case LFS_ERR_FBIG:
        return EFBIG;
    default:
        return EIO;
    }
}
constexpr uint64_t file_limit = static_cast<uint64_t>(std::numeric_limits<lfs_soff_t>::max());
} // namespace
int LittleFiles::begin(const char *label) {
    space_.invalidate();
    if (partition_) {
        return EBUSY;
    }
    mount_error_ = 0;
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, label);
    if (!partition) {
        return ENOENT;
    }
    if (partition->encrypted || partition->size % block_bytes != 0 ||
        partition->size / block_bytes < minimum_volume_blocks) {
        return EINVAL;
    }
    // A VFS mount already using the dedicated partition is explicitly refused.
    if (esp_littlefs_partition_mounted(partition)) {
        return EWOULDBLOCK;
    }
    const int acquired = claim(partition);
    if (acquired != 0) {
        return acquired;
    }
    partition_ = partition;
    config_ = {};
    config_.context = this;
    config_.read = read_block;
    config_.prog = program_block;
    config_.erase = erase_block;
    config_.sync = sync_medium;
    config_.read_size = transfer_unit_bytes;
    config_.prog_size = transfer_unit_bytes;
    config_.block_size = block_bytes;
    config_.block_count = partition->size / block_bytes;
    config_.block_cycles = block_cycles;
    config_.cache_size = cache_bytes;
    config_.lookahead_size = lookahead_bytes;
    config_.read_buffer = read_cache_.data();
    config_.prog_buffer = program_cache_.data();
    config_.lookahead_buffer = lookahead_.data();
    mount_error_ = lfs_mount(&filesystem_, &config_);
    if (mount_error_ != 0 && initialize_blank_) {
        bool blank = false;
        const int checked = check_blank(blank);
        if (checked != 0) {
            mount_error_ = 0; // Preserve the actual errno cause, not the preceding mount refusal.
            end();
            return checked;
        }
        // Formatting is allowed only for an entirely erased partition while leased.
        if (blank) {
            mount_error_ = lfs_format(&filesystem_, &config_);
            if (mount_error_ == 0) {
                mount_error_ = lfs_mount(&filesystem_, &config_);
            }
        }
    }
    if (mount_error_ != 0) {
        end();
        return EIO;
    }
    volume_state_ = VolumeState::mounted;
    return 0;
}
void LittleFiles::end() {
    space_.invalidate();
    if (volume_state_ != VolumeState::closed) {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            close(static_cast<int>(index));
        }
        static_cast<void>(lfs_unmount(&filesystem_));
        volume_state_ = VolumeState::closed;
    }
    if (partition_) {
        release(partition_);
        partition_ = nullptr;
    }
}
LittleFiles::Slot *LittleFiles::slot(int handle) const {
    return volume_state_ != VolumeState::closed && handle >= 0 &&
                   static_cast<std::size_t>(handle) < slots_.size() &&
                   slots_[static_cast<std::size_t>(handle)].state != State::free
               ? &slots_[static_cast<std::size_t>(handle)]
               : nullptr;
}
int LittleFiles::open(const char *name, Mode mode, int &handle) {
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    if (!name || std::strlen(name) >= slots_[0].name.size()) {
        return ENAMETOOLONG;
    }
    int flags = 0;
    switch (mode) {
    case Mode::read_existing:
    case Mode::read_existing_no_follow:
        flags = LFS_O_RDONLY;
        break; // LittleFS has no symlinks.
    case Mode::read_write_create:
        flags = LFS_O_RDWR | LFS_O_CREAT;
        break;
    case Mode::replace:
        flags = LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC;
        break;
    default:
        return EINVAL;
    }
    // C-HOST-010: a new reader must see the current bytes of a same-name writer.
    // POSIX shares that view automatically; raw LittleFS publishes a changed
    // file's CTZ head/length at sync. Only an existing dirty writer needs it.
    // This publication does not replace FileLog's subsequent identical rewrite
    // and final file/namespace confirmation of an inherited uncertain prefix.
    if (flags == LFS_O_RDONLY) {
        for (auto &entry : slots_) {
            if (entry.state == State::open && std::strcmp(entry.name.data(), name) == 0 &&
                (entry.file.flags & (LFS_F_DIRTY | LFS_F_WRITING)) != 0) {
                const auto status = lfs_file_sync(&filesystem_, &entry.file);
                if (status != 0) {
                    return cause(status);
                }
            }
        }
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto &entry = slots_[index];
        if (entry.state == State::free) {
            entry.config = {};
            entry.config.buffer = entry.cache.data();
            const auto status =
                lfs_file_opencfg(&filesystem_, &entry.file, name, flags, &entry.config);
            if (status != 0) {
                return cause(status);
            }
            entry.state = State::open;
            entry.access_flags = flags & LFS_O_RDWR;
            if (entry.access_flags == 0) {
                entry.access_flags = LFS_O_RDONLY;
            }
            std::memcpy(entry.name.data(), name, std::strlen(name) + 1);
            handle = static_cast<int>(index);
            return 0;
        }
    }
    return EMFILE;
}
void LittleFiles::close(int handle) {
    if (auto *entry = slot(handle)) {
        if (volume_state_ == VolumeState::mounted) {
            // Every acknowledged mutation was explicitly synced. A close never
            // promotes an uncertain write to a successful Host completion.
            static_cast<void>(lfs_file_close(&filesystem_, &entry->file));
        }
        // Poisoned cleanup must not invoke lfs_close's implicit sync through an
        // unproven directory association. All file/cache buffers are supplied here;
        // discard their state, then lfs_unmount only deinitializes volume buffers.
        // The raw mlist points into these still-live slots until that unmount.
        entry->state = State::free;
    }
}
int LittleFiles::lock(int handle) {
    return volume_state_ != VolumeState::mounted ? unavailable() : slot(handle) ? 0 : EBADF;
}
int LittleFiles::read(int handle, uint64_t offset, uint8_t *data, std::size_t bytes,
                      std::size_t &count) const {
    FileTimer timer(metrics_.read_us);
    ++metrics_.read_calls;
    count = 0;
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    auto *entry = slot(handle);
    if (!entry) {
        return EBADF;
    }
    if (offset > file_limit || bytes > file_limit - offset) {
        return EFBIG;
    }
    const auto position =
        lfs_file_seek(&filesystem_, &entry->file, static_cast<lfs_soff_t>(offset), LFS_SEEK_SET);
    if (position < 0) {
        return cause(position);
    }
    const auto status = lfs_file_read(&filesystem_, &entry->file, data, bytes);
    if (status < 0) {
        return cause(status);
    }
    count = static_cast<std::size_t>(status);
    metrics_.read_bytes += count;
    return 0;
}
int LittleFiles::write(int handle, uint64_t offset, const uint8_t *data, std::size_t bytes) {
    FileTimer timer(metrics_.write_us);
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    auto *entry = slot(handle);
    if (!entry) {
        return EBADF;
    }
    if (offset > file_limit || bytes > file_limit - offset) {
        return EFBIG;
    }
    const auto position =
        lfs_file_seek(&filesystem_, &entry->file, static_cast<lfs_soff_t>(offset), LFS_SEEK_SET);
    if (position < 0) {
        return cause(position);
    }
    const auto status = lfs_file_write(&filesystem_, &entry->file, data, bytes);
    return status < 0 ? cause(status) : static_cast<std::size_t>(status) == bytes ? 0 : EIO;
}
int LittleFiles::size(int handle, uint64_t &bytes) const {
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    auto *entry = slot(handle);
    if (!entry) {
        return EBADF;
    }
    const auto size = lfs_file_size(&filesystem_, &entry->file);
    if (size < 0) {
        return cause(size);
    }
    bytes = static_cast<uint64_t>(size);
    return 0;
}
int LittleFiles::truncate(int handle, uint64_t bytes) {
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    auto *entry = slot(handle);
    if (!entry) {
        return EBADF;
    }
    return bytes > file_limit ? EFBIG
                              : cause(lfs_file_truncate(&filesystem_, &entry->file,
                                                        static_cast<lfs_off_t>(bytes)));
}
int LittleFiles::sync_file(int handle) {
    FileTimer timer(metrics_.sync_us);
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    auto *entry = slot(handle);
    return entry ? cause(lfs_file_sync(&filesystem_, &entry->file)) : EBADF;
}
int LittleFiles::sync_namespace() {
    // lfs_rename/remove commit directory metadata and call cfg.sync before success;
    // esp_partition write/erase are synchronous. There is no deferred VFS directory cache.
    return volume_state_ == VolumeState::mounted ? 0 : unavailable();
}
// Raw LittleFS detaches an opened source during rename. Rebind the same outward
// token to the new directory entry before acknowledging replacement (C-HOST-010).
int LittleFiles::rename(const char *from, const char *to) {
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    if (std::strlen(to) >= slots_[0].name.size()) {
        return ENAMETOOLONG;
    }
    if (std::strcmp(from, to) == 0) {
        return namespace_result(lfs_rename(&filesystem_, from, to));
    }
    for (auto &entry : slots_) {
        if (entry.state == State::open && std::strcmp(entry.name.data(), from) == 0) {
            const int synced = lfs_file_sync(&filesystem_, &entry.file);
            if (synced != 0) {
                return cause(synced);
            }
        }
    }
    const int renamed = namespace_result(lfs_rename(&filesystem_, from, to));
    if (renamed != 0) {
        return renamed;
    }
    detach_name(to);
    for (auto &entry : slots_) {
        if (entry.state != State::open) {
            continue;
        }
        if (std::strcmp(entry.name.data(), from) == 0) {
            const int closed = lfs_file_close(&filesystem_, &entry.file);
            entry.state = State::free;
            const int opened =
                lfs_file_opencfg(&filesystem_, &entry.file, to, entry.access_flags, &entry.config);
            if (opened == 0) {
                entry.state = State::open;
                std::memcpy(entry.name.data(), to, std::strlen(to) + 1);
            }
            if (closed != 0 || opened != 0) {
                // The namespace replacement already committed. Refuse all future I/O
                // rather than acknowledge appends through a detached inode. Close is
                // still allowed; an orderly Host reopen establishes the actual namespace.
                volume_state_ = VolumeState::poisoned;
                return cause(closed != 0 ? closed : opened);
            }
        }
    }
    return 0;
}
// A raw metadata error can follow a committed directory change (e.g. readback
// or orphan cleanup). No continued token association is proven after that error.
int LittleFiles::namespace_result(int status) {
    if (status != 0) {
        volume_state_ = VolumeState::poisoned;
    }
    return cause(status);
}
void LittleFiles::detach_name(const char *name) {
    for (auto &entry : slots_) {
        if (entry.state == State::open && std::strcmp(entry.name.data(), name) == 0) {
            entry.state = State::detached;
        }
    }
}
int LittleFiles::remove(const char *name) {
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    const int status = lfs_remove(&filesystem_, name);
    // C-HVAL-004/007: cleanup may remove a never-persisted or already removed value.
    // NOENT is a lookup miss, not an uncertain namespace mutation; keep the volume usable.
    if (status == LFS_ERR_NOENT) {
        return cause(status);
    }
    const int removed = namespace_result(status);
    if (removed == 0) {
        detach_name(name);
    }
    return removed;
}
int LittleFiles::free_bytes(uint64_t &bytes) const {
    if (volume_state_ != VolumeState::mounted) {
        return unavailable();
    }
    if (!space_.needs_measurement() && space_.available(bytes)) {
        return 0;
    }
    const auto used = lfs_fs_size(&filesystem_);
    if (used < 0) {
        space_.invalidate();
        return cause(used);
    }
    constexpr uint32_t metadata_reserve_blocks = 2;
    const uint64_t unavailable = static_cast<uint64_t>(used) + metadata_reserve_blocks;
    bytes =
        unavailable < config_.block_count ? (config_.block_count - unavailable) * block_bytes : 0;
    space_.measured(bytes);
    return 0;
}
int LittleFiles::read_block(const lfs_config *config, lfs_block_t block, lfs_off_t offset,
                            void *data, lfs_size_t bytes) {
    auto &self = *static_cast<LittleFiles *>(config->context);
    if (block >= config->block_count || offset > block_bytes || bytes > block_bytes - offset) {
        return LFS_ERR_IO;
    }
    std::array<uint8_t, cache_bytes> bounce{};
    auto *out = static_cast<uint8_t *>(data);
    uint32_t at = block * block_bytes + offset;
    while (bytes) {
        const auto count = bytes < bounce.size() ? bytes : bounce.size();
        if (esp_partition_read(self.partition_, at, bounce.data(), count) != ESP_OK) {
            return LFS_ERR_IO;
        }
        std::memcpy(out, bounce.data(), count);
        out += count;
        at += count;
        bytes -= count;
    }
    return 0;
}
int LittleFiles::program_block(const lfs_config *config, lfs_block_t block, lfs_off_t offset,
                               const void *data, lfs_size_t bytes) {
    auto &self = *static_cast<LittleFiles *>(config->context);
    if (block >= config->block_count || offset > block_bytes || bytes > block_bytes - offset) {
        return LFS_ERR_IO;
    }
    self.space_.changing(block);
    std::array<uint8_t, cache_bytes> bounce{};
    const auto *in = static_cast<const uint8_t *>(data);
    uint32_t at = block * block_bytes + offset;
    while (bytes) {
        const auto count = bytes < bounce.size() ? bytes : bounce.size();
        std::memcpy(bounce.data(), in, count);
        if (esp_partition_write(self.partition_, at, bounce.data(), count) != ESP_OK) {
            return LFS_ERR_IO;
        }
        in += count;
        at += count;
        bytes -= count;
    }
    return 0;
}
int LittleFiles::erase_block(const lfs_config *config, lfs_block_t block) {
    auto &self = *static_cast<LittleFiles *>(config->context);
    if (block >= config->block_count) {
        return LFS_ERR_IO;
    }
    self.space_.changing(block);
    return esp_partition_erase_range(self.partition_, block * block_bytes, block_bytes) == ESP_OK
               ? 0
               : LFS_ERR_IO;
}
int LittleFiles::sync_medium(const lfs_config *) {
    // esp_partition_read/write/erase have returned; there is no asynchronous flash work.
    return 0;
}
int LittleFiles::check_blank(bool &blank) const {
    std::array<uint8_t, cache_bytes> chunk{};
    blank = false;
    for (uint32_t offset = 0; offset < partition_->size; offset += chunk.size()) {
        if (esp_partition_read(partition_, offset, chunk.data(), chunk.size()) != ESP_OK) {
            return EIO;
        }
        for (const auto byte : chunk) {
            if (byte != erased_byte) {
                return 0;
            }
        }
    }
    blank = true;
    return 0;
}
} // namespace ayni::hosts::esp_idf

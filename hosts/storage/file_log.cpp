#include <ayni/hosts/storage/file_log.h>

#include <ayni/storage_format.h>

#include <ayni/ayni.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace ayni::hosts::storage {
namespace {

constexpr std::array<uint8_t, 4> file_magic{{'A', 'Y', 'N', 'L'}};
constexpr uint32_t file_format_version = 1;
constexpr std::size_t file_header_size = 8;
constexpr std::array<uint8_t, 4> frame_magic{{'A', 'Y', 'N', 'F'}};
constexpr std::size_t frame_header_size = 20;
constexpr std::size_t frame_header_crc_at = 16;
constexpr std::size_t frame_trailer_size = 4;
// Payloads are checked through a buffer of this size on the stack.
constexpr std::size_t chunk_size = 4096;

constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < table.size(); ++index) {
        uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
        table[index] = crc;
    }
    return table;
}

constexpr std::array<uint32_t, 256> crc_table = make_crc_table();

// CRC-32/ISO-HDLC; passing a previous result continues it over more bytes.
uint32_t crc32(const uint8_t *data, std::size_t size, uint32_t previous = 0) {
    uint32_t crc = ~previous;
    for (std::size_t index = 0; index < size; ++index) {
        crc = crc_table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

void store_le32(uint8_t *out, uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

void store_le64(uint8_t *out, uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

uint32_t load_le32(const uint8_t *in) {
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<uint32_t>(in[index]) << (8U * index);
    }
    return value;
}

uint64_t load_le64(const uint8_t *in) {
    uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<uint64_t>(in[index]) << (8U * index);
    }
    return value;
}

std::array<uint8_t, file_header_size> file_header() {
    std::array<uint8_t, file_header_size> header{};
    std::memcpy(header.data(), file_magic.data(), file_magic.size());
    store_le32(header.data() + file_magic.size(), file_format_version);
    return header;
}

} // namespace

uint64_t FileLog::append_bytes(uint32_t payload_bytes) {
    return static_cast<uint64_t>(payload_bytes) + frame_header_size + frame_trailer_size;
}

// Files owns platform namespace exclusion; the log owns format admission and scan.
// Snapshot names cannot alias the main log: a normal snapshot replaces their bytes.
FileLog::Opened FileLog::open(const char *directory, const char *file_name) {
    if (!directory || !file_name || !file_name[0] || std::strchr(file_name, '/') ||
        std::strcmp(file_name, ".") == 0 || std::strcmp(file_name, "..") == 0 ||
        std::strcmp(file_name, snapshot_name(0)) == 0 ||
        std::strcmp(file_name, snapshot_name(1)) == 0) {
        return {OpenStatus::file_error, EINVAL};
    }
    if (opened_) {
        return {OpenStatus::file_error, EBUSY};
    }
    if (std::strlen(file_name) >= file_name_.size()) {
        return {OpenStatus::file_error, ENAMETOOLONG};
    }
    const auto excluded = files_.begin(directory);
    if (excluded != 0) {
        return {excluded == EWOULDBLOCK ? OpenStatus::in_use : OpenStatus::file_error, excluded};
    }
    opened_.emplace(OpenFiles{Files::invalid_handle});
    std::memcpy(file_name_.data(), file_name, std::strlen(file_name) + 1U);
    const auto compatible = check_snapshot_schemas();
    if (compatible.status != OpenStatus::ok) {
        close();
        return compatible;
    }
    const auto error = files_.open(file_name, Files::Mode::read_write_create, opened_->log);
    bool initialized = false;
    Opened opened =
        error != 0 ? Opened{OpenStatus::file_error, error} : check_file_header(initialized);
    if (opened.status == OpenStatus::ok) {
        opened = scan();
        if (opened.status == OpenStatus::ok && !initialized && !damaged_) {
            // A readable inherited prefix is not proof of prior synchronization.
            // Reads stay read-only; confirm every inherited byte before mutation.
            sync_failed_ = true;
            uncertain_start_ = 0;
        }
    }
    if (opened.status != OpenStatus::ok) {
        close();
    }
    return opened;
}

void FileLog::close() {
    if (log_handle() != Files::invalid_handle) {
        files_.close(log_handle());
    }
    if (opened_) {
        files_.end();
        opened_.reset();
    }
    writer_epoch_ = 1;
    file_size_ = 0;
    end_ = 0;
    damaged_ = false;
    tail_dirty_ = false;
    sync_failed_ = false;
    uncertain_start_ = UINT64_MAX;
    read_from_ = 0;
    read_max_seq_ = 0;
}

// Positive short reads are normal file-port behavior, not an end-of-file proof.
int FileLog::read_up_to(int handle, uint64_t at, uint8_t *out, std::size_t size,
                        std::size_t &total) const {
    total = 0;
    while (total < size) {
        std::size_t count = 0;
        const auto error = files_.read(handle, at + total, out + total, size - total, count);
        if (error != 0) {
            return error;
        }
        if (count > size - total) {
            return EIO;
        }
        if (count == 0) {
            break;
        }
        total += count;
    }
    return 0;
}

// A checked frame promises all requested bytes; an early EOF is an I/O error.
int FileLog::read_exact(int handle, uint64_t at, uint8_t *out, std::size_t size) const {
    std::size_t count = 0;
    const auto error = read_up_to(handle, at, out, size, count);
    return error != 0 ? error : count == size ? 0 : EIO;
}

// The two retained slots may contain the only Core prefix after log trimming.
// Probe only compatibility; full activation/CRC/semantic validation stays in Core.
FileLog::Opened FileLog::check_snapshot_schemas() const {
    constexpr uint32_t snapshot_slots = 2;
    for (uint32_t slot = 0; slot < snapshot_slots; ++slot) {
        int snapshot = Files::invalid_handle;
        const auto opened =
            files_.open(snapshot_name(slot), Files::Mode::read_existing_no_follow, snapshot);
        if (opened != 0) {
            if (opened == ENOENT) {
                continue;
            }
            return {OpenStatus::file_error, opened};
        }
        std::array<uint8_t, AYNI_STORAGE_SCHEMA_PREFIX_BYTES> prefix{};
        std::size_t size = 0;
        const auto error = read_up_to(snapshot, 0, prefix.data(), prefix.size(), size);
        files_.close(snapshot);
        if (error != 0) {
            return {OpenStatus::file_error, error};
        }
        uint32_t schema = 0;
        if (ayni_storage_read_schema(prefix.data(), size, &schema) &&
            schema != AYNI_STORAGE_SCHEMA_CURRENT) {
            return {OpenStatus::not_a_log, 0};
        }
    }
    return {OpenStatus::ok, 0};
}

FileLog::Opened FileLog::check_file_header(bool &initialized) {
    initialized = false;
    // Preserve file-level exclusion for adapters with legacy file-only writers.
    const auto locked = files_.lock(log_handle());
    if (locked != 0) {
        return {locked == EWOULDBLOCK ? OpenStatus::in_use : OpenStatus::file_error, locked};
    }
    const auto stat_error = files_.size(log_handle(), file_size_);
    if (stat_error != 0) {
        return {OpenStatus::file_error, stat_error};
    }

    const std::array<uint8_t, file_header_size> expected = file_header();
    std::array<uint8_t, file_header_size> found{};
    const std::size_t present =
        static_cast<std::size_t>(std::min<uint64_t>(file_size_, found.size()));
    int error = read_exact(log_handle(), 0, found.data(), present);
    if (error != 0) {
        return {OpenStatus::file_error, error};
    }
    // A power cut while the header was first written can leave a prefix of
    // it, or, where the length reached the disk before the bytes, zeros.
    const bool unwritten =
        file_size_ <= found.size() &&
        std::all_of(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(present),
                    [](uint8_t byte) { return byte == 0U; });
    if (!unwritten && std::memcmp(found.data(), expected.data(), present) != 0) {
        return {OpenStatus::not_a_log, 0};
    }
    if (!unwritten && present == expected.size()) {
        return {OpenStatus::ok, 0};
    }
    // A new file, or one whose creation a power cut tore: nothing is appended
    // before the header is synced, so it holds no batch.
    error = files_.write(log_handle(), 0, expected.data(), expected.size());
    if (error == 0) {
        error = files_.sync_file(log_handle());
    }
    if (error == 0) {
        error = files_.sync_namespace(); // the new directory entry
    }
    if (error != 0) {
        return {OpenStatus::file_error, error};
    }
    file_size_ = expected.size();
    initialized = true;
    return {OpenStatus::ok, 0};
}

FileLog::Opened FileLog::scan() {
    uint64_t at = file_header_size;
    while (at < file_size_) {
        Frame frame{};
        int error = 0;
        const FrameCheck check = check_frame(at, frame, error);
        if (check == FrameCheck::whole) {
            if (frame.payload_size >= AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES) {
                std::array<uint8_t, AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES> header{};
                error = read_exact(log_handle(), frame.payload_at, header.data(), header.size());
                if (error != 0) {
                    return {OpenStatus::file_error, error};
                }
                uint32_t schema = 0;
                if (ayni_storage_read_schema(header.data(), header.size(), &schema) &&
                    schema != AYNI_STORAGE_SCHEMA_CURRENT) {
                    return {OpenStatus::not_a_log,
                            0}; // ADR-0086: refuse old directory before outer-store creation.
                }
                ayni_storage_batch_identity_v1 identity{};
                if (ayni_storage_read_batch_identity(header.data(), header.size(), &identity)) {
                    writer_epoch_ = std::max(writer_epoch_, identity.writer_epoch);
                }
            }
            at = frame.end;
            continue;
        }
        if (check == FrameCheck::bad_header) {
            bool later = false;
            error = find_later_header(at, later);
            damaged_ = later;
        } else if (check == FrameCheck::bad_payload) {
            // The header is sound, so the frame's extent is known: anything
            // after it was appended once it was complete.
            damaged_ = frame.end < file_size_;
        }
        if (error != 0) {
            return {OpenStatus::file_error, error};
        }
        break;
    }
    end_ = at;
    tail_dirty_ = at < file_size_;
    read_from_ = file_header_size;
    read_max_seq_ = 0;
    return {OpenStatus::ok, 0};
}

FileLog::FrameCheck FileLog::check_frame(uint64_t at, Frame &frame, int &error) const {
    if (file_size_ - at < frame_header_size) {
        return FrameCheck::past_end;
    }
    if (!read_header(at, frame, error)) {
        return error != 0 ? FrameCheck::io_error : FrameCheck::bad_header;
    }
    if (frame.end > file_size_) {
        return FrameCheck::past_end;
    }
    if (!check_payload(frame, nullptr, error)) {
        return error != 0 ? FrameCheck::io_error : FrameCheck::bad_payload;
    }
    return FrameCheck::whole;
}

// Each later offset that starts with the frame magic is a candidate. A
// payload that happens to hold a sound frame header can make a torn tail
// look damaged; that restricts the kernel instead of losing a batch.
int FileLog::find_later_header(uint64_t at, bool &found) const {
    found = false;
    std::array<uint8_t, chunk_size> chunk{};
    uint64_t from = at + 1U;
    while (file_size_ - from >= frame_header_size) {
        const auto size =
            static_cast<std::size_t>(std::min<uint64_t>(chunk.size(), file_size_ - from));
        int error = read_exact(log_handle(), from, chunk.data(), size);
        if (error != 0) {
            return error;
        }
        for (std::size_t index = 0; index + frame_magic.size() <= size; ++index) {
            if (std::memcmp(chunk.data() + index, frame_magic.data(), frame_magic.size()) != 0) {
                continue;
            }
            if (file_size_ - (from + index) < frame_header_size) {
                return 0; // no later offset has room for a header either
            }
            Frame frame{};
            if (read_header(from + index, frame, error)) {
                found = true;
                return 0;
            }
            if (error != 0) {
                return error;
            }
        }
        // A magic that straddles the end of this chunk starts the next one.
        from += size - (frame_magic.size() - 1U);
    }
    return 0;
}

bool FileLog::read_header(uint64_t at, Frame &frame, int &error) const {
    std::array<uint8_t, frame_header_size> header{};
    error = read_exact(log_handle(), at, header.data(), header.size());
    if (error != 0 || std::memcmp(header.data(), frame_magic.data(), frame_magic.size()) != 0 ||
        load_le32(header.data() + frame_header_crc_at) !=
            crc32(header.data(), frame_header_crc_at)) {
        return false;
    }
    frame.payload_size = load_le32(header.data() + 4);
    frame.commit_seq = load_le64(header.data() + 8);
    frame.payload_at = at + frame_header_size;
    frame.end = frame.payload_at + frame.payload_size + frame_trailer_size;
    return true;
}

// Into out when it is not null, else through a chunk on the stack.
bool FileLog::check_payload(const Frame &frame, uint8_t *out, int &error) const {
    std::array<uint8_t, chunk_size> chunk{};
    uint32_t crc = 0;
    for (uint32_t done = 0; done < frame.payload_size;) {
        uint8_t *into = out != nullptr ? out : chunk.data();
        const uint32_t size = out != nullptr
                                  ? frame.payload_size
                                  : std::min<uint32_t>(chunk_size, frame.payload_size - done);
        error = read_exact(log_handle(), frame.payload_at + done, into, size);
        if (error != 0) {
            return false;
        }
        crc = crc32(into, size, crc);
        done += size;
    }
    std::array<uint8_t, frame_trailer_size> trailer{};
    error =
        read_exact(log_handle(), frame.end - frame_trailer_size, trailer.data(), trailer.size());
    return error == 0 && load_le32(trailer.data()) == crc;
}

FileLog::Result FileLog::append(uint64_t commit_seq, const uint8_t *data, uint32_t size) {
    if (!is_open()) {
        return {AYNI_WRITE_FAILED, 0};
    }
    if (sync_failed_ && !rescan_after_unknown()) {
        return {AYNI_WRITE_UNKNOWN, 0};
    }
    if (damaged_ || (tail_dirty_ && !truncate_tail())) {
        return {AYNI_WRITE_FAILED, 0};
    }
    // A retry can follow an UNKNOWN whose full frame reached the medium.
    // Compare every byte, rather than a checksum, before acknowledging it.
    for (uint64_t at = file_header_size; at < end_;) {
        Frame frame{};
        int error = 0;
        if (!read_header(at, frame, error) || frame.end > end_) {
            return {AYNI_WRITE_FAILED, 0};
        }
        if (frame.commit_seq == commit_seq) {
            bool same_identity = true;
            ayni_storage_batch_identity_v1 requested{};
            if (frame.payload_size >= AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES &&
                ayni_storage_read_batch_identity(data, size, &requested)) {
                std::array<uint8_t, AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES> found{};
                if (read_exact(log_handle(), frame.payload_at, found.data(), found.size()) != 0) {
                    return {AYNI_WRITE_FAILED, 0};
                }
                ayni_storage_batch_identity_v1 stored{};
                same_identity =
                    ayni_storage_read_batch_identity(found.data(), found.size(), &stored) &&
                    stored.writer_epoch == requested.writer_epoch &&
                    stored.transition_id == requested.transition_id;
            }
            if (same_identity) {
                if (!same_payload(frame, data, size, error)) {
                    return {AYNI_WRITE_FAILED, 0};
                }
                return {AYNI_WRITE_OK, 0};
            }
        }
        at = frame.end;
    }
    std::array<uint8_t, frame_header_size> header{};
    std::memcpy(header.data(), frame_magic.data(), frame_magic.size());
    store_le32(header.data() + 4, size);
    store_le64(header.data() + 8, commit_seq);
    store_le32(header.data() + frame_header_crc_at, crc32(header.data(), frame_header_crc_at));
    std::array<uint8_t, frame_trailer_size> trailer{};
    store_le32(trailer.data(), crc32(data, size));

    const uint64_t payload_at = end_ + frame_header_size;
    if (files_.write(log_handle(), end_, header.data(), header.size()) != 0 ||
        files_.write(log_handle(), payload_at, data, size) != 0 ||
        files_.write(log_handle(), payload_at + size, trailer.data(), trailer.size()) != 0) {
        // What reached the file is an incomplete frame past end_: never read,
        // and truncated before the next append.
        tail_dirty_ = true;
        return {AYNI_WRITE_FAILED, 0};
    }
    if (!sync()) {
        // The frame may be on the device or not. Reads keep ending at end_;
        // reopening the log finds out.
        tail_dirty_ = true;
        return {AYNI_WRITE_UNKNOWN, 0};
    }
    end_ = payload_at + size + frame_trailer_size;
    return {AYNI_WRITE_OK, 0};
}

FileLog::Result FileLog::read_prefix(uint64_t from_commit_seq, uint8_t *buffer, uint32_t size) {
    if (!is_open()) {
        return {AYNI_WRITE_FAILED, 0};
    }
    uint64_t at = file_header_size;
    uint64_t max_seq = 0;
    if (from_commit_seq > read_max_seq_) {
        at = read_from_;
        max_seq = read_max_seq_;
    }
    uint32_t written = 0;
    bool stopped = false;
    while (at < end_) {
        Frame frame{};
        int error = 0;
        // Frames before end_ were whole when opened or written; checking
        // them again catches the medium changing since.
        if (!read_header(at, frame, error) || frame.end > end_) {
            stopped = true;
            break;
        }
        if (frame.commit_seq >= from_commit_seq) {
            if (frame.payload_size > size - written ||
                !check_payload(frame, buffer + written, error)) {
                if (written == 0U && frame.payload_size > size) {
                    return {AYNI_READ_BUFFER_TOO_SMALL, frame.payload_size};
                }
                stopped = true;
                break;
            }
            written += frame.payload_size;
        }
        max_seq = std::max(max_seq, frame.commit_seq);
        at = frame.end;
    }
    read_from_ = at;
    read_max_seq_ = max_seq;
    // In a damaged log, end_ is where the damaged frame starts.
    if ((stopped || damaged_) && written == 0U) {
        return {AYNI_WRITE_FAILED, 0};
    }
    return {AYNI_WRITE_OK, written};
}

FileLog::Result FileLog::read_log(uint64_t cursor, uint8_t *buffer, uint32_t size) {
    if (!is_open()) {
        return {AYNI_WRITE_FAILED, 0};
    }
    uint64_t at = cursor == 0U ? file_header_size : cursor - 1U;
    if (at < file_header_size || at > end_) {
        return {AYNI_WRITE_FAILED, 0};
    }
    uint32_t written = 0;
    while (at < end_) {
        Frame frame{};
        int error = 0;
        if (!read_header(at, frame, error) || frame.end > end_) {
            return written == 0U ? Result{AYNI_WRITE_FAILED, 0}
                                 : Result{AYNI_WRITE_OK, written, at + 1U};
        }
        if (frame.payload_size > size - written) {
            return written == 0U ? Result{AYNI_READ_BUFFER_TOO_SMALL, frame.payload_size}
                                 : Result{AYNI_WRITE_OK, written, at + 1U};
        }
        if (!check_payload(frame, buffer + written, error)) {
            return written == 0U ? Result{AYNI_WRITE_FAILED, 0}
                                 : Result{AYNI_WRITE_OK, written, at + 1U};
        }
        written += frame.payload_size;
        at = frame.end;
    }
    if (damaged_ && written == 0U) {
        return {AYNI_WRITE_FAILED, 0};
    }
    return {AYNI_WRITE_OK, written, at + 1U};
}

bool FileLog::same_payload(const Frame &frame, const uint8_t *data, uint32_t size,
                           int &error) const {
    if (frame.payload_size != size || !check_payload(frame, nullptr, error)) {
        return false;
    }
    std::array<uint8_t, chunk_size> chunk{};
    for (uint32_t done = 0; done < size;) {
        const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(chunk.size()), size - done);
        error = read_exact(log_handle(), frame.payload_at + done, chunk.data(), count);
        if (error != 0 || std::memcmp(chunk.data(), data + done, count) != 0) {
            return false;
        }
        done += count;
    }
    return true;
}

bool FileLog::rescan_after_unknown() {
    // Keep both locks while resolving the uncertain frame. After EIO,
    // readable cached pages may already be clean: another sync alone does
    // not prove durability. Rewrite every uncertain byte before syncing.
    const uint64_t rewrite_from = uncertain_start_ == UINT64_MAX ? 0U : uncertain_start_;
    const auto keep_uncertain_prefix = [this, rewrite_from] {
        sync_failed_ = true;
        uncertain_start_ = std::min(uncertain_start_, rewrite_from);
        end_ = std::min(end_, std::max<uint64_t>(file_header_size, rewrite_from));
        tail_dirty_ = true;
        return false;
    };
    damaged_ = false;
    tail_dirty_ = false;
    bool initialized = false;
    const Opened opened = check_file_header(initialized);
    if (opened.status != OpenStatus::ok || scan().status != OpenStatus::ok || damaged_) {
        return keep_uncertain_prefix();
    }
    if (tail_dirty_ && files_.truncate(log_handle(), end_) != 0) {
        return keep_uncertain_prefix();
    }
    std::array<uint8_t, chunk_size> chunk{};
    for (uint64_t at = std::min(rewrite_from, end_); at < end_;) {
        const auto count = static_cast<std::size_t>(std::min<uint64_t>(chunk.size(), end_ - at));
        if (read_exact(log_handle(), at, chunk.data(), count) != 0 ||
            files_.write(log_handle(), at, chunk.data(), count) != 0) {
            return keep_uncertain_prefix();
        }
        at += count;
    }
    // A readable replacement inode does not prove that rename, or the
    // original creation, survives power loss. Confirm its binding too.
    if (files_.sync_file(log_handle()) != 0 || files_.sync_namespace() != 0) {
        return keep_uncertain_prefix();
    }
    tail_dirty_ = false;
    sync_failed_ = false;
    uncertain_start_ = UINT64_MAX;
    return true;
}

FileLog::Result FileLog::fence(uint32_t minimum_epoch) {
    if (!is_open()) {
        return {AYNI_FENCE_UNAVAILABLE, 0};
    }
    if (writer_epoch_ == UINT32_MAX || minimum_epoch == 0U ||
        (sync_failed_ && !rescan_after_unknown()) || damaged_) {
        return {AYNI_FENCE_UNAVAILABLE, 0};
    }
    writer_epoch_ = std::max(minimum_epoch, writer_epoch_ + 1U);
    // This runs after every earlier request on the sole I/O thread: no
    // accepted old append, snapshot or trim can still touch the medium.
    return {AYNI_FENCE_DRAINED, writer_epoch_};
}

const char *FileLog::snapshot_name(uint32_t slot) const {
    return slot == 0U ? "snapshot_0" : "snapshot_1";
}

FileLog::Result FileLog::write_snapshot(uint32_t slot, uint32_t writer_epoch, uint64_t offset,
                                        const uint8_t *data, uint32_t size) {
    if (!is_open()) {
        return {AYNI_WRITE_FAILED, 0};
    }
    constexpr uint64_t snapshot_limit = 8U * 1024U * 1024U;
    if (slot > 1U || writer_epoch < writer_epoch_ || damaged_ || offset > snapshot_limit ||
        size > snapshot_limit - offset) {
        return {AYNI_WRITE_FAILED, 0};
    }
    if (sync_failed_ && !rescan_after_unknown()) {
        return {AYNI_WRITE_UNKNOWN, 0};
    }
    int snapshot = Files::invalid_handle;
    if (files_.open(snapshot_name(slot), Files::Mode::read_write_create, snapshot) != 0) {
        return {AYNI_WRITE_FAILED, 0};
    }
    uint64_t present = 0;
    int error = files_.size(snapshot, present);
    bool same = error == 0 && offset <= present && size <= present - offset;
    std::array<uint8_t, chunk_size> chunk{};
    for (uint32_t done = 0; same && done < size;) {
        const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(chunk.size()), size - done);
        error = read_exact(snapshot, offset + done, chunk.data(), count);
        same = error == 0 && std::memcmp(chunk.data(), data + done, count) == 0;
        done += count;
    }
    if (error == 0 && !same) {
        if (offset == 0U) {
            error = files_.truncate(snapshot, 0);
        } else if (offset > present || offset + size < present) {
            error = EINVAL;
        } else if (offset < present) {
            const uint64_t partial = present - offset;
            for (uint64_t done = 0; error == 0 && done < partial;) {
                const uint32_t count =
                    static_cast<uint32_t>(std::min<uint64_t>(chunk.size(), partial - done));
                error = read_exact(snapshot, offset + done, chunk.data(), count);
                if (error == 0 && std::memcmp(chunk.data(), data + done, count) != 0) {
                    error = EINVAL;
                }
                done += count;
            }
        }
    }
    if (error == 0) {
        error = files_.write(snapshot, offset, data, size);
    }
    if (error != 0) {
        files_.close(snapshot);
        return {error == EINVAL ? AYNI_WRITE_FAILED : AYNI_WRITE_UNKNOWN, 0};
    }
    const bool durable = files_.sync_file(snapshot) == 0 && files_.sync_namespace() == 0;
    files_.close(snapshot);
    return {durable ? AYNI_WRITE_OK : AYNI_WRITE_UNKNOWN, 0};
}

FileLog::Result FileLog::read_snapshot(uint32_t slot, uint64_t offset, uint8_t *buffer,
                                       uint32_t size) {
    if (!is_open()) {
        return {AYNI_WRITE_FAILED, 0};
    }
    if (slot > 1U) {
        return {AYNI_WRITE_FAILED, 0};
    }
    int snapshot = Files::invalid_handle;
    const auto opened = files_.open(snapshot_name(slot), Files::Mode::read_existing, snapshot);
    if (opened != 0) {
        return {opened == ENOENT ? AYNI_WRITE_OK : AYNI_WRITE_FAILED, 0};
    }
    uint64_t present = 0;
    if (files_.size(snapshot, present) != 0) {
        files_.close(snapshot);
        return {AYNI_WRITE_FAILED, 0};
    }
    const uint32_t count =
        offset >= present ? 0U : static_cast<uint32_t>(std::min<uint64_t>(present - offset, size));
    const int error = read_exact(snapshot, offset, buffer, count);
    files_.close(snapshot);
    return {error == 0 ? AYNI_WRITE_OK : AYNI_WRITE_FAILED, error == 0 ? count : 0U};
}

FileLog::Result FileLog::trim_prefix(uint32_t writer_epoch, uint64_t commit_seq,
                                     uint64_t transition_id) {
    if (!is_open()) {
        return {AYNI_WRITE_FAILED, 0};
    }
    if (writer_epoch < writer_epoch_ || damaged_) {
        return {AYNI_WRITE_FAILED, 0};
    }
    if (sync_failed_ && !rescan_after_unknown()) {
        return {AYNI_WRITE_UNKNOWN, 0};
    }
    uint64_t retained = end_;
    for (uint64_t at = file_header_size; at < end_;) {
        Frame frame{};
        int error = 0;
        if (!read_header(at, frame, error) || frame.end > end_) {
            return {AYNI_WRITE_FAILED, 0};
        }
        if (frame.commit_seq == commit_seq &&
            frame.payload_size >= AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES) {
            std::array<uint8_t, AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES> header{};
            if (read_exact(log_handle(), frame.payload_at, header.data(), header.size()) != 0) {
                return {AYNI_WRITE_FAILED, 0};
            }
            ayni_storage_batch_identity_v1 identity{};
            if (ayni_storage_read_batch_identity(header.data(), header.size(), &identity) &&
                identity.writer_epoch == writer_epoch && identity.transition_id == transition_id &&
                check_payload(frame, nullptr, error)) {
                retained = at;
                break;
            }
        }
        at = frame.end;
    }
    if (retained == end_) {
        return {AYNI_WRITE_FAILED, 0};
    }
    if (retained == file_header_size) {
        return finish_trim();
    }
    std::array<char, 80> replacement{};
    static_cast<void>(
        std::snprintf(replacement.data(), replacement.size(), "%s.rotation", file_name_.data()));
    int next = Files::invalid_handle;
    if (files_.open(replacement.data(), Files::Mode::replace, next) != 0) {
        return {AYNI_WRITE_FAILED, 0};
    }
    int error = files_.lock(next);
    const auto header = file_header();
    if (error == 0) {
        error = files_.write(next, 0, header.data(), header.size());
    }
    std::array<uint8_t, chunk_size> chunk{};
    for (uint64_t done = 0; error == 0 && done < end_ - retained;) {
        const std::size_t count =
            static_cast<std::size_t>(std::min<uint64_t>(chunk.size(), end_ - retained - done));
        error = read_exact(log_handle(), retained + done, chunk.data(), count);
        if (error == 0) {
            error = files_.write(next, file_header_size + done, chunk.data(), count);
        }
        done += count;
    }
    if (error == 0) {
        error = files_.sync_file(next);
    }
    if (error == 0) {
        error = files_.rename(replacement.data(), file_name_.data());
    }
    if (error != 0) {
        files_.close(next);
        static_cast<void>(files_.remove(replacement.data()));
        return {AYNI_WRITE_FAILED, 0};
    }
    // Both file objects remain excluded across replacement. Native keeps the
    // old and new inode locks so legacy file-only writers cannot enter here.
    files_.close(log_handle());
    opened_->log = next;
    end_ = file_header_size + end_ - retained;
    file_size_ = end_;
    read_from_ = file_header_size;
    read_max_seq_ = 0;
    tail_dirty_ = false;
    return finish_trim();
}

FileLog::Result FileLog::finish_trim() {
    if (files_.sync_namespace() != 0) {
        // The new inode may still have only a cached directory binding.
        // Every subsequent mutating port, including fencing, must confirm
        // the complete prefix and its binding before admitting new work.
        sync_failed_ = true;
        uncertain_start_ = 0;
        return {AYNI_WRITE_UNKNOWN, 0};
    }
    return {AYNI_WRITE_OK, 0};
}

bool FileLog::free_bytes(uint64_t &available) const {
    if (!is_open()) {
        available = 0;
        return false;
    }
    uint64_t actual = 0;
    if (files_.free_bytes(actual) != 0) {
        available = 0;
        return false;
    }
    constexpr uint64_t usable_limit = 64U * 1024U * 1024U;
    available = std::min(usable_limit, actual);
    return true;
}

bool FileLog::truncate_tail() {
    if (files_.truncate(log_handle(), end_) != 0 || !sync()) {
        return false;
    }
    tail_dirty_ = false;
    return true;
}

bool FileLog::sync() {
    if (files_.sync_file(log_handle()) != 0) {
        sync_failed_ = true;
        uncertain_start_ = std::min(uncertain_start_, end_);
        return false;
    }
    sync_failed_ = false;
    uncertain_start_ = UINT64_MAX;
    return true;
}

} // namespace ayni::hosts::storage

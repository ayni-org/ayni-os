#include "conversation_store.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
namespace ayni::runtime {
namespace {
constexpr char filename[] = "conversation.bin";
constexpr char temporary[] = ".conversation.pending";
constexpr uint64_t format_version = 1;
constexpr uint32_t version_offset = 4, length_offset = 8, pending_offset = 16;
constexpr uint32_t u32_bytes = 4, u64_bytes = 8, byte_bits = 8;
void put(uint8_t *p, uint64_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        p[i] = static_cast<uint8_t>(v);
        v >>= byte_bits;
    }
}
uint64_t get(const uint8_t *p, uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
        v |= uint64_t{p[i]} << (i * byte_bits);
    }
    return v;
}
bool private_regular(int fd) {
    struct stat st {};
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & 077) == 0 && st.st_nlink == 1;
}
} // namespace
ConversationStore::~ConversationStore() {
    close();
}
ayni_status_t ConversationStore::open(const char *directory, SavedConversation &state) {
    if (directory_ >= 0) {
        return AYNI_INVALID_STATE;
    }
    directory_ = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_ < 0) {
        return AYNI_STORAGE_FAILED;
    }
    const auto status = read(state);
    if (status != AYNI_OK && status != AYNI_NOT_FOUND) {
        close();
        return status;
    }
    try {
        worker_ = std::thread([this] { run(); });
    } catch (const std::system_error &) {
        close();
        return AYNI_CAPACITY_EXCEEDED;
    }
    return status;
}
ayni_status_t ConversationStore::read(SavedConversation &state) {
    const int fd = openat(directory_, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT ? AYNI_NOT_FOUND : AYNI_STORAGE_FAILED;
    }
    struct stat st {};
    if (!private_regular(fd) || fstat(fd, &st) != 0 || st.st_size < header_size ||
        st.st_size > file_size_max) {
        ::close(fd);
        return AYNI_STORAGE_FAILED;
    }
    const auto size = static_cast<uint32_t>(st.st_size);
    uint32_t at = 0;
    while (at < size) {
        const auto n = ::read(fd, bytes_.data() + at, size - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ::close(fd);
            return AYNI_STORAGE_FAILED;
        }
        at += static_cast<uint32_t>(n);
    }
    if (::close(fd) != 0) {
        return AYNI_STORAGE_FAILED;
    }
    if (std::memcmp(bytes_.data(), "AYNF", version_offset) != 0 ||
        get(bytes_.data() + version_offset, u32_bytes) != format_version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (get(bytes_.data() + length_offset, u64_bytes) != size - header_size) {
        return AYNI_INVALID_ARGUMENT;
    }
    SavedConversation candidate;
    candidate.pending_goal = get(bytes_.data() + pending_offset, u64_bytes);
    const auto status = strategies::Conversation::decode(
        {bytes_.data() + header_size, size - header_size}, candidate.record);
    if (status == AYNI_OK) {
        state = candidate;
    }
    return status;
}
ayni_status_t ConversationStore::save(const SavedConversation &state) {
    strategies::Encoded encoded;
    const auto status = strategies::Conversation::encode(state.record, encoded);
    if (status != AYNI_OK) {
        return status;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (directory_ < 0) {
        return AYNI_INVALID_STATE;
    }
    if (phase_ != Phase::idle) {
        return AYNI_BUSY;
    }
    std::memcpy(bytes_.data(), "AYNF", version_offset);
    put(bytes_.data() + version_offset, format_version, u32_bytes);
    put(bytes_.data() + length_offset, encoded.size, u64_bytes);
    put(bytes_.data() + pending_offset, state.pending_goal, u64_bytes);
    std::memcpy(bytes_.data() + header_size, encoded.bytes.data(), encoded.size);
    size_ = header_size + encoded.size;
    phase_ = Phase::queued;
    wake_.notify_one();
    return AYNI_OK;
}
// Replace only a completely written, synced candidate. The old record survives
// pre-rename errors. Post-rename sync errors remain failures, never fabricated success.
ayni_status_t ConversationStore::write() {
    if (unlinkat(directory_, temporary, 0) != 0 && errno != ENOENT) {
        return AYNI_STORAGE_FAILED;
    }
    const int fd =
        openat(directory_, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return AYNI_STORAGE_FAILED;
    }
    uint32_t at = 0;
    while (at < size_) {
        const auto n = ::write(fd, bytes_.data() + at, size_ - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ::close(fd);
            return AYNI_STORAGE_FAILED;
        }
        at += static_cast<uint32_t>(n);
    }
    const bool synced = fdatasync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!synced || !closed || renameat(directory_, temporary, directory_, filename) != 0 ||
        fsync(directory_) != 0) {
        return AYNI_STORAGE_FAILED;
    }
    return AYNI_OK;
}
void ConversationStore::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return phase_ == Phase::queued || phase_ == Phase::stopping; });
        if (phase_ == Phase::stopping) {
            return;
        }
        phase_ = Phase::writing;
        lock.unlock();
        const auto result = write();
        lock.lock();
        status_ = result;
        phase_ = Phase::done;
        wake_.notify_all();
    }
}
bool ConversationStore::poll(ayni_status_t &status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != Phase::done) {
        return false;
    }
    status = status_;
    phase_ = Phase::idle;
    return true;
}
void ConversationStore::close() {
    if (worker_.joinable()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock,
                       [this] { return phase_ != Phase::queued && phase_ != Phase::writing; });
            phase_ = Phase::stopping;
        }
        wake_.notify_one();
        worker_.join();
    }
    if (directory_ >= 0) {
        ::close(directory_);
    }
    directory_ = -1;
    phase_ = Phase::idle;
}
} // namespace ayni::runtime

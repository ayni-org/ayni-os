// Native outer conversation storage (ADR-0057). The Host's exclusive directory
// lock is held before open and until close. Only the I/O worker writes files.
#ifndef AYNI_RUNTIME_CONVERSATION_STORE_H
#define AYNI_RUNTIME_CONVERSATION_STORE_H
#include <array>
#include <ayni/strategies/builtin.h>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace ayni::runtime {
struct SavedConversation {
    strategies::ConversationRecord record;
    uint64_t pending_goal = 0; // Zero when no turn is outstanding; otherwise exact Goal ID.
};
class ConversationStore {
  public:
    ConversationStore() = default;
    ~ConversationStore();
    // Startup-only read. NOT_FOUND preserves state; a new store is explicit.
    ayni_status_t open(const char *directory, SavedConversation &state);
    // One bounded request. BUSY retains nothing; accepted data is copied.
    ayni_status_t save(const SavedConversation &state);
    // True consumes a completed write; false leaves status unchanged.
    bool poll(ayni_status_t &status);
    void close(); // Joins worker; call only after consuming the last save.
  private:
    enum class Phase { idle, queued, writing, done, stopping };
    static constexpr uint32_t header_size = 24;
    static constexpr uint32_t file_size_max = header_size + strategies::state_bytes_max;
    using Buffer = std::array<uint8_t, file_size_max>;
    ayni_status_t read(SavedConversation &state);
    ayni_status_t write();
    void run();
    int directory_ = -1;
    Buffer bytes_{};
    uint32_t size_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    Phase phase_ = Phase::idle;
    ayni_status_t status_ = AYNI_OK;
    std::thread worker_;
};
} // namespace ayni::runtime
#endif

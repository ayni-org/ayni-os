// SQLite implements bounded asynchronous values only. Shared Session/FileBodies own
// conversation state, result representation, cache bounds and collection.
#ifndef AYNI_RUNTIME_SQLITE_VALUES_H
#define AYNI_RUNTIME_SQLITE_VALUES_H
#include <array>
#include <ayni/hosts/storage/file_value.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
struct sqlite3;
namespace ayni::runtime {
class SqliteValues {
  public:
    SqliteValues() = default;
    ~SqliteValues();
    // Under the Host directory lock, before Core starts. Unsupported formats stay intact.
    ayni_status_t open(const char *directory);
    hosts::storage::FileValuePort files() { return {this, submit}; }
    void close(); // Drains accepted callbacks, joins, then closes; never called from worker.

  private:
    enum class State { closed, idle, working, stopping };
    static ayni_submit_result_t submit(void *, const hosts::storage::FileValueRequest *);
    hosts::storage::FileValueResult value(const hosts::storage::FileValueRequest &);
    void run();
    int directory_ = -1;
    sqlite3 *database_ = nullptr;
    static constexpr size_t queue_capacity = 2; // Session value plus one background body quantum.
    std::array<hosts::storage::FileValueRequest, queue_capacity> pending_{};
    size_t queued_ = 0; // Live prefix; metadata only, each accepted buffer remains borrowed.
    State phase_ = State::closed;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
};
} // namespace ayni::runtime
#endif

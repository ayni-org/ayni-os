#include "console.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
namespace ayni::cli {
namespace {
constexpr size_t queue_capacity = 64, text_capacity = 65536;
constexpr auto drain_timeout = std::chrono::seconds(2);
// Untrusted model/user bytes cannot inject terminal control sequences.
ayni_status_t write(const std::string &body) {
    try {
        for (const char value : body) {
            const auto byte = static_cast<unsigned char>(value);
            std::cout.put((byte >= 32 && byte != 127) || value == '\n' || value == '\t' ? value
                                                                                        : '?');
        }
        std::cout << std::endl;
        return std::cout.good() ? AYNI_OK : AYNI_RESULT_UNKNOWN;
    } catch (...) {
        return AYNI_RESULT_UNKNOWN;
    }
}
} // namespace
struct Console::Impl {
    struct Job {
        std::string body;
        bool done = false;
        ayni_status_t status = AYNI_OK;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Job>> queue;
    bool closing = false, finished = false;
    ayni_status_t status = AYNI_OK;
    std::thread worker;
    Impl() : worker([this] { run(); }) {}
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return closing || !queue.empty(); });
            if (queue.empty() && closing) {
                finished = true;
                changed.notify_all();
                return;
            }
            const auto job = queue.front();
            queue.pop_front();
            changed.notify_all();
            const auto prior = status;
            lock.unlock();
            const auto result = prior == AYNI_OK ? write(job->body) : prior;
            lock.lock();
            job->status = result;
            job->done = true;
            if (result != AYNI_OK) {
                status = result;
            }
            changed.notify_all();
        }
    }
};
Console::Console() : impl_(std::make_unique<Impl>()) {}
Console::~Console() {
    if (impl_ && close() != AYNI_OK) {
        static_cast<void>(impl_.release());
        std::_Exit(1); // Failed CLI drain cannot use standard-stream exit flushing.
    }
}
ayni_status_t Console::close() {
    auto &s = *impl_;
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        s.closing = true;
        s.changed.notify_all();
        if (!s.changed.wait_for(lock, drain_timeout, [&] { return s.finished; })) {
            return AYNI_BUSY;
        }
    }
    if (s.worker.joinable()) {
        s.worker.join();
    }
    return status();
}
ayni_status_t Console::post(const std::string &body) {
    if (body.size() > text_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    try {
        auto &s = *impl_;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.closing) {
            return AYNI_NOT_READY;
        }
        if (s.status != AYNI_OK) {
            return s.status;
        }
        if (s.queue.size() == queue_capacity) {
            return AYNI_BUSY;
        }
        s.queue.push_back(std::make_shared<Impl::Job>(Impl::Job{body, false, AYNI_OK}));
        s.changed.notify_all();
        return AYNI_OK;
    } catch (const std::bad_alloc &) {
        return AYNI_CAPACITY_EXCEEDED;
    }
}
ayni_status_t Console::present(const std::string &body) {
    if (body.size() > text_capacity) {
        return AYNI_UNSUPPORTED;
    }
    try {
        auto &s = *impl_;
        std::unique_lock<std::mutex> lock(s.mutex);
        s.changed.wait(lock, [&] {
            return s.closing || s.status != AYNI_OK || s.queue.size() < queue_capacity;
        });
        if (s.closing || s.status != AYNI_OK) {
            return AYNI_UNSUPPORTED;
        }
        const auto job = std::make_shared<Impl::Job>(Impl::Job{body, false, AYNI_OK});
        s.queue.push_back(job);
        s.changed.notify_all();
        s.changed.wait(lock, [&] { return job->done; });
        return job->status;
    } catch (const std::bad_alloc &) {
        return AYNI_UNSUPPORTED;
    } // No job escaped before allocation failed.
}
ayni_status_t Console::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status;
}
} // namespace ayni::cli

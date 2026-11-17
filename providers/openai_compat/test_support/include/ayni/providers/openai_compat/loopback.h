// Local protocol fixture for ADR-0055. Never contacts an external service.
#ifndef AYNI_OPENAI_COMPAT_LOOPBACK_H
#define AYNI_OPENAI_COMPAT_LOOPBACK_H
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ayni::providers::openai_compat::fixtures {
class LoopbackHttp {
  public:
    LoopbackHttp(std::string body, int status = 200, unsigned delay_ms = 0,
                 std::string extra_headers = {},
                 std::function<std::string(const std::string &, unsigned)> responder = {})
        : body_(std::move(body)), status_(status), delay_ms_(delay_ms),
          extra_headers_(std::move(extra_headers)), responder_(std::move(responder)) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ < 0 ||
            bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(listener_, 4) != 0) {
            if (listener_ >= 0) {
                ::close(listener_);
            }
            throw std::runtime_error("loopback bind failed");
        }
        socklen_t size = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
            ::close(listener_);
            throw std::runtime_error("loopback address failed");
        }
        port_ = ntohs(address.sin_port);
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            // Constructor failure has no destructor: return the already-owned
            // listener before propagating the classified startup failure.
            ::close(listener_);
            listener_ = -1;
            throw;
        }
    }
    ~LoopbackHttp() {
        stopping_.store(true);
        changed_.notify_all();
        shutdown(listener_, SHUT_RDWR);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (client_ >= 0) {
                shutdown(client_, SHUT_RDWR);
            }
        }
        worker_.join();
        ::close(listener_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    unsigned requests() const { return requests_.load(); }
    std::string request() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return request_;
    }
    std::vector<std::string> history() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return history_;
    }

  private:
    void serve(int client) {
        constexpr size_t request_limit = 1048576;
        std::string request;
        char buffer[4096];
        while (!stopping_.load() && request.size() <= request_limit) {
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return;
            }
            request.append(buffer, static_cast<size_t>(count));
            const auto end = request.find("\r\n\r\n");
            const auto length = request.find("Content-Length: ");
            if (end == std::string::npos || length == std::string::npos) {
                continue;
            }
            const auto bytes =
                std::stoul(request.substr(length + std::string("Content-Length: ").size()));
            if (request.size() >= end + std::string("\r\n\r\n").size() + bytes) {
                break;
            }
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            request_ = request;
            history_.push_back(request);
            ++requests_;
            changed_.wait_for(lock, std::chrono::milliseconds(delay_ms_),
                              [this] { return stopping_.load(); });
        }
        if (stopping_.load()) {
            return;
        }
        const std::string body = responder_ ? responder_(request, requests_.load()) : body_;
        const std::string reply = "HTTP/1.1 " + std::to_string(status_) +
                                  " Fixture\r\nContent-Type: application/json\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n" +
                                  extra_headers_ + "\r\n" + body;
        size_t sent = 0;
        while (sent < reply.size()) {
            const auto count = send(client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) {
                break;
            }
            sent += static_cast<size_t>(count);
        }
    }
    void run() {
        constexpr int accept_poll_ms = 10;
        while (!stopping_.load()) {
            // shutdown(listener) does not wake accept on every POSIX Host (notably macOS).
            // Bound the idle wait so normal fixture teardown can always join the worker.
            pollfd readable{listener_, POLLIN, 0};
            if (poll(&readable, 1, accept_poll_ms) <= 0 || !(readable.revents & POLLIN)) {
                continue;
            }
            const int client = accept(listener_, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                client_ = client;
            }
            serve(client);
            {
                std::lock_guard<std::mutex> guard(mutex_);
                ::close(client);
                client_ = -1;
            }
        }
    }
    int listener_ = -1;
    int client_ = -1;
    uint16_t port_ = 0;
    std::string body_;
    int status_;
    unsigned delay_ms_;
    std::string extra_headers_;
    std::function<std::string(const std::string &, unsigned)> responder_;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> requests_{0};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string request_;
    std::vector<std::string> history_;
    std::thread worker_;
};
} // namespace ayni::providers::openai_compat::fixtures
#endif

#include "commands.h"
#include "config.h"
#include "console.h"
#include "format.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <poll.h>
#include <thread>
#include <unistd.h>
namespace {
using namespace ayni;
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal handler requires lock-free flag");
void stop_signal(int) {
    interrupted.store(true, std::memory_order_relaxed);
}
ayni_status_t present(void *context, ayni_string_v1 text) {
    try {
        return static_cast<cli::Console *>(context)->present("提醒：" +
                                                             std::string(text.data, text.size));
    } catch (...) {
        return AYNI_RESULT_UNKNOWN;
    }
}
// One queued request and one copied reply. The input thread never touches Core;
// /wait is a pending control request while the worker continues advancing I/O.
class Driver {
  public:
    explicit Driver(cli::Options options, cli::Console &console)
        : console_(console), worker_([this, options] {
              try {
                  run(options);
              } catch (const std::exception &) {
                  std::lock_guard<std::mutex> lock(mutex_);
                  startup_ = final_status_ = AYNI_INTERNAL;
                  ready_ = finished_ = true;
                  changed_.notify_all();
              }
          }) {}
    ~Driver() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!finished_) {
                cli::Request quit;
                quit.action = cli::Action::quit;
                pending_ = quit;
            }
        }
        changed_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (final_status_ != AYNI_OK) {
            // One failure boundary for startup, runtime and exception unwinding:
            // retained application callbacks still borrow the live Console here.
            static_cast<void>(console_.close());
            std::_Exit(1);
        }
    }
    ayni_status_t ready() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return ready_; });
        return startup_;
    }
    bool finished() {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }
    runtime::Reply request(cli::Request request) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (finished_) {
            runtime::Reply reply;
            reply.status = final_status_;
            return reply;
        }
        pending_ = std::move(request);
        reply_.reset();
        changed_.notify_all();
        changed_.wait(lock, [&] { return reply_.has_value() || finished_; });
        if (reply_) {
            return *reply_;
        }
        runtime::Reply result;
        result.status = final_status_;
        return result;
    }

  private:
    void respond(runtime::Reply reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        reply_ = std::move(reply);
        changed_.notify_all();
    }
    void run(const cli::Options &options) {
        runtime::Assistant app;
        auto status =
            app.open(options.data_directory, options.model, {&console_, present}, options.quiet);
        std::vector<runtime::Notice> notices;
        runtime::Command state;
        state.operation = runtime::Operation::status;
        const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // Only the initial outer-file save gates input. Recovered model work
        // continues in the normal loop, where management and signals remain usable.
        while (status == AYNI_OK && app.execute(state).saving) {
            if (interrupted.load(std::memory_order_relaxed) ||
                std::chrono::steady_clock::now() >= startup_deadline) {
                status = AYNI_BUSY;
                break;
            }
            status = app.advance(notices);
            app.wait();
        }
        for (const auto &notice : notices) {
            const auto emitted = console_.post(cli::format(notice));
            if (status == AYNI_OK) {
                status = emitted;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            startup_ = status;
            ready_ = true;
        }
        changed_.notify_all();
        bool waiting = false, quitting = status != AYNI_OK;
        auto wait_deadline = std::chrono::steady_clock::now();
        while (!quitting && !interrupted.load(std::memory_order_relaxed)) {
            notices.clear();
            status = app.advance(notices);
            if (status == AYNI_OK) {
                status = console_.status();
            }
            for (const auto &notice : notices) {
                const auto emitted = console_.post(cli::format(notice));
                if (status == AYNI_OK) {
                    status = emitted;
                }
            }
            if (status != AYNI_OK) {
                break;
            }
            std::optional<cli::Request> next;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                next.swap(pending_);
            }
            if (next) {
                if (next->action == cli::Action::quit) {
                    quitting = true;
                    continue;
                }
                if (next->action == cli::Action::wait) {
                    waiting = true;
                    wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
                } else {
                    respond(app.execute(next->command));
                }
            }
            if (waiting && (app.idle() || interrupted.load(std::memory_order_relaxed) ||
                            std::chrono::steady_clock::now() >= wait_deadline)) {
                runtime::Reply reply;
                if (!app.idle()) {
                    reply.status = AYNI_BUSY;
                }
                respond(std::move(reply));
                waiting = false;
            }
            app.wait();
        }
        const auto closed = app.close();
        if (status == AYNI_OK) {
            status = closed;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            final_status_ = status;
            finished_ = true;
        }
        changed_.notify_all();
    }
    cli::Console &console_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<cli::Request> pending_;
    std::optional<runtime::Reply> reply_;
    bool ready_ = false, finished_ = false;
    ayni_status_t startup_ = AYNI_NOT_READY, final_status_ = AYNI_OK;
    std::thread worker_;
};
enum class Line { ready, end, too_long, error };
Line read_line(std::string &line, Driver &driver) {
    constexpr size_t line_max = 4096;
    line.clear();
    bool overflow = false;
    for (;;) {
        if (interrupted.load(std::memory_order_relaxed) || driver.finished()) {
            return Line::end;
        }
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int polled = poll(&descriptor, 1, 100);
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Line::error;
        }
        if (!polled) {
            continue;
        }
        char c = 0;
        const auto n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Line::error;
        }
        if (!n) {
            return overflow ? Line::too_long : line.empty() ? Line::end : Line::ready;
        }
        if (c == '\n') {
            return overflow ? Line::too_long : Line::ready;
        }
        if (c == '\r') {
            continue;
        }
        if (line.size() == line_max) {
            overflow = true;
        } else if (!overflow) {
            line.push_back(c);
        }
    }
}

} // namespace
int main(int argc, char **argv) {
    try {
        ayni::cli::Options options;
        std::string error;
        if (!ayni::cli::read_options(argc, argv, options, error)) {
            std::cerr << error << '\n' << ayni::cli::usage();
            return 2;
        }
        if (options.help) {
            std::cout << ayni::cli::usage();
            return 0;
        }
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        std::signal(SIGPIPE, SIG_IGN); // EPIPE becomes UNKNOWN channel evidence, not abrupt death.
        auto console = std::make_unique<ayni::cli::Console>();
        bool output_failed = false;
        const auto output = [&](const std::string &body) {
            const auto sent = console->post(body);
            output_failed = output_failed || sent != AYNI_OK;
            return sent;
        };
        Driver driver(options, *console);
        const auto status = driver.ready();
        if (status != AYNI_OK) {
            output("启动失败 status=" + std::to_string(status));
            return 1;
        }
        output("Ayni Native CLI：A=规则，B=ToolLoop。输入 /help 查看命令；首次使用须明确授权。");
        bool failed = false;
        for (;;) {
            if (isatty(STDIN_FILENO)) {
                output("ayni>");
            }
            std::string line;
            const auto read = read_line(line, driver);
            if (read == Line::end) {
                break;
            }
            if (read != Line::ready) {
                output("ERROR 输入行无效或超过 4096 字节。");
                failed = true;
                if (read == Line::error) {
                    break;
                }
                continue;
            }
            if (line.empty()) {
                continue;
            }
            ayni::cli::Request request;
            if (!ayni::cli::parse(line, request)) {
                output("ERROR 命令格式无效，请用 /help。");
                failed = true;
                continue;
            }
            if (request.action == ayni::cli::Action::help) {
                output(ayni::cli::help());
                continue;
            }
            if (request.action == ayni::cli::Action::quit) {
                break;
            }
            const auto reply = driver.request(request);
            output(ayni::cli::format(reply, request.action, request.command.operation));
            failed = failed || reply.status != AYNI_OK;
        }
        ayni::cli::Request quit;
        quit.action = ayni::cli::Action::quit;
        const auto closed = driver.request(quit);
        if (closed.status != AYNI_OK) {
            output(ayni::cli::format(closed, quit.action, quit.command.operation));
        }
        if (closed.status != AYNI_OK) {
            // A failed drain may retain a Provider callback borrowing this channel.
            // Process exit reclaims it; do not destroy it underneath that worker.
            static_cast<void>(console.release());
            // Explicit failed shutdown: stdout may be stuck inside a standard
            // stream lock, so normal exit flushing could block again.
            std::_Exit(1);
        }
        const auto channel = console ? console->close() : AYNI_NOT_READY;
        if (console && channel != AYNI_OK) {
            static_cast<void>(console.release());
            std::_Exit(1);
        }
        return failed || output_failed || closed.status != AYNI_OK || channel != AYNI_OK ? 1 : 0;
    } catch (const std::exception &) {
        std::cerr << "CLI 资源或系统操作失败。\n";
        return 1;
    }
}

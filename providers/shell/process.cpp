#include "process.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ayni::providers::shell {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto poll_interval = std::chrono::milliseconds(10);
constexpr auto terminate_grace = std::chrono::milliseconds(100);
constexpr auto pipe_drain_grace = std::chrono::milliseconds(200);
constexpr uint32_t read_bytes = 4096;
constexpr int child_setup_failed = 126;
constexpr int child_exec_failed = 127;
constexpr int no_fd = -1;

// Move-only descriptor cleanup belongs to this OS boundary, including failures
// before fork. It never owns a Provider completion or changes Core state.
class Descriptor {
  public:
    explicit Descriptor(int fd = no_fd) : fd_(fd) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return fd_; }
    void reset(int fd = no_fd) {
        if (fd_ != no_fd) {
            ::close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_;
};

struct Pipe {
    Descriptor read;
    Descriptor write;
    bool open() {
        std::array<int, 2> fds{};
        if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
            return false;
        }
        read.reset(fds[0]);
        write.reset(fds[1]);
        return true;
    }
};

// A C++ allocation or unexpected worker error must not orphan an unreaped
// child. Release happens only after waitpid has consumed this exact PID.
class ChildOwner {
  public:
    explicit ChildOwner(pid_t pid) : pid_(pid) {}
    ~ChildOwner() {
        if (pid_ > 0) {
            signal(SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }
    ChildOwner(const ChildOwner &) = delete;
    ChildOwner &operator=(const ChildOwner &) = delete;
    void reaped() { pid_ = -1; }
    // A permanent wait failure means this PID may already have been reused.
    // Do not signal or reap it again after the Host lost exclusive ownership.
    void lost() { pid_ = -1; }
    void signal(int value) const {
        if (pid_ > 0) {
            ::kill(-pid_, value);
            ::kill(pid_, value); // A command may move its leader out of the original group.
        }
    }

  private:
    pid_t pid_;
};

// POSIX child branch: no allocation, locks, C++ cleanup or inherited I/O state
// is used. A setup error cannot accidentally fall through to command execution.
[[noreturn]] void child(const ProcessConfig &config, const char *script, const Pipe &out,
                        const Pipe &error) {
    if (::setpgid(0, 0) != 0 || ::chdir(config.directory.c_str()) != 0 ||
        ::dup2(out.write.get(), STDOUT_FILENO) < 0 ||
        ::dup2(error.write.get(), STDERR_FILENO) < 0) {
        ::_exit(child_setup_failed);
    }
    const int input = ::open("/dev/null", O_RDONLY);
    if (input < 0 || ::dup2(input, STDIN_FILENO) < 0) {
        ::_exit(child_setup_failed);
    }
    if (input != STDIN_FILENO) {
        ::close(input);
    }
    // dup2(fd, fd) does not clear CLOEXEC. Closed inherited standard streams
    // can make pipe2/open return exactly 0/1/2, so make all exec targets explicit.
    if (::fcntl(STDIN_FILENO, F_SETFD, 0) != 0 || ::fcntl(STDOUT_FILENO, F_SETFD, 0) != 0 ||
        ::fcntl(STDERR_FILENO, F_SETFD, 0) != 0) {
        ::_exit(child_setup_failed);
    }
    const char *argv[] = {"/bin/sh", "-c", script, nullptr};
    ::execve(argv[0], const_cast<char *const *>(argv), config.environment_view.data());
    ::_exit(child_exec_failed);
}

// Nonblocking drain consumes both channels fairly. The first excess byte
// records truncation explicitly and triggers group termination at the caller.
bool drain(Descriptor &fd, std::string &out, uint32_t limit, uint32_t &total, bool &overflow) {
    std::array<char, read_bytes> buffer{};
    // One bounded read per channel per poll, even if a child writes forever.
    const auto count = ::read(fd.get(), buffer.data(), buffer.size());
    if (count == 0) {
        fd.reset();
        return true;
    }
    if (count < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    const auto bytes = static_cast<uint32_t>(count);
    const uint32_t available = limit - total;
    const uint32_t copied = bytes < available ? bytes : available;
    out.append(buffer.data(), copied);
    total += copied;
    overflow = overflow || bytes > available;
    return true;
}

bool exited_without_reaping(pid_t pid, bool &exited) {
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
        exited = info.si_pid == pid;
        return true;
    }
    return errno == EINTR;
}

std::string base64(const std::string &input) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2U) / 3U) * 4U);
    for (size_t at = 0; at < input.size(); at += 3U) {
        const auto first = static_cast<unsigned char>(input[at]);
        const unsigned second =
            at + 1U < input.size() ? static_cast<unsigned char>(input[at + 1U]) : 0;
        const unsigned third =
            at + 2U < input.size() ? static_cast<unsigned char>(input[at + 2U]) : 0;
        const unsigned word = (static_cast<unsigned>(first) << 16U) | (second << 8U) | third;
        out.push_back(alphabet[(word >> 18U) & 63U]);
        out.push_back(alphabet[(word >> 12U) & 63U]);
        out.push_back(at + 1U < input.size() ? alphabet[(word >> 6U) & 63U] : '=');
        out.push_back(at + 2U < input.size() ? alphabet[word & 63U] : '=');
    }
    return out;
}
} // namespace

bool reaping_supported() {
    struct sigaction disposition {};
    return ::sigaction(SIGCHLD, nullptr, &disposition) == 0 && disposition.sa_handler != SIG_IGN &&
           (disposition.sa_flags & SA_NOCLDWAIT) == 0;
}

// One execution owns one unreaped group leader until after group cleanup, so
// a recycled PID can never be used as the cancellation target. Descendants
// that deliberately leave the process group require a real OS sandbox.
ProcessResult execute(const ProcessConfig &config, const char *script,
                      const std::atomic<bool> &cancelled) {
    ProcessResult result;
    // Allocate bounded channel storage before execution begins. No pipe read
    // can allocate after fork; ChildOwner still protects later failure paths.
    result.standard_output.reserve(config.output_bytes);
    result.standard_error.reserve(config.output_bytes);
    if (!reaping_supported()) {
        return result;
    }
    if (cancelled.load()) {
        result.end = ProcessResult::End::cancelled;
        return result;
    }
    Pipe output;
    Pipe error;
    if (!output.open() || !error.open()) {
        return result;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return result;
    }
    if (pid == 0) {
        child(config, script, output, error);
    }
    ChildOwner ownership(pid);
    result.started = true;
    result.end = ProcessResult::End::exited;
    output.write.reset();
    error.write.reset();
    static_cast<void>(::setpgid(pid, pid)); // Child also sets its group before exec.
    const bool nonblocking = ::fcntl(output.read.get(), F_SETFL, O_NONBLOCK) == 0 &&
                             ::fcntl(error.read.get(), F_SETFL, O_NONBLOCK) == 0;
    bool stopped = !nonblocking;
    if (stopped) {
        result.end = ProcessResult::End::io_failed;
        ownership.signal(SIGKILL);
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(config.timeout_ms);
    auto kill_at = Clock::time_point::max();
    auto drain_until = Clock::time_point::max();
    bool exited = false;
    bool overflow = false;
    uint32_t total = 0;
    while (!exited || output.read.get() != no_fd || error.read.get() != no_fd) {
        const auto now = Clock::now();
        if (!stopped && (cancelled.load() || now >= deadline || overflow)) {
            result.end = overflow           ? ProcessResult::End::output_limit
                         : cancelled.load() ? ProcessResult::End::cancelled
                                            : ProcessResult::End::timeout;
            stopped = true;
            ownership.signal(SIGTERM);
            kill_at = now + terminate_grace;
        }
        if (now >= kill_at) {
            ownership.signal(SIGKILL);
            kill_at = Clock::time_point::max();
        }
        if (!exited && !exited_without_reaping(pid, exited)) {
            result.end = ProcessResult::End::io_failed;
            ownership.lost();
            return result; // UNKNOWN: no retained PID authority, never spin or kill a recycled PID.
        }
        if (exited && drain_until == Clock::time_point::max()) {
            ownership.signal(SIGKILL); // Do not leave ordinary background descendants running.
            drain_until = now + pipe_drain_grace;
        }
        if (exited && now >= drain_until) {
            if (output.read.get() != no_fd || error.read.get() != no_fd) {
                result.end = ProcessResult::End::io_failed;
            }
            break;
        }
        std::array<pollfd, 2> fds{{{output.read.get(), POLLIN, 0}, {error.read.get(), POLLIN, 0}}};
        if (::poll(fds.data(), fds.size(), static_cast<int>(poll_interval.count())) < 0 &&
            errno != EINTR) {
            result.end = ProcessResult::End::io_failed;
            ownership.signal(SIGKILL);
            stopped = true;
        }
        for (size_t at = 0; at < fds.size(); ++at) {
            if (fds[at].fd != no_fd && fds[at].revents != 0) {
                auto &fd = at == 0U ? output.read : error.read;
                auto &body = at == 0U ? result.standard_output : result.standard_error;
                if (!drain(fd, body, config.output_bytes, total, overflow)) {
                    fd.reset();
                    result.end = ProcessResult::End::io_failed;
                    ownership.signal(SIGKILL);
                    stopped = true;
                }
            }
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            result.end = ProcessResult::End::io_failed;
            ownership.lost();
            return result;
        }
    }
    ownership.reaped();
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
    if (overflow) {
        result.end = ProcessResult::End::output_limit;
    }
    return result;
}

std::string encode_output(const std::string &bytes) {
    return base64(bytes);
}

} // namespace ayni::providers::shell

#include "magpie_agent/process_exec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace magpie_agent {

namespace {

constexpr std::size_t kMaxOutputBytes = 2u * 1024u * 1024u;

bool isExecutableFile(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void killProcessTree(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    if (::kill(-pid, SIGKILL) != 0) {
        ::kill(pid, SIGKILL);
    }
}

void drainNonBlocking(int fd, std::string& out, bool& eof) {
    std::array<char, 8192> buf {};
    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            if (out.size() < kMaxOutputBytes) {
                std::size_t keep = std::min<std::size_t>(
                    static_cast<std::size_t>(n), kMaxOutputBytes - out.size());
                out.append(buf.data(), keep);
            }
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        eof = true;
        break;
    }
}

} // namespace

std::string ProcessOutput::combinedText() const {
    if (stderr_text.empty()) {
        return stdout_text;
    }
    if (stdout_text.empty()) {
        return stderr_text;
    }
    if (stdout_text.back() == '\n') {
        return stdout_text + stderr_text;
    }
    return stdout_text + "\n" + stderr_text;
}

void ProcessCancelToken::cancel() {
    canceled_.store(true);
    std::lock_guard<std::mutex> lock(mu_);
    if (active_pid_ > 0) {
        killProcessTree(active_pid_);
    }
}

bool ProcessCancelToken::isCanceled() const {
    return canceled_.load();
}

bool ProcessCancelToken::setActivePid(pid_t pid) {
    std::lock_guard<std::mutex> lock(mu_);
    if (canceled_.load()) {
        return false;
    }
    active_pid_ = pid;
    return true;
}

void ProcessCancelToken::clearActivePid(pid_t pid) {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_pid_ == pid) {
        active_pid_ = -1;
    }
}

std::string resolveExecutable(const std::string& bin_name_or_path) {
    if (bin_name_or_path.empty()) {
        return {};
    }
    if (bin_name_or_path.find('/') != std::string::npos) {
        return isExecutableFile(bin_name_or_path) ? bin_name_or_path : std::string{};
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return {};
    }
    std::istringstream iss(path_env);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        if (dir.empty()) {
            dir = ".";
        }
        std::string candidate = dir + "/" + bin_name_or_path;
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

ProcessOutput runCommand(const std::vector<std::string>& args,
                         int timeout_ms,
                         const std::shared_ptr<ProcessCancelToken>& cancel) {
    ProcessOutput result;
    if (args.empty() || args[0].empty()) {
        result.spawn_error = "empty command arguments";
        return result;
    }
    if (cancel && cancel->isCanceled()) {
        result.canceled = true;
        result.spawn_error = "canceled before launch";
        return result;
    }

    int stdout_pipe[2] {-1, -1};
    int stderr_pipe[2] {-1, -1};
    if (::pipe(stdout_pipe) != 0) {
        result.spawn_error = std::string("pipe(stdout) failed: ") + std::strerror(errno);
        return result;
    }
    if (::pipe(stderr_pipe) != 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        result.spawn_error = std::string("pipe(stderr) failed: ") + std::strerror(errno);
        return result;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        result.spawn_error = std::string("fork failed: ") + std::strerror(errno);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);

        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) {
                ::close(devnull);
            }
        }

        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    result.spawned = true;
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    setNonBlocking(stdout_pipe[0]);
    setNonBlocking(stderr_pipe[0]);

    if (cancel && !cancel->setActivePid(pid)) {
        result.canceled = true;
        killProcessTree(pid);
    }

    bool out_eof = false;
    bool err_eof = false;
    const auto start = std::chrono::steady_clock::now();

    while ((!out_eof || !err_eof) && !result.canceled) {
        if (cancel && cancel->isCanceled()) {
            result.canceled = true;
            killProcessTree(pid);
            break;
        }

        int wait_slice_ms = 50;
        if (timeout_ms > 0) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
            if (elapsed_ms >= timeout_ms) {
                result.timed_out = true;
                killProcessTree(pid);
                break;
            }
            long rem = timeout_ms - elapsed_ms;
            wait_slice_ms = static_cast<int>(std::min<long>(50, rem));
        }

        struct pollfd fds[2] {};
        nfds_t nfds = 0;
        int out_idx = -1;
        int err_idx = -1;

        if (!out_eof) {
            out_idx = static_cast<int>(nfds);
            fds[nfds].fd = stdout_pipe[0];
            fds[nfds].events = POLLIN | POLLHUP;
            ++nfds;
        }
        if (!err_eof) {
            err_idx = static_cast<int>(nfds);
            fds[nfds].fd = stderr_pipe[0];
            fds[nfds].events = POLLIN | POLLHUP;
            ++nfds;
        }

        if (nfds == 0) {
            break;
        }

        int rc = ::poll(fds, nfds, wait_slice_ms);
        if (rc < 0 && errno == EINTR) {
            continue;
        }

        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            drainNonBlocking(stdout_pipe[0], result.stdout_text, out_eof);
        }
        if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            drainNonBlocking(stderr_pipe[0], result.stderr_text, err_eof);
        }
    }

    // Drain any buffered bytes before closing descriptors.
    if (!out_eof) {
        drainNonBlocking(stdout_pipe[0], result.stdout_text, out_eof);
    }
    if (!err_eof) {
        drainNonBlocking(stderr_pipe[0], result.stderr_text, err_eof);
    }

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (cancel) {
        cancel->clearActivePid(pid);
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

bool isPidAlive(int pid) {
    if (pid <= 0) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

} // namespace magpie_agent

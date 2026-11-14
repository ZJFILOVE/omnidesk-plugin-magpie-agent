#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

namespace magpie_agent {

struct ProcessOutput {
    int exit_code = -1;
    bool timed_out = false;
    bool canceled = false;
    bool spawned = false;
    std::string spawn_error;
    std::string stdout_text;
    std::string stderr_text;

    std::string combinedText() const;
};

class ProcessCancelToken {
public:
    void cancel();
    bool isCanceled() const;

    // Internal lifecycle hooks used by runCommand to register active child pid.
    bool setActivePid(pid_t pid);
    void clearActivePid(pid_t pid);

private:
    std::atomic<bool> canceled_{false};
    std::mutex mu_;
    pid_t active_pid_ = -1;
};

using CommandRunnerFn = ProcessOutput (*)(const std::vector<std::string>& args,
                                          int timeout_ms,
                                          const std::shared_ptr<ProcessCancelToken>& cancel);

// Resolves a binary name via PATH or validates an explicit file path with X_OK.
std::string resolveExecutable(const std::string& bin_name_or_path);

// Executes argv directly via fork+execvp (never invokes a shell).
ProcessOutput runCommand(const std::vector<std::string>& args,
                         int timeout_ms = 8000,
                         const std::shared_ptr<ProcessCancelToken>& cancel = nullptr);

// Checks whether a local PID currently exists and accepts signal 0.
bool isPidAlive(int pid);

} // namespace magpie_agent

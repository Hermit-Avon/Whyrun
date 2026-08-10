#include "whyrun/collector.hpp"
#include "whyrun/execution_state.hpp"

#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace whyrun {
namespace {

std::uint64_t now_ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string current_working_directory() {
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    return error ? std::string{} : cwd.string();
}

class LinuxTraceeMemory final : public TraceeMemory {
public:
    std::string read_string(pid_t tid, std::uint64_t address,
                            std::size_t max_length) const override {
        if (address == 0 || max_length == 0) {
            return {};
        }

        std::vector<char> buffer(max_length);
        iovec local{buffer.data(), buffer.size()};
        iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
                     buffer.size()};
        const ssize_t count = process_vm_readv(tid, &local, 1, &remote, 1, 0);
        if (count > 0) {
            const auto end = std::find(buffer.begin(), buffer.begin() + count, '\0');
            if (end != buffer.begin() + count || count == static_cast<ssize_t>(max_length)) {
                return std::string(buffer.begin(), end);
            }
        }

        std::string result;
        result.reserve(std::min<std::size_t>(max_length, 256));
        for (std::size_t offset = 0; offset < max_length; offset += sizeof(long)) {
            errno = 0;
            const long word = ptrace(PTRACE_PEEKDATA, tid,
                                     reinterpret_cast<void*>(address + offset), nullptr);
            if (word == -1 && errno != 0) {
                break;
            }
            std::array<char, sizeof(long)> bytes{};
            std::memcpy(bytes.data(), &word, sizeof(word));
            for (const char byte : bytes) {
                if (byte == '\0' || result.size() == max_length) {
                    return result;
                }
                result.push_back(byte);
            }
        }
        return result;
    }

    bool read_bytes(pid_t tid, std::uint64_t address, void* destination,
                    std::size_t size) const override {
        if (address == 0 || destination == nullptr || size == 0) {
            return false;
        }

        iovec local{destination, size};
        iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), size};
        if (process_vm_readv(tid, &local, 1, &remote, 1, 0) ==
            static_cast<ssize_t>(size)) {
            return true;
        }

        auto* output = static_cast<unsigned char*>(destination);
        std::size_t offset = 0;
        while (offset < size) {
            errno = 0;
            const long word = ptrace(PTRACE_PEEKDATA, tid,
                                     reinterpret_cast<void*>(address + offset), nullptr);
            if (word == -1 && errno != 0) {
                return false;
            }
            const auto copy_size = std::min(sizeof(word), size - offset);
            std::memcpy(output + offset, &word, copy_size);
            offset += copy_size;
        }
        return true;
    }
};

struct ThreadState {
    bool in_syscall{false};
    long syscall_number{};
    std::array<std::uint64_t, 6> args{};
};

class TraceeGuard {
public:
    explicit TraceeGuard(std::unordered_set<pid_t>& tids) : tids_(tids) {}

    ~TraceeGuard() {
        if (!active_) {
            return;
        }
        for (const pid_t tid : tids_) {
            static_cast<void>(kill(tid, SIGKILL));
        }
        while (waitpid(-1, nullptr, __WALL) > 0) {
        }
    }

    TraceeGuard(const TraceeGuard&) = delete;
    TraceeGuard& operator=(const TraceeGuard&) = delete;

    void release() { active_ = false; }

private:
    std::unordered_set<pid_t>& tids_;
    bool active_{true};
};

long ptrace_options() {
    return PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
           PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT |
           PTRACE_O_EXITKILL;
}

bool set_options(pid_t tid, std::string& error) {
    if (ptrace(PTRACE_SETOPTIONS, tid, nullptr, ptrace_options()) == -1) {
        error = "PTRACE_SETOPTIONS for tid " + std::to_string(tid) + ": " +
                std::strerror(errno);
        return false;
    }
    return true;
}

bool resume_syscall(pid_t tid, int signal, std::string& error) {
    if (ptrace(PTRACE_SYSCALL, tid, nullptr,
               reinterpret_cast<void*>(static_cast<std::intptr_t>(signal))) == -1) {
        if (errno == ESRCH) {
            return true;
        }
        error = "PTRACE_SYSCALL for tid " + std::to_string(tid) + ": " +
                std::strerror(errno);
        return false;
    }
    return true;
}

class PtraceCollector final : public Collector {
public:
    CollectionResult collect(const CollectionRequest& request,
                             EventSink& sink) override {
        CollectionResult result;
        if (request.command.empty()) {
            result.error = "record requires a command after --";
            return result;
        }

        const std::string initial_cwd = current_working_directory();
        const pid_t root_pid = fork();
        if (root_pid == -1) {
            result.error = std::string("fork: ") + std::strerror(errno);
            return result;
        }

        if (root_pid == 0) {
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) {
                _exit(126);
            }
            if (raise(SIGSTOP) != 0) {
                _exit(126);
            }

            for (const auto& [name, value] : request.environment) {
                if (setenv(name.c_str(), value.c_str(), 1) == -1) {
                    const std::string message = "whyrun: setenv " + name + ": " +
                                                std::strerror(errno) + "\n";
                    static_cast<void>(write(STDERR_FILENO, message.data(), message.size()));
                    _exit(126);
                }
            }

            std::vector<char*> argv;
            argv.reserve(request.command.size() + 1);
            for (const auto& argument : request.command) {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);
            execvp(argv.front(), argv.data());
            const std::string message = "whyrun: execvp " + request.command.front() + ": " +
                                        std::strerror(errno) + "\n";
            static_cast<void>(write(STDERR_FILENO, message.data(), message.size()));
            _exit(127);
        }

        int initial_status{};
        if (waitpid(root_pid, &initial_status, 0) == -1) {
            result.error = std::string("waitpid: ") + std::strerror(errno);
            static_cast<void>(kill(root_pid, SIGKILL));
            return result;
        }
        if (!WIFSTOPPED(initial_status)) {
            result.error = "tracee did not enter its initial ptrace stop";
            return result;
        }

        std::string trace_error;
        if (!set_options(root_pid, trace_error)) {
            result.error = std::move(trace_error);
            static_cast<void>(kill(root_pid, SIGKILL));
            static_cast<void>(waitpid(root_pid, nullptr, 0));
            return result;
        }

        LinuxTraceeMemory memory;
        ExecutionState execution(sink, memory);
        execution.add_initial_process(root_pid, initial_cwd);

        std::unordered_set<pid_t> live_tids{root_pid};
        TraceeGuard tracee_guard(live_tids);
        std::unordered_set<pid_t> configured_tids{root_pid};
        std::unordered_map<pid_t, ThreadState> threads;

        if (!resume_syscall(root_pid, 0, trace_error)) {
            result.error = std::move(trace_error);
            return result;
        }

        bool root_status_seen = false;
        while (!live_tids.empty()) {
            int status{};
            const pid_t tid = waitpid(-1, &status, __WALL);
            if (tid == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ECHILD) {
                    break;
                }
                result.error = std::string("waitpid: ") + std::strerror(errno);
                return result;
            }

            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                const int term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
                execution.on_process_exit(tid, exit_code, term_signal, now_ns());
                live_tids.erase(tid);
                configured_tids.erase(tid);
                threads.erase(tid);
                if (tid == root_pid) {
                    result.exit_code = term_signal == 0 ? exit_code : 128 + term_signal;
                    result.term_signal = term_signal;
                    root_status_seen = true;
                }
                continue;
            }

            if (!WIFSTOPPED(status)) {
                continue;
            }

            live_tids.insert(tid);
            const int stop_signal = WSTOPSIG(status);
            const unsigned int event = static_cast<unsigned int>(status) >> 16U;

            if (!configured_tids.contains(tid)) {
                if (!set_options(tid, trace_error)) {
                    result.error = std::move(trace_error);
                    return result;
                }
                configured_tids.insert(tid);
            }

            if (stop_signal == (SIGTRAP | 0x80)) {
                user_regs_struct registers{};
                if (ptrace(PTRACE_GETREGS, tid, nullptr, &registers) == -1) {
                    if (errno != ESRCH) {
                        result.error = "PTRACE_GETREGS for tid " + std::to_string(tid) +
                                       ": " + std::strerror(errno);
                        return result;
                    }
                } else {
                    auto& thread = threads[tid];
                    if (!thread.in_syscall) {
                        thread.in_syscall = true;
                        thread.syscall_number = static_cast<long>(registers.orig_rax);
                        thread.args = {registers.rdi, registers.rsi, registers.rdx,
                                       registers.r10, registers.r8, registers.r9};
                    } else {
                        RawSyscall syscall;
                        syscall.pid = execution.process_for_thread(tid);
                        syscall.tid = tid;
                        syscall.number = thread.syscall_number;
                        syscall.args = thread.args;
                        syscall.result = static_cast<std::int64_t>(registers.rax);
                        syscall.timestamp_ns = now_ns();
                        execution.on_syscall_exit(syscall);
                        thread.in_syscall = false;
                    }
                }
                if (!resume_syscall(tid, 0, trace_error)) {
                    result.error = std::move(trace_error);
                    return result;
                }
                continue;
            }

            if (stop_signal == SIGTRAP && event != 0) {
                if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
                    event == PTRACE_EVENT_CLONE) {
                    unsigned long child_tid{};
                    if (ptrace(PTRACE_GETEVENTMSG, tid, nullptr, &child_tid) == -1) {
                        result.error = "PTRACE_GETEVENTMSG for tid " +
                                       std::to_string(tid) + ": " + std::strerror(errno);
                        return result;
                    }
                    const auto thread = threads.find(tid);
                    const std::uint64_t clone_flags =
                        thread == threads.end() ? 0 : thread->second.args[0];
                    execution.on_process_created(tid, static_cast<pid_t>(child_tid), event,
                                                 clone_flags, now_ns());
                    live_tids.insert(static_cast<pid_t>(child_tid));
                } else if (event == PTRACE_EVENT_EXEC) {
                    execution.on_process_exec(tid, now_ns());
                }

                if (!resume_syscall(tid, 0, trace_error)) {
                    result.error = std::move(trace_error);
                    return result;
                }
                continue;
            }

            const int delivered_signal = stop_signal == SIGSTOP ? 0 : stop_signal;
            if (!resume_syscall(tid, delivered_signal, trace_error)) {
                result.error = std::move(trace_error);
                return result;
            }
        }

        result.collector_ok = true;
        result.end_ns = now_ns();
        if (!root_status_seen) {
            result.collector_ok = false;
            result.error = "root tracee status was not observed";
        }
        tracee_guard.release();
        return result;
    }
};

}  // namespace

std::unique_ptr<Collector> make_ptrace_collector() {
    return std::make_unique<PtraceCollector>();
}

}  // namespace whyrun

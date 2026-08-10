#pragma once

#include "whyrun/event.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>

namespace whyrun {

struct CommandMarker;

enum class ResourceType {
    File,
    Socket,
    Pipe,
    Unknown,
};

struct Resource {
    ResourceType type{ResourceType::Unknown};
    std::string name;
    bool is_directory{false};
};

struct ProcessState {
    pid_t pid{};
    pid_t parent_pid{};
    std::string executable;
    std::string cwd;
    std::optional<std::uint64_t> command_id;
    std::unordered_map<int, Resource> fd_table;
};

struct RawSyscall {
    pid_t pid{};
    pid_t tid{};
    long number{};
    std::array<std::uint64_t, 6> args{};
    std::int64_t result{};
    std::uint64_t timestamp_ns{};
};

class TraceeMemory {
public:
    virtual ~TraceeMemory() = default;
    virtual std::string read_string(pid_t tid, std::uint64_t address,
                                    std::size_t max_length = 4096) const = 0;
    virtual bool read_bytes(pid_t tid, std::uint64_t address, void* destination,
                            std::size_t size) const = 0;
};

class ExecutionState {
public:
    ExecutionState(EventSink& sink, const TraceeMemory& memory);

    void add_initial_process(pid_t pid, std::string cwd,
                             const std::optional<std::string>& root_command,
                             std::uint64_t timestamp_ns);
    void on_command_marker(pid_t tid, const CommandMarker& marker,
                           std::uint64_t timestamp_ns);
    void on_process_exec(pid_t tid, std::uint64_t timestamp_ns);
    void on_process_created(pid_t parent_tid, pid_t child_tid,
                            unsigned int ptrace_event, std::uint64_t clone_flags,
                            std::uint64_t timestamp_ns);
    void on_process_exit(pid_t tid, int exit_code, int term_signal,
                         std::uint64_t timestamp_ns);
    void on_syscall_exit(const RawSyscall& syscall);
    void forget_thread(pid_t tid);

    pid_t process_for_thread(pid_t tid) const;

private:
    struct ActiveCommand {
        std::uint64_t id{};
        std::optional<std::uint64_t> first_process_ns;
        bool started{false};
    };

    ProcessState* find_process_for_thread(pid_t tid);
    const ProcessState* find_process_for_thread(pid_t tid) const;
    std::string resolve_path(const ProcessState& process, int dirfd,
                             const std::string& path) const;
    void emit(Event event) const;

    EventSink& sink_;
    const TraceeMemory& memory_;
    std::unordered_map<pid_t, ProcessState> processes_;
    std::unordered_map<pid_t, pid_t> thread_to_process_;
    std::optional<ActiveCommand> active_command_;
    pid_t root_pid_{};
};

}  // namespace whyrun

#include "whyrun/execution_state.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

namespace whyrun {
namespace {

constexpr std::int64_t kMaxErrno = 4095;

int syscall_errno(std::int64_t result) {
    return result < 0 && result >= -kMaxErrno ? static_cast<int>(-result) : 0;
}

std::string read_proc_link(pid_t pid, const std::string& name) {
    std::error_code error;
    const auto path = std::filesystem::path("/proc") / std::to_string(pid) / name;
    const auto value = std::filesystem::read_symlink(path, error);
    return error ? std::string{} : value.string();
}

std::string unreadable_address(std::uint64_t address) {
    std::ostringstream output;
    output << "<unreadable@0x" << std::hex << address << '>';
    return output.str();
}

std::string normalize_path(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal().string();
    if (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

struct SocketEndpoint {
    EventType event_type{EventType::NetworkConnect};
    std::string resource;
    std::string family_name;
};

std::string render_unix_name(const char* path, std::size_t length) {
    if (length == 0) {
        return "unix:<unnamed>";
    }

    const bool abstract = path[0] == '\0';
    const std::size_t begin = abstract ? 1 : 0;
    std::size_t end = length;
    if (!abstract) {
        const auto* terminator = static_cast<const char*>(std::memchr(path, '\0', length));
        if (terminator != nullptr) {
            end = static_cast<std::size_t>(terminator - path);
        }
    }

    std::ostringstream output;
    output << "unix:" << (abstract ? "@" : "");
    for (std::size_t index = begin; index < end; ++index) {
        const auto character = static_cast<unsigned char>(path[index]);
        if (std::isprint(character) != 0 && character != '\\') {
            output << static_cast<char>(character);
        } else {
            output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned int>(character) << std::dec;
        }
    }
    return output.str();
}

std::optional<SocketEndpoint> socket_endpoint(const TraceeMemory& memory, pid_t tid,
                                              std::uint64_t address,
                                              std::uint64_t length) {
    if (address == 0 || length < sizeof(sa_family_t)) {
        return SocketEndpoint{EventType::NetworkConnect, "<invalid-sockaddr>",
                              "unknown"};
    }

    sa_family_t family{};
    if (!memory.read_bytes(tid, address, &family, sizeof(family))) {
        return SocketEndpoint{EventType::NetworkConnect, unreadable_address(address),
                              "unreadable"};
    }

    char buffer[INET6_ADDRSTRLEN]{};
    if (family == AF_INET && length >= sizeof(sockaddr_in)) {
        sockaddr_in value{};
        if (!memory.read_bytes(tid, address, &value, sizeof(value))) {
            return SocketEndpoint{EventType::NetworkConnect, unreadable_address(address),
                                  "inet"};
        }
        if (inet_ntop(AF_INET, &value.sin_addr, buffer, sizeof(buffer)) == nullptr) {
            return SocketEndpoint{EventType::NetworkConnect, "<invalid-ipv4>", "inet"};
        }
        return SocketEndpoint{EventType::NetworkConnect,
                              std::string(buffer) + ':' +
                                  std::to_string(ntohs(value.sin_port)),
                              "inet"};
    }

    if (family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        sockaddr_in6 value{};
        if (!memory.read_bytes(tid, address, &value, sizeof(value))) {
            return SocketEndpoint{EventType::NetworkConnect, unreadable_address(address),
                                  "inet6"};
        }
        if (inet_ntop(AF_INET6, &value.sin6_addr, buffer, sizeof(buffer)) == nullptr) {
            return SocketEndpoint{EventType::NetworkConnect, "<invalid-ipv6>",
                                  "inet6"};
        }
        return SocketEndpoint{EventType::NetworkConnect,
                              '[' + std::string(buffer) + "]:" +
                                  std::to_string(ntohs(value.sin6_port)),
                              "inet6"};
    }

    if (family == AF_UNIX) {
        sockaddr_un value{};
        const auto read_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, sizeof(value)));
        if (!memory.read_bytes(tid, address, &value, read_size)) {
            return SocketEndpoint{EventType::LocalIpcConnect,
                                  unreadable_address(address), "unix"};
        }
        constexpr std::size_t path_offset = offsetof(sockaddr_un, sun_path);
        const std::size_t path_length = read_size > path_offset ? read_size - path_offset : 0;
        return SocketEndpoint{EventType::LocalIpcConnect,
                              render_unix_name(value.sun_path, path_length), "unix"};
    }

    return std::nullopt;
}

std::string child_kind(unsigned int ptrace_event) {
    switch (ptrace_event) {
        case PTRACE_EVENT_FORK:
            return "fork";
        case PTRACE_EVENT_VFORK:
            return "vfork";
        case PTRACE_EVENT_CLONE:
            return "clone";
        default:
            return "unknown";
    }
}

}  // namespace

ExecutionState::ExecutionState(EventSink& sink, const TraceeMemory& memory)
    : sink_(sink), memory_(memory) {}

void ExecutionState::add_initial_process(pid_t pid, std::string cwd) {
    ProcessState process;
    process.pid = pid;
    process.cwd = std::move(cwd);
    processes_.insert_or_assign(pid, std::move(process));
    thread_to_process_[pid] = pid;
}

pid_t ExecutionState::process_for_thread(pid_t tid) const {
    const auto found = thread_to_process_.find(tid);
    return found == thread_to_process_.end() ? tid : found->second;
}

ProcessState* ExecutionState::find_process_for_thread(pid_t tid) {
    const auto process_id = process_for_thread(tid);
    const auto found = processes_.find(process_id);
    return found == processes_.end() ? nullptr : &found->second;
}

const ProcessState* ExecutionState::find_process_for_thread(pid_t tid) const {
    const auto process_id = process_for_thread(tid);
    const auto found = processes_.find(process_id);
    return found == processes_.end() ? nullptr : &found->second;
}

void ExecutionState::emit(Event event) const {
    sink_.emit(event);
}

void ExecutionState::on_process_exec(pid_t tid, std::uint64_t timestamp_ns) {
    auto* process = find_process_for_thread(tid);
    if (process == nullptr) {
        add_initial_process(tid, read_proc_link(tid, "cwd"));
        process = find_process_for_thread(tid);
    }

    auto executable = read_proc_link(tid, "exe");
    if (executable.empty()) {
        executable = "<unknown>";
    }
    process->executable = executable;

    Event event{timestamp_ns, process->pid, tid, EventType::ProcessExec,
                executable, 0, 0, {}};
    event.metadata["ppid"] = std::to_string(process->parent_pid);
    emit(std::move(event));
}

void ExecutionState::on_process_created(pid_t parent_tid, pid_t child_tid,
                                        unsigned int ptrace_event,
                                        std::uint64_t clone_flags,
                                        std::uint64_t timestamp_ns) {
    auto* parent = find_process_for_thread(parent_tid);
    if (parent == nullptr) {
        return;
    }

    const bool is_thread = ptrace_event == PTRACE_EVENT_CLONE &&
                           (clone_flags & static_cast<std::uint64_t>(CLONE_THREAD)) != 0;
    if (is_thread) {
        thread_to_process_[child_tid] = parent->pid;
        return;
    }

    ProcessState child = *parent;
    child.pid = child_tid;
    child.parent_pid = parent->pid;
    processes_.insert_or_assign(child_tid, std::move(child));
    thread_to_process_[child_tid] = child_tid;

    Event event{timestamp_ns, parent->pid, parent_tid, EventType::ProcessFork,
                std::to_string(child_tid), child_tid, 0, {}};
    event.metadata["child_pid"] = std::to_string(child_tid);
    event.metadata["kind"] = child_kind(ptrace_event);
    event.metadata["executable"] = parent->executable;
    emit(std::move(event));
}

void ExecutionState::on_process_exit(pid_t tid, int exit_code, int term_signal,
                                     std::uint64_t timestamp_ns) {
    const auto process_id = process_for_thread(tid);
    if (tid != process_id) {
        thread_to_process_.erase(tid);
        return;
    }

    auto* process = find_process_for_thread(tid);
    if (process == nullptr) {
        return;
    }

    const int result = term_signal == 0 ? exit_code : 128 + term_signal;
    Event event{timestamp_ns, process->pid, tid, EventType::ProcessExit,
                process->executable, result, 0, {}};
    event.metadata["exit_code"] = std::to_string(exit_code);
    event.metadata["term_signal"] = std::to_string(term_signal);
    emit(std::move(event));
    thread_to_process_.erase(tid);
}

std::string ExecutionState::resolve_path(const ProcessState& process, int dirfd,
                                         const std::string& path) const {
    if (path.empty()) {
        return path;
    }

    const std::filesystem::path candidate(path);
    if (candidate.is_absolute()) {
        return normalize_path(candidate);
    }

    if (dirfd == AT_FDCWD) {
        return normalize_path(std::filesystem::path(process.cwd) / candidate);
    }

    const auto found = process.fd_table.find(dirfd);
    if (found != process.fd_table.end() && found->second.type == ResourceType::File &&
        found->second.is_directory) {
        return normalize_path(std::filesystem::path(found->second.name) / candidate);
    }
    return "<dirfd:" + std::to_string(dirfd) + ">/" + path;
}

void ExecutionState::on_syscall_exit(const RawSyscall& syscall) {
    auto* process = find_process_for_thread(syscall.tid);
    if (process == nullptr) {
        return;
    }

    const auto make_event = [&](EventType type, std::string resource,
                                std::int64_t result) {
        Event event{syscall.timestamp_ns, process->pid, syscall.tid, type,
                    std::move(resource), result, syscall_errno(result), {}};
        return event;
    };

#ifdef __NR_open
    if (syscall.number == __NR_open) {
        const auto raw_path = memory_.read_string(syscall.tid, syscall.args[0]);
        const auto path = resolve_path(*process, AT_FDCWD,
                                       raw_path.empty() ? unreadable_address(syscall.args[0])
                                                        : raw_path);
        const int flags = static_cast<int>(syscall.args[1]);
        auto event = make_event(EventType::FileOpen, path, syscall.result);
        event.metadata["operation"] = "open";
        event.metadata["flags"] = std::to_string(flags);
        emit(event);
        if (syscall.result >= 0) {
            process->fd_table[static_cast<int>(syscall.result)] =
                Resource{ResourceType::File, path, (flags & O_DIRECTORY) != 0};
        }
        return;
    }
#endif

    if (syscall.number == __NR_openat
#ifdef __NR_openat2
        || syscall.number == __NR_openat2
#endif
    ) {
        const int dirfd = static_cast<int>(syscall.args[0]);
        const auto raw_path = memory_.read_string(syscall.tid, syscall.args[1]);
        const auto path = resolve_path(*process, dirfd,
                                       raw_path.empty() ? unreadable_address(syscall.args[1])
                                                        : raw_path);
        std::uint64_t flags = syscall.args[2];
#ifdef __NR_openat2
        if (syscall.number == __NR_openat2) {
            open_how how{};
            if (memory_.read_bytes(syscall.tid, syscall.args[2], &how, sizeof(how))) {
                flags = how.flags;
            } else {
                flags = 0;
            }
        }
#endif
        auto event = make_event(EventType::FileOpen, path, syscall.result);
        event.metadata["operation"] = syscall.number == __NR_openat ? "openat" : "openat2";
        event.metadata["flags"] = std::to_string(flags);
        event.metadata["dirfd"] = std::to_string(dirfd);
        emit(event);
        if (syscall.result >= 0) {
            process->fd_table[static_cast<int>(syscall.result)] = Resource{
                ResourceType::File, path, (flags & static_cast<std::uint64_t>(O_DIRECTORY)) != 0};
        }
        return;
    }

    if (syscall.number == __NR_read || syscall.number == __NR_write) {
        const int fd = static_cast<int>(syscall.args[0]);
        const auto found = process->fd_table.find(fd);
        if (found == process->fd_table.end() || found->second.type != ResourceType::File) {
            return;
        }
        const auto type = syscall.number == __NR_read ? EventType::FileRead
                                                       : EventType::FileWrite;
        auto event = make_event(type, found->second.name, syscall.result);
        event.metadata["fd"] = std::to_string(fd);
        event.metadata["requested_bytes"] = std::to_string(syscall.args[2]);
        emit(std::move(event));
        return;
    }

    if (syscall.number == __NR_close) {
        if (syscall.result == 0) {
            process->fd_table.erase(static_cast<int>(syscall.args[0]));
        }
        return;
    }

    if (syscall.number == __NR_dup || syscall.number == __NR_dup2
#ifdef __NR_dup3
        || syscall.number == __NR_dup3
#endif
    ) {
        if (syscall.result < 0) {
            return;
        }
        const int old_fd = static_cast<int>(syscall.args[0]);
        const int new_fd = static_cast<int>(syscall.result);
        const auto found = process->fd_table.find(old_fd);
        if (found == process->fd_table.end()) {
            process->fd_table.erase(new_fd);
        } else {
            process->fd_table[new_fd] = found->second;
        }
        return;
    }

    if (syscall.number == __NR_socket) {
        if (syscall.result >= 0) {
            Resource socket{ResourceType::Socket, "<socket>", false};
            process->fd_table[static_cast<int>(syscall.result)] = std::move(socket);
        }
        return;
    }

    if (syscall.number == __NR_connect) {
        const int fd = static_cast<int>(syscall.args[0]);
        const auto endpoint = socket_endpoint(memory_, syscall.tid, syscall.args[1],
                                              syscall.args[2]);
        if (!endpoint.has_value()) {
            return;
        }
        auto event = make_event(endpoint->event_type, endpoint->resource, syscall.result);
        event.metadata["operation"] = "connect";
        event.metadata["fd"] = std::to_string(fd);
        event.metadata["family"] = endpoint->family_name;
        emit(event);

        const auto found = process->fd_table.find(fd);
        if (found != process->fd_table.end() && found->second.type == ResourceType::Socket) {
            found->second.name = endpoint->resource;
        }
        return;
    }

#ifdef __NR_chdir
    if (syscall.number == __NR_chdir && syscall.result == 0) {
        const auto raw_path = memory_.read_string(syscall.tid, syscall.args[0]);
        if (!raw_path.empty()) {
            process->cwd = resolve_path(*process, AT_FDCWD, raw_path);
        }
        return;
    }
#endif

#ifdef __NR_fchdir
    if (syscall.number == __NR_fchdir && syscall.result == 0) {
        const auto found = process->fd_table.find(static_cast<int>(syscall.args[0]));
        if (found != process->fd_table.end() && found->second.type == ResourceType::File) {
            process->cwd = found->second.name;
        }
    }
#endif
}

void ExecutionState::forget_thread(pid_t tid) {
    thread_to_process_.erase(tid);
}

}  // namespace whyrun

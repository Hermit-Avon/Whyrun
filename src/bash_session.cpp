#include "whyrun/session.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace whyrun {
namespace {

constexpr std::size_t kProtocolFields = 5;
constexpr std::size_t kMaximumContextBytes = 64U * 1024U * 1024U;

constexpr std::string_view kCaptureFunction = R"bash(() {
    local command_status="$1"
    local history_number=$((HISTCMD - 1))
    if (( history_number > __whyrun_last_history )); then
        builtin printf '%s\0%s\0%s\0%s\0' \
            "$EPOCHREALTIME" "$BASHPID" "$command_status" "$PWD" \
            >&"$WHYRUN_CONTEXT_FD"
        HISTTIMEFORMAT=$'\034' builtin history 1 >&"$WHYRUN_CONTEXT_FD"
        builtin printf '\0' >&"$WHYRUN_CONTEXT_FD"
        __whyrun_last_history=$history_number
    fi
    return "$command_status"
})bash";

std::uint64_t parse_unsigned(std::string_view text, std::string_view field_name) {
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid session " + std::string(field_name));
    }
    return value;
}

int parse_status(std::string_view text) {
    int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid session exit status");
    }
    return value;
}

std::uint64_t parse_epoch_ns(std::string_view text) {
    const auto separator = text.find('.');
    const auto seconds_text = text.substr(0, separator);
    auto fractional = separator == std::string_view::npos
                          ? std::string_view{}
                          : text.substr(separator + 1);
    if (fractional.size() > 9) {
        fractional = fractional.substr(0, 9);
    }

    const auto seconds = parse_unsigned(seconds_text, "timestamp");
    std::uint64_t nanoseconds = fractional.empty()
                                    ? 0
                                    : parse_unsigned(fractional, "timestamp");
    for (std::size_t digits = fractional.size(); digits < 9; ++digits) {
        nanoseconds *= 10;
    }
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) /
                      1'000'000'000ULL) {
        throw std::runtime_error("session timestamp is out of range");
    }
    return seconds * 1'000'000'000ULL + nanoseconds;
}

std::vector<std::string> read_fields(int fd) {
    if (lseek(fd, 0, SEEK_SET) == -1) {
        throw std::system_error(errno, std::generic_category(),
                                "rewind session context");
    }

    std::string bytes;
    std::array<char, 8192> buffer{};
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(),
                                    "read session context");
        }
        if (bytes.size() + static_cast<std::size_t>(count) > kMaximumContextBytes) {
            throw std::runtime_error("session command context exceeds 64 MiB");
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }

    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\0', begin);
        if (end == std::string::npos) {
            throw std::runtime_error("incomplete session command context");
        }
        fields.emplace_back(bytes.substr(begin, end - begin));
        begin = end + 1;
    }
    if (fields.size() % kProtocolFields != 0) {
        throw std::runtime_error("malformed session command context");
    }
    return fields;
}

}  // namespace

BashSession::BashSession() {
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error) {
        throw std::system_error(temp_error, "locate temporary directory");
    }

    std::string path_template = (temp_directory / "whyrun-session-XXXXXX").string();
    std::vector<char> mutable_path(path_template.begin(), path_template.end());
    mutable_path.push_back('\0');
    const int temporary_fd = mkstemp(mutable_path.data());
    if (temporary_fd == -1) {
        throw std::system_error(errno, std::generic_category(),
                                "create session context");
    }
    if (unlink(mutable_path.data()) == -1) {
        const int error = errno;
        close(temporary_fd);
        throw std::system_error(error, std::generic_category(),
                                "unlink session context");
    }

    context_fd_ = fcntl(temporary_fd, F_DUPFD, 10);
    const int duplicate_error = errno;
    close(temporary_fd);
    if (context_fd_ == -1) {
        throw std::system_error(duplicate_error, std::generic_category(),
                                "duplicate session context descriptor");
    }

    const int descriptor_flags = fcntl(context_fd_, F_GETFD);
    if (descriptor_flags == -1 ||
        fcntl(context_fd_, F_SETFD, descriptor_flags & ~FD_CLOEXEC) == -1) {
        const int error = errno;
        close(context_fd_);
        context_fd_ = -1;
        throw std::system_error(error, std::generic_category(),
                                "configure session context");
    }

    request_.command = {"/bin/bash", "--noprofile", "--norc", "-i"};
    request_.environment = {
        {"BASH_FUNC___whyrun_capture%%", std::string(kCaptureFunction)},
        {"PROMPT_COMMAND", "__whyrun_capture \"$?\""},
        {"WHYRUN_CONTEXT_FD", std::to_string(context_fd_)},
        {"__whyrun_last_history", "0"},
        {"HISTFILE", ""},
        {"HISTCONTROL", ""},
        {"HISTIGNORE", ""},
        {"PS1", "[whyrun] \\w \\$ "},
        {"PS2", "[whyrun] > "},
    };
}

BashSession::~BashSession() {
    if (context_fd_ != -1) {
        close(context_fd_);
    }
}

std::vector<RecordedCommand> BashSession::read_commands() {
    if (commands_read_) {
        throw std::logic_error("session commands have already been read");
    }
    commands_read_ = true;

    const auto fields = read_fields(context_fd_);
    std::vector<RecordedCommand> commands;
    commands.reserve(fields.size() / kProtocolFields);
    for (std::size_t index = 0; index < fields.size(); index += kProtocolFields) {
        RecordedCommand command;
        command.sequence = commands.size() + 1;
        command.completed_ns = parse_epoch_ns(fields[index]);
        const auto shell_pid = parse_unsigned(fields[index + 1], "shell pid");
        if (shell_pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
            throw std::runtime_error("session shell pid is out of range");
        }
        command.shell_pid = static_cast<pid_t>(shell_pid);
        command.exit_code = parse_status(fields[index + 2]);
        command.completed_cwd = fields[index + 3];
        command.text = fields[index + 4];
        const auto history_prefix = command.text.find('\034');
        if (history_prefix == std::string::npos) {
            throw std::runtime_error("session command is missing its history prefix");
        }
        command.text.erase(0, history_prefix + 1);
        if (!command.text.empty() && command.text.back() == '\n') {
            command.text.pop_back();
        }
        commands.push_back(std::move(command));
    }
    return commands;
}

}  // namespace whyrun

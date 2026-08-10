#include "whyrun/session.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace whyrun {
namespace {

constexpr std::size_t kProtocolFields = 4;
constexpr std::size_t kMaximumFieldBytes = 64U * 1024U;

constexpr std::string_view kStartFunction = R"bash(() {
    local command_status="$1"
    if (( __whyrun_skip_debug > 0 )); then
        __whyrun_skip_debug=$((__whyrun_skip_debug - 1))
        return "$command_status"
    fi
    if (( __whyrun_at_prompt )); then
        __whyrun_at_prompt=0
        builtin printf 'S\0%s\0%s\0' \
            "$__whyrun_active_command" "$PWD" >&"$WHYRUN_CONTEXT_FD"
        HISTTIMEFORMAT=$'\034' builtin history 1 >&"$WHYRUN_CONTEXT_FD"
        builtin printf '\0' >&"$WHYRUN_CONTEXT_FD"
    fi
    return "$command_status"
})bash";

constexpr std::string_view kPromptFunction = R"bash(() {
    local command_status="$1"
    builtin trap - DEBUG
    if (( __whyrun_active_command > 0 )); then
        builtin printf 'E\0%s\0%s\0%s\0' \
            "$__whyrun_active_command" "$command_status" "$PWD" \
            >&"$WHYRUN_CONTEXT_FD"
        __whyrun_active_command=0
    fi
    __whyrun_sequence=$((__whyrun_sequence + 1))
    __whyrun_active_command=$__whyrun_sequence
    builtin printf 'A\0%s\0%s\0\0' \
        "$__whyrun_active_command" "$PWD" >&"$WHYRUN_CONTEXT_FD"
    __whyrun_at_prompt=1
    __whyrun_skip_debug=1
    builtin trap '__whyrun_start "$?"' DEBUG
    return "$command_status"
})bash";

std::uint64_t parse_id(std::string_view text) {
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (value == 0 || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid command marker id");
    }
    return value;
}

int parse_status(std::string_view text) {
    int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid command marker exit status");
    }
    return value;
}

}  // namespace

std::vector<CommandMarker> CommandProtocol::consume(std::string_view bytes) {
    std::vector<CommandMarker> markers;
    for (const char byte : bytes) {
        if (byte != '\0') {
            if (pending_field_.size() == kMaximumFieldBytes) {
                throw std::runtime_error("command marker field exceeds 64 KiB");
            }
            pending_field_.push_back(byte);
            continue;
        }

        fields_.push_back(std::move(pending_field_));
        pending_field_.clear();
        if (fields_.size() == kProtocolFields) {
            markers.push_back(decode_record());
            fields_.clear();
        }
    }
    return markers;
}

void CommandProtocol::finish() const {
    if (!pending_field_.empty() || !fields_.empty()) {
        throw std::runtime_error("incomplete command marker");
    }
}

CommandMarker CommandProtocol::decode_record() const {
    CommandMarker marker;
    marker.id = parse_id(fields_[1]);
    if (fields_[0] == "A") {
        marker.type = CommandMarkerType::Arm;
        marker.cwd = fields_[2];
        if (!fields_[3].empty()) {
            throw std::runtime_error("command arm marker has unexpected data");
        }
        return marker;
    }
    if (fields_[0] == "S") {
        marker.type = CommandMarkerType::Start;
        marker.cwd = fields_[2];
        marker.text = fields_[3];
        const auto history_prefix = marker.text.find('\034');
        if (history_prefix == std::string::npos) {
            throw std::runtime_error("command marker is missing its history prefix");
        }
        marker.text.erase(0, history_prefix + 1);
        if (!marker.text.empty() && marker.text.back() == '\n') {
            marker.text.pop_back();
        }
        return marker;
    }
    if (fields_[0] == "E") {
        marker.type = CommandMarkerType::End;
        marker.exit_code = parse_status(fields_[2]);
        marker.cwd = fields_[3];
        return marker;
    }
    throw std::runtime_error("unknown command marker type");
}

BashSession::BashSession() {
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd == -1) {
        throw std::system_error(errno, std::generic_category(),
                                "open command context sink");
    }

    context_fd_ = fcntl(null_fd, F_DUPFD, 10);
    const int duplicate_error = errno;
    close(null_fd);
    if (context_fd_ == -1) {
        throw std::system_error(duplicate_error, std::generic_category(),
                                "duplicate command context descriptor");
    }

    request_.command = {"/bin/bash", "--noprofile", "--norc", "-i"};
    request_.command_channel_fd = context_fd_;
    request_.environment = {
        {"BASH_FUNC___whyrun_start%%", std::string(kStartFunction)},
        {"BASH_FUNC___whyrun_prompt%%", std::string(kPromptFunction)},
        {"PROMPT_COMMAND", "__whyrun_prompt \"$?\""},
        {"WHYRUN_CONTEXT_FD", std::to_string(context_fd_)},
        {"__whyrun_at_prompt", "0"},
        {"__whyrun_skip_debug", "0"},
        {"__whyrun_sequence", "0"},
        {"__whyrun_active_command", "0"},
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

}  // namespace whyrun

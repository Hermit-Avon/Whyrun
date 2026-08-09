#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>

namespace whyrun {

enum class EventType {
    ProcessExec,
    ProcessFork,
    ProcessExit,
    FileOpen,
    FileRead,
    FileWrite,
    NetworkConnect,
    LocalIpcConnect,
};

struct Event {
    std::uint64_t timestamp_ns{};
    pid_t pid{};
    pid_t tid{};
    EventType type{};
    std::string resource;
    std::int64_t result{};
    int errno_value{};
    std::unordered_map<std::string, std::string> metadata;
};

std::string_view event_type_name(EventType type);

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(const Event& event) = 0;
};

}  // namespace whyrun

#include "whyrun/event.hpp"

#include <stdexcept>

namespace whyrun {

std::string_view event_type_name(EventType type) {
    switch (type) {
        case EventType::ProcessExec:
            return "process_exec";
        case EventType::ProcessFork:
            return "process_fork";
        case EventType::ProcessExit:
            return "process_exit";
        case EventType::FileOpen:
            return "file_open";
        case EventType::FileRead:
            return "file_read";
        case EventType::FileWrite:
            return "file_write";
        case EventType::NetworkConnect:
            return "network_connect";
        case EventType::LocalIpcConnect:
            return "local_ipc_connect";
    }
    throw std::logic_error("unknown event type");
}

}  // namespace whyrun

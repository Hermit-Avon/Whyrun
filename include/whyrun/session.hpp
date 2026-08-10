#pragma once

#include "whyrun/collector.hpp"

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace whyrun {

struct RecordedCommand {
    std::uint64_t sequence{};
    std::uint64_t completed_ns{};
    pid_t shell_pid{};
    std::string text;
    std::string completed_cwd;
    int exit_code{};
};

class BashSession {
public:
    BashSession();
    ~BashSession();

    BashSession(const BashSession&) = delete;
    BashSession& operator=(const BashSession&) = delete;

    const CollectionRequest& request() const { return request_; }
    std::vector<RecordedCommand> read_commands();

private:
    int context_fd_{-1};
    bool commands_read_{false};
    CollectionRequest request_;
};

}  // namespace whyrun

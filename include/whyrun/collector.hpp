#pragma once

#include "whyrun/event.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace whyrun {

struct CollectionResult {
    bool collector_ok{false};
    int exit_code{-1};
    int term_signal{};
    std::uint64_t end_ns{};
    std::string error;
};

struct CollectionRequest {
    std::vector<std::string> command;
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::string> root_command;
    std::optional<int> command_channel_fd;
};

class Collector {
public:
    virtual ~Collector() = default;

    virtual CollectionResult collect(
        const CollectionRequest& request,
        EventSink& sink) = 0;
};

std::unique_ptr<Collector> make_ptrace_collector();

}  // namespace whyrun

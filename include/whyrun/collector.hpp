#pragma once

#include "whyrun/event.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace whyrun {

struct CollectionResult {
    bool collector_ok{false};
    int exit_code{-1};
    int term_signal{};
    std::uint64_t end_ns{};
    std::string error;
};

class Collector {
public:
    virtual ~Collector() = default;

    virtual CollectionResult collect(
        const std::vector<std::string>& command,
        EventSink& sink) = 0;
};

std::unique_ptr<Collector> make_ptrace_collector();

}  // namespace whyrun


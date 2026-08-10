#pragma once

#include "whyrun/collector.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace whyrun {

enum class CommandMarkerType {
    Arm,
    Start,
    End,
};

struct CommandMarker {
    CommandMarkerType type{};
    std::uint64_t id{};
    std::string text;
    std::string cwd;
    int exit_code{};
};

class CommandProtocol {
public:
    std::vector<CommandMarker> consume(std::string_view bytes);
    void finish() const;

private:
    CommandMarker decode_record() const;

    std::string pending_field_;
    std::vector<std::string> fields_;
};

class BashSession {
public:
    BashSession();
    ~BashSession();

    BashSession(const BashSession&) = delete;
    BashSession& operator=(const BashSession&) = delete;

    const CollectionRequest& request() const { return request_; }

private:
    int context_fd_{-1};
    CollectionRequest request_;
};

}  // namespace whyrun

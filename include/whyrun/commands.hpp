#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace whyrun {

struct ShowOptions {
    bool events{false};
    std::optional<std::int64_t> command_id;
};

int show_capsule(const std::filesystem::path& path, const ShowOptions& options);
int diff_capsules(const std::filesystem::path& before,
                  const std::filesystem::path& after);

}  // namespace whyrun

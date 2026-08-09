#pragma once

#include <filesystem>

namespace whyrun {

int show_capsule(const std::filesystem::path& path, bool show_events);
int diff_capsules(const std::filesystem::path& before,
                  const std::filesystem::path& after);

}  // namespace whyrun


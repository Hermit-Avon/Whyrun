#pragma once

#include "whyrun/collector.hpp"
#include "whyrun/session.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace whyrun {

enum class RunMode {
    Command,
    Session,
};

std::string_view run_mode_name(RunMode mode);

struct RunMetadata {
    std::string id;
    RunMode mode{RunMode::Command};
    std::vector<std::string> command;
    std::string cwd;
    std::uint64_t start_ns{};
};

class CapsuleWriter : public EventSink {
public:
    CapsuleWriter(const std::filesystem::path& path, const RunMetadata& run);
    ~CapsuleWriter() override;

    CapsuleWriter(const CapsuleWriter&) = delete;
    CapsuleWriter& operator=(const CapsuleWriter&) = delete;
    CapsuleWriter(CapsuleWriter&&) noexcept;
    CapsuleWriter& operator=(CapsuleWriter&&) noexcept;

    void emit(const Event& event) override;
    void add_command(const RecordedCommand& command);
    void finish(const CollectionResult& result);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::filesystem::path default_capsule_path();
std::filesystem::path temporary_capsule_path(std::string_view run_id);
void publish_capsule(const std::filesystem::path& temporary_path,
                     const std::filesystem::path& final_path);
std::string format_command(const std::vector<std::string>& command);

}  // namespace whyrun

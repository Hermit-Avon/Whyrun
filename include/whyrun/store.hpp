#pragma once

#include "whyrun/collector.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace whyrun {

struct RunMetadata {
    std::string id;
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
    void finish(const CollectionResult& result);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::filesystem::path default_capsule_path();
std::string format_command(const std::vector<std::string>& command);

}  // namespace whyrun


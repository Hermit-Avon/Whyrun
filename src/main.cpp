#include "whyrun/collector.hpp"
#include "whyrun/commands.hpp"
#include "whyrun/store.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

void usage(std::ostream& output) {
    output << "WhyRun 0.1.0 - semantic Linux execution recorder\n\n"
              "Usage:\n"
              "  whyrun record -- <command> [args...]\n"
              "  whyrun show <capsule.wrun> [--events]\n"
              "  whyrun diff <a.wrun> <b.wrun>\n";
}

int record_command(int argc, char** argv) {
    if (argc < 4 || std::string_view(argv[2]) != "--") {
        std::cerr << "whyrun record: expected -- followed by a command\n";
        return 2;
    }

    std::vector<std::string> command;
    for (int index = 3; index < argc; ++index) {
        command.emplace_back(argv[index]);
    }

    const auto capsule_path = whyrun::default_capsule_path();
    const auto start = now_ns();
    std::error_code cwd_error;
    const auto cwd = std::filesystem::current_path(cwd_error);
    whyrun::RunMetadata metadata{capsule_path.stem().string(), command,
                                 cwd_error ? std::string{} : cwd.string(), start};

    try {
        whyrun::CapsuleWriter writer(capsule_path, metadata);
        auto collector = whyrun::make_ptrace_collector();
        const auto result = collector->collect(command, writer);
        writer.finish(result);
        if (!result.collector_ok) {
            std::cerr << "whyrun record: " << result.error << '\n';
            std::cerr << "partial capsule: " << capsule_path.string() << '\n';
            return 1;
        }
        std::cout << "\nCapsule\n  " << capsule_path.string() << '\n';
        return result.exit_code;
    } catch (const std::exception& error) {
        std::cerr << "whyrun record: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(std::cerr);
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "record") {
        return record_command(argc, argv);
    }
    if (command == "show") {
        if (argc != 3 && argc != 4) {
            usage(std::cerr);
            return 2;
        }
        const bool events = argc == 4 && std::string_view(argv[3]) == "--events";
        if (argc == 4 && !events) {
            usage(std::cerr);
            return 2;
        }
        return whyrun::show_capsule(argv[2], events);
    }
    if (command == "diff") {
        if (argc != 4) {
            usage(std::cerr);
            return 2;
        }
        return whyrun::diff_capsules(argv[2], argv[3]);
    }
    if (command == "--help" || command == "-h") {
        usage(std::cout);
        return 0;
    }
    if (command == "--version") {
        std::cout << "WhyRun 0.1.0\n";
        return 0;
    }

    std::cerr << "whyrun: unknown command: " << command << '\n';
    usage(std::cerr);
    return 2;
}

#include "whyrun/collector.hpp"
#include "whyrun/commands.hpp"
#include "whyrun/store.hpp"
#include "whyrun/version.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TemporaryCapsuleGuard {
public:
    explicit TemporaryCapsuleGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryCapsuleGuard() {
        if (active_) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }

    TemporaryCapsuleGuard(const TemporaryCapsuleGuard&) = delete;
    TemporaryCapsuleGuard& operator=(const TemporaryCapsuleGuard&) = delete;

    void release() { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

bool is_help_flag(std::string_view value) {
    return value == "--help" || value == "-h";
}

void print_general_help(std::ostream& output) {
    output << "WhyRun " << whyrun::kVersion
           << " - semantic Linux execution recorder\n\n"
              "Usage:\n"
              "  whyrun <command> [options]\n\n"
              "Commands:\n"
              "  record   Record a command into an execution capsule\n"
              "  show     Show a capsule summary or semantic event timeline\n"
              "  diff     Compare two execution capsules\n"
              "  help     Show general or command-specific help\n"
              "  version  Show the WhyRun version\n\n"
              "Run 'whyrun help <command>' for command-specific help.\n";
}

bool print_command_help(std::string_view command, std::ostream& output) {
    if (command != "record" && command != "show" && command != "diff" &&
        command != "help" && command != "version") {
        return false;
    }

    output << "WhyRun " << whyrun::kVersion << "\n\n";
    if (command == "record") {
        output << "Usage:\n"
                  "  whyrun record -- <command> [args...]\n\n"
                  "Record a command and its process tree into a .wrun capsule.\n";
        return true;
    }
    if (command == "show") {
        output << "Usage:\n"
                  "  whyrun show <capsule.wrun> [--events]\n\n"
                  "Options:\n"
                  "  --events  Include the semantic event timeline\n";
        return true;
    }
    if (command == "diff") {
        output << "Usage:\n"
                  "  whyrun diff <a.wrun> <b.wrun>\n\n"
                  "Compare exit status, files, network, local IPC, and processes.\n";
        return true;
    }
    if (command == "help") {
        output << "Usage:\n"
                  "  whyrun help [command]\n\n"
                  "Show general help or detailed help for one command.\n";
        return true;
    }
    output << "Usage:\n"
              "  whyrun version\n\n"
              "Show the WhyRun version.\n";
    return true;
}

int help_command(int argc, char** argv) {
    if (argc == 2 || (argc == 3 && is_help_flag(argv[2]))) {
        print_general_help(std::cout);
        return 0;
    }
    if (argc != 3) {
        std::cerr << "whyrun help: expected at most one command\n";
        return 2;
    }
    if (!print_command_help(argv[2], std::cout)) {
        std::cerr << "whyrun help: unknown command: " << argv[2] << '\n';
        return 2;
    }
    return 0;
}

int record_command(int argc, char** argv) {
    if (argc < 4 || std::string_view(argv[2]) != "--") {
        std::cerr << "whyrun record: expected -- followed by a command\n";
        std::cerr << "Try 'whyrun help record'.\n";
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
        const auto temporary_path = whyrun::temporary_capsule_path(metadata.id);
        TemporaryCapsuleGuard temporary_guard(temporary_path);
        whyrun::CollectionResult result;
        {
            whyrun::CapsuleWriter writer(temporary_path, metadata);
            auto collector = whyrun::make_ptrace_collector();
            result = collector->collect(command, writer);
            writer.finish(result);
        }
        whyrun::publish_capsule(temporary_path, capsule_path);
        temporary_guard.release();
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
        print_general_help(std::cerr);
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "help") {
        return help_command(argc, argv);
    }
    if (command == "--help" || command == "-h") {
        if (argc != 2) {
            std::cerr << "whyrun: global help does not accept arguments\n";
            return 2;
        }
        print_general_help(std::cout);
        return 0;
    }
    if (command == "version" || command == "--version") {
        if (argc != 2) {
            std::cerr << "whyrun version: unexpected argument\n";
            return 2;
        }
        std::cout << "WhyRun " << whyrun::kVersion << '\n';
        return 0;
    }

    if ((command == "record" || command == "show" || command == "diff") &&
        argc == 3 && is_help_flag(argv[2])) {
        print_command_help(command, std::cout);
        return 0;
    }

    if (command == "record") {
        return record_command(argc, argv);
    }
    if (command == "show") {
        if (argc != 3 && argc != 4) {
            std::cerr << "whyrun show: expected a capsule path and optional --events\n";
            std::cerr << "Try 'whyrun help show'.\n";
            return 2;
        }
        const bool events = argc == 4 && std::string_view(argv[3]) == "--events";
        if (argc == 4 && !events) {
            std::cerr << "whyrun show: unknown option: " << argv[3] << '\n';
            std::cerr << "Try 'whyrun help show'.\n";
            return 2;
        }
        return whyrun::show_capsule(argv[2], events);
    }
    if (command == "diff") {
        if (argc != 4) {
            std::cerr << "whyrun diff: expected two capsule paths\n";
            std::cerr << "Try 'whyrun help diff'.\n";
            return 2;
        }
        return whyrun::diff_capsules(argv[2], argv[3]);
    }

    std::cerr << "whyrun: unknown command: " << command << '\n';
    std::cerr << "Try 'whyrun help'.\n";
    return 2;
}

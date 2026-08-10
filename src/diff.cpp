#include "whyrun/commands.hpp"

#include "capsule_reader.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace whyrun {
namespace {

struct Counts {
    std::int64_t calls{};
    std::int64_t successes{};
    std::int64_t failures{};
    std::int64_t bytes{};

    bool operator==(const Counts&) const = default;
};

struct Summary {
    std::int64_t exit_code{};
    bool session{};
    std::map<std::string, Counts> reads;
    std::map<std::string, Counts> writes;
    std::map<std::string, Counts> network;
    std::map<std::string, Counts> local_ipc;
    std::set<std::string> executables;
    std::set<std::string> commands;
};

Summary load_summary(const std::filesystem::path& path) {
    detail::ReadDatabase database(path);
    const int schema_version = detail::validate_schema(database.get());
    Summary summary;

    detail::ReadStatement run(database.get(), "SELECT exit_code FROM runs LIMIT 1");
    if (!run.next()) {
        throw std::runtime_error("capsule does not contain a run: " + path.string());
    }
    summary.exit_code = run.integer(0);

    if (schema_version >= 2) {
        detail::ReadStatement mode(database.get(), "SELECT mode FROM runs LIMIT 1");
        if (mode.next()) {
            summary.session = mode.text(0) == "session";
        }
        if (summary.session) {
            detail::ReadStatement commands(
                database.get(), "SELECT DISTINCT command FROM commands ORDER BY command");
            while (commands.next()) {
                summary.commands.insert(commands.text(0));
            }
        }
    }

    detail::ReadStatement files(
        database.get(),
        "SELECT path, read_calls, write_calls, bytes_read, bytes_written "
        "FROM file_activity");
    while (files.next()) {
        if (files.integer(1) > 0) {
            summary.reads[files.text(0)] = Counts{files.integer(1), 0, 0,
                                                  files.integer(3)};
        }
        if (files.integer(2) > 0) {
            summary.writes[files.text(0)] = Counts{files.integer(2), 0, 0,
                                                   files.integer(4)};
        }
    }

    detail::ReadStatement network(
        database.get(),
        "SELECT endpoint, connect_count, success_count, failure_count "
        "FROM network_activity");
    while (network.next()) {
        summary.network[network.text(0)] = Counts{
            network.integer(1), network.integer(2), network.integer(3), 0};
    }

    detail::ReadStatement local_ipc(
        database.get(),
        "SELECT resource, COUNT(*), "
        "SUM(CASE WHEN errno_value=0 THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN errno_value>0 THEN 1 ELSE 0 END) "
        "FROM events WHERE type='local_ipc_connect' GROUP BY resource");
    while (local_ipc.next()) {
        summary.local_ipc[local_ipc.text(0)] = Counts{
            local_ipc.integer(1), local_ipc.integer(2), local_ipc.integer(3), 0};
    }

    detail::ReadStatement processes(
        database.get(),
        "SELECT DISTINCT executable FROM processes WHERE executable IS NOT NULL "
        "AND executable<>'' ORDER BY executable");
    while (processes.next()) {
        summary.executables.insert(processes.text(0));
    }
    return summary;
}

bool print_activity_diff(std::string_view label,
                         const std::map<std::string, Counts>& before,
                         const std::map<std::string, Counts>& after,
                         bool network = false) {
    bool changed = false;
    for (const auto& [name, counts] : before) {
        const auto found = after.find(name);
        if (found == after.end()) {
            std::cout << "- " << label << name << '\n';
            changed = true;
        } else if (!(counts == found->second)) {
            std::cout << "~ " << label << name;
            if (network) {
                std::cout << " (attempts " << counts.calls << " -> " << found->second.calls
                          << ", failures " << counts.failures << " -> "
                          << found->second.failures << ')';
            } else {
                std::cout << " (calls " << counts.calls << " -> " << found->second.calls
                          << ", bytes " << counts.bytes << " -> " << found->second.bytes
                          << ')';
            }
            std::cout << '\n';
            changed = true;
        }
    }
    for (const auto& [name, counts] : after) {
        static_cast<void>(counts);
        if (!before.contains(name)) {
            std::cout << "+ " << label << name << '\n';
            changed = true;
        }
    }
    return changed;
}

bool print_set_diff(const std::set<std::string>& before,
                    const std::set<std::string>& after) {
    bool changed = false;
    for (const auto& value : before) {
        if (!after.contains(value)) {
            std::cout << "- " << value << '\n';
            changed = true;
        }
    }
    for (const auto& value : after) {
        if (!before.contains(value)) {
            std::cout << "+ " << value << '\n';
            changed = true;
        }
    }
    return changed;
}

}  // namespace

int diff_capsules(const std::filesystem::path& before,
                  const std::filesystem::path& after) {
    try {
        const auto a = load_summary(before);
        const auto b = load_summary(after);

        std::cout << "Exit\n";
        if (a.exit_code == b.exit_code) {
            std::cout << "  unchanged (" << a.exit_code << ")\n";
        } else {
            std::cout << "- " << a.exit_code << "\n+ " << b.exit_code << '\n';
        }
        std::cout << '\n';

        if (a.session || b.session) {
            std::cout << "Commands\n";
            if (!print_set_diff(a.commands, b.commands)) {
                std::cout << "  none\n";
            }
            std::cout << '\n';
        }

        std::cout << "Files\n";
        bool file_changes = print_activity_diff("READ ", a.reads, b.reads);
        file_changes = print_activity_diff("WRITE ", a.writes, b.writes) || file_changes;
        if (!file_changes) {
            std::cout << "  none\n";
        }
        std::cout << '\n';

        std::cout << "Network\n";
        if (!print_activity_diff("", a.network, b.network, true)) {
            std::cout << "  none\n";
        }
        std::cout << '\n';

        std::cout << "Local IPC\n";
        if (!print_activity_diff("", a.local_ipc, b.local_ipc, true)) {
            std::cout << "  none\n";
        }
        std::cout << '\n';

        std::cout << "Processes\n";
        if (!print_set_diff(a.executables, b.executables)) {
            std::cout << "  none\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "whyrun diff: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace whyrun

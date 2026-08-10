#include "whyrun/commands.hpp"

#include "capsule_reader.hpp"

#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace whyrun {
namespace {

std::string errno_name(int value) {
    switch (value) {
        case EACCES:
            return "EACCES";
        case EADDRINUSE:
            return "EADDRINUSE";
        case EADDRNOTAVAIL:
            return "EADDRNOTAVAIL";
        case ECONNREFUSED:
            return "ECONNREFUSED";
        case ECONNRESET:
            return "ECONNRESET";
        case EEXIST:
            return "EEXIST";
        case EINPROGRESS:
            return "EINPROGRESS";
        case EINVAL:
            return "EINVAL";
        case EISDIR:
            return "EISDIR";
        case EMFILE:
            return "EMFILE";
        case ENETUNREACH:
            return "ENETUNREACH";
        case ENOENT:
            return "ENOENT";
        case ENOTDIR:
            return "ENOTDIR";
        case EPERM:
            return "EPERM";
        case EPIPE:
            return "EPIPE";
        case ETIMEDOUT:
            return "ETIMEDOUT";
        default:
            return "ERRNO_" + std::to_string(value);
    }
}

void show_files(sqlite3* database) {
    std::cout << "Files\n";
    for (const auto* mode : {"READ", "WRITE"}) {
        std::cout << "  " << mode << "\n";
        const bool read = std::string_view(mode) == "READ";
        const std::string sql =
            std::string("SELECT path, ") + (read ? "read_calls, bytes_read" :
                                                   "write_calls, bytes_written") +
            " FROM file_activity WHERE " + (read ? "read_calls" : "write_calls") +
            ">0 ORDER BY path";
        detail::ReadStatement statement(database, sql);
        bool any = false;
        while (statement.next()) {
            any = true;
            std::cout << "    " << statement.text(0) << " (" << statement.integer(1)
                      << " calls, " << statement.integer(2) << " bytes)\n";
        }
        if (!any) {
            std::cout << "    none\n";
        }
        std::cout << '\n';
    }
}

void show_network(sqlite3* database) {
    std::cout << "Network\n";
    detail::ReadStatement statement(
        database,
        "SELECT endpoint, connect_count, success_count, failure_count, last_errno "
        "FROM network_activity ORDER BY endpoint");
    bool any = false;
    while (statement.next()) {
        any = true;
        std::cout << "  " << statement.text(0) << " (" << statement.integer(1)
                  << " attempts, " << statement.integer(2) << " succeeded, "
                  << statement.integer(3) << " failed";
        if (statement.integer(4) != 0) {
            std::cout << ", last " << errno_name(static_cast<int>(statement.integer(4)));
        }
        std::cout << ")\n";
    }
    if (!any) {
        std::cout << "  none\n";
    }
    std::cout << '\n';
}

void show_local_ipc(sqlite3* database) {
    std::cout << "Local IPC\n";
    detail::ReadStatement statement(
        database,
        "SELECT resource, COUNT(*), "
        "SUM(CASE WHEN errno_value=0 THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN errno_value>0 THEN 1 ELSE 0 END) "
        "FROM events WHERE type='local_ipc_connect' "
        "GROUP BY resource ORDER BY resource");
    bool any = false;
    while (statement.next()) {
        any = true;
        std::cout << "  " << statement.text(0) << " (" << statement.integer(1)
                  << " attempts, " << statement.integer(2) << " succeeded, "
                  << statement.integer(3) << " failed)\n";
    }
    if (!any) {
        std::cout << "  none\n";
    }
    std::cout << '\n';
}

void show_errors(sqlite3* database) {
    std::cout << "Errors\n";
    detail::ReadStatement statement(
        database,
        "SELECT resource, type, errno_value, COUNT(*) FROM events WHERE errno_value>0 "
        "GROUP BY resource, type, errno_value ORDER BY resource, type, errno_value");
    bool any = false;
    while (statement.next()) {
        any = true;
        std::string operation = statement.text(1);
        if (operation.ends_with("_connect")) {
            operation = "connect";
        } else if (const auto separator = operation.find('_');
                   separator != std::string::npos) {
            operation.erase(0, separator + 1);
        }
        std::cout << "  " << statement.text(0) << '\n'
                  << "    " << operation << " -> "
                  << errno_name(static_cast<int>(statement.integer(2)));
        if (statement.integer(3) > 1) {
            std::cout << " (" << statement.integer(3) << " times)";
        }
        std::cout << '\n';
    }
    if (!any) {
        std::cout << "  none\n";
    }
    std::cout << '\n';
}

void show_commands(sqlite3* database) {
    std::cout << "Commands\n";
    detail::ReadStatement statement(
        database,
        "SELECT sequence, command, exit_code FROM commands ORDER BY sequence");
    bool any = false;
    while (statement.next()) {
        any = true;
        std::cout << "  " << statement.integer(0) << "  ";
        const std::string command = statement.text(1);
        for (const char character : command) {
            if (character == '\n') {
                std::cout << "\n     ";
            } else {
                std::cout << character;
            }
        }
        std::cout << " (exit " << statement.integer(2) << ")\n";
    }
    if (!any) {
        std::cout << "  none\n";
    }
    std::cout << '\n';
}

void show_timeline(sqlite3* database, std::int64_t start_ns) {
    std::cout << "Events\n";
    detail::ReadStatement statement(
        database,
        "SELECT timestamp_ns, pid, tid, type, resource, result, errno_value "
        "FROM events ORDER BY timestamp_ns, id");
    while (statement.next()) {
        const double milliseconds =
            static_cast<double>(statement.integer(0) - start_ns) / 1'000'000.0;
        std::cout << "  +" << std::fixed << std::setprecision(3) << milliseconds
                  << " ms pid=" << statement.integer(1) << " tid=" << statement.integer(2)
                  << ' ' << statement.text(3);
        if (!statement.text(4).empty()) {
            std::cout << ' ' << statement.text(4);
        }
        std::cout << " -> " << statement.integer(5);
        if (statement.integer(6) != 0) {
            std::cout << " (" << errno_name(static_cast<int>(statement.integer(6))) << ')';
        }
        std::cout << '\n';
    }
}

}  // namespace

int show_capsule(const std::filesystem::path& path, bool show_events) {
    try {
        detail::ReadDatabase database(path);
        const int schema_version = detail::validate_schema(database.get());

        detail::ReadStatement run(
            database.get(),
            schema_version >= 2
                ? "SELECT mode, command, exit_code, term_signal, start_ns "
                  "FROM runs LIMIT 1"
                : "SELECT 'command', command, exit_code, term_signal, start_ns "
                  "FROM runs LIMIT 1");
        if (!run.next()) {
            throw std::runtime_error("capsule does not contain a run");
        }
        const std::string mode = run.text(0);
        const std::string command = run.text(1);
        const auto exit_code = run.integer(2);
        const auto term_signal = run.integer(3);
        const auto start_ns = run.integer(4);

        if (mode == "session") {
            std::cout << "Session\n  shell " << command << "\n\n";
        } else {
            std::cout << "Command\n  " << command << "\n\n";
        }
        std::cout << "Exit\n  code " << exit_code;
        if (term_signal != 0) {
            std::cout << " (signal " << term_signal << ')';
        }
        std::cout << "\n\n";

        if (mode == "session") {
            show_commands(database.get());
        }

        detail::ReadStatement processes(database.get(), "SELECT COUNT(*) FROM processes");
        processes.next();
        std::cout << "Processes\n  " << processes.integer(0) << "\n\n";

        show_files(database.get());
        show_network(database.get());
        show_local_ipc(database.get());
        show_errors(database.get());
        if (show_events) {
            show_timeline(database.get(), start_ns);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "whyrun show: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace whyrun

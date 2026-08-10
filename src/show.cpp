#include "whyrun/commands.hpp"

#include "capsule_reader.hpp"

#include <cerrno>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace whyrun {
namespace {

using CommandFilter = std::optional<std::int64_t>;

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

std::string command_clause(CommandFilter command_id,
                           std::string_view table_prefix = {}) {
    if (!command_id.has_value()) {
        return {};
    }
    return " AND " + std::string(table_prefix) + "command_id=" +
           std::to_string(*command_id);
}

void print_command_text(std::string_view command, std::string_view indent) {
    for (const char character : command) {
        if (character == '\n') {
            std::cout << '\n' << indent;
        } else {
            std::cout << character;
        }
    }
}

void show_files(sqlite3* database, CommandFilter command_id) {
    std::cout << "Files\n";
    for (const auto* mode : {"READ", "WRITE"}) {
        std::cout << "  " << mode << "\n";
        const bool read = std::string_view(mode) == "READ";
        std::string sql;
        if (command_id.has_value()) {
            sql = "SELECT resource, COUNT(*), "
                  "COALESCE(SUM(CASE WHEN result>0 THEN result ELSE 0 END), 0) "
                  "FROM events WHERE type='" +
                  std::string(read ? "file_read" : "file_write") + "'" +
                  command_clause(command_id) +
                  " GROUP BY resource ORDER BY resource";
        } else {
            sql = std::string("SELECT path, ") +
                  (read ? "read_calls, bytes_read" :
                          "write_calls, bytes_written") +
                  " FROM file_activity WHERE " +
                  (read ? "read_calls" : "write_calls") + ">0 ORDER BY path";
        }

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

void show_network(sqlite3* database, CommandFilter command_id) {
    std::cout << "Network\n";
    std::string sql;
    if (command_id.has_value()) {
        const std::string id = std::to_string(*command_id);
        sql = "SELECT e.resource, COUNT(*), "
              "SUM(CASE WHEN e.errno_value=0 THEN 1 ELSE 0 END), "
              "SUM(CASE WHEN e.errno_value>0 THEN 1 ELSE 0 END), "
              "COALESCE((SELECT e2.errno_value FROM events e2 "
              "WHERE e2.type='network_connect' AND e2.command_id=" +
              id +
              " AND e2.resource=e.resource ORDER BY e2.timestamp_ns DESC, e2.id DESC "
              "LIMIT 1), 0) FROM events e WHERE e.type='network_connect' "
              "AND e.command_id=" +
              id + " GROUP BY e.resource ORDER BY e.resource";
    } else {
        sql = "SELECT endpoint, connect_count, success_count, failure_count, "
              "last_errno FROM network_activity ORDER BY endpoint";
    }

    detail::ReadStatement statement(database, sql);
    bool any = false;
    while (statement.next()) {
        any = true;
        std::cout << "  " << statement.text(0) << " (" << statement.integer(1)
                  << " attempts, " << statement.integer(2) << " succeeded, "
                  << statement.integer(3) << " failed";
        if (statement.integer(4) != 0) {
            std::cout << ", last "
                      << errno_name(static_cast<int>(statement.integer(4)));
        }
        std::cout << ")\n";
    }
    if (!any) {
        std::cout << "  none\n";
    }
    std::cout << '\n';
}

void show_local_ipc(sqlite3* database, CommandFilter command_id) {
    std::cout << "Local IPC\n";
    const std::string sql =
        "SELECT resource, COUNT(*), "
        "SUM(CASE WHEN errno_value=0 THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN errno_value>0 THEN 1 ELSE 0 END) "
        "FROM events WHERE type='local_ipc_connect'" +
        command_clause(command_id) + " GROUP BY resource ORDER BY resource";
    detail::ReadStatement statement(database, sql);
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

void show_errors(sqlite3* database, CommandFilter command_id) {
    std::cout << "Errors\n";
    const std::string sql =
        "SELECT resource, type, errno_value, COUNT(*) FROM events "
        "WHERE errno_value>0" +
        command_clause(command_id) +
        " GROUP BY resource, type, errno_value "
        "ORDER BY resource, type, errno_value";
    detail::ReadStatement statement(database, sql);
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
        print_command_text(statement.text(1), "     ");
        if (statement.is_null(2)) {
            std::cout << " (incomplete)\n";
        } else {
            std::cout << " (exit " << statement.integer(2) << ")\n";
        }
    }
    if (!any) {
        std::cout << "  none\n";
    }
    std::cout << '\n';
}

std::int64_t show_command_header(sqlite3* database, std::int64_t command_id) {
    detail::ReadStatement statement(
        database,
        "SELECT sequence, command, cwd, start_ns, exit_code FROM commands WHERE id=" +
            std::to_string(command_id));
    if (!statement.next()) {
        throw std::runtime_error("capsule does not contain command " +
                                 std::to_string(command_id));
    }

    std::cout << "Command " << statement.integer(0) << "\n  ";
    print_command_text(statement.text(1), "  ");
    std::cout << "\n\nCwd\n  " << statement.text(2) << "\n\nExit\n  ";
    if (statement.is_null(4)) {
        std::cout << "incomplete\n\n";
    } else {
        std::cout << "code " << statement.integer(4) << "\n\n";
    }
    return statement.integer(3);
}

void show_processes(sqlite3* database, CommandFilter command_id) {
    std::string sql = "SELECT COUNT(*) FROM processes";
    if (command_id.has_value()) {
        sql = "SELECT COUNT(DISTINCT pid) FROM events WHERE type='process_exec'" +
              command_clause(command_id);
    }
    detail::ReadStatement processes(database, sql);
    processes.next();
    std::cout << "Processes\n  " << processes.integer(0) << "\n\n";
}

void show_timeline(sqlite3* database, std::int64_t start_ns,
                   CommandFilter command_id) {
    std::cout << "Events\n";
    const std::string sql =
        "SELECT timestamp_ns, pid, tid, type, resource, result, errno_value "
        "FROM events WHERE 1=1" +
        command_clause(command_id) +
        " ORDER BY timestamp_ns, CASE type WHEN 'command_start' THEN 0 ELSE 1 END, id";
    detail::ReadStatement statement(database, sql);
    while (statement.next()) {
        const double milliseconds =
            static_cast<double>(statement.integer(0) - start_ns) / 1'000'000.0;
        std::cout << "  +" << std::fixed << std::setprecision(3) << milliseconds
                  << " ms pid=" << statement.integer(1)
                  << " tid=" << statement.integer(2) << ' ' << statement.text(3);
        if (!statement.text(4).empty()) {
            std::cout << ' ' << statement.text(4);
        }
        std::cout << " -> " << statement.integer(5);
        if (statement.integer(6) != 0) {
            std::cout << " (" << errno_name(static_cast<int>(statement.integer(6)))
                      << ')';
        }
        std::cout << '\n';
    }
}

std::int64_t show_run_header(sqlite3* database, int schema_version,
                             std::string& mode) {
    detail::ReadStatement run(
        database,
        schema_version >= 2
            ? "SELECT mode, command, exit_code, term_signal, start_ns "
              "FROM runs LIMIT 1"
            : "SELECT 'command', command, exit_code, term_signal, start_ns "
              "FROM runs LIMIT 1");
    if (!run.next()) {
        throw std::runtime_error("capsule does not contain a run");
    }
    mode = run.text(0);
    if (mode == "session") {
        std::cout << "Session\n  shell " << run.text(1) << "\n\n";
    } else {
        std::cout << "Command\n  " << run.text(1) << "\n\n";
    }
    std::cout << "Exit\n  code " << run.integer(2);
    if (run.integer(3) != 0) {
        std::cout << " (signal " << run.integer(3) << ')';
    }
    std::cout << "\n\n";
    return run.integer(4);
}

}  // namespace

int show_capsule(const std::filesystem::path& path, const ShowOptions& options) {
    try {
        detail::ReadDatabase database(path);
        const int schema_version = detail::validate_schema(database.get());
        const CommandFilter command_id = options.command_id;
        if (command_id.has_value() && schema_version < 3) {
            throw std::runtime_error(
                "capsule schema does not contain per-command attribution");
        }

        std::int64_t timeline_start{};
        std::string mode;
        if (command_id.has_value()) {
            timeline_start = show_command_header(database.get(), *command_id);
        } else {
            timeline_start = show_run_header(database.get(), schema_version, mode);
            if (mode == "session") {
                show_commands(database.get());
            }
        }

        show_processes(database.get(), command_id);
        show_files(database.get(), command_id);
        show_network(database.get(), command_id);
        show_local_ipc(database.get(), command_id);
        show_errors(database.get(), command_id);
        if (options.events) {
            show_timeline(database.get(), timeline_start, command_id);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "whyrun show: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace whyrun

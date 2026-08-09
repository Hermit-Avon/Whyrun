#include "whyrun/store.hpp"

#include "whyrun/event.hpp"

#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace whyrun {
namespace {

class Database {
public:
    Database(const std::filesystem::path& path, int flags) {
        const int status = sqlite3_open_v2(path.c_str(), &handle_, flags, nullptr);
        if (status != SQLITE_OK) {
            const std::string message = handle_ == nullptr ? "cannot open SQLite database"
                                                           : sqlite3_errmsg(handle_);
            if (handle_ != nullptr) {
                sqlite3_close(handle_);
                handle_ = nullptr;
            }
            throw std::runtime_error(message);
        }
        sqlite3_extended_result_codes(handle_, 1);
        sqlite3_busy_timeout(handle_, 5000);
    }

    ~Database() {
        if (handle_ != nullptr) {
            sqlite3_close(handle_);
        }
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* get() const { return handle_; }

    void execute(std::string_view sql) const {
        char* error = nullptr;
        const int status = sqlite3_exec(handle_, std::string(sql).c_str(), nullptr, nullptr,
                                        &error);
        if (status != SQLITE_OK) {
            const std::string message = error == nullptr ? sqlite3_errmsg(handle_) : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

private:
    sqlite3* handle_{};
};

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql) : database_(database) {
        if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()),
                              &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    void execute() {
        if (sqlite3_step(statement_) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

std::string json_escape(std::string_view input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(character) << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string metadata_json(
    const std::unordered_map<std::string, std::string>& metadata) {
    std::vector<std::pair<std::string, std::string>> entries(metadata.begin(),
                                                              metadata.end());
    std::sort(entries.begin(), entries.end());
    std::ostringstream output;
    output << '{';
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << '"' << json_escape(entries[index].first) << "\":\""
               << json_escape(entries[index].second) << '"';
    }
    output << '}';
    return output.str();
}

std::string metadata_value(const Event& event, const std::string& key,
                           std::string fallback = {}) {
    const auto found = event.metadata.find(key);
    return found == event.metadata.end() ? std::move(fallback) : found->second;
}

std::int64_t as_i64(std::uint64_t value) {
    return static_cast<std::int64_t>(value);
}

std::string shell_quote(std::string_view argument) {
    if (!argument.empty() &&
        std::all_of(argument.begin(), argument.end(), [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '/' || character == '_' ||
                   character == '-' || character == '.' || character == ':';
        })) {
        return std::string(argument);
    }

    std::string result{"'"};
    for (const char character : argument) {
        if (character == '\'') {
            result += "'\\''";
        } else {
            result += character;
        }
    }
    result += '\'';
    return result;
}

}  // namespace

class CapsuleWriter::Impl {
public:
    Impl(const std::filesystem::path& path, const RunMetadata& run)
        : database_(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE) {
        database_.execute("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        database_.execute(R"sql(
            CREATE TABLE metadata (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE runs (
                id TEXT PRIMARY KEY,
                command TEXT NOT NULL,
                cwd TEXT,
                start_ns INTEGER,
                end_ns INTEGER,
                exit_code INTEGER,
                term_signal INTEGER
            );
            CREATE TABLE processes (
                pid INTEGER PRIMARY KEY,
                ppid INTEGER,
                executable TEXT,
                start_ns INTEGER,
                end_ns INTEGER,
                exit_code INTEGER,
                term_signal INTEGER
            );
            CREATE TABLE events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp_ns INTEGER NOT NULL,
                pid INTEGER NOT NULL,
                tid INTEGER NOT NULL,
                type TEXT NOT NULL,
                resource TEXT,
                result INTEGER,
                errno_value INTEGER,
                metadata_json TEXT
            );
            CREATE TABLE file_activity (
                path TEXT PRIMARY KEY,
                read_calls INTEGER NOT NULL DEFAULT 0,
                write_calls INTEGER NOT NULL DEFAULT 0,
                bytes_read INTEGER NOT NULL DEFAULT 0,
                bytes_written INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE network_activity (
                endpoint TEXT PRIMARY KEY,
                connect_count INTEGER NOT NULL DEFAULT 0,
                success_count INTEGER NOT NULL DEFAULT 0,
                failure_count INTEGER NOT NULL DEFAULT 0,
                last_errno INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX events_by_time ON events(timestamp_ns, id);
            CREATE INDEX events_by_errno ON events(errno_value);
        )sql");
        database_.execute("BEGIN IMMEDIATE;");

        {
            Statement statement(database_.get(),
                                "INSERT INTO metadata(key, value) VALUES('schema_version', '1')");
            statement.execute();
        }
        {
            Statement statement(database_.get(),
                                "INSERT INTO metadata(key, value) VALUES('tool_version', '0.1.0')");
            statement.execute();
        }
        {
            Statement statement(database_.get(),
                                "INSERT INTO runs(id, command, cwd, start_ns) VALUES(?, ?, ?, ?)");
            statement.bind(1, run.id);
            statement.bind(2, format_command(run.command));
            statement.bind(3, run.cwd);
            statement.bind(4, as_i64(run.start_ns));
            statement.execute();
        }
    }

    ~Impl() {
        if (!finished_) {
            try {
                database_.execute("ROLLBACK;");
            } catch (...) {
            }
        }
    }

    void emit(const Event& event) {
        Statement insert_event(
            database_.get(),
            "INSERT INTO events(timestamp_ns, pid, tid, type, resource, result, "
            "errno_value, metadata_json) VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
        insert_event.bind(1, as_i64(event.timestamp_ns));
        insert_event.bind(2, static_cast<std::int64_t>(event.pid));
        insert_event.bind(3, static_cast<std::int64_t>(event.tid));
        insert_event.bind(4, event_type_name(event.type));
        insert_event.bind(5, event.resource);
        insert_event.bind(6, event.result);
        insert_event.bind(7, static_cast<std::int64_t>(event.errno_value));
        insert_event.bind(8, metadata_json(event.metadata));
        insert_event.execute();

        if (event.type == EventType::FileRead || event.type == EventType::FileWrite) {
            Statement activity(
                database_.get(),
                "INSERT INTO file_activity(path, read_calls, write_calls, bytes_read, "
                "bytes_written) VALUES(?, ?, ?, ?, ?) ON CONFLICT(path) DO UPDATE SET "
                "read_calls=read_calls+excluded.read_calls, "
                "write_calls=write_calls+excluded.write_calls, "
                "bytes_read=bytes_read+excluded.bytes_read, "
                "bytes_written=bytes_written+excluded.bytes_written");
            const bool is_read = event.type == EventType::FileRead;
            const auto bytes = std::max<std::int64_t>(0, event.result);
            activity.bind(1, event.resource);
            activity.bind(2, static_cast<std::int64_t>(is_read ? 1 : 0));
            activity.bind(3, static_cast<std::int64_t>(is_read ? 0 : 1));
            activity.bind(4, is_read ? bytes : 0);
            activity.bind(5, is_read ? 0 : bytes);
            activity.execute();
        } else if (event.type == EventType::NetworkConnect) {
            Statement activity(
                database_.get(),
                "INSERT INTO network_activity(endpoint, connect_count, success_count, "
                "failure_count, last_errno) VALUES(?, 1, ?, ?, ?) "
                "ON CONFLICT(endpoint) DO UPDATE SET "
                "connect_count=connect_count+1, "
                "success_count=success_count+excluded.success_count, "
                "failure_count=failure_count+excluded.failure_count, "
                "last_errno=excluded.last_errno");
            activity.bind(1, event.resource);
            activity.bind(2, static_cast<std::int64_t>(event.errno_value == 0 ? 1 : 0));
            activity.bind(3, static_cast<std::int64_t>(event.errno_value == 0 ? 0 : 1));
            activity.bind(4, static_cast<std::int64_t>(event.errno_value));
            activity.execute();
        } else if (event.type == EventType::ProcessExec) {
            Statement process(
                database_.get(),
                "INSERT INTO processes(pid, ppid, executable, start_ns) VALUES(?, ?, ?, ?) "
                "ON CONFLICT(pid) DO UPDATE SET executable=excluded.executable");
            process.bind(1, static_cast<std::int64_t>(event.pid));
            process.bind(2, static_cast<std::int64_t>(
                                std::stoll(metadata_value(event, "ppid", "0"))));
            process.bind(3, event.resource);
            process.bind(4, as_i64(event.timestamp_ns));
            process.execute();
        } else if (event.type == EventType::ProcessFork) {
            Statement process(
                database_.get(),
                "INSERT OR IGNORE INTO processes(pid, ppid, executable, start_ns) "
                "VALUES(?, ?, ?, ?)");
            process.bind(1, static_cast<std::int64_t>(
                                std::stoll(metadata_value(event, "child_pid", "0"))));
            process.bind(2, static_cast<std::int64_t>(event.pid));
            process.bind(3, metadata_value(event, "executable", "<unknown>"));
            process.bind(4, as_i64(event.timestamp_ns));
            process.execute();
        } else if (event.type == EventType::ProcessExit) {
            Statement process(database_.get(),
                              "UPDATE processes SET end_ns=?, exit_code=?, term_signal=? "
                              "WHERE pid=?");
            process.bind(1, as_i64(event.timestamp_ns));
            process.bind(2, static_cast<std::int64_t>(
                                std::stoll(metadata_value(event, "exit_code", "-1"))));
            process.bind(3, static_cast<std::int64_t>(
                                std::stoll(metadata_value(event, "term_signal", "0"))));
            process.bind(4, static_cast<std::int64_t>(event.pid));
            process.execute();
        }
    }

    void finish(const CollectionResult& result) {
        if (finished_) {
            throw std::logic_error("capsule has already been finalized");
        }
        Statement run(database_.get(),
                      "UPDATE runs SET end_ns=?, exit_code=?, term_signal=?");
        run.bind(1, as_i64(result.end_ns));
        run.bind(2, static_cast<std::int64_t>(result.exit_code));
        run.bind(3, static_cast<std::int64_t>(result.term_signal));
        run.execute();
        database_.execute("COMMIT;");
        finished_ = true;
    }

private:
    Database database_;
    bool finished_{false};
};

CapsuleWriter::CapsuleWriter(const std::filesystem::path& path, const RunMetadata& run)
    : impl_(std::make_unique<Impl>(path, run)) {}

CapsuleWriter::~CapsuleWriter() = default;
CapsuleWriter::CapsuleWriter(CapsuleWriter&&) noexcept = default;
CapsuleWriter& CapsuleWriter::operator=(CapsuleWriter&&) noexcept = default;

void CapsuleWriter::emit(const Event& event) {
    impl_->emit(event);
}

void CapsuleWriter::finish(const CollectionResult& result) {
    impl_->finish(result);
}

std::filesystem::path default_capsule_path() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);
    std::ostringstream base;
    base << "run-" << std::put_time(&local, "%Y%m%d-%H%M%S");

    auto candidate = std::filesystem::path(base.str() + ".wrun");
    for (unsigned int suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = base.str() + '-' + std::to_string(suffix) + ".wrun";
    }
    return candidate;
}

std::filesystem::path temporary_capsule_path(std::string_view run_id) {
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) {
        throw std::system_error(error, "locate temporary directory");
    }

    const std::string base = ".whyrun-" + std::string(run_id) + '-' +
                             std::to_string(getpid());
    for (unsigned int suffix = 0;; ++suffix) {
        const std::string name = base + (suffix == 0 ? std::string{} :
                                                       '-' + std::to_string(suffix)) +
                                 ".tmp";
        const auto candidate = directory / name;
        if (!std::filesystem::exists(candidate, error)) {
            if (error) {
                throw std::system_error(error, "inspect temporary capsule path");
            }
            return candidate;
        }
        if (error) {
            throw std::system_error(error, "inspect temporary capsule path");
        }
    }
}

void publish_capsule(const std::filesystem::path& temporary_path,
                     const std::filesystem::path& final_path) {
    std::error_code error;
    const bool copied = std::filesystem::copy_file(
        temporary_path, final_path, std::filesystem::copy_options::none, error);
    if (!copied) {
        if (!error) {
            error = std::make_error_code(std::errc::file_exists);
        }
        throw std::system_error(error, "publish capsule " + final_path.string());
    }

    // Collection has ended before the final path is created, so the tracee cannot
    // observe either the capsule or SQLite's rollback journal in its working tree.
    std::filesystem::remove(temporary_path, error);
}

std::string format_command(const std::vector<std::string>& command) {
    std::ostringstream output;
    for (std::size_t index = 0; index < command.size(); ++index) {
        if (index != 0) {
            output << ' ';
        }
        output << shell_quote(command[index]);
    }
    return output.str();
}

}  // namespace whyrun

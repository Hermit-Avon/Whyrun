#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace whyrun::detail {

class ReadDatabase {
public:
    explicit ReadDatabase(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READONLY, nullptr) !=
            SQLITE_OK) {
            const std::string message = database_ == nullptr
                                            ? "cannot open capsule"
                                            : sqlite3_errmsg(database_);
            if (database_ != nullptr) {
                sqlite3_close(database_);
                database_ = nullptr;
            }
            throw std::runtime_error(message);
        }
    }

    ~ReadDatabase() {
        if (database_ != nullptr) {
            sqlite3_close(database_);
        }
    }

    ReadDatabase(const ReadDatabase&) = delete;
    ReadDatabase& operator=(const ReadDatabase&) = delete;

    sqlite3* get() const { return database_; }

private:
    sqlite3* database_{};
};

class ReadStatement {
public:
    ReadStatement(sqlite3* database, std::string_view sql) : database_(database) {
        if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()),
                              &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~ReadStatement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    ReadStatement(const ReadStatement&) = delete;
    ReadStatement& operator=(const ReadStatement&) = delete;

    bool next() {
        const int status = sqlite3_step(statement_);
        if (status == SQLITE_ROW) {
            return true;
        }
        if (status == SQLITE_DONE) {
            return false;
        }
        throw std::runtime_error(sqlite3_errmsg(database_));
    }

    std::int64_t integer(int column) const {
        return sqlite3_column_int64(statement_, column);
    }

    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        return value == nullptr ? std::string{}
                                : reinterpret_cast<const char*>(value);
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

inline int validate_schema(sqlite3* database) {
    ReadStatement statement(database,
                            "SELECT value FROM metadata WHERE key='schema_version'");
    if (!statement.next()) {
        throw std::runtime_error("not a WhyRun capsule: schema_version is missing");
    }
    const std::string version = statement.text(0);
    if (version != "1" && version != "2") {
        throw std::runtime_error("unsupported WhyRun capsule schema version " +
                                 version);
    }
    return version == "1" ? 1 : 2;
}

}  // namespace whyrun::detail

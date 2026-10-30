#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <util/sqlite.h>

namespace util::sqlite {

const value& row::at(const std::string& name) const {
    auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) {
        throw std::out_of_range(fmt::format("no column '{}'", name));
    }
    return values[std::distance(names.begin(), it)];
}

std::string row::text(const std::string& name) const {
    auto& v = at(name);
    if (auto s = std::get_if<std::string>(&v)) {
        return *s;
    }
    if (auto i = std::get_if<std::int64_t>(&v)) {
        return std::to_string(*i);
    }
    return {};
}

std::optional<std::string> row::opt_text(const std::string& name) const {
    auto& v = at(name);
    if (std::holds_alternative<std::monostate>(v)) {
        return std::nullopt;
    }
    return text(name);
}

std::int64_t row::integer(const std::string& name) const {
    auto& v = at(name);
    if (auto i = std::get_if<std::int64_t>(&v)) {
        return *i;
    }
    if (auto s = std::get_if<std::string>(&v)) {
        return std::stoll(*s);
    }
    return 0;
}

util::expected<database, std::string>
database::open(const std::filesystem::path& path, open_mode mode) {
    sqlite3* db = nullptr;
    int flags = mode == open_mode::readonly    ? SQLITE_OPEN_READONLY
                : mode == open_mode::readwrite ? SQLITE_OPEN_READWRITE
                                               : (SQLITE_OPEN_READWRITE |
                                                  SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        sqlite3_close(db);
        return util::unexpected{
            fmt::format("unable to open database {}: {}", path.string(), msg)};
    }
    sqlite3_busy_timeout(db, 30000);
    bool ro = sqlite3_db_readonly(db, "main") == 1;
    database result{std::shared_ptr<sqlite3>(db, sqlite3_close), ro};
    if (!ro) {
        (void)result.exec("PRAGMA foreign_keys = ON");
    }
    return result;
}

util::expected<database, std::string> database::open_memory() {
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        sqlite3_close(db);
        return util::unexpected{std::string{"unable to open memory database"}};
    }
    database result{std::shared_ptr<sqlite3>(db, sqlite3_close), false};
    (void)result.exec("PRAGMA foreign_keys = ON");
    return result;
}

util::expected<void, std::string> database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &err) !=
        SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        return util::unexpected{fmt::format("sqlite: {} ({})", msg, sql)};
    }
    return {};
}

util::expected<std::vector<row>, std::string>
database::query(const std::string& sql, const std::vector<value>& params) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw, nullptr) !=
        SQLITE_OK) {
        return util::unexpected{fmt::format(
            "sqlite: unable to prepare '{}': {}", sql, sqlite3_errmsg(db_.get()))};
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
        raw, sqlite3_finalize);
    for (std::size_t i = 0; i < params.size(); ++i) {
        int idx = int(i) + 1;
        int rc = SQLITE_OK;
        if (auto s = std::get_if<std::string>(&params[i])) {
            rc = sqlite3_bind_text(raw, idx, s->c_str(), int(s->size()),
                                   SQLITE_TRANSIENT);
        } else if (auto n = std::get_if<std::int64_t>(&params[i])) {
            rc = sqlite3_bind_int64(raw, idx, *n);
        } else {
            rc = sqlite3_bind_null(raw, idx);
        }
        if (rc != SQLITE_OK) {
            return util::unexpected{fmt::format("sqlite: bind failed: {}",
                                                sqlite3_errmsg(db_.get()))};
        }
    }
    std::vector<row> rows;
    std::vector<std::string> names;
    int ncol = sqlite3_column_count(raw);
    for (int c = 0; c < ncol; ++c) {
        names.push_back(sqlite3_column_name(raw, c));
    }
    while (true) {
        int rc = sqlite3_step(raw);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return util::unexpected{fmt::format("sqlite: '{}': {}", sql,
                                                sqlite3_errmsg(db_.get()))};
        }
        row r{names, {}};
        for (int c = 0; c < ncol; ++c) {
            switch (sqlite3_column_type(raw, c)) {
            case SQLITE_INTEGER:
                r.values.push_back(std::int64_t(sqlite3_column_int64(raw, c)));
                break;
            case SQLITE_NULL:
                r.values.push_back(std::monostate{});
                break;
            default:
                r.values.push_back(std::string(
                    reinterpret_cast<const char*>(sqlite3_column_text(raw, c)),
                    sqlite3_column_bytes(raw, c)));
            }
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

util::expected<std::int64_t, std::string> database::user_version() {
    auto r = query("PRAGMA user_version");
    if (!r) {
        return util::unexpected{r.error()};
    }
    return r->empty() ? 0 : r->front().integer("user_version");
}

util::expected<void, std::string> database::set_user_version(std::int64_t v) {
    return exec(fmt::format("PRAGMA user_version = {}", v));
}

} // namespace util::sqlite

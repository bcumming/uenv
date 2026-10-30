#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <util/expected.h>

struct sqlite3;
struct sqlite3_stmt;

namespace util::sqlite {

// a value bound to a parameter or read from a column
using value = std::variant<std::monostate, std::int64_t, std::string>;

struct row {
    std::vector<std::string> names;
    std::vector<value> values;

    // the value of a column, by name: throws std::out_of_range if absent
    const value& at(const std::string& name) const;
    std::string text(const std::string& name) const;
    std::optional<std::string> opt_text(const std::string& name) const;
    std::int64_t integer(const std::string& name) const;
};

enum class open_mode { readonly, readwrite, create };

// A small SQLite wrapper. All user data goes through bound parameters.
class database {
  public:
    static util::expected<database, std::string>
    open(const std::filesystem::path& path, open_mode mode);
    static util::expected<database, std::string> open_memory();

    // run one or more statements with no parameters and no result
    util::expected<void, std::string> exec(const std::string& sql);

    // run one statement with bound parameters (1-based, in order)
    util::expected<std::vector<row>, std::string>
    query(const std::string& sql, const std::vector<value>& params = {});

    util::expected<std::int64_t, std::string> user_version();
    util::expected<void, std::string> set_user_version(std::int64_t v);

    // BEGIN IMMEDIATE ... COMMIT: rolled back if f returns an error
    template <typename F>
    util::expected<void, std::string> transaction(F&& f) {
        if (auto r = exec("BEGIN IMMEDIATE"); !r) {
            return r;
        }
        util::expected<void, std::string> result = f();
        if (!result) {
            (void)exec("ROLLBACK");
            return result;
        }
        return exec("COMMIT");
    }

    bool readonly() const {
        return readonly_;
    }

  private:
    database(std::shared_ptr<sqlite3> db, bool ro)
        : db_(std::move(db)), readonly_(ro) {
    }
    std::shared_ptr<sqlite3> db_;
    bool readonly_ = false;
};

} // namespace util::sqlite

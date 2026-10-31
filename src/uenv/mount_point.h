#pragma once

#include <compare>
#include <filesystem>
#include <string>
#include <string_view>

#include <util/expected.h>

namespace uenv {

// A mount point: an absolute path in the form that UENV_MOUNT_LIST can hold
// (design/1-composition.md, "Mount points"). The only way to make one is
// parse(), so each mount_point in uenv is valid, wherever it came from: a
// layer config, an index annotation, a compose file, a v1 env.json or the
// command line.
class mount_point {
  public:
    // the characters and limits of uenv::parse_path, which the setuid helper
    // uses for UENV_MOUNT_LIST: [A-Za-z0-9_./-], at most PATH_MAX characters
    // and NAME_MAX for each component. Also: absolute, and no empty, "." or
    // ".." component (thus no trailing '/').
    static util::expected<mount_point, std::string> parse(std::string_view text);

    const std::filesystem::path& path() const {
        return path_;
    }
    const std::string& string() const {
        return path_.native();
    }

    // true if this mount point is inside `parent`, for example
    // /user-environment/tools inside /user-environment
    bool is_inside(const mount_point& parent) const;
    // the path of this mount point relative to `parent` (is_inside(parent))
    std::string relative_to(const mount_point& parent) const;

    // byte order of the text: the order in which uenv sorts the layers
    friend std::strong_ordering operator<=>(const mount_point& a,
                                            const mount_point& b) {
        return a.string() <=> b.string();
    }
    friend bool operator==(const mount_point& a, const mount_point& b) {
        return a.string() == b.string();
    }

  private:
    explicit mount_point(std::filesystem::path p) : path_(std::move(p)) {
    }
    std::filesystem::path path_;
};

inline std::string format_as(const mount_point& m) {
    return m.string();
}

} // namespace uenv

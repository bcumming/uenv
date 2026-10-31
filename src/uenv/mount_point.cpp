#include <string>
#include <string_view>

#include <fmt/core.h>

#include <uenv/mount_point.h>
#include <uenv/parse.h>
#include <util/expected.h>
#include <util/strings.h>

namespace uenv {

util::expected<mount_point, std::string>
mount_point::parse(std::string_view text) {
    const std::string s(text);
    auto p = parse_path(s);
    if (!p) {
        return util::unexpected{fmt::format("the mount point '{}' is not valid: {}",
                                            s, p.error().message())};
    }
    if (p->empty() || p->front() != '/') {
        return util::unexpected{
            fmt::format("the mount point '{}' is not an absolute path", s)};
    }
    if (*p != "/") {
        // the first component is empty: the text starts with '/'
        auto parts = util::split(p->substr(1), '/');
        for (auto& c : parts) {
            if (c.empty() || c == "." || c == "..") {
                return util::unexpected{fmt::format(
                    "the mount point '{}' has an empty, '.' or '..' "
                    "component",
                    s)};
            }
        }
    }
    return mount_point(std::filesystem::path(*p));
}

bool mount_point::is_inside(const mount_point& parent) const {
    const auto& c = string();
    const auto& p = parent.string();
    if (p == "/") {
        return c != "/";
    }
    return c.size() > p.size() && c.starts_with(p) && c[p.size()] == '/';
}

std::string mount_point::relative_to(const mount_point& parent) const {
    const auto& p = parent.string();
    return string().substr(p == "/" ? 1 : p.size() + 1);
}

} // namespace uenv

#include <string>
#include <string_view>

#include <fmt/core.h>

#include <uenv/name.h>
#include <uenv/parse.h>
#include <util/expected.h>

namespace uenv {

util::expected<name, std::string> name::parse(std::string_view text) {
    const std::string s(text);
    auto n = parse_name(s);
    if (!n) {
        return util::unexpected{fmt::format(
            "'{}' is not a valid name: a name starts with a letter, a digit "
            "or '_', and contains only letters, digits, '_', '.' and '-'",
            s)};
    }
    return name(std::move(*n));
}

} // namespace uenv

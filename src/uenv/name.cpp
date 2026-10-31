#include <cstdlib>
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

namespace names {

namespace {
// a constant of the code: the literal is a name
const name& constant(const char* text) {
    auto n = name::parse(text);
    if (!n) {
        // a constant in the code that is not a name: a bug, found by the tests
        std::abort();
    }
    return *new name(std::move(*n));
}
} // namespace

const name& system() {
    static const name& n = constant("system");
    return n;
}
const name& uarch() {
    static const name& n = constant("uarch");
    return n;
}
const name& runtime_merge() {
    static const name& n = constant("runtime-merge");
    return n;
}
const name& unnamed() {
    static const name& n = constant("unnamed");
    return n;
}

} // namespace names

} // namespace uenv

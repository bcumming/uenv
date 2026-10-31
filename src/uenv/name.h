#pragma once

#include <compare>
#include <string>
#include <string_view>

#include <util/expected.h>

namespace uenv {

// A name: the grammar of a name in a v1 label, [A-Za-z0-9_][A-Za-z0-9_.-]*.
// It is the type of the name, version and release of a config, of an alias,
// of a view name, of a meta key and of a registry name
// (design/2-oci-layout.md, "The configs"). The only way to make one is
// parse(), so each name in uenv is valid, wherever it came from.
class name {
  public:
    static util::expected<name, std::string> parse(std::string_view text);

    const std::string& string() const {
        return value_;
    }

    friend std::strong_ordering operator<=>(const name&, const name&) = default;
    friend bool operator==(const name&, const name&) = default;

  private:
    explicit name(std::string s) : value_(std::move(s)) {
    }
    std::string value_;
};

inline const std::string& format_as(const name& n) {
    return n.string();
}

// the names that the code uses as constants: each one is parsed once, here
namespace names {
const name& system();
const name& uarch();
const name& runtime_merge();
const name& unnamed();
} // namespace names

} // namespace uenv

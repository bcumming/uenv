#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <util/expected.h>
#include <util/jcs.h>

namespace util {

namespace {

constexpr std::int64_t max_safe_integer = (std::int64_t(1) << 53);

// decode UTF-8 to UTF-16 code units, for the key order of RFC 8785 3.2.3.
// nlohmann::json validates UTF-8 on parse; invalid input is passed byte for
// byte, which gives a stable (if arbitrary) order.
std::u16string to_utf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        std::uint32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xf0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3fu) << 12) |
                 ((s[i + 2] & 0x3fu) << 6) | (s[i + 3] & 0x3fu);
            n = 4;
        } else if (c >= 0xe0 && i + 2 < s.size()) {
            cp = ((c & 0x0fu) << 12) | ((s[i + 1] & 0x3fu) << 6) |
                 (s[i + 2] & 0x3fu);
            n = 3;
        } else if (c >= 0xc0 && i + 1 < s.size()) {
            cp = ((c & 0x1fu) << 6) | (s[i + 1] & 0x3fu);
            n = 2;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(char16_t(0xd800 + (cp >> 10)));
            out.push_back(char16_t(0xdc00 + (cp & 0x3ff)));
        } else {
            out.push_back(char16_t(cp));
        }
        i += n;
    }
    return out;
}

void write_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += fmt::format("\\u{:04x}", c);
            } else {
                out.push_back(char(c));
            }
        }
    }
    out.push_back('"');
}

util::expected<void, std::string> write(std::string& out,
                                        const nlohmann::json& v) {
    using t = nlohmann::json::value_t;
    switch (v.type()) {
    case t::null:
        out += "null";
        return {};
    case t::boolean:
        out += v.get<bool>() ? "true" : "false";
        return {};
    case t::number_integer: {
        auto x = v.get<std::int64_t>();
        if (x > max_safe_integer || x < -max_safe_integer) {
            return util::unexpected{
                fmt::format("jcs: integer {} is out of range", x)};
        }
        out += std::to_string(x);
        return {};
    }
    case t::number_unsigned: {
        auto x = v.get<std::uint64_t>();
        if (x > std::uint64_t(max_safe_integer)) {
            return util::unexpected{
                fmt::format("jcs: integer {} is out of range", x)};
        }
        out += std::to_string(x);
        return {};
    }
    case t::number_float: {
        // uenv writes no floats; accept only integral values.
        auto x = v.get<double>();
        if (!std::isfinite(x) || std::floor(x) != x ||
            std::fabs(x) > double(max_safe_integer)) {
            return util::unexpected{fmt::format(
                "jcs: the number {} is not supported (integers only)", x)};
        }
        out += std::to_string(std::int64_t(x));
        return {};
    }
    case t::string:
        write_string(out, v.get_ref<const std::string&>());
        return {};
    case t::array: {
        out.push_back('[');
        bool first = true;
        for (auto& e : v) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            if (auto r = write(out, e); !r) {
                return r;
            }
        }
        out.push_back(']');
        return {};
    }
    case t::object: {
        std::vector<std::pair<std::u16string, const std::string*>> keys;
        for (auto it = v.begin(); it != v.end(); ++it) {
            keys.push_back({to_utf16(it.key()), &it.key()});
        }
        std::sort(keys.begin(), keys.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
        out.push_back('{');
        bool first = true;
        for (auto& [_, k] : keys) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            write_string(out, *k);
            out.push_back(':');
            if (auto r = write(out, v.at(*k)); !r) {
                return r;
            }
        }
        out.push_back('}');
        return {};
    }
    default:
        return util::unexpected{std::string{"jcs: unsupported JSON value"}};
    }
}

} // namespace

util::expected<std::string, std::string> jcs(const nlohmann::json& value) {
    std::string out;
    if (auto r = write(out, value); !r) {
        return util::unexpected{r.error()};
    }
    return out;
}

} // namespace util

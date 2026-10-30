#pragma once

#include <compare>
#include <optional>
#include <string>

#include <util/envvars.h>

namespace uenv {

/// The result of parsing a uenv description from the command line.
struct view_description {
    /// name of the uenv that provides the view -
    /// optional because view descriptions are in one of two forms:
    /// - view_name
    /// - uenv_name:view_name
    std::optional<std::string> uenv;
    /// name of the view
    std::string name;
};

/// A fully qualified uenv view
struct qualified_view_description {
    /// name of the uenv that provides the view
    std::string uenv;
    /// name of the view
    std::string name;
};

/// A fully qualified uenv view
struct env_view_description {
    /// the mount point of the uenv that provides the view
    std::string mount;
    /// name of the uenv that provides the view
    std::string uenv;
    /// name of the view
    std::string name;
};

/// A reference to a layer view of an image: the alias of the layer, and the
/// name of the view in that layer. The stored form is "alias:view".
struct layer_view_ref {
    std::string alias;
    std::string view;
    auto operator<=>(const layer_view_ref&) const = default;
};

/// The name of an image view. A stored image has names with no item. A
/// runtime merge keeps the image views of each item of the list: `item` is
/// the alias of the item that gave the view.
struct image_view_name {
    std::optional<std::string> item;
    std::string name;
    auto operator<=>(const image_view_name&) const = default;
};

std::string format_as(const layer_view_ref& r);
std::string format_as(const image_view_name& n);

struct concrete_view {
    /// the name of the view
    std::string name;
    /// the string description of the view: loaded from env.json meta data
    std::string description;
    /// the environment variable updates to be applied when the view is loaded:
    /// loaded from env.json meta data
    envvars::patch environment;
};

} // namespace uenv

#include <fmt/core.h>
template <> class fmt::formatter<uenv::view_description> {
  public:
    // parse format specification and store it:
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }
    // format a value using stored specification:
    template <typename FmtContext>
    constexpr auto format(uenv::view_description const& d,
                          FmtContext& ctx) const {
        return fmt::format_to(ctx.out(), "(name={}, uenv={})", d.name,
                              (d.uenv ? *d.uenv : "none"));
    }
};

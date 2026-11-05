// `uenv image inspect` and `uenv inspect`: show an image, a layer, or a
// list of them.

#include <algorithm>
#include <string>
#include <vector>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/session.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using json = nlohmann::json;
using argparse::completion;

namespace {

struct inspect_args {
    std::string uenv;
    bool json = false;
};

int image_inspect(const inspect_args& args, const global_settings& settings) {
    auto stores = open_stores(settings.config);
    if (!stores) {
        term::error("{}", stores.error());
        return 1;
    }
    auto items = parse_item_list(args.uenv, keys_of(settings.config),
                                 settings.config.system_name);
    if (!items) {
        term::error("{}", items.error());
        return 1;
    }
    auto resolved = resolve_items(*stores, *items, false);
    if (!resolved) {
        term::error("{}", resolved.error());
        return 1;
    }
    auto img = &resolved->image;
    json layers = json::array();
    for (auto& l : img->layers()) {
        // the same shape as the image views: name -> {description, env}
        json views = json::object();
        for (auto& [k, v] : l.layer.views) {
            views[k.string()] = {{"description", v.description},
                                 {"env", format::env_to_json(v.env)}};
        }
        layers.push_back(
            {{"alias", l.alias.string()},
             {"mount", l.mount.string()},
             {"build_mount", l.layer.mount.string()},
             {"relocatable", l.layer.relocatable},
             {"label", label_string(l.layer.config)},
             {"meta", cli::meta_json(l.layer.config.meta)},
             {"digest", l.layer.digest() ? l.layer.digest()->string() : ""},
             {"id",
              l.layer.digest() ? format::short_id(*l.layer.digest()) : ""},
             {"squashfs",
              l.layer.store.file ? l.layer.store.file->string() : ""},
             {"views", views}});
    }
    json views = json::object();
    for (auto& [k, v] : img->views()) {
        json inc = json::array();
        for (auto& r : v.include) {
            inc.push_back(format_as(r));
        }
        views[format_as(k)] = {{"description", v.description},
                               {"include", inc},
                               {"env", format::env_to_json(v.env)}};
    }
    std::vector<std::string> defaults;
    for (auto& d : img->default_views()) {
        defaults.push_back(format_as(d));
    }
    const std::string kind = img->kind() == uenv::image_kind::layer ? "layer"
                             : img->kind() == uenv::image_kind::runtime_merge
                                 ? "runtime merge"
                                 : "image";
    json j{{"kind", kind},
           {"label", label_string(img->config())},
           {"description", img->config().description},
           {"meta", cli::meta_json(img->config().meta)},
           {"digest", img->digest() ? img->digest()->string() : ""},
           {"layers", layers},
           {"views", views},
           {"default_views", defaults}};
    if (args.json) {
        term::msg("{}", j.dump(2));
        return 0;
    }
    // the colors of v1: a field in blue, a name in yellow, a value in cyan.
    // The views first: they are what a user loads.
    term::msg("{} {}", color::blue(kind),
              color::yellow(label_string(img->config())));
    if (!img->config().description.empty()) {
        term::msg("  {}", img->config().description);
    }
    if (img->views().empty()) {
        term::msg("  {} none", color::blue("views:"));
    } else {
        term::msg("  {}", color::blue("views:"));
        for (auto& [k, v] : img->views()) {
            const bool is_default = std::find(img->default_views().begin(),
                                              img->default_views().end(),
                                              k) != img->default_views().end();
            term::msg("    {}{}{}", color::yellow(format_as(k)),
                      is_default ? color::red(" (default)") : std::string{},
                      v.description.empty() ? std::string{}
                                            : ": " + v.description);
        }
    }
    term::msg("  {} {}", color::blue("digest:"),
              color::cyan(j["digest"].get<std::string>()));
    if (img->config().meta.empty()) {
        term::msg("  {} none", color::blue("meta:"));
    } else {
        term::msg("  {}", color::blue("meta:"));
        for (auto& [k, v] : img->config().meta) {
            term::msg("    {}: {}", color::yellow(k.string()), color::cyan(v));
        }
    }
    // each layer by its alias, the name that views and compose files use,
    // then its id: a squashfs file that is started directly has no id
    std::size_t wa = 0;
    std::size_t wl = 0;
    for (auto& l : img->layers()) {
        wa = std::max(wa, l.alias.string().size());
        wl = std::max(wl, label_string(l.layer.config).size());
    }
    term::msg("  {}", color::blue("layers:"));
    for (auto& l : img->layers()) {
        const auto d = l.layer.digest();
        const std::string id = d ? format::short_id(*d) : std::string(16, '-');
        term::msg("    {}  {}  {}  {}{}",
                  color::yellow(fmt::format("{:<{}}", l.alias.string(), wa)),
                  id, fmt::format("{:<{}}", label_string(l.layer.config), wl),
                  color::cyan(l.mount.string()),
                  l.layer.relocatable ? " (relocatable)" : "");
    }
    return 0;
}

} // namespace

argparse::command image_inspect_command(const global_settings& settings) {
    argparse::command_builder<inspect_args> c(
        "inspect", "show an image, a layer, or a list of them");
    c.add_positional("uenv", &inspect_args::uenv, "a label, list or file")
        .required()
        .complete(completion::custom("uenv_list"));
    c.add_flag("json", &inspect_args::json, "print JSON");
    c.action([&settings](const inspect_args& a) {
        return image_inspect(a, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

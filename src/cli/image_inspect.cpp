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
    auto image = &resolved->image;
    json layers = json::array();
    for (auto& entry : image->layers()) {
        // the same shape as the image views: name -> {description, env}
        json views = json::object();
        for (auto& [name, view] : entry.layer.views) {
            views[name.string()] = {{"description", view.description},
                                    {"env", format::env_to_json(view.env)}};
        }
        layers.push_back(
            {{"alias", entry.alias.string()},
             {"mount", entry.mount.string()},
             {"build_mount", entry.layer.mount.string()},
             {"relocatable", entry.layer.relocatable},
             {"label", label_string(entry.layer.config)},
             {"meta", cli::meta_json(entry.layer.config.meta)},
             {"digest",
              entry.layer.digest() ? entry.layer.digest()->string() : ""},
             {"id", entry.layer.digest()
                        ? format::short_id(*entry.layer.digest())
                        : ""},
             {"squashfs",
              entry.layer.store.file ? entry.layer.store.file->string() : ""},
             {"views", views}});
    }
    json views = json::object();
    for (auto& [name, view] : image->views()) {
        json include = json::array();
        for (auto& ref : view.include) {
            include.push_back(format_as(ref));
        }
        views[format_as(name)] = {{"description", view.description},
                                  {"include", include},
                                  {"env", format::env_to_json(view.env)}};
    }
    std::vector<std::string> defaults;
    for (auto& name : image->default_views()) {
        defaults.push_back(format_as(name));
    }
    const std::string kind = image->kind() == uenv::image_kind::layer ? "layer"
                             : image->kind() == uenv::image_kind::runtime_merge
                                 ? "runtime merge"
                                 : "image";
    json out{{"kind", kind},
             {"label", label_string(image->config())},
             {"description", image->config().description},
             {"meta", cli::meta_json(image->config().meta)},
             {"digest", image->digest() ? image->digest()->string() : ""},
             {"layers", layers},
             {"views", views},
             {"default_views", defaults}};
    if (args.json) {
        term::msg("{}", out.dump(2));
        return 0;
    }
    // the colors of v1: a field in blue, a name in yellow, a value in cyan.
    // The views first: they are what a user loads.
    term::msg("{} {}", color::blue(kind),
              color::yellow(label_string(image->config())));
    if (!image->config().description.empty()) {
        term::msg("  {}", image->config().description);
    }
    if (image->views().empty()) {
        term::msg("  {} none", color::blue("views:"));
    } else {
        term::msg("  {}", color::blue("views:"));
        for (auto& [name, view] : image->views()) {
            const bool is_default =
                std::find(image->default_views().begin(),
                          image->default_views().end(),
                          name) != image->default_views().end();
            term::msg("    {}{}{}", color::yellow(format_as(name)),
                      is_default ? color::red(" (default)") : std::string{},
                      view.description.empty() ? std::string{}
                                               : ": " + view.description);
        }
    }
    term::msg("  {} {}", color::blue("digest:"),
              color::cyan(out["digest"].get<std::string>()));
    if (image->config().meta.empty()) {
        term::msg("  {} none", color::blue("meta:"));
    } else {
        term::msg("  {}", color::blue("meta:"));
        for (auto& [key, value] : image->config().meta) {
            term::msg("    {}: {}", color::yellow(key.string()),
                      color::cyan(value));
        }
    }
    // each layer by its alias, the name that views and compose files use,
    // then its id: a squashfs file that is started directly has no id
    std::size_t alias_width = 0;
    std::size_t label_width = 0;
    for (auto& entry : image->layers()) {
        alias_width = std::max(alias_width, entry.alias.string().size());
        label_width =
            std::max(label_width, label_string(entry.layer.config).size());
    }
    term::msg("  {}", color::blue("layers:"));
    for (auto& entry : image->layers()) {
        const auto digest = entry.layer.digest();
        const std::string id =
            digest ? format::short_id(*digest) : std::string(16, '-');
        term::msg("    {}  {}  {}  {}{}",
                  color::yellow(
                      fmt::format("{:<{}}", entry.alias.string(), alias_width)),
                  id,
                  fmt::format("{:<{}}", label_string(entry.layer.config),
                              label_width),
                  color::cyan(entry.mount.string()),
                  entry.layer.relocatable ? " (relocatable)" : "");
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
    c.action([&settings](const inspect_args& args) {
        return image_inspect(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

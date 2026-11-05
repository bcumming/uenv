// `uenv image copy`: copy an image in a registry, or to a different
// registry.

#include <optional>
#include <string>
#include <vector>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/parse.h>
#include <uenv/registry.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct copy_args {
    std::string source;
    std::string destination;
    std::optional<std::string> set;
    std::optional<std::string> registry;
};

int image_copy(const copy_args& args, const global_settings& settings) {
    auto src =
        registry::parse_address(args.source, settings.config, args.registry);
    if (!src) {
        term::error("{}", src.error());
        return 1;
    }
    auto dst = registry::parse_address(args.destination, settings.config,
                                       args.registry);
    if (!dst) {
        term::error("{}", dst.error());
        return 1;
    }
    std::vector<config_change> changes;
    if (args.set) {
        auto c = parse_config_changes(*args.set);
        if (!c) {
            term::error("{}", c.error());
            return 1;
        }
        changes = std::move(*c);
    }
    auto ctx = cli::make_context(settings);
    auto r = registry::copy(*src, dst->where, ctx, changes);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("copied to {}:{} digest={} ({} referrers)", r->repository, r->tag,
              r->digest.string(), ctx.st.referrers_copied);
    return 0;
}

} // namespace

argparse::command image_copy_command(const global_settings& settings) {
    argparse::command_builder<copy_args> c("copy",
                                           "copy an image in a registry");
    c.add_positional("source", &copy_args::source, "the image")
        .required()
        .complete(completion::none());
    c.add_positional("destination", &copy_args::destination, "reg[/ns]::")
        .required()
        .complete(completion::none());
    c.add_option("set", &copy_args::set,
                 "changes: release=v2,meta.system=santis")
        .complete(completion::none());
    c.add_option("registry", &copy_args::registry, "a registry")
        .complete(completion::none());
    c.action(
        [&settings](const copy_args& a) { return image_copy(a, settings); });
    return std::move(c).build();
}

} // namespace uenv

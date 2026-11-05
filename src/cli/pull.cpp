// `uenv image pull`: pull an image or a layer into the user repository.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/registry.h>
#include <uenv/session.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct pull_args {
    std::string address;
    std::optional<std::string> registry;
};

int image_pull(const pull_args& args, const global_settings& settings) {
    auto s = open_user_store(settings.config);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto a =
        registry::parse_address(args.address, settings.config, args.registry);
    if (!a) {
        term::error("{}", a.error());
        return 1;
    }
    auto ctx = cli::make_context(settings);
    auto r = registry::pull(*s, *a, ctx);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("pulled {} id={} ({} squashfs downloaded, {} already present)",
              label_string(r->cfg), r->id, ctx.st.downloaded, ctx.st.skipped);
    return 0;
}

} // namespace

argparse::command image_pull_command(const global_settings& settings) {
    argparse::command_builder<pull_args> c("pull", "pull an image or a layer");
    c.add_positional("address", &pull_args::address, "the image")
        .required()
        .complete(completion::none());
    c.add_option("registry", &pull_args::registry, "a registry")
        .complete(completion::none());
    c.action(
        [&settings](const pull_args& a) { return image_pull(a, settings); });
    return std::move(c).build();
}

} // namespace uenv

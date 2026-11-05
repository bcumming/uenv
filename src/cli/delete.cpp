// `uenv image delete`: delete an image or a building block in a
// registry.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/registry.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct delete_args {
    std::string address;
    std::optional<std::string> registry;
};

int image_delete(const delete_args& args, const global_settings& settings) {
    auto a =
        registry::parse_address(args.address, settings.config, args.registry);
    if (!a) {
        term::error("{}", a.error());
        return 1;
    }
    auto ctx = cli::make_context(settings);
    auto r = registry::remove(*a, ctx);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("deleted {}@{}", r->repository, r->digest.string());
    return 0;
}

} // namespace

argparse::command image_delete_command(const global_settings& settings) {
    argparse::command_builder<delete_args> c(
        "delete", "delete an image or a building block in a registry");
    c.add_positional("address", &delete_args::address, "the image")
        .required()
        .complete(completion::none());
    c.add_option("registry", &delete_args::registry, "a registry")
        .complete(completion::none());
    c.action([&settings](const delete_args& a) {
        return image_delete(a, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

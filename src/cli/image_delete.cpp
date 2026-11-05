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
    auto address =
        registry::parse_address(args.address, settings.config, args.registry);
    if (!address) {
        term::error("{}", address.error());
        return 1;
    }
    auto context = cli::make_context(settings);
    auto result = registry::remove(*address, context);
    if (!result) {
        term::error("{}", result.error());
        return 1;
    }
    term::msg("deleted {}@{}", result->repository, result->digest.string());
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
    c.action([&settings](const delete_args& args) {
        return image_delete(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

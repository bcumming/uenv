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
    auto store = open_user_store(settings.config);
    if (!store) {
        term::error("{}", store.error());
        return 1;
    }
    auto address =
        registry::parse_address(args.address, settings.config, args.registry);
    if (!address) {
        term::error("{}", address.error());
        return 1;
    }
    auto context = cli::make_context(settings);
    auto record = registry::pull(*store, *address, context);
    if (!record) {
        term::error("{}", record.error());
        return 1;
    }
    term::msg("pulled {} id={} ({} squashfs downloaded, {} already present)",
              label_string(record->cfg), record->id, context.st.downloaded,
              context.st.skipped);
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
    c.action([&settings](const pull_args& args) {
        return image_pull(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

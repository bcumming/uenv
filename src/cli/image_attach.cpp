// `uenv image attach`: attach a referrer of a kind that the site
// defines.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include <uenv/parse.h>
#include <uenv/registry.h>
#include <util/fs.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct attach_args {
    std::string address;
    std::string kind;
    std::string content;
    std::optional<std::string> registry;
};

int image_attach(const attach_args& args, const global_settings& settings) {
    auto address =
        registry::parse_address(args.address, settings.config, args.registry);
    if (!address) {
        term::error("{}", address.error());
        return 1;
    }
    auto context = cli::make_context(settings);
    // a path (it starts with '/' or '.') is a file to read, else the content
    std::string content = args.content;
    if (auto file = parse_file_path(args.content)) {
        auto body = util::read_file(*file);
        if (!body) {
            term::error("{}", body.error());
            return 1;
        }
        content = std::move(*body);
    }
    auto repositories = registry::attach(*address, args.kind, content, context);
    if (!repositories) {
        term::error("{}", repositories.error());
        return 1;
    }
    term::msg("attached {} in {}", args.kind, fmt::join(*repositories, ", "));
    return 0;
}

} // namespace

argparse::command image_attach_command(const global_settings& settings) {
    argparse::command_builder<attach_args> c(
        "attach", "attach a referrer of a kind that the site defines");
    c.add_positional("address", &attach_args::address, "the subject")
        .required()
        .complete(completion::none());
    c.add_positional("kind", &attach_args::kind, "the kind, e.g. uenv/sbom")
        .required()
        .complete(completion::none());
    c.add_positional("content", &attach_args::content, "a file or a string")
        .required()
        .complete(completion::path());
    c.add_option("registry", &attach_args::registry, "a registry")
        .complete(completion::none());
    c.action([&settings](const attach_args& args) {
        return image_attach(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

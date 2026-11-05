// `uenv image push`: push an image, a layer, or an OCI layout directory.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/registry.h>
#include <uenv/session.h>
#include <util/expected.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct push_args {
    std::string source;
    std::optional<std::string> destination;
    std::optional<std::string> registry;
};

int image_push(const push_args& args, const global_settings& settings) {
    // the destination: reg[/ns]:: or the default registry
    std::optional<registry::target> target;
    if (args.destination) {
        auto address = registry::parse_address(*args.destination,
                                               settings.config, args.registry);
        if (!address) {
            term::error("{}", address.error());
            return 1;
        }
        target = address->where;
    } else {
        auto resolved =
            registry::resolve_target(settings.config, args.registry);
        if (!resolved) {
            term::error("{}", resolved.error());
            return 1;
        }
        target = *resolved;
    }
    auto context = cli::make_context(settings);
    util::expected<registry::push_result, std::string> result =
        util::unexpected{std::string{}};
    // a path (it starts with '/' or '.') is an OCI layout directory, else a
    // label: the form of the text decides, not the files that exist
    if (auto dir = parse_file_path(args.source)) {
        result = registry::push_layout(*dir, *target, context);
    } else {
        auto stores = open_stores(settings.config);
        if (!stores) {
            term::error("{}", stores.error());
            return 1;
        }
        auto query = parse_label_query(args.source, keys_of(settings.config),
                                       settings.config.system_name);
        if (!query) {
            term::error("{}", query.error());
            return 1;
        }
        auto found = find_unique(*stores, *query, args.source);
        if (!found) {
            term::error("{}", found.error());
            return 1;
        }
        result = registry::push((*stores)[found->store_index],
                                found->record.digest, *target, context);
    }
    if (!result) {
        term::error("{}", result.error());
        return 1;
    }
    term::msg("pushed {}:{} ({} uploaded, {} mounted, {} present)",
              result->repository, result->tag, context.st.uploaded,
              context.st.mounted, context.st.skipped);
    return 0;
}

} // namespace

argparse::command image_push_command(const global_settings& settings) {
    argparse::command_builder<push_args> c(
        "push", "push an image or a layer, or an OCI layout directory");
    c.add_positional("source", &push_args::source,
                     "a label, an id, or a directory")
        .required()
        .complete(completion::path());
    c.add_positional("destination", &push_args::destination,
                     "reg[/ns]:: (default: the default registry)")
        .complete(completion::none());
    c.add_option("registry", &push_args::registry, "a registry")
        .complete(completion::none());
    c.action([&settings](const push_args& args) {
        return image_push(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

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
    std::optional<registry::target> t;
    if (args.destination) {
        auto a = registry::parse_address(*args.destination, settings.config,
                                         args.registry);
        if (!a) {
            term::error("{}", a.error());
            return 1;
        }
        t = a->where;
    } else {
        auto r = registry::resolve_target(settings.config, args.registry);
        if (!r) {
            term::error("{}", r.error());
            return 1;
        }
        t = *r;
    }
    auto ctx = cli::make_context(settings);
    util::expected<registry::push_result, std::string> r =
        util::unexpected{std::string{}};
    // a path (it starts with '/' or '.') is an OCI layout directory, else a
    // label: the form of the text decides, not the files that exist
    if (auto dir = parse_file_path(args.source)) {
        r = registry::push_layout(*dir, *t, ctx);
    } else {
        auto stores = open_stores(settings.config);
        if (!stores) {
            term::error("{}", stores.error());
            return 1;
        }
        auto q = parse_label_query(args.source, keys_of(settings.config),
                                   settings.config.system_name);
        if (!q) {
            term::error("{}", q.error());
            return 1;
        }
        auto f = find_unique(*stores, *q, args.source);
        if (!f) {
            term::error("{}", f.error());
            return 1;
        }
        r = registry::push((*stores)[f->store_index], f->record.digest, *t,
                           ctx);
    }
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("pushed {}:{} ({} uploaded, {} mounted, {} present)",
              r->repository, r->tag, ctx.st.uploaded, ctx.st.mounted,
              ctx.st.skipped);
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
    c.action(
        [&settings](const push_args& a) { return image_push(a, settings); });
    return std::move(c).build();
}

} // namespace uenv

// `uenv image find`: find images and layers in the catalogue of a
// registry.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <uenv/registry.h>
#include <uenv/store.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct find_args {
    std::optional<std::string> label;
    std::optional<std::string> registry;
    bool json = false;
    bool no_header = false;
};

int image_find(const find_args& args, const global_settings& settings) {
    auto address = registry::parse_address(args.label.value_or(""),
                                           settings.config, args.registry);
    if (!address) {
        term::error("{}", address.error());
        return 1;
    }
    auto context = cli::make_context(settings);
    auto catalogue = registry::catalogue::open(address->where, context);
    if (!catalogue) {
        term::error("{}", catalogue.error());
        return 1;
    }
    auto stats = catalogue->refresh();
    if (!stats) {
        term::error("{}", stats.error());
        return 1;
    }
    spdlog::info("catalogue refresh: {} events, {} requests", stats->events,
                 stats->requests);
    // the query of the label; with no label, each image of the system
    label_query query;
    if (address->query) {
        query = *address->query;
    } else if (settings.config.system_name) {
        query.meta[settings.config.label.at] = *settings.config.system_name;
    }
    auto records = catalogue->db().query(query);
    if (!records) {
        term::error("{}", records.error());
        return 1;
    }
    cli::print_records(*records, args.json, args.no_header,
                       cli::record_time::pushed);
    return 0;
}

} // namespace

argparse::command image_find_command(const global_settings& settings) {
    argparse::command_builder<find_args> c("find", "find images in a registry");
    c.add_positional("label", &find_args::label, "a label")
        .complete(completion::none());
    c.add_option("registry", &find_args::registry, "a registry")
        .complete(completion::none());
    c.add_flag("json", &find_args::json, "print JSON");
    c.add_flag("no-header", &find_args::no_header, "no header");
    c.action([&settings](const find_args& args) {
        return image_find(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

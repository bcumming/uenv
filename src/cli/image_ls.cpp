// `uenv image ls`: list the images, or the layers (+), of the local
// repositories.

#include <optional>
#include <string>
#include <vector>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/parse.h>
#include <uenv/session.h>
#include <uenv/store.h>

#include "image.h"
#include "image_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct ls_args {
    std::optional<std::string> label;
    bool json = false;
    bool no_header = false;
    // a bare +: also the layers that are only inside images
    bool all = false;
};

int image_ls(const ls_args& args, const global_settings& settings) {
    auto stores = open_stores(settings.config);
    if (!stores) {
        term::error("{}", stores.error());
        return 1;
    }
    std::string text = args.label.value_or("");
    label_query query;
    if (text == "+") {
        query.layer = true;
    } else if (!text.empty()) {
        auto parsed = parse_label_query(text, keys_of(settings.config),
                                        settings.config.system_name);
        if (!parsed) {
            term::error("{}", parsed.error());
            return 1;
        }
        query = *parsed;
    } else if (settings.config.system_name) {
        query.meta[settings.config.label.at] = *settings.config.system_name;
    }
    std::vector<object_record> out;
    for (auto& store : *stores) {
        auto records = store.query(query);
        if (!records) {
            term::error("{}", records.error());
            return 1;
        }
        for (auto& record : *records) {
            // a bare + lists the building blocks, or each layer with --all.
            // A label lists each layer that matches it (5-discovery.md,
            // "Images and layers")
            if (query.layer && text == "+" && !record.standalone && !args.all) {
                continue;
            }
            out.push_back(record);
        }
    }
    cli::print_records(out, args.json, args.no_header, cli::record_time::added);
    return 0;
}

} // namespace

argparse::command image_ls_command(const global_settings& settings) {
    argparse::command_builder<ls_args> c("ls", "list images (or layers: +)");
    c.add_positional("label", &ls_args::label, "a label, or + for layers")
        .complete(completion::none());
    c.add_flag("json", &ls_args::json, "print JSON");
    c.add_flag("no-header", &ls_args::no_header, "no header");
    c.add_flag("all", &ls_args::all,
               "with a bare +: also the layers that are only inside images");
    c.action(
        [&settings](const ls_args& args) { return image_ls(args, settings); });
    return std::move(c).build();
}

} // namespace uenv

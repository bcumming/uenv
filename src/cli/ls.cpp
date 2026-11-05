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
    label_query q;
    if (text == "+") {
        q.layer = true;
    } else if (!text.empty()) {
        auto p = parse_label_query(text, keys_of(settings.config),
                                   settings.config.system_name);
        if (!p) {
            term::error("{}", p.error());
            return 1;
        }
        q = *p;
    } else if (settings.config.system_name) {
        q.meta[settings.config.label.at] = *settings.config.system_name;
    }
    std::vector<object_record> out;
    for (auto& s : *stores) {
        auto r = s.query(q);
        if (!r) {
            term::error("{}", r.error());
            return 1;
        }
        for (auto& o : *r) {
            // a bare + lists the building blocks, or each layer with --all.
            // A label lists each layer that matches it (5-discovery.md,
            // "Images and layers")
            if (q.layer && text == "+" && !o.standalone && !args.all) {
                continue;
            }
            out.push_back(o);
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
    c.action([&settings](const ls_args& a) { return image_ls(a, settings); });
    return std::move(c).build();
}

} // namespace uenv

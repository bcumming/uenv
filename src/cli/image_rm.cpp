// `uenv image rm`: delete an image or a building block from the user
// repository.

#include <string>
#include <vector>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/ops.h>
#include <uenv/session.h>
#include <uenv/store.h>

#include "image.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct rm_args {
    std::string label;
};

int image_rm(const rm_args& args, const global_settings& settings) {
    auto store = open_user_store(settings.config);
    if (!store) {
        term::error("{}", store.error());
        return 1;
    }
    auto query = parse_label_query(args.label, keys_of(settings.config));
    if (!query) {
        term::error("{}", query.error());
        return 1;
    }
    std::vector<uenv::store> stores;
    stores.push_back(std::move(*store));
    auto found = find_unique(stores, *query, args.label);
    if (!found) {
        term::error("{}", found.error());
        return 1;
    }
    if (query->layer && !found->record.standalone) {
        term::error("the layer {} is part of an image: delete the image",
                    label_string(found->record.cfg));
        return 1;
    }
    auto deleted = stores[0].remove(found->record.digest);
    if (!deleted) {
        term::error("{}", deleted.error());
        return 1;
    }
    term::msg("removed {} ({} blobs deleted)", label_string(found->record.cfg),
              deleted->size());
    return 0;
}

} // namespace

argparse::command image_rm_command(const global_settings& settings) {
    argparse::command_builder<rm_args> c("rm", "delete an image or a layer");
    c.add_positional("label", &rm_args::label, "a label or an id")
        .required()
        .complete(completion::none());
    c.action(
        [&settings](const rm_args& args) { return image_rm(args, settings); });
    return std::move(c).build();
}

} // namespace uenv

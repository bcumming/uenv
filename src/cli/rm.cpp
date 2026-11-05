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
    auto s = open_user_store(settings.config);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto q = parse_label_query(args.label, keys_of(settings.config));
    if (!q) {
        term::error("{}", q.error());
        return 1;
    }
    std::vector<store> one;
    one.push_back(std::move(*s));
    auto f = find_unique(one, *q, args.label);
    if (!f) {
        term::error("{}", f.error());
        return 1;
    }
    if (q->layer && !f->record.standalone) {
        term::error("the layer {} is part of an image: delete the image",
                    label_string(f->record.cfg));
        return 1;
    }
    auto r = one[0].remove(f->record.digest);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("removed {} ({} blobs deleted)", label_string(f->record.cfg),
              r->size());
    return 0;
}

} // namespace

argparse::command image_rm_command(const global_settings& settings) {
    argparse::command_builder<rm_args> c("rm", "delete an image or a layer");
    c.add_positional("label", &rm_args::label, "a label or an id")
        .required()
        .complete(completion::none());
    c.action([&settings](const rm_args& a) { return image_rm(a, settings); });
    return std::move(c).build();
}

} // namespace uenv

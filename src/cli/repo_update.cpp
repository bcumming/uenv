// `uenv repo update`: delete the records with missing blobs, and prune
// the unused blobs.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>

#include <uenv/store.h>

#include "repo.h"
#include "repo_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct repo_update_args {
    std::optional<std::string> path;
    bool force = false;
};

int repo_update(const repo_update_args& args, const global_settings& settings) {
    auto p = cli::repo_path(args.path, settings);
    if (!p) {
        term::error("{}", p.error());
        return 1;
    }
    auto s = store::open(*p, false);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto r = s->update(args.force, false);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("deleted {} records with missing blobs, {} blobs ({} bytes), {} "
              ".part files",
              r->missing.size(), r->pruned.size(), r->released,
              r->parts.size());
    return 0;
}

} // namespace

argparse::command repo_update_command(const global_settings& settings) {
    argparse::command_builder<repo_update_args> c(
        "update", "delete records with missing blobs, prune unused blobs");
    c.add_positional("path", &repo_update_args::path, "the repository")
        .complete(completion::directory());
    c.add_flag("force", &repo_update_args::force,
               "no grace period: use only when nothing writes");
    c.action([&settings](const repo_update_args& a) {
        return repo_update(a, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

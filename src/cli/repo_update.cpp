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
    auto path = cli::repo_path(args.path, settings);
    if (!path) {
        term::error("{}", path.error());
        return 1;
    }
    auto store = uenv::store::open(*path, false);
    if (!store) {
        term::error("{}", store.error());
        return 1;
    }
    auto report = store->update(args.force, false);
    if (!report) {
        term::error("{}", report.error());
        return 1;
    }
    term::msg("deleted {} records with missing blobs, {} blobs ({} bytes), {} "
              ".part files",
              report->missing.size(), report->pruned.size(), report->released,
              report->parts.size());
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
    c.action([&settings](const repo_update_args& args) {
        return repo_update(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

// `uenv repo status`: check a repository, and report what
// `uenv repo update` would delete.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <uenv/migrate.h>
#include <uenv/store.h>

#include "repo.h"
#include "repo_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct repo_status_args {
    std::optional<std::string> path;
    bool json = false;
};

int repo_status(const repo_status_args& args, const global_settings& settings) {
    auto path = cli::repo_path(args.path, settings);
    if (!path) {
        term::error("{}", path.error());
        return 1;
    }
    // the v1 images that are not in the v2 part: no hash, no write
    unsigned pending = 0;
    if (has_v1_part(*path)) {
        if (auto dry_run = migrate_v1(*path, {.dry_run = true})) {
            pending = dry_run->pending;
        }
    }
    auto state = inspect_repo(*path);
    if (state == repo_layout::v1_only) {
        term::msg("{}: v1 format, {} images. Run `uenv repo migrate`.",
                  path->string(), pending);
        return 0;
    }
    auto store = uenv::store::open(*path, true);
    if (!store) {
        term::error("{}", store.error());
        return 1;
    }
    auto report = store->update(false, true);
    if (!report) {
        term::error("{}", report.error());
        return 1;
    }
    auto objects = store->all();
    if (args.json) {
        nlohmann::json out{{"path", path->string()},
                           {"objects", objects ? objects->size() : 0},
                           {"missing", report->missing.size()},
                           {"prune", report->pruned.size()},
                           {"recent", report->kept_recent.size()},
                           {"parts", report->parts.size()},
                           {"released", report->released},
                           {"v1_not_migrated", pending}};
        term::msg("{}", out.dump());
        return 0;
    }
    term::msg("{}: {} objects", path->string(), objects ? objects->size() : 0);
    for (auto& record : report->missing) {
        term::msg("  missing blobs: {} id={}", label_string(record.cfg),
                  record.id);
    }
    term::msg("  `uenv repo update` would delete {} blobs ({} bytes) and {} "
              ".part files; {} unused blobs are newer than 24 hours",
              report->pruned.size(), report->released, report->parts.size(),
              report->kept_recent.size());
    if (pending) {
        term::msg("  {} v1 images are not migrated: run `uenv repo migrate`",
                  pending);
    }
    return 0;
}

} // namespace

argparse::command repo_status_command(const global_settings& settings) {
    argparse::command_builder<repo_status_args> c("status",
                                                  "check a repository");
    c.add_positional("path", &repo_status_args::path, "the repository")
        .complete(completion::directory());
    c.add_flag("json", &repo_status_args::json, "print JSON");
    c.action([&settings](const repo_status_args& args) {
        return repo_status(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

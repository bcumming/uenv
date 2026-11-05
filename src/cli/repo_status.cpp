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
    auto p = cli::repo_path(args.path, settings);
    if (!p) {
        term::error("{}", p.error());
        return 1;
    }
    // the v1 images that are not in the v2 part: no hash, no write
    unsigned pending = 0;
    if (has_v1_part(*p)) {
        if (auto m = migrate_v1(*p, {.dry_run = true})) {
            pending = m->pending;
        }
    }
    auto state = inspect_repo(*p);
    if (state == repo_layout::v1_only) {
        term::msg("{}: v1 format, {} images. Run `uenv repo migrate`.",
                  p->string(), pending);
        return 0;
    }
    auto s = store::open(*p, true);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto r = s->update(false, true);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    auto all = s->all();
    if (args.json) {
        nlohmann::json j{{"path", p->string()},
                         {"objects", all ? all->size() : 0},
                         {"missing", r->missing.size()},
                         {"prune", r->pruned.size()},
                         {"recent", r->kept_recent.size()},
                         {"parts", r->parts.size()},
                         {"released", r->released},
                         {"v1_not_migrated", pending}};
        term::msg("{}", j.dump());
        return 0;
    }
    term::msg("{}: {} objects", p->string(), all ? all->size() : 0);
    for (auto& m : r->missing) {
        term::msg("  missing blobs: {} id={}", label_string(m.cfg), m.id);
    }
    term::msg("  `uenv repo update` would delete {} blobs ({} bytes) and {} "
              ".part files; {} unused blobs are newer than 24 hours",
              r->pruned.size(), r->released, r->parts.size(),
              r->kept_recent.size());
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
    c.action([&settings](const repo_status_args& a) {
        return repo_status(a, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

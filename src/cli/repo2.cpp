// `uenv repo`: create, status, update and migrate a repository.

#include <filesystem>
#include <optional>
#include <string>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <uenv/migrate.h>
#include <uenv/store.h>
#include <util/expected.h>

#include "repo.h"
#include "terminal.h"
#include "uenv.h"

namespace uenv {

namespace fs = std::filesystem;
using argparse::completion;

namespace {

struct repo_args {
    std::optional<std::string> path;
    bool json = false;
    bool force = false;
};

std::optional<fs::path> repo_path(const repo_args& args,
                                  const global_settings& settings) {
    if (args.path) {
        return fs::absolute(*args.path);
    }
    if (auto r = settings.config.user_repo()) {
        return r->path;
    }
    return std::nullopt;
}

int repo_create(const repo_args& args, const global_settings& settings) {
    auto p = repo_path(args, settings);
    if (!p) {
        term::error("no repository path is given");
        return 1;
    }
    auto s = store::create(*p);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    term::msg("the repository {} is ready", p->string());
    return 0;
}

int repo_status(const repo_args& args, const global_settings& settings) {
    auto p = repo_path(args, settings);
    if (!p) {
        term::error("no repository path is given");
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
        term::msg("  missing blobs: {}/{}:{} id={}", m.cfg.name, m.cfg.version,
                  m.cfg.release, m.id);
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

int repo_update(const repo_args& args, const global_settings& settings) {
    auto p = repo_path(args, settings);
    if (!p) {
        term::error("no repository path is given");
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

int repo_migrate(const repo_args& args, const global_settings& settings) {
    auto p = repo_path(args, settings);
    if (!p) {
        term::error("no repository path is given");
        return 1;
    }
    auto r = migrate_v1(*p, {},
                        [](const std::string& m) { term::msg("{}", m); });
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("migrated {} images ({} already done, {} squashfs hashed)",
              r->migrated, r->skipped, r->hashed);
    if (r->failed) {
        term::error("{} images were not migrated. The other images can be "
                    "used. Run `uenv repo migrate` again after the problem is "
                    "fixed.\n{}",
                    r->failed, fmt::join(r->errors, "\n"));
        return 1;
    }
    return 0;
}

} // namespace

argparse::command repo_command(const global_settings& settings) {
    argparse::command_builder<> repo_cli("repo", "manage repositories");
    auto sub = [&](const char* name, const char* help, auto fn, bool json,
                   bool force) {
        argparse::command_builder<repo_args> c(name, help);
        c.add_positional("path", &repo_args::path, "the repository")
            .complete(completion::directory());
        if (json) {
            c.add_flag("json", &repo_args::json, "print JSON");
        }
        if (force) {
            c.add_flag("force", &repo_args::force,
                       "no grace period: use only when nothing writes");
        }
        c.action([&settings, fn](const repo_args& a) { return fn(a, settings); });
        repo_cli.add_subcommand(std::move(c).build());
    };
    sub("create", "create a repository", repo_create, false, false);
    sub("status", "check a repository", repo_status, true, false);
    sub("update", "delete records with missing blobs, prune unused blobs",
        repo_update, false, true);
    sub("migrate", "convert a v1 repository", repo_migrate, false, false);
    return std::move(repo_cli).build();
}

} // namespace uenv

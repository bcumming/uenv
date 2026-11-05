// `uenv repo migrate`: convert a v1 repository.

#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include <uenv/migrate.h>

#include "repo.h"
#include "repo_util.h"
#include "terminal.h"

namespace uenv {

using argparse::completion;

namespace {

struct repo_migrate_args {
    std::optional<std::string> path;
};

int repo_migrate(const repo_migrate_args& args,
                 const global_settings& settings) {
    auto path = cli::repo_path(args.path, settings);
    if (!path) {
        term::error("{}", path.error());
        return 1;
    }
    auto result = migrate_v1(*path, {}, [](const std::string& message) {
        term::msg("{}", message);
    });
    if (!result) {
        term::error("{}", result.error());
        return 1;
    }
    term::msg("migrated {} images ({} already done, {} squashfs hashed)",
              result->migrated, result->skipped, result->hashed);
    if (result->failed) {
        term::error("{} images were not migrated. The other images can be "
                    "used. Run `uenv repo migrate` again after the problem is "
                    "fixed.\n{}",
                    result->failed, fmt::join(result->errors, "\n"));
        return 1;
    }
    return 0;
}

} // namespace

argparse::command repo_migrate_command(const global_settings& settings) {
    argparse::command_builder<repo_migrate_args> c("migrate",
                                                   "convert a v1 repository");
    c.add_positional("path", &repo_migrate_args::path, "the repository")
        .complete(completion::directory());
    c.action([&settings](const repo_migrate_args& args) {
        return repo_migrate(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

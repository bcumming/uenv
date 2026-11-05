// The `uenv repo` command: the tree of its subcommands. Each subcommand is in
// a file of its own, and the code that they share is in repo_util.cpp.

#include <argparse/argparse.h>

#include "repo.h"

namespace uenv {

argparse::command repo_command(const global_settings& settings) {
    argparse::command_builder<> repo_cli("repo", "manage repositories");
    repo_cli.add_subcommand(repo_create_command(settings));
    repo_cli.add_subcommand(repo_status_command(settings));
    repo_cli.add_subcommand(repo_update_command(settings));
    repo_cli.add_subcommand(repo_migrate_command(settings));
    return std::move(repo_cli).build();
}

} // namespace uenv

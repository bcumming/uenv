// vim: ts=4 sts=4 sw=4 et
#pragma once

#include <argparse/argparse.h>

#include "uenv.h"

namespace uenv {

// the `uenv repo` command, and its subcommands: one file each
argparse::command repo_command(const global_settings& settings);

argparse::command repo_create_command(const global_settings& settings);
argparse::command repo_status_command(const global_settings& settings);
argparse::command repo_update_command(const global_settings& settings);
argparse::command repo_migrate_command(const global_settings& settings);

} // namespace uenv

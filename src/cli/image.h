// vim: ts=4 sts=4 sw=4 et
#pragma once

#include <argparse/argparse.h>

#include "uenv.h"

namespace uenv {

// the `uenv image` command, and its subcommands: one file each
argparse::command image_command(const global_settings& settings);

argparse::command image_ls_command(const global_settings& settings);
argparse::command image_add_command(const global_settings& settings);
argparse::command image_rm_command(const global_settings& settings);
// also the top-level `uenv inspect`
argparse::command image_inspect_command(const global_settings& settings);
argparse::command image_compose_command(const global_settings& settings);
argparse::command image_find_command(const global_settings& settings);
argparse::command image_pull_command(const global_settings& settings);
argparse::command image_push_command(const global_settings& settings);
argparse::command image_copy_command(const global_settings& settings);
argparse::command image_delete_command(const global_settings& settings);
argparse::command image_attach_command(const global_settings& settings);

} // namespace uenv

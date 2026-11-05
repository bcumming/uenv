// The `uenv image` command: the tree of its subcommands. Each subcommand is
// in a file of its own, and the code that they share is in image_util.cpp.

#include <argparse/argparse.h>

#include "image.h"

namespace uenv {

argparse::command image_command(const global_settings& settings) {
    argparse::command_builder<> image_cli("image",
                                          "manage and query uenv images");
    image_cli.add_subcommand(image_ls_command(settings));
    image_cli.add_subcommand(image_add_command(settings));
    image_cli.add_subcommand(image_rm_command(settings));
    image_cli.add_subcommand(image_inspect_command(settings));
    image_cli.add_subcommand(image_compose_command(settings));
    image_cli.add_subcommand(image_find_command(settings));
    image_cli.add_subcommand(image_pull_command(settings));
    image_cli.add_subcommand(image_push_command(settings));
    image_cli.add_subcommand(image_copy_command(settings));
    image_cli.add_subcommand(image_delete_command(settings));
    image_cli.add_subcommand(image_attach_command(settings));
    return std::move(image_cli).build();
}

} // namespace uenv

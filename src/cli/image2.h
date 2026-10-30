#pragma once

#include <argparse/argparse.h>

#include "uenv.h"

namespace uenv {

argparse::command image_command(const global_settings& settings);
argparse::command image_inspect_command(const global_settings& settings);

} // namespace uenv

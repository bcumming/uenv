// The code that the `uenv repo` commands share.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <util/expected.h>

#include "uenv.h"

namespace uenv::cli {

// the path of the repository: the argument (parsed), else the repository
// that the user writes
util::expected<std::filesystem::path, std::string>
repo_path(const std::optional<std::string>& arg,
          const global_settings& settings);

} // namespace uenv::cli

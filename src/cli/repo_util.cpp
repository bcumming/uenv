// The code that the `uenv repo` commands share.

#include <filesystem>
#include <optional>
#include <string>

#include <uenv/parse.h>
#include <uenv/session.h>
#include <util/expected.h>

#include "repo_util.h"

namespace uenv::cli {

util::expected<std::filesystem::path, std::string>
repo_path(const std::optional<std::string>& arg,
          const global_settings& settings) {
    if (arg) {
        return parse_file_path(*arg);
    }
    return user_repo_path(settings.config);
}

} // namespace uenv::cli

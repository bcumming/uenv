#pragma once

#include <optional>
#include <string>
#include <vector>

#include <uenv/model.h>
#include <uenv/ops.h>
#include <uenv/store.h>
#include <util/envvars.h>
#include <util/expected.h>

#include "uenv.h"

namespace uenv {

// open the repositories of the configuration, in order
util::expected<std::vector<store>, std::string>
open_stores(const global_settings& settings, bool readonly = true);

// open (or create) the repository that the user writes
util::expected<store, std::string> open_user_store(const global_settings& settings);

label_keys keys_of(const global_settings& settings);

struct session {
    composed_image image;
    concrete_image concrete;
    std::vector<std::string> views;
    envvars::state environment;
    std::vector<std::string> mounts; // "file:mount"
};

// resolve the list, merge it, concretise it, and make the environment of
// the command
util::expected<session, std::string>
prepare_session(const std::string& uenv_arg,
                const std::optional<std::string>& view_arg,
                bool no_default_view, bool force,
                const global_settings& settings);

} // namespace uenv

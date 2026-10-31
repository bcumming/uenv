#pragma once

#include <optional>
#include <string>
#include <vector>

#include <uenv/model.h>
#include <uenv/ops.h>
#include <uenv/store.h>
#include <util/envvars.h>
#include <util/expected.h>

#include <uenv/settings.h>

namespace uenv {

// open the repositories of the configuration, in order
util::expected<std::vector<store>, std::string>
open_stores(const configuration& config, bool readonly = true);

// open (or create) the repository that the user writes
util::expected<store, std::string> open_user_store(const configuration& config);

label_keys keys_of(const configuration& config);

struct session {
    composed_image image;
    concrete_image concrete;
    std::vector<view_request> views;
    envvars::state environment;
    std::vector<std::string> mounts; // "file:mount"
};

// resolve the list, make a runtime merge of it, concretise it, and make the
// environment of
// the command
util::expected<session, std::string>
prepare_session(const std::string& uenv_arg,
                const std::optional<std::string>& view_arg,
                bool no_default_view, bool force, const configuration& config,
                const envvars::state& calling_environment,
                std::optional<std::string> forward_prefix = "SQFSMNT_FWD_");

} // namespace uenv

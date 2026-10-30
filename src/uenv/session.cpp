#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <uenv/env.h>
#include <uenv/format.h>
#include <uenv/model.h>
#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/store.h>
#include <util/expected.h>

#include <uenv/session.h>

namespace uenv {

namespace fs = std::filesystem;
using util::unexpected;

util::expected<std::vector<store>, std::string>
open_stores(const configuration& config, bool readonly) {
    std::vector<store> stores;
    for (auto& r : config.repos) {
        // a v1 repository that the user cannot write, for example one that a
        // site provides, is skipped: the user cannot migrate it
        if (inspect_repo(r.path) == repo_layout::v1_only &&
            ::access(r.path.c_str(), W_OK) != 0) {
            spdlog::warn("the repository {} is in the v1 format and is read "
                         "only: it is ignored until it is migrated",
                         r.path.string());
            continue;
        }
        auto s = store::open(r.path, readonly);
        if (!s) {
            return unexpected{s.error()};
        }
        stores.push_back(std::move(*s));
    }
    return stores;
}

util::expected<store, std::string>
open_user_store(const configuration& config) {
    auto repo = config.user_repo();
    if (!repo) {
        return unexpected{std::string{
            "there is no repository: give one with --repo, or in the "
            "configuration"}};
    }
    return store::create(repo->path);
}

label_keys keys_of(const configuration& config) {
    return label_keys{config.label.at, config.label.percent};
}

util::expected<session, std::string>
prepare_session(const std::string& uenv_arg,
                const std::optional<std::string>& view_arg,
                bool no_default_view, bool force, const configuration& config,
                const envvars::state& calling_environment,
                std::optional<std::string> forward_prefix) {
    auto items = parse_item_list(uenv_arg, keys_of(config), config.system_name);
    if (!items) {
        return unexpected{items.error()};
    }
    // open the repositories only if an item needs a lookup: a squashfs file
    // needs no repository, also one that is not migrated
    std::vector<store> no_stores;
    util::expected<std::vector<store>, std::string> stores = std::move(no_stores);
    if (std::any_of(items->begin(), items->end(),
                    [](auto& i) { return !i.file; })) {
        stores = open_stores(config);
        if (!stores) {
            return unexpected{stores.error()};
        }
    }
    auto img = resolve_items(*stores, *items, force);
    if (!img) {
        return unexpected{img.error()};
    }
    // a runtime merge can put the mount point of one item inside a layer of
    // another item: check it here, as compose does, so that the error names
    // the layers. This runs unprivileged: see check_nested_mounts in ops.h.
    if (auto r = check_nested_mounts(*img); !r) {
        return unexpected{r.error()};
    }
    std::vector<view_description> requested;
    if (view_arg) {
        auto v = parse_view_args(*view_arg);
        if (!v) {
            return unexpected{
                fmt::format("invalid --view: {}", v.error().message())};
        }
        requested = std::move(*v);
    }
    auto c = concretise(*img, requested, !no_default_view);
    if (!c) {
        return unexpected{c.error()};
    }
    const auto& views = c->views;
    // a start checks that each squashfs is in the store before a mount
    for (auto& l : img->layers) {
        std::error_code ec;
        if (l.layer.store.file && !fs::exists(*l.layer.store.file, ec)) {
            return unexpected{fmt::format(
                "the squashfs of the layer '{}' of {} is missing ({}). Run "
                "`uenv repo update`, then pull the image again.",
                l.alias, img->config.name,
                l.layer.store.digest ? l.layer.store.digest->string()
                                     : l.layer.store.file->string())};
        }
    }
    session out{.image = *img, .concrete = *c, .views = views};
    out.environment = calling_environment;
    auto patches = c->patches;
    patches.push_back(session_patch(*c, views));
    apply_patches(out.environment, patches);

    // telemetry: one entry for each layer
    nlohmann::json tel = nlohmann::json::array();
    for (auto& l : img->layers) {
        std::vector<std::string> lviews;
        for (auto& v : views) {
            auto it = img->views.find({.item = v.uenv, .name = v.name});
            if (it != img->views.end()) {
                for (auto& inc : it->second.include) {
                    if (inc.alias == l.alias) {
                        lviews.push_back(inc.view);
                    }
                }
            } else if (v.uenv == l.alias) {
                lviews.push_back(v.name);
            }
        }
        auto& cfg = l.layer.config;
        nlohmann::json e{{"mount", l.mount},
                         {"sqfs", l.layer.store.file->string()},
                         {"views", lviews},
                         {"name", l.alias}};
        e["digest"] = l.layer.store.digest
                          ? nlohmann::json(l.layer.store.digest->hex())
                          : nlohmann::json(nullptr);
        e["label"] = cfg.version.empty()
                         ? nlohmann::json(nullptr)
                         : nlohmann::json(fmt::format("{}/{}:{}", cfg.name,
                                                      cfg.version, cfg.release));
        tel.push_back(e);
    }
    out.environment.set("UENV_TELEMETRY", tel.dump());
    if (forward_prefix) {
        forward_unsecure_variables(out.environment, *forward_prefix);
    }
    for (auto& [file, mount] : c->mounts) {
        out.mounts.push_back(fmt::format("{}:{}", file.string(), mount));
    }
    return out;
}

} // namespace uenv

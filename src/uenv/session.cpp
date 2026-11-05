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
    auto resolved = resolve_items(*stores, *items, force);
    if (!resolved) {
        return unexpected{resolved.error()};
    }
    auto img = &resolved->image;
    // a runtime merge can put the mount point of one item inside a layer of
    // another item: check it here, as compose does, so that the error names
    // the layers. This runs unprivileged: see check_nested_mounts in ops.h.
    if (auto r = check_nested_mounts(*img); !r) {
        return unexpected{r.error()};
    }
    std::vector<view_request> requested;
    if (view_arg) {
        auto v = parse_view_requests(*view_arg);
        if (!v) {
            return unexpected{fmt::format("invalid --view: {}", v.error())};
        }
        requested = std::move(*v);
    }
    auto c = concretise(*img, requested, !no_default_view);
    if (!c) {
        return unexpected{c.error()};
    }
    const auto& views = c->views;
    // a start checks that each squashfs is in the store before a mount
    for (auto& l : img->layers()) {
        std::error_code ec;
        if (l.layer.store.file && !fs::exists(*l.layer.store.file, ec)) {
            return unexpected{fmt::format(
                "the squashfs of the layer '{}' of {} is missing ({}). Run "
                "`uenv repo update`, then pull the image again.",
                l.alias, img->config().name,
                l.layer.store.digest ? l.layer.store.digest->string()
                                     : l.layer.store.file->string())};
        }
    }
    // the views of each item: a view with no item belongs to the one item
    // that has it
    auto described = std::move(resolved->items);
    for (auto& v : views) {
        std::optional<uenv::name> owner = v.item;
        if (!owner && described.size() == 1u) {
            owner = described.front().name;
        }
        if (!owner) {
            for (auto& [k, _] : img->views()) {
                if (k.name == v.name && k.item) {
                    owner = k.item;
                }
            }
        }
        for (auto& d : described) {
            if (owner && d.name == *owner) {
                d.views.push_back(v.name);
            }
        }
    }
    session out{.image = *img,
                .concrete = *c,
                .items = std::move(described),
                .views = views};
    out.environment = calling_environment;
    auto patches = c->patches;
    patches.push_back(session_patch(*c, views));
    apply_patches(out.environment, patches);

    // telemetry: one entry for each layer
    nlohmann::json tel = nlohmann::json::array();
    for (auto& l : img->layers()) {
        std::vector<std::string> lviews;
        for (auto& v : views) {
            auto it = img->views().find(image_view_name{.item = v.item, .name = v.name});
            if (it != img->views().end()) {
                for (auto& inc : it->second.include) {
                    if (inc.alias == l.alias) {
                        lviews.push_back(inc.view.string());
                    }
                }
            } else if (v.item == l.alias) {
                lviews.push_back(v.name.string());
            }
        }
        auto& cfg = l.layer.config;
        nlohmann::json e{{"mount", l.mount.string()},
                         {"sqfs", l.layer.store.file->string()},
                         {"views", lviews},
                         {"name", l.alias.string()}};
        e["digest"] = l.layer.store.digest
                          ? nlohmann::json(l.layer.store.digest->hex())
                          : nlohmann::json(nullptr);
        e["label"] = !cfg.version || !cfg.release
                         ? nlohmann::json(nullptr)
                         : nlohmann::json(fmt::format("{}/{}:{}", cfg.name,
                                                      *cfg.version, *cfg.release));
        tel.push_back(e);
    }
    out.environment.set("UENV_TELEMETRY", tel.dump());
    out.environment.set("UENV_SESSION", session_json(out.items));
    if (forward_prefix) {
        forward_unsecure_variables(out.environment, *forward_prefix);
    }
    for (auto& [file, mount] : c->mounts) {
        out.mounts.push_back(fmt::format("{}:{}", file.string(), mount));
    }
    return out;
}

namespace {

const char* kind_string(session_item::kind_t k) {
    switch (k) {
    case session_item::kind_t::layer:
        return "layer";
    case session_item::kind_t::file:
        return "file";
    default:
        return "image";
    }
}

} // namespace

std::string session_json(const std::vector<session_item>& items) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& d : items) {
        nlohmann::json layers = nlohmann::json::array();
        for (auto& l : d.layers) {
            layers.push_back({{"alias", l.alias.string()},
                              {"label", l.label},
                              {"mount", l.mount.string()}});
        }
        nlohmann::json views = nlohmann::json::array();
        for (auto& v : d.views) {
            views.push_back(v.string());
        }
        j.push_back({{"name", d.name.string()},
                     {"kind", kind_string(d.kind)},
                     {"label", d.label},
                     {"digest", d.digest ? nlohmann::json(d.digest->string())
                                         : nlohmann::json(nullptr)},
                     {"layers", layers},
                     {"views", views}});
    }
    return j.dump();
}

util::expected<std::vector<session_item>, std::string>
parse_session_json(const std::string& text) {
    using json = nlohmann::json;
    auto j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        return unexpected{std::string{"UENV_SESSION is not a JSON array"}};
    }
    auto str = [](const json& o, const char* key)
        -> util::expected<std::string, std::string> {
        auto it = o.find(key);
        if (it == o.end() || !it->is_string()) {
            return unexpected{
                fmt::format("UENV_SESSION: '{}' must be a string", key)};
        }
        return it->get<std::string>();
    };
    auto nm = [&](const json& o, const char* key)
        -> util::expected<uenv::name, std::string> {
        auto t = str(o, key);
        if (!t) {
            return unexpected{t.error()};
        }
        auto n = uenv::name::parse(*t);
        if (!n) {
            return unexpected{fmt::format("UENV_SESSION: '{}': {}", key, n.error())};
        }
        return *n;
    };
    std::vector<session_item> out;
    for (auto& e : j) {
        if (!e.is_object()) {
            return unexpected{std::string{"UENV_SESSION: an item is not an object"}};
        }
        auto name = nm(e, "name");
        auto kind = str(e, "kind");
        auto label = str(e, "label");
        if (!name || !kind || !label) {
            return unexpected{!name ? name.error() : !kind ? kind.error() : label.error()};
        }
        session_item::kind_t k;
        if (*kind == "image") {
            k = session_item::kind_t::image;
        } else if (*kind == "layer") {
            k = session_item::kind_t::layer;
        } else if (*kind == "file") {
            k = session_item::kind_t::file;
        } else {
            return unexpected{fmt::format("UENV_SESSION: the kind '{}' is not "
                                          "image, layer or file", *kind)};
        }
        std::optional<oci::digest> digest;
        if (auto d = e.find("digest"); d != e.end() && !d->is_null()) {
            if (!d->is_string()) {
                return unexpected{
                    std::string{"UENV_SESSION: the digest is not a string"}};
            }
            auto p = oci::digest::parse(d->get<std::string>());
            if (!p) {
                return unexpected{fmt::format("UENV_SESSION: the digest: {}",
                                              p.error().message())};
            }
            digest = *p;
        }
        session_item item{.name = *name, .kind = k, .label = *label, .digest = digest};
        auto layers = e.find("layers");
        auto views = e.find("views");
        if (layers == e.end() || !layers->is_array() || views == e.end() ||
            !views->is_array()) {
            return unexpected{std::string{
                "UENV_SESSION: an item needs the arrays 'layers' and 'views'"}};
        }
        for (auto& l : *layers) {
            if (!l.is_object()) {
                return unexpected{std::string{"UENV_SESSION: a layer is not an object"}};
            }
            auto alias = nm(l, "alias");
            auto llabel = str(l, "label");
            auto mount = str(l, "mount");
            if (!alias || !llabel || !mount) {
                return unexpected{!alias ? alias.error() : !llabel ? llabel.error() : mount.error()};
            }
            auto mp = mount_point::parse(*mount);
            if (!mp) {
                return unexpected{fmt::format("UENV_SESSION: {}", mp.error())};
            }
            item.layers.push_back({.alias = *alias, .label = *llabel, .mount = *mp});
        }
        for (auto& v : *views) {
            auto n = v.is_string() ? uenv::name::parse(v.get<std::string>())
                                   : util::unexpected{std::string{"not a string"}};
            if (!n) {
                return unexpected{fmt::format("UENV_SESSION: a view: {}", n.error())};
            }
            item.views.push_back(*n);
        }
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace uenv

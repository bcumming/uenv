#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <uenv/model.h>
#include <util/envvars.h>
#include <util/expected.h>

namespace uenv {

namespace {

std::string replace_all(std::string s, std::string_view from,
                        std::string_view to) {
    if (from.empty()) {
        return s;
    }
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool valid_var_name(std::string_view name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

// split "alias:view" at the first ':'
std::optional<std::pair<std::string, std::string>>
split_qualified(const std::string& s) {
    auto pos = s.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 == s.size()) {
        return std::nullopt;
    }
    return std::make_pair(s.substr(0, pos), s.substr(pos + 1));
}

const named_layer* find_alias(const std::vector<named_layer>& layers,
                              const std::string& alias) {
    for (auto& l : layers) {
        if (l.alias == alias) {
            return &l;
        }
    }
    return nullptr;
}

util::expected<composed_image, model_error>
make_composed_image_impl(std::optional<oci::digest> digest, config cfg,
                         std::vector<named_layer> layers,
                         std::map<std::string, image_view> views,
                         std::vector<std::string> default_views,
                         bool allow_forced_moves) {
    using util::unexpected;
    if (layers.empty()) {
        return unexpected{model_error{"an image must have at least one layer"}};
    }
    std::set<std::string> aliases;
    std::set<std::string> mounts;
    for (auto& l : layers) {
        if (l.alias.empty()) {
            return unexpected{fmt::format("a layer has an empty alias")};
        }
        if (!aliases.insert(l.alias).second) {
            return unexpected{
                fmt::format("two layers have the same alias '{}'", l.alias)};
        }
        if (auto r = validate_mount_point(l.mount); !r) {
            return unexpected{
                fmt::format("layer '{}': {}", l.alias, r.error())};
        }
        if (!mounts.insert(l.mount).second) {
            return unexpected{fmt::format(
                "two layers have the same mount point '{}'", l.mount)};
        }
        if (!l.layer.relocatable && l.mount != l.layer.mount &&
            !allow_forced_moves) {
            return unexpected{fmt::format(
                "layer '{}' is not relocatable: it must be mounted at its "
                "build mount point '{}', not '{}'",
                l.alias, l.layer.mount, l.mount)};
        }
    }
    for (auto& [name, v] : views) {
        if (name.empty()) {
            return unexpected{model_error{"an image view has an empty name"}};
        }
        for (auto& inc : v.include) {
            auto q = split_qualified(inc);
            if (!q) {
                return unexpected{fmt::format(
                    "image view '{}': '{}' is not of the form alias:view", name,
                    inc)};
            }
            auto l = find_alias(layers, q->first);
            if (!l) {
                return unexpected{
                    fmt::format("image view '{}' includes '{}', but the image "
                                "has no layer with the alias '{}'",
                                name, inc, q->first)};
            }
            if (!l->layer.views.contains(q->second)) {
                return unexpected{
                    fmt::format("image view '{}' includes '{}', but the layer "
                                "'{}' has no view '{}'",
                                name, inc, q->first, q->second)};
            }
        }
    }
    for (auto& d : default_views) {
        if (!views.contains(d)) {
            return unexpected{
                fmt::format("the default view '{}' is not a view of the image",
                            d)};
        }
    }
    std::sort(layers.begin(), layers.end(),
              [](auto& a, auto& b) { return a.mount < b.mount; });
    return composed_image{.digest = std::move(digest),
                          .is_layer = false,
                          .layers = std::move(layers),
                          .config = std::move(cfg),
                          .views = std::move(views),
                          .default_views = std::move(default_views)};
}

} // namespace

util::expected<void, model_error> validate_mount_point(const std::string& m) {
    if (m.empty() || m[0] != '/') {
        return util::unexpected{
            fmt::format("the mount point '{}' is not an absolute path", m)};
    }
    if (m.find('$') != std::string::npos ||
        m.find(uenv_root_placeholder) != std::string::npos) {
        return util::unexpected{fmt::format(
            "the mount point '{}' contains a variable or placeholder", m)};
    }
    if (m.find(',') != std::string::npos || m.find(':') != std::string::npos) {
        return util::unexpected{fmt::format(
            "the mount point '{}' contains ',' or ':', which "
            "UENV_MOUNT_LIST cannot represent",
            m)};
    }
    if (m.size() > 1 && m.back() == '/') {
        return util::unexpected{
            fmt::format("the mount point '{}' has a trailing '/'", m)};
    }
    return {};
}

util::expected<composed_image, model_error>
make_composed_image(std::optional<oci::digest> digest, config cfg,
                    std::vector<named_layer> layers,
                    std::map<std::string, image_view> views,
                    std::optional<std::string> default_view) {
    std::vector<std::string> defaults;
    if (default_view) {
        defaults.push_back(*default_view);
    }
    return make_composed_image_impl(std::move(digest), std::move(cfg),
                                    std::move(layers), std::move(views),
                                    std::move(defaults), false);
}

util::expected<composed_image, model_error>
raise_layer(const layer& l, std::optional<std::string> mount, bool force,
            std::optional<std::string> alias) {
    const std::string m = mount.value_or(l.mount);
    bool forced = false;
    if (m != l.mount && !l.relocatable) {
        if (!force) {
            return util::unexpected{fmt::format(
                "the layer {} is not relocatable: it can only be mounted at "
                "{}. Use --force to mount it at {}",
                l.config.name, l.mount, m)};
        }
        spdlog::warn("the layer {} is not relocatable: mounting it at {} "
                     "instead of {}. Its views can have wrong paths.",
                     l.config.name, m, l.mount);
        forced = true;
    }
    const std::string a = alias.value_or(l.config.name);
    std::map<std::string, image_view> views;
    for (auto& [name, _] : l.views) {
        views[name] = image_view{.include = {a + ":" + name}};
    }
    std::vector<std::string> defaults;
    if (l.default_view) {
        defaults.push_back(*l.default_view);
    }
    auto img = make_composed_image_impl(
        l.digest, l.config, {named_layer{.alias = a, .mount = m, .layer = l}},
        std::move(views), std::move(defaults), forced);
    if (img) {
        img->is_layer = true;
    }
    return img;
}

util::expected<composed_image, model_error>
merge_items(std::vector<merge_item> items, digest_fn digest_of) {
    using util::unexpected;
    if (items.empty()) {
        return unexpected{model_error{"no image to start"}};
    }
    // one item with nothing to change is the item itself
    if (items.size() == 1u && !items[0].alias && !items[0].mount) {
        return std::move(items[0].image);
    }

    std::vector<named_layer> layers;
    std::map<std::string, image_view> views;
    std::vector<std::string> defaults;
    std::vector<std::string> names;
    bool all_digests = true;
    bool forced = false;

    for (auto& item : items) {
        auto& img = item.image;
        if (img.layers.size() != 1u) {
            return unexpected{fmt::format(
                "the image {} has {} layers: an image with more than one layer "
                "cannot be merged with other items. Write an image config "
                "that contains all the layers, and compose it.",
                img.config.name, img.layers.size())};
        }
        auto nl = img.layers.front();
        const std::string old_alias = nl.alias;
        const std::string alias = item.alias.value_or(img.config.name);
        if (item.mount && *item.mount != nl.mount) {
            if (!item.movable) {
                return unexpected{fmt::format(
                    "the image {} cannot move to {}: only a layer can be "
                    "mounted at a different mount point. Give the layer "
                    "with the layer marker '+' instead.",
                    img.config.name, *item.mount)};
            }
            if (!nl.layer.relocatable) {
                if (!item.force) {
                    return unexpected{fmt::format(
                        "the layer {} is not relocatable: it can only be "
                        "mounted at {}. Use --force to mount it at {}",
                        nl.layer.config.name, nl.layer.mount, *item.mount)};
                }
                spdlog::warn("the layer {} is not relocatable: mounting it "
                             "at {}. Its views can have wrong paths.",
                             nl.layer.config.name, *item.mount);
                forced = true;
            }
            nl.mount = *item.mount;
        }
        if (!nl.layer.relocatable && nl.mount != nl.layer.mount) {
            forced = true;
        }
        nl.alias = alias;
        if (!nl.layer.digest) {
            all_digests = false;
        }
        names.push_back(alias);

        for (auto& [vname, v] : img.views) {
            image_view nv = v;
            for (auto& inc : nv.include) {
                auto q = split_qualified(inc);
                if (q && q->first == old_alias) {
                    inc = alias + ":" + q->second;
                }
            }
            const auto key = alias + ":" + vname;
            if (views.contains(key)) {
                return unexpected{fmt::format(
                    "two items give the view '{}': give one item an alias",
                    key)};
            }
            views[key] = std::move(nv);
        }
        for (auto& d : img.default_views) {
            defaults.push_back(alias + ":" + d);
        }
        layers.push_back(std::move(nl));
    }

    config cfg{.name = fmt::format("{}", fmt::join(names, ","))};
    auto result =
        make_composed_image_impl(std::nullopt, std::move(cfg), std::move(layers),
                                 std::move(views), std::move(defaults), forced);
    if (!result) {
        return result;
    }
    // the name of the merge: the aliases in the order of the layers, so that
    // the order of the list has no effect
    std::vector<std::string> sorted;
    for (auto& l : result->layers) {
        sorted.push_back(l.alias);
    }
    result->config.name = fmt::format("{}", fmt::join(sorted, ","));
    if (all_digests && digest_of) {
        result->digest = digest_of(*result);
    }
    return result;
}

envvars::patch replace_root(const envvars::patch& p, const std::string& mount) {
    envvars::patch out;
    for (auto& [name, s] : p.scalars()) {
        if (s.value) {
            out.update_scalar(name,
                              replace_all(*s.value, uenv_root_placeholder, mount));
        } else {
            out.update_scalar(name, std::nullopt);
        }
    }
    for (auto& [name, pp] : p.prefix_paths()) {
        for (auto u : pp.updates()) {
            for (auto& v : u.values) {
                v = replace_all(v, uenv_root_placeholder, mount);
            }
            out.update_prefix_path(name, u);
        }
    }
    return out;
}

util::expected<concrete_image, model_error>
concretise(const composed_image& img, const std::vector<std::string>& views) {
    using util::unexpected;
    concrete_image result;

    // 1. the mount list
    for (auto& l : img.layers) {
        if (!l.layer.store.file) {
            return unexpected{fmt::format(
                "the layer '{}' ({}) has no local file system image: pull the "
                "image first",
                l.alias,
                l.layer.digest ? l.layer.digest->string() : "no digest")};
        }
        result.mounts.push_back({*l.layer.store.file, l.mount});
    }

    // 2. replace @uenv-root@ in each layer view
    std::map<std::string, std::map<std::string, envvars::patch>> layer_views;
    for (auto& l : img.layers) {
        for (auto& [vname, v] : l.layer.views) {
            layer_views[l.alias][vname] = replace_root(v.env, l.mount);
        }
    }

    // 3. find the views
    const auto& names = views.empty() ? img.default_views : views;

    // 4. the list of patches
    for (auto& name : names) {
        if (auto it = img.views.find(name); it != img.views.end()) {
            for (auto& inc : it->second.include) {
                auto q = split_qualified(inc);
                result.patches.push_back(layer_views.at(q->first).at(q->second));
            }
            result.patches.push_back(it->second.env);
            continue;
        }
        if (auto q = split_qualified(name)) {
            if (auto l = layer_views.find(q->first); l != layer_views.end()) {
                if (auto v = l->second.find(q->second); v != l->second.end()) {
                    result.patches.push_back(v->second);
                    continue;
                }
            }
        }
        std::vector<std::string> available;
        for (auto& [k, _] : img.views) {
            available.push_back(k);
        }
        for (auto& l : img.layers) {
            for (auto& [k, _] : l.layer.views) {
                available.push_back(l.alias + ":" + k);
            }
        }
        return unexpected{
            fmt::format("'{}' is not a view of the image. The views are: {}",
                        name, fmt::join(available, ", "))};
    }
    return result;
}

envvars::patch session_patch(const concrete_image& c,
                             const std::vector<std::string>& views) {
    envvars::patch p;
    std::vector<std::string> mounts;
    for (auto& [file, mount] : c.mounts) {
        mounts.push_back(fmt::format("{}:{}", file.string(), mount));
    }
    p.update_scalar("UENV_MOUNT_LIST",
                    fmt::format("{}", fmt::join(mounts, ",")));
    // always set: a session is detected by UENV_MOUNT_LIST and UENV_VIEW
    p.update_scalar("UENV_VIEW", fmt::format("{}", fmt::join(views, ",")));
    return p;
}

std::string expand_view_value(std::string_view value,
                              const envvars::state& before,
                              apply_report& report) {
    std::string out;
    std::size_t pos = 0;
    while (pos < value.size()) {
        auto start = value.find("${@", pos);
        if (start == std::string_view::npos) {
            out.append(value.substr(pos));
            break;
        }
        out.append(value.substr(pos, start - pos));
        auto end = value.find("@}", start + 3);
        if (end == std::string_view::npos) {
            report.errors.push_back(fmt::format(
                "the value '{}' has '${{@' with no closing '@}}': the text "
                "from '${{@' to the end is dropped",
                value));
            break;
        }
        auto name = value.substr(start + 3, end - start - 3);
        if (!valid_var_name(name)) {
            report.warnings.push_back(fmt::format(
                "'{}' is not a valid variable name: it is not replaced", name));
            out.append(value.substr(start, end + 2 - start));
        } else if (auto v = before.get(name)) {
            out.append(*v);
        } else {
            report.warnings.push_back(fmt::format(
                "the variable {} is not set: it is replaced with ''", name));
        }
        pos = end + 2;
    }
    return out;
}

apply_report apply_patches(envvars::state& env,
                           const std::vector<envvars::patch>& patches) {
    apply_report report;
    for (auto& p : patches) {
        const envvars::state before = env;
        for (auto& [name, s] : p.scalars()) {
            if (s.value) {
                env.set(name, expand_view_value(*s.value, before, report));
            } else {
                env.unset(name);
            }
        }
        for (auto& [name, pp] : p.prefix_paths()) {
            envvars::prefix_path expanded{name};
            for (auto u : pp.updates()) {
                for (auto& v : u.values) {
                    v = expand_view_value(v, before, report);
                }
                expanded.update(u);
            }
            if (auto value = expanded.get(before.get(name).value_or(""))) {
                env.set(name, *value);
            } else {
                env.unset(name);
            }
        }
    }
    for (auto& w : report.warnings) {
        spdlog::warn("{}", w);
    }
    for (auto& e : report.errors) {
        spdlog::error("{}", e);
    }
    return report;
}

} // namespace uenv

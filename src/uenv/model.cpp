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
                         std::map<image_view_name, image_view> views,
                         std::vector<image_view_name> default_views,
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
        for (auto& inc : v.include) {
            auto l = find_alias(layers, inc.alias);
            if (!l) {
                return unexpected{
                    fmt::format("image view '{}' includes '{}', but the image "
                                "has no layer with the alias '{}'",
                                name, inc, inc.alias)};
            }
            if (!l->layer.views.contains(inc.view)) {
                return unexpected{
                    fmt::format("image view '{}' includes '{}', but the layer "
                                "'{}' has no view '{}'",
                                name, inc, inc.alias, inc.view)};
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
                          .kind = image_kind::image,
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
    // a stored image names its views with no alias
    std::map<image_view_name, image_view> named;
    for (auto& [name, v] : views) {
        named.emplace(image_view_name{.name = name}, std::move(v));
    }
    std::vector<image_view_name> defaults;
    if (default_view) {
        defaults.push_back({.name = *default_view});
    }
    return make_composed_image_impl(std::move(digest), std::move(cfg),
                                    std::move(layers), std::move(named),
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
    std::map<image_view_name, image_view> views;
    for (auto& [name, _] : l.views) {
        views[{.name = name}] =
            image_view{.include = {{.alias = a, .view = name}}};
    }
    std::vector<image_view_name> defaults;
    if (l.default_view) {
        defaults.push_back({.name = *l.default_view});
    }
    auto img = make_composed_image_impl(
        l.digest(), l.config, {named_layer{.alias = a, .mount = m, .layer = l}},
        std::move(views), std::move(defaults), forced);
    if (img) {
        img->kind = uenv::image_kind::layer;
    }
    return img;
}

namespace {

// the layer of an earlier item that is the same layer at the same mount point
const named_layer* find_same(const std::vector<named_layer>& layers,
                             const named_layer& l) {
    if (!l.layer.digest()) {
        return nullptr;
    }
    for (auto& o : layers) {
        if (o.layer.digest() == l.layer.digest() && o.mount == l.mount) {
            return &o;
        }
    }
    return nullptr;
}

} // namespace

util::expected<composed_image, model_error>
runtime_merge(std::vector<runtime_merge_item> items) {
    using util::unexpected;
    if (items.empty()) {
        return unexpected{model_error{"no image to start"}};
    }
    // one item with nothing to change is the item itself
    if (items.size() == 1u && !items[0].alias && !items[0].mount) {
        return std::move(items[0].image);
    }

    std::vector<named_layer> layers;
    std::map<image_view_name, image_view> views;
    std::vector<image_view_name> defaults;
    std::vector<std::string> names;
    bool forced = false;

    for (auto& item : items) {
        auto& img = item.image;
        const std::string item_alias = item.alias.value_or(img.config.name);
        if (std::find(names.begin(), names.end(), item_alias) != names.end()) {
            return unexpected{fmt::format(
                "two items of the list have the name '{}': give one of them "
                "an alias, for example x={}",
                item_alias, img.config.name)};
        }
        names.push_back(item_alias);
        if (item.mount && img.layers.size() != 1u) {
            return unexpected{fmt::format(
                "the image {} has {} layers: it cannot move to {}",
                img.config.name, img.layers.size(), *item.mount)};
        }

        // the alias of each layer of the item, in the runtime merge
        std::map<std::string, std::string> rename;
        for (auto nl : img.layers) {
            const std::string source = nl.alias;
            if (img.layers.size() == 1u) {
                // the one layer of an item takes the alias of the item
                nl.alias = item_alias;
                if (item.mount && *item.mount != nl.mount) {
                    if (!item.movable) {
                        return unexpected{fmt::format(
                            "the image {} cannot move to {}: only a layer can "
                            "be mounted at a different mount point. Give the "
                            "layer with the layer marker '+' instead.",
                            img.config.name, *item.mount)};
                    }
                    if (!nl.layer.relocatable) {
                        if (!item.force) {
                            return unexpected{fmt::format(
                                "the layer {} is not relocatable: it can only "
                                "be mounted at {}. Use --force to mount it at "
                                "{}",
                                nl.layer.config.name, nl.layer.mount,
                                *item.mount)};
                        }
                        spdlog::warn("the layer {} is not relocatable: "
                                     "mounting it at {}. Its views can have "
                                     "wrong paths.",
                                     nl.layer.config.name, *item.mount);
                    }
                    nl.mount = *item.mount;
                }
            }
            // a layer that an earlier item has at the same mount point is
            // mounted once
            if (auto same = find_same(layers, nl)) {
                rename[source] = same->alias;
                continue;
            }
            // a clash of aliases between items: rename the later layer
            if (find_alias(layers, nl.alias)) {
                const auto renamed = fmt::format("{}.{}", item_alias, nl.alias);
                spdlog::warn("the layer '{}' of {} has the alias of a layer of "
                             "an earlier item: it is renamed '{}'",
                             nl.alias, item_alias, renamed);
                nl.alias = renamed;
            }
            if (!nl.layer.relocatable && nl.mount != nl.layer.mount) {
                forced = true;
            }
            rename[source] = nl.alias;
            layers.push_back(std::move(nl));
        }

        // the image views of the item, named by the alias of the item. Each
        // include refers to a layer of the item (make_composed_image checks
        // it), so the rename has an entry for it.
        for (auto& [vname, v] : img.views) {
            image_view nv = v;
            for (auto& inc : nv.include) {
                inc.alias = rename.at(inc.alias);
            }
            views[{.item = item_alias, .name = vname.name}] = std::move(nv);
        }
        for (auto& d : img.default_views) {
            defaults.push_back({.item = item_alias, .name = d.name});
        }
    }

    // the name is only for display: a runtime merge is never stored
    config cfg{.name = fmt::format("{}", fmt::join(names, ","))};
    auto result =
        make_composed_image_impl(std::nullopt, std::move(cfg), std::move(layers),
                                 std::move(views), std::move(defaults), forced);
    if (result) {
        result->kind = image_kind::runtime_merge;
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
concretise(const composed_image& img,
           const std::vector<view_description>& views,
           bool use_default_views) {
    using util::unexpected;
    concrete_image result;

    // 1. the mount list
    for (auto& l : img.layers) {
        if (!l.layer.store.file) {
            return unexpected{fmt::format(
                "the layer '{}' ({}) has no local file system image: pull the "
                "image first",
                l.alias,
                l.layer.digest() ? l.layer.digest()->string() : "no digest")};
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
    result.views = views;
    if (views.empty() && use_default_views) {
        for (auto& d : img.default_views) {
            result.views.push_back({.uenv = d.item, .name = d.name});
        }
    }

    // 4. the list of patches. A name is an image view first, else a layer
    // view: in a runtime merge, the image view `a:v` of an item wraps the
    // layer view `a:v`, and the image view can add an env patch.
    for (auto& v : result.views) {
        auto it = img.views.find({.item = v.uenv, .name = v.name});
        // in a runtime merge, a name with no item is the view of the one
        // item that has a view of that name
        if (it == img.views.end() && !v.uenv) {
            std::vector<std::string> found;
            for (auto i = img.views.begin(); i != img.views.end(); ++i) {
                if (i->first.name == v.name) {
                    it = i;
                    found.push_back(format_as(i->first));
                }
            }
            if (found.size() > 1u) {
                return unexpected{fmt::format(
                    "more than one item has the view '{}': give one of {}",
                    v.name, fmt::join(found, ", "))};
            }
        }
        if (it != img.views.end()) {
            for (auto& inc : it->second.include) {
                result.patches.push_back(layer_views.at(inc.alias).at(inc.view));
            }
            result.patches.push_back(it->second.env);
            continue;
        }
        if (v.uenv) {
            if (auto l = layer_views.find(*v.uenv); l != layer_views.end()) {
                if (auto lv = l->second.find(v.name); lv != l->second.end()) {
                    result.patches.push_back(lv->second);
                    continue;
                }
            }
        }
        std::vector<std::string> available;
        for (auto& [k, _] : img.views) {
            available.push_back(format_as(k));
        }
        for (auto& l : img.layers) {
            for (auto& [k, _] : l.layer.views) {
                available.push_back(
                    format_as(layer_view_ref{.alias = l.alias, .view = k}));
            }
        }
        return unexpected{
            fmt::format("'{}' is not a view of the image. The views are: {}",
                        view_string(v), fmt::join(available, ", "))};
    }
    return result;
}

std::string view_string(const view_description& v) {
    return v.uenv ? fmt::format("{}:{}", *v.uenv, v.name) : v.name;
}

envvars::patch session_patch(const concrete_image& c,
                             const std::vector<view_description>& views) {
    envvars::patch p;
    std::vector<std::string> mounts;
    for (auto& [file, mount] : c.mounts) {
        mounts.push_back(fmt::format("{}:{}", file.string(), mount));
    }
    p.update_scalar("UENV_MOUNT_LIST",
                    fmt::format("{}", fmt::join(mounts, ",")));
    // always set: a session is detected by UENV_MOUNT_LIST and UENV_VIEW
    std::vector<std::string> names;
    for (auto& v : views) {
        names.push_back(view_string(v));
    }
    p.update_scalar("UENV_VIEW", fmt::format("{}", fmt::join(names, ",")));
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

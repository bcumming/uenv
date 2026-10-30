#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <oci/manifest.h>
#include <uenv/format.h>
#include <uenv/meta.h>
#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/store.h>
#include <util/expected.h>
#include <util/fs.h>

namespace uenv {

namespace fs = std::filesystem;
using util::unexpected;

namespace {

bool is_hex(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
    });
}

} // namespace

util::expected<label_query, std::string>
parse_label_query(std::string text, const label_keys& keys,
                  std::optional<std::string> default_system) {
    label_query q;
    if (!text.empty() && text[0] == '+') {
        q.layer = true;
        text = text.substr(1);
    }
    // an id or a digest
    if (text.starts_with("sha256:") || (is_hex(text) && (text.size() == 16 ||
                                                         text.size() == 64))) {
        q.id = text;
        return q;
    }
    auto label = parse_uenv_label(text);
    if (!label) {
        return unexpected{fmt::format("invalid label '{}': {}", text,
                                      label.error().message())};
    }
    q.name = label->name;
    q.version = label->version;
    q.release = label->tag;
    if (label->system && *label->system != "*") {
        q.meta[keys.at] = *label->system;
    } else if (!label->system && default_system) {
        q.meta[keys.at] = *default_system;
    }
    if (label->uarch) {
        q.meta[keys.percent] = *label->uarch;
    }
    return q;
}

util::expected<std::vector<item_spec>, std::string>
parse_item_list(const std::string& text, const label_keys& keys,
                std::optional<std::string> default_system) {
    std::vector<item_spec> items;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(',', start);
        auto word = text.substr(start, end == std::string::npos
                                           ? std::string::npos
                                           : end - start);
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        if (word.empty()) {
            continue;
        }
        item_spec item{.text = word};
        // [alias=]
        if (auto eq = word.find('='); eq != std::string::npos && eq > 0) {
            auto alias = word.substr(0, eq);
            if (std::all_of(alias.begin(), alias.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
                })) {
                item.alias = alias;
                word = word.substr(eq + 1);
            }
        }
        bool layer = !word.empty() && word[0] == '+';
        auto rest = layer ? word.substr(1) : word;
        auto desc = parse_uenv_description(rest);
        if (!desc) {
            return unexpected{fmt::format("invalid item '{}': {}", item.text,
                                          desc.error().message())};
        }
        item.mount = desc->mount();
        if (auto file = desc->filename()) {
            item.file = fs::absolute(*file);
        } else if (auto label = desc->label()) {
            // keep the text of the label: re-parse it as a query
            auto label_text = rest;
            if (item.mount) {
                label_text = label_text.substr(
                    0, label_text.size() - item.mount->size() - 1);
            }
            auto q = parse_label_query((layer ? "+" : "") + label_text, keys,
                                       default_system);
            if (!q) {
                return unexpected{q.error()};
            }
            item.query = *q;
        }
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        return unexpected{std::string{"no image is given"}};
    }
    return items;
}

namespace v1 {

format::layer_config make_layer_config(const uenv::meta& m,
                                       const std::optional<v1_label>& label) {
    format::layer_config c;
    c.shared.name = m.name;
    c.shared.description = m.description.value_or("");
    if (label) {
        c.shared.version = label->version;
        c.shared.release = label->release;
        if (label->uarch) {
            c.shared.meta["uarch"] = *label->uarch;
        }
    }
    c.mount = m.mount;
    c.relocatable = false;
    for (auto& [name, v] : m.views) {
        c.views[name] =
            layer_view{.description = v.description, .env = v.environment};
    }
    c.default_view = m.default_view;
    return c;
}

} // namespace v1

util::expected<uenv::meta, std::string>
read_squashfs_meta(const fs::path& sqfs) {
    auto next_to = sqfs.parent_path() / "meta" / "env.json";
    std::error_code ec;
    if (fs::is_regular_file(next_to, ec)) {
        return load_meta(next_to);
    }
    auto tmp = util::unsquashfs_tmp(sqfs, "meta");
    if (!tmp) {
        return unexpected{tmp.error()};
    }
    auto inside = *tmp / "meta" / "env.json";
    if (!fs::is_regular_file(inside, ec)) {
        return unexpected{
            fmt::format("{} has no meta/env.json", sqfs.string())};
    }
    return load_meta(inside);
}

util::expected<composed_image, std::string>
raise_from_squashfs(const fs::path& sqfs, std::optional<std::string> mount) {
    std::error_code ec;
    if (!fs::is_regular_file(sqfs, ec)) {
        return unexpected{fmt::format("{} is not a file", sqfs.string())};
    }
    format::layer_config cfg;
    if (auto m = read_squashfs_meta(sqfs)) {
        cfg = v1::make_layer_config(*m, std::nullopt);
    } else {
        spdlog::warn("{}: the layer has no views", m.error());
        cfg.shared.name = sqfs.stem().string();
        if (!mount) {
            return unexpected{fmt::format(
                "{} has no meta/env.json: give the mount point, for example "
                "{}:/user-environment",
                sqfs.string(), sqfs.string())};
        }
        cfg.mount = *mount;
    }
    if (mount && *mount != cfg.mount) {
        spdlog::warn("{} was made for the mount point {}, and is mounted at {}",
                     sqfs.string(), cfg.mount, *mount);
        cfg.mount = *mount;
    }
    if (auto r = validate_mount_point(cfg.mount); !r) {
        return unexpected{r.error()};
    }
    layer l;
    l.store.size = fs::file_size(sqfs, ec);
    l.store.file = sqfs;
    l.config = cfg.shared;
    l.mount = cfg.mount;
    l.relocatable = false;
    l.views = cfg.views;
    l.default_view = cfg.default_view;
    return raise_layer(l, std::nullopt, false);
}

util::expected<object_record, std::string>
write_image(store& s, const composed_image& img) {
    auto objs = format::make_image_objects(img);
    if (!objs) {
        return unexpected{objs.error()};
    }
    auto empty = format::make_blob(std::string(oci::empty_config_body));
    for (auto* b : {&empty, &objs->image_config, &objs->wrapper, &objs->index}) {
        if (auto r = s.put_bytes(*b); !r) {
            return unexpected{r.error()};
        }
        test_fault("add-after-blob");
    }
    return s.record(objs->index.digest);
}

util::expected<object_record, std::string>
add_squashfs(store& s, const fs::path& sqfs, const format::layer_config& cfg,
             const std::map<std::string, std::string>& image_meta,
             bool as_layer,
             std::function<void(std::uint64_t, std::uint64_t)> progress) {
    if (cfg.shared.release.empty() || cfg.shared.version.empty() ||
        cfg.shared.name.empty()) {
        return unexpected{std::string{
            "a layer needs a name, a version and a release, for example "
            "prgenv-gnu/24.11:v1"}};
    }
    if (auto r = format::check_relocatable(cfg); !r) {
        return unexpected{r.error()};
    }
    // 1. hash the file, and make the layer config and the layer manifest
    auto sqfs_digest = s.put_file(sqfs, progress);
    if (!sqfs_digest) {
        return unexpected{sqfs_digest.error()};
    }
    test_fault("add-after-squashfs");
    std::error_code ec;
    layer l;
    l.store.size = fs::file_size(s.blob_path(*sqfs_digest), ec);
    l.store.digest = *sqfs_digest;
    l.config = cfg.shared;
    l.mount = cfg.mount;
    l.relocatable = cfg.relocatable;
    l.views = cfg.views;
    l.default_view = cfg.default_view;
    auto lo = format::make_layer_objects(l);
    if (!lo) {
        return unexpected{lo.error()};
    }
    for (auto* b : {&lo->config, &lo->manifest}) {
        if (auto r = s.put_bytes(*b); !r) {
            return unexpected{r.error()};
        }
    }
    l.digest = lo->manifest.digest;
    l.manifest_size = lo->manifest.size();
    if (as_layer) {
        return s.record(lo->manifest.digest, true);
    }
    // 2. promote the layer to an image
    auto img = raise_layer(l, std::nullopt, false);
    if (!img) {
        return unexpected{img.error()};
    }
    img->is_layer = false;
    for (auto& [k, v] : image_meta) {
        img->config.meta[k] = v;
    }
    // 3. and 4.: store the blobs, then record the image
    return write_image(s, *img);
}

util::expected<found_object, std::string>
find_unique(std::vector<store>& stores, const label_query& q,
            const std::string& text) {
    std::vector<found_object> found;
    std::set<std::string> digests;
    for (std::size_t i = 0; i < stores.size(); ++i) {
        auto r = stores[i].query(q);
        if (!r) {
            return unexpected{r.error()};
        }
        for (auto& rec : *r) {
            if (digests.insert(rec.digest.string()).second) {
                found.push_back({i, rec});
            }
        }
    }
    if (found.empty()) {
        return unexpected{fmt::format("no {} matches '{}'",
                                      q.layer ? "layer" : "image", text)};
    }
    if (found.size() > 1u) {
        std::vector<std::string> lines;
        for (auto& f : found) {
            std::vector<std::string> meta;
            for (auto& [k, v] : f.record.cfg.meta) {
                meta.push_back(fmt::format("{}={}", k, v));
            }
            lines.push_back(fmt::format("  {}/{}:{} {} id={}", f.record.cfg.name,
                                        f.record.cfg.version,
                                        f.record.cfg.release,
                                        fmt::join(meta, ","), f.record.id));
        }
        return unexpected{fmt::format(
            "'{}' matches more than one {}: give the id\n{}", text,
            q.layer ? "layer" : "image", fmt::join(lines, "\n"))};
    }
    return found.front();
}

std::vector<std::string> compose_warnings(const composed_image& img) {
    std::vector<std::string> out;
    for (auto& l : img.layers) {
        if (l.mount == l.layer.mount) {
            continue;
        }
        // the old path can be the mount point of a different layer
        bool used = false;
        for (auto& o : img.layers) {
            if (o.mount == l.layer.mount) {
                used = true;
            }
        }
        if (used) {
            continue;
        }
        for (auto& [name, v] : img.views) {
            for (auto& var : format::patch_mentions(v.env, l.layer.mount)) {
                out.push_back(fmt::format(
                    "the image view '{}' sets {} with the path {}, but the "
                    "layer '{}' is now at {}",
                    name, var, l.layer.mount, l.alias, l.mount));
            }
        }
    }
    return out;
}

util::expected<object_record, std::string>
compose(store& s, const nlohmann::json& spec, const label_keys& keys,
        std::vector<std::string>* warnings) {
    if (!spec.is_object()) {
        return unexpected{std::string{"the compose file must be an object"}};
    }
    auto cfg_json = spec;
    cfg_json.erase("layers");
    auto ic = format::parse_image_config(cfg_json.dump());
    if (!ic) {
        return unexpected{ic.error()};
    }
    auto ls = spec.find("layers");
    if (ls == spec.end() || !ls->is_array() || ls->empty()) {
        return unexpected{
            std::string{"the compose file needs a list 'layers'"}};
    }

    std::vector<named_layer> layers;
    for (auto& e : *ls) {
        if (!e.is_object() || !e.contains("from") || !e["from"].is_string()) {
            return unexpected{
                std::string{"each layer needs a string 'from'"}};
        }
        const auto from = e["from"].get<std::string>();
        std::optional<layer> src;
        std::optional<std::string> source_alias;
        if (auto hash = from.find('#'); hash != std::string::npos) {
            auto q = parse_label_query(from.substr(0, hash), keys);
            if (!q) {
                return unexpected{q.error()};
            }
            auto r = s.query(*q);
            if (!r || r->size() != 1u) {
                return unexpected{fmt::format(
                    "the image '{}' is not unique, or not in the repository",
                    from.substr(0, hash))};
            }
            auto img = s.load(r->front().digest);
            if (!img) {
                return unexpected{img.error()};
            }
            auto alias = from.substr(hash + 1);
            for (auto& l : img->layers) {
                if (l.alias == alias) {
                    src = l.layer;
                    source_alias = alias;
                }
            }
            if (!src) {
                return unexpected{fmt::format(
                    "the image '{}' has no layer '{}'", from.substr(0, hash),
                    alias)};
            }
        } else {
            auto text = from.starts_with("+") ? from : "+" + from;
            auto q = parse_label_query(text, keys);
            if (!q) {
                return unexpected{q.error()};
            }
            auto r = s.query(*q);
            if (!r) {
                return unexpected{r.error()};
            }
            if (r->size() != 1u) {
                return unexpected{fmt::format(
                    "'{}' matches {} layers in the repository: it must match "
                    "one",
                    from, r->size())};
            }
            auto img = s.load(r->front().digest);
            if (!img) {
                return unexpected{img.error()};
            }
            src = img->layers.front().layer;
        }
        if (auto r = format::check_relocatable(format::layer_config_of(*src));
            !r) {
            return unexpected{r.error()};
        }
        named_layer nl{.alias = e.value("alias", src->config.name),
                       .mount = e.value("mount", src->mount),
                       .layer = *src};
        layers.push_back(std::move(nl));
    }
    auto img = make_composed_image(std::nullopt, ic->shared, std::move(layers),
                                   ic->views, ic->default_view);
    if (!img) {
        return unexpected{img.error()};
    }
    for (auto& w : compose_warnings(*img)) {
        spdlog::warn("{}", w);
        if (warnings) {
            warnings->push_back(w);
        }
    }
    return write_image(s, *img);
}

std::optional<oci::digest> merged_digest(const composed_image& img) {
    // the default views of a merge are a choice at start, one for each item,
    // and an image config holds only one: the identity leaves them out
    auto copy = img;
    copy.default_views.clear();
    return format::image_digest(copy);
}

util::expected<composed_image, std::string>
resolve_items(std::vector<store>& stores, const std::vector<item_spec>& items,
              bool force) {
    std::vector<merge_item> merge;
    for (auto& item : items) {
        merge_item m{.alias = item.alias, .mount = item.mount, .force = force};
        if (item.file) {
            auto img = raise_from_squashfs(*item.file, item.mount);
            if (!img) {
                return unexpected{img.error()};
            }
            m.image = std::move(*img);
            m.movable = true;
            // the mount point of a file is already applied
            m.mount = std::nullopt;
        } else {
            auto f = find_unique(stores, *item.query, item.text);
            if (!f) {
                return unexpected{f.error()};
            }
            auto img = stores[f->store_index].load(f->record.digest);
            if (!img) {
                return unexpected{img.error()};
            }
            m.image = std::move(*img);
            m.movable = item.query->layer;
            if (m.movable && items.size() == 1u && (item.mount || item.alias)) {
                // a layer started on its own: the mount point is an input
                // to the raise
                auto raised = raise_layer(m.image.layers.front().layer,
                                          item.mount, force, item.alias);
                if (!raised) {
                    return unexpected{raised.error()};
                }
                return raised;
            }
        }
        merge.push_back(std::move(m));
    }
    return merge_items(std::move(merge), merged_digest);
}

} // namespace uenv

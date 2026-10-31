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
#include <util/subprocess.h>

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
        if (auto eq = word.find('='); eq != std::string::npos) {
            auto alias = name::parse(word.substr(0, eq));
            if (!alias) {
                return unexpected{fmt::format("invalid item '{}': the alias {}",
                                              item.text, alias.error())};
            }
            item.alias = *alias;
            word = word.substr(eq + 1);
        }
        bool layer = !word.empty() && word[0] == '+';
        auto rest = layer ? word.substr(1) : word;
        auto desc = parse_uenv_description(rest);
        if (!desc) {
            return unexpected{fmt::format("invalid item '{}': {}", item.text,
                                          desc.error().message())};
        }
        if (auto m = desc->mount()) {
            auto mp = mount_point::parse(*m);
            if (!mp) {
                return unexpected{
                    fmt::format("invalid item '{}': {}", item.text, mp.error())};
            }
            item.mount = *mp;
        }
        if (auto file = desc->filename()) {
            item.file = fs::absolute(*file);
        } else if (auto label = desc->label()) {
            // keep the text of the label: re-parse it as a query
            auto label_text = rest;
            if (item.mount) {
                label_text = label_text.substr(
                    0, label_text.size() - item.mount->string().size() - 1);
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

util::expected<layer_config, std::string>
make_layer_config(const uenv::meta& m, const std::optional<v1_label>& label) {
    // the v1 data enters the new model here: parse each field
    auto field = [](std::string_view what, const std::string& text)
        -> util::expected<uenv::name, std::string> {
        auto n = uenv::name::parse(text);
        if (!n) {
            return unexpected{fmt::format("meta/env.json: the {} {}", what, n.error())};
        }
        return n;
    };
    auto mount = mount_point::parse(m.mount);
    if (!mount) {
        return unexpected{fmt::format("meta/env.json: {}", mount.error())};
    }
    auto n = field("name", m.name);
    if (!n) {
        return unexpected{n.error()};
    }
    config shared{.name = *n, .description = m.description.value_or("")};
    if (label) {
        auto version = field("version", label->version);
        if (!version) {
            return unexpected{version.error()};
        }
        auto release = field("release", label->release);
        if (!release) {
            return unexpected{release.error()};
        }
        shared.version = *version;
        shared.release = *release;
        if (label->uarch) {
            auto key = field("meta key", "uarch");
            if (!key) {
                return unexpected{key.error()};
            }
            shared.meta[*key] = *label->uarch;
        }
    }
    std::map<uenv::name, layer_view> views;
    for (auto& [vname, v] : m.views) {
        auto key = field("view name", vname);
        if (!key) {
            return unexpected{key.error()};
        }
        views[*key] =
            layer_view{.description = v.description, .env = v.environment};
    }
    std::optional<uenv::name> default_view;
    if (m.default_view) {
        auto d = field("default view", *m.default_view);
        if (!d) {
            return unexpected{d.error()};
        }
        default_view = *d;
    }
    return layer_config::make(std::move(shared), std::move(*mount), false,
                              std::move(views), std::move(default_view));
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
raise_from_squashfs(const fs::path& sqfs, std::optional<mount_point> mount) {
    std::error_code ec;
    if (!fs::is_regular_file(sqfs, ec)) {
        return unexpected{fmt::format("{} is not a file", sqfs.string())};
    }
    // the layer config: from meta/env.json, else a layer with no views at
    // the mount point that the user gives
    auto cfg = [&]() -> util::expected<layer_config, std::string> {
        auto m = read_squashfs_meta(sqfs);
        if (m) {
            auto c = v1::make_layer_config(*m, std::nullopt);
            if (!c) {
                return unexpected{
                    fmt::format("{}: {}", sqfs.string(), c.error())};
            }
            return c;
        }
        spdlog::warn("{}: the layer has no views", m.error());
        if (!mount) {
            return unexpected{fmt::format(
                "{} has no meta/env.json: give the mount point, for example "
                "{}:/user-environment",
                sqfs.string(), sqfs.string())};
        }
        // the name of the file, if it is a name
        auto n = uenv::name::parse(sqfs.stem().string());
        if (!n) {
            n = uenv::name::parse("unnamed");
        }
        if (!n) {
            return unexpected{n.error()};
        }
        return layer_config{.shared = {.name = *n}, .mount = *mount};
    }();
    if (!cfg) {
        return unexpected{cfg.error()};
    }
    if (mount && *mount != cfg->mount) {
        spdlog::warn("{} was made for the mount point {}, and is mounted at {}",
                     sqfs.string(), cfg->mount, *mount);
        cfg->mount = *mount;
    }
    // a squashfs file is not hashed: it has no digest and no layer manifest
    const auto size = fs::file_size(sqfs, ec);
    if (ec) {
        return unexpected{
            fmt::format("unable to read {}: {}", sqfs.string(), ec.message())};
    }
    storage store{.size = size, .file = sqfs};
    auto l = layer::make(std::move(*cfg), std::move(store));
    if (!l) {
        return unexpected{l.error()};
    }
    return raise_layer(*l, std::nullopt, false);
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
add_squashfs(store& s, const fs::path& sqfs, const layer_config& cfg,
             const std::map<uenv::name, std::string>& image_meta,
             bool as_layer,
             std::function<void(std::uint64_t, std::uint64_t)> progress) {
    if (!cfg.shared.release || !cfg.shared.version) {
        return unexpected{std::string{
            "a layer needs a name, a version and a release, for example "
            "prgenv-gnu/24.11:v1"}};
    }
    // 1. hash the file, and make the layer config and the layer manifest
    auto sqfs_digest = s.put_file(sqfs, progress);
    if (!sqfs_digest) {
        return unexpected{sqfs_digest.error()};
    }
    test_fault("add-after-squashfs");
    std::error_code ec;
    const auto size = fs::file_size(s.blob_path(*sqfs_digest), ec);
    if (ec) {
        return unexpected{fmt::format("unable to read {}: {}",
                                      s.blob_path(*sqfs_digest).string(),
                                      ec.message())};
    }
    storage store{.size = size, .digest = *sqfs_digest};
    auto lo = format::make_layer_objects(cfg, store);
    if (!lo) {
        return unexpected{lo.error()};
    }
    for (auto* b : {&lo->config, &lo->manifest}) {
        if (auto r = s.put_bytes(*b); !r) {
            return unexpected{r.error()};
        }
    }
    const auto l = layer::make(
        cfg, std::move(store),
        format::layer_descriptor(lo->manifest.digest, lo->manifest.size()));
    if (!l) {
        return unexpected{l.error()};
    }
    if (as_layer) {
        return s.record(lo->manifest.digest, true);
    }
    // 2. promote the layer to an image
    auto img = raise_layer(*l, std::nullopt, false);
    if (!img) {
        return unexpected{img.error()};
    }
    img->kind = uenv::image_kind::image;
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
            lines.push_back(fmt::format("  {} {} id={}",
                                        label_string(f.record.cfg),
                                        fmt::join(meta, ","), f.record.id));
        }
        return unexpected{fmt::format(
            "'{}' matches more than one {}: give the id\n{}", text,
            q.layer ? "layer" : "image", fmt::join(lines, "\n"))};
    }
    return found.front();
}

util::expected<void, std::string> check_nested_mounts(const composed_image& img) {
    // the layers are sorted, so each parent comes before the layers in it
    for (std::size_t i = 0; i < img.layers.size(); ++i) {
        auto& child = img.layers[i];
        const named_layer* parent = nullptr;
        for (std::size_t j = 0; j < i; ++j) {
            auto& p = img.layers[j];
            if (child.mount.is_inside(p.mount) &&
                (!parent || p.mount.is_inside(parent->mount))) {
                parent = &p;
            }
        }
        if (!parent || !parent->layer.store.file) {
            continue;
        }
        auto rel = child.mount.relative_to(parent->mount);
        auto proc = util::run({"unsquashfs", "-l", "-d", "", "-no-progress",
                               parent->layer.store.file->string(), rel});
        if (!proc) {
            spdlog::warn("unable to run unsquashfs: the mount point {} is not "
                         "checked",
                         child.mount);
            continue;
        }
        bool found = false;
        std::string line;
        while (std::getline(proc->out.stream(), line)) {
            if (line == "/" + rel) {
                found = true;
            }
        }
        if (proc->wait() != 0) {
            spdlog::warn("unsquashfs could not read {}: the mount point {} is "
                         "not checked",
                         parent->layer.store.file->string(), child.mount);
            continue;
        }
        if (!found) {
            return unexpected{fmt::format(
                "the layer '{}' is mounted at {}, inside the layer '{}' at {}, "
                "but '{}' has no directory {}",
                child.alias, child.mount, parent->alias, parent->mount,
                parent->alias, "/" + rel)};
        }
    }
    return {};
}

util::expected<object_record, std::string>
compose(store& s, const nlohmann::json& spec, const label_keys& keys) {
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
            auto alias = uenv::name::parse(from.substr(hash + 1));
            if (!alias) {
                return unexpected{fmt::format(
                    "the compose file: the layer '{}': the alias {}", from,
                    alias.error())};
            }
            for (auto& l : img->layers) {
                if (l.alias == *alias) {
                    src = l.layer;
                }
            }
            if (!src) {
                return unexpected{fmt::format(
                    "the image '{}' has no layer '{}'", from.substr(0, hash),
                    *alias)};
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
        auto mount = src->mount;
        if (auto m = e.find("mount"); m != e.end()) {
            auto mp = m->is_string() ? mount_point::parse(m->get<std::string>())
                                     : util::unexpected{std::string{
                                           "'mount' must be a string"}};
            if (!mp) {
                return unexpected{
                    fmt::format("the compose file: {}", mp.error())};
            }
            mount = *mp;
        }
        auto alias = src->config.name;
        if (auto a = e.find("alias"); a != e.end()) {
            auto n = a->is_string()
                         ? uenv::name::parse(a->get<std::string>())
                         : util::unexpected{std::string{"'alias' must be a string"}};
            if (!n) {
                return unexpected{fmt::format(
                    "the compose file: the layer '{}': the alias {}", from,
                    n.error())};
            }
            alias = *n;
        }
        named_layer nl{.alias = alias, .mount = mount, .layer = *src};
        layers.push_back(std::move(nl));
    }
    auto img = make_composed_image(std::nullopt, ic->shared, std::move(layers),
                                   ic->views, ic->default_view);
    if (!img) {
        return unexpected{img.error()};
    }
    if (auto r = check_nested_mounts(*img); !r) {
        return unexpected{r.error()};
    }
    return write_image(s, *img);
}

util::expected<composed_image, std::string>
resolve_items(std::vector<store>& stores, const std::vector<item_spec>& items,
              bool force) {
    std::vector<runtime_merge_item> merge;
    for (auto& item : items) {
        if (item.file) {
            auto img = raise_from_squashfs(*item.file, item.mount);
            if (!img) {
                return unexpected{img.error()};
            }
            // the mount point of a file is already applied
            merge.push_back({.alias = item.alias,
                             .image = std::move(*img),
                             .movable = true,
                             .force = force});
            continue;
        }
        auto f = find_unique(stores, *item.query, item.text);
        if (!f) {
            return unexpected{f.error()};
        }
        auto img = stores[f->store_index].load(f->record.digest);
        if (!img) {
            return unexpected{img.error()};
        }
        const bool movable = item.query->layer;
        if (movable && items.size() == 1u && (item.mount || item.alias)) {
            // a layer started on its own: the mount point is an input to the
            // raise
            return raise_layer(img->layers.front().layer, item.mount, force,
                               item.alias);
        }
        merge.push_back({.alias = item.alias,
                         .mount = item.mount,
                         .image = std::move(*img),
                         .movable = movable,
                         .force = force});
    }
    return runtime_merge(std::move(merge));
}

} // namespace uenv

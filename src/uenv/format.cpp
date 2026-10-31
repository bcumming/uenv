#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <oci/manifest.h>
#include <uenv/format.h>
#include <uenv/model.h>
#include <uenv/parse.h>
#include <util/jcs.h>
#include <util/sha.h>
#include <util/strings.h>

namespace uenv::format {

using json = nlohmann::json;
using util::unexpected;

blob make_blob(std::string bytes) {
    auto d = oci::digest::sha256(util::sha256_string(bytes));
    return blob{std::move(bytes), d};
}

oci::digest empty_config_digest() {
    return make_blob(std::string(oci::empty_config_body)).digest;
}

oci::descriptor layer_descriptor(const oci::digest& digest, std::size_t size) {
    return oci::descriptor{.media_type = std::string(oci::media_type_manifest),
                           .digest = digest,
                           .size = size,
                           .artifact_type = std::string(at_layer)};
}

util::expected<std::string, error> canonical(const json& j) {
    return util::jcs(j);
}

//
// media types
//

namespace {

struct parsed_media_type {
    std::string type;
    unsigned version;
};

// "application/vnd.uenv.<type>.v<N>[+json]"
std::optional<parsed_media_type> parse_uenv_media_type(std::string_view mt) {
    constexpr std::string_view prefix = "application/vnd.uenv.";
    if (!mt.starts_with(prefix)) {
        return std::nullopt;
    }
    mt.remove_prefix(prefix.size());
    if (auto plus = mt.find('+'); plus != std::string_view::npos) {
        mt = mt.substr(0, plus);
    }
    auto dot = mt.rfind(".v");
    if (dot == std::string_view::npos || dot + 2 >= mt.size()) {
        return std::nullopt;
    }
    auto num = mt.substr(dot + 2);
    unsigned version = 0;
    auto [end, ec] = std::from_chars(num.data(), num.data() + num.size(), version);
    if (ec != std::errc{} || end != num.data() + num.size()) {
        return std::nullopt;
    }
    return parsed_media_type{std::string(mt.substr(0, dot)), version};
}

} // namespace

util::expected<void, error> check_media_type(std::string_view found,
                                             std::string_view expected) {
    if (found == expected) {
        return {};
    }
    auto f = parse_uenv_media_type(found);
    auto e = parse_uenv_media_type(expected);
    if (f && e && f->type == e->type) {
        return unexpected{fmt::format(
            "this image uses vnd.uenv.{}.v{}, and this version of uenv reads "
            "only vnd.uenv.{}.v{}. Use a later version of uenv.",
            f->type, f->version, e->type, e->version)};
    }
    return unexpected{fmt::format("unexpected media type '{}': expected '{}'",
                                  found, expected)};
}

//
// helpers for strict parsing
//

namespace {

util::expected<std::string, error> req_string(const json& j,
                                              std::string_view key,
                                              std::string_view what) {
    auto it = j.find(key);
    if (it == j.end()) {
        return unexpected{
            fmt::format("{}: the required field '{}' is missing", what, key)};
    }
    if (!it->is_string()) {
        return unexpected{
            fmt::format("{}: the field '{}' must be a string", what, key)};
    }
    return it->get<std::string>();
}

util::expected<std::optional<std::string>, error>
opt_string(const json& j, std::string_view key, std::string_view what) {
    auto it = j.find(key);
    if (it == j.end()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        return unexpected{
            fmt::format("{}: the field '{}' must be a string", what, key)};
    }
    return it->get<std::string>();
}

util::expected<oci::descriptor, error> parse_descriptor(const json& j,
                                                        std::string_view what) {
    if (!j.is_object()) {
        return unexpected{fmt::format("{}: a descriptor must be an object", what)};
    }
    auto mt = req_string(j, "mediaType", what);
    if (!mt) {
        return unexpected{mt.error()};
    }
    auto ds = req_string(j, "digest", what);
    if (!ds) {
        return unexpected{ds.error()};
    }
    auto d = oci::digest::parse(*ds);
    if (!d) {
        return unexpected{fmt::format("{}: invalid digest '{}'", what, *ds)};
    }
    auto sz = j.find("size");
    if (sz == j.end() || !sz->is_number_unsigned()) {
        return unexpected{
            fmt::format("{}: the field 'size' must be an unsigned integer", what)};
    }
    oci::descriptor desc{.media_type = *mt, .digest = *d,
                         .size = sz->get<std::size_t>()};
    if (auto at = opt_string(j, "artifactType", what); !at) {
        return unexpected{at.error()};
    } else {
        desc.artifact_type = *at;
    }
    if (auto data = opt_string(j, "data", what); !data) {
        return unexpected{data.error()};
    } else if (*data) {
        // inline content: base64 of exactly the bytes of the digest
        auto bytes = util::base64_decode(**data);
        if (!bytes) {
            return unexpected{
                fmt::format("{}: the 'data' of a descriptor is not base64", what)};
        }
        if (bytes->size() != desc.size || make_blob(*bytes).digest != desc.digest) {
            return unexpected{fmt::format(
                "{}: the 'data' of a descriptor does not match its digest {}",
                what, desc.digest.string())};
        }
        desc.data = *data;
    }
    return desc;
}

json descriptor_json(std::string_view media_type, const oci::digest& d,
                     std::uint64_t size) {
    return json{{"mediaType", media_type}, {"digest", d.string()}, {"size", size}};
}

util::expected<json, error> parse_json_object(std::string_view body,
                                              std::string_view what) {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        return unexpected{fmt::format("{}: invalid JSON", what)};
    }
    if (!j.is_object()) {
        return unexpected{fmt::format("{}: must be a JSON object", what)};
    }
    return j;
}

// a required field that is a name
util::expected<name, error> req_name(const json& j, std::string_view key,
                                     std::string_view what) {
    auto v = req_string(j, key, what);
    if (!v) {
        return unexpected{v.error()};
    }
    auto n = name::parse(*v);
    if (!n) {
        return unexpected{fmt::format("{}: the field '{}': {}", what, key, n.error())};
    }
    return *n;
}

util::expected<config, error> parse_shared(const json& j, std::string_view what) {
    auto n = req_name(j, "name", what);
    if (!n) {
        return unexpected{n.error()};
    }
    auto version = req_name(j, "version", what);
    if (!version) {
        return unexpected{version.error()};
    }
    auto release = req_name(j, "release", what);
    if (!release) {
        return unexpected{release.error()};
    }
    auto d = opt_string(j, "description", what);
    if (!d) {
        return unexpected{d.error()};
    }
    config c{.name = *n,
             .version = *version,
             .release = *release,
             .description = d->value_or("")};
    auto m = j.find("meta");
    if (m == j.end()) {
        return unexpected{
            fmt::format("{}: the required field 'meta' is missing", what)};
    }
    if (!m->is_object()) {
        return unexpected{fmt::format("{}: 'meta' must be an object", what)};
    }
    for (auto it = m->begin(); it != m->end(); ++it) {
        if (!it->is_string()) {
            return unexpected{fmt::format(
                "{}: the value of meta '{}' must be a string", what, it.key())};
        }
        auto key = name::parse(it.key());
        if (!key) {
            return unexpected{
                fmt::format("{}: the meta key {}", what, key.error())};
        }
        c.meta[*key] = it->get<std::string>();
    }
    return c;
}

// a stored config has a version and a release: the writers check it
// (check_storable) before they call this
void write_shared(json& j, const config& c) {
    j["name"] = c.name.string();
    if (c.version) {
        j["version"] = c.version->string();
    }
    if (c.release) {
        j["release"] = c.release->string();
    }
    if (!c.description.empty()) {
        j["description"] = c.description;
    }
    j["meta"] = json::object();
    for (auto& [k, v] : c.meta) {
        j["meta"][k.string()] = v;
    }
}

} // namespace

//
// the environment grammar
//

json env_to_json(const envvars::patch& p) {
    json out = json::object();
    if (!p.prefix_paths().empty()) {
        json list = json::object();
        for (auto& [name, pp] : p.prefix_paths()) {
            json updates = json::array();
            for (auto& u : pp.updates()) {
                updates.push_back(json{{"op", fmt::format("{}", u.op)},
                                       {"value", u.values}});
            }
            list[name] = updates;
        }
        out["list"] = list;
    }
    if (!p.scalars().empty()) {
        json scalar = json::object();
        for (auto& [name, s] : p.scalars()) {
            scalar[name] = s.value ? json(*s.value) : json(nullptr);
        }
        out["scalar"] = scalar;
    }
    return out;
}

util::expected<envvars::patch, error>
env_from_json(const json& j, std::vector<std::string>* warnings) {
    envvars::patch p;
    if (!j.is_object()) {
        return unexpected{error{"env: must be an object"}};
    }
    // uenv sets only variables with a valid name ("Variables in a value")
    auto check_name = [](const std::string& n) -> util::expected<void, error> {
        const bool valid =
            !n.empty() && !std::isdigit(static_cast<unsigned char>(n[0])) &&
            std::all_of(n.begin(), n.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
        if (!valid) {
            return unexpected{fmt::format(
                "env: '{}' is not a valid variable name: a name has the form "
                "[A-Za-z_][A-Za-z0-9_]*",
                n)};
        }
        return {};
    };
    if (auto s = j.find("scalar"); s != j.end()) {
        if (!s->is_object()) {
            return unexpected{error{"env: 'scalar' must be an object"}};
        }
        for (auto it = s->begin(); it != s->end(); ++it) {
            if (auto r = check_name(it.key()); !r) {
                return unexpected{r.error()};
            }
            if (it->is_null()) {
                p.update_scalar(it.key(), std::nullopt);
            } else if (it->is_string()) {
                p.update_scalar(it.key(), it->get<std::string>());
            } else {
                return unexpected{fmt::format(
                    "env: the value of '{}' must be a string or null", it.key())};
            }
        }
    }
    if (auto l = j.find("list"); l != j.end()) {
        if (!l->is_object()) {
            return unexpected{error{"env: 'list' must be an object"}};
        }
        for (auto it = l->begin(); it != l->end(); ++it) {
            if (auto r = check_name(it.key()); !r) {
                return unexpected{r.error()};
            }
            if (!it->is_array()) {
                return unexpected{fmt::format(
                    "env: the updates of '{}' must be a list", it.key())};
            }
            for (auto& u : *it) {
                if (!u.is_object()) {
                    return unexpected{fmt::format(
                        "env: an update of '{}' must be an object", it.key())};
                }
                auto op = req_string(u, "op", "env");
                if (!op) {
                    return unexpected{op.error()};
                }
                envvars::update_kind kind;
                if (*op == "set") {
                    kind = envvars::update_kind::set;
                } else if (*op == "prepend") {
                    kind = envvars::update_kind::prepend;
                } else if (*op == "append") {
                    kind = envvars::update_kind::append;
                } else if (*op == "unset") {
                    kind = envvars::update_kind::unset;
                } else {
                    return unexpected{
                        fmt::format("env: unknown operation '{}'", *op)};
                }
                std::vector<std::string> values;
                if (auto v = u.find("value"); v != u.end()) {
                    if (v->is_string()) {
                        values.push_back(v->get<std::string>());
                    } else if (v->is_array() &&
                               std::all_of(v->begin(), v->end(),
                                           [](auto& x) { return x.is_string(); })) {
                        values = v->get<std::vector<std::string>>();
                    } else if (!v->is_null()) {
                        return unexpected{fmt::format(
                            "env: the value of an update of '{}' must be a list "
                            "of strings",
                            it.key())};
                    }
                }
                if (p.update_prefix_path(it.key(), {kind, values})) {
                    auto msg = fmt::format(
                        "env: '{}' is both a value and a prefix path in one "
                        "view: the prefix path is used",
                        it.key());
                    spdlog::warn("{}", msg);
                    if (warnings) {
                        warnings->push_back(msg);
                    }
                }
            }
        }
    }
    return p;
}

//
// the configs
//

json to_json(const layer_config& c) {
    json j = json::object();
    write_shared(j, c.shared);
    j["mount"] = c.mount.string();
    j["relocatable"] = c.relocatable;
    j["views"] = json::object();
    for (auto& [name, v] : c.views) {
        json jv = json::object();
        if (!v.description.empty()) {
            jv["description"] = v.description;
        }
        jv["env"] = env_to_json(v.env);
        j["views"][name.string()] = jv;
    }
    if (c.default_view) {
        j["default_view"] = c.default_view->string();
    }
    return j;
}

json to_json(const image_config& c) {
    json j = json::object();
    write_shared(j, c.shared);
    j["views"] = json::object();
    for (auto& [name, v] : c.views) {
        json jv = json::object();
        if (!v.description.empty()) {
            jv["description"] = v.description;
        }
        if (!v.include.empty()) {
            json inc = json::array();
            for (auto& r : v.include) {
                inc.push_back(format_as(r));
            }
            jv["include"] = inc;
        }
        auto env = env_to_json(v.env);
        if (!env.empty()) {
            jv["env"] = env;
        }
        j["views"][name.string()] = jv;
    }
    if (c.default_view) {
        j["default_view"] = c.default_view->string();
    }
    return j;
}

namespace {

util::expected<std::optional<name>, error>
parse_default_view(const json& j, std::string_view what,
                   const std::function<bool(const name&)>& exists) {
    auto d = opt_string(j, "default_view", what);
    if (!d) {
        return unexpected{d.error()};
    }
    if (!*d) {
        return std::optional<name>{};
    }
    auto n = name::parse(**d);
    if (!n) {
        return unexpected{
            fmt::format("{}: the default view {}", what, n.error())};
    }
    if (!exists(*n)) {
        return unexpected{fmt::format(
            "{}: the default view '{}' is not a view", what, **d)};
    }
    return std::optional<name>{*n};
}

} // namespace

util::expected<layer_config, error> parse_layer_config(std::string_view body) {
    constexpr std::string_view what = "layer config";
    auto j = parse_json_object(body, what);
    if (!j) {
        return unexpected{j.error()};
    }
    auto shared = parse_shared(*j, what);
    if (!shared) {
        return unexpected{shared.error()};
    }
    auto mount = req_string(*j, "mount", what);
    if (!mount) {
        return unexpected{mount.error()};
    }
    auto mp = mount_point::parse(*mount);
    if (!mp) {
        return unexpected{fmt::format("{}: {}", what, mp.error())};
    }
    auto rel = j->find("relocatable");
    if (rel == j->end()) {
        return unexpected{
            fmt::format("{}: the required field 'relocatable' is missing", what)};
    }
    if (!rel->is_boolean()) {
        return unexpected{fmt::format("{}: 'relocatable' must be a bool", what)};
    }
    const bool relocatable = rel->get<bool>();
    auto views = j->find("views");
    if (views == j->end() || !views->is_object()) {
        return unexpected{
            fmt::format("{}: the required field 'views' must be an object", what)};
    }
    std::map<name, layer_view> layer_views;
    for (auto it = views->begin(); it != views->end(); ++it) {
        if (!it->is_object()) {
            return unexpected{
                fmt::format("{}: the view '{}' must be an object", what, it.key())};
        }
        layer_view v;
        auto d = opt_string(*it, "description", what);
        if (!d) {
            return unexpected{d.error()};
        }
        v.description = d->value_or("");
        if (auto e = it->find("env"); e != it->end()) {
            auto env = env_from_json(*e);
            if (!env) {
                return unexpected{fmt::format("{}: view '{}': {}", what,
                                              it.key(), env.error())};
            }
            v.env = *env;
        }
        auto vname = name::parse(it.key());
        if (!vname) {
            return unexpected{
                fmt::format("{}: the view name {}", what, vname.error())};
        }
        layer_views[*vname] = std::move(v);
    }
    auto dv = parse_default_view(*j, what, [&](const name& n) {
        return layer_views.contains(n);
    });
    if (!dv) {
        return unexpected{dv.error()};
    }
    return layer_config{.shared = std::move(*shared),
                        .mount = std::move(*mp),
                        .relocatable = relocatable,
                        .views = std::move(layer_views),
                        .default_view = std::move(*dv)};
}

util::expected<image_config, error> parse_image_config(std::string_view body) {
    constexpr std::string_view what = "image config";
    auto j = parse_json_object(body, what);
    if (!j) {
        return unexpected{j.error()};
    }
    auto shared = parse_shared(*j, what);
    if (!shared) {
        return unexpected{shared.error()};
    }
    std::map<name, image_view> image_views;
    auto views = j->find("views");
    if (views == j->end() || !views->is_object()) {
        return unexpected{
            fmt::format("{}: the required field 'views' must be an object", what)};
    }
    for (auto it = views->begin(); it != views->end(); ++it) {
        if (!it->is_object()) {
            return unexpected{
                fmt::format("{}: the view '{}' must be an object", what, it.key())};
        }
        image_view v;
        auto d = opt_string(*it, "description", what);
        if (!d) {
            return unexpected{d.error()};
        }
        v.description = d->value_or("");
        if (auto inc = it->find("include"); inc != it->end()) {
            if (!inc->is_array() ||
                !std::all_of(inc->begin(), inc->end(),
                             [](auto& x) { return x.is_string(); })) {
                return unexpected{fmt::format(
                    "{}: the view '{}': 'include' must be a list of strings",
                    what, it.key())};
            }
            for (auto& x : *inc) {
                auto r = parse_layer_view_ref(x.get<std::string>());
                if (!r) {
                    return unexpected{fmt::format("{}: the view '{}': {}", what,
                                                  it.key(), r.error())};
                }
                v.include.push_back(*r);
            }
        }
        if (auto e = it->find("env"); e != it->end()) {
            auto env = env_from_json(*e);
            if (!env) {
                return unexpected{fmt::format("{}: view '{}': {}", what,
                                              it.key(), env.error())};
            }
            v.env = *env;
        }
        auto vname = name::parse(it.key());
        if (!vname) {
            return unexpected{
                fmt::format("{}: the view name {}", what, vname.error())};
        }
        image_views[*vname] = std::move(v);
    }
    auto dv = parse_default_view(*j, what, [&](const name& n) {
        return image_views.contains(n);
    });
    if (!dv) {
        return unexpected{dv.error()};
    }
    return image_config{.shared = std::move(*shared),
                        .views = std::move(image_views),
                        .default_view = std::move(*dv)};
}

//
// manifests and the index
//

util::expected<blob, error> make_layer_manifest(const blob& config,
                                                const oci::digest& sqfs,
                                                std::uint64_t sqfs_size) {
    json j{{"schemaVersion", 2},
           {"mediaType", oci::media_type_manifest},
           {"artifactType", at_layer},
           {"config", descriptor_json(mt_layer_config, config.digest, config.size())},
           {"layers", json::array({descriptor_json(mt_squashfs, sqfs, sqfs_size)})}};
    auto bytes = canonical(j);
    if (!bytes) {
        return unexpected{bytes.error()};
    }
    return make_blob(*bytes);
}

util::expected<blob, error> make_wrapper_manifest(const blob& image_config) {
    auto empty = descriptor_json(oci::media_type_empty, empty_config_digest(), 2);
    empty["data"] = oci::empty_config_data;
    json j{{"schemaVersion", 2},
           {"mediaType", oci::media_type_manifest},
           {"artifactType", at_wrapper},
           {"config", descriptor_json(mt_image_config, image_config.digest,
                                      image_config.size())},
           {"layers", json::array({empty})}};
    auto bytes = canonical(j);
    if (!bytes) {
        return unexpected{bytes.error()};
    }
    return make_blob(*bytes);
}

util::expected<blob, error> make_index(const blob& wrapper,
                                       std::vector<index_entry> layers) {
    std::stable_sort(layers.begin(), layers.end(),
                     [](auto& a, auto& b) { return a.mount < b.mount; });
    json manifests = json::array();
    auto w = descriptor_json(oci::media_type_manifest, wrapper.digest,
                             wrapper.size());
    w["artifactType"] = at_wrapper;
    manifests.push_back(w);
    for (auto& e : layers) {
        auto d = descriptor_json(oci::media_type_manifest, e.manifest.digest,
                                 e.manifest.size);
        d["artifactType"] = at_layer;
        d["annotations"] =
            json{{ann_alias, e.alias.string()}, {ann_mount, e.mount.string()}};
        manifests.push_back(d);
    }
    json j{{"schemaVersion", 2},
           {"mediaType", oci::media_type_index},
           {"artifactType", at_image},
           {"manifests", manifests}};
    auto bytes = canonical(j);
    if (!bytes) {
        return unexpected{bytes.error()};
    }
    return make_blob(*bytes);
}

namespace {

util::expected<json, error> parse_manifest_json(std::string_view body,
                                                std::string_view what,
                                                std::string_view artifact_type) {
    auto j = parse_json_object(body, what);
    if (!j) {
        return unexpected{j.error()};
    }
    auto mt = req_string(*j, "mediaType", what);
    if (!mt) {
        return unexpected{mt.error()};
    }
    if (*mt != oci::media_type_manifest) {
        return unexpected{fmt::format("{}: the mediaType '{}' is not an OCI "
                                      "image manifest",
                                      what, *mt)};
    }
    auto at = req_string(*j, "artifactType", what);
    if (!at) {
        return unexpected{at.error()};
    }
    if (auto r = check_media_type(*at, artifact_type); !r) {
        return unexpected{fmt::format("{}: {}", what, r.error())};
    }
    return j;
}

} // namespace

util::expected<layer_manifest, error> parse_layer_manifest(std::string_view body) {
    constexpr std::string_view what = "layer manifest";
    auto j = parse_manifest_json(body, what, at_layer);
    if (!j) {
        return unexpected{j.error()};
    }
    auto cfg = j->find("config");
    if (cfg == j->end()) {
        return unexpected{fmt::format("{}: no config", what)};
    }
    auto config = parse_descriptor(*cfg, what);
    if (!config) {
        return unexpected{config.error()};
    }
    if (auto r = check_media_type(config->media_type, mt_layer_config); !r) {
        return unexpected{fmt::format("{}: {}", what, r.error())};
    }
    auto layers = j->find("layers");
    if (layers == j->end() || !layers->is_array() || layers->size() != 1u) {
        return unexpected{
            fmt::format("{}: must have exactly one entry in 'layers'", what)};
    }
    auto sqfs = parse_descriptor(layers->at(0), what);
    if (!sqfs) {
        return unexpected{sqfs.error()};
    }
    if (auto r = check_media_type(sqfs->media_type, mt_squashfs); !r) {
        return unexpected{fmt::format("{}: {}", what, r.error())};
    }
    return layer_manifest{*config, *sqfs};
}

util::expected<wrapper_manifest, error>
parse_wrapper_manifest(std::string_view body) {
    constexpr std::string_view what = "wrapper manifest";
    auto j = parse_manifest_json(body, what, at_wrapper);
    if (!j) {
        return unexpected{j.error()};
    }
    auto cfg = j->find("config");
    if (cfg == j->end()) {
        return unexpected{fmt::format("{}: no config", what)};
    }
    auto config = parse_descriptor(*cfg, what);
    if (!config) {
        return unexpected{config.error()};
    }
    if (auto r = check_media_type(config->media_type, mt_image_config); !r) {
        return unexpected{fmt::format("{}: {}", what, r.error())};
    }
    return wrapper_manifest{*config};
}

util::expected<image_index, error> parse_index(std::string_view body) {
    constexpr std::string_view what = "image index";
    auto j = parse_json_object(body, what);
    if (!j) {
        return unexpected{j.error()};
    }
    auto mt = req_string(*j, "mediaType", what);
    if (!mt) {
        return unexpected{mt.error()};
    }
    if (*mt != oci::media_type_index) {
        return unexpected{
            fmt::format("{}: the mediaType '{}' is not an OCI index", what, *mt)};
    }
    auto at = req_string(*j, "artifactType", what);
    if (!at) {
        return unexpected{at.error()};
    }
    if (auto r = check_media_type(*at, at_image); !r) {
        return unexpected{fmt::format("{}: {}", what, r.error())};
    }
    auto ms = j->find("manifests");
    if (ms == j->end() || !ms->is_array() || ms->empty()) {
        return unexpected{fmt::format("{}: 'manifests' is empty", what)};
    }
    std::optional<oci::descriptor> wrapper;
    std::vector<index_entry> layer_entries;
    bool have_wrapper = false;
    for (std::size_t i = 0; i < ms->size(); ++i) {
        auto d = parse_descriptor(ms->at(i), what);
        if (!d) {
            return unexpected{d.error()};
        }
        if (d->media_type == oci::media_type_index) {
            return unexpected{fmt::format(
                "{}: entry {} is an index: an image has a depth of two", what, i)};
        }
        if (d->media_type != oci::media_type_manifest) {
            return unexpected{fmt::format(
                "{}: entry {} has the media type '{}'", what, i, d->media_type)};
        }
        if (!d->artifact_type) {
            return unexpected{
                fmt::format("{}: entry {} has no artifactType", what, i)};
        }
        auto wr = check_media_type(*d->artifact_type, at_wrapper);
        auto la = check_media_type(*d->artifact_type, at_layer);
        bool is_wrapper_type =
            parse_uenv_media_type(*d->artifact_type) &&
            parse_uenv_media_type(*d->artifact_type)->type == "image-config";
        if (is_wrapper_type) {
            if (!wr) {
                return unexpected{fmt::format("{}: {}", what, wr.error())};
            }
            if (have_wrapper) {
                return unexpected{
                    fmt::format("{}: the index has more than one wrapper", what)};
            }
            if (i != 0) {
                return unexpected{fmt::format(
                    "{}: the wrapper is entry {}, not the first entry", what, i)};
            }
            have_wrapper = true;
            wrapper = *d;
            continue;
        }
        if (i == 0) {
            return unexpected{
                fmt::format("{}: the first entry is not the wrapper", what)};
        }
        if (!la) {
            return unexpected{fmt::format("{}: entry {}: {}", what, i, la.error())};
        }
        auto& entry = ms->at(i);
        auto ann = entry.find("annotations");
        if (ann == entry.end() || !ann->is_object()) {
            return unexpected{
                fmt::format("{}: layer entry {} has no annotations", what, i)};
        }
        auto alias = req_name(*ann, ann_alias, fmt::format("{}: entry {}", what, i));
        if (!alias) {
            return unexpected{alias.error()};
        }
        auto mount = req_string(*ann, ann_mount, fmt::format("{}: entry {}", what, i));
        if (!mount) {
            return unexpected{mount.error()};
        }
        auto mp = mount_point::parse(*mount);
        if (!mp) {
            return unexpected{fmt::format("{}: entry {}: {}", what, i, mp.error())};
        }
        layer_entries.push_back({*d, *alias, *mp});
    }
    if (layer_entries.empty()) {
        return unexpected{fmt::format("{}: the index has no layers", what)};
    }
    return image_index{*wrapper, std::move(layer_entries)};
}

util::expected<object_kind, error> top_level_kind(std::string_view body) {
    auto j = parse_json_object(body, "top-level object");
    if (!j) {
        return unexpected{j.error()};
    }
    auto mt = j->find("mediaType");
    if (mt == j->end() || !mt->is_string()) {
        return unexpected{error{"the object has no mediaType"}};
    }
    if (*mt == oci::media_type_index) {
        return object_kind::image;
    }
    if (*mt == oci::media_type_manifest) {
        auto at = j->find("artifactType");
        if (at != j->end() && at->is_string()) {
            auto p = parse_uenv_media_type(at->get<std::string>());
            if (p && p->type == "layer") {
                return object_kind::layer;
            }
            if (p && p->type == "image-config") {
                return unexpected{error{
                    "the object is the wrapper of an image, not an image: "
                    "use the digest of the index"}};
            }
        }
        return unexpected{error{"the manifest is not a uenv layer"}};
    }
    return unexpected{fmt::format("the media type '{}' is not a uenv image or "
                                  "layer",
                                  mt->get<std::string>())};
}

//
// load
//

util::expected<layer, error> load_layer(const oci::descriptor& desc,
                                        const blob_reader& read,
                                        const file_locator& locate) {
    auto body = read(desc);
    if (!body) {
        return unexpected{body.error()};
    }
    auto j = json::parse(*body, nullptr, false);
    if (desc.artifact_type && j.is_object()) {
        auto at = j.find("artifactType");
        if (at == j.end() || !at->is_string() ||
            at->get<std::string>() != *desc.artifact_type) {
            return unexpected{fmt::format(
                "the artifactType hint '{}' of the layer {} does not agree "
                "with the layer manifest",
                *desc.artifact_type, desc.digest.string())};
        }
    }
    auto m = parse_layer_manifest(*body);
    if (!m) {
        return unexpected{m.error()};
    }
    auto cbody = read(m->config);
    if (!cbody) {
        return unexpected{cbody.error()};
    }
    auto c = parse_layer_config(*cbody);
    if (!c) {
        return unexpected{
            fmt::format("{}: {}", desc.digest.string(), c.error())};
    }
    storage store{.size = m->squashfs.size, .digest = m->squashfs.digest};
    if (locate) {
        store.file = locate(m->squashfs.digest);
    }
    return layer::make(std::move(*c), std::move(store),
                       layer_descriptor(desc.digest, body->size()));
}

util::expected<composed_image, error>
load_composed_image(const oci::digest& digest, const std::string& body,
                    const blob_reader& read, const file_locator& locate) {
    auto kind = top_level_kind(body);
    if (!kind) {
        return unexpected{kind.error()};
    }
    if (*kind == object_kind::layer) {
        blob_reader with_top = [&](const oci::descriptor& d)
            -> util::expected<std::string, error> {
            if (d.digest == digest) {
                return body;
            }
            return read(d);
        };
        oci::descriptor top{.media_type = std::string(oci::media_type_manifest),
                            .digest = digest,
                            .size = body.size()};
        auto l = load_layer(top, with_top, locate);
        if (!l) {
            return unexpected{l.error()};
        }
        return raise_layer(*l, std::nullopt, false);
    }

    auto idx = parse_index(body);
    if (!idx) {
        return unexpected{idx.error()};
    }
    auto wbody = read(idx->wrapper);
    if (!wbody) {
        return unexpected{wbody.error()};
    }
    {
        auto j = json::parse(*wbody, nullptr, false);
        auto at = j.is_object() ? j.find("artifactType") : j.end();
        if (at == j.end() || !at->is_string() ||
            at->get<std::string>() != *idx->wrapper.artifact_type) {
            return unexpected{error{"the artifactType hint of the wrapper does "
                                    "not agree with the wrapper manifest"}};
        }
    }
    auto w = parse_wrapper_manifest(*wbody);
    if (!w) {
        return unexpected{w.error()};
    }
    auto cbody = read(w->config);
    if (!cbody) {
        return unexpected{cbody.error()};
    }
    auto ic = parse_image_config(*cbody);
    if (!ic) {
        return unexpected{ic.error()};
    }
    std::vector<named_layer> layers;
    std::map<std::string, layer> cache;
    for (auto& e : idx->layers) {
        auto key = e.manifest.digest.string();
        if (!cache.contains(key)) {
            auto l = load_layer(e.manifest, read, locate);
            if (!l) {
                return unexpected{l.error()};
            }
            cache.emplace(key, std::move(*l));
        }
        layers.push_back({e.alias, e.mount, cache.at(key)});
    }
    return make_composed_image(digest, ic->shared, std::move(layers), ic->views,
                               ic->default_view);
}

//
// write
//

image_config image_config_of(const composed_image& img) {
    // only an image or a raised layer: the views have no item, and there is
    // at most one default view (make_image_objects refuses a runtime merge)
    image_config c{.shared = img.config};
    for (auto& [name, v] : img.views) {
        c.views[name.name] = v;
    }
    if (!img.default_views.empty()) {
        c.default_view = img.default_views.front().name;
    }
    return c;
}

namespace {

// a stored config has a version and a release
util::expected<void, error> check_storable(const config& c, std::string_view what) {
    if (!c.version || !c.release) {
        return unexpected{fmt::format(
            "the {} {} has no version and release, and cannot be stored: give "
            "a label, for example {}/1.0:v1",
            what, c.name, c.name)};
    }
    return {};
}

} // namespace

util::expected<image_objects, error> make_image_objects(const composed_image& img) {
    if (img.kind == image_kind::runtime_merge) {
        return unexpected{error{"a runtime merge cannot be stored: it has no "
                                "image config. Write an image config that "
                                "contains the layers, and compose it."}};
    }
    if (auto r = check_storable(img.config, "image"); !r) {
        return unexpected{r.error()};
    }
    auto cfg_bytes = canonical(to_json(image_config_of(img)));
    if (!cfg_bytes) {
        return unexpected{cfg_bytes.error()};
    }
    auto cfg = make_blob(*cfg_bytes);
    auto wrapper = make_wrapper_manifest(cfg);
    if (!wrapper) {
        return unexpected{wrapper.error()};
    }
    std::vector<index_entry> entries;
    for (auto& l : img.layers) {
        if (!l.layer.manifest) {
            return unexpected{fmt::format(
                "the layer '{}' has no identity: it was not added to a "
                "repository",
                l.alias)};
        }
        entries.push_back({*l.layer.manifest, l.alias, l.mount});
    }
    auto index = make_index(*wrapper, entries);
    if (!index) {
        return unexpected{index.error()};
    }
    return image_objects{cfg, *wrapper, *index};
}

layer_config layer_config_of(const layer& l) {
    return layer_config{.shared = l.config,
                        .mount = l.mount,
                        .relocatable = l.relocatable,
                        .views = l.views,
                        .default_view = l.default_view};
}

util::expected<layer_objects, error> make_layer_objects(const layer_config& c,
                                                        const storage& store) {
    if (!store.digest) {
        return unexpected{error{"the squashfs of the layer has no digest"}};
    }
    if (auto r = check_storable(c.shared, "layer"); !r) {
        return unexpected{r.error()};
    }
    auto cfg_bytes = canonical(to_json(c));
    if (!cfg_bytes) {
        return unexpected{cfg_bytes.error()};
    }
    auto cfg = make_blob(*cfg_bytes);
    auto manifest = make_layer_manifest(cfg, *store.digest, store.size);
    if (!manifest) {
        return unexpected{manifest.error()};
    }
    return layer_objects{cfg, *manifest};
}

util::expected<layer_objects, error> make_layer_objects(const layer& l) {
    return make_layer_objects(layer_config_of(l), l.store);
}

std::optional<oci::digest> image_digest(const composed_image& img) {
    auto objs = make_image_objects(img);
    if (!objs) {
        return std::nullopt;
    }
    return objs->index.digest;
}

//
// tags
//

std::string short_id(const oci::digest& d) {
    return d.hex().substr(0, 16);
}

std::string make_tag(const config& c, const oci::digest& d) {
    // a stored config has a version and a release
    std::string readable = fmt::format("{}-{}", c.version ? c.version->string() : "",
                                       c.release ? c.release->string() : "");
    for (auto& [k, v] : c.meta) {
        readable += "-" + v;
    }
    for (auto& ch : readable) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
              ch == '.' || ch == '-')) {
            ch = '_';
        }
    }
    if (!readable.empty() && (readable[0] == '.' || readable[0] == '-')) {
        readable[0] = '_';
    }
    const std::size_t max_readable = 128 - 17;
    if (readable.size() > max_readable) {
        readable.resize(max_readable);
    }
    return readable + "-" + short_id(d);
}

} // namespace uenv::format

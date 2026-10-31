#pragma once

// helpers for the tests of the composable design

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/model.h>
#include <uenv/name.h>
#include <uenv/parse.h>
#include <util/envvars.h>
#include <util/sha.h>

namespace v2test {

// a mount point from a literal that the test knows is valid
inline uenv::mount_point mp(const std::string& s) {
    return *uenv::mount_point::parse(s);
}

// a name from a literal that the test knows is valid
inline uenv::name nm(const std::string& s) {
    return *uenv::name::parse(s);
}

// a map with name keys, from literals
template <typename T>
std::map<uenv::name, T> named(std::initializer_list<std::pair<std::string, T>> l) {
    std::map<uenv::name, T> out;
    for (auto& [k, v] : l) {
        out.emplace(nm(k), v);
    }
    return out;
}

// the shared fields of a config, from literals
inline uenv::config
cfg(const std::string& name, const std::string& version,
    const std::string& release,
    std::initializer_list<std::pair<std::string, std::string>> meta = {}) {
    return uenv::config{.name = nm(name),
                        .version = nm(version),
                        .release = nm(release),
                        .meta = named<std::string>(meta)};
}

// "alias:view": a reference to a layer view
inline uenv::layer_view_ref ref(const std::string& s) {
    return *uenv::parse_layer_view_ref(s);
}
inline std::vector<uenv::layer_view_ref>
refs(std::initializer_list<std::string> l) {
    std::vector<uenv::layer_view_ref> out;
    for (auto& s : l) {
        out.push_back(ref(s));
    }
    return out;
}

// "[item:]name": the name of an image view
inline uenv::image_view_name vname(const std::string& s) {
    auto v = uenv::parse_view_requests(s)->front();
    return {.item = v.item, .name = v.name};
}
inline std::vector<uenv::image_view_name>
vnames(std::initializer_list<std::string> l) {
    std::vector<uenv::image_view_name> out;
    for (auto& s : l) {
        out.push_back(vname(s));
    }
    return out;
}

// a --view argument: "a:x,y"
inline std::vector<uenv::view_request> views(const std::string& s) {
    return *uenv::parse_view_requests(s);
}

inline envvars::patch prepend(std::string var, std::string value) {
    envvars::patch p;
    p.update_prefix_path(var, {envvars::update_kind::prepend, {value}});
    return p;
}

inline envvars::patch set_value(std::string var, std::string value) {
    envvars::patch p;
    p.update_scalar(var, value);
    return p;
}

// a layer with a fake squashfs digest
inline uenv::layer make_layer(std::string name, std::string mount,
                              bool relocatable,
                              std::map<std::string, envvars::patch> views,
                              std::optional<std::string> default_view = {}) {
    std::map<uenv::name, uenv::layer_view> lviews;
    for (auto& [k, v] : views) {
        lviews[nm(k)] = uenv::layer_view{.env = v};
    }
    uenv::layer_config cfg{
        .shared = v2test::cfg(name, "1.0", "v1", {{"uarch", "zen2"}}),
        .mount = mp(mount),
        .relocatable = relocatable,
        .views = std::move(lviews),
        .default_view = default_view ? std::optional{nm(*default_view)}
                                     : std::nullopt};
    uenv::storage store{
        .size = 4096,
        .digest = oci::digest::sha256(util::sha256_string(name + mount)),
        .file = "/blobs/" + name};
    auto objs = uenv::format::make_layer_objects(cfg, store);
    return *uenv::layer::make(
        std::move(cfg), std::move(store),
        uenv::format::layer_descriptor(objs->manifest.digest,
                                       objs->manifest.size()));
}

// prgenv-gnu: not relocatable, two views
inline uenv::layer prgenv() {
    return make_layer(
        "prgenv-gnu", "/user-environment", false,
        {{"prgenv", prepend("PATH", "/user-environment/env/prgenv/bin")},
         {"modules", prepend("MODULEPATH", "/user-environment/modules")}},
        "prgenv");
}

// tools: relocatable
inline uenv::layer tools() {
    return make_layer("tools", "/user-environment/tools", true,
                      {{"tools", prepend("PATH", "@uenv-root@/bin")}}, "tools");
}

inline std::string write_file(const std::filesystem::path& p,
                              const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
    return p.string();
}

} // namespace v2test

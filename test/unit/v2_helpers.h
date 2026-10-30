#pragma once

// helpers for the tests of the composable design

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/model.h>
#include <uenv/parse.h>
#include <util/envvars.h>
#include <util/sha.h>

namespace v2test {

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
    auto v = uenv::parse_view_args(s)->front();
    return {.item = v.uenv, .name = v.name};
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
inline std::vector<uenv::view_description> views(const std::string& s) {
    return *uenv::parse_view_args(s);
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
    uenv::layer l;
    l.config = {.name = name, .version = "1.0", .release = "v1",
                .meta = {{"uarch", "zen2"}}};
    l.mount = mount;
    l.relocatable = relocatable;
    for (auto& [k, v] : views) {
        l.views[k] = uenv::layer_view{.env = v};
    }
    l.default_view = default_view;
    l.store.size = 4096;
    l.store.digest = oci::digest::sha256(util::sha256_string(name + mount));
    l.store.file = "/blobs/" + name;
    auto objs = uenv::format::make_layer_objects(l);
    l.manifest = uenv::format::layer_descriptor(objs->manifest.digest,
                                                objs->manifest.size());
    return l;
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

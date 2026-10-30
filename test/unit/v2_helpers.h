#pragma once

// helpers for the tests of the composable design

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/model.h>
#include <util/envvars.h>
#include <util/sha.h>

namespace v2test {

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
    l.digest = objs->manifest.digest;
    l.manifest_size = objs->manifest.size();
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

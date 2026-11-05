// The code that the `uenv image` commands share.

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/registry.h>
#include <uenv/session.h>
#include <uenv/settings.h>
#include <uenv/store.h>

#include "image_util.h"
#include "terminal.h"
#include "util.h"

namespace uenv::cli {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string meta_string(const config& c) {
    std::vector<std::string> v;
    for (auto& [k, x] : c.meta) {
        v.push_back(fmt::format("{}={}", k, x));
    }
    return fmt::format("{}", fmt::join(v, ","));
}

json name_json(const std::optional<uenv::name>& n) {
    return n ? json(n->string()) : json(nullptr);
}

std::string size_string(std::uint64_t b) {
    if (b > (1u << 30)) {
        return fmt::format("{:.1f}GB", double(b) / (1u << 30));
    }
    if (b > (1u << 20)) {
        return fmt::format("{:.1f}MB", double(b) / (1u << 20));
    }
    return fmt::format("{}kB", (b + 1023) / 1024);
}

// the date of a time stamp "yyyy-mm-ddThh:mm:ssZ": "yyyy-mm-dd"
std::string date_string(const std::string& added) {
    return added.substr(0, added.find('T'));
}

json record_to_json(const object_record& r, record_time t) {
    json layers = json::array();
    for (auto& l : r.layers) {
        layers.push_back({{"alias", l.alias.string()},
                          {"mount", l.mount.string()},
                          {"digest", l.digest.string()},
                          {"label", label_string(l.cfg)},
                          {"meta", meta_json(l.cfg.meta)}});
    }
    json j{{"kind", r.kind == format::object_kind::image ? "image" : "layer"},
           {"label", label_string(r.cfg)},
           {"name", r.cfg.name.string()},
           {"version", name_json(r.cfg.version)},
           {"release", name_json(r.cfg.release)},
           {"meta", meta_json(r.cfg.meta)},
           {"id", r.id},
           {"digest", r.digest.string()},
           {"size", r.size},
           {"layers", layers}};
    if (!r.added.empty()) {
        j[t == record_time::added ? "added" : "pushed"] = r.added;
    }
    if (r.kind == format::object_kind::layer) {
        j["building_block"] = r.standalone;
    }
    if (r.repository) {
        j["repository"] = *r.repository;
        j["tag"] = r.tag.value_or("");
    }
    return j;
}

} // namespace

json meta_json(const std::map<uenv::name, std::string>& meta) {
    json j = json::object();
    for (auto& [k, v] : meta) {
        j[k.string()] = v;
    }
    return j;
}

void print_records(const std::vector<object_record>& records, bool json_out,
                   bool no_header, record_time t) {
    if (json_out) {
        json j = json::array();
        for (auto& r : records) {
            j.push_back(record_to_json(r, t));
        }
        term::msg("{}", j.dump(2));
        return;
    }
    // each column is as wide as its widest value
    const bool dates = t == record_time::pushed;
    struct row {
        std::string uenv, meta, id, size, date;
    };
    std::vector<row> rows;
    row width{"uenv", "meta", "id", "size", "date"};
    for (auto& r : records) {
        rows.push_back({(r.kind == format::object_kind::layer ? "+" : "") +
                            label_string(r.cfg),
                        meta_string(r.cfg), r.id, size_string(r.size),
                        date_string(r.added)});
    }
    auto w = [&](auto field) {
        std::size_t n = (width.*field).size();
        for (auto& x : rows) {
            n = std::max(n, (x.*field).size());
        }
        return n;
    };
    const auto wu = w(&row::uenv), wm = w(&row::meta), wi = w(&row::id),
               ws = w(&row::size);
    auto print = [&](const row& x) {
        auto line = fmt::format("{:<{}}  {:<{}}  {:<{}}  {:>{}}", x.uenv, wu,
                                x.meta, wm, x.id, wi, x.size, ws);
        if (dates) {
            line += "  " + x.date;
        }
        term::msg("{}", line);
    };
    if (!no_header) {
        // the header in yellow, as v1 does: pad first, then color
        auto line =
            fmt::format("{:<{}}  {:<{}}  {:<{}}  {:>{}}", width.uenv, wu,
                        width.meta, wm, width.id, wi, width.size, ws);
        if (dates) {
            line += "  " + width.date;
        }
        term::msg("{}", color::yellow(line));
    }
    for (auto& x : rows) {
        print(x);
    }
}

registry::context make_context(const global_settings& settings) {
    registry::context ctx;
    ctx.cfg = &settings.config;
    ctx.keys = keys_of(settings.config);
    ctx.default_system = settings.config.system_name;
    auto cache = user_cache_path(settings.calling_environment);
    ctx.cache = cache ? *cache : fs::temp_directory_path() / "uenv-cache";
    // each registry uses its own credentials: the client asks for those of
    // the registry that it connects to
    const auto& env = settings.calling_environment;
    ctx.credentials = [&env](const util::url& base) {
        auto c =
            resolve_registry_credentials(env, base, std::nullopt, std::nullopt);
        return c ? std::optional<oci::credentials>{*c} : std::nullopt;
    };
    return ctx;
}

} // namespace uenv::cli

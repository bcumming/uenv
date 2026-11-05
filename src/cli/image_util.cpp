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

std::string meta_string(const config& cfg) {
    std::vector<std::string> pairs;
    for (auto& [key, value] : cfg.meta) {
        pairs.push_back(fmt::format("{}={}", key, value));
    }
    return fmt::format("{}", fmt::join(pairs, ","));
}

json name_json(const std::optional<uenv::name>& name) {
    return name ? json(name->string()) : json(nullptr);
}

std::string size_string(std::uint64_t bytes) {
    if (bytes > (1u << 30)) {
        return fmt::format("{:.1f}GB", double(bytes) / (1u << 30));
    }
    if (bytes > (1u << 20)) {
        return fmt::format("{:.1f}MB", double(bytes) / (1u << 20));
    }
    return fmt::format("{}kB", (bytes + 1023) / 1024);
}

// the date of a time stamp "yyyy-mm-ddThh:mm:ssZ": "yyyy-mm-dd"
std::string date_string(const std::string& added) {
    return added.substr(0, added.find('T'));
}

json record_to_json(const object_record& record, record_time time) {
    json layers = json::array();
    for (auto& layer : record.layers) {
        layers.push_back({{"alias", layer.alias.string()},
                          {"mount", layer.mount.string()},
                          {"digest", layer.digest.string()},
                          {"label", label_string(layer.cfg)},
                          {"meta", meta_json(layer.cfg.meta)}});
    }
    json out{
        {"kind", record.kind == format::object_kind::image ? "image" : "layer"},
        {"label", label_string(record.cfg)},
        {"name", record.cfg.name.string()},
        {"version", name_json(record.cfg.version)},
        {"release", name_json(record.cfg.release)},
        {"meta", meta_json(record.cfg.meta)},
        {"id", record.id},
        {"digest", record.digest.string()},
        {"size", record.size},
        {"layers", layers}};
    if (!record.added.empty()) {
        out[time == record_time::added ? "added" : "pushed"] = record.added;
    }
    if (record.kind == format::object_kind::layer) {
        out["building_block"] = record.standalone;
    }
    if (record.repository) {
        out["repository"] = *record.repository;
        out["tag"] = record.tag.value_or("");
    }
    return out;
}

} // namespace

json meta_json(const std::map<uenv::name, std::string>& meta) {
    json out = json::object();
    for (auto& [key, value] : meta) {
        out[key.string()] = value;
    }
    return out;
}

void print_records(const std::vector<object_record>& records, bool json_out,
                   bool no_header, record_time time) {
    if (json_out) {
        json out = json::array();
        for (auto& record : records) {
            out.push_back(record_to_json(record, time));
        }
        term::msg("{}", out.dump(2));
        return;
    }
    // each column is as wide as its widest value
    const bool dates = time == record_time::pushed;
    struct row {
        std::string uenv, meta, id, size, date;
    };
    std::vector<row> rows;
    row header{"uenv", "meta", "id", "size", "date"};
    for (auto& record : records) {
        rows.push_back({(record.kind == format::object_kind::layer ? "+" : "") +
                            label_string(record.cfg),
                        meta_string(record.cfg), record.id,
                        size_string(record.size), date_string(record.added)});
    }
    auto width_of = [&](auto field) {
        std::size_t widest = (header.*field).size();
        for (auto& entry : rows) {
            widest = std::max(widest, (entry.*field).size());
        }
        return widest;
    };
    const auto uenv_width = width_of(&row::uenv),
               meta_width = width_of(&row::meta), id_width = width_of(&row::id),
               size_width = width_of(&row::size);
    auto print = [&](const row& entry) {
        auto line = fmt::format("{:<{}}  {:<{}}  {:<{}}  {:>{}}", entry.uenv,
                                uenv_width, entry.meta, meta_width, entry.id,
                                id_width, entry.size, size_width);
        if (dates) {
            line += "  " + entry.date;
        }
        term::msg("{}", line);
    };
    if (!no_header) {
        // the header in yellow, as v1 does: pad first, then color
        auto line = fmt::format("{:<{}}  {:<{}}  {:<{}}  {:>{}}", header.uenv,
                                uenv_width, header.meta, meta_width, header.id,
                                id_width, header.size, size_width);
        if (dates) {
            line += "  " + header.date;
        }
        term::msg("{}", color::yellow(line));
    }
    for (auto& entry : rows) {
        print(entry);
    }
}

registry::context make_context(const global_settings& settings) {
    registry::context context;
    context.cfg = &settings.config;
    context.keys = keys_of(settings.config);
    context.default_system = settings.config.system_name;
    auto cache = user_cache_path(settings.calling_environment);
    context.cache = cache ? *cache : fs::temp_directory_path() / "uenv-cache";
    // each registry uses its own credentials: the client asks for those of
    // the registry that it connects to
    const auto& env = settings.calling_environment;
    context.credentials = [&env](const util::url& base) {
        auto credentials =
            resolve_registry_credentials(env, base, std::nullopt, std::nullopt);
        return credentials ? std::optional<oci::credentials>{*credentials}
                           : std::nullopt;
    };
    return context;
}

} // namespace uenv::cli

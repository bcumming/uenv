// `uenv image add`: add a squashfs file to the user repository, as an
// image or as a layer (+label).

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include <argparse/argparse.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/meta.h>
#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/session.h>
#include <uenv/store.h>
#include <util/expected.h>
#include <util/fs.h>

#include "image.h"
#include "terminal.h"

namespace uenv {

namespace fs = std::filesystem;
using json = nlohmann::json;
using argparse::completion;

namespace {

struct add_args {
    std::string label;
    std::string squashfs;
    std::optional<std::string> config;
};

int image_add(const add_args& args, const global_settings& settings) {
    auto store = open_user_store(settings.config);
    if (!store) {
        term::error("{}", store.error());
        return 1;
    }
    bool as_layer = args.label.starts_with("+");
    auto text = as_layer ? args.label.substr(1) : args.label;
    auto label = parse_uenv_label(text);
    if (!label || !label->name || !label->version || !label->tag) {
        term::error("the label '{}' must have the form "
                    "name/version:release[@system][%uarch]",
                    args.label);
        return 1;
    }
    auto parsed_sqfs = parse_file_path(args.squashfs);
    if (!parsed_sqfs) {
        term::error("{}", parsed_sqfs.error());
        return 1;
    }
    const fs::path sqfs = *parsed_sqfs;
    // the layer config: from the file that the user gives, else from the
    // meta/env.json of the squashfs
    auto cfg = [&]() -> util::expected<layer_config, std::string> {
        if (args.config) {
            auto file = parse_file_path(*args.config);
            if (!file) {
                return util::unexpected{file.error()};
            }
            auto body = util::read_file(*file);
            if (!body) {
                return util::unexpected{body.error()};
            }
            // the label gives the fields that the file does not give
            auto config_json = json::parse(*body, nullptr, false);
            if (config_json.is_discarded()) {
                return util::unexpected{
                    fmt::format("{} is not valid JSON", *args.config)};
            }
            config_json["name"] = *label->name;
            config_json["version"] = *label->version;
            config_json["release"] = *label->tag;
            if (!config_json.contains("meta")) {
                config_json["meta"] = json::object();
            }
            if (label->uarch) {
                config_json["meta"]["uarch"] = *label->uarch;
            }
            return format::parse_layer_config(config_json.dump());
        }
        auto meta = read_squashfs_meta(sqfs);
        if (!meta) {
            return util::unexpected{meta.error()};
        }
        auto converted = v1::make_layer_config(
            *meta, v1::v1_label{*label->version, *label->tag, label->system,
                                label->uarch});
        if (!converted) {
            return converted;
        }
        // the name of the label, not the name in meta/env.json
        auto name = uenv::name::parse(*label->name);
        if (!name) {
            return util::unexpected{name.error()};
        }
        converted->shared.name = *name;
        return converted;
    }();
    if (!cfg) {
        term::error("{}", cfg.error());
        return 1;
    }
    std::map<uenv::name, std::string> image_meta;
    if (label->system) {
        image_meta[settings.config.label.at] = *label->system;
    }
    auto record = add_squashfs(*store, sqfs, *cfg, image_meta, as_layer);
    if (!record) {
        term::error("{}", record.error());
        return 1;
    }
    term::msg("added {}{} id={} digest={}", as_layer ? "+" : "",
              label_string(record->cfg), record->id, record->digest.string());
    return 0;
}

} // namespace

argparse::command image_add_command(const global_settings& settings) {
    argparse::command_builder<add_args> c(
        "add", "add a squashfs file as an image (or a layer: +label)");
    c.add_positional("label", &add_args::label,
                     "name/version:release[@system][%uarch]")
        .required()
        .complete(completion::none());
    c.add_positional("squashfs", &add_args::squashfs, "the squashfs file")
        .required()
        .complete(completion::file("*.squashfs"));
    c.add_option("config", &add_args::config,
                 "a layer config (JSON), in place of meta/env.json")
        .complete(completion::file("*.json"));
    c.action([&settings](const add_args& args) {
        return image_add(args, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

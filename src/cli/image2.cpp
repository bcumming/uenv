// The `uenv image` commands for composable images: ls, add, rm, inspect,
// compose, find, pull, push, copy, delete and attach.

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <uenv/format.h>
#include <uenv/meta.h>
#include <uenv/ops.h>
#include <uenv/parse.h>
#include <uenv/registry.h>
#include <uenv/settings.h>
#include <uenv/store.h>
#include <util/expected.h>
#include <util/fs.h>

#include "help.h"
#include "image2.h"
#include <uenv/session.h>
#include "terminal.h"
#include "uenv.h"
#include "util.h"

namespace uenv {

namespace fs = std::filesystem;
using json = nlohmann::json;
using argparse::completion;

namespace {

std::string meta_string(const config& c) {
    std::vector<std::string> v;
    for (auto& [k, x] : c.meta) {
        v.push_back(fmt::format("{}={}", k, x));
    }
    return fmt::format("{}", fmt::join(v, ","));
}

std::string label_of(const config& c) {
    return label_string(c);
}

json meta_json(const std::map<uenv::name, std::string>& meta) {
    json j = json::object();
    for (auto& [k, v] : meta) {
        j[k.string()] = v;
    }
    return j;
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

json record_to_json(const object_record& r) {
    json layers = json::array();
    for (auto& l : r.layers) {
        layers.push_back({{"alias", l.alias.string()},
                          {"mount", l.mount.string()},
                          {"digest", l.digest.string()},
                          {"label", label_of(l.cfg)},
                          {"meta", meta_json(l.cfg.meta)}});
    }
    json j{{"kind", r.kind == format::object_kind::image ? "image" : "layer"},
           {"label", label_of(r.cfg)},
           {"name", r.cfg.name.string()},
           {"version", name_json(r.cfg.version)},
           {"release", name_json(r.cfg.release)},
           {"meta", meta_json(r.cfg.meta)},
           {"id", r.id},
           {"digest", r.digest.string()},
           {"size", r.size},
           {"layers", layers}};
    if (r.kind == format::object_kind::layer) {
        j["building_block"] = r.standalone;
    }
    if (r.repository) {
        j["repository"] = *r.repository;
        j["tag"] = r.tag.value_or("");
    }
    return j;
}

void print_records(const std::vector<object_record>& records, bool json_out,
                   bool no_header) {
    if (json_out) {
        json j = json::array();
        for (auto& r : records) {
            j.push_back(record_to_json(r));
        }
        term::msg("{}", j.dump(2));
        return;
    }
    if (!no_header) {
        term::msg("{:<36} {:<28} {:<16} {:>8} {}", "uenv", "meta", "id", "size",
                  "layers");
    }
    for (auto& r : records) {
        std::string extra;
        if (r.kind == format::object_kind::layer) {
            extra = r.standalone ? "building block" : "in an image";
        } else {
            std::vector<std::string> ls;
            for (auto& l : r.layers) {
                ls.push_back(l.alias.string());
            }
            extra = fmt::format("{}", fmt::join(ls, ","));
        }
        term::msg("{:<36} {:<28} {:<16} {:>8} {}",
                  (r.kind == format::object_kind::layer ? "+" : "") +
                      label_of(r.cfg),
                  meta_string(r.cfg), r.id, size_string(r.size), extra);
    }
}

registry::context make_context(const global_settings& settings,
                               std::optional<std::string> registry_option) {
    registry::context ctx;
    ctx.cfg = &settings.config;
    ctx.keys = keys_of(settings.config);
    ctx.default_system = settings.config.system_name;
    auto cache = user_cache_path(settings.calling_environment);
    ctx.cache = cache ? *cache : fs::temp_directory_path() / "uenv-cache";
    if (auto t = registry::resolve_target(settings.config, registry_option)) {
        if (auto c = resolve_registry_credentials(settings.calling_environment,
                                                  t->base, std::nullopt,
                                                  std::nullopt);
            c) {
            ctx.creds = *c;
        }
    }
    return ctx;
}

//
// ls
//

struct ls_args {
    std::optional<std::string> label;
    bool json = false;
    bool no_header = false;
};

int image_ls(const ls_args& args, const global_settings& settings) {
    auto stores = open_stores(settings.config);
    if (!stores) {
        term::error("{}", stores.error());
        return 1;
    }
    std::string text = args.label.value_or("");
    label_query q;
    if (text == "+") {
        q.layer = true;
    } else if (!text.empty()) {
        auto p = parse_label_query(text, keys_of(settings.config),
                                   settings.config.system_name);
        if (!p) {
            term::error("{}", p.error());
            return 1;
        }
        q = *p;
    } else if (settings.config.system_name) {
        q.meta[settings.config.label.at] = *settings.config.system_name;
    }
    std::vector<object_record> out;
    for (auto& s : *stores) {
        auto r = s.query(q);
        if (!r) {
            term::error("{}", r.error());
            return 1;
        }
        out.insert(out.end(), r->begin(), r->end());
    }
    print_records(out, args.json, args.no_header);
    return 0;
}

//
// add
//

struct add_args {
    std::string label;
    std::string squashfs;
    std::optional<std::string> config;
};

int image_add(const add_args& args, const global_settings& settings) {
    auto s = open_user_store(settings.config);
    if (!s) {
        term::error("{}", s.error());
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
    const fs::path sqfs = fs::absolute(args.squashfs);
    // the layer config: from the file that the user gives, else from the
    // meta/env.json of the squashfs
    auto cfg = [&]() -> util::expected<layer_config, std::string> {
        if (args.config) {
            auto body = util::read_file(*args.config);
            if (!body) {
                return util::unexpected{body.error()};
            }
            // the label gives the fields that the file does not give
            auto j = json::parse(*body, nullptr, false);
            if (j.is_discarded()) {
                return util::unexpected{
                    fmt::format("{} is not valid JSON", *args.config)};
            }
            j["name"] = *label->name;
            j["version"] = *label->version;
            j["release"] = *label->tag;
            if (!j.contains("meta")) {
                j["meta"] = json::object();
            }
            if (label->uarch) {
                j["meta"]["uarch"] = *label->uarch;
            }
            return format::parse_layer_config(j.dump());
        }
        auto m = read_squashfs_meta(sqfs);
        if (!m) {
            return util::unexpected{m.error()};
        }
        auto c = v1::make_layer_config(
            *m, v1::v1_label{*label->version, *label->tag, label->system,
                             label->uarch});
        if (!c) {
            return c;
        }
        // the name of the label, not the name in meta/env.json
        auto n = uenv::name::parse(*label->name);
        if (!n) {
            return util::unexpected{n.error()};
        }
        c->shared.name = *n;
        return c;
    }();
    if (!cfg) {
        term::error("{}", cfg.error());
        return 1;
    }
    std::map<uenv::name, std::string> image_meta;
    if (label->system) {
        auto key = uenv::name::parse(settings.config.label.at);
        if (!key) {
            term::error("the setting label.at: {}", key.error());
            return 1;
        }
        image_meta[*key] = *label->system;
    }
    auto r = add_squashfs(*s, sqfs, *cfg, image_meta, as_layer);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("added {}{} id={} digest={}", as_layer ? "+" : "",
              label_of(r->cfg), r->id, r->digest.string());
    return 0;
}

//
// rm
//

struct rm_args {
    std::string label;
};

int image_rm(const rm_args& args, const global_settings& settings) {
    auto s = open_user_store(settings.config);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto q = parse_label_query(args.label, keys_of(settings.config));
    if (!q) {
        term::error("{}", q.error());
        return 1;
    }
    std::vector<store> one;
    one.push_back(std::move(*s));
    auto f = find_unique(one, *q, args.label);
    if (!f) {
        term::error("{}", f.error());
        return 1;
    }
    if (q->layer && !f->record.standalone) {
        term::error("the layer {} is part of an image: delete the image",
                    label_of(f->record.cfg));
        return 1;
    }
    auto r = one[0].remove(f->record.digest);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("removed {} ({} blobs deleted)", label_of(f->record.cfg),
              r->size());
    return 0;
}

//
// inspect
//

struct inspect_args {
    std::string uenv;
    bool json = false;
};

json env_json_of(const envvars::patch& p) {
    return format::env_to_json(p);
}

int image_inspect(const inspect_args& args, const global_settings& settings) {
    auto stores = open_stores(settings.config);
    if (!stores) {
        term::error("{}", stores.error());
        return 1;
    }
    auto items = parse_item_list(args.uenv, keys_of(settings.config),
                                 settings.config.system_name);
    if (!items) {
        term::error("{}", items.error());
        return 1;
    }
    auto img = resolve_items(*stores, *items, false);
    if (!img) {
        term::error("{}", img.error());
        return 1;
    }
    json layers = json::array();
    for (auto& l : img->layers) {
        json views = json::object();
        for (auto& [k, v] : l.layer.views) {
            views[k.string()] = env_json_of(v.env);
        }
        layers.push_back(
            {{"alias", l.alias.string()},
             {"mount", l.mount.string()},
             {"build_mount", l.layer.mount.string()},
             {"relocatable", l.layer.relocatable},
             {"label", label_of(l.layer.config)},
             {"meta", meta_json(l.layer.config.meta)},
             {"digest", l.layer.digest() ? l.layer.digest()->string() : ""},
             {"squashfs", l.layer.store.file ? l.layer.store.file->string() : ""},
             {"views", views}});
    }
    json views = json::object();
    for (auto& [k, v] : img->views) {
        json inc = json::array();
        for (auto& r : v.include) {
            inc.push_back(format_as(r));
        }
        views[format_as(k)] = {{"include", inc}, {"env", env_json_of(v.env)}};
    }
    std::vector<std::string> defaults;
    for (auto& d : img->default_views) {
        defaults.push_back(format_as(d));
    }
    const std::string kind =
        img->kind == uenv::image_kind::layer           ? "layer"
        : img->kind == uenv::image_kind::runtime_merge ? "runtime merge"
                                                       : "image";
    json j{{"kind", kind},
           {"label", label_of(img->config)},
           {"description", img->config.description},
           {"meta", meta_json(img->config.meta)},
           {"digest", img->digest ? img->digest->string() : ""},
           {"layers", layers},
           {"views", views},
           {"default_views", defaults}};
    if (args.json) {
        term::msg("{}", j.dump(2));
        return 0;
    }
    term::msg("{} {}", kind, label_of(img->config));
    term::msg("  digest: {}", j["digest"].get<std::string>());
    term::msg("  meta:   {}", meta_string(img->config));
    term::msg("  layers:");
    for (auto& l : img->layers) {
        term::msg("    {:<12} {:<28} {} {}", l.alias, l.mount,
                  label_of(l.layer.config),
                  l.layer.relocatable ? "(relocatable)" : "");
    }
    term::msg("  views: {}", fmt::join(views.items() | std::views::transform(
                                                          [](auto&& kv) {
                                                              return kv.key();
                                                          }),
                                       ", "));
    term::msg("  default: {}", fmt::join(defaults, ","));
    return 0;
}

//
// compose
//

struct compose_args {
    std::string file;
};

int image_compose(const compose_args& args, const global_settings& settings) {
    auto s = open_user_store(settings.config);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto body = util::read_file(args.file);
    if (!body) {
        term::error("{}", body.error());
        return 1;
    }
    auto spec = json::parse(*body, nullptr, false);
    if (spec.is_discarded()) {
        term::error("{} is not valid JSON", args.file);
        return 1;
    }
    auto r = compose(*s, spec, keys_of(settings.config));
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("composed {} id={} digest={}", label_of(r->cfg), r->id,
              r->digest.string());
    return 0;
}

//
// find
//

struct find_args {
    std::optional<std::string> label;
    std::optional<std::string> registry;
    bool json = false;
    bool no_header = false;
};

int image_find(const find_args& args, const global_settings& settings) {
    auto text = args.label.value_or("");
    auto a = registry::parse_address(text.empty() ? "x" : text, settings.config,
                                     args.registry);
    if (!a) {
        term::error("{}", a.error());
        return 1;
    }
    auto ctx = make_context(settings, args.registry);
    auto cat = registry::catalogue::open(a->where, ctx);
    if (!cat) {
        term::error("{}", cat.error());
        return 1;
    }
    auto st = cat->refresh();
    if (!st) {
        term::error("{}", st.error());
        return 1;
    }
    spdlog::info("catalogue refresh: {} events, {} requests", st->events,
                 st->requests);
    label_query q;
    auto label = text.empty() ? std::string{} : a->label.value_or("");
    if (label == "+") {
        q.layer = true;
    } else if (!label.empty()) {
        auto p = parse_label_query(label, keys_of(settings.config),
                                   settings.config.system_name);
        if (!p) {
            term::error("{}", p.error());
            return 1;
        }
        q = *p;
    } else if (settings.config.system_name) {
        q.meta[settings.config.label.at] = *settings.config.system_name;
    }
    auto r = cat->db().query(q);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    print_records(*r, args.json, args.no_header);
    return 0;
}

//
// pull
//

struct pull_args {
    std::string address;
    std::optional<std::string> registry;
};

int image_pull(const pull_args& args, const global_settings& settings) {
    auto s = open_user_store(settings.config);
    if (!s) {
        term::error("{}", s.error());
        return 1;
    }
    auto a = registry::parse_address(args.address, settings.config,
                                     args.registry);
    if (!a) {
        term::error("{}", a.error());
        return 1;
    }
    auto ctx = make_context(settings, args.registry);
    auto r = registry::pull(*s, *a, ctx);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("pulled {} id={} ({} squashfs downloaded, {} already present)",
              label_of(r->cfg), r->id, ctx.st.downloaded, ctx.st.skipped);
    return 0;
}

//
// push
//

struct push_args {
    std::string source;
    std::optional<std::string> destination;
    std::optional<std::string> registry;
};

int image_push(const push_args& args, const global_settings& settings) {
    // the destination: reg[/ns]:: or the default registry
    std::optional<registry::target> t;
    if (args.destination) {
        auto a = registry::parse_address(*args.destination, settings.config,
                                         args.registry);
        if (!a) {
            term::error("{}", a.error());
            return 1;
        }
        t = a->where;
    } else {
        auto r = registry::resolve_target(settings.config, args.registry);
        if (!r) {
            term::error("{}", r.error());
            return 1;
        }
        t = *r;
    }
    auto ctx = make_context(settings, args.registry);
    std::error_code ec;
    util::expected<registry::push_result, std::string> r =
        util::unexpected{std::string{}};
    if (fs::is_directory(args.source, ec)) {
        r = registry::push_layout(args.source, *t, ctx);
    } else {
        auto stores = open_stores(settings.config);
        if (!stores) {
            term::error("{}", stores.error());
            return 1;
        }
        auto q = parse_label_query(args.source, keys_of(settings.config),
                                   settings.config.system_name);
        if (!q) {
            term::error("{}", q.error());
            return 1;
        }
        auto f = find_unique(*stores, *q, args.source);
        if (!f) {
            term::error("{}", f.error());
            return 1;
        }
        r = registry::push((*stores)[f->store_index], f->record.digest, *t, ctx);
    }
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("pushed {}:{} ({} uploaded, {} mounted, {} present)",
              r->repository, r->tag, ctx.st.uploaded, ctx.st.mounted,
              ctx.st.skipped);
    return 0;
}

//
// copy
//

struct copy_args {
    std::string source;
    std::string destination;
    std::optional<std::string> set;
    std::optional<std::string> registry;
};

int image_copy(const copy_args& args, const global_settings& settings) {
    auto src = registry::parse_address(args.source, settings.config,
                                       args.registry);
    if (!src) {
        term::error("{}", src.error());
        return 1;
    }
    auto dst = registry::parse_address(args.destination, settings.config,
                                       args.registry);
    if (!dst) {
        term::error("{}", dst.error());
        return 1;
    }
    std::map<std::string, std::string> changes;
    if (args.set) {
        std::size_t start = 0;
        auto text = *args.set;
        while (start < text.size()) {
            auto end = text.find(',', start);
            auto kv = text.substr(start, end == std::string::npos
                                             ? std::string::npos
                                             : end - start);
            auto eq = kv.find('=');
            if (eq == std::string::npos) {
                term::error("--set: '{}' must have the form key=value", kv);
                return 1;
            }
            changes[kv.substr(0, eq)] = kv.substr(eq + 1);
            start = end == std::string::npos ? text.size() : end + 1;
        }
    }
    auto ctx = make_context(settings, args.registry);
    auto r = registry::copy(*src, dst->where, ctx, changes);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("copied to {}:{} digest={} ({} referrers)", r->repository, r->tag,
              r->digest.string(), ctx.st.referrers_copied);
    return 0;
}

//
// delete
//

struct delete_args {
    std::string address;
    std::optional<std::string> registry;
};

int image_delete(const delete_args& args, const global_settings& settings) {
    auto a = registry::parse_address(args.address, settings.config,
                                     args.registry);
    if (!a) {
        term::error("{}", a.error());
        return 1;
    }
    auto ctx = make_context(settings, args.registry);
    auto r = registry::remove(*a, ctx);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("deleted {}@{}", r->repository, r->digest.string());
    return 0;
}

//
// attach
//

struct attach_args {
    std::string address;
    std::string kind;
    std::string content;
    std::optional<std::string> registry;
};

int image_attach(const attach_args& args, const global_settings& settings) {
    auto a = registry::parse_address(args.address, settings.config,
                                     args.registry);
    if (!a) {
        term::error("{}", a.error());
        return 1;
    }
    auto ctx = make_context(settings, args.registry);
    auto r = registry::attach(*a, args.kind, args.content, ctx);
    if (!r) {
        term::error("{}", r.error());
        return 1;
    }
    term::msg("attached {} in {}", args.kind, fmt::join(*r, ", "));
    return 0;
}

} // namespace

argparse::command image_command(const global_settings& settings) {
    argparse::command_builder<> image_cli("image",
                                          "manage and query uenv images");
    {
        argparse::command_builder<ls_args> c("ls", "list images (or layers: +)");
        c.add_positional("label", &ls_args::label, "a label, or + for layers")
            .complete(completion::none());
        c.add_flag("json", &ls_args::json, "print JSON");
        c.add_flag("no-header", &ls_args::no_header, "no header");
        c.action([&settings](const ls_args& a) { return image_ls(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
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
        c.action(
            [&settings](const add_args& a) { return image_add(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<rm_args> c("rm", "delete an image or a layer");
        c.add_positional("label", &rm_args::label, "a label or an id")
            .required()
            .complete(completion::none());
        c.action([&settings](const rm_args& a) { return image_rm(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    image_cli.add_subcommand(image_inspect_command(settings));
    {
        argparse::command_builder<compose_args> c(
            "compose", "make an image from layers, with a compose file");
        c.add_positional("file", &compose_args::file, "the compose file (JSON)")
            .required()
            .complete(completion::file("*.json"));
        c.action([&settings](const compose_args& a) {
            return image_compose(a, settings);
        });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<find_args> c("find",
                                               "find images in a registry");
        c.add_positional("label", &find_args::label, "a label")
            .complete(completion::none());
        c.add_option("registry", &find_args::registry, "a registry")
            .complete(completion::none());
        c.add_flag("json", &find_args::json, "print JSON");
        c.add_flag("no-header", &find_args::no_header, "no header");
        c.action(
            [&settings](const find_args& a) { return image_find(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<pull_args> c("pull", "pull an image or a layer");
        c.add_positional("address", &pull_args::address, "the image")
            .required()
            .complete(completion::none());
        c.add_option("registry", &pull_args::registry, "a registry")
            .complete(completion::none());
        c.action(
            [&settings](const pull_args& a) { return image_pull(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<push_args> c(
            "push", "push an image or a layer, or an OCI layout directory");
        c.add_positional("source", &push_args::source,
                         "a label, an id, or a directory")
            .required()
            .complete(completion::path());
        c.add_positional("destination", &push_args::destination,
                         "reg[/ns]:: (default: the default registry)")
            .complete(completion::none());
        c.add_option("registry", &push_args::registry, "a registry")
            .complete(completion::none());
        c.action(
            [&settings](const push_args& a) { return image_push(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<copy_args> c("copy",
                                               "copy an image in a registry");
        c.add_positional("source", &copy_args::source, "the image")
            .required()
            .complete(completion::none());
        c.add_positional("destination", &copy_args::destination, "reg[/ns]::")
            .required()
            .complete(completion::none());
        c.add_option("set", &copy_args::set,
                     "changes: release=v2,meta.system=santis")
            .complete(completion::none());
        c.add_option("registry", &copy_args::registry, "a registry")
            .complete(completion::none());
        c.action(
            [&settings](const copy_args& a) { return image_copy(a, settings); });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<delete_args> c(
            "delete", "delete an image or a building block in a registry");
        c.add_positional("address", &delete_args::address, "the image")
            .required()
            .complete(completion::none());
        c.add_option("registry", &delete_args::registry, "a registry")
            .complete(completion::none());
        c.action([&settings](const delete_args& a) {
            return image_delete(a, settings);
        });
        image_cli.add_subcommand(std::move(c).build());
    }
    {
        argparse::command_builder<attach_args> c(
            "attach", "attach a referrer of a kind that the site defines");
        c.add_positional("address", &attach_args::address, "the subject")
            .required()
            .complete(completion::none());
        c.add_positional("kind", &attach_args::kind, "the kind, e.g. uenv/sbom")
            .required()
            .complete(completion::none());
        c.add_positional("content", &attach_args::content, "a file or a string")
            .required()
            .complete(completion::path());
        c.add_option("registry", &attach_args::registry, "a registry")
            .complete(completion::none());
        c.action([&settings](const attach_args& a) {
            return image_attach(a, settings);
        });
        image_cli.add_subcommand(std::move(c).build());
    }
    return std::move(image_cli).build();
}

argparse::command image_inspect_command(const global_settings& settings) {
    argparse::command_builder<inspect_args> c(
        "inspect", "show an image, a layer, or a list of them");
    c.add_positional("uenv", &inspect_args::uenv, "a label, list or file")
        .required()
        .complete(completion::custom("uenv_list"));
    c.add_flag("json", &inspect_args::json, "print JSON");
    c.action([&settings](const inspect_args& a) {
        return image_inspect(a, settings);
    });
    return std::move(c).build();
}

} // namespace uenv

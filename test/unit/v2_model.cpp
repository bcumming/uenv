// Tests of the composable design: JCS, the configs, the model, views,
// concretise and the form of the index.
// Each test names the design document and the section that it checks.

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <oci/manifest.h>
#include <uenv/format.h>
#include <uenv/model.h>
#include <uenv/ops.h>
#include <util/envvars.h>
#include <util/jcs.h>

#include "v2_helpers.h"

using json = nlohmann::json;
using namespace v2test;
namespace format = uenv::format;

//
// 2-oci-layout "Canonical encoding"
//

TEST_CASE("2/canonical: JCS sorts keys by UTF-16 code units", "[jcs]") {
    // RFC 8785, 3.2.3: the sorted order of these keys
    auto j = json::parse(
        R"({"\u20ac":"Euro","\r":"CR","\ufb33":"Hebrew","1":"One","\ud83d\ude00":"Smiley","\u0080":"Ctl","\u00f6":"Latin"})");
    auto out = util::jcs(j);
    REQUIRE(out);
    // the keys in the RFC order: \r, 1, \u0080, \u00f6, \u20ac, \ud83d\ude00,
    // \ufb33
    std::vector<std::string> expected{"CR",    "One",    "Ctl",   "Latin",
                                      "Euro",  "Smiley", "Hebrew"};
    std::size_t pos = 0;
    for (auto& v : expected) {
        auto p = out->find("\"" + v + "\"");
        REQUIRE(p != std::string::npos);
        REQUIRE(p >= pos);
        pos = p;
    }
}

TEST_CASE("2/canonical: JCS strings and whitespace", "[jcs]") {
    json j = {{"b", "line\nfeed/\x0f\"q\"\\"}, {"a", {1, true, nullptr}},
              {"c", "\xc3\xb6"}};
    auto out = util::jcs(j);
    REQUIRE(out);
    REQUIRE(*out == R"({"a":[1,true,null],"b":"line\nfeed/\u000f\"q\"\\","c":")"
                    "\xc3\xb6"
                    R"("})");
}

TEST_CASE("2/canonical: JCS refuses numbers that it can not encode", "[jcs]") {
    REQUIRE(util::jcs(json(std::uint64_t(1) << 53)));
    REQUIRE(!util::jcs(json((std::uint64_t(1) << 53) + 1)));
    REQUIRE(!util::jcs(json(1.5)));
    REQUIRE(util::jcs(json(4046512128u)));
}

TEST_CASE("2/canonical: the same objects give the same bytes", "[jcs]") {
    auto a = format::make_layer_objects(tools());
    auto b = format::make_layer_objects(tools());
    REQUIRE(a->manifest.bytes == b->manifest.bytes);
    REQUIRE(a->config.bytes == b->config.bytes);
    // no whitespace and sorted keys
    REQUIRE(a->manifest.bytes.find(' ') == std::string::npos);
    REQUIRE(a->manifest.bytes.starts_with("{\"artifactType\""));
}

//
// 2-oci-layout "The configs"
//

namespace {
json layer_config_json() {
    return json::parse(R"({
      "name": "tools", "version": "1.0", "release": "v1",
      "description": "uv-based Python tools",
      "meta": {"uarch": "zen2", "built-on": "eiger"},
      "mount": "/user-environment/tools", "relocatable": true,
      "views": {"tools": {"env": {"list": {"PATH": [{"op": "prepend", "value": ["@uenv-root@/bin"]}]}}}},
      "default_view": "tools"
    })");
}
json image_config_json() {
    return json::parse(R"({
      "name": "my-env", "version": "1.0", "release": "v1",
      "description": "compilers and my tools",
      "meta": {"uarch": "zen2", "system": "eiger"},
      "views": {
        "dev": {"include": ["prgenv-gnu:prgenv", "tools:tools"],
                "env": {"scalar": {"MY_TOOLS_ROOT": "/user-environment/tools"}}},
        "modules": {"include": ["prgenv-gnu:modules"]}
      },
      "default_view": "dev"
    })");
}
} // namespace

TEST_CASE("2/configs: a layer config round trip", "[config]") {
    auto c = format::parse_layer_config(layer_config_json().dump());
    REQUIRE(c);
    REQUIRE(c->shared.name == "tools");
    REQUIRE(c->relocatable);
    REQUIRE(c->shared.meta.at("built-on") == "eiger");
    auto again = format::parse_layer_config(format::to_json(*c).dump());
    REQUIRE(again);
    REQUIRE(*format::canonical(format::to_json(*again)) ==
            *format::canonical(format::to_json(*c)));
    REQUIRE(*format::canonical(format::to_json(*c)) ==
            *format::canonical(layer_config_json()));
}

TEST_CASE("2/configs: an image config round trip", "[config]") {
    auto c = format::parse_image_config(image_config_json().dump());
    REQUIRE(c);
    REQUIRE(c->views.at("dev").include.size() == 2u);
    REQUIRE(*format::canonical(format::to_json(*c)) ==
            *format::canonical(image_config_json()));
}

TEST_CASE("2/configs: relocatable and meta are required", "[config]") {
    for (auto field : {"relocatable", "meta", "mount", "views", "name",
                       "version", "release"}) {
        auto j = layer_config_json();
        j.erase(field);
        auto c = format::parse_layer_config(j.dump());
        INFO(field);
        REQUIRE(!c);
        REQUIRE(c.error().find(field) != std::string::npos);
    }
    auto j = image_config_json();
    j.erase("meta");
    REQUIRE(!format::parse_image_config(j.dump()));
}

TEST_CASE("2/configs: release is always given", "[config]") {
    auto j = layer_config_json();
    j["release"] = "";
    REQUIRE(!format::parse_layer_config(j.dump()));
}

TEST_CASE("2/configs: the parser is strict on types", "[config]") {
    auto j = layer_config_json();
    j["relocatable"] = "true";
    REQUIRE(!format::parse_layer_config(j.dump()));
    j = layer_config_json();
    j["meta"]["uarch"] = 1;
    REQUIRE(!format::parse_layer_config(j.dump()));
    j = layer_config_json();
    j["mount"] = "relative/path";
    REQUIRE(!format::parse_layer_config(j.dump()));
    j = layer_config_json();
    j["default_view"] = "nope";
    REQUIRE(!format::parse_layer_config(j.dump()));
}

TEST_CASE("2/configs: the parser ignores unknown fields", "[config]") {
    auto j = layer_config_json();
    j["future_field"] = {{"x", 1}};
    j["views"]["tools"]["future"] = true;
    REQUIRE(format::parse_layer_config(j.dump()));
    auto i = image_config_json();
    i["future_field"] = 1;
    REQUIRE(format::parse_image_config(i.dump()));
}

TEST_CASE("2/versions: a reader refuses a version that it does not know",
          "[config]") {
    auto r = format::check_media_type("application/vnd.uenv.layer-config.v2+json",
                                      format::mt_layer_config);
    REQUIRE(!r);
    REQUIRE(r.error().find("vnd.uenv.layer-config.v2") != std::string::npos);
    REQUIRE(r.error().find("later version of uenv") != std::string::npos);
    REQUIRE(format::check_media_type(format::mt_layer_config,
                                     format::mt_layer_config));
    REQUIRE(!format::check_media_type("application/x-squashfs",
                                      format::mt_squashfs));
}

TEST_CASE("1/views: a value and a prefix path in one patch warn", "[config]") {
    std::vector<std::string> warnings;
    auto p = format::env_from_json(
        json::parse(R"({"scalar": {"X": "a"}, "list": {"X": [{"op": "prepend", "value": ["b"]}]}})"),
        &warnings);
    REQUIRE(p);
    REQUIRE(warnings.size() == 1u);
}

TEST_CASE("1/views: a relocatable layer must not contain its mount point",
          "[config]") {
    auto c = *format::parse_layer_config(layer_config_json().dump());
    REQUIRE(format::check_relocatable(c));
    c.views["tools"].env =
        prepend("PATH", "/user-environment/tools/bin");
    REQUIRE(!format::check_relocatable(c));
    // a different path that starts with the mount point is not a match
    c.views["tools"].env = prepend("PATH", "/user-environment/tools2/bin");
    REQUIRE(format::check_relocatable(c));
    // a value with no '/' after the root, the case that motivates the
    // placeholder
    c.views["tools"].env = set_value("FLAGS", "-DMOUNT=/user-environment/tools\t-I");
    REQUIRE(!format::check_relocatable(c));
}

//
// 1-composition "Checks"
//

namespace {
std::vector<uenv::named_layer> my_env_layers() {
    return {{"prgenv-gnu", "/user-environment", prgenv()},
            {"tools", "/user-environment/tools", tools()}};
}
std::map<std::string, uenv::image_view> my_env_views() {
    return {{"dev",
             {.include = {"prgenv-gnu:prgenv", "tools:tools"},
              .env = set_value("MY_TOOLS_ROOT", "/user-environment/tools")}},
            {"modules", {.include = {"prgenv-gnu:modules"}}}};
}
uenv::config my_env_config() {
    return {.name = "my-env", .version = "1.0", .release = "v1",
            .meta = {{"system", "eiger"}}};
}
} // namespace

TEST_CASE("1/checks: a valid image", "[model]") {
    auto img = uenv::make_composed_image(std::nullopt, my_env_config(),
                                         my_env_layers(), my_env_views(), "dev");
    REQUIRE(img);
    REQUIRE(img->layers.front().alias == "prgenv-gnu");
}

TEST_CASE("1/checks: each check of a composed_image", "[model]") {
    SECTION("two layers with the same alias") {
        auto ls = my_env_layers();
        ls[1].alias = "prgenv-gnu";
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("same alias") != std::string::npos);
    }
    SECTION("two layers with the same mount point") {
        auto ls = my_env_layers();
        ls[1].mount = "/user-environment";
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("same mount point") != std::string::npos);
    }
    SECTION("a layer that is not relocatable, at a different mount point") {
        auto ls = my_env_layers();
        ls[0].mount = "/opt/pe";
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("not relocatable") != std::string::npos);
    }
    SECTION("a relocatable layer can move") {
        auto ls = my_env_layers();
        ls[1].mount = "/opt/tools";
        REQUIRE(uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                          {}, std::nullopt));
    }
    SECTION("an image view that refers to an alias that does not exist") {
        auto v = my_env_views();
        v["dev"].include.push_back("nope:tools");
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(),
                                           my_env_layers(), v, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("no layer with the alias") != std::string::npos);
    }
    SECTION("an image view that refers to a layer view that does not exist") {
        auto v = my_env_views();
        v["dev"].include.push_back("tools:nope");
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(),
                                           my_env_layers(), v, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("has no view") != std::string::npos);
    }
    SECTION("a default_view that is not a view of the image") {
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(),
                                           my_env_layers(), my_env_views(),
                                           "nope");
        REQUIRE(!r);
    }
    SECTION("a mount point with a variable") {
        auto ls = my_env_layers();
        ls[1].mount = "/scratch/$USER/tools";
        REQUIRE(!uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt));
    }
}

TEST_CASE("1/order of layers: the order has no meaning", "[model]") {
    // three layers, all permutations: the same digest, mounts and env
    auto extra = make_layer("extra", "/user-tools", true,
                            {{"x", set_value("X", "@uenv-root@")}}, "x");
    std::vector<uenv::named_layer> layers = my_env_layers();
    layers.push_back({"extra", "/user-tools", extra});
    auto views = my_env_views();
    views["dev"].include.push_back("extra:x");

    std::vector<int> perm{0, 1, 2};
    std::optional<oci::digest> first_digest;
    std::optional<std::string> first_env;
    std::vector<std::string> first_mounts;
    do {
        std::vector<uenv::named_layer> ls;
        for (auto i : perm) {
            ls.push_back(layers[i]);
        }
        auto img = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                             views, "dev");
        REQUIRE(img);
        auto d = format::image_digest(*img);
        REQUIRE(d);
        auto c = uenv::concretise(*img, {});
        REQUIRE(c);
        envvars::state env;
        env.set("PATH", "/usr/bin");
        uenv::apply_patches(env, c->patches);
        std::vector<std::string> vars;
        for (auto& [k, v] : env.variables()) {
            vars.push_back(k + "=" + v);
        }
        std::sort(vars.begin(), vars.end());
        auto env_str = fmt::format("{}", fmt::join(vars, ";"));
        std::vector<std::string> mounts;
        for (auto& [f, m] : c->mounts) {
            mounts.push_back(m);
        }
        if (!first_digest) {
            first_digest = d;
            first_env = env_str;
            first_mounts = mounts;
            // parents first
            REQUIRE(mounts ==
                    std::vector<std::string>{"/user-environment",
                                             "/user-environment/tools",
                                             "/user-tools"});
        } else {
            REQUIRE(*d == *first_digest);
            REQUIRE(env_str == *first_env);
            REQUIRE(mounts == first_mounts);
        }
    } while (std::next_permutation(perm.begin(), perm.end()));
}

TEST_CASE("2/index entries: a layer can occur two times in one image",
          "[model]") {
    auto t = tools();
    std::vector<uenv::named_layer> ls{{"a", "/opt/a", t}, {"b", "/opt/b", t}};
    auto img = uenv::make_composed_image(
        std::nullopt, my_env_config(), ls,
        {{"both", {.include = {"a:tools", "b:tools"}}}}, "both");
    REQUIRE(img);
    auto c = uenv::concretise(*img, {});
    REQUIRE(c);
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    // b is applied last, so it is first in PATH
    REQUIRE(env.get("PATH") == "/opt/b/bin:/opt/a/bin");
    auto objs = format::make_image_objects(*img);
    REQUIRE(objs);
    auto idx = format::parse_index(objs->index.bytes);
    REQUIRE(idx);
    REQUIRE(idx->layers.size() == 2u);
    REQUIRE(idx->layers[0].manifest.digest == idx->layers[1].manifest.digest);
}

//
// 1-composition "Raise"
//

TEST_CASE("1/raise: the raise of a layer", "[model]") {
    auto t = tools();
    auto img = uenv::raise_layer(t, std::nullopt, false);
    REQUIRE(img);
    REQUIRE(img->digest == t.digest);
    REQUIRE(img->is_layer);
    REQUIRE(img->layers.size() == 1u);
    REQUIRE(img->layers[0].alias == "tools");
    REQUIRE(img->views.at("tools").include ==
            std::vector<std::string>{"tools:tools"});
    REQUIRE(img->default_views == std::vector<std::string>{"tools"});
    REQUIRE(img->config == t.config);
    // two raises give the same result
    auto again = uenv::raise_layer(t, std::nullopt, false);
    REQUIRE(format::image_digest(*again) == format::image_digest(*img));
}

TEST_CASE("1/raise: a different mount point is an input to the raise",
          "[model]") {
    auto img = uenv::raise_layer(tools(), "/opt/tools", false);
    REQUIRE(img);
    REQUIRE(img->layers[0].mount == "/opt/tools");
    auto c = uenv::concretise(*img, {});
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    REQUIRE(env.get("PATH") == "/opt/tools/bin");
}

TEST_CASE("1/raise: --force mounts a layer that is not relocatable",
          "[model]") {
    REQUIRE(!uenv::raise_layer(prgenv(), "/opt/pe", false));
    auto img = uenv::raise_layer(prgenv(), "/opt/pe", true);
    REQUIRE(img);
    REQUIRE(img->layers[0].mount == "/opt/pe");
}

//
// 1-composition "Merge a list at start"
//

namespace {
uenv::composed_image image_of(uenv::layer l, std::map<std::string, std::string> meta = {}) {
    auto img = *uenv::raise_layer(l, std::nullopt, false);
    img.is_layer = false;
    img.config.meta.insert(meta.begin(), meta.end());
    img.digest = format::image_digest(img);
    return img;
}
auto digest_fn = [](const uenv::composed_image& i) {
    return format::image_digest(i);
};
} // namespace

TEST_CASE("1/merge: two images", "[model]") {
    std::vector<uenv::merge_item> items{
        {.image = image_of(prgenv())},
        {.image = image_of(tools())}};
    auto img = uenv::merge_items(items, digest_fn);
    REQUIRE(img);
    REQUIRE(img->layers.size() == 2u);
    REQUIRE(img->config.meta.empty());
    REQUIRE(img->views.contains("prgenv-gnu:prgenv"));
    REQUIRE(img->views.contains("tools:tools"));
    REQUIRE(img->default_views ==
            std::vector<std::string>{"prgenv-gnu:prgenv", "tools:tools"});
    auto c = uenv::concretise(*img, {});
    REQUIRE(c);
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    REQUIRE(env.get("PATH") ==
            "/user-environment/tools/bin:/user-environment/env/prgenv/bin");
}

TEST_CASE("1/merge: the same name needs an alias", "[model]") {
    std::vector<uenv::merge_item> items{{.image = image_of(prgenv())},
                                        {.image = image_of(prgenv())}};
    REQUIRE(!uenv::merge_items(items, digest_fn));
    items[0].alias = "gnu";
    // still the same mount point
    auto r = uenv::merge_items(items, digest_fn);
    REQUIRE(!r);
    REQUIRE(r.error().find("same mount point") != std::string::npos);
}

TEST_CASE("1/merge: an alias renames the include of each view", "[model]") {
    std::vector<uenv::merge_item> items{
        {.alias = "pe", .image = image_of(prgenv())},
        {.alias = "t", .mount = "/opt/t", .image = image_of(tools()),
         .movable = true}};
    auto img = uenv::merge_items(items, digest_fn);
    REQUIRE(img);
    REQUIRE(img->views.at("t:tools").include ==
            std::vector<std::string>{"t:tools"});
    REQUIRE(uenv::concretise(*img, {"t:tools", "pe:modules"}));
}

TEST_CASE("1/merge: only a layer can move", "[model]") {
    std::vector<uenv::merge_item> items{
        {.image = image_of(prgenv())},
        {.mount = "/opt/t", .image = image_of(tools()), .movable = false}};
    auto r = uenv::merge_items(items, digest_fn);
    REQUIRE(!r);
    REQUIRE(r.error().find("only a layer") != std::string::npos);
}

TEST_CASE("1/merge: an image with more than one layer cannot be merged",
          "[model]") {
    auto big = uenv::make_composed_image(std::nullopt, my_env_config(),
                                         my_env_layers(), my_env_views(), "dev");
    std::vector<uenv::merge_item> items{
        {.image = *big},
        {.image = image_of(make_layer("x", "/x", false, {}))}};
    auto r = uenv::merge_items(items, digest_fn);
    REQUIRE(!r);
    REQUIRE(r.error().find("more than one layer") != std::string::npos);
}

TEST_CASE("1/merge: one item is the item itself", "[model]") {
    auto big = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                          my_env_layers(), my_env_views(),
                                          "dev");
    big.digest = format::image_digest(big);
    auto r = uenv::merge_items({{.image = big}}, digest_fn);
    REQUIRE(r);
    REQUIRE(r->digest == big.digest);
    REQUIRE(r->views.contains("dev"));
}

TEST_CASE("1/merge: the digest of a merged list", "[model]") {
    std::vector<uenv::merge_item> one_default{
        {.image = image_of(prgenv())},
        {.image = image_of(make_layer("x", "/x", false, {{"v", {}}}))}};
    auto a = uenv::merge_items(one_default, digest_fn);
    REQUIRE(a);
    REQUIRE(a->digest);

    // each item has a default view: the merge has two
    std::vector<uenv::merge_item> two_defaults{{.image = image_of(prgenv())},
                                               {.image = image_of(tools())}};
    auto b = uenv::merge_items(two_defaults, digest_fn);
    REQUIRE(b);
    // FINDING (merge-default-views): an image config holds one default view,
    // so the image that uenv "would write" does not exist, and the merge has
    // no digest.
    REQUIRE(!b->digest);

    // the fix: the identity of a merge leaves out the default views
    auto fixed = uenv::merge_items(two_defaults, uenv::merged_digest);
    REQUIRE(fixed->digest);
    REQUIRE(fixed->default_views.size() == 2u);
    // the same list in a different order: the same identity
    std::vector<uenv::merge_item> reversed{two_defaults[1], two_defaults[0]};
    REQUIRE(uenv::merge_items(reversed, uenv::merged_digest)->digest ==
            fixed->digest);
}

//
// 1-composition "Views"
//

TEST_CASE("1/patches: the operations on a prefix path", "[env]") {
    envvars::state env;
    env.set("P", "/a:/b");
    envvars::patch p;
    p.update_prefix_path("P", {envvars::update_kind::prepend, {"/x"}});
    p.update_prefix_path("P", {envvars::update_kind::append, {"/y", "/a"}});
    uenv::apply_patches(env, {p});
    // duplicates removed, first kept
    REQUIRE(env.get("P") == "/x:/a:/b:/y");

    envvars::patch s;
    s.update_prefix_path("P", {envvars::update_kind::set, {"/only", "", "/only"}});
    uenv::apply_patches(env, {s});
    REQUIRE(env.get("P") == "/only");

    envvars::patch u;
    u.update_prefix_path("P", {envvars::update_kind::unset, {}});
    uenv::apply_patches(env, {u});
    REQUIRE(!env.get("P"));
}

TEST_CASE("1/patches: values set and unset", "[env]") {
    envvars::state env;
    env.set("A", "1");
    envvars::patch p;
    p.update_scalar("A", std::nullopt);
    p.update_scalar("B", "2");
    uenv::apply_patches(env, {p});
    REQUIRE(!env.get("A"));
    REQUIRE(env.get("B") == "2");
}

TEST_CASE("1/variables in a value", "[env]") {
    envvars::state env;
    env.set("HOME", "/home/x");
    SECTION("a variable of the calling environment") {
        envvars::patch p;
        p.update_scalar("CFG", "${@HOME@}/.config/tools");
        uenv::apply_patches(env, {p});
        REQUIRE(env.get("CFG") == "/home/x/.config/tools");
    }
    SECTION("a patch cannot see a variable that it sets") {
        envvars::patch p;
        p.update_scalar("ROOT", "/r");
        p.update_scalar("BIN", "${@ROOT@}/bin");
        auto rep = uenv::apply_patches(env, {p});
        REQUIRE(env.get("BIN") == "/bin");
        REQUIRE(rep.warnings.size() == 1u);
    }
    SECTION("a later view sees a variable that an earlier view sets") {
        envvars::patch a;
        a.update_scalar("ROOT", "/r");
        envvars::patch b;
        b.update_scalar("BIN", "${@ROOT@}/bin");
        uenv::apply_patches(env, {a, b});
        REQUIRE(env.get("BIN") == "/r/bin");
    }
    SECTION("an invalid name is not replaced") {
        envvars::patch p;
        p.update_scalar("X", "a${@not valid@}b");
        auto rep = uenv::apply_patches(env, {p});
        REQUIRE(env.get("X") == "a${@not valid@}b");
        REQUIRE(rep.warnings.size() == 1u);
    }
    SECTION("a missing @} drops the rest of the value") {
        envvars::patch p;
        p.update_scalar("X", "a${@HOME");
        auto rep = uenv::apply_patches(env, {p});
        REQUIRE(env.get("X") == "a");
        REQUIRE(rep.errors.size() == 1u);
    }
    SECTION("${...} is not expanded") {
        envvars::patch p;
        p.update_scalar("X", "${HOME}");
        uenv::apply_patches(env, {p});
        REQUIRE(env.get("X") == "${HOME}");
    }
    SECTION("in a prefix path") {
        envvars::patch p;
        p.update_prefix_path("PATH", {envvars::update_kind::prepend,
                                      {"${@HOME@}/bin"}});
        uenv::apply_patches(env, {p});
        REQUIRE(env.get("PATH") == "/home/x/bin");
    }
    SECTION("the calling environment is not expanded") {
        env.set("PATH", "/odd/${@HOME@}");
        envvars::patch p;
        p.update_prefix_path("PATH", {envvars::update_kind::append, {"/x"}});
        uenv::apply_patches(env, {p});
        REQUIRE(env.get("PATH") == "/odd/${@HOME@}:/x");
    }
}

TEST_CASE("1/load more than one view: in order", "[env]") {
    envvars::state env;
    auto a = prepend("PATH", "/a");
    auto b = prepend("PATH", "/b");
    uenv::apply_patches(env, {a, b});
    REQUIRE(env.get("PATH") == "/b:/a");
    envvars::state env2;
    uenv::apply_patches(env2, {b, a});
    REQUIRE(env2.get("PATH") == "/a:/b");
    // a later set removes the earlier paths
    envvars::patch s;
    s.update_prefix_path("PATH", {envvars::update_kind::set, {"/s"}});
    uenv::apply_patches(env, {s});
    REQUIRE(env.get("PATH") == "/s");
}

TEST_CASE("1/the mount point in a layer view", "[env]") {
    auto p = set_value("X", "-DMOUNT=@uenv-root@\t-I@uenv-root@/include");
    auto r = uenv::replace_root(p, "/opt/t");
    REQUIRE(r.scalars().at("X").value == "-DMOUNT=/opt/t\t-I/opt/t/include");
}

//
// 1-composition "Concretisation"
//

TEST_CASE("1/concretise: the views", "[concretise]") {
    auto img = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                          my_env_layers(), my_env_views(),
                                          "dev");
    SECTION("the default view") {
        auto c = uenv::concretise(img, {});
        REQUIRE(c);
        // two layer views and the env patch of dev
        REQUIRE(c->patches.size() == 3u);
        envvars::state env;
        uenv::apply_patches(env, c->patches);
        REQUIRE(env.get("MY_TOOLS_ROOT") == "/user-environment/tools");
        REQUIRE(env.get("PATH") ==
                "/user-environment/tools/bin:/user-environment/env/prgenv/bin");
    }
    SECTION("a layer view by its full name") {
        auto c = uenv::concretise(img, {"tools:tools"});
        REQUIRE(c);
        REQUIRE(c->patches.size() == 1u);
    }
    SECTION("an image view and a layer view") {
        auto c = uenv::concretise(img, {"modules", "tools:tools"});
        REQUIRE(c);
        auto s = uenv::session_patch(*c, {"modules", "tools:tools"});
        REQUIRE(s.scalars().at("UENV_VIEW").value == "modules,tools:tools");
        REQUIRE(s.scalars().at("UENV_MOUNT_LIST").value ==
                "/blobs/prgenv-gnu:/user-environment,/blobs/tools:/user-"
                "environment/tools");
    }
    SECTION("a name that is not a view") {
        auto c = uenv::concretise(img, {"nope"});
        REQUIRE(!c);
        REQUIRE(c.error().find("dev") != std::string::npos);
    }
    SECTION("no default view: no view") {
        img.default_views.clear();
        auto c = uenv::concretise(img, {});
        REQUIRE(c);
        REQUIRE(c->patches.empty());
    }
    SECTION("a layer with no local file") {
        img.layers[1].layer.store.file.reset();
        auto c = uenv::concretise(img, {});
        REQUIRE(!c);
        REQUIRE(c.error().find("pull") != std::string::npos);
    }
}

//
// 2-oci-layout "The index entries" and "Objects and media types"
//

namespace {
json my_env_index() {
    auto img = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                          my_env_layers(), my_env_views(),
                                          "dev");
    return json::parse(format::make_image_objects(img)->index.bytes);
}
} // namespace

TEST_CASE("2/objects: the index of my-env", "[oci-layout]") {
    auto j = my_env_index();
    REQUIRE(j["mediaType"] == "application/vnd.oci.image.index.v1+json");
    REQUIRE(j["artifactType"] == "application/vnd.uenv.image.v1");
    REQUIRE(j["manifests"].size() == 3u);
    REQUIRE(j["manifests"][0]["artifactType"] ==
            "application/vnd.uenv.image-config.v1");
    REQUIRE(j["manifests"][1]["annotations"]["vnd.uenv.layer"] == "prgenv-gnu");
    REQUIRE(j["manifests"][2]["annotations"]["vnd.uenv.mount"] ==
            "/user-environment/tools");
    REQUIRE(!j.contains("platform"));
    REQUIRE(format::parse_index(j.dump()));
}

TEST_CASE("2/objects: the wrapper uses the OCI empty descriptor", "[oci-layout]") {
    auto img = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                          my_env_layers(), my_env_views(),
                                          "dev");
    auto w = json::parse(format::make_image_objects(img)->wrapper.bytes);
    REQUIRE(w["layers"].size() == 1u);
    REQUIRE(w["layers"][0]["data"] == "e30=");
    REQUIRE(w["layers"][0]["size"] == 2);
}

TEST_CASE("2/objects: the squashfs descriptor has no annotations",
          "[oci-layout]") {
    auto m = json::parse(format::make_layer_objects(tools())->manifest.bytes);
    REQUIRE(m["layers"].size() == 1u);
    REQUIRE(m["layers"][0]["mediaType"] == "application/vnd.uenv.squashfs.v1");
    REQUIRE(!m["layers"][0].contains("annotations"));
    REQUIRE(!m.contains("annotations"));
}

TEST_CASE("2/index entries: an index that does not have the form is an error",
          "[oci-layout]") {
    auto good = my_env_index();
    SECTION("no wrapper") {
        auto j = good;
        j["manifests"].erase(0);
        REQUIRE(!format::parse_index(j.dump()));
    }
    SECTION("more than one wrapper") {
        auto j = good;
        j["manifests"][2] = j["manifests"][0];
        REQUIRE(!format::parse_index(j.dump()));
    }
    SECTION("a wrapper that is not the first entry") {
        auto j = good;
        std::swap(j["manifests"][0], j["manifests"][1]);
        auto r = format::parse_index(j.dump());
        REQUIRE(!r);
    }
    SECTION("no layer entries") {
        auto j = good;
        j["manifests"] = json::array({good["manifests"][0]});
        REQUIRE(!format::parse_index(j.dump()));
    }
    SECTION("a layer entry without an alias") {
        auto j = good;
        j["manifests"][1]["annotations"].erase("vnd.uenv.layer");
        REQUIRE(!format::parse_index(j.dump()));
    }
    SECTION("a layer entry without a mount point") {
        auto j = good;
        j["manifests"][1]["annotations"].erase("vnd.uenv.mount");
        REQUIRE(!format::parse_index(j.dump()));
    }
    SECTION("an index that points at an index") {
        auto j = good;
        j["manifests"][1]["mediaType"] =
            "application/vnd.oci.image.index.v1+json";
        REQUIRE(!format::parse_index(j.dump()));
    }
    SECTION("an index of a later version") {
        auto j = good;
        j["artifactType"] = "application/vnd.uenv.image.v2";
        auto r = format::parse_index(j.dump());
        REQUIRE(!r);
        REQUIRE(r.error().find("later version") != std::string::npos);
    }
    SECTION("the layers in any order are accepted") {
        auto j = good;
        std::swap(j["manifests"][1], j["manifests"][2]);
        REQUIRE(format::parse_index(j.dump()));
    }
}

TEST_CASE("2/identity: no content depends on time", "[oci-layout]") {
    auto a = my_env_index().dump();
    auto b = my_env_index().dump();
    REQUIRE(a == b);
    REQUIRE(a.find("created") == std::string::npos);
}

//
// 2-oci-layout "Tags"
//

TEST_CASE("2/tags: the form of a tag", "[oci-layout]") {
    auto d = *oci::digest::parse(
        "sha256:3fa9c2e1d04b7a65aaaabbbbccccddddeeeeffff000011112222333344445555");
    uenv::config c{.name = "prgenv-gnu", .version = "26.3", .release = "v1",
                   .meta = {{"uarch", "gh200"}, {"system", "daint"}}};
    REQUIRE(format::make_tag(c, d) == "26.3-v1-daint-gh200-3fa9c2e1d04b7a65");
    c.version = "1.0+cuda";
    REQUIRE(format::make_tag(c, d).starts_with("1.0_cuda-v1"));
    c.version = std::string(300, 'x');
    auto t = format::make_tag(c, d);
    REQUIRE(t.size() <= 128u);
    REQUIRE(t.ends_with("-3fa9c2e1d04b7a65"));
    c.version = ".hidden";
    REQUIRE(format::make_tag(c, d)[0] == '_');
}

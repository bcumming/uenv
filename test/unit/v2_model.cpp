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
    REQUIRE(c->shared.name == nm("tools"));
    REQUIRE(c->relocatable);
    REQUIRE(c->shared.meta.at(nm("built-on")) == "eiger");
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
    REQUIRE(c->views.at(nm("dev")).include.size() == 2u);
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

TEST_CASE("2/configs: view references are parsed", "[config]") {
    // an include entry is parsed into the alias and the view
    auto c = format::parse_image_config(image_config_json().dump());
    REQUIRE(c);
    REQUIRE(c->views.at(nm("dev")).include ==
            refs({"prgenv-gnu:prgenv", "tools:tools"}));
    REQUIRE(c->views.at(nm("dev")).include[1].alias == nm("tools"));
    REQUIRE(c->views.at(nm("dev")).include[1].view == nm("tools"));
    // an include entry that is not alias:view
    for (auto bad : {"tools", "tools:", ":tools", "a:b:c", "a b:c"}) {
        auto j = image_config_json();
        j["views"]["dev"]["include"] = {bad};
        INFO(bad);
        REQUIRE(!format::parse_image_config(j.dump()));
    }
    // the name of a view has no alias
    auto j = image_config_json();
    j["views"]["a:modules"] = j["views"]["modules"];
    REQUIRE(!format::parse_image_config(j.dump()));
    auto l = layer_config_json();
    l["views"]["a:tools"] = l["views"]["tools"];
    REQUIRE(!format::parse_layer_config(l.dump()));
}

TEST_CASE("2/configs: names are parsed where the config is read", "[config]") {
    auto expect_error = [](json j, bool layer, std::string_view text) {
        auto body = j.dump();
        auto e = layer ? format::parse_layer_config(body).error()
                       : format::parse_image_config(body).error();
        INFO(e);
        REQUIRE(e.find(text) != std::string::npos);
    };
    // the shared fields, the meta keys and the view names
    auto l = layer_config_json();
    l["name"] = "my tools";
    expect_error(l, true, "'my tools' is not a valid name");
    l = layer_config_json();
    l["release"] = "v1+cuda";
    expect_error(l, true, "the field 'release'");
    l = layer_config_json();
    l["meta"]["built on"] = "x";
    expect_error(l, true, "the meta key 'built on'");
    l = layer_config_json();
    l["views"]["my view"] = l["views"]["tools"];
    expect_error(l, true, "the view name 'my view'");
    // a variable name in a patch
    l = layer_config_json();
    l["views"]["tools"]["env"]["scalar"]["MY-VAR"] = "x";
    expect_error(l, true, "'MY-VAR' is not a valid variable name");
    auto i = image_config_json();
    i["default_view"] = "a:b";
    expect_error(i, false, "the default view");
}

TEST_CASE("2/objects: inline data must match its digest", "[oci-layout]") {
    auto wrapper = format::make_wrapper_manifest(format::make_blob("{}"));
    REQUIRE(wrapper);
    REQUIRE(format::parse_wrapper_manifest(wrapper->bytes));
    auto j = json::parse(wrapper->bytes);
    j["config"]["data"] = "e30=";  // "{}" encoded: it matches
    REQUIRE(format::parse_wrapper_manifest(j.dump()));
    for (auto bad : {"eyJ9", "not base64!"}) {
        INFO(bad);
        auto k = json::parse(wrapper->bytes);
        k["config"]["data"] = bad;
        auto r = format::parse_wrapper_manifest(k.dump());
        REQUIRE(!r);
    }
}

TEST_CASE("6/the v1 code: a v1 env.json is parsed into the new types",
          "[config]") {
    uenv::meta m{.name = "tools", .mount = "/user-tools"};
    m.views["tools"] = uenv::concrete_view{.name = "tools"};
    m.default_view = "tools";
    REQUIRE(uenv::v1::make_layer_config(m, std::nullopt));
    auto bad = m;
    bad.views["a b"] = uenv::concrete_view{.name = "a b"};
    auto r = uenv::v1::make_layer_config(bad, std::nullopt);
    REQUIRE(!r);
    REQUIRE(r.error().find("the view name 'a b'") != std::string::npos);
    bad = m;
    bad.name = "my tools";
    REQUIRE(!uenv::v1::make_layer_config(bad, std::nullopt));
    bad = m;
    bad.mount = "/user tools";
    REQUIRE(!uenv::v1::make_layer_config(bad, std::nullopt));
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

//
// 1-composition "Checks"
//

namespace {
std::vector<uenv::named_layer> my_env_layers() {
    return {{nm("prgenv-gnu"), mp("/user-environment"), prgenv()},
            {nm("tools"), mp("/user-environment/tools"), tools()}};
}
std::map<uenv::name, uenv::image_view> my_env_views() {
    return named<uenv::image_view>(
        {{"dev",
          {.include = refs({"prgenv-gnu:prgenv", "tools:tools"}),
           .env = set_value("MY_TOOLS_ROOT", "/user-environment/tools")}},
         {"modules", {.include = refs({"prgenv-gnu:modules"})}}});
}
uenv::config my_env_config() {
    return cfg("my-env", "1.0", "v1", {{"system", "eiger"}});
}
} // namespace

TEST_CASE("1/checks: a valid image", "[model]") {
    auto img = uenv::make_composed_image(std::nullopt, my_env_config(),
                                         my_env_layers(), my_env_views(), nm("dev"));
    REQUIRE(img);
    REQUIRE(img->layers().front().alias == nm("prgenv-gnu"));
}

TEST_CASE("1/checks: each check of a composed_image", "[model]") {
    SECTION("two layers with the same alias") {
        auto ls = my_env_layers();
        ls[1].alias = nm("prgenv-gnu");
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("same alias") != std::string::npos);
    }
    SECTION("two layers with the same mount point") {
        auto ls = my_env_layers();
        ls[1].mount = mp("/user-environment");
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("same mount point") != std::string::npos);
    }
    SECTION("a layer that is not relocatable, at a different mount point") {
        auto ls = my_env_layers();
        ls[0].mount = mp("/opt/pe");
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                           {}, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("not relocatable") != std::string::npos);
    }
    SECTION("a relocatable layer can move") {
        auto ls = my_env_layers();
        ls[1].mount = mp("/opt/tools");
        REQUIRE(uenv::make_composed_image(std::nullopt, my_env_config(), ls,
                                          {}, std::nullopt));
    }
    SECTION("an image view that refers to an alias that does not exist") {
        auto v = my_env_views();
        v[nm("dev")].include.push_back(ref("nope:tools"));
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(),
                                           my_env_layers(), v, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("no layer with the alias") != std::string::npos);
    }
    SECTION("an image view that refers to a layer view that does not exist") {
        auto v = my_env_views();
        v[nm("dev")].include.push_back(ref("tools:nope"));
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(),
                                           my_env_layers(), v, std::nullopt);
        REQUIRE(!r);
        REQUIRE(r.error().find("has no view") != std::string::npos);
    }
    SECTION("a default_view that is not a view of the image") {
        auto r = uenv::make_composed_image(std::nullopt, my_env_config(),
                                           my_env_layers(), my_env_views(),
                                           nm("nope"));
        REQUIRE(!r);
    }
}

TEST_CASE("2/configs: a name has the grammar of a v1 label", "[config]") {
    using uenv::name;
    for (auto ok : {"prgenv-gnu", "24.11", "v1", "a_b.c-1", "2", "netcdf-c", "_a"}) {
        INFO(ok);
        REQUIRE(name::parse(ok));
        REQUIRE(name::parse(ok)->string() == ok);
    }
    for (auto bad : {"", "-a", ".a", "a b", "a:b", "a/b", "a+b", "a@b", "a,b",
                     "a=b", "a%b"}) {
        INFO(bad);
        auto n = name::parse(bad);
        REQUIRE(!n);
        REQUIRE(n.error().find(std::string("'") + bad + "'") != std::string::npos);
    }
    REQUIRE(*name::parse("a") < *name::parse("b"));
}

TEST_CASE("2/paths: a path that a user gives is parsed, and made absolute",
          "[config]") {
    auto p = uenv::parse_file_path("/opt/images/store.squashfs");
    REQUIRE(p);
    REQUIRE(*p == "/opt/images/store.squashfs");
    auto r = uenv::parse_file_path("./images/../store.squashfs");
    REQUIRE(r);
    REQUIRE(r->is_absolute());
    REQUIRE(*r == std::filesystem::current_path() / "store.squashfs");
    for (auto bad : {"", "store squashfs", "images/a", "/a:b", "~/x"}) {
        INFO(bad);
        auto e = uenv::parse_file_path(bad);
        REQUIRE(!e);
        REQUIRE(e.error().find(std::string("'") + bad + "'") != std::string::npos);
    }
}

TEST_CASE("2/versions: a media type with a very long version does not throw",
          "[config]") {
    auto r = format::check_media_type(
        "application/vnd.uenv.layer-config.v99999999999999999999+json",
        format::mt_layer_config);
    REQUIRE(!r);
}

TEST_CASE("1/mount points: the form that UENV_MOUNT_LIST can hold", "[model]") {
    using uenv::mount_point;
    for (auto ok : {"/", "/user-environment", "/user-environment/tools",
                    "/opt/a_b-1.2"}) {
        INFO(ok);
        REQUIRE(mount_point::parse(ok));
        REQUIRE(mount_point::parse(ok)->string() == ok);
    }
    for (auto bad : {"", "user-environment", "./tools", "/a b", "/a,b", "/a:b",
                     "/a/", "/a//b", "/a/./b", "/a/../b", "/scratch/$USER",
                     "/@uenv-root@", "/a=b", "/a+b", "/a@b"}) {
        INFO(bad);
        REQUIRE(!mount_point::parse(bad));
    }
    // the limits of the setuid helper
    REQUIRE(!mount_point::parse("/" + std::string(300, 'a')));
    // a nested mount point
    auto pe = mp("/user-environment");
    auto tools = mp("/user-environment/tools");
    REQUIRE(tools.is_inside(pe));
    REQUIRE(!pe.is_inside(tools));
    REQUIRE(!mp("/user-environment2").is_inside(pe));
    REQUIRE(tools.relative_to(pe) == "tools");
    REQUIRE(tools.is_inside(mp("/")));
    // the sort order is the byte order of the text
    REQUIRE(mp("/a-b") < mp("/a/b"));
}

TEST_CASE("1/order of layers: the order has no meaning", "[model]") {
    // three layers, all permutations: the same digest, mounts and env
    auto extra = make_layer("extra", "/user-tools", true,
                            {{"x", set_value("X", "@uenv-root@")}}, "x");
    std::vector<uenv::named_layer> layers = my_env_layers();
    layers.push_back({nm("extra"), mp("/user-tools"), extra});
    auto views = my_env_views();
    views[nm("dev")].include.push_back(ref("extra:x"));

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
                                             views, nm("dev"));
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
            mounts.push_back(m.string());
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
    std::vector<uenv::named_layer> ls{{nm("a"), mp("/opt/a"), t}, {nm("b"), mp("/opt/b"), t}};
    auto img = uenv::make_composed_image(
        std::nullopt, my_env_config(), ls,
        named<uenv::image_view>({{"both", {.include = refs({"a:tools", "b:tools"})}}}),
        nm("both"));
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
    REQUIRE(img->digest() == t.digest());
    REQUIRE(img->kind() == uenv::image_kind::layer);
    REQUIRE(img->layers().size() == 1u);
    REQUIRE(img->layers()[0].alias == nm("tools"));
    REQUIRE(img->views().at(vname("tools")).include ==
            refs({"tools:tools"}));
    REQUIRE(img->default_views() == vnames({"tools"}));
    REQUIRE(img->config() == t.config);
    // two raises give the same result
    auto again = uenv::raise_layer(t, std::nullopt, false);
    REQUIRE(format::image_digest(*again) == format::image_digest(*img));
}

TEST_CASE("1/raise: a different mount point is an input to the raise",
          "[model]") {
    auto img = uenv::raise_layer(tools(), mp("/opt/tools"), false);
    REQUIRE(img);
    REQUIRE(img->layers()[0].mount == mp("/opt/tools"));
    auto c = uenv::concretise(*img, {});
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    REQUIRE(env.get("PATH") == "/opt/tools/bin");
}

TEST_CASE("1/raise: --force mounts a layer that is not relocatable",
          "[model]") {
    REQUIRE(!uenv::raise_layer(prgenv(), mp("/opt/pe"), false));
    auto img = uenv::raise_layer(prgenv(), mp("/opt/pe"), true);
    REQUIRE(img);
    REQUIRE(img->layers()[0].mount == mp("/opt/pe"));
}

//
// 1-composition "Runtime merge"
//

namespace {
uenv::composed_image image_of(uenv::layer l, std::map<uenv::name, std::string> meta = {}) {
    auto img = *uenv::promote(*uenv::raise_layer(l, std::nullopt, false), meta);
    return img.identified_by(*format::image_digest(img));
}
// my-env: prgenv-gnu at /user-environment, tools at /user-environment/tools
uenv::composed_image my_env() {
    auto img = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                          my_env_layers(), my_env_views(),
                                          nm("dev"));
    return img.identified_by(*format::image_digest(img));
}
std::vector<std::string> aliases_of(const uenv::composed_image& img) {
    std::vector<std::string> out;
    for (auto& l : img.layers()) {
        out.push_back(l.alias.string());
    }
    return out;
}
} // namespace

TEST_CASE("1/promotion: only the raise of a layer is promoted", "[model]") {
    auto raised = *uenv::raise_layer(tools());
    auto img = uenv::promote(raised, named<std::string>({{"system", "daint"}}));
    REQUIRE(img);
    REQUIRE(img->kind() == uenv::image_kind::image);
    REQUIRE(!img->digest());
    REQUIRE(img->config().meta.at(nm("system")) == "daint");
    // an image is not promoted again
    REQUIRE(!uenv::promote(*img, {}));
}

TEST_CASE("4/copy: the changes of --set are parsed, then checked", "[model]") {
    auto c = uenv::parse_config_changes("release=v2,meta.system=santis,meta.uarch=");
    REQUIRE(c);
    REQUIRE(c->size() == 3u);
    for (auto bad : {"release=", "release", "name=x", "size=3", "meta.a b=x",
                     "release=v2+x", "release=v2,"}) {
        INFO(bad);
        auto e = uenv::parse_config_changes(bad);
        REQUIRE(!e);
        REQUIRE(e.error().find(bad) != std::string::npos);
    }
    auto img = my_env();
    auto changed = uenv::with_changes(img, *c);
    REQUIRE(changed);
    REQUIRE(changed->config().release == nm("v2"));
    REQUIRE(changed->config().meta.at(nm("system")) == "santis");
    REQUIRE(!changed->config().meta.contains(nm("uarch")));
    // a new image: no digest until it is stored
    REQUIRE(!changed->digest());
    // only an image can be copied with changes
    REQUIRE(!uenv::with_changes(*uenv::raise_layer(tools()), *c));
}

TEST_CASE("1/runtime merge: two images", "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.image = image_of(prgenv())},
        {.image = image_of(tools())}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    REQUIRE(img->kind() == uenv::image_kind::runtime_merge);
    REQUIRE(img->layers().size() == 2u);
    REQUIRE(img->config().meta.empty());
    REQUIRE(img->views().contains(vname("prgenv-gnu:prgenv")));
    REQUIRE(img->views().contains(vname("tools:tools")));
    REQUIRE(img->default_views() ==
            vnames({"prgenv-gnu:prgenv", "tools:tools"}));
    auto c = uenv::concretise(*img, {});
    REQUIRE(c);
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    REQUIRE(env.get("PATH") ==
            "/user-environment/tools/bin:/user-environment/env/prgenv/bin");
}

TEST_CASE("1/runtime merge: no identity, and it cannot be stored", "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.image = image_of(prgenv())},
        {.image = image_of(tools())}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    REQUIRE(!img->digest());
    auto objs = format::make_image_objects(*img);
    REQUIRE(!objs);
    REQUIRE(objs.error().find("runtime merge cannot be stored") !=
            std::string::npos);
    REQUIRE(!format::image_digest(*img));
    // a runtime merge has no label: the description lists the items, in the
    // order of the list
    REQUIRE(img->config().name == nm("runtime-merge"));
    REQUIRE(img->config().description == "prgenv-gnu,tools");
    // the layers are sorted by mount point, whatever the order of the list
    auto reversed = uenv::runtime_merge({items[1], items[0]});
    REQUIRE(reversed);
    REQUIRE(aliases_of(*reversed) == aliases_of(*img));
}

TEST_CASE("1/runtime merge: two items with the same name need an alias",
          "[model]") {
    std::vector<uenv::runtime_merge_item> items{{.image = image_of(prgenv())},
                                                {.image = image_of(prgenv())}};
    auto r = uenv::runtime_merge(items);
    REQUIRE(!r);
    REQUIRE(r.error().find("give one of them an alias") != std::string::npos);
    // with an alias: the same layer at the same mount point is mounted once,
    // and each item keeps its views
    items[0].alias = nm("gnu");
    r = uenv::runtime_merge(items);
    REQUIRE(r);
    REQUIRE(r->layers().size() == 1u);
    REQUIRE(r->views().at(vname("gnu:prgenv")).include ==
            r->views().at(vname("prgenv-gnu:prgenv")).include);
}

TEST_CASE("1/runtime merge: two layers at one mount point", "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.image = image_of(prgenv())},
        {.image = image_of(make_layer("other", "/user-environment", false, {}))}};
    auto r = uenv::runtime_merge(items);
    REQUIRE(!r);
    REQUIRE(r.error().find("same mount point") != std::string::npos);
}

TEST_CASE("1/runtime merge: an alias renames the include of each view",
          "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.alias = nm("pe"), .image = image_of(prgenv())},
        {.alias = nm("t"), .mount = mp("/opt/t"), .image = image_of(tools()),
         .movable = true}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    REQUIRE(img->views().at(vname("t:tools")).include == refs({"t:tools"}));
    REQUIRE(uenv::concretise(*img, views("t:tools,pe:modules")));
}

TEST_CASE("1/runtime merge: an image view of an item is not a layer view",
          "[model]") {
    // an image with one layer, and an image view that adds a patch
    auto p = prgenv();
    auto pe = *uenv::make_composed_image(
        std::nullopt, my_env_config(), {{nm("prgenv-gnu"), p.mount, p}},
        named<uenv::image_view>({{"modules",
                                  {.include = refs({"prgenv-gnu:modules"}),
                                   .env = set_value("EXTRA", "1")}}}),
        nm("modules"));
    std::vector<uenv::runtime_merge_item> items{
        {.alias = nm("a"), .image = pe},
        {.image = image_of(tools())}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    // the image view is named by the alias of the item
    const auto key = uenv::image_view_name{.item = nm("a"), .name = nm("modules")};
    REQUIRE(img->views().contains(key));
    REQUIRE(img->views().at(key).include == refs({"a:modules"}));
    REQUIRE(img->default_views()[0] == key);
    // a:modules is the image view: the layer view, then the env patch
    auto c = uenv::concretise(*img, views("a:modules"));
    REQUIRE(c);
    REQUIRE(c->patches.size() == 2u);
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    REQUIRE(env.get("EXTRA") == "1");
    // a layer view with no image view of that name: a:prgenv
    c = uenv::concretise(*img, views("a:prgenv"));
    REQUIRE(c);
    REQUIRE(c->patches.size() == 1u);
}

TEST_CASE("1/runtime merge: only a layer can move", "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.image = image_of(prgenv())},
        {.mount = mp("/opt/t"), .image = image_of(tools()), .movable = false}};
    auto r = uenv::runtime_merge(items);
    REQUIRE(!r);
    REQUIRE(r.error().find("only a layer") != std::string::npos);
    // an image with more than one layer cannot move
    r = uenv::runtime_merge({{.mount = mp("/opt/x"), .image = my_env()},
                             {.image = image_of(make_layer("x", "/x", false, {}))}});
    REQUIRE(!r);
    REQUIRE(r.error().find("cannot move") != std::string::npos);
}

TEST_CASE("1/runtime merge: an image with more than one layer", "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.image = my_env()},
        {.image = image_of(make_layer("debugger", "/user-tools", false,
                                      {{"debug", prepend("PATH", "/user-tools/bin")}},
                                      "debug"))}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    REQUIRE(img->layers().size() == 3u);
    REQUIRE(aliases_of(*img) ==
            std::vector<std::string>{"prgenv-gnu", "tools", "debugger"});
    REQUIRE(img->views().at(vname("my-env:dev")).include ==
            refs({"prgenv-gnu:prgenv", "tools:tools"}));
    REQUIRE(img->default_views() == vnames({"my-env:dev", "debugger:debug"}));
    // a view name with no item: the one item that has a view of that name
    auto c = uenv::concretise(*img, views("dev,debug"));
    REQUIRE(c);
    envvars::state env;
    uenv::apply_patches(env, c->patches);
    REQUIRE(env.get("PATH") == "/user-tools/bin:/user-environment/tools/bin:"
                               "/user-environment/env/prgenv/bin");
    REQUIRE(env.get("MY_TOOLS_ROOT") == "/user-environment/tools");
    // a layer view of the multi-layer item, by its full name
    REQUIRE(uenv::concretise(*img, views("tools:tools")));
}

TEST_CASE("1/runtime merge: a clash of layer aliases renames the later layer",
          "[model]") {
    // the layer tools of my-env, and the layer tools moved to /opt/tools
    std::vector<uenv::runtime_merge_item> items{
        {.image = my_env()},
        {.mount = mp("/opt/tools"), .image = image_of(tools()), .movable = true}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    REQUIRE(aliases_of(*img) ==
            std::vector<std::string>{"tools.tools", "prgenv-gnu", "tools"});
    // the views of each item refer to the layers of that item
    REQUIRE(img->views().at(vname("tools:tools")).include ==
            refs({"tools.tools:tools"}));
    REQUIRE(img->views().at(vname("my-env:dev")).include ==
            refs({"prgenv-gnu:prgenv", "tools:tools"}));
    envvars::state env;
    uenv::apply_patches(env, uenv::concretise(*img, views("tools:tools"))->patches);
    REQUIRE(env.get("PATH") == "/opt/tools/bin");
}

TEST_CASE("1/runtime merge: a view name that more than one item has",
          "[model]") {
    std::vector<uenv::runtime_merge_item> items{
        {.image = image_of(prgenv())},
        {.image = image_of(make_layer("other", "/other", false,
                                      {{"prgenv", {}}}))}};
    auto img = uenv::runtime_merge(items);
    REQUIRE(img);
    auto c = uenv::concretise(*img, views("prgenv"));
    REQUIRE(!c);
    REQUIRE(c.error().find("prgenv-gnu:prgenv") != std::string::npos);
    REQUIRE(c.error().find("other:prgenv") != std::string::npos);
    // a name that only one item has
    REQUIRE(uenv::concretise(*img, views("modules")));
}

TEST_CASE("1/runtime merge: one item is the item itself", "[model]") {
    auto big = my_env();
    auto r = uenv::runtime_merge({{.image = big}});
    REQUIRE(r);
    REQUIRE(r->kind() == uenv::image_kind::image);
    REQUIRE(r->digest() == big.digest());
    REQUIRE(r->views().contains(vname("dev")));
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
    SECTION("an invalid name is kept as text, with one warning for each name") {
        envvars::patch p;
        p.update_scalar("X", "a${@not valid@}b");
        p.update_scalar("Y", "${@not valid@}:${@MY-ROOT@}:${@not valid@}");
        envvars::patch q;
        q.update_prefix_path("PATH", {envvars::update_kind::prepend,
                                      {"${@MY-ROOT@}/bin"}});
        auto rep = uenv::apply_patches(env, {p, q});
        REQUIRE(env.get("X") == "a${@not valid@}b");
        REQUIRE(env.get("Y") == "${@not valid@}:${@MY-ROOT@}:${@not valid@}");
        REQUIRE(env.get("PATH")->starts_with("${@MY-ROOT@}/bin"));
        // two invalid names: two warnings
        REQUIRE(rep.warnings.size() == 2u);
        REQUIRE(rep.warnings[0].find("not valid") != std::string::npos);
        REQUIRE(rep.warnings[1].find("MY-ROOT") != std::string::npos);
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
    auto r = uenv::replace_root(p, mp("/opt/t"));
    REQUIRE(r.scalars().at("X").value == "-DMOUNT=/opt/t\t-I/opt/t/include");
}

//
// 1-composition "Concretisation"
//

TEST_CASE("1/concretise: the views", "[concretise]") {
    auto img = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                          my_env_layers(), my_env_views(),
                                          nm("dev"));
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
        auto c = uenv::concretise(img, views("tools:tools"));
        REQUIRE(c);
        REQUIRE(c->patches.size() == 1u);
    }
    SECTION("an image view and a layer view") {
        auto c = uenv::concretise(img, views("modules,tools:tools"));
        REQUIRE(c);
        auto s = uenv::session_patch(*c, c->views);
        REQUIRE(s.scalars().at("UENV_VIEW").value == "modules,tools:tools");
        REQUIRE(s.scalars().at("UENV_MOUNT_LIST").value ==
                "/blobs/prgenv-gnu:/user-environment,/blobs/tools:/user-"
                "environment/tools");
    }
    SECTION("a name that is not a view") {
        auto c = uenv::concretise(img, views("nope"));
        REQUIRE(!c);
        REQUIRE(c.error().find("dev") != std::string::npos);
    }
    SECTION("no default view: no view") {
        auto none = *uenv::make_composed_image(std::nullopt, my_env_config(),
                                               my_env_layers(), my_env_views(),
                                               std::nullopt);
        auto c = uenv::concretise(none, {});
        REQUIRE(c);
        REQUIRE(c->patches.empty());
    }
    SECTION("--no-default-view: no view") {
        auto c = uenv::concretise(img, {}, false);
        REQUIRE(c);
        REQUIRE(c->patches.empty());
        REQUIRE(c->views.empty());
    }
    SECTION("the default view is in the list of views") {
        auto c = uenv::concretise(img, {});
        REQUIRE(c);
        REQUIRE(c->views.size() == 1u);
        REQUIRE(uenv::view_string(c->views[0]) == "dev");
    }
    SECTION("a layer with no local file") {
        auto ls = my_env_layers();
        ls[1].layer.store.file.reset();
        auto unpulled = *uenv::make_composed_image(
            std::nullopt, my_env_config(), ls, my_env_views(), nm("dev"));
        auto c = uenv::concretise(unpulled, {});
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
                                          nm("dev"));
    return json::parse(format::make_image_objects(img)->index.bytes);
}
} // namespace

TEST_CASE("1/the composed_image model: a layer refers to its manifest by "
          "descriptor",
          "[oci-layout]") {
    auto l = prgenv();
    REQUIRE(l.manifest);
    REQUIRE(l.manifest->size == format::make_layer_objects(l)->manifest.size());
    REQUIRE(l.manifest->artifact_type == std::string(format::at_layer));
    REQUIRE(l.digest() == l.manifest->digest);
    // a layer manifest that a different tool wrote: other bytes, so a size
    // that a new encoding of the layer config does not give. The index entry
    // is the stored descriptor.
    l.manifest = format::layer_descriptor(
        oci::digest::sha256(util::sha256_string("other bytes")), 1234);
    auto img = uenv::make_composed_image(
        std::nullopt, my_env_config(), {{nm("prgenv-gnu"), l.mount, l}}, {},
        std::nullopt);
    REQUIRE(img);
    auto objs = format::make_image_objects(*img);
    REQUIRE(objs);
    auto idx = format::parse_index(objs->index.bytes);
    REQUIRE(idx);
    REQUIRE(idx->layers.size() == 1u);
    REQUIRE(idx->layers[0].manifest == *l.manifest);
}

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
                                          nm("dev"));
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
    SECTION("a layer entry with an alias that is not a name") {
        auto j = good;
        j["manifests"][1]["annotations"]["vnd.uenv.layer"] = "tools:2";
        auto r = format::parse_index(j.dump());
        REQUIRE(!r);
        REQUIRE(r.error().find("'tools:2' is not a valid name") != std::string::npos);
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
    auto c = cfg("prgenv-gnu", "26.3", "v1", {{"uarch", "gh200"}, {"system", "daint"}});
    REQUIRE(format::make_tag(c, d) == "26.3-v1-daint-gh200-3fa9c2e1d04b7a65");
    // a meta value can have characters that a tag cannot
    c.meta[nm("system")] = "daint+alps";
    REQUIRE(format::make_tag(c, d).starts_with("26.3-v1-daint_alps"));
    c.version = nm(std::string(300, 'x'));
    auto t = format::make_tag(c, d);
    REQUIRE(t.size() <= 128u);
    REQUIRE(t.ends_with("-3fa9c2e1d04b7a65"));
}

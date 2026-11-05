// Tests of the local repository (3-repository.md), and of the local
// operations: add (promote) and compose.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <catch2/catch_all.hpp>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/ops.h>
#include <uenv/session.h>
#include <uenv/store.h>
#include <util/fs.h>
#include <util/sqlite.h>

#include "v2_helpers.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace v2test;
namespace format = uenv::format;

namespace {

fs::path tmpdir() {
    return *util::make_temp_dir();
}

// a fake squashfs file: the store does not read its content
fs::path fake_sqfs(const fs::path& dir, const std::string& name,
                   std::size_t size = 10000) {
    std::string content(size, 'x');
    content.replace(0, name.size(), name);
    return write_file(dir / (name + ".squashfs"), content);
}

uenv::layer_config cfg_of(uenv::layer l) {
    return format::layer_config_of(l);
}

uenv::store new_store() {
    return *uenv::store::create(tmpdir() / "repo");
}

// run f in a child process with UENV_TEST_FAULT set: returns the exit status.
// The child writes no stderr, so that the message of the hook is not in the
// output of the tests: the test checks the exit status.
template <typename F> int in_child_with_fault(const char* point, F&& f) {
    auto pid = fork();
    if (pid == 0) {
        if (int null = open("/dev/null", O_WRONLY); null >= 0) {
            dup2(null, STDERR_FILENO);
            close(null);
        }
        setenv("UENV_TEST_FAULT", point, 1);
        f();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void make_old(const fs::path& p) {
    fs::last_write_time(p, fs::file_time_type::clock::now() -
                               std::chrono::hours(48));
}

} // namespace

TEST_CASE("3/layout: a new repository", "[store]") {
    auto root = tmpdir() / "repo";
    auto s = uenv::store::create(root);
    REQUIRE(s);
    REQUIRE(fs::is_regular_file(root / "v2" / "oci-layout"));
    REQUIRE(fs::is_regular_file(root / "v2" / "index.json"));
    REQUIRE(fs::is_regular_file(root / "v2" / "index.db"));
    REQUIRE(fs::is_directory(root / "v2" / "blobs" / "sha256"));
    REQUIRE(fs::is_directory(root / "v2" / "tmp"));
    auto db = util::sqlite::database::open(root / "v2" / "index.db",
                                           util::sqlite::open_mode::readonly);
    REQUIRE(*db->user_version() == 2);
}

TEST_CASE("3/layout: a repository with only a v1 part is refused", "[store]") {
    auto root = tmpdir() / "repo";
    write_file(root / "index.db", "");
    fs::create_directories(root / "images");
    auto s = uenv::store::open(root);
    REQUIRE(!s);
    REQUIRE(s.error().find("uenv repo migrate") != std::string::npos);
    REQUIRE(!uenv::store::create(root));
}

TEST_CASE("3/layout: a database of an unknown layout version is refused",
          "[store]") {
    auto root = tmpdir() / "repo";
    REQUIRE(uenv::store::create(root));
    {
        auto db = util::sqlite::database::open(
            root / "v2" / "index.db", util::sqlite::open_mode::readwrite);
        REQUIRE(db->set_user_version(3));
    }
    auto s = uenv::store::open(root);
    REQUIRE(!s);
    REQUIRE(s.error().find("layout version 3") != std::string::npos);
}

TEST_CASE("3/add: add promotes a squashfs file to an image", "[store]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto r = uenv::add_squashfs(s, fake_sqfs(dir, "tools"), cfg_of(tools()),
                                named<std::string>({{"system", "daint"}}), false);
    REQUIRE(r);
    REQUIRE(r->kind == format::object_kind::image);
    REQUIRE(r->cfg.meta.at(nm("system")) == "daint");
    REQUIRE(r->cfg.meta.at(nm("uarch")) == "zen2");
    REQUIRE(r->layers.size() == 1u);
    REQUIRE(r->layers[0].cfg.meta.count(nm("system")) == 0u);
    // five objects + the empty blob
    std::size_t n = 0;
    for (auto& e : fs::directory_iterator(s.v2() / "blobs" / "sha256")) {
        (void)e;
        ++n;
    }
    REQUIRE(n == 7u);
    // the image config views are the views of the raise
    auto img = s.load(r->digest);
    REQUIRE(img);
    REQUIRE(img->views().at(vname("tools")).include ==
            refs({"tools:tools"}));
    REQUIRE(img->kind() != uenv::image_kind::layer);
    REQUIRE(img->layers()[0].layer.store.file ==
            s.blob_path(*img->layers()[0].layer.store.digest));
    // the squashfs is mounted from the blob store
    auto c = uenv::concretise(*img, {});
    REQUIRE(c);
    REQUIRE(c->mounts[0].first.string().find("/v2/blobs/sha256/") !=
            std::string::npos);
}

TEST_CASE("3/add: the same file gives the same identity", "[store]") {
    auto dir = tmpdir();
    auto f = fake_sqfs(dir, "tools");
    auto s1 = new_store();
    auto s2 = new_store();
    auto a = uenv::add_squashfs(s1, f, cfg_of(tools()), {}, false);
    auto b = uenv::add_squashfs(s2, f, cfg_of(tools()), {}, false);
    REQUIRE(a->digest == b->digest);
}

TEST_CASE("3/add: a release is required", "[store]") {
    auto s = new_store();
    auto c = cfg_of(tools());
    c.shared.release = std::nullopt;
    REQUIRE(!uenv::add_squashfs(s, fake_sqfs(tmpdir(), "t"), c, {}, false));
}

TEST_CASE("3/database: lookup by label, by id, and layers", "[store]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto daint = uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                                    named<std::string>({{"system", "daint"}}), false);
    auto santis = uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                                     named<std::string>({{"system", "santis"}}), false);
    auto layer = uenv::add_squashfs(s, fake_sqfs(dir, "tools"), cfg_of(tools()),
                                    {}, true);
    REQUIRE(daint);
    REQUIRE(santis);
    REQUIRE(layer);
    REQUIRE(daint->digest != santis->digest);
    // the images of one build share the layer
    REQUIRE(daint->layers[0].digest == santis->layers[0].digest);

    uenv::label_keys keys;
    auto q = [&](std::string text, std::optional<std::string> sys = {}) {
        return *s.query(*uenv::parse_label_query(text, keys, sys));
    };
    REQUIRE(q("prgenv-gnu").size() == 2u);
    REQUIRE(q("prgenv-gnu/1.0:v1@daint").size() == 1u);
    REQUIRE(q("prgenv-gnu", "santis").size() == 1u);
    REQUIRE(q("prgenv-gnu@*", "santis").size() == 2u);
    REQUIRE(q("prgenv-gnu%zen2").size() == 2u);
    REQUIRE(q("prgenv-gnu%gh200").empty());
    REQUIRE(q(daint->id).size() == 1u);
    REQUIRE(q(daint->digest.string()).size() == 1u);
    REQUIRE(q(daint->digest.hex()).size() == 1u);
    // an id is matched exactly: no text of the user reaches a pattern
    for (auto bad : {"sha256:", "sha256:%", "sha256:_", "sha256:abc"}) {
        INFO(bad);
        auto e = uenv::parse_label_query(bad, keys);
        REQUIRE(!e);
        REQUIRE(e.error().find(bad) != std::string::npos);
    }
    // a layer is found only with the marker
    REQUIRE(q("tools").empty());
    REQUIRE(q("+tools").size() == 1u);
    REQUIRE(q("+tools/1.0:v1").size() == 1u);
    // a layer that is only inside an image is found too
    REQUIRE(q("+prgenv-gnu").size() == 1u);
    // an image with no system matches each system
    auto nosys = uenv::add_squashfs(s, fake_sqfs(dir, "x"),
                                    cfg_of(make_layer("x", "/x", false, {})), {},
                                    false);
    REQUIRE(nosys);
    REQUIRE(q("x@daint").size() == 1u);
    auto all = *s.all();
    REQUIRE(all.size() == 4u);
}

TEST_CASE("3/database: a label that matches more than one is an error",
          "[store]") {
    std::vector<uenv::store> stores;
    stores.push_back(new_store());
    auto dir = tmpdir();
    uenv::add_squashfs(stores[0], fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                       named<std::string>({{"system", "daint"}}), false);
    uenv::add_squashfs(stores[0], fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                       named<std::string>({{"system", "santis"}}), false);
    auto q = *uenv::parse_label_query("prgenv-gnu", {});
    auto r = uenv::find_unique(stores, q, "prgenv-gnu");
    REQUIRE(!r);
    REQUIRE(r.error().find("id=") != std::string::npos);
}

TEST_CASE("3/database: a damaged row is an error that names the database",
          "[store]") {
    auto s = new_store();
    auto a = *uenv::add_squashfs(s, fake_sqfs(tmpdir(), "pe"), cfg_of(prgenv()),
                                 named<std::string>({{"system", "daint"}}), false);
    for (auto sql : {"UPDATE image_layers SET alias = 'a b'",
                     "UPDATE image_layers SET mount = 'relative'",
                     "UPDATE layers SET squashfs = 'sha256:nothex'",
                     "UPDATE images SET release = 'v1+x'"}) {
        INFO(sql);
        auto t = new_store();
        auto b = *uenv::add_squashfs(t, fake_sqfs(tmpdir(), "pe"), cfg_of(prgenv()),
                                     {}, false);
        REQUIRE(t.db().db().query(sql));
        auto r = t.db().get(b.digest);
        REQUIRE(!r);
        REQUIRE(r.error().find("the database") != std::string::npos);
        REQUIRE(r.error().find("index.db") != std::string::npos);
    }
    REQUIRE(s.db().get(a.digest));
}

TEST_CASE("1/runtime merge: the list is parsed once, on the lexer", "[store]") {
    uenv::label_keys keys;
    auto items = uenv::parse_item_list(
        "gnu=prgenv-gnu/24.11:v1,+tools:/opt/tools,./x.squashfs,"
        "3fa9c2e1d04b7a65",
        keys, std::nullopt);
    REQUIRE(items);
    REQUIRE(items->size() == 4u);
    REQUIRE((*items)[0].alias == nm("gnu"));
    REQUIRE((*items)[0].query->name == nm("prgenv-gnu"));
    REQUIRE((*items)[0].query->release == nm("v1"));
    REQUIRE((*items)[1].query->layer);
    REQUIRE((*items)[1].mount == mp("/opt/tools"));
    REQUIRE((*items)[2].file->is_absolute());
    REQUIRE((*items)[3].query->id);
    // an alias that is not a name is an error, not a part of the label
    for (auto bad : {"-x=tools", "a b=tools", "tools:v1:relative", "x=+tools,"}) {
        INFO(bad);
        REQUIRE(!uenv::parse_item_list(bad, keys, std::nullopt));
    }
}

TEST_CASE("3/delete: shared blobs stay", "[store]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto a = *uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                                 named<std::string>({{"system", "daint"}}), false);
    auto b = *uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                                 named<std::string>({{"system", "santis"}}), false);
    auto sqfs = a.layers[0].squashfs;
    auto released = s.remove(a.digest);
    REQUIRE(released);
    REQUIRE(released->size() == 3u); // index, wrapper, image config
    REQUIRE(s.has_blob(sqfs));
    REQUIRE(!s.has_blob(a.digest));
    REQUIRE(s.load(b.digest));
    REQUIRE(s.remove(b.digest));
    REQUIRE(!s.has_blob(sqfs));
    REQUIRE(s.all()->empty());
}

TEST_CASE("3/update: prune with the grace period", "[store]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto a = *uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()), {},
                                 false);
    // an unused blob, new and old, and .part files
    auto unused_new = format::make_blob("new");
    auto unused_old = format::make_blob("old");
    REQUIRE(s.put_bytes(unused_new));
    REQUIRE(s.put_bytes(unused_old));
    make_old(s.blob_path(unused_old.digest));
    auto part_new = write_file(s.v2() / "tmp" / "aaaa.1234.part", "x");
    auto part_old = write_file(s.v2() / "tmp" / "bbbb.1234.part", "x");
    make_old(part_old);

    auto dry = s.update(false, true);
    REQUIRE(dry);
    REQUIRE(dry->pruned.size() == 1u);
    REQUIRE(s.has_blob(unused_old.digest));

    auto r = s.update(false, false);
    REQUIRE(r);
    REQUIRE(r->pruned.size() == 1u);
    REQUIRE(r->kept_recent.size() == 1u);
    REQUIRE(r->parts.size() == 1u);
    REQUIRE(!s.has_blob(unused_old.digest));
    REQUIRE(s.has_blob(unused_new.digest));
    REQUIRE(fs::exists(part_new));
    REQUIRE(!fs::exists(part_old));
    REQUIRE(s.load(a.digest));

    auto f = s.update(true, false);
    REQUIRE(f->pruned.size() == 1u);
    REQUIRE(!s.has_blob(unused_new.digest));
    REQUIRE(!fs::exists(part_new));
    REQUIRE(s.load(a.digest));
}

TEST_CASE("3/update: an image with a missing blob is deleted", "[store]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto a = *uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()), {},
                                 false);
    fs::remove(s.blob_path(a.layers[0].squashfs));
    auto r = s.update(false, false);
    REQUIRE(r);
    REQUIRE(r->missing.size() == 1u);
    REQUIRE(s.all()->empty());
}

TEST_CASE("3/reads and writes: add stopped before the database", "[store]") {
    auto root = tmpdir() / "repo";
    REQUIRE(uenv::store::create(root));
    auto dir = tmpdir();
    auto f = fake_sqfs(dir, "pe");
    for (auto point : {"add-after-squashfs", "add-after-blob", "record-before-db"}) {
        INFO(point);
        int status = in_child_with_fault(point, [&] {
            auto s = *uenv::store::open(root);
            (void)uenv::add_squashfs(s, f, cfg_of(prgenv()), {}, false);
        });
        REQUIRE(status == 99);
        auto s = *uenv::store::open(root);
        // the image is not in the database: ls does not show it
        REQUIRE(s.all()->empty());
        // a forced update deletes the blobs that it left
        auto u = s.update(true, false);
        REQUIRE(u);
        REQUIRE(!u->pruned.empty());
    }
    // a later add works
    auto s = *uenv::store::open(root);
    REQUIRE(uenv::add_squashfs(s, f, cfg_of(prgenv()), {}, false));
}

TEST_CASE("3/reads and writes: delete stopped after the database", "[store]") {
    auto root = tmpdir() / "repo";
    auto s0 = *uenv::store::create(root);
    auto a = *uenv::add_squashfs(s0, fake_sqfs(tmpdir(), "pe"), cfg_of(prgenv()),
                                 {}, false);
    int status = in_child_with_fault("remove-after-db", [&] {
        auto s = *uenv::store::open(root);
        (void)s.remove(a.digest);
    });
    REQUIRE(status == 99);
    auto s = *uenv::store::open(root);
    REQUIRE(s.all()->empty());
    REQUIRE(s.has_blob(a.layers[0].squashfs));
    auto u = s.update(true, false);
    REQUIRE(u->pruned.size() == 7u);
}

TEST_CASE("3/writes from more than one process", "[store]") {
    auto root = tmpdir() / "repo";
    REQUIRE(uenv::store::create(root));
    auto dir = tmpdir();
    std::vector<pid_t> pids;
    const int n = 6;
    for (int i = 0; i < n; ++i) {
        auto f = fake_sqfs(dir, fmt::format("img{}", i));
        auto pid = fork();
        if (pid == 0) {
            auto s = *uenv::store::open(root);
            auto c = cfg_of(make_layer(fmt::format("img{}", i), "/x", false, {}));
            auto r = uenv::add_squashfs(s, f, c, {}, false);
            _exit(r ? 0 : 1);
        }
        pids.push_back(pid);
    }
    for (auto p : pids) {
        int status = 0;
        waitpid(p, &status, 0);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
    }
    auto s = *uenv::store::open(root);
    REQUIRE(s.all()->size() == std::size_t(n));
}

TEST_CASE("3/oci image layout: index.json lists the images and layers",
          "[store]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto a = *uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()), {},
                                 false);
    auto l = *uenv::add_squashfs(s, fake_sqfs(dir, "t"), cfg_of(tools()), {},
                                 true);
    // index.json changes only at an update
    auto before = json::parse(*util::read_file(s.v2() / "index.json"));
    REQUIRE(before["manifests"].empty());
    REQUIRE(s.update(false, false));
    auto j = json::parse(*util::read_file(s.v2() / "index.json"));
    REQUIRE(j["manifests"].size() == 2u);
    for (auto& m : j["manifests"]) {
        auto ref = m["annotations"]["org.opencontainers.image.ref.name"]
                       .get<std::string>();
        auto d = m["digest"].get<std::string>();
        REQUIRE(ref.ends_with(d.substr(7, 16)));
    }
}

TEST_CASE("1/compose: an image from layers", "[store][compose]") {
    auto s = new_store();
    auto dir = tmpdir();
    auto pe = *uenv::add_squashfs(s, fake_sqfs(dir, "pe"), cfg_of(prgenv()), {},
                                  true);
    auto t = *uenv::add_squashfs(s, fake_sqfs(dir, "t"), cfg_of(tools()), {},
                                 true);
    auto spec = json::parse(R"({
      "name": "my-env", "version": "1.0", "release": "v1",
      "meta": {"system": "eiger"},
      "views": {"dev": {"include": ["prgenv-gnu:prgenv", "tools:tools"]}},
      "default_view": "dev",
      "layers": [{"from": "+tools/1.0:v1"}, {"from": "+prgenv-gnu/1.0:v1"}]
    })");
    auto r = uenv::compose(s, spec, {});
    REQUIRE(r);
    // the layer manifests are the same objects
    REQUIRE(r->layers[0].digest == pe.digest);
    REQUIRE(r->layers[1].digest == t.digest);

    SECTION("the 'from' of a layer is parsed") {
        for (auto bad : {"+tools/1.0:v1#a#b", "my-env#a b", "+tools/1.0 :v1"}) {
            INFO(bad);
            auto e = spec;
            e["layers"][0]["from"] = bad;
            auto m = uenv::compose(s, e, {});
            REQUIRE(!m);
            REQUIRE(m.error().find("the compose file") != std::string::npos);
        }
    }
    SECTION("an alias is a name") {
        for (auto bad : {json("my tools"), json(42), json("a:b")}) {
            INFO(bad.dump());
            auto e = spec;
            e["layers"][0]["alias"] = bad;
            auto m = uenv::compose(s, e, {});
            REQUIRE(!m);
            REQUIRE(m.error().find("the alias") != std::string::npos);
        }
    }

    SECTION("a relocatable layer moves") {
        spec["layers"][0]["mount"] = "/opt/tools";
        auto m = uenv::compose(s, spec, {});
        REQUIRE(m);
        // sorted by mount point: /opt/tools is now first
        REQUIRE(m->layers[0].digest == t.digest);
        REQUIRE(m->layers[0].mount == mp("/opt/tools"));
        auto img = *s.load(m->digest);
        auto c = *uenv::concretise(img, {});
        envvars::state env;
        uenv::apply_patches(env, c.patches);
        REQUIRE(env.get("PATH") ==
                "/opt/tools/bin:/user-environment/env/prgenv/bin");
    }
    SECTION("a layer taken from an image") {
        auto spec2 = spec;
        spec2["name"] = "my-env2";
        spec2["layers"][1]["from"] = "my-env/1.0:v1#prgenv-gnu";
        auto m = uenv::compose(s, spec2, {});
        REQUIRE(m);
        REQUIRE(m->layers[0].digest == pe.digest);
    }
    SECTION("compose is reproducible") {
        auto again = uenv::compose(s, spec, {});
        REQUIRE(again->digest == r->digest);
    }
    SECTION("the order of the layers in the compose file has no effect") {
        auto swapped = spec;
        std::swap(swapped["layers"][0], swapped["layers"][1]);
        REQUIRE(uenv::compose(s, swapped, {})->digest == r->digest);
    }
}

TEST_CASE("6/rollout: a read-only v1 repository does not block a v2 one",
          "[store]") {
    auto site = tmpdir() / "site";
    write_file(site / "index.db", "");
    fs::create_directories(site / "images");
    auto user = tmpdir() / "user";
    REQUIRE(uenv::store::create(user));
    uenv::configuration cfg;
    cfg.repos.accumulate(std::vector<uenv::repo_description>{
        {.name = "user", .path = user}, {.name = "site", .path = site}});
    // writable v1: an error that tells the user to migrate
    auto w = uenv::open_stores(cfg);
    REQUIRE(!w);
    REQUIRE(w.error().find("migrate") != std::string::npos);
    // read only v1: skipped
    fs::permissions(site, fs::perms::owner_write, fs::perm_options::remove);
    auto r = uenv::open_stores(cfg);
    fs::permissions(site, fs::perms::owner_write, fs::perm_options::add);
    REQUIRE(r);
    REQUIRE(r->size() == 1u);
}

TEST_CASE("1/runtime merge: the session describes each item",
          "[store][session]") {
    std::vector<uenv::store> stores;
    stores.push_back(new_store());
    auto dir = tmpdir();
    REQUIRE(uenv::add_squashfs(stores[0], fake_sqfs(dir, "pe"), cfg_of(prgenv()),
                               named<std::string>({{"system", "daint"}}), false));
    REQUIRE(uenv::add_squashfs(stores[0], fake_sqfs(dir, "t"), cfg_of(tools()),
                               {}, true));
    auto items = uenv::parse_item_list("prgenv-gnu,t=+tools:/opt/t", {},
                                       std::nullopt);
    REQUIRE(items);
    auto r = uenv::resolve_items(stores, *items, false);
    REQUIRE(r);
    REQUIRE(r->image.kind() == uenv::image_kind::runtime_merge);
    // the items, in the order of the list: not the merge
    auto& d = r->items;
    REQUIRE(d.size() == 2u);
    REQUIRE(d[0].name == nm("prgenv-gnu"));
    REQUIRE(d[0].kind == uenv::session_item::kind_t::image);
    REQUIRE(d[0].label == "prgenv-gnu/1.0:v1");
    REQUIRE(d[0].digest);
    REQUIRE(d[0].layers.size() == 1u);
    REQUIRE(d[0].layers[0].mount == mp("/user-environment"));
    // the alias of the item, and the mount point of the session
    REQUIRE(d[1].name == nm("t"));
    REQUIRE(d[1].kind == uenv::session_item::kind_t::layer);
    REQUIRE(d[1].label == "tools/1.0:v1");
    REQUIRE(d[1].layers[0].alias == nm("t"));
    REQUIRE(d[1].layers[0].mount == mp("/opt/t"));

    // UENV_SESSION: the same items after a round trip
    d[1].views = {nm("tools")};
    auto back = uenv::parse_session_json(uenv::session_json(d));
    REQUIRE(back);
    REQUIRE(back->size() == 2u);
    REQUIRE((*back)[0].digest == d[0].digest);
    REQUIRE((*back)[1].name == nm("t"));
    REQUIRE((*back)[1].kind == uenv::session_item::kind_t::layer);
    REQUIRE((*back)[1].layers[0].mount == mp("/opt/t"));
    REQUIRE((*back)[1].views == std::vector{nm("tools")});
}

TEST_CASE("1/concretisation: UENV_SESSION is parsed", "[session]") {
    auto item = [](std::string name, std::string mount, std::string kind) {
        return fmt::format(
            R"([{{"name":"{}","kind":"{}","label":"x/1:v1","digest":null,)"
            R"("layers":[{{"alias":"x","label":"x/1:v1","mount":"{}"}}],)"
            R"("views":["v"]}}])",
            name, kind, mount);
    };
    REQUIRE(uenv::parse_session_json(item("x", "/opt/x", "image")));
    REQUIRE(uenv::parse_session_json(item("x", "/opt/x", "file")));
    // each error names UENV_SESSION
    for (auto bad : {std::string{"{}"}, std::string{"not json"},
                     item("a:b", "/opt/x", "image"),
                     item("x", "opt/x", "image"),
                     item("x", "/opt/x", "merge")}) {
        auto r = uenv::parse_session_json(bad);
        REQUIRE(!r);
        REQUIRE(r.error().find("UENV_SESSION") != std::string::npos);
    }
}

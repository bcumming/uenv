// Tests of the registry operations (4-registry-ops.md) and the catalogue
// (5-discovery.md) against a real zot registry. The cases SKIP if no zot
// binary is found.

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <oci/client.h>
#include <oci/manifest.h>
#include <oci/reference.h>
#include <uenv/format.h>
#include <uenv/ops.h>
#include <uenv/registry.h>
#include <uenv/settings.h>
#include <uenv/store.h>
#include <util/curl.h>
#include <util/fs.h>
#include <util/subprocess.h>
#include <util/url.h>

#include "v2_helpers.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace v2test;
namespace format = uenv::format;
namespace reg = uenv::registry;

namespace {

std::string zot_binary() {
    if (const char* e = std::getenv("UENV_TEST_ZOT")) {
        if (::access(e, X_OK) == 0) {
            return e;
        }
    }
#ifdef UENV_TEST_ZOT_PATH
    if (::access(UENV_TEST_ZOT_PATH, X_OK) == 0) {
        return UENV_TEST_ZOT_PATH;
    }
#endif
    return "zot";
}

int free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool ready(const std::string& base) {
    util::curl::request req;
    req.url = base + "/v2/";
    auto resp = util::curl::perform(req);
    return resp && (resp->status == 200 || resp->status == 401);
}

struct zot {
    std::optional<util::subprocess> proc;
    std::string base;
    fs::path state;

    explicit zot(std::string storage_extra = "", bool start = true) {
        if (!start) {
            return;
        }
        const int port = free_port();
        state = util::make_temp_dir().value();
        const auto cfg = state / "config.json";
        {
            std::ofstream f{cfg};
            f << fmt::format(
                R"({{"storage":{{"rootDirectory":"{}"{}}},)"
                R"("http":{{"address":"127.0.0.1","port":"{}"}},)"
                R"("log":{{"level":"error","output":"{}"}}}})",
                (state / "registry").string(), storage_extra, port,
                (state / "zot.log").string());
        }
        // zot writes to stdout and stderr: send them to a file. A pipe that
        // no one reads fills up after some 64 kB, and zot then blocks in a
        // request while it holds the lock of a repository
        auto p = util::run(
            {"/bin/sh", "-c",
             fmt::format("exec '{}' serve '{}' > '{}' 2>&1", zot_binary(),
                         cfg.string(), (state / "zot.out").string())});
        if (!p) {
            return;
        }
        const auto url = fmt::format("http://127.0.0.1:{}", port);
        for (int i = 0; i < 150; ++i) {
            if (p->finished()) {
                return;
            }
            if (ready(url)) {
                proc = std::move(*p);
                base = url;
                return;
            }
            ::usleep(200 * 1000);
        }
        p->kill();
    }
    zot(zot&&) = default;
    ~zot() {
        if (proc) {
            proc->kill();
        }
    }
};

// UENV_TEST_REGISTRY=http://host:port runs the tests against a different
// registry, for example distribution (registry:2), started by the caller
zot& shared_zot() {
    static zot z = [] {
        if (const char* r = std::getenv("UENV_TEST_REGISTRY")) {
            zot external(std::string{}, false);
            external.base = r;
            return external;
        }
        return zot{};
    }();
    return z;
}

#define REQUIRE_ZOT()                                                          \
    if (shared_zot().base.empty()) {                                           \
        SKIP("no zot binary");                                                 \
    }

// a configuration with a registry 'deploy' (namespace ns) and 'build'
struct env_t {
    uenv::configuration cfg;
    reg::context ctx;
    std::string ns;
};

std::unique_ptr<env_t> make_env(const std::string& base,
                                std::vector<uenv::referrer_rule> rules = {}) {
    static std::atomic<int> counter{0};
    auto e = std::make_unique<env_t>();
    e->ns = fmt::format("t{}x{}", getpid(), counter++);
    auto url = *util::parse_url(base + "/uenvtest");
    e->cfg.registries = {{"deploy", url, e->ns + "/deploy"},
                         {"build", url, e->ns + "/build"}};
    e->cfg.default_registry = "deploy";
    e->cfg.referrer_rules = std::move(rules);
    e->ctx.cfg = &e->cfg;
    e->ctx.cache = *util::make_temp_dir();
    return e;
}

fs::path fake_sqfs(const std::string& name, std::size_t size = 20000) {
    auto dir = *util::make_temp_dir();
    std::string content(size, 'y');
    content.replace(0, name.size(), name);
    return write_file(dir / (name + ".squashfs"), content);
}

uenv::store new_store() {
    return *uenv::store::create(*util::make_temp_dir() / "repo");
}

reg::address addr(env_t& e, const std::string& text,
                  std::optional<std::string> registry = {}) {
    return *reg::parse_address(text, e.cfg, registry);
}

uenv::object_record add_image(uenv::store& s, uenv::layer l,
                              std::map<std::string, std::string> meta = {}) {
    return *uenv::add_squashfs(s, fake_sqfs(l.config.name + l.mount),
                               format::layer_config_of(l), meta, false);
}

std::size_t index_of(const std::vector<std::string>& v, std::string_view prefix) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].starts_with(prefix)) {
            return i;
        }
    }
    return v.size();
}
std::size_t last_index_of(const std::vector<std::string>& v,
                          std::string_view prefix) {
    std::size_t r = v.size();
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].starts_with(prefix)) {
            r = i;
        }
    }
    return r;
}

} // namespace

TEST_CASE("4/addresses: the three forms", "[registry-addr]") {
    uenv::configuration cfg;
    cfg.registries = {
        {"deploy", *util::parse_url("https://jfrog.svc.cscs.ch/uenv"), "deploy"},
        {"build", *util::parse_url("https://jfrog.svc.cscs.ch/uenv"), "build"},
        {"cscs", *util::parse_url("https://jfrog.svc.cscs.ch/uenv"), {}},
        {"community", *util::parse_url("https://ghcr.io/biology"),
         "production/deploy"}};
    cfg.default_registry = "deploy";
    auto repo = [&](std::string text, std::optional<std::string> r = {}) {
        auto a = reg::parse_address(text, cfg, r);
        REQUIRE(a);
        return fmt::format("{}/{}", a->where.base.string(),
                           a->where.repo("foo"));
    };
    REQUIRE(repo("foo/26.3:v2") == "https://jfrog.svc.cscs.ch/uenv/deploy/foo");
    REQUIRE(repo("build::foo/26.3:v2") ==
            "https://jfrog.svc.cscs.ch/uenv/build/foo");
    REQUIRE(repo("cscs/wombat/soup::foo/26.3:v2") ==
            "https://jfrog.svc.cscs.ch/uenv/wombat/soup/foo");
    REQUIRE(repo("community::foo/26.3:v2") ==
            "https://ghcr.io/biology/production/deploy/foo");
    REQUIRE(repo("foo/26.3:v2", "ghcr.io/lab") == "https://ghcr.io/lab/foo");
    auto full = reg::parse_address(
        "ghcr.io/wombat/soup/prgenv-gnu:26.6-v2-3fa9c2e1d04b7a65", cfg, {});
    REQUIRE(full);
    REQUIRE(*full->repository == "wombat/soup/prgenv-gnu");
    REQUIRE(*full->reference == "26.6-v2-3fa9c2e1d04b7a65");
    // a registry name that does not exist is an error that lists them
    auto bad = reg::parse_address("nope::foo", cfg, {});
    REQUIRE(!bad);
    REQUIRE(bad.error().find("community") != std::string::npos);
}

TEST_CASE("4/push and pull an image", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto s = new_store();
    auto img = add_image(s, prgenv(), {{"system", "daint"}});
    auto t = *reg::resolve_target(e->cfg, {});
    auto r = reg::push(s, img.digest, t, e->ctx);
    REQUIRE(r);
    REQUIRE(r->repository == "uenvtest/" + e->ns + "/deploy/prgenv-gnu");
    REQUIRE(r->tag == format::make_tag(img.cfg, img.digest));

    auto& w = e->ctx.st.writes;
    INFO(fmt::format("{}", fmt::join(w, "\n")));
    // the order of writes: blobs, manifests, the tag, then the event
    auto last_blob = last_index_of(w, "blob:");
    auto first_manifest = index_of(w, "manifest:");
    auto tag = index_of(w, "tag:");
    auto event = index_of(w, "event:");
    REQUIRE(last_blob < first_manifest);
    REQUIRE(last_index_of(w, "manifest:") < tag);
    REQUIRE(tag < event);
    REQUIRE(event < w.size());

    // the empty blob is in the repository of the wrapper
    auto c = *reg::make_client(t, r->repository, e->ctx);
    REQUIRE(*c.blob_exists(*oci::digest::parse(
        fmt::format("sha256:{}", oci::empty_config_hex))));

    // pull into a new repository, by label
    auto s2 = new_store();
    auto p = reg::pull(s2, addr(*e, "prgenv-gnu/1.0:v1@daint"), e->ctx);
    REQUIRE(p);
    REQUIRE(p->digest == img.digest);
    auto loaded = s2.load(p->digest);
    REQUIRE(loaded);
    REQUIRE(uenv::concretise(*loaded, {}));
    // the same identity in the registry and the repository
    REQUIRE(s2.read_blob(p->digest) == s.read_blob(img.digest));

    // a second pull downloads no squashfs
    e->ctx.st = {};
    auto s3 = new_store();
    (void)s3;
    REQUIRE(reg::pull(s2, addr(*e, "prgenv-gnu/1.0:v1@daint"), e->ctx));
    REQUIRE(e->ctx.st.downloaded == 0u);
}

TEST_CASE("4/push of a layer is a building block, compose mounts its blobs",
          "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto s = new_store();
    auto t = *reg::resolve_target(e->cfg, {});
    auto pe = *uenv::add_squashfs(s, fake_sqfs("pe", 50000),
                                  format::layer_config_of(prgenv()), {}, true);
    auto tl = *uenv::add_squashfs(s, fake_sqfs("tl", 50000),
                                  format::layer_config_of(tools()), {}, true);
    REQUIRE(reg::push(s, pe.digest, t, e->ctx));
    REQUIRE(reg::push(s, tl.digest, t, e->ctx));

    auto spec = json::parse(R"({
      "name": "my-env", "version": "1.0", "release": "v1",
      "meta": {"system": "eiger"},
      "views": {"dev": {"include": ["prgenv-gnu:prgenv", "tools:tools"]}},
      "default_view": "dev",
      "layers": [{"from": "+prgenv-gnu"}, {"from": "+tools", "mount": "/opt/tools"}]
    })");
    auto img = uenv::compose(s, spec, {});
    REQUIRE(img);
    e->ctx.st = {};
    auto r = reg::push(s, img->digest, t, e->ctx);
    REQUIRE(r);
    // the squashfs files are not uploaded again. zot reports a blob of a
    // different repository as present (dedupe), so the mount is not needed
    // on zot: FINDING (zot-dedupe)
    WARN(fmt::format("compose push: mounted={} skipped={} uploaded={} "
                     "uploaded_bytes={}",
                     e->ctx.st.mounted, e->ctx.st.skipped, e->ctx.st.uploaded,
                     e->ctx.st.uploaded_bytes));
    REQUIRE(e->ctx.st.mounted + e->ctx.st.skipped >= 2u);
    REQUIRE(e->ctx.st.uploaded_bytes < 10000u);

    // the image does not depend on the building blocks
    auto bb = addr(*e, "+tools");
    REQUIRE(reg::remove(bb, e->ctx));
    auto s2 = new_store();
    auto p = reg::pull(s2, addr(*e, "my-env"), e->ctx);
    REQUIRE(p);
    auto loaded = *s2.load(p->digest);
    auto c = *uenv::concretise(loaded, {});
    envvars::state env;
    uenv::apply_patches(env, c.patches);
    REQUIRE(env.get("PATH") == "/opt/tools/bin:/user-environment/env/prgenv/bin");
}

TEST_CASE("4/uenv does not change a tag", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto s = new_store();
    auto t = *reg::resolve_target(e->cfg, {});
    auto img = add_image(s, prgenv());
    REQUIRE(reg::push(s, img.digest, t, e->ctx));
    // a second push of the same image is accepted
    REQUIRE(reg::push(s, img.digest, t, e->ctx));
    // the tag points at a different object: the push stops
    auto c = *reg::make_client(t, t.repo("prgenv-gnu"), e->ctx);
    auto tag = format::make_tag(img.cfg, img.digest);
    auto other = add_image(s, make_layer("prgenv-gnu", "/other", false, {}));
    auto other_body = *s.read_blob(other.digest);
    // write the other index (its children first) under the first tag
    REQUIRE(reg::push(s, other.digest, t, e->ctx));
    REQUIRE(c.put_manifest(oci::reference::tag(*oci::tag::parse(tag)),
                           other_body, oci::media_type_index));
    auto again = reg::push(s, img.digest, t, e->ctx);
    REQUIRE(!again);
    REQUIRE(again.error().find("does not change a tag") != std::string::npos);
}

TEST_CASE("4/pull checks the image before the squashfs download", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto s = new_store();
    auto t = *reg::resolve_target(e->cfg, {});
    auto img = add_image(s, prgenv());
    REQUIRE(reg::push(s, img.digest, t, e->ctx));
    auto c = *reg::make_client(t, t.repo("prgenv-gnu"), e->ctx);
    auto good = json::parse(*s.read_blob(img.digest));

    auto pull_bad = [&](json idx) {
        auto body = idx.dump();
        auto d = oci::digest::sha256(util::sha256_string(body));
        REQUIRE(c.put_manifest(oci::reference::digest(d), body,
                               oci::media_type_index));
        e->ctx.st = {};
        auto s2 = new_store();
        auto full = fmt::format("{}/{}@{}", shared_zot().base.substr(7),
                                t.repo("prgenv-gnu"), d.string());
        auto r = reg::pull(s2, addr(*e, full), e->ctx);
        REQUIRE(e->ctx.st.downloaded == 0u);
        REQUIRE(s2.all()->empty());
        return r;
    };
    SECTION("an artifactType hint that does not agree with the child") {
        auto j = good;
        j["manifests"][1]["artifactType"] = "application/vnd.uenv.layer.v1";
        j["manifests"][0]["artifactType"] = "application/vnd.uenv.image-config.v1";
        // the hint of a layer says wrapper: the index form is wrong
        auto k = good;
        k["manifests"][1]["artifactType"] = "application/vnd.uenv.image-config.v1";
        REQUIRE(!pull_bad(k));
    }
    SECTION("a layer entry without a mount point") {
        auto j = good;
        j["manifests"][1]["annotations"].erase("vnd.uenv.mount");
        REQUIRE(!pull_bad(j));
    }
    SECTION("a layer that is not relocatable at a different mount point") {
        auto j = good;
        j["manifests"][1]["annotations"]["vnd.uenv.mount"] = "/somewhere";
        auto r = pull_bad(j);
        REQUIRE(!r);
        REQUIRE(r.error().find("not relocatable") != std::string::npos);
    }
}

TEST_CASE("4/copy with no change, and its referrers", "[registry]") {
    REQUIRE_ZOT();
    std::vector<uenv::referrer_rule> rules{
        {"uenv/sbom", "application/spdx+json", {"layer"}, {"copy", "compose"}, true},
        {"uenv/signature", "application/vnd.dev.sigstore.bundle.v0.3+json",
         {"image", "layer"}, {"copy"}, false},
        {"uenv/status", "application/vnd.uenv.status.v1+json",
         {"image", "layer"}, {}, true}};
    auto e = make_env(shared_zot().base, rules);
    auto s = new_store();
    auto build = *reg::resolve_target(e->cfg, "build");
    auto deploy = *reg::resolve_target(e->cfg, "deploy");
    auto img = add_image(s, prgenv(), {{"system", "daint"}});
    REQUIRE(reg::push(s, img.digest, build, e->ctx));

    auto a = addr(*e, "build::prgenv-gnu");
    REQUIRE(reg::attach(a, "uenv/signature", "sig", e->ctx));
    REQUIRE(reg::attach(a, "uenv/status", R"({"status":"deprecated"})", e->ctx));
    // an sbom is for a layer, not an image
    REQUIRE(!reg::attach(a, "uenv/sbom", "{}", e->ctx));
    // a kind with no rule is refused
    REQUIRE(!reg::attach(a, "uenv/unknown", "x", e->ctx));
    auto la = addr(*e, "build::+prgenv-gnu");
    REQUIRE(reg::attach(la, "uenv/sbom", R"({"spdx":1})", e->ctx));

    e->ctx.st = {};
    auto r = reg::copy(a, deploy, e->ctx);
    REQUIRE(r);
    // the same image: the same digest
    REQUIRE(r->digest == img.digest);
    REQUIRE(r->repository == "uenvtest/" + e->ns + "/deploy/prgenv-gnu");
    // within one registry, each blob is mounted
    REQUIRE(e->ctx.st.uploaded_bytes < 1000u);
    auto image_refs = *reg::list_referrers(deploy, r->repository, img.digest,
                                           e->ctx);
    // the signature travels, the status does not
    REQUIRE(image_refs.size() == 1u);
    REQUIRE(image_refs[0].artifact_type ==
            "application/vnd.dev.sigstore.bundle.v0.3+json");
    auto layer_refs = *reg::list_referrers(deploy, r->repository,
                                           img.layers[0].digest, e->ctx);
    REQUIRE(layer_refs.size() == 1u);

    // pull from deploy
    auto s2 = new_store();
    REQUIRE(reg::pull(s2, addr(*e, "prgenv-gnu"), e->ctx));
}

TEST_CASE("4/copy with a change is a new image with the same layers",
          "[registry]") {
    REQUIRE_ZOT();
    std::vector<uenv::referrer_rule> rules{
        {"uenv/sbom", "application/spdx+json", {"layer"}, {"copy", "compose"}, true},
        {"uenv/signature", "application/vnd.dev.sigstore.bundle.v0.3+json",
         {"image", "layer"}, {"copy"}, false}};
    auto e = make_env(shared_zot().base, rules);
    auto s = new_store();
    auto deploy = *reg::resolve_target(e->cfg, "deploy");
    auto img = add_image(s, prgenv(), {{"system", "daint"}});
    REQUIRE(reg::push(s, img.digest, deploy, e->ctx));
    auto a = addr(*e, "prgenv-gnu@daint");
    REQUIRE(reg::attach(a, "uenv/signature", "sig", e->ctx));
    REQUIRE(reg::attach(addr(*e, "+prgenv-gnu"), "uenv/sbom", "{}", e->ctx));

    auto r = reg::copy(a, deploy, e->ctx, {{"meta.system", "santis"}});
    REQUIRE(r);
    REQUIRE(r->digest != img.digest);
    auto refs = *reg::list_referrers(deploy, r->repository, r->digest, e->ctx);
    REQUIRE(refs.empty());
    auto lrefs = *reg::list_referrers(deploy, r->repository,
                                      img.layers[0].digest, e->ctx);
    REQUIRE(lrefs.size() == 1u);
    // both images are in the catalogue, one for each system
    auto s2 = new_store();
    auto p = reg::pull(s2, addr(*e, "prgenv-gnu@santis"), e->ctx);
    REQUIRE(p);
    REQUIRE(p->digest == r->digest);
    REQUIRE(p->layers[0].digest == img.layers[0].digest);
    // a lookup with no system matches the two: an error that lists them
    auto both = reg::pull(s2, addr(*e, "prgenv-gnu@*"), e->ctx);
    REQUIRE(!both);
    REQUIRE(both.error().find("id=") != std::string::npos);
}

TEST_CASE("4/delete removes the top-level object only", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto s = new_store();
    auto t = *reg::resolve_target(e->cfg, {});
    auto a = add_image(s, prgenv(), {{"system", "daint"}});
    auto b = add_image(s, prgenv(), {{"system", "santis"}});
    REQUIRE(reg::push(s, a.digest, t, e->ctx));
    REQUIRE(reg::push(s, b.digest, t, e->ctx));
    auto d = reg::remove(addr(*e, "prgenv-gnu@daint"), e->ctx);
    REQUIRE(d);
    REQUIRE(d->digest == a.digest);
    // the tag is gone
    auto c = *reg::make_client(t, t.repo("prgenv-gnu"), e->ctx);
    REQUIRE(!c.get_manifest(oci::reference::tag(*oci::tag::parse(d->tag))));
    // the catalogue does not list it
    auto s2 = new_store();
    REQUIRE(!reg::pull(s2, addr(*e, "prgenv-gnu@daint"), e->ctx));
    // the image that shares its layer is complete
    REQUIRE(reg::pull(s2, addr(*e, "prgenv-gnu@santis"), e->ctx));
}

TEST_CASE("4/attach with fan-out reaches each copy of a layer", "[registry]") {
    REQUIRE_ZOT();
    std::vector<uenv::referrer_rule> rules{
        {"uenv/status", "application/vnd.uenv.status.v1+json",
         {"image", "layer"}, {}, true}};
    auto e = make_env(shared_zot().base, rules);
    auto s = new_store();
    auto t = *reg::resolve_target(e->cfg, {});
    // the building block, and two images that contain it
    auto bb = *uenv::add_squashfs(s, fake_sqfs("tl"),
                                  format::layer_config_of(tools()), {}, true);
    REQUIRE(reg::push(s, bb.digest, t, e->ctx));
    for (auto name : {"env-a", "env-b"}) {
        auto spec = json{{"name", name}, {"version", "1"}, {"release", "v1"},
                         {"meta", json::object()}, {"views", json::object()},
                         {"layers", json::array({{{"from", "+tools"}}})}};
        auto img = uenv::compose(s, spec, {});
        REQUIRE(img);
        REQUIRE(reg::push(s, img->digest, t, e->ctx));
    }
    auto repos = reg::attach(addr(*e, "+tools"), "uenv/status",
                             R"({"status":"deprecated"})", e->ctx);
    REQUIRE(repos);
    REQUIRE(repos->size() == 3u);
    for (auto& repo : *repos) {
        auto refs = *reg::list_referrers(t, repo, bb.digest, e->ctx);
        REQUIRE(refs.size() == 1u);
    }
}

TEST_CASE("5/catalogue: a refresh with no change costs one request",
          "[registry][catalogue]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto s = new_store();
    auto t = *reg::resolve_target(e->cfg, {});
    for (int i = 0; i < 5; ++i) {
        auto img = add_image(s, make_layer(fmt::format("img{}", i), "/x", false, {}));
        REQUIRE(reg::push(s, img.digest, t, e->ctx));
    }
    // a new reader: the first fill reads each event
    reg::context reader;
    reader.cfg = &e->cfg;
    reader.cache = *util::make_temp_dir();
    auto cat = *reg::catalogue::open(t, reader);
    auto first = cat.refresh();
    REQUIRE(first);
    REQUIRE(first->events == 5u);
    REQUIRE(first->requests == 6u);
    REQUIRE(cat.db().all()->size() == 5u);
    auto again = cat.refresh();
    REQUIRE(again->events == 0u);
    REQUIRE(again->requests == 1u);
}

TEST_CASE("5/catalogue: no update is lost when writers run at the same time",
          "[registry][catalogue]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    const int n = 8;
    std::vector<pid_t> pids;
    for (int i = 0; i < n; ++i) {
        auto pid = fork();
        if (pid == 0) {
            auto s = new_store();
            auto img = add_image(
                s, make_layer(fmt::format("par{}", i), "/x", false, {}));
            auto r = reg::push(s, img.digest, t, e->ctx);
            _exit(r ? 0 : 1);
        }
        pids.push_back(pid);
    }
    for (auto p : pids) {
        int status = 0;
        waitpid(p, &status, 0);
        REQUIRE(WEXITSTATUS(status) == 0);
    }
    reg::context reader;
    reader.cfg = &e->cfg;
    reader.cache = *util::make_temp_dir();
    auto cat = *reg::catalogue::open(t, reader);
    REQUIRE(cat.refresh()->events == unsigned(n));
}

TEST_CASE("5/catalogue: an event with an earlier time than the last event "
          "that a reader saw",
          "[registry][catalogue]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    reg::context reader;
    reader.cfg = &e->cfg;
    reader.cache = *util::make_temp_dir();
    auto cat = *reg::catalogue::open(t, reader);

    auto a = add_image(s, make_layer("a", "/x", false, {}));
    REQUIRE(reg::push(s, a.digest, t, e->ctx));
    REQUIRE(cat.refresh()->events == 1u);

    // a slow writer, or a writer with a clock that is 5 s behind: its event
    // sorts before the event that the reader already saw
    auto slow = *e;
    slow.ctx.cfg = &slow.cfg;
    slow.ctx.clock_offset_us = -5'000'000;
    auto b = add_image(s, make_layer("b", "/x", false, {}));
    REQUIRE(reg::push(s, b.digest, t, slow.ctx));

    SECTION("no overlap: the event is lost") {
        reader.refresh_overlap = 0;
        auto r = cat.refresh();
        // FINDING (catalogue-order): the reader never sees b
        REQUIRE(r->events == 0u);
        REQUIRE(cat.db().all()->size() == 1u);
    }
    SECTION("an overlap of 10 s: the event is found") {
        reader.refresh_overlap = 10;
        auto r = cat.refresh();
        REQUIRE(r->events == 1u);
        REQUIRE(cat.db().all()->size() == 2u);
        // the overlap reads the seen events again, with no request for them
        auto again = cat.refresh();
        REQUIRE(again->requests == 1u);
    }
    SECTION("the default overlap finds the event") {
        auto r = cat.refresh();
        REQUIRE(r->events == 1u);
    }
}

TEST_CASE("5/catalogue: find each repository that has a layer",
          "[registry][catalogue]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    auto a = add_image(s, prgenv(), {{"system", "daint"}});
    auto b = add_image(s, prgenv(), {{"system", "santis"}});
    REQUIRE(reg::push(s, a.digest, t, e->ctx));
    REQUIRE(reg::push(s, b.digest, t, e->ctx));
    auto cat = *reg::catalogue::open(t, e->ctx);
    REQUIRE(cat.refresh());
    auto repos = cat.repositories_with_layer(a.layers[0].digest);
    REQUIRE(repos);
    REQUIRE(repos->size() == 1u);
    REQUIRE(repos->front() == t.repo("prgenv-gnu"));
}

TEST_CASE("4/tags/list pagination", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    for (int i = 0; i < 5; ++i) {
        auto img = add_image(
            s, make_layer("paged", fmt::format("/p{}", i), false, {}));
        REQUIRE(reg::push(s, img.digest, t, e->ctx));
    }
    auto c = *reg::make_client(t, t.repo("paged"), e->ctx);
    auto all = c.list_tags_after(std::nullopt, 2);
    REQUIRE(all);
    REQUIRE(all->tags.size() == 5u);
    REQUIRE(all->requests >= 3u);
}

TEST_CASE("4/a registry with no catalogue: a full reference", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    e->ctx.write_catalogue = false;
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    auto img = add_image(s, tools());
    auto r = reg::push(s, img.digest, t, e->ctx);
    REQUIRE(r);
    auto s2 = new_store();
    // no label lookup: the catalogue is empty
    REQUIRE(!reg::pull(s2, addr(*e, "tools"), e->ctx));
    auto full = fmt::format("{}/{}:{}", shared_zot().base.substr(7),
                            r->repository, r->tag);
    auto p = reg::pull(s2, addr(*e, full), e->ctx);
    REQUIRE(p);
    REQUIRE(p->digest == img.digest);
}

TEST_CASE("4/push from an OCI image layout directory", "[registry]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    auto img = add_image(s, tools());
    REQUIRE(s.update(false, false));
    // the repository is an OCI layout with one image
    auto r = reg::push_layout(s.v2(), t, e->ctx);
    REQUIRE(r);
    REQUIRE(r->digest == img.digest);
    // a layout with two objects is refused
    add_image(s, prgenv());
    REQUIRE(s.update(false, false));
    REQUIRE(!reg::push_layout(s.v2(), t, e->ctx));
}

TEST_CASE("4/delete and registry GC keep the layers of other images",
          "[registry][gc]") {
    zot gcz(R"(,"gc":true,"gcDelay":"1s","gcInterval":"2s","retention":{"delay":"1s","policies":[{"repositories":["**"],"deleteReferrers":true,"deleteUntagged":true}]})");
    if (gcz.base.empty()) {
        SKIP("no zot binary");
    }
    auto e = make_env(gcz.base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    auto a = add_image(s, prgenv(), {{"system", "daint"}});
    auto b = add_image(s, prgenv(), {{"system", "santis"}});
    REQUIRE(reg::push(s, a.digest, t, e->ctx));
    REQUIRE(reg::push(s, b.digest, t, e->ctx));
    REQUIRE(reg::remove(addr(*e, "prgenv-gnu@daint"), e->ctx));
    // wait for GC
    ::sleep(10);
    auto c = *reg::make_client(t, t.repo("prgenv-gnu"), e->ctx);
    auto idx_a = format::parse_index(*s.read_blob(a.digest));
    // the wrapper of a is gone (nothing refers to it)
    auto wa = c.get_manifest(oci::reference::digest(idx_a->wrapper.digest));
    INFO("the wrapper of the deleted image is still in the registry");
    CHECK(!wa);
    // b is complete
    auto s2 = new_store();
    REQUIRE(reg::pull(s2, addr(*e, "prgenv-gnu@santis"), e->ctx));
}

TEST_CASE("5/catalogue: an event that does not match the configs",
          "[registry][catalogue]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    auto app = add_image(s, prgenv());
    // the only event in the log is wrong (a second event for one digest is
    // ignored: the first event wins)
    e->ctx.write_catalogue = false;
    auto r = reg::push(s, app.digest, t, e->ctx);
    REQUIRE(r);
    // a wrong event: the label tools/1.0:v1 for the digest of prgenv-gnu
    auto cat = *reg::catalogue::open(t, e->ctx);
    auto rec = app;
    rec.cfg.name = "tools";
    REQUIRE(cat.append({"add", r->repository, r->tag, rec}));
    auto s2 = new_store();
    auto p = reg::pull(s2, addr(*e, "tools/1.0:v1"), e->ctx);
    // FINDING (catalogue-trust): with no check, the pull gives prgenv-gnu
    REQUIRE(!p);
    REQUIRE(p.error().find("catalogue is not correct") != std::string::npos);
    REQUIRE(e->ctx.st.downloaded == 0u);
}

TEST_CASE("5/catalogue: the cost of a first fill grows with the history",
          "[.bench][catalogue]") {
    REQUIRE_ZOT();
    auto e = make_env(shared_zot().base);
    auto t = *reg::resolve_target(e->cfg, {});
    auto s = new_store();
    auto img = add_image(s, tools());
    auto cat = *reg::catalogue::open(t, e->ctx);
    unsigned total = 0;
    for (unsigned batch = 0; batch < 8; ++batch) {
        // the history: one event for each push, copy or delete
        auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 50; ++i, ++total) {
            auto rec = img;
            rec.cfg.version = fmt::format("{}", total);
            REQUIRE(cat.append({"add", "x/tools", "t", rec}));
        }
        auto append = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - start)
                          .count();
        reg::context reader;
        reader.cfg = &e->cfg;
        reader.cache = *util::make_temp_dir();
        auto c = *reg::catalogue::open(t, reader);
        start = std::chrono::steady_clock::now();
        auto r = c.refresh();
        auto first = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start)
                         .count();
        auto again = c.refresh();
        WARN(fmt::format("events={} 50 appends: {:.2f}s ({:.0f} ms each); "
                         "first fill: {} requests {:.2f}s; refresh: {} "
                         "requests",
                         r->events, append, append * 20, r->requests, first,
                         again->requests));
    }
}

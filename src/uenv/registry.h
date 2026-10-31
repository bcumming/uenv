#pragma once

// Operations on a registry (design/4-registry-ops.md) and the catalogue
// (design/5-discovery.md).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <oci/auth.h>
#include <oci/client.h>
#include <oci/digest.h>
#include <uenv/ops.h>
#include <uenv/settings.h>
#include <uenv/store.h>
#include <util/expected.h>
#include <util/url.h>

namespace uenv::registry {

// the name of the repository of the catalogue in each namespace
inline constexpr std::string_view catalogue_name = "uenv-catalogue";

// a registry and a namespace: where uenv reads and writes repositories
struct target {
    std::string name; // the name of the registry in the config, or the url
    util::url base;   // the origin, for example https://jfrog.svc.cscs.ch
    std::string prefix; // the url path and the namespace, can be empty
    std::string repo(std::string_view name) const;
    std::string catalogue_repo() const {
        return repo(catalogue_name);
    }
};

// an address on the command line (4-registry-ops.md, "Registries and
// addresses")
struct address {
    target where;
    // a label (with the layer marker), or an id
    std::optional<std::string> label;
    // a full reference: the repository (with no prefix) and a tag or digest
    std::optional<std::string> repository;
    std::optional<std::string> reference;
};

// the registry of an address with no registry, or of --registry
util::expected<target, std::string>
resolve_target(const configuration& cfg,
               std::optional<std::string> registry_option);

util::expected<address, std::string>
parse_address(const std::string& text, const configuration& cfg,
              std::optional<std::string> registry_option);

// what an operation did, to check the design rules in tests
struct stats {
    unsigned uploaded = 0;
    unsigned mounted = 0;
    unsigned skipped = 0;
    unsigned downloaded = 0;
    unsigned manifests = 0;
    unsigned referrers_copied = 0;
    unsigned requests = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    // the order of the writes: "blob:<digest>", "manifest:<digest>",
    // "tag:<tag>", "referrer:<digest>", "event:<tag>"
    std::vector<std::string> writes;
};

struct context {
    const configuration* cfg = nullptr;
    std::optional<oci::credentials> creds;
    // the directory of the local copies of the catalogues
    std::filesystem::path cache;
    label_keys keys;
    std::optional<std::string> default_system;
    // the catalogue: if false, nothing is appended (to test a registry
    // with no catalogue)
    bool write_catalogue = true;
    // the overlap of a refresh, in seconds: re-read the events of a writer
    // that is slow, or whose clock is behind. Seen events are skipped, so
    // the overlap costs no request.
    unsigned refresh_overlap = 300;
    // a test hook: an offset of the clock of this writer, in microseconds,
    // to make a writer with a clock that is wrong, or a slow writer
    std::int64_t clock_offset_us = 0;
    stats st;
};

//
// the catalogue: a log of events in the registry
//

struct event {
    std::string op; // "add" or "delete"
    std::string repository;
    std::string tag;
    object_record record;
};

struct refresh_stats {
    unsigned requests = 0;
    unsigned events = 0;
};

class catalogue {
  public:
    static util::expected<catalogue, std::string> open(const target& t,
                                                       context& ctx);
    // append one event: one new tagged manifest in the catalogue repository
    util::expected<std::string, std::string> append(const event& e);
    // read the events after the last one that this copy has
    util::expected<refresh_stats, std::string> refresh();
    object_db& db() {
        return db_;
    }
    // each repository that has a given layer
    util::expected<std::vector<std::string>, std::string>
    repositories_with_layer(const oci::digest& layer);
    // find one object by a label
    util::expected<object_record, std::string> find(const std::string& text);

  private:
    catalogue(target t, object_db db, context* ctx)
        : target_(std::move(t)), db_(std::move(db)), ctx_(ctx) {
    }
    util::expected<void, std::string> apply(const std::string& tag,
                                            const nlohmann::json& j);
    target target_;
    object_db db_;
    context* ctx_;
};

// the tag of an event: "e-<UTC time to the µs>-<hash of the writer>"
std::string event_tag(std::int64_t offset_us = 0);

//
// the operations
//

struct push_result {
    std::string repository;
    std::string tag;
    oci::digest digest;
};

// push an image or a layer from a local repository
util::expected<push_result, std::string>
push(store& s, const oci::digest& object, const target& t, context& ctx);

// push from an OCI image layout directory that holds one image or layer
util::expected<push_result, std::string>
push_layout(const std::filesystem::path& dir, const target& t, context& ctx);

// pull an image or a layer into a local repository
util::expected<object_record, std::string>
pull(store& s, const address& a, context& ctx);

// copy an image. With changes (for example {"release": "v2"} or
// {"meta.system": "santis"}), the copy is a new image with the same layers.
util::expected<push_result, std::string>
copy(const address& src, const target& dst, context& ctx,
     const std::vector<config_change>& changes = {});

// delete the top-level object of an image or of a building block
util::expected<push_result, std::string> remove(const address& a,
                                                context& ctx);

// attach a referrer of a kind that the site defines
util::expected<std::vector<std::string>, std::string>
attach(const address& subject, const std::string& kind,
       const std::string& content, context& ctx);

// the referrers of an object in a repository
util::expected<std::vector<oci::descriptor>, std::string>
list_referrers(const target& t, const std::string& repository,
               const oci::digest& subject, context& ctx);

// find an object for an address: the repository and the digest
struct located {
    std::string repository;
    oci::digest digest;
    std::optional<std::string> tag;
};
util::expected<located, std::string> locate(const address& a, context& ctx);

util::expected<oci::client, std::string>
make_client(const target& t, const std::string& repository, context& ctx);

} // namespace uenv::registry

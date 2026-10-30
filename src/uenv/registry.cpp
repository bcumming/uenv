#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <oci/client.h>
#include <oci/manifest.h>
#include <oci/reference.h>
#include <oci/types.h>
#include <uenv/format.h>
#include <uenv/registry.h>
#include <uenv/store.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/sha.h>
#include <util/strings.h>

namespace uenv::registry {

namespace fs = std::filesystem;
using json = nlohmann::json;
using util::unexpected;

//
// targets and addresses
//

std::string target::repo(std::string_view name) const {
    if (prefix.empty()) {
        return std::string(name);
    }
    return fmt::format("{}/{}", prefix, name);
}

namespace {

bool is_local_host(std::string_view host) {
    return host.starts_with("localhost") || host.starts_with("127.") ||
           host.starts_with("[::1]");
}

util::expected<target, std::string> target_from_url(std::string name,
                                                    std::string url,
                                                    std::optional<std::string> ns) {
    if (url.find("://") == std::string::npos) {
        url = (is_local_host(url) ? "http://" : "https://") + url;
    }
    auto u = util::parse_url(url);
    if (!u) {
        return unexpected{fmt::format("invalid registry url '{}'", url)};
    }
    auto loc = oci::split_registry(*u);
    std::string prefix = loc.prefix;
    if (ns && !ns->empty()) {
        prefix = prefix.empty() ? *ns : prefix + "/" + *ns;
    }
    return target{std::move(name), loc.base, prefix};
}

util::expected<target, std::string> named(const configuration& cfg,
                                          const std::string& name,
                                          std::optional<std::string> ns) {
    for (auto& r : cfg.registries) {
        if (r.name == name) {
            return target_from_url(name, r.url.string(),
                                   ns ? ns : r.namespace_);
        }
    }
    std::vector<std::string> names;
    for (auto& r : cfg.registries) {
        names.push_back(r.name);
    }
    return unexpected{fmt::format(
        "there is no registry '{}'. The registries are: {}", name,
        names.empty() ? std::string{"(none)"}
                      : fmt::format("{}", fmt::join(names, ", ")))};
}

} // namespace

util::expected<target, std::string>
resolve_target(const configuration& cfg,
               std::optional<std::string> registry_option) {
    if (registry_option) {
        for (auto& r : cfg.registries) {
            if (r.name == *registry_option) {
                return named(cfg, r.name, std::nullopt);
            }
        }
        // a URL: a registry with no namespace
        return target_from_url(*registry_option, *registry_option,
                               std::nullopt);
    }
    if (!cfg.default_registry) {
        return unexpected{std::string{
            "there is no default registry: set default_registry in the "
            "configuration, or use --registry"}};
    }
    return named(cfg, *cfg.default_registry, std::nullopt);
}

util::expected<address, std::string>
parse_address(const std::string& text, const configuration& cfg,
              std::optional<std::string> registry_option) {
    // reg[/ns]::label
    if (auto pos = text.find("::"); pos != std::string::npos) {
        auto left = text.substr(0, pos);
        auto label = text.substr(pos + 2);
        auto slash = left.find('/');
        auto reg = left.substr(0, slash);
        std::optional<std::string> ns;
        if (slash != std::string::npos) {
            ns = left.substr(slash + 1);
        }
        auto t = named(cfg, reg, ns);
        if (!t) {
            return unexpected{t.error()};
        }
        address a{.where = *t};
        if (!label.empty()) {
            a.label = label;
        }
        return a;
    }
    // a full reference starts with a host
    auto first = text.substr(0, text.find('/'));
    if (text.find('/') != std::string::npos &&
        (first.find('.') != std::string::npos ||
         first.find(':') != std::string::npos || first == "localhost")) {
        auto rest = text.substr(first.size() + 1);
        std::string repo, ref;
        if (auto at = rest.find('@'); at != std::string::npos) {
            repo = rest.substr(0, at);
            ref = rest.substr(at + 1);
        } else if (auto colon = rest.rfind(':');
                   colon != std::string::npos &&
                   rest.find('/', colon) == std::string::npos) {
            repo = rest.substr(0, colon);
            ref = rest.substr(colon + 1);
        } else {
            return unexpected{fmt::format(
                "the full reference '{}' must end with a tag or a digest", text)};
        }
        auto t = target_from_url(first, first, std::nullopt);
        if (!t) {
            return unexpected{t.error()};
        }
        return address{.where = *t, .repository = repo, .reference = ref};
    }
    auto t = resolve_target(cfg, registry_option);
    if (!t) {
        return unexpected{t.error()};
    }
    return address{.where = *t, .label = text};
}

util::expected<oci::client, std::string>
make_client(const target& t, const std::string& repository, context& ctx) {
    auto c = oci::client::create(t.base, repository, ctx.creds);
    if (!c) {
        return unexpected{fmt::format("{}: {}", t.base.string(),
                                      c.error().message)};
    }
    return std::move(*c);
}

//
// records as JSON (catalogue events)
//

namespace {

json config_json(const config& c) {
    return json{{"name", c.name},
                {"version", c.version},
                {"release", c.release},
                {"description", c.description},
                {"meta", c.meta}};
}

config config_from(const json& j) {
    return config{.name = j.value("name", ""),
                  .version = j.value("version", ""),
                  .release = j.value("release", ""),
                  .description = j.value("description", ""),
                  .meta = j.value("meta", std::map<std::string, std::string>{})};
}

json record_json(const object_record& r) {
    json layers = json::array();
    for (auto& l : r.layers) {
        layers.push_back(json{{"alias", l.alias},
                              {"mount", l.mount},
                              {"digest", l.digest.string()},
                              {"config", config_json(l.cfg)},
                              {"build_mount", l.build_mount},
                              {"relocatable", l.relocatable},
                              {"squashfs", l.squashfs.string()},
                              {"squashfs_size", l.squashfs_size}});
    }
    json j{{"kind", r.kind == format::object_kind::image ? "image" : "layer"},
           {"digest", r.digest.string()},
           {"config", config_json(r.cfg)},
           {"size", r.size},
           {"layers", layers}};
    if (r.kind == format::object_kind::layer) {
        j["mount"] = r.mount;
        j["relocatable"] = r.relocatable;
        j["squashfs"] = r.squashfs->string();
    }
    return j;
}

util::expected<object_record, std::string> record_from(const json& j) {
    try {
        auto d = oci::digest::parse(j.at("digest").get<std::string>());
        if (!d) {
            return unexpected{std::string{"an event has an invalid digest"}};
        }
        object_record r{.kind = j.at("kind") == "image"
                                    ? format::object_kind::image
                                    : format::object_kind::layer,
                        .digest = *d};
        r.id = format::short_id(*d);
        r.cfg = config_from(j.at("config"));
        r.size = j.value("size", std::uint64_t(0));
        r.added = now_utc();
        for (auto& l : j.at("layers")) {
            r.layers.push_back(layer_ref{
                .alias = l.at("alias"),
                .mount = l.at("mount"),
                .digest = *oci::digest::parse(l.at("digest").get<std::string>()),
                .cfg = config_from(l.at("config")),
                .build_mount = l.at("build_mount"),
                .relocatable = l.at("relocatable"),
                .squashfs =
                    *oci::digest::parse(l.at("squashfs").get<std::string>()),
                .squashfs_size = l.at("squashfs_size")});
        }
        if (r.kind == format::object_kind::layer) {
            r.mount = j.at("mount");
            r.relocatable = j.at("relocatable");
            r.squashfs = *oci::digest::parse(j.at("squashfs").get<std::string>());
            r.standalone = true;
        }
        return r;
    } catch (std::exception& e) {
        return unexpected{fmt::format("an event is not valid: {}", e.what())};
    }
}

std::string writer_hash() {
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    return util::sha256_string(fmt::format("{}.{}", getpid(), host))
        .string()
        .substr(0, 8);
}

std::string format_time(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto us = duration_cast<microseconds>(tp.time_since_epoch()).count();
    std::time_t secs = us / 1000000;
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", &tm);
    return fmt::format("{}.{:06d}Z", buf, us % 1000000);
}

// the time in an event tag, as a time point
std::optional<std::chrono::system_clock::time_point>
tag_time(const std::string& tag) {
    // e-YYYYMMDDTHHMMSS.ffffffZ-xxxxxxxx
    if (tag.size() < 25 || !tag.starts_with("e-")) {
        return std::nullopt;
    }
    std::tm tm{};
    if (!strptime(tag.substr(2, 15).c_str(), "%Y%m%dT%H%M%S", &tm)) {
        return std::nullopt;
    }
    auto secs = timegm(&tm);
    auto us = std::stol(tag.substr(18, 6));
    return std::chrono::system_clock::from_time_t(secs) +
           std::chrono::microseconds(us);
}

} // namespace

std::string event_tag(std::int64_t offset_us) {
    auto now = std::chrono::system_clock::now() +
               std::chrono::microseconds(offset_us);
    return fmt::format("e-{}-{}", format_time(now), writer_hash());
}

//
// the catalogue
//

util::expected<catalogue, std::string> catalogue::open(const target& t,
                                                       context& ctx) {
    auto key = util::sha256_string(t.base.string() + "/" + t.prefix)
                   .string()
                   .substr(0, 16);
    auto dir = ctx.cache / "catalogue" / key;
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto db = object_db::open(dir / "index.db", util::sqlite::open_mode::create);
    if (!db) {
        return unexpected{db.error()};
    }
    if (auto r = db->db().exec(
            "CREATE TABLE IF NOT EXISTS catalogue_state (key TEXT PRIMARY KEY, "
            "value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS catalogue_seen (tag TEXT PRIMARY KEY);");
        !r) {
        return unexpected{r.error()};
    }
    return catalogue{t, std::move(*db), &ctx};
}

util::expected<std::string, std::string> catalogue::append(const event& e) {
    json ev{{"op", e.op},
            {"repository", e.repository},
            {"tag", e.tag},
            {"object", record_json(e.record)}};
    auto bytes = format::canonical(ev);
    if (!bytes) {
        return unexpected{bytes.error()};
    }
    auto cfg = format::make_blob(*bytes);
    auto c = make_client(target_, target_.catalogue_repo(), *ctx_);
    if (!c) {
        return unexpected{c.error()};
    }
    if (auto r = c->put_blob_bytes(cfg.digest, cfg.bytes); !r) {
        return unexpected{r.error().message};
    }
    auto empty = format::make_blob(std::string(oci::empty_config_body));
    if (auto r = c->put_blob_bytes(empty.digest, empty.bytes); !r) {
        return unexpected{r.error().message};
    }
    json m{{"schemaVersion", 2},
           {"mediaType", oci::media_type_manifest},
           {"artifactType", format::at_catalogue_event},
           {"config",
            {{"mediaType", format::mt_catalogue_event},
             {"digest", cfg.digest.string()},
             {"size", cfg.size()},
             {"data", util::base64_encode(cfg.bytes)}}},
           {"layers",
            json::array({{{"mediaType", oci::media_type_empty},
                          {"digest", empty.digest.string()},
                          {"size", 2},
                          {"data", oci::empty_config_data}}})}};
    auto body = format::canonical(m);
    auto tag = event_tag(ctx_->clock_offset_us);
    auto t = oci::tag::parse(tag);
    if (!t) {
        return unexpected{fmt::format("invalid event tag {}", tag)};
    }
    if (auto r = c->put_manifest(oci::reference::tag(*t), *body); !r) {
        return unexpected{r.error().message};
    }
    ctx_->st.writes.push_back("event:" + tag);
    return tag;
}

util::expected<void, std::string> catalogue::apply(const std::string& tag,
                                                   const json& ev) {
    auto& db = db_.db();
    auto op = ev.value("op", "");
    auto repo = ev.value("repository", "");
    auto rec = record_from(ev.at("object"));
    if (!rec) {
        return unexpected{rec.error()};
    }
    if (op == "add") {
        rec->repository = repo;
        rec->tag = ev.value("tag", "");
        if (auto r = db_.add(*rec, {}); !r) {
            return r;
        }
    } else if (op == "delete") {
        if (auto r = db.query(
                "DELETE FROM locations WHERE object = ? AND repository = ?",
                {rec->digest.string(), repo});
            !r) {
            return unexpected{r.error()};
        }
        auto left = db.query("SELECT 1 FROM locations WHERE object = ?",
                             {rec->digest.string()});
        if (left && left->empty()) {
            if (auto r = db_.remove(rec->digest); !r) {
                return unexpected{r.error()};
            }
        }
    }
    auto seen = db.query("INSERT OR IGNORE INTO catalogue_seen (tag) VALUES (?)",
                         {tag});
    if (!seen) {
        return unexpected{seen.error()};
    }
    return {};
}

util::expected<refresh_stats, std::string> catalogue::refresh() {
    refresh_stats st;
    auto& db = db_.db();
    auto last = db.query("SELECT value FROM catalogue_state WHERE key = 'last'");
    if (!last) {
        return unexpected{last.error()};
    }
    std::optional<std::string> after;
    if (!last->empty()) {
        after = last->front().text("value");
        if (ctx_->refresh_overlap > 0) {
            // re-read the events of the last `overlap` seconds. The value of
            // `last` must be a tag that exists: zot returns no tags for a
            // value of `last` that is not a tag. Thus use the newest tag
            // that this copy saw before the start of the overlap.
            after.reset();
            if (auto t = tag_time(last->front().text("value"))) {
                auto cutoff = "e-" + format_time(*t - std::chrono::seconds(
                                                          ctx_->refresh_overlap));
                auto prev = db.query(
                    "SELECT MAX(tag) AS tag FROM catalogue_seen WHERE tag < ?",
                    {cutoff});
                if (prev && !prev->empty()) {
                    after = prev->front().opt_text("tag");
                }
            }
        }
    }
    auto c = make_client(target_, target_.catalogue_repo(), *ctx_);
    if (!c) {
        return unexpected{c.error()};
    }
    auto page = c->list_tags_after(after, 1000);
    if (!page) {
        return unexpected{page.error().message};
    }
    st.requests += page->requests;
    std::vector<std::string> tags = page->tags;
    std::sort(tags.begin(), tags.end());
    std::optional<std::string> newest =
        last->empty() ? std::nullopt
                      : std::optional{last->front().text("value")};
    for (auto& tag : tags) {
        if (!tag.starts_with("e-")) {
            // for example the fallback tags of referrers
            continue;
        }
        auto seen = db.query("SELECT 1 FROM catalogue_seen WHERE tag = ?", {tag});
        if (seen && !seen->empty()) {
            continue;
        }
        auto t = oci::tag::parse(tag);
        auto m = c->get_manifest(oci::reference::tag(*t));
        st.requests += 1;
        if (!m) {
            return unexpected{m.error().message};
        }
        auto mj = json::parse(m->body, nullptr, false);
        auto data = mj.is_object() && mj.contains("config")
                        ? mj["config"].value("data", "")
                        : std::string{};
        auto bytes = util::base64_decode(data);
        if (!bytes) {
            spdlog::warn("the catalogue event {} has no data: it is ignored",
                         tag);
            continue;
        }
        // the inline data must match the digest of the config
        auto cd = oci::digest::parse(mj["config"].value("digest", ""));
        if (!cd || oci::digest::sha256(util::sha256_string(*bytes)) != *cd) {
            spdlog::warn("the catalogue event {} does not match its digest", tag);
            continue;
        }
        auto ev = json::parse(*bytes, nullptr, false);
        if (ev.is_discarded()) {
            continue;
        }
        if (auto r = apply(tag, ev); !r) {
            spdlog::warn("the catalogue event {}: {}", tag, r.error());
            continue;
        }
        st.events += 1;
        if (!newest || tag > *newest) {
            newest = tag;
        }
    }
    if (newest) {
        auto r = db.query("INSERT OR REPLACE INTO catalogue_state (key, value) "
                          "VALUES ('last', ?)",
                          {*newest});
        if (!r) {
            return unexpected{r.error()};
        }
    }
    return st;
}

util::expected<std::vector<std::string>, std::string>
catalogue::repositories_with_layer(const oci::digest& layer) {
    auto rows = db_.db().query(
        "SELECT DISTINCT repository FROM locations WHERE object = ?1 UNION "
        "SELECT DISTINCT loc.repository FROM image_layers il JOIN locations "
        "loc ON loc.object = il.image WHERE il.layer = ?1",
        {layer.string()});
    if (!rows) {
        return unexpected{rows.error()};
    }
    std::vector<std::string> out;
    for (auto& r : *rows) {
        out.push_back(r.text("repository"));
    }
    return out;
}

util::expected<object_record, std::string>
catalogue::find(const std::string& text) {
    auto q = parse_label_query(text, ctx_->keys, ctx_->default_system);
    if (!q) {
        return unexpected{q.error()};
    }
    auto r = db_.query(*q);
    if (!r) {
        return unexpected{r.error()};
    }
    if (r->empty()) {
        return unexpected{fmt::format("no {} in {} matches '{}'",
                                      q->layer ? "layer" : "image",
                                      target_.name, text)};
    }
    if (r->size() > 1u) {
        std::vector<std::string> lines;
        for (auto& rec : *r) {
            std::vector<std::string> meta;
            for (auto& [k, v] : rec.cfg.meta) {
                meta.push_back(fmt::format("{}={}", k, v));
            }
            lines.push_back(fmt::format("  {}/{}:{} {} id={}", rec.cfg.name,
                                        rec.cfg.version, rec.cfg.release,
                                        fmt::join(meta, ","), rec.id));
        }
        return unexpected{fmt::format(
            "'{}' matches more than one {}: give the id\n{}", text,
            q->layer ? "layer" : "image", fmt::join(lines, "\n"))};
    }
    return r->front();
}

//
// locate
//

util::expected<located, std::string> locate(const address& a, context& ctx) {
    if (a.repository && a.reference) {
        auto c = make_client(a.where, *a.repository, ctx);
        if (!c) {
            return unexpected{c.error()};
        }
        std::optional<oci::reference> ref;
        if (auto d = oci::digest::parse(*a.reference)) {
            ref = oci::reference::digest(*d);
        } else if (auto t = oci::tag::parse(*a.reference)) {
            ref = oci::reference::tag(*t);
        } else {
            return unexpected{
                fmt::format("invalid tag or digest '{}'", *a.reference)};
        }
        auto m = c->get_manifest(*ref);
        if (!m) {
            return unexpected{m.error().message};
        }
        std::optional<std::string> tag;
        if (!oci::digest::parse(*a.reference)) {
            tag = *a.reference;
        }
        return located{*a.repository, m->digest, tag};
    }
    if (!a.label) {
        return unexpected{std::string{"no image is given"}};
    }
    auto cat = catalogue::open(a.where, ctx);
    if (!cat) {
        return unexpected{cat.error()};
    }
    if (auto r = cat->refresh(); !r) {
        return unexpected{r.error()};
    }
    auto rec = cat->find(*a.label);
    if (!rec) {
        return unexpected{rec.error()};
    }
    if (rec->repository) {
        return located{*rec->repository, rec->digest, rec->tag};
    }
    // a layer with no tag: it is in the repository of an image
    auto repos = cat->repositories_with_layer(rec->digest);
    if (!repos || repos->empty()) {
        return unexpected{fmt::format("the layer {} is in no repository",
                                      rec->digest.string())};
    }
    return located{repos->front(), rec->digest, std::nullopt};
}

//
// copy blobs and manifests
//

namespace {

struct repo_handle {
    target where;
    std::string repository;
    oci::client client;
};

util::expected<repo_handle, std::string>
open_repo(const target& t, const std::string& repo, context& ctx) {
    auto c = make_client(t, repo, ctx);
    if (!c) {
        return unexpected{c.error()};
    }
    return repo_handle{t, repo, std::move(*c)};
}

// a source of a blob: a local file, bytes, or a repository
struct blob_source {
    std::optional<fs::path> file;
    std::optional<std::string> bytes;
    repo_handle* repo = nullptr;
};

// "uenv uploads a blob only if the registry does not have it"
util::expected<void, std::string>
ensure_blob(repo_handle& dst, const oci::digest& d, std::uint64_t size,
            const blob_source& src, const std::vector<std::string>& candidates,
            context& ctx) {
    auto exists = dst.client.blob_exists(d);
    if (!exists) {
        return unexpected{exists.error().message};
    }
    if (*exists) {
        ctx.st.skipped++;
        return {};
    }
    // a cross-repository mount from a repository of the same registry
    std::vector<std::string> from = candidates;
    if (src.repo && src.repo->where.base.string() == dst.where.base.string()) {
        from.insert(from.begin(), src.repo->repository);
    }
    for (auto& repo : from) {
        if (repo == dst.repository) {
            continue;
        }
        dst.client.add_pull_scope(repo);
        auto m = dst.client.mount_blob(d, repo);
        if (m && *m) {
            ctx.st.mounted++;
            ctx.st.writes.push_back("mount:" + d.string());
            return {};
        }
    }
    if (src.bytes) {
        if (auto r = dst.client.put_blob_bytes(d, *src.bytes); !r) {
            return unexpected{r.error().message};
        }
    } else if (src.file) {
        if (auto r = dst.client.put_blob(d, *src.file); !r) {
            return unexpected{r.error().message};
        }
    } else if (src.repo) {
        auto tmp = util::make_temp_dir();
        if (!tmp) {
            return unexpected{tmp.error()};
        }
        auto file = *tmp / d.hex();
        if (auto r = src.repo->client.get_blob_to_file(d, file); !r) {
            return unexpected{r.error().message};
        }
        ctx.st.downloaded++;
        if (auto r = dst.client.put_blob(d, file); !r) {
            return unexpected{r.error().message};
        }
        std::error_code ec;
        fs::remove(file, ec);
    } else {
        return unexpected{fmt::format("no source for the blob {}", d.string())};
    }
    ctx.st.uploaded++;
    ctx.st.uploaded_bytes += size;
    ctx.st.writes.push_back("blob:" + d.string());
    return {};
}

util::expected<void, std::string> put_manifest(repo_handle& dst,
                                               const std::string& ref_text,
                                               const std::string& body,
                                               std::string_view media_type,
                                               context& ctx) {
    std::optional<oci::reference> ref;
    if (auto d = oci::digest::parse(ref_text)) {
        ref = oci::reference::digest(*d);
    } else if (auto t = oci::tag::parse(ref_text)) {
        ref = oci::reference::tag(*t);
    } else {
        return unexpected{fmt::format("invalid reference {}", ref_text)};
    }
    if (auto r = dst.client.put_manifest(*ref, body, media_type); !r) {
        return unexpected{fmt::format("unable to write {} to {}: {}", ref_text,
                                      dst.repository, r.error().message)};
    }
    ctx.st.manifests++;
    ctx.st.writes.push_back(
        (oci::digest::parse(ref_text) ? "manifest:" : "tag:") + ref_text);
    return {};
}

// does a rule let a referrer travel with an operation
bool travels(const context& ctx, const std::optional<std::string>& artifact_type,
             std::string_view op) {
    if (ctx.cfg && artifact_type) {
        for (auto& r : ctx.cfg->referrer_rules) {
            if (r.artifact_type == *artifact_type) {
                return std::find(r.travel.begin(), r.travel.end(), op) !=
                       r.travel.end();
            }
        }
    }
    // the defaults: copy copies each referrer; compose copies the referrers
    // of each layer
    return true;
}

// copy the referrers of a subject from one repository to another
util::expected<unsigned, std::string>
copy_referrers(repo_handle& src, repo_handle& dst, const oci::digest& subject,
               std::string_view op, context& ctx) {
    auto list = src.client.referrers(subject);
    if (!list) {
        // a registry with no referrers: nothing to copy
        spdlog::debug("referrers of {} in {}: {}", subject.string(),
                      src.repository, list.error().message);
        return 0u;
    }
    unsigned n = 0;
    for (auto& d : *list) {
        if (!travels(ctx, d.artifact_type, op)) {
            continue;
        }
        auto m = src.client.get_manifest(oci::reference::digest(d.digest));
        if (!m) {
            return unexpected{m.error().message};
        }
        auto j = json::parse(m->body, nullptr, false);
        if (j.is_discarded()) {
            continue;
        }
        std::vector<json> descs;
        if (j.contains("config")) {
            descs.push_back(j["config"]);
        }
        for (auto& l : j.value("layers", json::array())) {
            descs.push_back(l);
        }
        for (auto& bd : descs) {
            auto bdig = oci::digest::parse(bd.value("digest", ""));
            if (!bdig) {
                continue;
            }
            blob_source s{.repo = &src};
            if (bd.contains("data")) {
                s.bytes = util::base64_decode(bd["data"].get<std::string>());
            }
            if (auto r = ensure_blob(dst, *bdig, bd.value("size", 0ul), s, {},
                                     ctx);
                !r) {
                return unexpected{r.error()};
            }
        }
        if (auto r = put_manifest(dst, d.digest.string(), m->body,
                                  oci::media_type_manifest, ctx);
            !r) {
            return unexpected{r.error()};
        }
        ctx.st.writes.back() = "referrer:" + d.digest.string();
        ctx.st.referrers_copied++;
        ++n;
    }
    return n;
}

// the tag check: uenv does not change a tag
util::expected<bool, std::string> check_tag(repo_handle& dst,
                                            const std::string& tag,
                                            const oci::digest& d) {
    auto t = oci::tag::parse(tag);
    if (!t) {
        return unexpected{fmt::format("invalid tag {}", tag)};
    }
    auto m = dst.client.get_manifest(oci::reference::tag(*t));
    if (!m) {
        // assume the tag does not exist
        return false;
    }
    if (m->digest != d) {
        return unexpected{fmt::format(
            "the tag {} in {} already points at {}: uenv does not change a "
            "tag",
            tag, dst.repository, m->digest.string())};
    }
    return true;
}

void append_event(const target& t, const std::string& op,
                  const std::string& repository, const std::string& tag,
                  const object_record& rec, context& ctx) {
    if (!ctx.write_catalogue) {
        return;
    }
    test_fault("push-before-event");
    auto cat = catalogue::open(t, ctx);
    if (!cat) {
        spdlog::warn("the catalogue was not updated: {}", cat.error());
        return;
    }
    if (auto r = cat->append({op, repository, tag, rec}); !r) {
        spdlog::warn("the catalogue was not updated: {}", r.error());
    }
}

// the repositories that can have the blobs of a layer: its building block,
// and each repository that the catalogue knows
std::vector<std::string> layer_candidates(const target& t, const layer& l,
                                          context& ctx) {
    std::vector<std::string> out{t.repo(l.config.name)};
    if (auto cat = catalogue::open(t, ctx)) {
        if (auto repos = cat->repositories_with_layer(*l.digest)) {
            for (auto& r : *repos) {
                if (std::find(out.begin(), out.end(), r) == out.end()) {
                    out.push_back(r);
                }
            }
        }
    }
    return out;
}

} // namespace

//
// push
//

util::expected<push_result, std::string>
push(store& s, const oci::digest& object, const target& t, context& ctx) {
    auto img = s.load(object);
    if (!img) {
        return unexpected{img.error()};
    }
    auto top = s.read_blob(object);
    if (!top) {
        return unexpected{top.error()};
    }
    const bool is_layer = img->kind == uenv::image_kind::layer;
    const auto& cfg = is_layer ? img->layers.front().layer.config : img->config;
    const auto repository = t.repo(cfg.name);
    const auto tag = format::make_tag(cfg, object);
    auto dst = open_repo(t, repository, ctx);
    if (!dst) {
        return unexpected{dst.error()};
    }
    auto exists = check_tag(*dst, tag, object);
    if (!exists) {
        return unexpected{exists.error()};
    }

    // the catalogue: for the mounts of the blobs of each layer
    if (auto cat = catalogue::open(t, ctx)) {
        (void)cat->refresh();
    }

    // 1. the blobs
    for (auto& l : img->layers) {
        auto lm = format::parse_layer_manifest(*s.read_blob(*l.layer.digest));
        auto candidates = layer_candidates(t, l.layer, ctx);
        if (auto r = ensure_blob(*dst, *l.layer.store.digest, l.layer.store.size,
                                 {.file = s.blob_path(*l.layer.store.digest)},
                                 candidates, ctx);
            !r) {
            return unexpected{r.error()};
        }
        auto cbytes = s.read_blob(lm->config.digest);
        if (auto r = ensure_blob(*dst, lm->config.digest, cbytes->size(),
                                 {.bytes = *cbytes}, {}, ctx);
            !r) {
            return unexpected{r.error()};
        }
    }
    std::optional<format::image_index> idx;
    std::optional<format::wrapper_manifest> wrapper;
    if (!is_layer) {
        idx = *format::parse_index(*top);
        wrapper = *format::parse_wrapper_manifest(*s.read_blob(idx->wrapper.digest));
        auto ic = s.read_blob(wrapper->config.digest);
        if (auto r = ensure_blob(*dst, wrapper->config.digest, ic->size(),
                                 {.bytes = *ic}, {}, ctx);
            !r) {
            return unexpected{r.error()};
        }
        // the empty blob, also if the wrapper holds it in `data`
        auto empty = format::make_blob(std::string(oci::empty_config_body));
        if (auto r = ensure_blob(*dst, empty.digest, 2, {.bytes = empty.bytes},
                                 {}, ctx);
            !r) {
            return unexpected{r.error()};
        }
    }
    test_fault("push-after-blobs");

    // 2. the manifests
    if (!is_layer) {
        std::set<std::string> done;
        for (auto& l : img->layers) {
            if (!done.insert(l.layer.digest->string()).second) {
                continue;
            }
            if (auto r = put_manifest(*dst, l.layer.digest->string(),
                                      *s.read_blob(*l.layer.digest),
                                      oci::media_type_manifest, ctx);
                !r) {
                return unexpected{r.error()};
            }
        }
        if (auto r = put_manifest(*dst, idx->wrapper.digest.string(),
                                  *s.read_blob(idx->wrapper.digest),
                                  oci::media_type_manifest, ctx);
            !r) {
            return unexpected{r.error()};
        }
    }
    test_fault("push-before-tag");

    // 3. the top-level object, with its tag
    if (auto r = put_manifest(*dst, tag, *top,
                              is_layer ? oci::media_type_manifest
                                       : oci::media_type_index,
                              ctx);
        !r) {
        return unexpected{r.error()};
    }

    // 4. the referrers of the layers (compose: the layers come from building
    // blocks, and their referrers travel)
    if (!is_layer) {
        std::set<std::string> done;
        for (auto& l : img->layers) {
            if (!done.insert(l.layer.digest->string()).second) {
                continue;
            }
            for (auto& repo : layer_candidates(t, l.layer, ctx)) {
                if (repo == repository) {
                    continue;
                }
                auto src = open_repo(t, repo, ctx);
                if (!src) {
                    continue;
                }
                (void)copy_referrers(*src, *dst, *l.layer.digest, "compose",
                                     ctx);
            }
        }
    }

    auto rec = s.db().get(object);
    if (rec && *rec) {
        append_event(t, "add", repository, tag, **rec, ctx);
    }
    return push_result{repository, tag, object};
}

util::expected<push_result, std::string>
push_layout(const fs::path& dir, const target& t, context& ctx) {
    // read the layout: index.json must list exactly one object
    auto idx_bytes = util::read_file(dir / "index.json");
    if (!idx_bytes) {
        return unexpected{fmt::format("{} is not an OCI image layout",
                                      dir.string())};
    }
    auto idx = json::parse(*idx_bytes, nullptr, false);
    if (idx.is_discarded() || !idx.contains("manifests") ||
        idx["manifests"].size() != 1u) {
        return unexpected{fmt::format(
            "the index.json of {} must list exactly one image or layer",
            dir.string())};
    }
    auto d = oci::digest::parse(idx["manifests"][0].value("digest", ""));
    if (!d) {
        return unexpected{std::string{"index.json has an invalid digest"}};
    }
    // check the object as a pull does: load it into a temporary store.
    // The files are linked, so nothing is copied.
    auto tmp = util::make_temp_dir();
    if (!tmp) {
        return unexpected{tmp.error()};
    }
    auto s = store::create(*tmp / "repo");
    if (!s) {
        return unexpected{s.error()};
    }
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir / "blobs" / "sha256", ec)) {
        auto bd = oci::digest::parse("sha256:" + e.path().filename().string());
        if (!bd) {
            continue;
        }
        if (auto r = s->link_file(e.path(), *bd); !r) {
            return unexpected{r.error()};
        }
    }
    // the digest of each blob that the object uses is checked as it is read
    auto img = s->load(*d);
    if (!img) {
        return unexpected{fmt::format("the image in {} is not valid: {}",
                                      dir.string(), img.error())};
    }
    for (auto& l : img->layers) {
        // a squashfs is large: check it as it is linked
        auto h = util::sha256_file(s->blob_path(*l.layer.store.digest));
        if (!h || oci::digest::sha256(*h) != *l.layer.store.digest) {
            return unexpected{fmt::format("the squashfs {} does not match its "
                                          "digest",
                                          l.layer.store.digest->string())};
        }
    }
    if (!s->has_blob(*oci::digest::parse(
            fmt::format("sha256:{}", oci::empty_config_hex)))) {
        (void)s->put_bytes(format::make_blob(std::string(oci::empty_config_body)));
    }
    if (auto r = s->record(*d, img->kind == uenv::image_kind::layer); !r) {
        return unexpected{r.error()};
    }
    return push(*s, *d, t, ctx);
}

//
// pull
//

util::expected<object_record, std::string> pull(store& s, const address& a,
                                                context& ctx) {
    auto loc = locate(a, ctx);
    if (!loc) {
        return unexpected{loc.error()};
    }
    auto src = open_repo(a.where, loc->repository, ctx);
    if (!src) {
        return unexpected{src.error()};
    }
    // 1. the index, each manifest and each config
    auto top = src->client.get_manifest(oci::reference::digest(loc->digest));
    if (!top) {
        return unexpected{top.error().message};
    }
    if (auto r = s.put_bytes(top->body, loc->digest); !r) {
        return unexpected{r.error()};
    }
    format::blob_reader reader = [&](const oci::descriptor& d)
        -> util::expected<std::string, std::string> {
        if (s.has_blob(d.digest)) {
            return s.read_blob(d.digest);
        }
        std::string bytes;
        if (d.media_type == oci::media_type_manifest ||
            d.media_type == oci::media_type_index) {
            auto m = src->client.get_manifest(oci::reference::digest(d.digest));
            if (!m) {
                return unexpected{m.error().message};
            }
            bytes = std::move(m->body);
        } else {
            auto part = s.part_path(d.digest);
            if (auto r = src->client.get_blob_to_file(d.digest, part); !r) {
                return unexpected{r.error().message};
            }
            auto b = util::read_file(part);
            std::error_code ec;
            fs::remove(part, ec);
            if (!b) {
                return unexpected{b.error()};
            }
            bytes = std::move(*b);
        }
        if (auto r = s.put_bytes(bytes, d.digest); !r) {
            return unexpected{r.error()};
        }
        return bytes;
    };
    // 2. the composed_image: stop before a download of some GB if the image
    // is not valid
    auto img = format::load_composed_image(loc->digest, top->body, reader, {});
    if (!img) {
        return unexpected{img.error()};
    }
    // the catalogue is a copy of the configs, not checked against them: check
    // the configs against the label before the download
    if (a.label) {
        auto q = parse_label_query(*a.label, ctx.keys, ctx.default_system);
        if (q && !q->id) {
            const auto& c = img->config;
            std::vector<std::string> bad;
            if (q->name && *q->name != c.name) {
                bad.push_back("name " + c.name);
            }
            if (q->version && *q->version != c.version) {
                bad.push_back("version " + c.version);
            }
            if (q->release && *q->release != c.release) {
                bad.push_back("release " + c.release);
            }
            for (auto& [k, v] : q->meta) {
                if (auto it = c.meta.find(k); it != c.meta.end() && it->second != v) {
                    bad.push_back(fmt::format("{} {}", k, it->second));
                }
            }
            if (!bad.empty()) {
                return unexpected{fmt::format(
                    "the catalogue gives {} for '{}', but its config has {}: "
                    "the catalogue is not correct",
                    loc->digest.string(), *a.label, fmt::join(bad, ", "))};
            }
        }
    }
    test_fault("pull-after-configs");
    // 3. each squashfs that is not in the store
    for (auto& l : img->layers) {
        auto& d = *l.layer.store.digest;
        if (s.has_blob(d)) {
            s.touch(d);
            ctx.st.skipped++;
            continue;
        }
        auto part = s.part_path(d);
        if (auto r = src->client.get_blob_to_file(d, part); !r) {
            return unexpected{r.error().message};
        }
        if (auto r = s.commit_part(part, d); !r) {
            return unexpected{r.error()};
        }
        ctx.st.downloaded++;
        ctx.st.downloaded_bytes += l.layer.store.size;
    }
    if (img->kind != uenv::image_kind::layer) {
        if (auto r = s.put_bytes(
                format::make_blob(std::string(oci::empty_config_body)));
            !r) {
            return unexpected{r.error()};
        }
    }
    test_fault("pull-before-db");
    // 4. the database
    return s.record(loc->digest, img->kind == uenv::image_kind::layer);
}

//
// copy
//

util::expected<push_result, std::string>
copy(const address& srca, const target& dstt, context& ctx,
     const std::map<std::string, std::string>& changes) {
    auto loc = locate(srca, ctx);
    if (!loc) {
        return unexpected{loc.error()};
    }
    auto src = open_repo(srca.where, loc->repository, ctx);
    if (!src) {
        return unexpected{src.error()};
    }
    auto name = loc->repository.substr(loc->repository.rfind('/') + 1);
    auto dst = open_repo(dstt, dstt.repo(name), ctx);
    if (!dst) {
        return unexpected{dst.error()};
    }
    auto top = src->client.get_manifest(oci::reference::digest(loc->digest));
    if (!top) {
        return unexpected{top.error().message};
    }
    auto kind = format::top_level_kind(top->body);
    if (!kind) {
        return unexpected{kind.error()};
    }
    if (*kind == format::object_kind::layer && !changes.empty()) {
        return unexpected{std::string{"a copy with a change applies only to "
                                      "an image"}};
    }
    // read each object from the source, and check it
    std::map<std::string, std::string> manifests;
    format::blob_reader reader = [&](const oci::descriptor& d)
        -> util::expected<std::string, std::string> {
        if (d.media_type == oci::media_type_manifest) {
            auto m = src->client.get_manifest(oci::reference::digest(d.digest));
            if (!m) {
                return unexpected{m.error().message};
            }
            manifests[d.digest.string()] = m->body;
            return m->body;
        }
        auto tmp = util::make_temp_dir();
        auto file = *tmp / d.digest.hex();
        if (auto r = src->client.get_blob_to_file(d.digest, file); !r) {
            return unexpected{r.error().message};
        }
        return util::read_file(file);
    };
    auto img = format::load_composed_image(loc->digest, top->body, reader, {});
    if (!img) {
        return unexpected{img.error()};
    }

    // the blobs of the layers
    std::set<std::string> done;
    for (auto& l : img->layers) {
        if (!done.insert(l.layer.digest->string()).second) {
            continue;
        }
        auto lm = format::parse_layer_manifest(manifests.at(l.layer.digest->string()));
        for (auto& [d, size] :
             {std::pair{*l.layer.store.digest, l.layer.store.size},
              std::pair{lm->config.digest, std::uint64_t(lm->config.size)}}) {
            if (auto r = ensure_blob(*dst, d, size, {.repo = &*src}, {}, ctx);
                !r) {
                return unexpected{r.error()};
            }
        }
        if (*kind == format::object_kind::image) {
            if (auto r = put_manifest(*dst, l.layer.digest->string(),
                                      manifests.at(l.layer.digest->string()),
                                      oci::media_type_manifest, ctx);
                !r) {
                return unexpected{r.error()};
            }
        }
    }

    std::string tag;
    oci::digest result = loc->digest;
    object_record rec = record_of(*img, 0);
    if (*kind == format::object_kind::layer) {
        tag = loc->tag.value_or(format::make_tag(img->config, loc->digest));
        if (auto x = check_tag(*dst, tag, loc->digest); !x) {
            return unexpected{x.error()};
        }
        if (auto r = put_manifest(*dst, tag, top->body, oci::media_type_manifest,
                                  ctx);
            !r) {
            return unexpected{r.error()};
        }
        rec.standalone = true;
    } else if (changes.empty()) {
        // a copy with no change: the same objects, the same digest
        auto idx = format::parse_index(top->body);
        auto wbody = manifests.at(idx->wrapper.digest.string());
        auto w = format::parse_wrapper_manifest(wbody);
        if (auto r = ensure_blob(*dst, w->config.digest, w->config.size,
                                 {.repo = &*src}, {}, ctx);
            !r) {
            return unexpected{r.error()};
        }
        auto empty = format::make_blob(std::string(oci::empty_config_body));
        (void)ensure_blob(*dst, empty.digest, 2, {.bytes = empty.bytes}, {}, ctx);
        if (auto r = put_manifest(*dst, idx->wrapper.digest.string(), wbody,
                                  oci::media_type_manifest, ctx);
            !r) {
            return unexpected{r.error()};
        }
        tag = loc->tag.value_or(format::make_tag(img->config, loc->digest));
        if (auto x = check_tag(*dst, tag, loc->digest); !x) {
            return unexpected{x.error()};
        }
        if (auto r = put_manifest(*dst, tag, top->body, oci::media_type_index,
                                  ctx);
            !r) {
            return unexpected{r.error()};
        }
    } else {
        // a copy with a change is a compose with one change
        auto changed = *img;
        for (auto& [k, v] : changes) {
            if (k == "release") {
                changed.config.release = v;
            } else if (k == "version") {
                changed.config.version = v;
            } else if (k == "name") {
                return unexpected{std::string{
                    "a copy cannot change the name: the name gives the "
                    "repository"}};
            } else if (k.starts_with("meta.")) {
                if (v.empty()) {
                    changed.config.meta.erase(k.substr(5));
                } else {
                    changed.config.meta[k.substr(5)] = v;
                }
            } else {
                return unexpected{fmt::format("a copy cannot change '{}'", k)};
            }
        }
        auto objs = format::make_image_objects(changed);
        if (!objs) {
            return unexpected{objs.error()};
        }
        auto empty = format::make_blob(std::string(oci::empty_config_body));
        for (auto* b : {&objs->image_config, &empty}) {
            if (auto r = ensure_blob(*dst, b->digest, b->size(),
                                     {.bytes = b->bytes}, {}, ctx);
                !r) {
                return unexpected{r.error()};
            }
        }
        if (auto r = put_manifest(*dst, objs->wrapper.digest.string(),
                                  objs->wrapper.bytes, oci::media_type_manifest,
                                  ctx);
            !r) {
            return unexpected{r.error()};
        }
        result = objs->index.digest;
        changed.digest = result;
        tag = format::make_tag(changed.config, result);
        if (auto x = check_tag(*dst, tag, result); !x) {
            return unexpected{x.error()};
        }
        if (auto r = put_manifest(*dst, tag, objs->index.bytes,
                                  oci::media_type_index, ctx);
            !r) {
            return unexpected{r.error()};
        }
        rec = record_of(changed, 0);
    }

    // the referrers
    const bool same_image = changes.empty();
    if (same_image) {
        // copy: each referrer of the image and of its layers
        if (auto r = copy_referrers(*src, *dst, loc->digest, "copy", ctx); !r) {
            return unexpected{r.error()};
        }
    }
    if (*kind == format::object_kind::image) {
        std::set<std::string> rdone;
        for (auto& l : img->layers) {
            if (!rdone.insert(l.layer.digest->string()).second) {
                continue;
            }
            auto r = copy_referrers(*src, *dst, *l.layer.digest,
                                    same_image ? "copy" : "compose", ctx);
            if (!r) {
                return unexpected{r.error()};
            }
        }
    }
    append_event(dstt, "add", dst->repository, tag, rec, ctx);
    return push_result{dst->repository, tag, result};
}

//
// delete
//

util::expected<push_result, std::string> remove(const address& a,
                                                context& ctx) {
    auto loc = locate(a, ctx);
    if (!loc) {
        return unexpected{loc.error()};
    }
    auto c = open_repo(a.where, loc->repository, ctx);
    if (!c) {
        return unexpected{c.error()};
    }
    // the record, for the event
    std::optional<object_record> rec;
    if (auto cat = catalogue::open(a.where, ctx)) {
        if (auto r = cat->db().get(loc->digest); r && *r) {
            rec = **r;
        }
    }
    if (auto r = c->client.delete_manifest(oci::reference::digest(loc->digest)); !r) {
        return unexpected{fmt::format("unable to delete {}: {}",
                                      loc->digest.string(), r.error().message)};
    }
    if (rec) {
        append_event(a.where, "delete", loc->repository, loc->tag.value_or(""),
                     *rec, ctx);
    }
    return push_result{loc->repository, loc->tag.value_or(""), loc->digest};
}

//
// referrers
//

util::expected<std::vector<oci::descriptor>, std::string>
list_referrers(const target& t, const std::string& repository,
               const oci::digest& subject, context& ctx) {
    auto c = make_client(t, repository, ctx);
    if (!c) {
        return unexpected{c.error()};
    }
    auto r = c->referrers(subject);
    if (!r) {
        return unexpected{r.error().message};
    }
    return *r;
}

util::expected<std::vector<std::string>, std::string>
attach(const address& subject, const std::string& kind,
       const std::string& content, context& ctx) {
    const referrer_rule* rule = nullptr;
    if (ctx.cfg) {
        for (auto& r : ctx.cfg->referrer_rules) {
            if (r.name == kind) {
                rule = &r;
            }
        }
    }
    if (!rule) {
        return unexpected{fmt::format(
            "there is no rule for the kind of referrer '{}': uenv attaches "
            "only a kind that the site configures",
            kind)};
    }
    auto loc = locate(subject, ctx);
    if (!loc) {
        return unexpected{loc.error()};
    }
    auto c = open_repo(subject.where, loc->repository, ctx);
    if (!c) {
        return unexpected{c.error()};
    }
    auto top = c->client.get_manifest(oci::reference::digest(loc->digest));
    if (!top) {
        return unexpected{top.error().message};
    }
    auto k = format::top_level_kind(top->body);
    if (!k) {
        return unexpected{k.error()};
    }
    const std::string subject_kind =
        *k == format::object_kind::image ? "image" : "layer";
    if (std::find(rule->subjects.begin(), rule->subjects.end(),
                  subject_kind) == rule->subjects.end()) {
        return unexpected{fmt::format("a referrer of the kind '{}' cannot be "
                                      "attached to a {}",
                                      kind, subject_kind)};
    }
    std::string bytes = content;
    std::error_code ec;
    if (fs::is_regular_file(content, ec)) {
        auto b = util::read_file(content);
        if (!b) {
            return unexpected{b.error()};
        }
        bytes = *b;
    }
    auto blob = format::make_blob(bytes);
    auto empty = format::make_blob(std::string(oci::empty_config_body));
    json m{{"schemaVersion", 2},
           {"mediaType", oci::media_type_manifest},
           {"artifactType", rule->artifact_type},
           {"subject",
            {{"mediaType", *k == format::object_kind::image
                               ? oci::media_type_index
                               : oci::media_type_manifest},
             {"digest", loc->digest.string()},
             {"size", top->body.size()}}},
           {"config",
            {{"mediaType", oci::media_type_empty},
             {"digest", empty.digest.string()},
             {"size", 2},
             {"data", oci::empty_config_data}}},
           {"layers",
            json::array({{{"mediaType", rule->artifact_type},
                          {"digest", blob.digest.string()},
                          {"size", blob.size()}}})}};
    auto body = format::canonical(m);
    auto ref = format::make_blob(*body);

    std::vector<std::string> repos{loc->repository};
    if (rule->fan_out && *k == format::object_kind::layer) {
        auto cat = catalogue::open(subject.where, ctx);
        if (cat) {
            (void)cat->refresh();
            if (auto rs = cat->repositories_with_layer(loc->digest)) {
                for (auto& r : *rs) {
                    if (std::find(repos.begin(), repos.end(), r) == repos.end()) {
                        repos.push_back(r);
                    }
                }
            }
        }
    }
    for (auto& repo : repos) {
        auto dst = open_repo(subject.where, repo, ctx);
        if (!dst) {
            return unexpected{dst.error()};
        }
        for (auto* b : {&empty, &blob}) {
            if (auto r = ensure_blob(*dst, b->digest, b->size(),
                                     {.bytes = b->bytes}, {}, ctx);
                !r) {
                return unexpected{r.error()};
            }
        }
        if (auto r = put_manifest(*dst, ref.digest.string(), ref.bytes,
                                  oci::media_type_manifest, ctx);
            !r) {
            return unexpected{r.error()};
        }
    }
    return repos;
}

} // namespace uenv::registry

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <oci/manifest.h>
#include <uenv/format.h>
#include <uenv/store.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/sha.h>
#include <util/sqlite.h>

namespace uenv {

namespace fs = std::filesystem;
using util::unexpected;
using util::sqlite::value;

void test_fault(std::string_view point) {
    // test only: the one place where uenv reads the environment directly,
    // so that a fault can be injected into any process, also a child.
    if (const char* f = std::getenv("UENV_TEST_FAULT")) {
        if (std::string_view{f} == point) {
            std::fprintf(stderr, "UENV_TEST_FAULT: stopping at %s\n", f);
            std::fflush(stderr);
            _exit(99);
        }
    }
}

std::string now_utc() {
    auto t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

//
// object_db
//

namespace {

constexpr const char* schema = R"(
CREATE TABLE IF NOT EXISTS images (
    digest TEXT PRIMARY KEY, id TEXT NOT NULL, added TEXT NOT NULL,
    size INTEGER NOT NULL, name TEXT NOT NULL, version TEXT NOT NULL,
    release TEXT NOT NULL, description TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS image_meta (
    image TEXT NOT NULL REFERENCES images(digest) ON DELETE CASCADE,
    key TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY (image, key));
CREATE TABLE IF NOT EXISTS layers (
    digest TEXT PRIMARY KEY, id TEXT NOT NULL, squashfs TEXT NOT NULL,
    squashfs_size INTEGER NOT NULL, name TEXT NOT NULL, version TEXT NOT NULL,
    release TEXT NOT NULL, description TEXT NOT NULL, mount TEXT NOT NULL,
    relocatable INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS layer_meta (
    layer TEXT NOT NULL REFERENCES layers(digest) ON DELETE CASCADE,
    key TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY (layer, key));
CREATE TABLE IF NOT EXISTS image_layers (
    image TEXT NOT NULL REFERENCES images(digest) ON DELETE CASCADE,
    alias TEXT NOT NULL, mount TEXT NOT NULL,
    layer TEXT NOT NULL REFERENCES layers(digest),
    PRIMARY KEY (image, alias));
CREATE TABLE IF NOT EXISTS standalone_layers (
    layer TEXT PRIMARY KEY REFERENCES layers(digest), added TEXT NOT NULL,
    size INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS object_blobs (
    object TEXT NOT NULL, blob TEXT NOT NULL, PRIMARY KEY (object, blob));
CREATE INDEX IF NOT EXISTS object_blobs_blob ON object_blobs(blob);
CREATE TABLE IF NOT EXISTS locations (
    object TEXT NOT NULL, repository TEXT NOT NULL, tag TEXT NOT NULL,
    PRIMARY KEY (object, repository, tag));
)";

oci::digest to_digest(const std::string& s) {
    return *oci::digest::parse(s);
}

} // namespace

util::expected<object_db, std::string>
object_db::open(const fs::path& path, util::sqlite::open_mode mode) {
    auto db = util::sqlite::database::open(path, mode);
    if (!db) {
        return unexpected{db.error()};
    }
    object_db result{std::move(*db)};
    auto version = result.db_.user_version();
    if (!version) {
        return unexpected{version.error()};
    }
    if (*version == 0 && mode != util::sqlite::open_mode::readonly) {
        if (auto r = result.init_schema(); !r) {
            return unexpected{r.error()};
        }
    } else if (*version != store_layout_version) {
        return unexpected{fmt::format(
            "the database {} has the layout version {}, and this uenv reads "
            "only version {}: use a different version of uenv",
            path.string(), *version, store_layout_version)};
    }
    return result;
}

util::expected<object_db, std::string> object_db::memory() {
    auto db = util::sqlite::database::open_memory();
    if (!db) {
        return unexpected{db.error()};
    }
    object_db result{std::move(*db)};
    if (auto r = result.init_schema(); !r) {
        return unexpected{r.error()};
    }
    return result;
}

util::expected<void, std::string> object_db::init_schema() {
    if (auto r = db_.exec(schema); !r) {
        return r;
    }
    return db_.set_user_version(store_layout_version);
}

namespace {

util::expected<void, std::string> insert_layer(util::sqlite::database& db,
                                               const oci::digest& d,
                                               const config& cfg,
                                               const oci::digest& sqfs,
                                               std::uint64_t sqfs_size,
                                               const std::string& mount,
                                               bool relocatable) {
    auto r = db.query(
        "INSERT OR IGNORE INTO layers (digest, id, squashfs, squashfs_size, "
        "name, version, release, description, mount, relocatable) VALUES "
        "(?,?,?,?,?,?,?,?,?,?)",
        {d.string(), format::short_id(d), sqfs.string(),
         std::int64_t(sqfs_size), cfg.name, cfg.version, cfg.release,
         cfg.description, mount, std::int64_t(relocatable)});
    if (!r) {
        return unexpected{r.error()};
    }
    for (auto& [k, v] : cfg.meta) {
        auto m = db.query(
            "INSERT OR IGNORE INTO layer_meta (layer, key, value) VALUES (?,?,?)",
            {d.string(), k, v});
        if (!m) {
            return unexpected{m.error()};
        }
    }
    return {};
}

} // namespace

util::expected<void, std::string>
object_db::add(const object_record& r, const std::vector<oci::digest>& blobs) {
    return db_.transaction([&]() -> util::expected<void, std::string> {
        const auto d = r.digest.string();
        if (r.kind == format::object_kind::image) {
            auto q = db_.query(
                "INSERT OR IGNORE INTO images (digest, id, added, size, name, "
                "version, release, description) VALUES (?,?,?,?,?,?,?,?)",
                {d, r.id, r.added, std::int64_t(r.size), r.cfg.name,
                 r.cfg.version, r.cfg.release, r.cfg.description});
            if (!q) {
                return unexpected{q.error()};
            }
            for (auto& [k, v] : r.cfg.meta) {
                auto m = db_.query("INSERT OR IGNORE INTO image_meta (image, "
                                   "key, value) VALUES (?,?,?)",
                                   {d, k, v});
                if (!m) {
                    return unexpected{m.error()};
                }
            }
            for (auto& l : r.layers) {
                if (auto x = insert_layer(db_, l.digest, l.cfg, l.squashfs,
                                          l.squashfs_size, l.build_mount,
                                          l.relocatable);
                    !x) {
                    return x;
                }
                auto q2 = db_.query("INSERT OR IGNORE INTO image_layers "
                                    "(image, alias, mount, layer) VALUES "
                                    "(?,?,?,?)",
                                    {d, l.alias, l.mount, l.digest.string()});
                if (!q2) {
                    return unexpected{q2.error()};
                }
            }
        } else {
            if (!r.squashfs) {
                return unexpected{std::string{"a layer record has no squashfs"}};
            }
            if (auto x = insert_layer(db_, r.digest, r.cfg, *r.squashfs,
                                      r.layers.empty() ? 0
                                                       : r.layers[0].squashfs_size,
                                      r.mount, r.relocatable);
                !x) {
                return x;
            }
            if (r.standalone) {
                auto q = db_.query("INSERT OR IGNORE INTO standalone_layers "
                                   "(layer, added, size) VALUES (?,?,?)",
                                   {d, r.added, std::int64_t(r.size)});
                if (!q) {
                    return unexpected{q.error()};
                }
            }
        }
        for (auto& b : blobs) {
            auto q = db_.query("INSERT OR IGNORE INTO object_blobs (object, "
                               "blob) VALUES (?,?)",
                               {d, b.string()});
            if (!q) {
                return unexpected{q.error()};
            }
        }
        if (r.repository && r.tag) {
            auto q = db_.query("INSERT OR IGNORE INTO locations (object, "
                               "repository, tag) VALUES (?,?,?)",
                               {d, *r.repository, *r.tag});
            if (!q) {
                return unexpected{q.error()};
            }
        }
        return {};
    });
}

util::expected<std::vector<oci::digest>, std::string>
object_db::blobs_of(const oci::digest& object) {
    auto rows = db_.query("SELECT blob FROM object_blobs WHERE object = ?",
                          {object.string()});
    if (!rows) {
        return unexpected{rows.error()};
    }
    std::vector<oci::digest> out;
    for (auto& r : *rows) {
        out.push_back(to_digest(r.text("blob")));
    }
    return out;
}

util::expected<std::vector<oci::digest>, std::string> object_db::all_blobs() {
    auto rows = db_.query("SELECT DISTINCT blob FROM object_blobs");
    if (!rows) {
        return unexpected{rows.error()};
    }
    std::vector<oci::digest> out;
    for (auto& r : *rows) {
        out.push_back(to_digest(r.text("blob")));
    }
    return out;
}

util::expected<std::vector<oci::digest>, std::string>
object_db::remove(const oci::digest& d) {
    std::vector<oci::digest> released;
    auto result = db_.transaction([&]() -> util::expected<void, std::string> {
        const auto ds = d.string();
        auto blobs = blobs_of(d);
        if (!blobs) {
            return unexpected{blobs.error()};
        }
        for (auto sql : {"DELETE FROM images WHERE digest = ?",
                         "DELETE FROM standalone_layers WHERE layer = ?",
                         "DELETE FROM object_blobs WHERE object = ?",
                         "DELETE FROM locations WHERE object = ?"}) {
            if (auto q = db_.query(sql, {ds}); !q) {
                return unexpected{q.error()};
            }
        }
        // the layers that no image and no standalone record uses
        if (auto q = db_.exec(
                "DELETE FROM layers WHERE digest NOT IN (SELECT layer FROM "
                "image_layers) AND digest NOT IN (SELECT layer FROM "
                "standalone_layers)");
            !q) {
            return q;
        }
        for (auto& b : *blobs) {
            auto used = db_.query(
                "SELECT 1 FROM object_blobs WHERE blob = ? LIMIT 1",
                {b.string()});
            if (!used) {
                return unexpected{used.error()};
            }
            if (used->empty()) {
                released.push_back(b);
            }
        }
        return {};
    });
    if (!result) {
        return unexpected{result.error()};
    }
    return released;
}

namespace {

util::expected<std::map<std::string, std::string>, std::string>
read_meta(util::sqlite::database& db, const char* table, const char* col,
          const std::string& d) {
    auto rows = db.query(
        fmt::format("SELECT key, value FROM {} WHERE {} = ?", table, col), {d});
    if (!rows) {
        return unexpected{rows.error()};
    }
    std::map<std::string, std::string> m;
    for (auto& r : *rows) {
        m[r.text("key")] = r.text("value");
    }
    return m;
}

config config_of_row(const util::sqlite::row& r) {
    return config{.name = r.text("name"),
                  .version = r.text("version"),
                  .release = r.text("release"),
                  .description = r.text("description")};
}

} // namespace

util::expected<std::optional<object_record>, std::string>
object_db::get(const oci::digest& d) {
    const auto ds = d.string();
    auto img = db_.query("SELECT * FROM images WHERE digest = ?", {ds});
    if (!img) {
        return unexpected{img.error()};
    }
    std::optional<object_record> out;
    if (!img->empty()) {
        auto& row = img->front();
        object_record r{.kind = format::object_kind::image, .digest = d};
        r.id = row.text("id");
        r.cfg = config_of_row(row);
        r.added = row.text("added");
        r.size = row.integer("size");
        auto meta = read_meta(db_, "image_meta", "image", ds);
        if (!meta) {
            return unexpected{meta.error()};
        }
        r.cfg.meta = *meta;
        auto layers = db_.query(
            "SELECT il.alias AS alias, il.mount AS imount, l.* FROM "
            "image_layers il JOIN layers l ON il.layer = l.digest WHERE "
            "il.image = ? ORDER BY il.mount",
            {ds});
        if (!layers) {
            return unexpected{layers.error()};
        }
        for (auto& lr : *layers) {
            layer_ref ref{.alias = lr.text("alias"),
                          .mount = lr.text("imount"),
                          .digest = to_digest(lr.text("digest")),
                          .cfg = config_of_row(lr),
                          .build_mount = lr.text("mount"),
                          .relocatable = lr.integer("relocatable") != 0,
                          .squashfs = to_digest(lr.text("squashfs")),
                          .squashfs_size =
                              std::uint64_t(lr.integer("squashfs_size"))};
            auto lm = read_meta(db_, "layer_meta", "layer", lr.text("digest"));
            if (!lm) {
                return unexpected{lm.error()};
            }
            ref.cfg.meta = *lm;
            r.layers.push_back(std::move(ref));
        }
        out = std::move(r);
    } else {
        auto lay = db_.query(
            "SELECT l.*, s.added AS sadded, s.size AS ssize FROM layers l LEFT "
            "JOIN standalone_layers s ON s.layer = l.digest WHERE l.digest = ?",
            {ds});
        if (!lay) {
            return unexpected{lay.error()};
        }
        if (lay->empty()) {
            return std::optional<object_record>{};
        }
        auto& row = lay->front();
        object_record r{.kind = format::object_kind::layer, .digest = d};
        r.id = row.text("id");
        r.cfg = config_of_row(row);
        auto lm = read_meta(db_, "layer_meta", "layer", ds);
        if (!lm) {
            return unexpected{lm.error()};
        }
        r.cfg.meta = *lm;
        r.standalone = row.opt_text("sadded").has_value();
        r.added = row.opt_text("sadded").value_or("");
        r.mount = row.text("mount");
        r.relocatable = row.integer("relocatable") != 0;
        r.squashfs = to_digest(row.text("squashfs"));
        r.size = r.standalone ? row.integer("ssize")
                              : row.integer("squashfs_size");
        r.layers.push_back(layer_ref{.alias = r.cfg.name,
                                     .mount = r.mount,
                                     .digest = d,
                                     .cfg = r.cfg,
                                     .build_mount = r.mount,
                                     .relocatable = r.relocatable,
                                     .squashfs = *r.squashfs,
                                     .squashfs_size = std::uint64_t(
                                         row.integer("squashfs_size"))});
        out = std::move(r);
    }
    auto loc = db_.query(
        "SELECT repository, tag FROM locations WHERE object = ? LIMIT 1", {ds});
    if (!loc) {
        return unexpected{loc.error()};
    }
    if (!loc->empty()) {
        out->repository = loc->front().text("repository");
        out->tag = loc->front().text("tag");
    }
    return out;
}

util::expected<std::vector<object_record>, std::string>
object_db::query(const label_query& q) {
    const bool layer = q.layer;
    const std::string table = layer ? "layers" : "images";
    const std::string meta_table = layer ? "layer_meta" : "image_meta";
    const std::string meta_col = layer ? "layer" : "image";
    std::string sql = fmt::format("SELECT digest FROM {} o WHERE 1", table);
    std::vector<value> params;
    if (q.name) {
        sql += " AND o.name = ?";
        params.push_back(*q.name);
    }
    if (q.version) {
        sql += " AND o.version = ?";
        params.push_back(*q.version);
    }
    if (q.release) {
        sql += " AND o.release = ?";
        params.push_back(*q.release);
    }
    for (auto& [k, v] : q.meta) {
        sql += fmt::format(
            " AND (NOT EXISTS (SELECT 1 FROM {0} m WHERE m.{1} = o.digest AND "
            "m.key = ?) OR EXISTS (SELECT 1 FROM {0} m WHERE m.{1} = o.digest "
            "AND m.key = ? AND m.value = ?))",
            meta_table, meta_col);
        params.push_back(k);
        params.push_back(k);
        params.push_back(v);
    }
    if (q.id) {
        auto id = *q.id;
        if (id.starts_with("sha256:")) {
            id = id.substr(7);
        }
        sql += " AND (o.id = ? OR o.digest LIKE ?)";
        params.push_back(id);
        params.push_back("sha256:" + id + "%");
    }
    sql += " ORDER BY o.name, o.version, o.release, o.digest";
    auto rows = db_.query(sql, params);
    if (!rows) {
        return unexpected{rows.error()};
    }
    std::vector<object_record> out;
    for (auto& r : *rows) {
        auto rec = get(to_digest(r.text("digest")));
        if (!rec) {
            return unexpected{rec.error()};
        }
        if (*rec) {
            out.push_back(std::move(**rec));
        }
    }
    return out;
}

util::expected<std::vector<object_record>, std::string> object_db::all() {
    auto images = query(label_query{});
    if (!images) {
        return images;
    }
    auto rows = db_.query("SELECT layer FROM standalone_layers ORDER BY layer");
    if (!rows) {
        return unexpected{rows.error()};
    }
    for (auto& r : *rows) {
        auto rec = get(to_digest(r.text("layer")));
        if (!rec) {
            return unexpected{rec.error()};
        }
        if (*rec) {
            images->push_back(std::move(**rec));
        }
    }
    return images;
}

util::expected<std::vector<object_record>, std::string>
object_db::images_with_layer(const oci::digest& layer) {
    auto rows = db_.query(
        "SELECT DISTINCT image FROM image_layers WHERE layer = ?",
        {layer.string()});
    if (!rows) {
        return unexpected{rows.error()};
    }
    std::vector<object_record> out;
    for (auto& r : *rows) {
        auto rec = get(to_digest(r.text("image")));
        if (!rec) {
            return unexpected{rec.error()};
        }
        if (*rec) {
            out.push_back(std::move(**rec));
        }
    }
    return out;
}

//
// store
//

repo_layout inspect_repo(const fs::path& root) {
    std::error_code ec;
    if (fs::exists(root / "v2" / "index.db", ec)) {
        return repo_layout::v2;
    }
    if (fs::exists(root / "index.db", ec)) {
        return repo_layout::v1_only;
    }
    if (!fs::exists(root, ec)) {
        return repo_layout::none;
    }
    if (fs::is_directory(root, ec)) {
        return repo_layout::none;
    }
    return repo_layout::invalid;
}

util::expected<store, std::string> store::open(const fs::path& root,
                                               bool readonly) {
    switch (inspect_repo(root)) {
    case repo_layout::v1_only:
        return unexpected{fmt::format(
            "the repository {} is in the v1 format. Run `uenv repo migrate {}` "
            "to use it with this version of uenv.",
            root.string(), root.string())};
    case repo_layout::none:
        return unexpected{
            fmt::format("no repository at {}", root.string())};
    case repo_layout::invalid:
        return unexpected{
            fmt::format("{} is not a repository", root.string())};
    case repo_layout::v2:
        break;
    }
    auto db = object_db::open(root / "v2" / "index.db",
                              readonly ? util::sqlite::open_mode::readonly
                                       : util::sqlite::open_mode::readwrite);
    if (!db) {
        return unexpected{db.error()};
    }
    bool ro = readonly || db->db().readonly();
    return store{root, std::move(*db), ro};
}

util::expected<store, std::string> store::create(const fs::path& root) {
    auto state = inspect_repo(root);
    if (state == repo_layout::v2) {
        return open(root, false);
    }
    if (state == repo_layout::v1_only) {
        return unexpected{fmt::format(
            "the repository {} is in the v1 format: run `uenv repo migrate`",
            root.string())};
    }
    if (state == repo_layout::invalid) {
        return unexpected{
            fmt::format("{} is not a directory", root.string())};
    }
    std::error_code ec;
    const auto v2 = root / "v2";
    fs::create_directories(v2 / "blobs" / "sha256", ec);
    fs::create_directories(v2 / "tmp", ec);
    if (ec) {
        return unexpected{fmt::format("unable to create {}: {}", v2.string(),
                                      ec.message())};
    }
    if (auto r = util::write_file_atomic(v2 / "oci-layout",
                                         R"({"imageLayoutVersion":"1.0.0"})");
        !r) {
        return unexpected{r.error()};
    }
    auto db = object_db::open(v2 / "index.db", util::sqlite::open_mode::create);
    if (!db) {
        return unexpected{db.error()};
    }
    store s{root, std::move(*db), false};
    if (auto r = s.write_index_json(); !r) {
        return unexpected{r.error()};
    }
    return s;
}

fs::path store::blob_path(const oci::digest& d) const {
    return v2() / "blobs" / d.algorithm() / d.hex();
}

bool store::has_blob(const oci::digest& d) const {
    std::error_code ec;
    return fs::is_regular_file(blob_path(d), ec);
}

namespace {

std::string process_hash() {
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    return util::sha256_string(fmt::format("{}.{}", getpid(), host))
        .string()
        .substr(0, 8);
}

} // namespace

fs::path store::part_path(const oci::digest& d) const {
    return v2() / "tmp" / fmt::format("{}.{}.part", d.hex(), process_hash());
}

util::expected<void, std::string> store::commit_part(const fs::path& part,
                                                     const oci::digest& d) {
    std::error_code ec;
    fs::rename(part, blob_path(d), ec);
    if (ec) {
        return unexpected{fmt::format("unable to move {} into the store: {}",
                                      part.string(), ec.message())};
    }
    return {};
}

util::expected<void, std::string> store::put_bytes(const std::string& bytes,
                                                   const oci::digest& d) {
    if (oci::digest::sha256(util::sha256_string(bytes)) != d) {
        return unexpected{
            fmt::format("the content does not have the digest {}", d.string())};
    }
    if (has_blob(d)) {
        touch(d);
        return {};
    }
    auto part = part_path(d);
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f) {
            return unexpected{
                fmt::format("unable to write {}", part.string())};
        }
    }
    return commit_part(part, d);
}

util::expected<void, std::string> store::put_bytes(const format::blob& b) {
    return put_bytes(b.bytes, b.digest);
}

util::expected<oci::digest, std::string>
store::put_file(const fs::path& src,
                std::function<void(std::uint64_t, std::uint64_t)> progress) {
    std::error_code ec;
    const auto total = fs::file_size(src, ec);
    if (ec) {
        return unexpected{
            fmt::format("unable to read {}: {}", src.string(), ec.message())};
    }
    auto part = v2() / "tmp" / fmt::format("import.{}.part", process_hash());
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        return unexpected{fmt::format("unable to copy {}", src.string())};
    }
    util::sha256_hasher hasher;
    std::vector<char> buf(4 << 20);
    std::uint64_t done = 0;
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        auto n = in.gcount();
        if (n <= 0) {
            break;
        }
        hasher.update(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(buf.data()), std::size_t(n)));
        out.write(buf.data(), n);
        done += std::uint64_t(n);
        if (progress) {
            progress(done, total);
        }
    }
    out.close();
    if (!out) {
        fs::remove(part, ec);
        return unexpected{fmt::format("unable to write {}", part.string())};
    }
    auto d = oci::digest::sha256(hasher.finalize());
    if (has_blob(d)) {
        fs::remove(part, ec);
        touch(d);
        return d;
    }
    if (auto r = commit_part(part, d); !r) {
        return unexpected{r.error()};
    }
    return d;
}

util::expected<void, std::string> store::link_file(const fs::path& src,
                                                   const oci::digest& d) {
    if (has_blob(d)) {
        return {};
    }
    std::error_code ec;
    fs::create_hard_link(src, blob_path(d), ec);
    if (ec) {
        return unexpected{fmt::format("unable to link {} into the store: {}",
                                      src.string(), ec.message())};
    }
    return {};
}

void store::touch(const oci::digest& d) const {
    std::error_code ec;
    fs::last_write_time(blob_path(d), fs::file_time_type::clock::now(), ec);
}

util::expected<std::string, std::string>
store::read_blob(const oci::digest& d) const {
    auto bytes = util::read_file(blob_path(d));
    if (!bytes) {
        return unexpected{fmt::format("the blob {} is not in the store",
                                      d.string())};
    }
    if (oci::digest::sha256(util::sha256_string(*bytes)) != d) {
        return unexpected{fmt::format(
            "the blob {} in the store does not match its digest", d.string())};
    }
    return *bytes;
}

format::blob_reader store::reader() const {
    return [this](const oci::descriptor& d)
               -> util::expected<std::string, std::string> {
        if (d.data) {
            // the descriptor has the content: no read
            spdlog::trace("store: inline data for {}", d.digest.string());
        }
        return read_blob(d.digest);
    };
}

format::file_locator store::locator() const {
    return [this](const oci::digest& d) -> std::optional<fs::path> {
        if (has_blob(d)) {
            return blob_path(d);
        }
        return std::nullopt;
    };
}

util::expected<composed_image, std::string>
store::load(const oci::digest& d) const {
    auto body = read_blob(d);
    if (!body) {
        return unexpected{body.error()};
    }
    return format::load_composed_image(d, *body, reader(), locator());
}

object_record record_of(const composed_image& img, std::uint64_t size) {
    object_record r{.kind = img.is_layer ? format::object_kind::layer
                                         : format::object_kind::image,
                    .digest = *img.digest};
    r.id = format::short_id(*img.digest);
    r.cfg = img.config;
    r.added = now_utc();
    r.size = size;
    for (auto& l : img.layers) {
        r.layers.push_back(layer_ref{.alias = l.alias,
                                     .mount = l.mount,
                                     .digest = *l.layer.digest,
                                     .cfg = l.layer.config,
                                     .build_mount = l.layer.mount,
                                     .relocatable = l.layer.relocatable,
                                     .squashfs = *l.layer.store.digest,
                                     .squashfs_size = l.layer.store.size});
    }
    if (img.is_layer) {
        auto& l = img.layers.front().layer;
        r.mount = l.mount;
        r.relocatable = l.relocatable;
        r.squashfs = l.store.digest;
    }
    return r;
}

util::expected<object_record, std::string>
store::record(const oci::digest& top, bool standalone_layer) {
    if (readonly_) {
        return unexpected{
            fmt::format("the repository {} is read only", root_.string())};
    }
    auto img = load(top);
    if (!img) {
        return unexpected{img.error()};
    }
    // the blobs that the object reaches
    std::vector<oci::digest> blobs{top};
    if (!img->is_layer) {
        auto idx = format::parse_index(*read_blob(top));
        blobs.push_back(idx->wrapper.digest);
        auto w = format::parse_wrapper_manifest(*read_blob(idx->wrapper.digest));
        blobs.push_back(w->config.digest);
        blobs.push_back(*oci::digest::parse(
            fmt::format("sha256:{}", oci::empty_config_hex)));
    }
    for (auto& l : img->layers) {
        auto m = format::parse_layer_manifest(*read_blob(*l.layer.digest));
        if (!m) {
            return unexpected{m.error()};
        }
        blobs.push_back(*l.layer.digest);
        blobs.push_back(m->config.digest);
        blobs.push_back(m->squashfs.digest);
    }
    std::uint64_t size = 0;
    std::set<std::string> seen;
    std::vector<oci::digest> unique;
    for (auto& b : blobs) {
        if (!seen.insert(b.string()).second) {
            continue;
        }
        if (!has_blob(b)) {
            return unexpected{fmt::format(
                "the blob {} of {} is not in the store", b.string(),
                top.string())};
        }
        std::error_code ec;
        size += fs::file_size(blob_path(b), ec);
        unique.push_back(b);
    }
    auto r = record_of(*img, size);
    r.standalone = standalone_layer && img->is_layer;
    test_fault("record-before-db");
    if (auto x = db_.add(r, unique); !x) {
        return unexpected{x.error()};
    }
    return r;
}

util::expected<std::vector<oci::digest>, std::string>
store::remove(const oci::digest& d) {
    if (readonly_) {
        return unexpected{
            fmt::format("the repository {} is read only", root_.string())};
    }
    auto released = db_.remove(d);
    if (!released) {
        return released;
    }
    test_fault("remove-after-db");
    for (auto& b : *released) {
        std::error_code ec;
        fs::remove(blob_path(b), ec);
    }
    return released;
}

util::expected<update_report, std::string> store::update(bool force,
                                                         bool dry_run) {
    update_report report;
    using clock = fs::file_time_type::clock;
    const auto grace = std::chrono::hours(24);
    const auto now = clock::now();

    // 2. missing blobs
    auto all = db_.all();
    if (!all) {
        return unexpected{all.error()};
    }
    for (auto& r : *all) {
        auto blobs = db_.blobs_of(r.digest);
        if (!blobs) {
            return unexpected{blobs.error()};
        }
        bool missing = false;
        for (auto& b : *blobs) {
            if (!has_blob(b)) {
                missing = true;
            }
        }
        if (missing) {
            report.missing.push_back(r);
            if (!dry_run) {
                if (auto x = db_.remove(r.digest); !x) {
                    return unexpected{x.error()};
                }
            }
        }
    }

    // 3. prune
    auto keep_list = db_.all_blobs();
    if (!keep_list) {
        return unexpected{keep_list.error()};
    }
    std::set<std::string> keep;
    for (auto& b : *keep_list) {
        keep.insert(b.hex());
    }
    if (dry_run) {
        for (auto& r : report.missing) {
            // the blobs of a missing record would be released
            (void)r;
        }
    }
    std::error_code ec;
    auto old = [&](const fs::path& p) {
        auto t = fs::last_write_time(p, ec);
        return force || ec || (now - t) > grace;
    };
    for (auto& e : fs::directory_iterator(v2() / "blobs" / "sha256", ec)) {
        auto name = e.path().filename().string();
        if (keep.contains(name)) {
            continue;
        }
        if (old(e.path())) {
            report.released += fs::file_size(e.path(), ec);
            report.pruned.push_back(e.path());
            if (!dry_run) {
                fs::remove(e.path(), ec);
            }
        } else {
            report.kept_recent.push_back(e.path());
        }
    }
    for (auto& e : fs::directory_iterator(v2() / "tmp", ec)) {
        if (e.path().extension() != ".part") {
            continue;
        }
        if (old(e.path())) {
            report.parts.push_back(e.path());
            if (!dry_run) {
                fs::remove(e.path(), ec);
            }
        }
    }

    // 4. the OCI index
    if (!dry_run) {
        if (auto r = write_index_json(); !r) {
            return unexpected{r.error()};
        }
    }
    return report;
}

util::expected<void, std::string> store::write_index_json() {
    using json = nlohmann::json;
    json manifests = json::array();
    auto all = db_.all();
    if (!all) {
        return unexpected{all.error()};
    }
    for (auto& r : *all) {
        std::error_code ec;
        auto size = fs::file_size(blob_path(r.digest), ec);
        if (ec) {
            continue;
        }
        bool image = r.kind == format::object_kind::image;
        manifests.push_back(json{
            {"mediaType", image ? oci::media_type_index : oci::media_type_manifest},
            {"artifactType", image ? format::at_image : format::at_layer},
            {"digest", r.digest.string()},
            {"size", size},
            {"annotations",
             {{"org.opencontainers.image.ref.name",
               format::make_tag(r.cfg, r.digest)}}}});
    }
    json idx{{"schemaVersion", 2},
             {"mediaType", oci::media_type_index},
             {"manifests", manifests}};
    return util::write_file_atomic(v2() / "index.json", idx.dump());
}

} // namespace uenv

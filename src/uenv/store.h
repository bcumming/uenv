#pragma once

// The local repository (design/3-repository.md): a blob store, an OCI image
// layout, and a database of the images and layers that are ready to start.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <oci/digest.h>
#include <util/sha.h>
#include <uenv/format.h>
#include <uenv/model.h>
#include <util/expected.h>
#include <util/sqlite.h>

namespace uenv {

inline constexpr std::int64_t store_layout_version = 2;

// a test hook: if the environment variable UENV_TEST_FAULT names `point`, the
// process exits at once with status 99, as if it was killed at that step.
// Only a build with -Dtests=enabled has the hook: in other builds it does
// nothing, and uenv does not read UENV_TEST_FAULT.
void test_fault(std::string_view point);

struct layer_ref {
    uenv::name alias;
    // the image mount point
    mount_point mount;
    oci::digest digest;
    config cfg;
    // from the layer config and the layer manifest
    mount_point build_mount;
    bool relocatable = false;
    oci::digest squashfs;
    std::uint64_t squashfs_size = 0;
};

// one image or one layer, as recorded in a database
struct object_record {
    format::object_kind kind = format::object_kind::image;
    oci::digest digest;
    std::string id;
    config cfg;
    std::string added;
    std::uint64_t size = 0;
    // images: the layers. layers: the layer itself (one entry)
    std::vector<layer_ref> layers;
    // layers: true if the layer is on its own (a building block)
    bool standalone = false;
    // layers: the build mount point and relocatable
    std::optional<mount_point> mount;
    bool relocatable = false;
    std::optional<oci::digest> squashfs;
    // the find database of a registry: where the object is
    std::optional<std::string> repository;
    std::optional<std::string> tag;
};

// the identity of an object that a user gives: its digest, or its id (the
// first 16 hex characters of the digest)
using object_id = std::variant<oci::digest, util::uenv_id>;

// a query on the fields of images or layers. A label is a query.
struct label_query {
    bool layer = false;
    std::optional<uenv::name> name;
    std::optional<uenv::name> version;
    std::optional<uenv::name> release;
    // site keys, for example system and uarch. An object that does not have a
    // key matches each value of the key.
    std::map<uenv::name, std::string> meta;
    std::optional<object_id> id;
};

// The database of images and layers. The local repository uses it with a blob
// store, and a registry lookup uses it with no blob store.
class object_db {
  public:
    static util::expected<object_db, std::string>
    open(const std::filesystem::path& path, util::sqlite::open_mode mode);
    static util::expected<object_db, std::string> memory();

    util::expected<void, std::string>
    add(const object_record& r, const std::vector<oci::digest>& blobs);
    // delete a top-level object: returns the blobs that no other top-level
    // object refers to
    util::expected<std::vector<oci::digest>, std::string>
    remove(const oci::digest& d);
    util::expected<std::vector<object_record>, std::string>
    query(const label_query& q);
    util::expected<std::optional<object_record>, std::string>
    get(const oci::digest& d);
    util::expected<std::vector<object_record>, std::string> all();
    util::expected<std::vector<oci::digest>, std::string>
    blobs_of(const oci::digest& object);
    util::expected<std::vector<oci::digest>, std::string> all_blobs();
    // the images that contain a layer
    util::expected<std::vector<object_record>, std::string>
    images_with_layer(const oci::digest& layer);

    util::sqlite::database& db() {
        return db_;
    }

  private:
    object_db(util::sqlite::database db, std::filesystem::path path)
        : db_(std::move(db)), path_(std::move(path)) {
    }
    util::expected<void, std::string> init_schema();
    util::sqlite::database db_;
    // for errors: the file of the database
    std::filesystem::path path_;
};

struct update_report {
    std::vector<object_record> missing;              // deleted from the db
    std::vector<std::filesystem::path> pruned;       // blobs deleted
    std::vector<std::filesystem::path> kept_recent;  // unused, in grace period
    std::vector<std::filesystem::path> parts;        // .part files deleted
    std::uint64_t released = 0;
};

class store {
  public:
    // open an existing repository
    static util::expected<store, std::string>
    open(const std::filesystem::path& root, bool readonly = false);
    // create a repository (or open it if it exists)
    static util::expected<store, std::string>
    create(const std::filesystem::path& root);
    // create or open the layout in root/<dir>, for example "v2.tmp" during
    // a migration
    static util::expected<store, std::string>
    create_in(const std::filesystem::path& root, const std::string& dir);

    const std::filesystem::path& root() const {
        return root_;
    }
    std::filesystem::path v2() const {
        return root_ / dir_;
    }
    std::filesystem::path blob_path(const oci::digest& d) const;
    bool has_blob(const oci::digest& d) const;
    bool readonly() const {
        return readonly_;
    }

    // store bytes that uenv made or received
    util::expected<void, std::string> put_bytes(const format::blob& b);
    util::expected<void, std::string> put_bytes(const std::string& bytes,
                                                const oci::digest& d);
    // copy a file into the store: returns its digest
    util::expected<oci::digest, std::string>
    put_file(const std::filesystem::path& src,
             std::function<void(std::uint64_t, std::uint64_t)> progress = {});
    // make a hard link to a file in the store (no copy). The digest of the
    // file must be known, and is checked unless `trusted`.
    util::expected<void, std::string> link_file(const std::filesystem::path& src,
                                                const oci::digest& d);
    // a path for a download of d: tmp/<hex>.<hash>.part
    std::filesystem::path part_path(const oci::digest& d) const;
    // move a checked download into the store
    util::expected<void, std::string> commit_part(const std::filesystem::path& part,
                                                  const oci::digest& d);
    // set the modification time of a blob to now (pull uses it)
    void touch(const oci::digest& d) const;

    // read a small blob, and check it against its digest
    util::expected<std::string, std::string> read_blob(const oci::digest& d) const;

    format::blob_reader reader() const;
    format::file_locator locator() const;

    // load an image or a layer from the blob store
    util::expected<composed_image, std::string> load(const oci::digest& d) const;

    // record an image (index) or a layer on its own (layer manifest) in the
    // database. Each of its blobs must be in the store.
    util::expected<object_record, std::string>
    record(const oci::digest& top, bool standalone_layer = false);

    util::expected<std::vector<object_record>, std::string>
    query(const label_query& q) {
        return db_.query(q);
    }
    util::expected<std::vector<object_record>, std::string> all() {
        return db_.all();
    }
    // delete from the database, then delete the blobs
    util::expected<std::vector<oci::digest>, std::string>
    remove(const oci::digest& d);

    util::expected<update_report, std::string> update(bool force,
                                                      bool dry_run);
    util::expected<void, std::string> write_index_json();

    object_db& db() {
        return db_;
    }

  private:
    store(std::filesystem::path root, object_db db, bool ro,
          std::string dir = "v2")
        : root_(std::move(root)), db_(std::move(db)), readonly_(ro),
          dir_(std::move(dir)) {
    }
    std::filesystem::path root_;
    object_db db_;
    bool readonly_ = false;
    std::string dir_ = "v2";
};

// the state of a repository directory
enum class repo_layout { none, v1_only, v2, invalid };
repo_layout inspect_repo(const std::filesystem::path& root);

// make an object_record from a composed_image that was loaded from a store
util::expected<object_record, std::string>
record_of(const composed_image& img, std::uint64_t size);

std::string now_utc();

} // namespace uenv

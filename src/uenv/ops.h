#pragma once

// Local operations on images and layers: add (promote), compose, the parse
// and resolution of labels, and the runtime merge of the list of run/start.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <uenv/format.h>
#include <uenv/meta.h>
#include <uenv/model.h>
#include <uenv/store.h>
#include <uenv/uenv.h>
#include <util/expected.h>

namespace uenv {

// the keys of meta that the label syntax maps to (a setting of the site)
struct label_keys {
    uenv::name at = names::system();
    uenv::name percent = names::uarch();
};

// a label, or an id, as a query.
// default_system: the value of the '@' key when the label gives none.
// the query of a parsed label: the layer marker, and the label
util::expected<label_query, std::string>
query_of_label(bool layer, const uenv_label& label, const label_keys& keys,
               std::optional<std::string> default_system = std::nullopt);

util::expected<label_query, std::string>
parse_label_query(std::string text, const label_keys& keys,
                  std::optional<std::string> default_system = std::nullopt);

// one item of a list for start/run: [alias=][+]label[:mount] or path[:mount]
struct item_spec {
    std::string text;
    std::optional<uenv::name> alias;
    std::optional<label_query> query;
    std::optional<std::filesystem::path> file;
    std::optional<mount_point> mount;
};
util::expected<std::vector<item_spec>, std::string>
parse_item_list(const std::string& text, const label_keys& keys,
                std::optional<std::string> default_system);

namespace v1 {
// the label of a v1 image, from the v1 database
struct v1_label {
    std::string version;
    std::string release;
    std::optional<std::string> system;
    std::optional<std::string> uarch;
};
// make a layer config from the meta/env.json of a v1 image
util::expected<layer_config, std::string>
make_layer_config(const uenv::meta& m, const std::optional<v1_label>& label);
} // namespace v1

// a squashfs file that a user starts or inspects: in memory only
util::expected<composed_image, std::string>
raise_from_squashfs(const std::filesystem::path& sqfs,
                    std::optional<mount_point> mount);

// the layer config of a squashfs file, from its meta/env.json
util::expected<uenv::meta, std::string>
read_squashfs_meta(const std::filesystem::path& sqfs);

// add a squashfs file to a repository.
// as_layer: store a layer on its own; else promote the layer to an image,
// with image_meta added to the meta of the layer.
util::expected<object_record, std::string>
add_squashfs(store& s, const std::filesystem::path& sqfs,
             const layer_config& cfg,
             const std::map<uenv::name, std::string>& image_meta,
             bool as_layer,
             std::function<void(std::uint64_t, std::uint64_t)> progress = {});

// write a composed_image to a store as an image: the image config, the
// wrapper and the index. Each layer must be in the store.
util::expected<object_record, std::string> write_image(store& s,
                                                       const composed_image& img);

// a lookup in a list of repositories
struct found_object {
    std::size_t store_index;
    object_record record;
};
// find exactly one object. More than one is an error that lists them.
util::expected<found_object, std::string>
find_unique(std::vector<store>& stores, const label_query& q,
            const std::string& text);

// compose an image from a compose file (JSON): the image config, plus
// "layers": [{"alias", "mount", "from"}]. Each layer must be in the store.
util::expected<object_record, std::string>
compose(store& s, const nlohmann::json& spec, const label_keys& keys);

// check that each nested mount point is a directory in its parent layer, with
// unsquashfs (1-composition.md, "Nested mount points")
//
// This is the heavy check, for unprivileged code only: compose, and the
// runtime merge in prepare_session (uenv run, uenv start, and the local
// context of the Slurm plugin). It runs an external program, and it reads the
// contents of a file system image that the user names.
//
// NEVER call it from a privileged path: the setuid squashfs-mount helper, or
// the remote context of the Slurm plugin, which runs as root. As root, it is
// an oracle for the contents of files that the user cannot read. Those paths
// use parse_and_validate_mounts (mount.h), the light check that is safe in
// privileged code. Do not "consolidate" the two checks into one.
util::expected<void, std::string> check_nested_mounts(const composed_image& img);


// one item of the list, as `uenv status` shows it: what the user started
struct session_item {
    enum class kind_t { image, layer, file };
    struct layer_t {
        uenv::name alias;
        std::string label;
        mount_point mount;
    };
    // the alias of the item, else the name of its config
    uenv::name name;
    kind_t kind = kind_t::image;
    // name/version:release, or the path of a squashfs file
    std::string label;
    // the digest of the image or the layer. Absent for a squashfs file.
    std::optional<oci::digest> digest;
    // the layers of the item, at the mount points of the session
    std::vector<layer_t> layers;
    // the views of the item that the session loads
    std::vector<uenv::name> views;
};

struct resolved_items {
    // the item itself for one item, else a runtime merge
    composed_image image;
    // the items, in the order of the list
    std::vector<session_item> items;
};

// resolve the list of uenv run/start or srun --uenv, and make one
// composed_image
util::expected<resolved_items, std::string>
resolve_items(std::vector<store>& stores, const std::vector<item_spec>& items,
              bool force);

} // namespace uenv

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
    std::string at = "system";
    std::string percent = "uarch";
};

// a label, or an id, as a query.
// default_system: the value of the '@' key when the label gives none.
util::expected<label_query, std::string>
parse_label_query(std::string text, const label_keys& keys,
                  std::optional<std::string> default_system = std::nullopt);

// one item of a list for start/run: [alias=][+]label[:mount] or path[:mount]
struct item_spec {
    std::string text;
    std::optional<std::string> alias;
    std::optional<label_query> query;
    std::optional<std::filesystem::path> file;
    std::optional<std::string> mount;
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
format::layer_config make_layer_config(const uenv::meta& m,
                                       const std::optional<v1_label>& label);
} // namespace v1

// a squashfs file that a user starts or inspects: in memory only
util::expected<composed_image, std::string>
raise_from_squashfs(const std::filesystem::path& sqfs,
                    std::optional<std::string> mount);

// the layer config of a squashfs file, from its meta/env.json
util::expected<uenv::meta, std::string>
read_squashfs_meta(const std::filesystem::path& sqfs);

// add a squashfs file to a repository.
// as_layer: store a layer on its own; else promote the layer to an image,
// with image_meta added to the meta of the layer.
util::expected<object_record, std::string>
add_squashfs(store& s, const std::filesystem::path& sqfs,
             const format::layer_config& cfg,
             const std::map<std::string, std::string>& image_meta,
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
compose(store& s, const nlohmann::json& spec, const label_keys& keys,
        std::vector<std::string>* warnings = nullptr);

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

// the warnings of a composed image: an image view env that contains the
// build mount point of a layer that the image moves
std::vector<std::string> compose_warnings(const composed_image& img);

// resolve the list of uenv run/start or srun --uenv, and make one
// composed_image: the item itself for one item, else a runtime merge
util::expected<composed_image, std::string>
resolve_items(std::vector<store>& stores, const std::vector<item_spec>& items,
              bool force);

} // namespace uenv

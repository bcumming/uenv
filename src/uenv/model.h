#pragma once

// The model of layers and images (design/1-composition.md).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <oci/digest.h>
#include <oci/types.h>
#include <uenv/view.h>
#include <util/envvars.h>
#include <util/expected.h>

namespace uenv {

// the placeholder for the root of a relocatable layer in a layer view
inline constexpr std::string_view uenv_root_placeholder = "@uenv-root@";

// the shared fields of a layer config and an image config
struct config {
    std::string name;
    std::string version;
    std::string release;
    // empty means absent: the stored form omits it
    std::string description;
    std::map<std::string, std::string> meta;

    friend bool operator==(const config&, const config&) = default;
};

struct storage {
    enum class kind_t { squashfs, erofs };
    kind_t kind = kind_t::squashfs;
    std::uint64_t size = 0;
    // absent for a squashfs file that is started directly: it is not hashed
    std::optional<oci::digest> digest;
    // the local file, when the file system image is available locally
    std::optional<std::filesystem::path> file;
};

// a layer view: an environment patch that can contain @uenv-root@
struct layer_view {
    std::string description;
    envvars::patch env;
};

struct layer {
    // the descriptor of the layer manifest, as an index entry refers to it.
    // Its digest is the identity of the layer. Absent for a squashfs file.
    std::optional<oci::descriptor> manifest;
    // the file system image: its digest and size
    storage store;

    uenv::config config;
    std::string mount;
    bool relocatable = false;
    std::map<std::string, layer_view> views;
    std::optional<std::string> default_view;

    // the identity of the layer: the digest of its layer manifest
    std::optional<oci::digest> digest() const {
        if (!manifest) {
            return std::nullopt;
        }
        return manifest->digest;
    }
};

struct named_layer {
    std::string alias;
    std::string mount;
    struct layer layer;
};

struct image_view {
    std::string description;
    std::vector<layer_view_ref> include;
    envvars::patch env;
};

// what a composed_image was made from
enum class image_kind {
    // an image: read from a repository or a registry, or made by compose
    image,
    // the raise of one layer, or of a squashfs file
    layer,
    // the result of `uenv run/start a,b` (1-composition.md, "Runtime
    // merge"). It has no identity, and uenv never stores it.
    runtime_merge,
};

struct composed_image {
    // the digest of the top-level object: an index, or the layer manifest of
    // a raised layer. Absent for a squashfs file and for a runtime merge.
    std::optional<oci::digest> digest;
    image_kind kind = image_kind::image;

    // sorted by mount point
    std::vector<named_layer> layers;

    uenv::config config;
    std::map<image_view_name, image_view> views;
    // the views to load if the user names none.
    // A stored image has at most one. A runtime merge has the default view of
    // each item, in the order of the list.
    std::vector<image_view_name> default_views;
};

// the result of concretise: never stored, never an input to compose
struct concrete_image {
    // (file system image, mount point), sorted by mount point
    std::vector<std::pair<std::filesystem::path, std::string>> mounts;
    // applied in order, one at a time
    std::vector<envvars::patch> patches;
    // the views that the patches load, in order
    std::vector<view_description> views;
};

using model_error = std::string;

// Make a composed_image from its parts: sort the layers by mount point and
// apply the checks of "Checks" in 1-composition.md.
util::expected<composed_image, model_error>
make_composed_image(std::optional<oci::digest> digest, config cfg,
                    std::vector<named_layer> layers,
                    std::map<std::string, image_view> views,
                    std::optional<std::string> default_view);

// check a mount point: an absolute path with no variables
util::expected<void, model_error> validate_mount_point(const std::string& m);

// Raise a layer: a composed_image with one layer, and an image view for each
// layer view. `mount` is the mount point at which to use the layer.
util::expected<composed_image, model_error>
raise_layer(const layer& l, std::optional<std::string> mount, bool force,
            std::optional<std::string> alias = std::nullopt);

// one item of the list given to uenv run/start or srun --uenv:
// [alias=]label[:mount]
struct runtime_merge_item {
    std::optional<std::string> alias;
    std::optional<std::string> mount;
    // an image, a raised layer, or a raised squashfs file
    composed_image image;
    // true if the item is a layer or a squashfs file (it can move)
    bool movable = false;
    bool force = false;
};

// The runtime merge: make one composed_image from the items of the list
// (1-composition.md, "Runtime merge"). The result has the kind
// runtime_merge, no digest, and the image views of each item, named by the
// alias of the item.
util::expected<composed_image, model_error>
runtime_merge(std::vector<runtime_merge_item> items);

// Replace each @uenv-root@ in a patch with the mount point
envvars::patch replace_root(const envvars::patch& p, const std::string& mount);

// Concretise: the mount list, and the list of patches for the views.
// views: the views that the user gives. If there are none, concretise loads
// the default views of the image, unless use_default_views is false.
util::expected<concrete_image, model_error>
concretise(const composed_image& img,
           const std::vector<view_description>& views,
           bool use_default_views = true);

// "alias:view" or "view": the form of a view in --view and UENV_VIEW
std::string view_string(const view_description& v);

// The session variables: UENV_VIEW, UENV_MOUNT_LIST
envvars::patch session_patch(const concrete_image& c,
                             const std::vector<view_description>& views);

// Apply a list of patches to an environment, one at a time
// (1-composition.md, "Load more than one view").
// In each patch, ${@NAME@} reads from the state before the patch.
struct apply_report {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    // the invalid names in ${@NAME@}: one warning for each name
    std::set<std::string> invalid_names;
};
apply_report apply_patches(envvars::state& env,
                           const std::vector<envvars::patch>& patches);

// expand ${@NAME@} in a value, reading from `before`
std::string expand_view_value(std::string_view value,
                              const envvars::state& before,
                              apply_report& report);

} // namespace uenv

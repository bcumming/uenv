#pragma once

// The model of layers and images (design/1-composition.md).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <oci/digest.h>
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
    // the digest of the layer manifest: absent for a squashfs file
    std::optional<oci::digest> digest;
    // the size of the layer manifest: an index entry is a descriptor, so a
    // writer needs the size as well as the digest
    std::uint64_t manifest_size = 0;
    storage store;

    uenv::config config;
    std::string mount;
    bool relocatable = false;
    std::map<std::string, layer_view> views;
    std::optional<std::string> default_view;
};

struct named_layer {
    std::string alias;
    std::string mount;
    struct layer layer;
};

struct image_view {
    std::string description;
    std::vector<std::string> include; // "alias:view"
    envvars::patch env;
};

struct composed_image {
    // the digest of the top-level object: an index, or the layer manifest of
    // a raised layer. Absent for a squashfs file, or a merge with one.
    std::optional<oci::digest> digest;
    // true if the top-level object is a layer (the result of a raise)
    bool is_layer = false;

    // sorted by mount point
    std::vector<named_layer> layers;

    uenv::config config;
    std::map<std::string, image_view> views;
    // the views to load if the user names none.
    // A stored image has at most one. A merged list has one for each item.
    std::vector<std::string> default_views;
};

// the result of concretise: never stored, never an input to compose
struct concrete_image {
    // (file system image, mount point), sorted by mount point
    std::vector<std::pair<std::filesystem::path, std::string>> mounts;
    // applied in order, one at a time
    std::vector<envvars::patch> patches;
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

// one item of a list given to start/run: [alias=]label[:mount]
struct merge_item {
    std::optional<std::string> alias;
    std::optional<std::string> mount;
    // the image, a raised layer, or a raised squashfs file
    composed_image image;
    // true if the item is a layer or a squashfs file (it can move)
    bool movable = false;
    bool force = false;
};

// Merge a list of items into one composed_image ("Merge a list at start").
// The digest is the identity of the image that uenv would write for the
// result, calculated by `digest_of`, or absent if an item has no digest.
using digest_fn = std::function<std::optional<oci::digest>(const composed_image&)>;
util::expected<composed_image, model_error>
merge_items(std::vector<merge_item> items, digest_fn digest_of = {});

// Replace each @uenv-root@ in a patch with the mount point
envvars::patch replace_root(const envvars::patch& p, const std::string& mount);

// Concretise: the mount list, and the list of patches for the views.
// views: the names that the user gives (empty: the default views)
util::expected<concrete_image, model_error>
concretise(const composed_image& img, const std::vector<std::string>& views);

// The session variables: UENV_VIEW, UENV_MOUNT_LIST
envvars::patch session_patch(const concrete_image& c,
                             const std::vector<std::string>& views);

// Apply a list of patches to an environment, one at a time
// (1-composition.md, "Load more than one view").
// In each patch, ${@NAME@} reads from the state before the patch.
struct apply_report {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};
apply_report apply_patches(envvars::state& env,
                           const std::vector<envvars::patch>& patches);

// expand ${@NAME@} in a value, reading from `before`
std::string expand_view_value(std::string_view value,
                              const envvars::state& before,
                              apply_report& report);

} // namespace uenv

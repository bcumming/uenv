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
#include <variant>
#include <vector>

#include <oci/digest.h>
#include <oci/types.h>
#include <uenv/mount_point.h>
#include <uenv/view.h>
#include <util/envvars.h>
#include <util/expected.h>

namespace uenv {

// the placeholder for the root of a relocatable layer in a layer view
inline constexpr std::string_view uenv_root_placeholder = "@uenv-root@";

// the shared fields of a layer config and an image config
struct config {
    uenv::name name;
    // absent only for a squashfs file that a user starts directly: a stored
    // config always has them
    std::optional<uenv::name> version;
    std::optional<uenv::name> release;
    // empty means absent: the stored form omits it
    std::string description;
    std::map<uenv::name, std::string> meta;

    friend bool operator==(const config&, const config&) = default;
};

// "name/version:release", or the name if the config has no version and
// release (a squashfs file started directly)
std::string label_string(const config& c);

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

// the layer config: what the layer is, where to mount it, and how to use it
struct layer_config {
    // a layer config whose default view is one of its views
    static util::expected<layer_config, std::string>
    make(config shared, mount_point mount, bool relocatable,
         std::map<uenv::name, layer_view> views,
         std::optional<uenv::name> default_view);

    config shared;
    mount_point mount;
    bool relocatable = false;
    std::map<uenv::name, layer_view> views;
    std::optional<uenv::name> default_view;
};

struct layer {
    // make a layer from its parts: the layer config, the file system image,
    // and the descriptor of the layer manifest. The manifest is absent if and
    // only if the file system image has no digest: a squashfs file that is
    // started directly.
    static util::expected<layer, std::string>
    make(layer_config cfg, storage store,
         std::optional<oci::descriptor> manifest = std::nullopt);

    // the descriptor of the layer manifest, as an index entry refers to it.
    // Its digest is the identity of the layer. Absent for a squashfs file.
    std::optional<oci::descriptor> manifest;
    // the file system image: its digest and size
    storage store;

    uenv::config config;
    // the build mount point
    mount_point mount;
    bool relocatable = false;
    std::map<uenv::name, layer_view> views;
    std::optional<uenv::name> default_view;

    // the identity of the layer: the digest of its layer manifest
    std::optional<oci::digest> digest() const {
        if (!manifest) {
            return std::nullopt;
        }
        return manifest->digest;
    }
};

struct named_layer {
    uenv::name alias;
    mount_point mount;
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

using model_error = std::string;

// a change to the image config that a copy makes (copy --set)
struct set_version {
    uenv::name value;
};
struct set_release {
    uenv::name value;
};
struct set_meta {
    uenv::name key;
    std::string value;
};
struct erase_meta {
    uenv::name key;
};
using config_change = std::variant<set_version, set_release, set_meta, erase_meta>;

class composed_image;
struct runtime_merge_item;

util::expected<composed_image, model_error>
make_composed_image(std::optional<oci::digest> digest, config cfg,
                    std::vector<named_layer> layers,
                    std::map<uenv::name, image_view> views,
                    std::optional<uenv::name> default_view);
util::expected<composed_image, model_error>
raise_layer(const layer& l, std::optional<mount_point> mount, bool force,
            std::optional<uenv::name> alias);
util::expected<composed_image, model_error>
runtime_merge(std::vector<runtime_merge_item> items);
util::expected<composed_image, model_error>
promote(composed_image raised, const std::map<uenv::name, std::string>& meta);
util::expected<composed_image, model_error>
with_changes(composed_image img, const std::vector<config_change>& changes);

// The model of an image (1-composition.md, "The composed_image model"). Only
// the functions above make or change one, and each runs the checks of the
// model, so each composed_image is valid. The fields are read only.
class composed_image {
  public:
    // the digest of the top-level object: an index, or the layer manifest of
    // a raised layer. Absent for a squashfs file, for a runtime merge, and
    // for an image that is not stored yet.
    const std::optional<oci::digest>& digest() const {
        return digest_;
    }
    image_kind kind() const {
        return kind_;
    }
    // sorted by mount point
    const std::vector<named_layer>& layers() const {
        return layers_;
    }
    const uenv::config& config() const {
        return config_;
    }
    const std::map<image_view_name, image_view>& views() const {
        return views_;
    }
    // the views to load if the user names none.
    // A stored image has at most one. A runtime merge has the default view of
    // each item, in the order of the list.
    const std::vector<image_view_name>& default_views() const {
        return default_views_;
    }

    // the same image, identified by the digest of its stored objects: the
    // caller makes the objects (format::make_image_objects) and gives their
    // digest
    composed_image identified_by(oci::digest d) const {
        auto c = *this;
        c.digest_ = std::move(d);
        return c;
    }

  private:
    // the checks of the model ("Checks" in 1-composition.md), then the sort
    static util::expected<composed_image, model_error>
    checked(std::optional<oci::digest> digest, image_kind kind, uenv::config cfg,
            std::vector<named_layer> layers,
            std::map<image_view_name, image_view> views,
            std::vector<image_view_name> default_views, bool allow_forced_moves);

    composed_image(std::optional<oci::digest> digest, image_kind kind,
                   std::vector<named_layer> layers, uenv::config cfg,
                   std::map<image_view_name, image_view> views,
                   std::vector<image_view_name> default_views)
        : digest_(std::move(digest)), kind_(kind), layers_(std::move(layers)),
          config_(std::move(cfg)), views_(std::move(views)),
          default_views_(std::move(default_views)) {
    }

    friend util::expected<composed_image, model_error>
    make_composed_image(std::optional<oci::digest>, uenv::config,
                        std::vector<named_layer>, std::map<uenv::name, image_view>,
                        std::optional<uenv::name>);
    friend util::expected<composed_image, model_error>
    raise_layer(const layer&, std::optional<mount_point>, bool,
                std::optional<uenv::name>);
    friend util::expected<composed_image, model_error>
        runtime_merge(std::vector<runtime_merge_item>);
    friend util::expected<composed_image, model_error>
    promote(composed_image, const std::map<uenv::name, std::string>&);
    friend util::expected<composed_image, model_error>
    with_changes(composed_image, const std::vector<config_change>&);

    std::optional<oci::digest> digest_;
    image_kind kind_ = image_kind::image;
    std::vector<named_layer> layers_;
    uenv::config config_;
    std::map<image_view_name, image_view> views_;
    std::vector<image_view_name> default_views_;
};

// the result of concretise: never stored, never an input to compose
struct concrete_image {
    // (file system image, mount point), sorted by mount point
    std::vector<std::pair<std::filesystem::path, mount_point>> mounts;
    // applied in order, one at a time
    std::vector<envvars::patch> patches;
    // the views that the patches load, in order
    std::vector<view_request> views;
};

// Make a composed_image from its parts: sort the layers by mount point and
// apply the checks of "Checks" in 1-composition.md.
// (declared above composed_image)

// Raise a layer: a composed_image with one layer, and an image view for each
// layer view. `mount` is the mount point at which to use the layer.
inline util::expected<composed_image, model_error>
raise_layer(const layer& l, std::optional<mount_point> mount = std::nullopt,
            bool force = false) {
    return raise_layer(l, std::move(mount), force, std::nullopt);
}

// Promote: the raise of a layer, as an image with one layer (1-composition.md,
// "Promotion"), with `meta` added to the meta of the layer. The result is not
// stored yet: it has no digest.
// (declared above composed_image)

// A copy with changes: the image with the changes to its image config, and no
// digest. The checks run again, and a stored config always has a version and
// a release.
// (declared above composed_image)

// one item of the list given to uenv run/start or srun --uenv:
// [alias=]label[:mount]
struct runtime_merge_item {
    std::optional<uenv::name> alias;
    std::optional<mount_point> mount;
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
// (declared above composed_image)

// Replace each @uenv-root@ in a patch with the mount point
envvars::patch replace_root(const envvars::patch& p, const mount_point& mount);

// Concretise: the mount list, and the list of patches for the views.
// views: the views that the user gives. If there are none, concretise loads
// the default views of the image, unless use_default_views is false.
util::expected<concrete_image, model_error>
concretise(const composed_image& img,
           const std::vector<view_request>& views,
           bool use_default_views = true);

// "alias:view" or "view": the form of a view in --view and UENV_VIEW
std::string view_string(const view_request& v);

// The session variables: UENV_VIEW, UENV_MOUNT_LIST
envvars::patch session_patch(const concrete_image& c,
                             const std::vector<view_request>& views);

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

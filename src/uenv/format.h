#pragma once

// The stored format of layers and images (design/2-oci-layout.md).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <oci/digest.h>
#include <oci/types.h>
#include <uenv/model.h>
#include <uenv/mount_point.h>
#include <util/expected.h>

namespace uenv::format {

inline constexpr std::string_view mt_squashfs =
    "application/vnd.uenv.squashfs.v1";
inline constexpr std::string_view mt_layer_config =
    "application/vnd.uenv.layer-config.v1+json";
inline constexpr std::string_view at_layer = "application/vnd.uenv.layer.v1";
inline constexpr std::string_view mt_image_config =
    "application/vnd.uenv.image-config.v1+json";
inline constexpr std::string_view at_wrapper =
    "application/vnd.uenv.image-config.v1";
inline constexpr std::string_view at_image = "application/vnd.uenv.image.v1";
inline constexpr std::string_view ann_alias = "vnd.uenv.layer";
inline constexpr std::string_view ann_mount = "vnd.uenv.mount";
inline constexpr std::string_view mt_catalogue_event =
    "application/vnd.uenv.catalogue-event.v1+json";
inline constexpr std::string_view at_catalogue_event =
    "application/vnd.uenv.catalogue-event.v1";

using error = std::string;

// an object that uenv made: its canonical bytes and its descriptor
struct blob {
    std::string bytes;
    oci::digest digest;
    std::size_t size() const {
        return bytes.size();
    }
};
blob make_blob(std::string bytes);

// the digest of the OCI empty config "{}"
oci::digest empty_config_digest();

// the descriptor of a layer manifest, as an image index refers to it
oci::descriptor layer_descriptor(const oci::digest& digest, std::size_t size);

// the canonical encoding (JCS) of a JSON value
util::expected<std::string, error> canonical(const nlohmann::json& j);

//
// version checks of media types
//

// Check that a media type of a uenv type has a version that this uenv reads.
// For example "application/vnd.uenv.layer-config.v2+json" is refused with a
// message that names the type.
util::expected<void, error> check_media_type(std::string_view found,
                                             std::string_view expected);

//
// the configs
//

nlohmann::json env_to_json(const envvars::patch& p);
util::expected<envvars::patch, error>
env_from_json(const nlohmann::json& j,
              std::vector<std::string>* warnings = nullptr);

struct image_config {
    config shared;
    std::map<uenv::name, image_view> views;
    std::optional<uenv::name> default_view;
};

nlohmann::json to_json(const layer_config& c);
nlohmann::json to_json(const image_config& c);
util::expected<layer_config, error> parse_layer_config(std::string_view body);
util::expected<image_config, error> parse_image_config(std::string_view body);

//
// manifests and the index
//

struct layer_manifest {
    oci::descriptor config;
    oci::descriptor squashfs;
};
struct wrapper_manifest {
    oci::descriptor config;
};
struct index_entry {
    oci::descriptor manifest;
    uenv::name alias;
    mount_point mount;
};
struct image_index {
    oci::descriptor wrapper;
    std::vector<index_entry> layers;
};

util::expected<blob, error> make_layer_manifest(const blob& config,
                                                const oci::digest& sqfs,
                                                std::uint64_t sqfs_size);
util::expected<blob, error> make_wrapper_manifest(const blob& image_config);
// the entries are sorted by mount point
util::expected<blob, error> make_index(const blob& wrapper,
                                       std::vector<index_entry> layers);

util::expected<layer_manifest, error>
parse_layer_manifest(std::string_view body);
util::expected<wrapper_manifest, error>
parse_wrapper_manifest(std::string_view body);
util::expected<image_index, error> parse_index(std::string_view body);

enum class object_kind { image, layer };
// the kind of a top-level object, from its mediaType and artifactType
util::expected<object_kind, error> top_level_kind(std::string_view body);

//
// load a composed_image from blobs
//

// returns the bytes of a blob, already checked against its digest
using blob_reader =
    std::function<util::expected<std::string, error>(const oci::descriptor&)>;
// returns the local file of a squashfs, if there is one
using file_locator =
    std::function<std::optional<std::filesystem::path>(const oci::digest&)>;

util::expected<layer, error> load_layer(const oci::descriptor& manifest,
                                        const blob_reader& read,
                                        const file_locator& locate);

// load an image (index) or a layer (raised) from its top-level object
// the stored objects of an image or a layer, parsed: each layer manifest by
// its digest, and the index and the wrapper of an image
struct stored_objects {
    std::optional<image_index> index;
    std::optional<wrapper_manifest> wrapper;
    std::map<oci::digest, layer_manifest> layers;
};
// read and parse the objects of `img`, whose top-level object is `top`
util::expected<stored_objects, error>
read_objects(const composed_image& img, const oci::digest& top,
             const blob_reader& read);

util::expected<composed_image, error>
load_composed_image(const oci::digest& digest, const std::string& body,
                    const blob_reader& read, const file_locator& locate);

//
// write a composed_image: the objects that uenv makes
//

struct image_objects {
    blob image_config;
    blob wrapper;
    blob index;
};
image_config image_config_of(const composed_image& img);
util::expected<image_objects, error>
make_image_objects(const composed_image& img);

struct layer_objects {
    blob config;
    blob manifest;
};
layer_config layer_config_of(const layer& l);
// the layer config blob and the layer manifest of a layer config and a file
// system image: the manifest gives the descriptor of the layer
util::expected<layer_objects, error> make_layer_objects(const layer_config& c,
                                                        const storage& store);
util::expected<layer_objects, error> make_layer_objects(const layer& l);

// the identity of the image that uenv would write for img
std::optional<oci::digest> image_digest(const composed_image& img);

//
// tags
//

// "<version>-<release>[-<meta values>]-<id>", at most 128 characters
std::string make_tag(const config& c, const oci::digest& d);
// the id: the first 16 hex characters of a digest
std::string short_id(const oci::digest& d);

} // namespace uenv::format

#pragma once

// The migration of a v1 repository (design/3-repository.md, "Migration from
// v1", and design/6-migration.md).

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <util/expected.h>

namespace uenv {

struct migrate_report {
    unsigned migrated = 0;
    unsigned skipped = 0;     // already in the v2 part
    unsigned pending = 0;     // dry run: the images that a migration adds
    unsigned hashed = 0;      // squashfs files that were hashed
    unsigned failed = 0;
    std::vector<std::string> errors;
};

struct migrate_options {
    // find the digest of a squashfs that an earlier migration linked into
    // the store by its inode, and do not hash it again
    bool reuse_links = true;
    // write nothing, and hash nothing: count the v1 images that are not in
    // the v2 part (an image that would need a hash counts as pending)
    bool dry_run = false;
};

// true if root has a v1 part
bool has_v1_part(const std::filesystem::path& root);

// Promote each v1 image that is not in the v2 part to an image with one
// layer, directly in root/v2. Each image is written in the order of an add
// (blobs first, the database last), so the v2 part is usable after each
// image. A migration can run again at any time: it adds only the v1 images
// that are not in the v2 part. An image that fails is reported in the
// report, and does not stop the others.
util::expected<migrate_report, std::string>
migrate_v1(const std::filesystem::path& root, migrate_options opts = {},
           std::function<void(const std::string&)> log = {});

} // namespace uenv

#pragma once

// The migration of a v1 repository (design/3-repository.md, "Migration from
// v1", and design/6-migration.md).

#include <filesystem>
#include <functional>
#include <string>

#include <util/expected.h>

namespace uenv {

struct migrate_report {
    unsigned migrated = 0;
    unsigned skipped = 0;     // already in the database (a resumed migration)
    unsigned hashed = 0;      // squashfs files that were hashed
    unsigned failed = 0;
    std::vector<std::string> errors;
};

// promote each v1 image to an image with one layer, in root/v2.tmp, then
// rename root/v2.tmp to root/v2.
// reuse_links: find the digest of a squashfs that an earlier, stopped,
// migration linked into the store by its inode, and do not hash it again.
util::expected<migrate_report, std::string>
migrate_v1(const std::filesystem::path& root, bool reuse_links = true,
           std::function<void(const std::string&)> log = {});

} // namespace uenv

#include <sys/stat.h>

#include <filesystem>
#include <map>
#include <string>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <oci/manifest.h>
#include <uenv/format.h>
#include <uenv/meta.h>
#include <uenv/migrate.h>
#include <uenv/ops.h>
#include <uenv/repository.h>
#include <uenv/store.h>
#include <util/sha.h>

namespace uenv {

namespace fs = std::filesystem;
using util::unexpected;

util::expected<migrate_report, std::string>
migrate_v1(const fs::path& root, bool reuse_links,
           std::function<void(const std::string&)> log) {
    auto state = inspect_repo(root);
    if (state == repo_layout::v2) {
        return unexpected{
            fmt::format("the repository {} is already migrated", root.string())};
    }
    if (state != repo_layout::v1_only) {
        return unexpected{
            fmt::format("{} is not a v1 repository", root.string())};
    }
    auto v1 = open_repository(root, repo_open_mode::readonly);
    if (!v1) {
        return unexpected{v1.error()};
    }
    auto records = v1->query(uenv_label{});
    if (!records) {
        return unexpected{records.error()};
    }
    auto s = store::create_in(root, "v2.tmp");
    if (!s) {
        return unexpected{s.error()};
    }
    migrate_report report;

    // the blobs that an earlier migration linked: by inode
    std::map<std::pair<dev_t, ino_t>, oci::digest> linked;
    if (reuse_links) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(s->v2() / "blobs" / "sha256", ec)) {
            struct stat st {};
            if (::stat(e.path().c_str(), &st) == 0 && st.st_nlink > 1) {
                if (auto d = oci::digest::parse("sha256:" +
                                                e.path().filename().string())) {
                    linked.emplace(std::make_pair(st.st_dev, st.st_ino), *d);
                }
            }
        }
    }

    for (auto& r : *records) {
        auto paths = v1->uenv_paths(r.sha);
        auto label = fmt::format("{}/{}:{}@{}%{}", r.name, r.version, r.tag,
                                 r.system, r.uarch);
        auto meta = load_meta(paths.meta / "env.json");
        if (!meta) {
            report.failed++;
            report.errors.push_back(fmt::format("{}: {}", label, meta.error()));
            continue;
        }
        // the digest of the squashfs: the v1 sha is not always the digest of
        // the squashfs (a v1 image can be keyed by its manifest)
        std::optional<oci::digest> sqfs;
        struct stat st {};
        if (::stat(paths.squashfs.c_str(), &st) == 0) {
            if (auto it = linked.find({st.st_dev, st.st_ino});
                it != linked.end()) {
                sqfs = it->second;
            }
        }
        if (!sqfs) {
            auto h = util::sha256_file(paths.squashfs);
            if (!h) {
                report.failed++;
                report.errors.push_back(fmt::format("{}: {}", label, h.error()));
                continue;
            }
            report.hashed++;
            sqfs = oci::digest::sha256(*h);
        }
        auto cfg = v1::make_layer_config(
            *meta, v1::v1_label{r.version, r.tag, r.system, r.uarch});
        cfg.shared.name = r.name;
        layer l;
        l.store.digest = *sqfs;
        l.store.size = fs::file_size(paths.squashfs);
        l.config = cfg.shared;
        l.mount = cfg.mount;
        l.relocatable = false;
        l.views = cfg.views;
        l.default_view = cfg.default_view;
        auto lo = format::make_layer_objects(l);
        if (!lo) {
            report.failed++;
            report.errors.push_back(fmt::format("{}: {}", label, lo.error()));
            continue;
        }
        l.digest = lo->manifest.digest;
        l.manifest_size = lo->manifest.size();
        auto img = raise_layer(l, std::nullopt, false);
        img->is_layer = false;
        img->config.meta["system"] = r.system;
        img->config.meta["uarch"] = r.uarch;
        auto objs = format::make_image_objects(*img);
        if (!objs) {
            report.failed++;
            report.errors.push_back(fmt::format("{}: {}", label, objs.error()));
            continue;
        }
        if (auto e = s->db().get(objs->index.digest); e && *e) {
            report.skipped++;
            continue;
        }
        // blobs first: a hard link, no copy
        if (auto x = s->link_file(paths.squashfs, *sqfs); !x) {
            report.failed++;
            report.errors.push_back(fmt::format("{}: {}", label, x.error()));
            continue;
        }
        test_fault("migrate-after-link");
        auto empty = format::make_blob(std::string(oci::empty_config_body));
        for (auto* b : {&lo->config, &lo->manifest, &empty, &objs->image_config,
                        &objs->wrapper, &objs->index}) {
            if (auto x = s->put_bytes(*b); !x) {
                return unexpected{x.error()};
            }
        }
        // the database last
        if (auto x = s->record(objs->index.digest); !x) {
            report.failed++;
            report.errors.push_back(fmt::format("{}: {}", label, x.error()));
            continue;
        }
        report.migrated++;
        if (log) {
            log(fmt::format("migrated {} -> {}", label,
                            format::short_id(objs->index.digest)));
        }
    }
    if (report.failed) {
        return unexpected{fmt::format(
            "{} images were not migrated: the migration is not complete. "
            "Run it again after the problem is fixed.\n{}",
            report.failed, fmt::format("{}", fmt::join(report.errors, "\n")))};
    }
    if (auto x = s->write_index_json(); !x) {
        return unexpected{x.error()};
    }
    test_fault("migrate-before-rename");
    std::error_code ec;
    fs::rename(root / "v2.tmp", root / "v2", ec);
    if (ec) {
        return unexpected{fmt::format("unable to rename {}: {}",
                                      (root / "v2.tmp").string(), ec.message())};
    }
    return report;
}

} // namespace uenv

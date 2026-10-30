# Composable uenv: report of the spike

**Branch:** `spike/composable-v2`, from `main` at `ca5ef47` (2026-10-30).
**Input:** the design documents `design/1-composition.md` to
`design/6-migration.md`.

## Summary

The spike implements the design, and a test suite that checks it rule by
rule. Most of the design works as written. The model, the OCI layout, the
blob store and the order of writes held up under unit tests, fault injection
and real mounts. They also held up against two registries: zot and
distribution (`registry:2`).

The implementation found 25 shortcomings. Most are small gaps in the text.
Six need a change to the design:

1. **The merge of a list at start** has no defined identity, name or default
   view. The model has one `default_view`, and a merge needs one for each
   item.
2. **The strict migration** blocks each command on a v1 repository. This
   includes the start of a squashfs file, which needs no repository. It also
   blocks each user of a read-only site repository that the site did not
   migrate.
3. **The catalogue** (not designed yet) is the weakest part. An event log of
   tags loses events from a slow writer, a lookup trusts copies of the
   configs, and the history grows with no limit.
4. **The nested mount point** check is possible at compose time with no new
   dependency, and it costs some milliseconds.
5. **The image view `env`** keeps the old path when a compose moves a layer,
   with no check.
6. **The Slurm plugin** reads `UENV_MOUNT_LIST` into 1024 bytes, which holds
   only 6 layers from the blob store. This is from the code: no test ran it.

The spike fixes 11 of the shortcomings, in separate commits. Most fixes have
a test that fails before the fix and passes after it. The fixes are
proposals for the design: the design documents are not changed.

## 1. The implementation

The spike adds one module for each design topic. It uses the code of `main`
where it fits: the native OCI client, `envvars`, `sha256`, and the argument
parser.

| module                       | design            | what it does                                                     |
| ---------------------------- | ----------------- | ---------------------------------------------------------------- |
| `src/util/jcs.*`             | 2 "Canonical"     | RFC 8785 encoding of JSON                                        |
| `src/util/sqlite.*`          | 3 "The database"  | SQLite with bound parameters                                     |
| `src/uenv/model.*`           | 1                 | `composed_image`, checks, raise, merge, concretise, patches      |
| `src/uenv/format.*`          | 2                 | configs, manifests, index, media types, tags                     |
| `src/uenv/store.*`           | 3                 | blob store, v2 schema, add, delete, update, `index.json`         |
| `src/uenv/ops.*`             | 1, 3              | add (promote), compose, labels, resolution of a list             |
| `src/uenv/registry.*`        | 4, 5              | push, pull, copy, delete, attach, catalogue                      |
| `src/uenv/migrate.*`         | 3, 6              | `uenv repo migrate`                                              |
| `src/uenv/session.*`         | 1 "Concretisation"| the environment of `start`, `run` and the Slurm plugin           |
| `src/cli/image2.cpp`, `repo2`| all               | `uenv image ...` and `uenv repo ...`                             |

The commits are in this order:

| commit    | content                                                               |
| --------- | --------------------------------------------------------------------- |
| `1a5736b` | core: model, format, store, local operations, unit tests              |
| `e9884a1` | registry operations and the catalogue, tests against zot              |
| `1634e42` | the CLI, and the end-to-end suite `composable.bats`                   |
| `4bc65dd` | the session in the library, the Slurm plugin on the composed model    |
| `0a8a6e0` | fixes, one for each commit, to `585f657` (see section 5)              |

The CLI replaces `uenv image ls/add/rm/inspect/find/pull/push/copy/delete`
and `uenv repo create/status/update/migrate`. It adds `uenv image compose`
and `uenv image attach`. `uenv start` and `uenv run` take a list of images,
layers (`+label`) and squashfs files, with `--force`.

## 2. How the spike settled the open questions

The design leaves some questions open, and `5-discovery.md` is a skeleton.
The spike chose the simplest solution that is good enough to test.

| question                              | choice in the spike                                                         |
| ------------------------------------- | --------------------------------------------------------------------------- |
| the catalogue (5, not written)        | an event log in the registry: a tagged manifest for each push, copy, delete |
| the form of an event                  | the event JSON is in `data` of the config descriptor: one GET for each      |
| a refresh of the catalogue            | `tags/list` after the newest seen tag, then a GET for each new event        |
| `@` and `%` (5, site attributes)      | `[label] at = "system"`, `percent = "uarch"`. A missing key matches         |
| the default system                    | `system_name` sets the `@` filter when the label has none, as in v1         |
| the layer marker (5, question 1)      | `+`, as proposed                                                            |
| a name with an id (5, question 2)     | no new syntax: an id, a digest or a full reference                          |
| a status after a copy (4, question 1) | the leading proposal: `[[referrers]]` with `travel` and `fan_out`           |
| the steps of the operations (4)       | from the decided rules: order of writes, empty blob, mount, digests         |
| the input of compose                  | a JSON file: the image config, and `layers` (`alias`, `mount`, `from`)      |
| the merge of one item (1)             | one item with no alias and no mount is the item itself                      |
| the name of a merge (1)               | the aliases in the order of the layers                                      |

**Why an event log.** The catalogue must work with the OCI API only. It must
also lose no update when writers run at the same time (5, "Requirements").
A new tag for each event needs no read before the write, so two writers never
conflict. The cost is that the history grows (see C6 in section 4).

**Why a missing key matches.** A user image often has no `system`. With the
default `@` filter of v1, an exact match would hide each such image.

**The readable name with an id.** The safe characters for a shell are all in
use: `/`, `:`, `@`, `%`, `,`, `=` and `+`. A bare id works for each lookup in
the tests, so the spike adds no syntax.

## 3. The test suite

The suite has three parts. Each test has a name of the form
`<document>/<section>: <description>`. The script
`docs/composable-spike/coverage.py` reads the names, and makes a table of the
design sections and their tests.

| part        | file                                | cases | what it uses                       |
| ----------- | ----------------------------------- | ----- | ---------------------------------- |
| model       | `test/unit/v2_model.cpp`            | 39    | memory only                        |
| repository  | `test/unit/v2_store.cpp`            | 19    | temporary repositories, `fork`     |
| registry    | `test/unit/v2_registry.cpp`         | 19    | a zot that the tests start         |
| end to end  | `test/integration/composable.bats`  | 29    | the CLI, real mounts, zot          |

### What each part checks

- **Model** (`[jcs]`, `[config]`, `[model]`, `[env]`, `[concretise]`,
  `[oci-layout]`): the RFC 8785 test vectors for the key order. The strict
  parser, unknown fields, and the refusal of a later version. The five checks
  of a `composed_image`. Each permutation of three layers gives the same
  digest, mounts and environment. Raise, `--force` and the merge of a list.
  The four operations on a prefix path, and the rules of `${@NAME@}`. Each
  form of index that the design calls an error. The form of a tag.
- **Repository** (`[store]`, `[compose]`): the layout, and the refusal of a
  v1 repository and of an unknown `user_version`. Promotion, shared blobs,
  lookup by label, id and layer marker. An add and a delete that stop at
  each step (`UENV_TEST_FAULT`, in a child process). The 24-hour grace period
  of a prune, and `--force`. Six writers at the same time. Compose, which
  copies layer manifests byte for byte.
- **Registry** (`[registry]`, `[catalogue]`, `[gc]`): the three forms of an
  address. The order of writes. The empty blob, and no overwrite of a tag.
  Checks before a download. A copy with and without a change, and the
  referrers that travel. Delete, then zot GC. Attach with fan-out.
  Pagination. A registry with no catalogue. Push from an OCI layout
  directory. The catalogue with eight writers at the same time, a slow
  writer, and a wrong event.
- **End to end**: add, ls, inspect, run with the setuid helper, merged lists,
  aliases, the order of views, compose with a nested mount point, a missing
  squashfs, `oras` on the repository, a Python reference for the canonical
  form, a stopped add and pull, migration and its resume, and the registry
  commands through the CLI.

### Results

| run                                                   | result                                          |
| ----------------------------------------------------- | ----------------------------------------------- |
| `build-v2/test/unit` (all, old and new)               | 5007 assertions pass                            |
| `build-v2/test/bats test/composable.bats`             | 29 of 29 pass, with real mounts and zot         |
| new unit tests with AddressSanitizer                  | all pass, no ASan error                         |
| registry tests on distribution `registry:2`           | 18 of 23 pass (see R3 in section 4)             |
| Slurm plugin                                          | builds (`build-slurm`). Not run: no cluster     |
| old suites `cli.bats`, `registry.bats`                | 4 of 16 and 0 of 5 pass: they test v1 formats   |
| old suite `completion.bats`                           | 16 of 24 pass: completion reads the v1 database |

### How to run it

```
meson setup -Dtests=enabled -Dsquashfs_mount=true build-v2
ninja -C build-v2
cd build-v2
./test/unit "[jcs],[config],[model],[env],[concretise],[oci-layout]"
./test/unit "[store],[registry],[catalogue],[registry-addr]"
./test/unit "[gc]"                # zot GC, 10 s
./test/unit "[.bench]"            # the first fill of the catalogue
./test/bats ./test/composable.bats
```

The mount tests need a setuid `squashfs-mount` in `$STAGING_PATH/bin` or in
`/usr/local/bin`. They skip without it. To run the registry tests on a
different registry, start it, and set `UENV_TEST_REGISTRY`:

```
docker run -d -p 127.0.0.1:5000:5000 -e REGISTRY_STORAGE_DELETE_ENABLED=true registry:2
UENV_TEST_REGISTRY=http://127.0.0.1:5000 ./test/unit "[registry]~[gc]"
```

### What the design passed

The tests show that these rules work, with no change:

- **The order of layers has no meaning.** Each permutation gives the same
  identity, mounts and environment.
- **The identity is reproducible.** The same file and label give the same id
  in two repositories, and compose gives the same digest each time.
- **The canonical form** agrees with an independent Python encoding on each
  object that uenv makes.
- **The repository is an OCI layout.** `oras` lists its tags and reads its
  manifests.
- **The order of writes is safe.** An add, a pull or a delete that stops at
  any step leaves a valid repository. `uenv repo update` deletes what it
  left.
- **Writes from more than one process** need no lock: six writers add six
  images.
- **A registry keeps the layers of an image.** After a delete of one image
  and a zot GC, an image that shares its layers is still complete.
- **Views in order** and `${@NAME@}` behave as the design says, with real
  mounts.
- **The migration** links the squashfs files, and does not change the v1
  part.

## 4. Shortcomings

Each entry gives the design section, what happened, and the evidence. The
severity is high (the design needs a change), medium (a rule is missing),
or low (the text is not clear).

### The model and the start (1-composition)

| id  | section                         | severity | status     |
| --- | ------------------------------- | -------- | ---------- |
| M1  | "Merge a list at start"         | high     | fixed      |
| M2  | "Merge a list at start"         | medium   | spike rule |
| M3  | "Merge a list at start"         | medium   | fixed      |
| M4  | "Merge", "Concretisation"       | low      | open       |
| M5  | "Nested mount points"           | high     | fixed      |
| M6  | "Image views"                   | medium   | fixed      |
| M7  | "The composed_image model"      | low      | spike rule |
| M8  | "Variables in a value"          | low      | spike rule |
| M9  | "Rules" of mount points         | low      | spike rule |
| M10 | "A check for absolute paths"    | low      | spike rule |

- **M1. A merge has no identity.** The model has one `default_view`. A merge
  takes "the `default_view` of each item", so the result has more than one.
  An image config holds only one, so the "image that uenv would write" does
  not exist when two items have a default view. This is the usual case. The
  test `1/merge: the digest of a merged list` shows it. The spike makes the
  model hold a list, and leaves the default views out of the identity.
- **M2. A merge of one image changes the image.** A list of one image, merged
  with the rules, gets views named `alias:view`, an empty `meta` and a new
  digest. Thus `uenv start my-env --view=dev` would fail. The spike treats a
  list of one item with no alias and no mount as the item itself.
- **M3. A merge has no name.** The design does not give the name, version or
  release of a merge. The first spike used the aliases in the order of the
  list. The identity then depended on that order: the test of M1 showed it.
  The fix uses the order of the layers.
- **M4. Two kinds of view share one syntax.** The image views of a merge are
  named `alias:view`, which is also the full name of a layer view. The image
  view comes first in the lookup, so the result is right. But a reader cannot
  tell the two kinds apart.
- **M5. The nested mount point is not checked.** The design says that a check
  at compose needs `unsquashfs`, and that uenv does not want a new
  dependency. But uenv already runs `unsquashfs` to read `meta/` from a
  squashfs file (`src/util/fs.cpp`). Without a check, the start fails with
  "the mount point is not a valid path: /user-environment/nope", with no
  layer name. A filtered listing (`unsquashfs -l <file> <path>`) takes about
  2 ms on a squashfs with 200 000 inodes. The fix checks at compose. A merge
  at start is still not checked.
- **M6. An image view keeps the old path after a move.** An image view `env`
  uses concrete paths. When a compose moves a relocatable layer, an `env`
  with the old path passes with no message. The fix gives a warning,
  unless a different layer is at the old path.
- **M7. The model needs the size of a manifest.** An index entry is a
  descriptor: a digest and a size. The model has only the digest of a layer.
  The spike adds `manifest_size`.
- **M8. "Does not replace an invalid name"** does not say if the text stays.
  The spike keeps it.
- **M9. A mount point with `,` or `:`** cannot go into `UENV_MOUNT_LIST`. The
  spike refuses it.
- **M10. "Contains its build mount point"** needs a rule for a prefix:
  `/a/tools` is in `/a/tools/bin`, but not in `/a/tools2`. The spike requires
  the end of the value, or a character that cannot continue a file name.

### The repository and the migration (3, 6)

| id  | section                                  | severity | status |
| --- | ---------------------------------------- | -------- | ------ |
| D1  | 3 "Layout", 6 "The rollout"              | high     | fixed  |
| D2  | 6 "The migration of a local repository"  | medium   | fixed  |
| D3  | 4 "Site rules for referrers"             | low      | open   |

- **D1. The strict migration blocks too much.** Each command stops on a v1
  repository. In the test environment, the default repository
  (`~/.uenv/repo`) was in the v1 format. Thus `uenv run ./file.squashfs`
  failed, although it needs no repository. The design also says that a
  repository can be read only, for example one that a site provides. A user
  cannot migrate such a repository, so each command fails until the site
  migrates it. The fixes open the repositories only for a lookup, and skip a
  read-only v1 repository with a warning.
- **D2. The migration hashes each squashfs.** On `main`, a v1 record can have
  the digest of a manifest, not of the squashfs file. Thus the migration must
  hash each file. A migration that continues must find "each v1 image whose
  index is already in the database". To do that, it must hash each file
  again. The spike finds the digest of a file that an earlier run linked by
  its inode, and does not hash it again. The resume test shows "2 squashfs
  hashed" for 3 images.
- **D3. The site rules cannot be tested end to end.** The referrer rules are
  only in `/etc/uenv/config.toml`, which a test cannot write without root.
  The unit tests give the rules directly.

### The registry operations (4)

| id  | section                                 | severity | status    |
| --- | --------------------------------------- | -------- | --------- |
| R1  | "uenv uploads a blob only if"           | low      | evidence  |
| R2  | "Delete and cleanup"                    | low      | evidence  |
| R3  | "Requirements of a registry"            | low      | evidence  |
| R4  | Slurm and the blob store                | high     | fixed     |

- **R1. zot does not need the mount.** zot answers a HEAD for a blob that is in
  a different repository with 200, because it stores each blob one time. Thus
  step 2 (the cross-repository mount) never runs on zot. On `registry:2` the
  mount runs: a push of a composed image mounts the two squashfs files, and
  uploads only small blobs.
- **R2. zot deletes untagged manifests only with a policy.** The default zot
  GC keeps them. With the retention policy `deleteUntagged`, a delete of one
  image deletes its wrapper, and keeps the layers of a sibling image. This is
  the rule that the design asks of a sweep.
- **R3. distribution 2.x does not meet the requirements.** It has no
  referrers API, so 3 referrer tests fail. It ignores `n` and `last` on
  `tags/list`, and returns all tags in one response. The design already
  requires the referrers API, so this is evidence, not a defect.
- **R4. The Slurm plugin reads 1024 bytes.** An entry of `UENV_MOUNT_LIST` has
  about 149 characters on Alps, in v1 and in the blob store. 1024 bytes hold
  6 layers. Composition makes images with more layers. From the code, with a
  7th layer `spank_getenv` returns `ESPANK_NOSPACE`, and the plugin stops
  with an error on the compute nodes. No test ran this. The fix grows the
  buffer when `spank_getenv` reports `ESPANK_NOSPACE`. The fix builds, but no
  test runs it.

### The catalogue (5, a choice of the spike)

| id  | problem                                     | severity | status   |
| --- | ------------------------------------------- | -------- | -------- |
| C1  | a slow writer: an event is lost             | high     | fixed    |
| C2  | `last` must be a tag that exists            | medium   | fixed    |
| C3  | a push that stops after the tag             | medium   | analysis |
| C4  | a lookup trusts copies of the configs       | high     | fixed    |
| C5  | the first event for a digest wins           | low      | open     |
| C6  | the history grows with no limit             | high     | open     |

- **C1. An event of a slow writer is lost.** The tags sort by the time of the
  writer. A writer that is slow, or whose clock is behind, writes an event
  that sorts before the newest event that a reader saw. The reader never
  reads it. The test uses a clock that is 5 s behind. The fix lists from the
  newest seen tag before a window of 300 s, and skips the events that it
  saw. The window costs no request.
- **C2. The cursor must be a real tag.** zot returns no tags for a value of
  `last` that is not a tag, so a cursor made from a time fails. The fix uses
  a seen tag. distribution ignores `last`, so there the set of seen tags
  keeps the result right.
- **C3. A push that stops after the tag is not in the catalogue.** The image
  is in the registry, with its tag, but a lookup by label does not find it.
  A second push adds the event, because the tag check accepts the same
  digest.
  The other order (event first) gives an entry for an image that does not
  exist. The spike keeps the order, and documents the second push.
- **C4. A lookup trusts the copies.** An event holds the fields of each
  image, which nobody checks against the configs. A wrong event gave a
  different image for a label: the test shows it without the fix. The fix
  checks the configs against the label before the download of a squashfs.
- **C5. The first event for a digest wins.** The find database keys records
  by digest. A later event for the same digest, for example a correction, has
  no effect.
- **C6. The history grows.** A first fill costs one request for each event,
  plus the listing. The benchmark measured 0.04 s for 50 events and 0.5 s for
  400, on a local zot. Each push, copy and delete adds an event, and nothing
  deletes them. Requirement 5 ("a first fill costs a small number of
  requests") then fails after some months.

### The tests and the tools

- **T1. A hang in the test harness.** The tests start zot with pipes for its
  output, and nothing reads them. After some 64 kB of output, zot blocked in
  a request that held the lock of a repository. The catalogue benchmark then
  hung after about 200 events. The fix sends the output to a file.
  `test/unit/oci_registry.cpp` on `main` starts zot in the same way.
- **T2. The old suites test v1.** `cli.bats`, `registry.bats` and the label
  completion use the v1 database, the v1 fixture and the listing service.
  They need a new version for the new format.

## 5. Fixes and findings

| commit    | fixes | change                                         | test                                       |
| --------- | ----- | ---------------------------------------------- | ------------------------------------------ |
| `0a8a6e0` | R4    | grow the buffer of `getenv_wrapper`            | builds only                                |
| `c565827` | C1    | re-read a window of 300 s, skip seen events    | `5/catalogue: an event with an earlier...` |
| `5470cc9` | M6    | warn about an old path in an image view `env`  | `1/compose: a stale path...`, bats         |
| `40da085` | M1    | the identity of a merge has no default views   | `1/merge: the digest of a merged list`     |
| `8a7157e` | M3    | the name of a merge in the order of the layers | the same test, with a reversed list        |
| `5552646` | D1    | skip a read-only v1 repository                 | `6/rollout: ...`                           |
| `c3c3627` | C4    | check the configs against the label            | `5/catalogue: an event that does not...`   |
| `06117d4` | M5    | check a nested mount point at compose          | bats `1/nested mount points: ...`          |
| `585f657` | M5    | no check if `unsquashfs` cannot read it        | the store tests                            |
| `013206d` | T1    | zot output to a file, the benchmark of C6      | `[.bench]`                                 |

The changes in `1634e42` also fix D1 (the start of a squashfs file opens no
repository) and C2. The inode reuse of D2 is in `1634e42`.

### Proposed changes to the design

1. **1-composition, "Merge a list at start":** the result has one default view
   for each item, in the order of the list. The identity is the image that
   uenv would write with no default view. The name is the aliases in the
   order of the layers. A list of one item with no alias and no mount is the
   item itself.
2. **1-composition, "Nested mount points":** compose checks each nested mount
   point with `unsquashfs`, which uenv already uses. A merge at start is not
   checked.
3. **1-composition, "Image views":** compose warns about an image view `env`
   that has the build mount point of a moved layer.
4. **3-repository and 6-migration:** a command that needs no lookup does not
   open the repositories. uenv skips a read-only v1 repository, with a
   warning. Thus the site must migrate its repositories before the upgrade.
5. **6-migration:** the migration hashes each squashfs, because the v1 sha is
   not always the digest of the file. A resumed migration finds the linked
   files by inode.
6. **5-discovery, "The catalogue":** if the design keeps the event log, it
   needs a window and a set of seen events. A lookup checks the configs
   against the label. It also needs compaction, for example a snapshot event
   that holds the state, so that a first fill starts at the newest snapshot.
   The spike did not test compaction.
7. **The Slurm plugin:** no fixed limit on the size of an environment
   variable.

## 6. What the spike does not cover

- **JFrog.** Each registry test ran on zot and on distribution. The JFrog
  evidence in `design/evidence/` is from earlier tests.
- **The Slurm plugin at run time.** It builds, but no cluster was available.
- **The FUSE backend.** The spike used the kernel backend only.
- **Tab completion** of labels and views in the new format.
- **The code in `uenv::v1`.** The v1 code stays in place: the migration uses
  it, and nothing else does.
- **Erofs, Lustre striping, credentials and signatures.** The spike does not
  change them.

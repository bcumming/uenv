# Composable uenv: the design and its spike

**Branch:** `spike/composable-v2`, from `main` at `ca5ef47`.
**Design:** the documents `design/1-composition.md` to
`design/6-migration.md`.

This report describes the current state of the design and of the spike that
implements it. When the design documents describe the solution to a
problem, and the spike implements it with a test, the problem is not in this
report.

## Summary

The spike implements the design, and a test suite that checks it rule by
rule. The model, the runtime merge, the OCI layout, the blob store, the
order of writes and the migration of v1 repositories work as the design
says. They held up under unit tests, fault injection and real mounts. The
registry operations held up against two registries: zot and distribution
(`registry:2`).

These items are open:

1. **The catalogue** is the weakest part. The design does not describe it
   yet, and the spike uses an event log of tags. The log needs a window for
   slow writers and a check of the configs against the label, which the
   spike has. It also needs compaction, which it does not have: the history
   grows with no limit (C1 to C6).
2. **Three rules are missing from the design** (M8 to M10). The spike
   chose a rule for each.
3. **Two parts are not tested:** the site rules for referrers end to end
   (D3), and the old test suites, which test v1 (T2).
4. **Three registry observations** (R1 to R3) are evidence for the design,
   not defects.

## 1. The implementation

The spike adds one module for each design topic. It uses the code of `main`
where it fits: the native OCI client, `envvars`, `sha256`, the label and
view parsers, and the argument parser.

| module                       | design            | what it does                                                     |
| ---------------------------- | ----------------- | ---------------------------------------------------------------- |
| `src/util/jcs.*`             | 2 "Canonical"     | RFC 8785 encoding of JSON                                        |
| `src/util/sqlite.*`          | 3 "The database"  | SQLite with bound parameters                                     |
| `src/uenv/model.*`           | 1                 | `composed_image`, checks, raise, runtime merge, concretise       |
| `src/uenv/format.*`          | 2                 | configs, manifests, index, media types, tags                     |
| `src/uenv/store.*`           | 3                 | blob store, v2 schema, add, delete, update, `index.json`         |
| `src/uenv/ops.*`             | 1, 3              | add (promote), compose, labels, nested mount points              |
| `src/uenv/registry.*`        | 4, 5              | push, pull, copy, delete, attach, catalogue                      |
| `src/uenv/migrate.*`         | 3, 6              | `uenv repo migrate`, and the v1 images that are not migrated     |
| `src/uenv/session.*`         | 1 "Concretisation"| the environment of `start`, `run` and the Slurm plugin           |
| `src/cli/image2.cpp`, `repo2`| all               | `uenv image ...` and `uenv repo ...`                             |

The CLI replaces `uenv image ls/add/rm/inspect/find/pull/push/copy/delete`
and `uenv repo create/status/update/migrate`. It adds `uenv image compose`
and `uenv image attach`. `uenv start` and `uenv run` take a list of images,
layers (`+label`) and squashfs files, with `--force`. A list of more than
one item is a runtime merge.

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
| model       | `test/unit/v2_model.cpp`            | 45    | memory only                        |
| repository  | `test/unit/v2_store.cpp`            | 19    | temporary repositories, `fork`     |
| registry    | `test/unit/v2_registry.cpp`         | 19    | a zot that the tests start         |
| end to end  | `test/integration/composable.bats`  | 33    | the CLI, real mounts, zot          |

### What each part checks

- **Model** (`[jcs]`, `[config]`, `[model]`, `[env]`, `[concretise]`,
  `[oci-layout]`): the RFC 8785 test vectors for the key order. The strict
  parser, the parse of view references, unknown fields, and the refusal of a
  later version. The five checks of a `composed_image`. Each permutation of
  three layers gives the same digest, mounts and environment. Raise and
  `--force`. The runtime merge: images with more than one layer, view names
  of each item, a rename of an alias, a shared layer, no identity. The four
  operations on a prefix path, and the rules of `${@NAME@}`. Each form of
  index that the design calls an error. The form of a tag.
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
- **End to end**: add, ls, inspect, run with the setuid helper, the default
  view and `--no-default-view`, runtime merges with images of more than one
  layer, the order of views, compose with a nested mount point, a missing
  squashfs, `oras` on the repository, a Python reference for the canonical
  form, a stopped add and pull, the migration (a stop, a failed image, a
  second run, `meta/` inside the squashfs), and the registry commands
  through the CLI.

### Results

| run                                                   | result                                          |
| ----------------------------------------------------- | ----------------------------------------------- |
| `build-v2/test/unit` (all, old and new)               | 5068 assertions pass                            |
| `build-v2/test/bats test/composable.bats`             | 33 of 33 pass, with real mounts and zot         |
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

### What the tests show

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
- **A runtime merge** starts an image with more than one layer next to a
  different image, with real mounts. A layer that two items share is
  mounted once, and a nested mount point is checked before the mount.
- **The migration** links the squashfs files, and does not change the v1
  part. Each migrated image can be used at once. A failed image does not
  stop the others, and a second run adds only what is not migrated.

## 4. Open items

Each entry gives the design section, what happened, and the evidence.

### Rules that the design does not give (1-composition)

The spike chose a rule for each. The design must state a rule.

| id  | section                         | status     |
| --- | ------------------------------- | ---------- |
| M8  | "Variables in a value"          | spike rule |
| M9  | "Rules" of mount points         | spike rule |
| M10 | "A check for absolute paths"    | spike rule |

- **M8. "Does not replace an invalid name"** does not say if the text stays.
  The spike keeps it.
- **M9. A mount point with `,` or `:`** cannot go into `UENV_MOUNT_LIST`. The
  spike refuses it.
- **M10. "Contains its build mount point"** needs a rule for a prefix:
  `/a/tools` is in `/a/tools/bin`, but not in `/a/tools2`. The spike requires
  the end of the value, or a character that cannot continue a file name.

### The catalogue (5, a choice of the spike)

The design does not describe the catalogue yet. These are the problems of
the event log that the spike uses. C1, C2 and C4 have a solution in the
spike, with a test. The catalogue design must include them.

| id  | problem                                     | status     |
| --- | ------------------------------------------- | ---------- |
| C1  | a slow writer: an event is lost             | spike only |
| C2  | `last` must be a tag that exists            | spike only |
| C3  | a push that stops after the tag             | analysis   |
| C4  | a lookup trusts copies of the configs       | spike only |
| C5  | the first event for a digest wins           | open       |
| C6  | the history grows with no limit             | open       |

- **C1. An event of a slow writer is lost.** The tags sort by the time of the
  writer. A writer that is slow, or whose clock is behind, writes an event
  that sorts before the newest event that a reader saw. The reader never
  reads it. The test uses a clock that is 5 s behind. The spike lists from
  the newest seen tag before a window of 300 s, and skips the events that it
  saw. The window costs no request.
- **C2. The cursor must be a real tag.** zot returns no tags for a value of
  `last` that is not a tag, so a cursor made from a time fails. The spike
  uses a seen tag. distribution ignores `last`, so there the set of seen
  tags keeps the result right.
- **C3. A push that stops after the tag is not in the catalogue.** The image
  is in the registry, with its tag, but a lookup by label does not find it.
  A second push adds the event, because the tag check accepts the same
  digest. The other order (event first) gives an entry for an image that
  does not exist. The spike keeps the order, and documents the second push.
- **C4. A lookup trusts the copies.** An event holds the fields of each
  image, which nobody checks against the configs. A wrong event gave a
  different image for a label: the test shows it without the check. The
  spike checks the configs against the label before the download of a
  squashfs.
- **C5. The first event for a digest wins.** The find database keys records
  by digest. A later event for the same digest, for example a correction, has
  no effect.
- **C6. The history grows.** A first fill costs one request for each event,
  plus the listing. The benchmark measured 0.04 s for 50 events and 0.5 s for
  400, on a local zot. Each push, copy and delete adds an event, and nothing
  deletes them. Requirement 5 ("a first fill costs a small number of
  requests") then fails after some months.

### Parts that are not tested

- **D3. The site rules for referrers** (4, "Site rules for referrers") are
  only in `/etc/uenv/config.toml`, which a test cannot write without root.
  The unit tests give the rules directly, but no test reads them from the
  site config.
- **T2. The old suites test v1.** `cli.bats`, `registry.bats` and the label
  completion use the v1 database, the v1 fixture and the listing service.
  They need a new version for the new format.

### Registry observations (4)

These are evidence for the design, not defects.

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
  requires the referrers API.

## 5. Proposed changes to the design

1. **5-discovery, "The catalogue":** if the design keeps the event log, it
   needs a window and a set of seen events (C1, C2). A lookup checks the
   configs against the label (C4). It also needs compaction, for example a
   snapshot event that holds the state, so that a first fill starts at the
   newest snapshot (C6). The spike did not test compaction.
2. **1-composition:** a rule for each of M8 to M10. The rules of the spike
   are a good start.

## 6. What the spike does not cover

- **JFrog.** Each registry test ran on zot and on distribution. The JFrog
  evidence in `design/evidence/` is from earlier tests.
- **The Slurm plugin at run time.** It builds, but no cluster was available.
  The plugin no longer limits the size of `UENV_MOUNT_LIST`, but no test ran
  it with many layers.
- **The FUSE backend.** The spike used the kernel backend only.
- **Tab completion** of labels and views in the new format.
- **The code in `uenv::v1`.** The v1 code stays in place: the migration uses
  it, and nothing else does.
- **Erofs, Lustre striping, credentials and signatures.** The spike does not
  change them.

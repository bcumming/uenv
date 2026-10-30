# Findings log (working notes, not the report)

Each entry: id, design section, what happened, evidence, status.

- F-merge-default-views (1 "Merge a list at start", "The composed_image
  model"): composed_image has one default_view, a merge needs one per item.
  The image config holds one, so the "digest of the image uenv would write"
  does not exist when two items have a default view (the usual case).
  Evidence: v2_model.cpp "1/merge: the digest of a merged list". Spike: model
  uses default_views (a list); a merge has no digest then.
- F-merge-single (1 "Merge"): a list of one image, raised as a merge, renames
  its views to alias:view, drops meta and changes the digest. Spike: one item
  with no alias/mount is the item itself. Design must say so.
- F-merge-config (1 "Merge"): name/version/release of the merged config not
  specified. Spike: name = aliases joined with ','.
- F-merge-view-names (1 "Merge", "Concretisation"): merged image views are
  named alias:view, the same syntax as a layer view. Works because the image
  view wins, but two namespaces share one syntax.
- F-descriptor (1 model vs 2 index): an index entry is a descriptor; the
  model has the layer digest but not the manifest size. Spike adds
  manifest_size.
- F-catalogue-order (5 catalogue, spike choice): tags ordered by writer time.
  A slow writer, or a clock behind, writes an event that sorts before the
  newest event a reader saw: the reader never reads it. Evidence: v2_registry
  "5/catalogue: an event with an earlier time". Fix: re-read an overlap
  window, skip seen tags. Tested.
- F-zot-last (5, registry behaviour): zot returns no tags when `last` is not
  an existing tag. The overlap cursor must be a tag that exists. Evidence:
  manual curl probe, fix in catalogue::refresh.
- F-zot-dedupe (4 "uenv uploads a blob only if"): zot answers HEAD 200 for a
  blob that is in a different repository (global dedupe), so step 2 (mount) is
  never used on zot. The mount path is not tested by the zot suite.
- F-zot-gc (4 "Delete and cleanup"): zot deletes untagged manifests only with
  a retention policy (deleteUntagged). With it, a delete of one image keeps the
  layers of a sibling image. Evidence: v2_registry "[gc]".
- F-mount-chars (1 "Rules" of mount points): ',' and ':' in a mount point
  cannot be written in UENV_MOUNT_LIST. Spike refuses them.
- F-reloc-check (1 "A check for absolute paths"): "contains its build mount
  point" needs a rule for prefixes (/a/tools vs /a/tools2). Spike: the mount
  point followed by end, '/', ':' or a non-alphanumeric character.
- F-invalid-name (1 "Variables in a value"): "does not replace an invalid
  name" - keep the text or drop it? Spike keeps it.
- F-value-and-prefix (1 "A variable that is a value and a prefix path"):
  "does not try to resolve" - a patch has to hold one. Spike keeps the later.

To check:
- tag then event is not atomic (push-before-event)
- image env with concrete paths after a move (compose)
- catalogue first fill grows with history
- catalogue event content is not checked against the configs
- Slurm 1024-byte buffer and UENV_MOUNT_LIST length
- delete/pull race message at start

Confirmed in the CLI suite (composable.bats):
- F-strict-start (6 "The rollout", 3 "Layout"): each command stops on a v1
  repository. The default repository of a user (~/.uenv/repo here) is v1, so
  `uenv run ./file.squashfs` failed, also if it needs no repository. A
  read-only site repository in v1 format blocks each user until the site
  migrates it: the user cannot. Spike: open repositories only for a lookup.
- F-nested-msg (1 "Nested mount points", todo 3): a missing nested mount
  point gives "the mount point is not a valid path: /user-environment/nope"
  from the helper: no layer name.
- F-image-env-stale (1 "The env patch of an image view uses concrete
  paths"): compose moves a relocatable layer, and accepts an image view env
  with the old path. No check, no warning.
- F-tag-then-event (5 catalogue, spike choice): a push that stops after the
  tag and before the event leaves an image that a label lookup does not find.
  A second push repairs it.
- F-migrate-hash (6 "The migration of a local repository"): the v1 sha is not
  always the digest of the squashfs (main keys new images by a manifest), so
  the migration hashes each squashfs. A resumed migration would hash again
  to find "the index that is already in the database". Spike: find the
  digest of a linked squashfs by its inode.
- F-site-policy-test (4 "Site rules for referrers"): rules only in
  /etc/uenv/config.toml: an end-to-end test needs root. Covered by unit tests.

Registry compatibility (registry:2 via UENV_TEST_REGISTRY):
- distribution 2.x: no referrers API (3 cases fail); tags/list ignores n/last
  (one response with all tags); cross-repo mount works (mounted=2 for the
  compose push). zot: HEAD finds blobs of other repos (dedupe).
- F-catalogue-first-fill: first fill = N+1 requests (N events); 0.04 s for
  50 events, 0.5 s for 400 (local zot). Refresh with no change: 1 request.
  No compaction: N grows with each push/copy/delete forever.
- F-first-event-wins: the find DB keys records by digest; a second add event
  for one digest (for example a correction) is ignored.
- test harness: zot started with unread stdout/stderr pipes blocks after
  ~64 kB of output, holding a repository lock (hang at ~208 tags). Fixed in
  v2_registry.cpp; main's oci_registry.cpp has the same pattern.
- nested-mount check: unsquashfs -l with a path filter: ~2 ms on a squashfs
  with 200 000 inodes.
- Slurm: UENV_MOUNT_LIST entry ~149 chars (v1 and v2): 1024-byte buffer
  holds 6 layers. Fixed (grow on ESPANK_NOSPACE); compile-only.
- old suites: cli.bats 4/16, registry.bats 0/5, completion.bats 16/24 pass:
  they test v1 formats, the v1 fixture and the listing service.

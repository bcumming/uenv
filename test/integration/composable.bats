# End to end tests of the composable design: add, ls, inspect, run, compose,
# rm, repo update and migrate, and push/find/pull/copy/delete against a zot
# registry. Each test names the design document and section that it checks.
#
# Run from the build directory:  ./test/bats ./test/composable.bats
#
# The tests that mount images need the setuid squashfs-mount helper (in
# STAGING_PATH/bin, or /usr/local/bin). They skip without it. The registry
# tests skip without zot.

function setup_file() {
    load ./common
    export FIX="$(mktemp -d)"
    local src="$SRC_PATH/test/setup/apptool"

    # images in the v1 layout of a squashfs file with meta/ next to it
    for name in app42 app43 tool; do
        mkdir -p "$FIX/$name"
        mksquashfs "$src/$name" "$FIX/$name/store.squashfs" -quiet -no-progress > /dev/null
        cp -R "$src/$name/meta" "$FIX/$name/"
    done

    # a relocatable layer: a tool that does not depend on its path
    mkdir -p "$FIX/reltools-src/bin"
    printf '#!/bin/sh\necho "reltools in $(dirname $(dirname $0))"\n' > "$FIX/reltools-src/bin/reltool"
    chmod +x "$FIX/reltools-src/bin/reltool"
    mksquashfs "$FIX/reltools-src" "$FIX/reltools.squashfs" -quiet -no-progress > /dev/null
    cat > "$FIX/reltools.json" <<'JSON'
{
  "description": "relocatable tools",
  "meta": {"built-on": "arapiles"},
  "mount": "/user-environment/tools",
  "relocatable": true,
  "views": {
    "tools": {"env": {"list": {"PATH": [{"op": "prepend", "value": ["@uenv-root@/bin"]}]},
                      "scalar": {"RELTOOLS_ROOT": "@uenv-root@",
                                 "RELTOOLS_CFG": "${@HOME@}/.config/reltools"}}}
  },
  "default_view": "tools"
}
JSON
    # a relocatable layer config with an absolute path: refused
    sed 's|@uenv-root@/bin|/user-environment/tools/bin|' "$FIX/reltools.json" > "$FIX/bad.json"

    # the v1 repository fixture, for the migration
    export V1_FIXTURE="$REPOS/apptool"

    # the registry
    if [[ -n "$(registry_ctl runtime)" ]]; then
        export REG_PORT="$(registry_ctl free-port)"
        export REG_STATE="$(mktemp -u)"
        registry_ctl serve "$REG_STATE" "$REG_PORT" &
        registry_ctl wait "$REG_PORT" --timeout 30
    fi
}

function teardown_file() {
    load ./common
    if [[ -n "${REG_STATE:-}" ]]; then
        registry_ctl kill "$REG_STATE" 2>/dev/null || true
    fi
    rm -rf "$FIX"
}

function setup() {
    bats_load_library bats-support
    bats_load_library bats-assert
    load ./common

    export TMP="$(mktemp -d)"
    export XDG_CONFIG_HOME="$TMP/config"
    export XDG_CACHE_HOME="$TMP/cache"
    mkdir -p "$XDG_CONFIG_HOME/uenv"
    export NS="ns${BATS_TEST_NUMBER}x$$"
    cat > "$XDG_CONFIG_HOME/uenv/config.toml" <<EOF
system_name = 'arapiles'
default_registry = 'deploy'

[[registries]]
name = 'deploy'
url = 'http://127.0.0.1:${REG_PORT:-1}/uenvtest'
namespace = '${NS}/deploy'

[[registries]]
name = 'build'
url = 'http://127.0.0.1:${REG_PORT:-1}/uenvtest'
namespace = '${NS}/build'
EOF
    export REPO="$TMP/repo"
    export PATH="$UENV_BIN_PATH:$PATH"
    unset -f uenv
    uenv repo create "$REPO"
    export R="--repo=$REPO"
}

function teardown() {
    rm -rf "$TMP"
}

# the setuid helper: STAGING_PATH, or /usr/local/bin
function require_mount() {
    local helper=""
    for p in "${STAGING_PATH:-}/bin/squashfs-mount" /usr/local/bin/squashfs-mount; do
        if [[ -u "$p" ]]; then
            helper="$p"
            break
        fi
    done
    if [[ -z "$helper" ]]; then
        skip "no setuid squashfs-mount"
    fi
    # the uenv under test, and the setuid helper (not the one in the build
    # directory, which is not setuid)
    mkdir -p "$TMP/bin"
    ln -sf "$UENV_BIN_PATH/uenv" "$TMP/bin/uenv"
    ln -sf "$helper" "$TMP/bin/squashfs-mount"
    export PATH="$TMP/bin:$PATH"
}

function require_zot() {
    if [[ -z "${REG_PORT:-}" ]]; then
        skip "no zot"
    fi
}

function add_fixtures() {
    uenv $R image add app/42.0:v1@arapiles%zen3 "$FIX/app42/store.squashfs"
    uenv $R image add tool/1.0:v1@arapiles%zen3 "$FIX/tool/store.squashfs"
    uenv $R image add +reltools/1.0:v1 "$FIX/reltools.squashfs" --config "$FIX/reltools.json"
}

function json_field() {
    python3 -c "import json,sys; j=json.load(sys.stdin); print($1)"
}

@test "1/promotion: image add makes an image with one layer" {
    run uenv $R image add app/42.0:v1@arapiles%zen3 "$FIX/app42/store.squashfs"
    assert_success
    run uenv $R image ls --no-header
    assert_success
    assert_line --partial "app/42.0:v1"
    assert_line --partial "system=arapiles,uarch=zen3"
    # the image config has the system, the layer config does not
    run bash -c "uenv $R image inspect app/42.0 --json | python3 -c 'import json,sys; j=json.load(sys.stdin); print(j[\"kind\"], j[\"meta\"], j[\"layers\"][0][\"meta\"])'"
    assert_output "image {'system': 'arapiles', 'uarch': 'zen3'} {'uarch': 'zen3'}"
}

@test "2/identity: the same file and label give the same id" {
    run uenv $R image add app/42.0:v1@arapiles%zen3 "$FIX/app42/store.squashfs"
    local first=$(echo "$output" | grep -o 'id=[0-9a-f]*')
    uenv repo create "$TMP/other"
    run uenv --repo="$TMP/other" image add app/42.0:v1@arapiles%zen3 "$FIX/app42/store.squashfs"
    assert_equal "$first" "$(echo "$output" | grep -o 'id=[0-9a-f]*')"
}

@test "3/blob store: two images of one build share the squashfs" {
    uenv $R image add app/42.0:v1@arapiles%zen3 "$FIX/app42/store.squashfs"
    uenv $R image add app/42.0:v1@santis%zen3 "$FIX/app42/store.squashfs"
    # the layer is shared: squashfs, layer config and layer manifest, the
    # empty blob, and two times (image config, wrapper, index)
    run bash -c "ls $REPO/v2/blobs/sha256 | wc -l"
    assert_output "10"
    run uenv $R image ls 'app@*' --no-header
    assert_equal "$(echo "$output" | wc -l)" "2"
    # a label that matches two images is an error that lists the ids
    run uenv $R image rm 'app/42.0:v1@*'
    assert_failure
    assert_output --partial "id="
    run uenv $R image rm app/42.0:v1@santis
    assert_success
    run bash -c "ls $REPO/v2/blobs/sha256 | wc -l"
    assert_output "7"
}

@test "5/images and layers: a layer is found only with the marker" {
    add_fixtures
    run uenv $R image ls --no-header
    refute_output --partial "reltools"
    run uenv $R image ls + --no-header
    assert_output --partial "+reltools/1.0:v1"
    assert_output --partial "building block"
    # the layers inside images are listed too
    assert_output --partial "+app/42.0:v1"
}

@test "2/configs: a relocatable layer with its mount point in a view is refused" {
    run uenv $R image add +bad/1.0:v1 "$FIX/reltools.squashfs" --config "$FIX/bad.json"
    assert_failure
    assert_output --partial "use @uenv-root@"
}

@test "1/mount points: a mount point that UENV_MOUNT_LIST cannot hold is refused" {
    # the setuid helper parses UENV_MOUNT_LIST: uenv refuses the mount point
    # when it reads the config, not when it mounts
    sed 's|"mount": "/user-environment/tools"|"mount": "/user env/tools"|' \
        "$FIX/reltools.json" > "$TMP/space.json"
    run uenv $R image add +space/1.0:v1 "$FIX/reltools.squashfs" --config "$TMP/space.json"
    assert_failure
    assert_output --partial "the mount point '/user env/tools' is not valid"
    # the command line, and a compose file
    add_fixtures
    run uenv $R run +reltools:/opt/a+b -- true
    assert_failure
    cat > "$TMP/c.json" <<'EOF'
{"name": "x", "version": "1.0", "release": "v1", "meta": {}, "views": {},
 "layers": [{"from": "+reltools", "mount": "/opt/tools:2"}]}
EOF
    run uenv $R image compose "$TMP/c.json"
    assert_failure
    assert_output --partial "not valid"
}

@test "1/concretisation: run an image with a view" {
    require_mount
    add_fixtures
    run uenv $R run tool --view=tool -- bash -c 'echo "$PATH|$TOOLVAR|$UENV_VIEW"'
    assert_success
    assert_output --regexp "^/user-tools/env/tool/bin:.*\|SET\|tool$"
    # the squashfs is mounted from the blob store
    run uenv $R run tool -- bash -c 'echo $UENV_MOUNT_LIST'
    assert_output --regexp "/v2/blobs/sha256/[0-9a-f]{64}:/user-tools$"
    # uenv status reads the session
    run uenv $R run tool --view=tool -- uenv status
    assert_success
    assert_output --partial "tool"
}

@test "1/concretisation: the default view, and --no-default-view" {
    require_mount
    add_fixtures
    # the default view of tool is defaultwombat
    run uenv $R run tool -- bash -c 'echo "$WOMBAT|$UENV_VIEW"'
    assert_success
    assert_output "soup|defaultwombat"
    run uenv $R run tool --no-default-view -- bash -c 'echo "$WOMBAT|$UENV_VIEW"'
    assert_success
    assert_output "|"
}

@test "1/runtime merge: start a list of an image and a relocatable layer" {
    require_mount
    add_fixtures
    # the relocatable layer moves to /user-tools
    run uenv $R run app/42.0,+reltools:/user-tools -- bash -c 'reltool; echo "$RELTOOLS_ROOT|$UENV_VIEW"'
    assert_success
    assert_line "reltools in /user-tools"
    assert_line --regexp "^/user-tools\|.*reltools:tools"
    # ${@HOME@} reads the calling environment
    run uenv $R run +reltools:/user-tools -- bash -c 'echo $RELTOOLS_CFG'
    assert_output "$HOME/.config/reltools"
}

@test "1/runtime merge: an alias lets two items with the same name start" {
    require_mount
    add_fixtures
    run uenv $R run a=+reltools:/user-tools,b=+reltools:/user-environment --view=a:tools,b:tools -- bash -c 'echo $PATH'
    assert_success
    # b is applied last: it is first in PATH
    assert_output --regexp "^/user-environment/bin:/user-tools/bin:"
}

@test "1/load more than one view: the order is important" {
    require_mount
    add_fixtures
    run uenv $R run app/42.0,+reltools:/user-tools --view=app:app,reltools:tools -- bash -c 'echo $PATH'
    assert_success
    [[ "$output" == /user-tools/bin:* ]]
    run uenv $R run app/42.0,+reltools:/user-tools --view=reltools:tools,app:app -- bash -c 'echo $PATH'
    assert_success
    [[ "$output" == /user-environment/env/app/bin:* ]]
}

@test "1/runtime merge: only a layer can move" {
    add_fixtures
    run uenv $R inspect app/42.0:/user-tools,tool
    assert_failure
    assert_output --partial "only a layer"
}

@test "1/raise: a layer that is not relocatable needs --force to move" {
    require_mount
    add_fixtures
    run uenv $R run +tool:/user-environment -- true
    assert_failure
    assert_output --partial "not relocatable"
    run uenv $R run --force +tool:/user-environment -- bash -c 'ls /user-environment'
    assert_success
    assert_output --partial "env"
}

@test "1/runtime merge: an image with more than one layer" {
    require_mount
    add_fixtures
    cat > "$TMP/c.json" <<'EOF'
{"name": "my-env", "version": "1.0", "release": "v1", "meta": {},
 "views": {"dev": {"include": ["app:app", "reltools:tools"]}}, "default_view": "dev",
 "layers": [{"from": "app/42.0:v1#app"},
            {"from": "+reltools", "mount": "/user-environment/modules"}]}
EOF
    uenv $R image compose "$TMP/c.json"
    # a runtime merge has no digest, and keeps the views of each item
    run uenv $R inspect --json my-env,tool
    assert_success
    run json_field '"|".join([j["kind"], j["digest"], str(len(j["layers"])), ",".join(sorted(j["views"]))])' <<< "$output"
    assert_output "runtime merge||3|my-env:dev,tool:defaultwombat,tool:modules,tool:spack,tool:tool,tool:wombat"
    # the layer app of my-env and the image app/42.0 are one layer
    run uenv $R inspect --json my-env,app/42.0
    assert_success
    run json_field 'len(j["layers"])' <<< "$output"
    assert_output "2"
    # a view name with no item: the one item that has the view
    run uenv $R run my-env,tool --view=dev,tool -- bash -c 'reltool; echo "$TOOLVAR|$UENV_VIEW"'
    assert_success
    assert_line "reltools in /user-environment/modules"
    assert_line "SET|dev,tool"
}

@test "1/compose: an image from a layer of an image and a building block" {
    require_mount
    add_fixtures
    cat > "$TMP/c.json" <<'EOF'
{"name": "my-env", "version": "1.0", "release": "v1", "meta": {"system": "arapiles"},
 "views": {"dev": {"include": ["app:app", "reltools:tools"],
                   "env": {"scalar": {"MY_TOOLS_ROOT": "/user-environment/modules"}}}},
 "default_view": "dev",
 "layers": [{"from": "app/42.0:v1#app"},
            {"from": "+reltools", "mount": "/user-environment/modules"}]}
EOF
    run uenv $R image compose "$TMP/c.json"
    assert_success
    local id1=$(echo "$output" | grep -o 'id=[0-9a-f]*')
    # a nested mount point: the parent layer has the directory
    run uenv $R run my-env -- bash -c 'reltool; echo "$MY_TOOLS_ROOT|$UENV_VIEW"'
    assert_success
    assert_line "reltools in /user-environment/modules"
    assert_line "/user-environment/modules|dev"
    # compose is reproducible
    run uenv $R image compose "$TMP/c.json"
    assert_equal "$id1" "$(echo "$output" | grep -o 'id=[0-9a-f]*')"
}

@test "1/nested mount points: a missing directory is found by compose" {
    add_fixtures
    cat > "$TMP/c.json" <<'EOF'
{"name": "nest", "version": "1.0", "release": "v1", "meta": {}, "views": {},
 "layers": [{"from": "app/42.0:v1#app"},
            {"from": "+reltools", "mount": "/user-environment/nope"}]}
EOF
    # compose checks the directory with unsquashfs, so that the error names
    # the layers
    run uenv $R image compose "$TMP/c.json"
    assert_failure
    assert_output --partial "the layer 'reltools' is mounted at /user-environment/nope"
    assert_output --partial "'app' has no directory /nope"
}

@test "1/nested mount points: a runtime merge with a missing directory" {
    add_fixtures
    # a merge is checked as compose is, before the helper runs
    run uenv $R run app/42.0,+reltools:/user-environment/nope -- true
    assert_failure
    assert_output --partial "the layer 'reltools' is mounted at /user-environment/nope"
    assert_output --partial "'app' has no directory /nope"
}

@test "1/compose: a warning for the old path of a moved layer" {
    add_fixtures
    # the author moves reltools, and does not change MY_TOOLS_ROOT
    cat > "$TMP/c.json" <<'EOF'
{"name": "stale", "version": "1.0", "release": "v1", "meta": {},
 "views": {"dev": {"include": ["reltools:tools"],
                   "env": {"scalar": {"MY_TOOLS_ROOT": "/user-environment/tools"}}}},
 "default_view": "dev",
 "layers": [{"from": "+reltools", "mount": "/user-tools"}]}
EOF
    run uenv $R image compose "$TMP/c.json"
    assert_success
    assert_output --partial "sets MY_TOOLS_ROOT with the path /user-environment/tools"
    # no warning when the path is correct
    sed -i 's|"MY_TOOLS_ROOT": "/user-environment/tools"|"MY_TOOLS_ROOT": "/user-tools"|' "$TMP/c.json"
    run uenv $R image compose "$TMP/c.json"
    refute_output --partial "warning"
}

@test "6/a squashfs file: start it directly" {
    require_mount
    run uenv run "$FIX/tool/store.squashfs:/user-tools" --view=tool -- bash -c 'echo $TOOLVAR'
    assert_success
    assert_output "SET"
}

@test "3/start: a missing squashfs gives a clear error" {
    add_fixtures
    local sqfs=$(uenv $R image inspect tool --json | json_field 'j["layers"][0]["squashfs"]')
    rm "$sqfs"
    run uenv $R run tool -- true
    assert_failure
    assert_output --partial "uenv repo update"
    run uenv repo status "$REPO"
    assert_output --partial "missing blobs: tool/1.0:v1"
    run uenv repo update "$REPO"
    assert_success
    run uenv $R image ls tool --no-header
    refute_output --partial "tool/1.0"
}

@test "3/oci image layout: index.json after update, readable by oras" {
    if ! command -v oras > /dev/null; then
        skip "no oras"
    fi
    add_fixtures
    uenv repo update "$REPO"
    run oras repo tags --oci-layout "$REPO/v2"
    assert_success
    echo "# oras tags: $(echo $output)" >&3
    local tag=$(echo "$output" | head -1)
    run oras manifest fetch --oci-layout "$REPO/v2:$tag"
    assert_success
}

@test "2/canonical: configs match a reference canonical form" {
    add_fixtures
    run python3 -I - "$REPO/v2/blobs/sha256" <<'PY'
import json, os, sys
d = sys.argv[1]
n = 0
for f in os.listdir(d):
    b = open(os.path.join(d, f), 'rb').read()
    if not b.startswith(b'{'):
        continue
    j = json.loads(b)
    # JCS for objects with ASCII keys, strings and integers
    ref = json.dumps(j, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()
    if ref != b:
        print("mismatch", f, b[:80])
        sys.exit(1)
    n += 1
print("checked", n)
PY
    assert_success
    assert_output --regexp "checked [0-9]+"
}

@test "3/reads and writes: an add that stops leaves no image" {
    run env UENV_TEST_FAULT=record-before-db uenv $R image add app/42.0:v1@arapiles%zen3 "$FIX/app42/store.squashfs"
    assert_equal "$status" "99"
    run uenv $R image ls --no-header
    assert_output ""
    run uenv repo status "$REPO" --json
    assert_output --partial '"recent":7'
    run uenv repo update --force "$REPO"
    assert_success
    run bash -c "ls $REPO/v2/blobs/sha256 | wc -l"
    assert_output "0"
}

@test "6/migration: an image with meta/ only inside the squashfs" {
    cp -a "$V1_FIXTURE" "$TMP/v1repo"
    # v1 reads meta/env.json from the squashfs when meta/ is not next to it
    rm -rf "$TMP"/v1repo/images/*/meta
    run uenv repo migrate "$TMP/v1repo"
    assert_success
    run uenv --repo="$TMP/v1repo" image ls --no-header
    assert_line --partial "app/42.0:v1"
    assert_line --partial "tool/17.3.2:v1"
}

@test "6/migration: a v1 repository is refused, then migrated" {
    require_mount
    cp -a "$V1_FIXTURE" "$TMP/v1repo"
    local before=$(sha256sum < "$TMP/v1repo/index.db")
    run uenv --repo="$TMP/v1repo" image ls
    assert_failure
    assert_output --partial "uenv repo migrate"
    run uenv repo migrate "$TMP/v1repo"
    assert_success
    assert_output --partial "migrated 3 images"
    run uenv --repo="$TMP/v1repo" image ls --no-header
    assert_line --partial "app/42.0:v1"
    assert_line --partial "tool/17.3.2:v1"
    # the v1 part is unchanged
    assert_equal "$(sha256sum < "$TMP/v1repo/index.db")" "$before"
    # no data is copied: a hard link
    local sqfs=$(ls "$TMP/v1repo/images/"*/store.squashfs | head -1)
    assert_equal "$(stat -c %h "$sqfs")" "2"
    run uenv --repo="$TMP/v1repo" run tool -- bash -c 'echo $UENV_VIEW'
    assert_success
}

@test "6/migration: a migration that stops can continue" {
    cp -a "$V1_FIXTURE" "$TMP/v1repo"
    run env UENV_TEST_FAULT=migrate-after-link uenv repo migrate "$TMP/v1repo"
    assert_equal "$status" "99"
    # the v2 part is usable at once: the stop left no image in it
    run uenv --repo="$TMP/v1repo" image ls --no-header
    assert_success
    run uenv repo status "$TMP/v1repo"
    assert_output --partial "3 v1 images are not migrated"
    run uenv repo migrate "$TMP/v1repo"
    assert_success
    # the squashfs that the stopped migration linked is not hashed again
    assert_output --partial "migrated 3 images (0 already done, 2 squashfs hashed)"
}

@test "6/migration: a v2.tmp of an earlier migration is used" {
    cp -a "$V1_FIXTURE" "$TMP/v1repo"
    uenv repo migrate "$TMP/v1repo"
    mv "$TMP/v1repo/v2" "$TMP/v1repo/v2.tmp"
    run uenv repo migrate "$TMP/v1repo"
    assert_success
    assert_output --partial "migrated 0 images (3 already done"
    [[ -d "$TMP/v1repo/v2" && ! -e "$TMP/v1repo/v2.tmp" ]]
}

@test "6/migration: a failed image does not stop the others" {
    cp -a "$V1_FIXTURE" "$TMP/v1repo"
    run uenv repo status "$TMP/v1repo"
    assert_output --partial "v1 format, 3 images"
    # one squashfs that uenv cannot read
    local bad=$(ls "$TMP"/v1repo/images/*/store.squashfs | head -1)
    chmod 000 "$bad"
    run uenv repo migrate "$TMP/v1repo"
    chmod 644 "$bad"
    assert_failure
    assert_output --partial "migrated 2 images"
    assert_output --partial "1 images were not migrated. The other images can be used."
    # the migrated images can be used
    run uenv --repo="$TMP/v1repo" image ls --no-header
    assert_success
    assert_equal "$(echo "$output" | grep -c 'v1')" "2"
    run uenv repo status "$TMP/v1repo"
    assert_output --partial "1 v1 images are not migrated"
    # a later migration adds only the image that is not migrated
    run uenv repo migrate "$TMP/v1repo"
    assert_success
    assert_output --partial "migrated 1 images (2 already done, 1 squashfs hashed)"
    run uenv repo status "$TMP/v1repo"
    refute_output --partial "not migrated"
    run uenv repo migrate "$TMP/v1repo"
    assert_success
    assert_output --partial "migrated 0 images (3 already done, 0 squashfs hashed)"
}

#
# registry
#

@test "4/push, find and pull an image" {
    require_zot
    add_fixtures
    run uenv $R image push app/42.0:v1
    assert_success
    assert_output --partial "/deploy/app:42.0-v1-arapiles-zen3-"
    run uenv image find app --no-header
    assert_success
    assert_output --partial "app/42.0:v1"
    uenv repo create "$TMP/r2"
    run uenv --repo="$TMP/r2" image pull app/42.0:v1
    assert_success
    assert_output --partial "1 squashfs downloaded"
    run uenv --repo="$TMP/r2" image ls --no-header
    assert_output --partial "app/42.0:v1"
    # the same id in both repositories
    local a=$(uenv $R image ls app --json | json_field 'j[0]["id"]')
    local b=$(uenv --repo="$TMP/r2" image ls app --json | json_field 'j[0]["id"]')
    assert_equal "$a" "$b"
}

@test "4/compose and push: the squashfs of a building block is not uploaded again" {
    require_zot
    add_fixtures
    uenv $R image push +reltools/1.0:v1
    uenv $R image push app/42.0:v1
    cat > "$TMP/c.json" <<'EOF'
{"name": "my-env", "version": "1.0", "release": "v1", "meta": {"system": "arapiles"},
 "views": {"dev": {"include": ["app:app", "reltools:tools"]}}, "default_view": "dev",
 "layers": [{"from": "app/42.0:v1#app"}, {"from": "+reltools", "mount": "/user-environment/modules"}]}
EOF
    uenv $R image compose "$TMP/c.json"
    run uenv $R image push my-env
    assert_success
    echo "# $output" >&3
    # two small blobs: the image config and the empty blob. No squashfs.
    assert_output --regexp "\((0|1|2) uploaded"
    uenv repo create "$TMP/r2"
    run uenv --repo="$TMP/r2" image pull my-env
    assert_success
    run uenv --repo="$TMP/r2" image inspect my-env
    assert_output --partial "/user-environment/modules"
}

@test "4/copy: deploy an image, and deploy it on one more system" {
    require_zot
    add_fixtures
    uenv $R image push app/42.0:v1 build::
    run uenv image find build::app --no-header
    assert_output --partial "app/42.0:v1"
    run uenv image copy build::app/42.0:v1 deploy::
    assert_success
    run uenv image find app --no-header
    assert_output --partial "system=arapiles"
    run uenv image copy build::app/42.0:v1 deploy:: --set meta.system=santis
    assert_success
    run uenv image find 'app@*' --no-header
    assert_output --partial "system=santis"
    assert_output --partial "system=arapiles"
}

@test "4/delete: the image is no longer found" {
    require_zot
    add_fixtures
    uenv $R image push tool
    run uenv image delete tool/1.0:v1
    assert_success
    run uenv image find tool --no-header
    refute_output --partial "tool/1.0"
    uenv repo create "$TMP/r2"
    run uenv --repo="$TMP/r2" image pull tool
    assert_failure
}

@test "4/pull: a pull that stops before the database" {
    require_zot
    add_fixtures
    uenv $R image push app/42.0:v1
    uenv repo create "$TMP/r2"
    run env UENV_TEST_FAULT=pull-before-db uenv --repo="$TMP/r2" image pull app/42.0:v1
    assert_equal "$status" "99"
    run uenv --repo="$TMP/r2" image ls --no-header
    assert_output ""
    # a second pull downloads no squashfs
    run uenv --repo="$TMP/r2" image pull app/42.0:v1
    assert_success
    assert_output --partial "0 squashfs downloaded, 1 already present"
}

@test "5/catalogue: a push that stops after the tag is not in the catalogue" {
    require_zot
    add_fixtures
    run env UENV_TEST_FAULT=push-before-event uenv $R image push tool
    assert_equal "$status" "99"
    # FINDING (tag-then-event): the image is in the registry, with its tag,
    # but a lookup by label does not find it
    run uenv image find tool --no-header
    refute_output --partial "tool/1.0"
    # a second push repairs it: the tag check accepts the same digest
    run uenv $R image push tool
    assert_success
    run uenv image find tool --no-header
    assert_output --partial "tool/1.0"
}

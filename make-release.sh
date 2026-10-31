#!/usr/bin/env bash
#
# make-release.sh — build the binaries for a tagged release, reproducibly.
#
#   ./make-release.sh v1.4.3                          # build assets only
#   ./make-release.sh v1.4.3 --publish notes.md       # ...and create the release
#
# Everything happens in the official ESP-IDF Docker image, from a scrubbed
# build tree: sdkconfig and build/ are deleted first, so the release can never
# ship binaries carrying settings from an older sdkconfig (which overrides
# sdkconfig.defaults — see README "Building"). That is the whole point of this
# script; building by hand in a tree you have been working in is how a release
# ends up with the wrong controller mode compiled in.
#
# Output lands in build/release/, owned by you rather than by root.

set -euo pipefail

IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.4.4}"
BTSTACK_TAG="v1.8.2"

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
say() { printf '\033[36m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- arguments

VERSION="${1:-}"
[ -n "$VERSION" ] || die "usage: $0 vX.Y.Z [--publish <notes.md>] [--allow-dirty]"
shift

[[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
    die "version must look like v1.4.3, got '$VERSION'"

PUBLISH=0
NOTES=""
ALLOW_DIRTY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --publish)
            PUBLISH=1
            NOTES="${2:-}"
            [ -n "$NOTES" ] || die "--publish needs a release-notes file"
            [ -f "$NOTES" ] || die "release-notes file not found: $NOTES"
            shift 2
            ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        *) die "unknown argument: $1" ;;
    esac
done

# ------------------------------------------------------------- sanity checks

cd "$(dirname "$0")"
ROOT="$PWD"

[ -f "$ROOT/main/gateway.c" ] || die "run this from the vetera-bridge checkout"
command -v docker >/dev/null || die "docker not found"

[ -d "$ROOT/components/btstack/src" ] || die \
"components/btstack is missing — it is not vendored. Fetch it once:

    git clone --depth 1 -b $BTSTACK_TAG https://github.com/bluekitchen/btstack.git components/btstack"

if [ -n "$(git status --porcelain)" ]; then
    if [ "$ALLOW_DIRTY" -eq 1 ]; then
        say "working tree is dirty — continuing because --allow-dirty was given"
    else
        git status --short
        die "working tree is dirty; commit or stash first (or pass --allow-dirty)"
    fi
fi

# The tag should already exist and point at what is checked out, so the
# binaries and the tag cannot disagree.
if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null; then
    if [ "$(git rev-parse "$VERSION^{commit}")" != "$(git rev-parse HEAD)" ]; then
        die "tag $VERSION does not point at HEAD — check out the tag first"
    fi
else
    say "note: tag $VERSION does not exist yet; create it before publishing:"
    printf '      git tag -a %s -m "%s" && git push origin %s\n' \
        "$VERSION" "$VERSION" "$VERSION"
fi

if [ "$PUBLISH" -eq 1 ]; then
    command -v gh >/dev/null || die "gh not found, needed for --publish"
    gh auth status >/dev/null 2>&1 || die "gh is not authenticated (gh auth login)"
    git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null || \
        die "cannot publish: tag $VERSION does not exist"
fi

# ---------------------------------------------------------------- the build

say "building $VERSION from a clean tree with $IDF_IMAGE"

# Runs as root inside the container, which is also what lets it delete the
# root-owned sdkconfig and build/ a previous Docker build left behind.
docker run --rm -v "$ROOT":/project -w /project "$IDF_IMAGE" bash -euc '
V="'"$VERSION"'"
HOST_UID='"$(id -u)"'
HOST_GID='"$(id -g)"'

rm -rf /project/build /project/sdkconfig /project/sdkconfig.old

# The checkout belongs to the host user, not root, so git refuses to read it
# ("dubious ownership") and ESP-IDF falls back to stamping the app version
# as "1". Every release up to v1.5.1 shipped like that.
git config --global --add safe.directory /project

idf.py set-target esp32
idf.py build

OUT=/project/build/release
mkdir -p "$OUT"
cd /project/build

cp vetera-bridge.bin                    "$OUT/vetera-bridge-$V-app.bin"
cp bootloader/bootloader.bin            "$OUT/vetera-bridge-$V-bootloader.bin"
cp partition_table/partition-table.bin  "$OUT/vetera-bridge-$V-partition-table.bin"

# Single image for the web flasher and for esptool at offset 0x0. The layout
# must match build/flash_args, which is what idf.py itself would flash.
python -m esptool --chip esp32 merge_bin \
    -o "$OUT/vetera-bridge-$V-full-flash.bin" \
    --flash_mode dio --flash_freq 40m --flash_size 2MB \
    0x1000  bootloader/bootloader.bin \
    0x8000  partition_table/partition-table.bin \
    0x10000 vetera-bridge.bin

cd "$OUT"
sha256sum vetera-bridge-$V-*.bin > SHA256SUMS.txt
chown -R "$HOST_UID:$HOST_GID" /project/build /project/sdkconfig
'

# ----------------------------------------------------------- verify & report

OUT="$ROOT/build/release"
ASSETS=(
    "$OUT/vetera-bridge-$VERSION-app.bin"
    "$OUT/vetera-bridge-$VERSION-bootloader.bin"
    "$OUT/vetera-bridge-$VERSION-partition-table.bin"
    "$OUT/vetera-bridge-$VERSION-full-flash.bin"
    "$OUT/SHA256SUMS.txt"
)
for f in "${ASSETS[@]}"; do
    [ -s "$f" ] || die "expected asset missing or empty: $f"
done

# The controller mode is the setting that silently reverts on a stale
# sdkconfig, and the one whose absence cost four releases of stability.
grep -q '^CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y$' "$ROOT/sdkconfig" || \
    die "generated sdkconfig is not in BR/EDR-only mode — refusing to release"

# The version the firmware reports at boot comes from `git describe`, read
# from esp_app_desc_t (magic 0xABCD5432 right after the first segment
# header, version string 16 bytes in). It only equals the tag when the tag
# is on HEAD and no tracked file is modified.
APP_VER="$(python3 - "$OUT/vetera-bridge-$VERSION-app.bin" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
if struct.unpack_from("<I", d, 0x20)[0] != 0xABCD5432:
    sys.exit("app descriptor not found")
print(d[0x30:0x50].split(b"\0")[0].decode())
PY
)" || die "could not read the app version from the built image"
if [ "$APP_VER" != "$VERSION" ]; then
    [ "$PUBLISH" -eq 1 ] && \
        die "image reports version '$APP_VER', not '$VERSION' — refusing to release"
    say "warning: image reports version '$APP_VER', not '$VERSION'"
fi

say "assets in build/release:"
( cd "$OUT" && ls -1sh vetera-bridge-"$VERSION"-*.bin SHA256SUMS.txt )
echo
cat "$OUT/SHA256SUMS.txt"
echo

if [ "$PUBLISH" -eq 1 ]; then
    say "creating GitHub release $VERSION"
    gh release create "$VERSION" \
        --title "$VERSION" \
        --notes-file "$NOTES" \
        "${ASSETS[@]}"
else
    say "not published. To publish, write the notes and run:"
    printf '      %s %s --publish <notes.md>\n' "$0" "$VERSION"
fi

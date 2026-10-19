#!/usr/bin/env bash
# Packages a macOS Release build of Sidecar as a disk image.
#
#   packaging/package-macos.sh <Sidecar.app> <x86_64 Sidecar.app or ""> <version> <output dir>
#
# With a second (x86_64) build, its executable is merged into the first with lipo,
# making a universal (arm64 + x86_64) app. The app is signed ad hoc (it isn't
# notarized, so the first launch needs right-click > Open) and written to
# <output dir>/Sidecar-<version>-macOS.dmg with an /Applications link.
set -euo pipefail

if [ $# -ne 4 ]; then
    echo "usage: $0 <Sidecar.app> <x86_64 Sidecar.app or \"\"> <version> <output dir>" >&2
    exit 2
fi

app=$1
other=$2
version=$3
mkdir -p "$4"
out=$(cd "$4" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)
name="Sidecar-$version-macOS"

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

ditto "$app" "$stage/Sidecar.app"
bin="$stage/Sidecar.app/Contents/MacOS/Sidecar"

if [ -n "$other" ]; then
    lipo -create "$bin" "$other/Contents/MacOS/Sidecar" -output "$bin.universal"
    mv "$bin.universal" "$bin"
fi
lipo -info "$bin"

codesign --force --deep --sign - "$stage/Sidecar.app"
codesign --verify --deep --strict --verbose=2 "$stage/Sidecar.app"

for f in README.md LICENSE THIRD_PARTY_NOTICES.md CHANGELOG.md; do
    if [ -f "$root/$f" ]; then cp "$root/$f" "$stage/"; fi
done
cp "$root/resources/fonts/OFL.txt" "$stage/FONT-LICENSE.txt"
cp "$root/packaging/macos/README-MACOS.txt" "$stage/"
ln -s /Applications "$stage/Applications"

# hdiutil sometimes fails with "Resource busy" on CI machines; retry.
for attempt in 1 2 3 4 5; do
    if hdiutil create -volname "Sidecar $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$out/$name.dmg"; then
        echo "wrote $out/$name.dmg"
        exit 0
    fi
    echo "hdiutil failed (attempt $attempt), retrying" >&2
    sleep $((attempt * 5))
done
exit 1

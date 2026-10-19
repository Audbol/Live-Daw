#!/usr/bin/env bash
# Packages a Linux Release build of Sidecar.
#
#   packaging/package-linux.sh <build dir> <version> <output dir>
#
# Writes <output dir>/Sidecar-<version>-Linux-x86_64.tar.gz and, when APPIMAGETOOL
# (path to appimagetool) and APPIMAGE_RUNTIME (path to the type2 runtime) are set,
# <output dir>/Sidecar-<version>-Linux-x86_64.AppImage.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <build dir> <version> <output dir>" >&2
    exit 2
fi

build=$1
version=$2
mkdir -p "$3"
out=$(cd "$3" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)
exe="$build/LiveDAW_artefacts/Release/Sidecar"
name="Sidecar-$version-Linux-x86_64"

[ -x "$exe" ] || { echo "no Release build at $exe" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

# Docs and licences that go into every package. LICENSE and THIRD_PARTY_NOTICES.md
# are optional so older commits still package.
copy_docs() {
    local dest=$1 f
    for f in README.md LICENSE THIRD_PARTY_NOTICES.md CHANGELOG.md; do
        if [ -f "$root/$f" ]; then cp "$root/$f" "$dest/"; fi
    done
    cp "$root/resources/fonts/OFL.txt" "$dest/FONT-LICENSE.txt"
    if [ -d "$root/licenses" ]; then cp -R "$root/licenses" "$dest/"; fi
    cp "$root/packaging/linux/README-LINUX.txt" "$dest/"
}

#--- tarball ------------------------------------------------------------------
dir="$stage/$name"
mkdir -p "$dir"
install -m 755 "$exe" "$dir/Sidecar"
strip --strip-unneeded "$dir/Sidecar"
install -m 644 "$root/packaging/linux/sidecar.desktop" "$dir/sidecar.desktop"
install -m 644 "$root/resources/icon/sidecar-256.png" "$dir/sidecar.png"
install -m 644 "$root/resources/icon/sidecar.svg" "$dir/sidecar.svg"
copy_docs "$dir"
tar -C "$stage" --owner=0 --group=0 -czf "$out/$name.tar.gz" "$name"
echo "wrote $out/$name.tar.gz"

#--- AppImage -----------------------------------------------------------------
if [ -n "${APPIMAGETOOL:-}" ]; then
    appdir="$stage/Sidecar.AppDir"
    mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/doc/sidecar" \
             "$appdir/usr/share/icons/hicolor/scalable/apps"
    install -m 755 "$dir/Sidecar" "$appdir/usr/bin/Sidecar"
    install -m 644 "$root/packaging/linux/sidecar.desktop" "$appdir/usr/share/applications/sidecar.desktop"
    install -m 644 "$root/packaging/linux/sidecar.desktop" "$appdir/sidecar.desktop"
    for size in 16 32 48 64 128 256 512; do
        mkdir -p "$appdir/usr/share/icons/hicolor/${size}x${size}/apps"
        install -m 644 "$root/resources/icon/sidecar-$size.png" "$appdir/usr/share/icons/hicolor/${size}x${size}/apps/sidecar.png"
    done
    install -m 644 "$root/resources/icon/sidecar.svg" "$appdir/usr/share/icons/hicolor/scalable/apps/sidecar.svg"
    install -m 644 "$root/resources/icon/sidecar-256.png" "$appdir/sidecar.png"
    ln -s sidecar.png "$appdir/.DirIcon"
    copy_docs "$appdir/usr/share/doc/sidecar"
    cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/Sidecar" "$@"
EOF
    chmod 755 "$appdir/AppRun"

    # Extract-and-run: CI runners (and containers) usually have no FUSE.
    runtime_args=()
    if [ -n "${APPIMAGE_RUNTIME:-}" ]; then runtime_args=(--runtime-file "$APPIMAGE_RUNTIME"); fi
    ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" --no-appstream "${runtime_args[@]}" \
        "$appdir" "$out/$name.AppImage"
    chmod 755 "$out/$name.AppImage"
    echo "wrote $out/$name.AppImage"
fi

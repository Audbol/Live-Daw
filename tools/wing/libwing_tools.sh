#!/bin/sh
# Builds libwing's command-line clients (wingdrive, wingprop, wingmon) and our
# oscprobe (tools/wing/oscprobe.rs, which uses libwing's OSC client) into
# DIR/tools/target/release, for testing the Sidecar WING remote against them:
#
#   tools/wing/libwing_tools.sh DIR [REPO] [REV]
#   LIBWING_TOOLS=DIR/tools/target/release cargo test -p livedaw-wing --test libwing
#
# libwing is cloned into DIR; nothing from it is copied into this repository.
set -eu
DIR=${1:?usage: libwing_tools.sh DIR [REPO] [REV]}
REPO=${2:-https://github.com/jerrythemaus/libwing.git}
REV=${3:-410cc1596384ea02d2fef9393c4da2bce59d9ad7}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
[ -d "$DIR/libwing/.git" ] || git clone --quiet "$REPO" "$DIR/libwing"
git -C "$DIR/libwing" checkout --quiet "$REV"
# libwing's tools are Cargo examples whose dev-dependencies pull in a GUI
# toolkit; a small wrapper crate next to the checkout builds just the clients.
mkdir -p "$DIR/tools"
cat > "$DIR/tools/Cargo.toml" <<EOF
[package]
name = "libwing-tools"
version = "0.1.0"
edition = "2021"
autobins = false

[dependencies]
libwing = { path = "../libwing" }
jzon = "0.12.5"

[[bin]]
name = "wingdrive"
path = "../libwing/tools/wingdrive.rs"

[[bin]]
name = "wingprop"
path = "../libwing/tools/wingprop.rs"

[[bin]]
name = "wingmon"
path = "../libwing/tools/wingmon.rs"

[[bin]]
name = "oscprobe"
path = "$HERE/oscprobe.rs"

[workspace]
EOF
cargo build --quiet --release --manifest-path "$DIR/tools/Cargo.toml"
echo "LIBWING_TOOLS=$DIR/tools/target/release"

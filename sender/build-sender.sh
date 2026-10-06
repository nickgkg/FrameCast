#!/bin/bash
# Build framecast_sender (Open Screen Cast Streaming sender) for the Steam Frame (linux arm64) on an
# x86-64 Linux PC, and copy it to ../bin/framecast_sender. install.sh pushes it to the Frame.
#
# The Open Screen tree must be on a Linux file system (ext4): the Chromium sysroots contain file
# names that NTFS/exFAT do not accept. It needs about 4 GB. Default: ~/src/openscreen-build.
#
# The tree is upstream Open Screen at the pin of github.com/gianlucamazza/cast (its setup script
# applies that project's sender patch, which only adds files we do not build), plus
# openscreen-build.patch (adds our target to the build and to the visibility lists).
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
WORK="${WORK:-$HOME/src/openscreen-build}"
CAST_REF_COMMIT=9e1b1260ce5119af0b12cc47403480b4896e6627
OS="$WORK/openscreen"

mkdir -p "$WORK"
if [ ! -d "$WORK/cast-ref" ]; then
    git clone https://github.com/gianlucamazza/cast.git "$WORK/cast-ref"
    git -C "$WORK/cast-ref" checkout --detach "$CAST_REF_COMMIT"
fi
if [ ! -d "$OS/third_party/abseil" ]; then
    # Clones depot_tools and Open Screen, applies the reference patch, runs gclient sync.
    # Its last step (gn gen with ffmpeg/SDL args) may fail; that is not needed here.
    (cd "$WORK" && OPENSCREEN_DIR="$OS" bash cast-ref/native/integration/setup-openscreen.sh --from-upstream) || true
fi
[ -d "$OS/third_party/abseil" ] || { echo "Open Screen checkout failed" >&2; exit 1; }

ln -sfn "$HERE" "$OS/cast/framecast_sender"
if ! grep -q framecast_sender "$OS/BUILD.gn"; then
    git -C "$OS" apply "$HERE/openscreen-build.patch"
fi
[ -d "$OS/build/linux/debian_bullseye_arm64-sysroot" ] ||
    python3 "$OS/build/linux/sysroot_scripts/install-sysroot.py" --arch=arm64

cd "$OS"
buildtools/linux64/gn gen out/arm64 \
    --args='is_debug=false use_sysroot=true target_os="linux" target_cpu="arm64"'
third_party/ninja/ninja -C out/arm64 framecast_sender
mkdir -p "$HERE/../bin"
cp out/arm64/framecast_sender "$HERE/../bin/framecast_sender"
echo "built $HERE/../bin/framecast_sender"

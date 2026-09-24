#!/bin/sh
#
# build-linux-tarball.sh VERSION [OUTDIR]
#
# Builds the desktop engine (SOUND=alsa) and packs it with the launcher, the
# menu entry and install.sh into quake-linux-x86_64-VERSION.tar.gz.
#
# The binary runs on distributions with the same C library as the machine it
# is built on or newer, so the release is built on an old one: see the Linux
# job in .github/workflows/docker-publish.yml.
#

set -eu

version="${1:?usage: build-linux-tarball.sh VERSION [OUTDIR]}"
out="${2:-.}"
here=$(cd "$(dirname "$0")/.." && pwd)
arch=$(uname -m)
name="quake-linux-$arch-$version"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

make -C "$here/WinQuake" BUILDDIR=linux-desktop SOUND=alsa clean >/dev/null
make -C "$here/WinQuake" BUILDDIR=linux-desktop SOUND=alsa -j"$(nproc)"
strip "$here/WinQuake/linux-desktop/xquake"

root="$stage/$name"
install -d "$root/bin" "$root/lib/quake" "$root/share/applications" \
           "$root/share/icons/hicolor/48x48/apps"
install -m 755 "$here/WinQuake/linux-desktop/xquake" "$root/lib/quake/xquake"
install -m 755 "$here/desktop/quake" "$root/bin/quake"
install -m 644 "$here/desktop/quake.desktop" "$root/share/applications/"
install -m 644 "$here/desktop/quake.png" "$root/share/icons/hicolor/48x48/apps/"
install -m 755 "$here/desktop/install.sh" "$root/install.sh"
install -m 644 "$here/desktop/README.txt" "$root/README.txt"
install -m 644 "$here/gnu.txt" "$root/COPYING"

mkdir -p "$out"
tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
echo "$out/$name.tar.gz"

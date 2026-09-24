#!/bin/sh
#
# install.sh -- installs the Linux build from this directory.
#
#   ./install.sh                 for you alone, into ~/.local
#   sudo ./install.sh /usr/local for everybody on the machine
#   ./install.sh --uninstall [PREFIX]
#
# What goes where, under PREFIX:
#   bin/quake                               the launcher: start the game with this
#   lib/quake/xquake                        the engine
#   share/applications/quake.desktop        the entry in the desktop's menu
#   share/icons/hicolor/48x48/apps/quake.png
#
# Your settings and saves are in ~/.local/share/quake either way, and
# uninstalling leaves them there.
#

set -eu

here=$(cd "$(dirname "$0")" && pwd)

uninstall=0
if [ "${1:-}" = --uninstall ]; then
    uninstall=1
    shift
fi

prefix="${1:-$HOME/.local}"

files="bin/quake lib/quake/xquake share/applications/quake.desktop
share/icons/hicolor/48x48/apps/quake.png"

if [ "$uninstall" = 1 ]; then
    for f in $files; do
        rm -f "$prefix/$f"
    done
    rmdir "$prefix/lib/quake" 2>/dev/null || true
    echo "Removed Quake from $prefix. Settings and saves are still in"
    echo "${XDG_DATA_HOME:-$HOME/.local/share}/quake."
    exit 0
fi

# Everything the engine needs from the system, said by name when it is missing
# rather than as a loader error the first time it is run.
missing=$(ldd "$here/lib/quake/xquake" 2>/dev/null | awk '/not found/ {print $1}')
if [ -n "$missing" ]; then
    echo "These libraries are needed and not installed:"
    for m in $missing; do
        echo "    $m"
    done
    echo "On Debian or Ubuntu: sudo apt install libasound2 libsndfile1 libxrandr2"
    echo "On Fedora:           sudo dnf install alsa-lib libsndfile libXrandr"
    echo "On Arch:             sudo pacman -S alsa-lib libsndfile libxrandr"
    exit 1
fi

# Controllers come through SDL2, loaded when the game starts if it is there;
# the game runs without it, so this is a word rather than a refusal.
if ! ldconfig -p 2>/dev/null | grep -q 'libSDL2-2.0.so.0'; then
    echo "Note: SDL2 is not installed, so game controllers will not work."
    echo "      (libsdl2-2.0-0 on Debian/Ubuntu, SDL2 on Fedora, sdl2 on Arch)"
fi

install -d "$prefix/bin" "$prefix/lib/quake" "$prefix/share/applications" \
           "$prefix/share/icons/hicolor/48x48/apps"
install -m 755 "$here/lib/quake/xquake" "$prefix/lib/quake/xquake"
install -m 755 "$here/bin/quake" "$prefix/bin/quake"
install -m 644 "$here/share/icons/hicolor/48x48/apps/quake.png" \
               "$prefix/share/icons/hicolor/48x48/apps/quake.png"

# The menu entry runs the launcher by its full path: ~/.local/bin is not on
# every desktop session's PATH.
sed "s|^Exec=quake|Exec=\"$prefix/bin/quake\"|" \
    "$here/share/applications/quake.desktop" \
    > "$prefix/share/applications/quake.desktop"
chmod 644 "$prefix/share/applications/quake.desktop"

command -v update-desktop-database >/dev/null 2>&1 \
    && update-desktop-database -q "$prefix/share/applications" 2>/dev/null || true

echo "Installed Quake into $prefix."
echo
echo "Start it from your desktop's menu, or run: $prefix/bin/quake"
echo "The first time, point it at your game files if it does not find them:"
echo "    $prefix/bin/quake --data /path/to/Quake"

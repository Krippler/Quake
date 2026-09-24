Quake for the Linux desktop
===========================

id Software's 1999 GPL source release of Quake, ported to current Linux, with
the re-release's mission packs, Dimension of the Machine, coloured light, fog
and translucency, and menus laid out the way the re-release lays them out.
It needs the game files from a copy of Quake you own (Steam, GOG, or the
original CD); none are included.

Install
-------

    ./install.sh                  for you alone, into ~/.local
    sudo ./install.sh /usr/local  for everybody on the machine

Then start Quake from your desktop's menu, or run `quake` (~/.local/bin/quake
if ~/.local/bin is not on your PATH).

It runs on Ubuntu 22.04, Debian 12, Fedora 35 or anything newer, under X11
or XWayland, and needs these libraries, which almost every desktop already
has:

    Debian, Ubuntu:  sudo apt install libasound2 libsndfile1 libxrandr2
    Fedora:          sudo dnf install alsa-lib libsndfile libXrandr
    Arch:            sudo pacman -S alsa-lib libsndfile libxrandr

Game files
----------

The first time it starts, Quake looks for a Steam or GOG copy in the usual
places and uses the re-release if it finds one. If it does not find yours:

    quake --data /path/to/Quake

where /path/to/Quake is the directory with id1/pak0.pak in it. For a Steam
copy of the re-release that is .../steamapps/common/Quake/rerelease. It is
remembered after that. Your game files are only read; settings, saves and
screenshots go in ~/.local/share/quake.

Playing
-------

Options -> Display -> Fullscreen switches to fullscreen and back. Video
Modes sets the resolution the game draws at, up to 1920x1200; fullscreen
scales that up to fill the monitor.

The mouse is captured while you play, and let go in the menus and the
console, and when another window has the focus. `_windowed_mouse 0` at the
console turns capture off.

Sound goes to ALSA's default device, which is PipeWire or PulseAudio on a
desktop that runs either. -alsadevice NAME picks another.

Options -> Game / Mod switches between the base game, the mission packs and
any mods; Quake restarts on the one picked.

Anything after `quake` is passed to the engine: `quake +map e1m1`,
`quake -game hipnotic`, `quake +vid_fullscreen 1`.

Uninstall
---------

    ./install.sh --uninstall [PREFIX]

Settings and saves stay in ~/.local/share/quake.

Source, the Docker version for playing in a browser, and the full notes:
https://github.com/Krippler/Quake
Licensed under the GNU General Public License, version 2: see COPYING.

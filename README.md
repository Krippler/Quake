# Quake

The 1999 GPL source release, repaired until it builds and runs on a current
64-bit Linux, given sound and music the container age can actually carry, and
packaged so you play it in a browser.

```
docker run --rm -p 6080:6080 -v /path/to/quake:/quakedata:ro ghcr.io/krippler/quake
```

Then open **<http://localhost:6080/play.html>** and click to play.

Nothing is installed on the host — no X server, no display, no audio setup —
and picture and sound both arrive in the browser.

## Your own game data

There is no game data in the image, and there will not be. id's shareware
licence ([`WinQuake/data/SLICNSE.TXT`](WinQuake/data/SLICNSE.TXT), clause 6)
permits passing the shareware release along *as a whole*, free of charge, by
electronic means, in a compressed format, with the agreement attached. A pak
file lifted out of that archive and baked into a container image is none of
those things. DOOM's shareware IWAD may be copied unmodified on its own; Quake's
may not, and that is the one thing this port cannot do the same way.

So `/path/to/quake` above is your own install directory — the one holding
`id1/pak0.pak`. Any of these will do:

* the CD, or the directory a CD install left behind;
* the Steam or GOG install (`.../Quake/id1/`);
* the shareware release unpacked, from id's own `quake106.zip`.

The mission packs and mods go beside `id1` in the same directory, and the
container finds them:

```
/path/to/quake/
├── id1/pak0.pak, pak1.pak    ← the game
├── hipnotic/pak0.pak         ← Scourge of Armagon      (QUAKE_GAME=hipnotic)
├── rogue/pak0.pak            ← Dissolution of Eternity (QUAKE_GAME=rogue)
├── ad/                       ← any mod                 (QUAKE_GAME=ad)
└── music/track02.ogg ...     ← the soundtrack, see below
```

Mounted read-only, as above, is right: the container never writes to your game
data. It builds a writable game directory of its own in the state volume and
links your pak files into it, because Quake writes `config.cfg` and its
savegames next to the pak files and would otherwise silently lose both.

## Music

Quake's soundtrack is not in the pak files and never was: it is audio tracks 2
to 11 of the CD, and the 1996 code plays them by telling a CD drive to. There
is no drive here, so rip them and put them in `music/` as `track02.ogg`,
`track03.ogg` and so on — the numbering the CD used. Ogg Vorbis, FLAC, Opus,
WAV, and MP3 where the installed libsndfile has it.

Without them the game is silent where the music would be, which is exactly what
the shareware release was like for anyone who downloaded it.

## Keeping savegames and settings

`config.cfg`, savegames and screenshots live in `/quake/state`. Without a volume
there they go when the container does:

```
docker run --rm -p 6080:6080 \
  -v /path/to/quake:/quakedata:ro -v quake-state:/quake/state \
  ghcr.io/krippler/quake
```

With Compose:

```
mkdir -p quakedata && cp -r /path/to/quake/id1 quakedata/
docker compose up --build
```

Running as root is not required. Started as root the container takes ownership
of the state directory as `PUID:PGID` (1001 by default, 99:100 on Unraid) and
drops to that user; started with `--user` it stays as whoever you gave it.

## Resolution

The software renderer draws every pixel on the CPU, so the resolution is a real
choice rather than a free one. 640x480 is the default; the ceiling is 1920x1200.

```
docker run --rm -p 6080:6080 -e QUAKE_WIDTH=1280 -e QUAKE_HEIGHT=800 \
  -v /path/to/quake:/quakedata:ro ghcr.io/krippler/quake
```

The browser scales whatever it is given to fit the window, so a lower number is
not a smaller picture — it is a softer one, and a faster one.

## Building it yourself

```
docker build -t quake .
```

Or without a container, if you have an 8-bit PseudoColor X display to point it
at — which in practice means an Xvfb:

```
make -C WinQuake            # -> WinQuake/linux/xquake
make -C audiostream         # -> audiostream/linux/audiostream
tools/smoke-test.sh         # starts it on a throwaway Xvfb and checks it draws
```

## Documentation

| | |
| --- | --- |
| [ABOUT.md](ABOUT.md) | What this actually is: what the 1999 sources needed, and what the port added |
| [DOCKER.md](DOCKER.md) | Running it: game data, controls, game controllers, options, saves, sound, troubleshooting |
| [PORTING-NOTES.md](PORTING-NOTES.md) | Every change made to the 1999 sources, and why |
| [PUBLISHING.md](PUBLISHING.md) | Releases, image tags, and the Unraid listing |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each release |
| [readme.txt](readme.txt) | id Software's original 1999 release note |

## Licence and game data

The sources are GPLv2 — see [gnu.txt](gnu.txt), the licence id released them
under in December 1999.

None of that covers the game data, and unlike DOOM there is no part of Quake's
data that can travel with the code. `pak0.pak` and `pak1.pak` are id's, come
from your own copy, and are not in this repository or in the published images.
The ignore rules exclude every `*.pak` so they cannot be committed or baked in
by accident.

QUAKE is a trademark of id Software LLC. This is an unaffiliated port of the
sources they published.

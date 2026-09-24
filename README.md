# Quake

id Software's 1999 GPL source release of Quake, repaired to build and run on
current 64-bit Linux and packaged to play in a browser: picture and sound both
arrive there, and nothing is installed on the host.

## Quick start

```
docker run --rm -p 6080:6080 \
  -v /path/to/quake:/quakedata:ro \
  -v quake-state:/quake/state \
  ghcr.io/krippler/quake
```

Open **<http://localhost:6080/play.html>** and click to play.

- `/path/to/quake` is your own Quake install: the directory holding
  `id1/pak0.pak`.
- `quake-state` keeps your settings and savegames between runs. Leave it out
  and they go when the container does.

## Game data

None is included. Quake's data is id's, and its licence does not allow a pak
file to be shipped on its own ([why](ABOUT.md#no-bundled-game-data)). Any of
these works:

- a Steam or GOG install, or a CD install;
- the free shareware release (id's `quake106.zip`), which is episode 1;
- the 2021 re-release: mount its `rerelease` folder
  (`steamapps/common/Quake/rerelease` on Steam) to get the expansions and its
  in-game messages as well.

Mission packs and mods go beside `id1`, and the game's **Options → Game /
Mod** menu switches between them:

```
/path/to/quake/
├── id1/pak0.pak, pak1.pak
├── hipnotic/pak0.pak        Scourge of Armagon
├── rogue/pak0.pak           Dissolution of Eternity
└── <mod>/                   anything else
```

**Music** is not in the pak files; it was audio on the CD. Rip it to
`id1/music/track02.ogg`, `track03.ogg` and so on, and it plays. Without it the
game runs silent where the music would be.

## Common settings

| | |
| --- | --- |
| `-e QUAKE_WIDTH=1280 -e QUAKE_HEIGHT=800` | starting resolution; also in **Options → Display → Video Modes** |
| `-e QUAKE_GAME=hipnotic` | the game or mod to start on |
| `-e PUID=1000 -e PGID=1000` | who owns the saved files (default 1001) |

Everything else, including controls, game controllers, sound, Compose and
Unraid, is in [DOCKER.md](DOCKER.md).

## Documentation

| | |
| --- | --- |
| [DOCKER.md](DOCKER.md) | Running it: game data in detail, every setting, controls, troubleshooting |
| [BUILDING.md](BUILDING.md) | Building the image or the engine yourself, and the smoke test |
| [ABOUT.md](ABOUT.md) | What the 1999 sources needed and what the port added |
| [PORTING-NOTES.md](PORTING-NOTES.md) | Every change made to the sources, and why |
| [PUBLISHING.md](PUBLISHING.md) | Releases, image tags, and the Unraid listing |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each release |
| [readme.txt](readme.txt) | id Software's original 1999 release note |

## Licence

The sources are GPLv2 ([gnu.txt](gnu.txt)), as id released them in December
1999. That does not cover the game data, which comes from your own copy and is
never in this repository or the images.

QUAKE is a trademark of id Software LLC. This is an unaffiliated port of the
sources they published.

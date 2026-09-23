# Building it yourself

## The image

```
docker build -t quake .
```

Or with Compose, which builds the image and mounts `./quakedata` as the game
data (see [DOCKER.md](DOCKER.md#compose)):

```
mkdir -p quakedata && cp -r /path/to/quake/id1 quakedata/
docker compose up --build
```

## Without a container

The engine needs an X display at depth 8 or 24, which in practice means an
Xvfb, plus the X11, libsndfile and zlib development packages:

```
make -C WinQuake            # -> WinQuake/linux/xquake
make -C audiostream         # -> audiostream/linux/audiostream
```

`make -C WinQuake` takes `SOUND=stream|oss|none` and `MUSIC=sndfile|cd|none`;
the defaults are what the image uses.

## The smoke test

```
tools/smoke-test.sh
```

It starts the engine on a throwaway Xvfb and checks that it draws a frame and
produces sound. There is no game data in the repository to test against, so it
builds its own synthetic data and stops at the console.

Point it at real data and it also loads E1M1, which is where the renderer, the
server and the QuakeC interpreter get exercised. The shareware `pak0.pak` is
enough:

```
QUAKE_SMOKE_DATA=/path/to/quake tools/smoke-test.sh
```

CI runs it on every push, along with builds of the other `SOUND` and `MUSIC`
combinations.

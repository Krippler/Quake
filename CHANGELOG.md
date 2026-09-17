# Changelog

All notable changes are here. The format follows Keep a Changelog, and the
top section's heading is what the release workflow reads: `## [X.Y.Z] — DATE`
on the default branch publishes that version, `## [Unreleased]` publishes only
`edge`.

## [1.0.0] — 2026-09-17

First release.

The 1999 GPL source release, repaired until it builds and runs on a current
64-bit Linux, and packaged as a container you play in a browser.

### The sources

Repaired, all of them bugs the release always had:

- `model.c` walked `mtexinfo_t::vecs` off the end of its first row, which gcc
  diagnoses as undefined behaviour and may delete — taking the second texture
  axis of every surface in the map with it.
- `r_part.c` had the same bug in the particle field's velocities.
- `vid_x.c` set the framebuffer pointer to the `XImage` header rather than to
  the pixels, so the non-shared-memory path overwrote the structure describing
  where to draw.
- `vid_x.c` typed a 24-bit pixel as `unsigned long`, which is eight bytes on
  LP64 and wrote twice the length of every scanline.
- `vid_x.c` wrote a nul into the string `getenv` returned, emptying `DISPLAY`
  for the process and everything it exec'd.
- `sbar.c` filled a 20-byte scoreboard row from a 32-byte player name, which a
  server chooses.
- `net_udp.c` declared `gethostname` with an `int` length, which disagrees with
  glibc on LP64 and is a hard error.
- `chase.c` called `SV_RecursiveHullCheck` with no declaration in scope.
- `common.h`, `d_surf.c` and `d_edge.c` put pointers and member offsets through
  `int`.
- `common.c` built paths with unbounded `sprintf` into `MAX_OSPATH`, which was
  128.

Modernised where what the code talked to no longer exists:

- `Sys_FloatTime` reads `CLOCK_MONOTONIC` rather than the wall clock.
- The frame loop sleeps out the rest of the frame instead of spinning: 100% of
  a core at the console became 1.9%.
- The default heap went from 8 MB to 64 MB.
- Signals raise a flag that the frame loop acts on, instead of calling
  `Host_Shutdown` and Xlib from the handler.
- `XSynchronize(True)`, left on in the release with "for debugging only" above
  it, is now behind `-verbose`.
- The engine installs its own colormap, because there is no window manager to
  do it.
- Shared memory uses `IPC_PRIVATE` and mode 0600, and its failures are checked.
- The resolution is clamped and rounded; `MAXWIDTH`/`MAXHEIGHT` went from
  1280x1024 to 1920x1200.
- `WM_DELETE_WINDOW` is handled, so closing the window writes the config.

### New

- **Sound**, through a backend that keeps its own clock and writes the mixed
  output to a pipe — `/dev/dsp` has not existed for twenty years and a
  container has no sound card. `audiostream` reads the pipe and serves it to
  the browser alongside the picture, on one port.
- **Music**, from `track02.ogg` and friends, decoded with libsndfile and mixed
  into the engine's own output. Quake's soundtrack was CD audio and was never
  in the game data.
- **The container**: Xvfb at depth 8, x11vnc, noVNC, and a `play.html` that
  captures the mouse — noVNC reports where the pointer is, and a game needs to
  know how far it moved.
- **Controller support**, entirely in the browser, reading the engine's own
  `config.cfg` so that a rebound control still works in a level.
- **A writable game directory**, assembled in the state volume from the
  read-only mount, because Quake writes its config and savegames next to the
  pak files.
- **A smoke test** that builds its own game data, starts the engine on a
  throwaway Xvfb, and checks that it draws a frame and produces a 440 Hz tone —
  there is no game data in this repository to test against.

# Changelog

All notable changes are here. The format follows Keep a Changelog, and the
top section's heading is what the release workflow reads: `## [X.Y.Z] — DATE`
on the default branch publishes that version, `## [Unreleased]` publishes only
`edge`.

## [1.1.0] — 2026-09-17

### Added

- **WASD and mouse look by default.** The container seeds `config.cfg` once,
  on a state volume that has none, with W A S D to move, the mouse to look, the
  wheel to change weapon and E/Q to swim. id's 1996 defaults — arrow keys to
  move, `,` and `.` to sidestep, `a` to look up, and the mouse walking you
  forward unless you hold `\` — are still one `QUAKE_MODERN_CONTROLS=0` away,
  and the engine owns the file afterwards, so anything changed in the game
  persists over them.
- **A `freelook` cvar**, archived and set to 1, so the mouse steers the view
  with no key held. `+mlook` is untouched and still wins while it is held;
  `freelook 0` is the 1996 behaviour exactly. Every place that asked
  `in_mlook.state & 1` for this question now asks one macro, so the two cannot
  drift apart.
- **Mouse wheel support in the X11 driver.** X delivers the wheel as buttons 4
  and 5; the 1996 code handled three buttons and dropped the rest, while
  `keys.c` had `K_MWHEELUP` and `K_MWHEELDOWN` in it the whole time. Sent
  straight to `Key_Event`, because a notch is momentary and `IN_Commands` only
  reports changes between frames.
- **Customize controls covers everything**, thirty-one actions rather than
  eighteen, including the weapon keys, the console, the scoreboard, pause and
  screenshot. The old limit was the screen: eighteen rows is all that fits, so
  the menu scrolls now, with indicators for which way there is more.

### Fixed

- A comment in the entrypoint had the pak search order backwards. It claimed a
  loose file shadows the pak copy of the same name;
  `COM_AddGameDirectory` pushes the directory onto `com_searchpaths` first and
  each pak on top, so the paks win and a loose file is only reached for a name
  no pak holds. The log line now says that, and this was offered as a
  hypothesis for a crash report, so it is worth correcting in public.

## [1.0.1] — 2026-09-17

### Fixed

- A crash in the container said nothing at all. The engine's stdout is a pipe
  under docker, so the C library block-buffered it, and a SIGSEGV during
  startup took the entire log with it — the container printed nothing between
  the entrypoint's "running: xquake" and the shell's "Segmentation fault", so
  the one question worth asking, how far did it get, had no answer. stdout is
  line-buffered now.
- The engine prints a backtrace when it dies on SIGSEGV, SIGBUS, SIGFPE,
  SIGILL or SIGABRT, with the signal, the faulting address and named frames,
  then re-raises so the exit status is unchanged. `-rdynamic` is what makes the
  names available; `strip` keeps the dynamic symbol table, so the shipped
  binary reports them too.
- The startup log now lists anything other than pak files that a mount
  contributed to a game directory. Once the registered game is running the
  engine searches the directory as well as the paks, and a loose file shadows
  the pak copy of the same name, so a mount holding both an extracted tree and
  the paks can feed the engine a mixture — which nothing in the log used to
  show.

## [1.0.0] — 2026-09-17

First release.

The 1999 GPL source release, repaired until it builds and runs on a current
64-bit Linux, and packaged as a container you play in a browser.

### The sources

Repaired, all of them bugs the release always had:

- `sv_main.c` made a QuakeC string offset by subtracting `pr_strings` from a
  pointer in bss. On a 32-bit machine that always fitted in the `int` the field
  is; here the two are terabytes apart, so it truncated, and `world.model`,
  `mapname` and the buffer `ftos`/`vtos`/`etos` return all became wild
  pointers. Nothing faulted at the assignment — the first QuakeC `==` on a
  string did, in `worldspawn`, so no map would load. Whether it crashed at all
  depended on the resolution, and demo playback never touched it, so the engine
  played three demos faultlessly and died the moment anybody started a game.
- `pr_edict.c` sized an `ev_pointer` progs field with `sizeof(void *)/4`, which
  is 2 here where a progs slot is one.
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
  there is no game data in this repository to test against. With
  `QUAKE_SMOKE_DATA` pointed at a real install it also loads E1M1, which is
  what found the `pr_strings` bug above; the shareware pak is enough for it.

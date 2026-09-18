# Changelog

All notable changes are here. The format follows Keep a Changelog, and the
top section's heading is what the release workflow reads: `## [X.Y.Z] — DATE`
on the default branch publishes that version, `## [Unreleased]` publishes only
`edge`.

## [1.4.4] — 2026-09-18

### Fixed

- **A game that could not start took the container with it.** The engine stops
  on a `Sys_Error` — a map it cannot read, data that is not there — and that is
  not something restarting fixes, so the run loop breaks on it. But which game
  is played is a choice stored in the state volume, and the menu that sets it is
  inside the game: if that choice was what the engine died on, the container
  stopped on every start and the only way back was to edit the state volume by
  hand.

  The stored choice is dropped and the base game tried once instead, with the
  reason in the log. An explicit `-game`, `-hipnotic` or `-rogue` in
  `QUAKE_ARGS` is left alone, because it applies again on the next start
  whatever the stored file says — dropping it would throw away the player's pick
  and change nothing else. A clean quit and a crash are unaffected: they already
  restart on the same game.

- **BSP2 maps now say what they are.** `Mod_LoadBrushModel` reported them as
  `wrong version number (844124994 should be 29)`, and that figure is the four
  bytes `BSP2` read as an integer. BSP2 is a map format from long after 1996 —
  32-bit node and leaf indices, a larger visibility lump — and it is what the
  Quake re-release ships, so anyone taking an episode out of Steam rather than
  off the CD will meet it. *Dimension of the Past* is the usual way. The engine
  names the format and says this renderer only reads the original version 29.

  Reading BSP2 is not something this port does. It is the 1996 software
  renderer, and the format exists precisely to get past what that renderer
  assumes.

## [1.4.3] — 2026-09-18

### Fixed

- **One stray pak file could hide every mission pack and mod.** The data mount
  was checked for pak files sitting loose at the top *first*, and the scan for a
  directory per game was the `else` branch — so a single `pak0.pak` beside a
  perfectly good `id1/`, `hipnotic/` and `rogue/` took the whole mount over. It
  became id1, nothing else was ever looked at, and the game directories were
  linked into id1 as loose files. The symptom was a Game menu holding nothing
  but Quake.

  The per-directory scan runs first now, and the loose-paks guess only when that
  found no base game — so the documented layout always wins and the convenience
  still works for a mount that is nothing but pak files.

- **A mod added while the container was running never appeared.** The mount was
  scanned once, at container start, but the engine restarts whenever the player
  quits or picks a different game — so anything dropped in between was invisible
  until a `docker restart`. The scan runs before each start of the engine now,
  and says so in the log only when the answer changed.

- **A game directory that lost its pak files took the player's savegames with
  it.** The directory is removed when it turns out to hold no game data, which
  is right for a directory the script has just made and wrong for one the engine
  has been writing `config.cfg`, savegames and screenshots into. It is left
  alone now, with a line in the log saying why.

## [1.4.2] — 2026-09-18

### Fixed

- **A mission pack played Quake's music instead of its own.** The soundtracks
  differ — Scourge of Armagon and Dissolution of Eternity have their own — and
  a rip of each belongs beside its own pak files, in `hipnotic/music`,
  `rogue/music` and so on. The engine does look there. It just looked at
  `$QUAKE_MUSICDIR` first, and the entrypoint set that to `id1/music` whenever
  that directory existed, so from then on every game played Quake's tracks.

  `$QUAKE_MUSICDIR` can only ever name one directory for every game the
  container can run, so it is the cross-game default and the game's own
  directory now beats it. The full order is `-musicdir`, then `<game>/music`,
  then `$QUAKE_MUSICDIR`, then `id1/music` — the last so a mod with no music of
  its own still gets Quake's. The entrypoint no longer points
  `$QUAKE_MUSICDIR` at `id1/music` at all; that fallback was already in the
  engine, and setting it was what did the damage.

- **An empty `music` directory won and played nothing.** The check behind the
  search was `S_ISDIR`, despite being called `CDAudio_DirHasFiles`, so a
  directory with no tracks in it was taken and the fallbacks below were never
  reached. It looks for a `track*` file in a format libsndfile reads now, and
  falls through when there is not one. The container links a `music` directory
  into every game directory it finds, so an empty one is easy to end up with.

- The startup log now says which games have music of their own, rather than
  naming one directory and leaving the rest to be guessed at.

- The smoke test grew a phase for it: 880 Hz in the game's own music directory,
  440 Hz in the shared one, `$QUAKE_MUSICDIR` pointed at the shared one, and an
  assertion that what comes out of the mixer is 880. It fails on the previous
  build and passes on this one.

## [1.4.1] — 2026-09-18

### Fixed

- **The sound stopped for good after any frame longer than three quarters of a
  second — which every level load is.**

  `GetSoundtime` worked out where playback had got to by counting the times the
  ring buffer wrapped, one wrap per call, and id's own comment above it says
  what is wrong with that: *"it is possible to miscount buffers if it has
  wrapped twice between calls to S_Update. Oh well."* The ring is 16384 frames,
  0.74 s at 22050 Hz. In 1996 a frame that long meant the machine had stopped.
  Here a level load is that long, and a browser on a busy machine is worse —
  this port logs picture gaps of one to eighteen seconds as an ordinary
  occurrence.

  A lost wrap is lost for good, because nothing ever recounts. `paintedtime`
  follows `soundtime`, and `SNDDMA_Submit` will not send past `paintedtime`, so
  from then on every frame went down the pipe as silence rather than as the
  mixer's output. At exactly the right rate — so the browser's buffer never
  underran, the page reported nothing, the container reported nothing, and the
  game simply went quiet and stayed quiet.

  Measured on a demo loop: a 3.5 s level load left `paintedtime` 63331 frames
  behind the clock, and it was still exactly 63331 frames behind twenty seconds
  later. With the same engine stalled deliberately for four seconds, the audio
  captured off the pipe was full-scale for twelve seconds and then flat zero for
  the remaining fourteen.

  There is no sound card here and nothing to reconstruct: this backend's clock
  *is* the playback position. `SNDDMA_GetSamples` hands the running count over
  and `GetSoundtime` stops guessing. Same test, same stall: audio the whole way
  through.

  The 0.04 s floor on the sound-delay slider in 1.4.0 was not this. It was a
  guard put up while the cause was still unknown, and it stays because 0.02 s
  is genuinely too little for one frame's grace — but it fixed nothing, and
  this is what was actually wrong.

- The smoke test grew a phase for it: a sustained tone, `SIGSTOP` for three
  seconds, and an assertion that the tone is still arriving five seconds after
  the engine is let go. It fails on the previous build and passes on this one.

- **The quicksave was the one save the load menu would not show.** F6 and F9 are
  bound to `save quick` and `load quick`, which writes `quick.sav`; the menu only
  ever looked for `s0.sav` to `s11.sav`. In 1996 that was survivable, because F9
  was right there. In a browser it is not — the page may never see F9 at all, and
  a player who had quicksaved had no way back to it.

  It is a thirteenth row now, in both the load and the save menu, after a blank
  line so it reads as separate and drawn in white rather than gold so it is still
  identifiable once it holds a real comment.

## [1.4.0] — 2026-09-18

### Added

- **Mission packs and mods are in the menu.** **Options → Game / mission pack**
  lists everything installed beside `id1` — Scourge of Armagon, Dissolution of
  Eternity and the mission packs' own name for anything else, by directory.
  Picking one restarts Quake on it.

  It restarts rather than switching in place because the search path is built
  once, in `COM_InitFilesystem`, and nothing rebuilds it: swapping it underneath
  a running game means throwing away every model, sound, texture and progs the
  hunk holds and loading them again, which is most of what starting over does
  anyway with none of the certainty. In the container the restart is close to
  invisible — the engine already runs in a restart loop and the page reconnects
  by itself, so it is a few dark seconds. Started by hand it quits, and the
  choice applies next time.

  The choice is stored beside the game directories and read at startup, and it
  applies exactly what the switch for that directory would have: `rogue` and
  `hipnotic` are not only search paths, they change the status bar and the menu.
  An explicit `-game`, `-hipnotic` or `-rogue` still wins, because the engine
  reads its command line before it reads the file.

- **`QUAKE_GAME` is a starting point rather than a lock.** Setting it, or
  changing it, overrides whatever the menu last chose; leaving it alone leaves
  the menu's choice alone. Without that, a container configured with
  `QUAKE_GAME=hipnotic` would drag the game back to Scourge of Armagon on every
  restart and the new menu would appear to do nothing.

- **The sliders say what they are set to.** A slider shows how far along it is,
  which for a field of view or a mouse speed is not the number anybody wants.
  Printed to the right of the bar now, with as few decimals as the value needs.

### Fixed

- **The weapon disappeared above a 90 degree field of view.** id's own guard:
  `R_DrawViewModel` returned on `r_fov_greater_than_90`, because the gun is a
  foot from the camera where a wide projection stretches it across the screen
  and out through the wall behind it. Fine when the field of view was a console
  command nobody found; not fine now that it is a slider, where moving it made
  the weapon vanish with no explanation.

  Answered the way every later port answers it: the view model gets its own
  field of view, fixed at 90 and widened for the screen exactly as the world's
  is, and the world keeps the player's. The software renderer reads the
  projection out of four globals, and nothing but the view model is drawn
  between saving them and putting them back.

- **The sound delay slider went low enough to starve the mixer.** Quake paints
  one buffer per frame, so `_snd_mixahead` is also how long a frame may take
  before the sound runs dry; 0.02 s is 20 ms, which no browser-in-a-container is
  going to hold to. The slider starts at 0.04 now. Anything lower is still there
  at the console for somebody who means it.

## [1.3.0] — 2026-09-18

### Added

- **The options menu covers the settings, rather than thirteen of them.**
  Twenty-three rows now, scrolling the way the controls menu does: the field of
  view, mouse look, smooth mouse, the crosshair, whether the weapon is drawn,
  view bob, view kick, water warp, texture detail and the sound delay, next to
  everything that was already there.

  id's menu drew each row in one switch statement, adjusted it in a second and
  acted on Enter in a third, with the row's identity being its position in all
  three — so adding a setting meant editing three places and getting the
  numbering right in each. Everything id added after 1996 went to the console
  instead, and so had everything this port added: `freelook`, the sound delay,
  the field of view that widescreen made worth changing. One table now says
  what each row is and which cvar it moves, and adding a setting is one line.

### Fixed

- Six of the settings now in the menu were not archived cvars, because id only
  ever offered them at the console and a console setting was not expected to
  last. A row in a menu is: set it, and it is still set tomorrow. `fov`,
  `r_drawviewmodel`, `cl_bob`, `v_kicktime`, `r_waterwarp` and `d_mipcap` are
  archived now.

- **`d_mipcap` did nothing until the resolution changed.** It caps how blurry
  the far end of a wall may get, and dropping detail is one of the few things
  that buys frames in a software renderer — but it was read once, in
  `D_InitCaches`, which runs on a video mode change and at no other time. Read
  per frame now, which is a float and a clamp against everything else a frame
  does.

- A menu row whose cvar does not exist says `n/a` and does nothing, rather than
  printing `Cvar_Set: variable volume not found` once per press of an arrow
  key. `-nosound` makes `S_Init` return before it registers `volume`,
  `bgmvolume` and `_snd_mixahead`, so the three sound rows had nothing behind
  them on a run with the sound off. id's menu did that too.

## [1.2.0] — 2026-09-18

### Fixed

- **Widescreen modes were stretched.** `vid.aspect` is the shape of one pixel,
  and the renderer multiplies the vertical scale by it. id computed it as
  `(height / width) * (320 / 240)`, which cancels to a constant 4:3 whatever
  the mode is — correct in 1996, when every mode was 4:3 and 320x200 really was
  displayed with non-square pixels. Every mode here is a framebuffer in a
  browser, where a pixel is square, so the renderer drew a 4:3 picture and the
  browser showed it at 16:9. Measured as the ratio of the renderer's vertical
  to horizontal scale: 1.0 at 4:3, 0.83 at 16:10, 0.75 at 16:9 — a quarter too
  wide, which is exactly the mode's aspect over 4:3.

  A wider screen now also shows more to the sides rather than cropping the top
  and bottom off, which is what a fixed horizontal `fov` would otherwise do:
  the horizontal field of view is widened by however much wider than 4:3 the
  screen is, leaving the vertical one where 4:3 puts it. Nothing changes at 4:3
  or narrower — 640x480 renders the same scale factors it always did — and
  `fov` still means the horizontal angle at 4:3.

- **The sound lagged further behind than it needed to.** The engine mixes ahead
  of itself, and id's `_snd_mixahead` of 0.1 s is most of the delay between
  firing a shot and hearing it. Measured from a keystroke to the sound reaching
  the socket: 130 ms at 0.1, 84 ms at 0.06, 72 ms at 0.04, and no further gain
  at 0.02 — below about 0.04 the floor is one engine frame plus the chunk size.
  The container asks for 0.06, which takes the 46 ms that is really there;
  `QUAKE_SND_MIXAHEAD` moves it, and 0.1 is id's behaviour exactly.

  Passed as a console command rather than seeded into `config.cfg`, because the
  cvar is archived: a state volume that already exists has id's 0.1 in its
  config and would overrule anything written there.

- The page says in the container log when its own buffer grows, and by how
  much. It answers an underrun by holding more sound — a longer delay beats a
  click — and on a machine that cannot keep up that climbs to 250 ms and
  becomes most of the delay. The figure existed only in `window.__audio()`, so
  a report of delay arrived with no way to tell a grown buffer from a container
  sending late.

- **The engine crashed while reporting that it could not open the display.**
  `VID_Init` answers a display it cannot open with `Sys_Error`, which calls
  `Host_Shutdown` and so `VID_Shutdown` — the one path where there is no
  display to close. `XAutoRepeatOn` then dereferenced a null `Display` and the
  process died on `SIGSEGV` at `0x968`.

  The real message had already been printed, but what the container saw next
  was a segfault, so it reported a crash, restarted, crashed identically, and
  gave up three runs later — with the signal in the log and the reason sitting
  above it looking like part of the previous run. That is the shape of the
  startup crash reported against 1.0.0 and never explained.

- The container starts the engine at the resolution `config.cfg` asks for
  rather than at `QUAKE_WIDTH`/`QUAKE_HEIGHT` and resizing into it a frame
  later. `vid_width` is archived and `config.cfg` is exec'd after `VID_Init`,
  so the window was always created at one size and resized to another; now it
  is not, which removes a browser reconnect from every start. Re-read before
  each run, because the engine rewrites `config.cfg` when it exits.

  On its own this is not what fixes the colours below; it removes a gratuitous
  resize, which is worth having anyway.

- **The picture came back in the wrong colours after quitting to the title
  screen.** Teal and magenta instead of Quake's browns, and it stayed that way.

  The engine exits, the container starts it again, and the new window has a
  colormap of its own. x11vnc shows this 8-bit screen to the browser by
  converting it through the window's colormap, and it carried on converting
  through the one that died with the old window. x11vnc's manual owns the
  limitation: "if there are multiple 8bpp windows using different colormaps,
  one may have to iconify all but one for the colors to be correct."

  A VNC session that connects afresh is correct every time; nothing else tried
  was — not `x11vnc -R refresh`, not `-fixscreen 8=t`, not re-uploading the
  palette from the engine, not starting the engine at the config's resolution.
  So the container counts engine starts, serves the count at `/quake-run`, and
  the page opens a new session when it moves, about five seconds after a
  restart. Nothing in the VNC protocol says a window was replaced, so it has to
  be counted rather than noticed.

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
- **A Video Options menu**, with twenty resolutions from 320x240 to 1920x1200.
  The X11 driver never set `vid_menudrawfn`, and `menu.c` hides the line when
  it is null, so the X build had no video menu at all — the resolution was
  whatever the command line said and nothing could change it afterwards.
  Picking a mode writes the archived `vid_width` and `vid_height` cvars, which
  is also what makes the choice survive a restart; setting them at the console
  does the same thing. Modes the X server will not accept are not offered.
- **The engine resizes the X screen, not just its window.** The browser sees
  the whole root window, so a smaller window would sit in the corner of a
  framebuffer it cannot fill. The engine creates the RANDR mode and moves the
  screen to it, and x11vnc's `-xrandr resize` passes the new size to the
  browser as NewFBSize — which noVNC handles by resizing its canvas, visible
  as a brief blink. A server's maximum screen size is fixed when it starts, so
  the container now starts Xvfb at 1920x1200 and the engine brings it down;
  `QUAKE_MAX_WIDTH` and `QUAKE_MAX_HEIGHT` change that ceiling. Only done when
  `-resizescreen` says the engine owns the display, because on a desktop
  picking a resolution in Quake has no business rearranging anything else.

### Fixed

- **Only the first shifted character of a session reached the console.**
  `XLateKey` took the keysym with the event's shift state applied, so
  shift+minus arrived as `_` going down and — shift being up by then — as `-`
  coming up. `Key_Event` counts autorepeats in `key_repeats[key]` and only
  clears the entry on the release, so `key_repeats['_']` went to 1 and stayed
  there, and every `_` after the first was discarded as an autorepeat. The
  same for every capital letter, colon and quote: `vid_width` reached the
  console as `vidwidth`, and a name or a server address could be typed once.
  The keysym is now taken with shift masked out, which is what `keys.h` asks
  for ("normal keys should be passed as lowercased ascii") and leaves the
  shift table in `keys.c` to do its job.
- **A resize crashed the engine when the new mode was larger.**
  `D_InitCaches` announces the new surface cache size with `Con_Printf`, and
  `Con_Printf` draws the screen — which re-entered `SCR_UpdateScreen` from
  inside `VID_Update`, with `vid.width` already the new size and `vid.buffer`
  still the old, smaller framebuffer. `Draw_ConsoleBackground` then wrote a
  640-pixel row into a 512-pixel one. `block_drawing`, which `SCR_UpdateScreen`
  has always checked first thing and which `vid_win.c` sets around a mode
  change for this exact reason, is now set here too; nothing in this build had
  ever set it. Reachable before this release by resizing the window from
  outside.
- `vid_menudrawfn` and `vid_menukeyfn` were defined in both `menu.c` and
  `vid_x.c`, and only `-fcommon` merged the two into one symbol. `menu.c` owns
  them now.
- **A command line longer than 1023 bytes killed the engine.** `Cbuf_Execute`
  copies each line out of the 8 KB command buffer into a 1024-byte array on the
  stack with `memcpy`, using the length it measured in the buffer and not the
  size of the array, then writes a nul one past that. One command with no
  newline or semicolon in it — a config file whose last line has no terminator,
  or a long enough `bind` — overran it; glibc's `_FORTIFY_SOURCE` check turns
  that into `SIGABRT`, which is why it aborts rather than doing something
  worse. Such a line is now reported and dropped, because half a command is not
  the command that was asked for.
- **`Cbuf_AddText: overflow` now says what overflowed.** The 1996 message was
  that one word: not how large the buffer is, not how much was in use, and not
  what was being added. Whatever fills the buffer is usually still going, so it
  arrived scores of times and pushed anything that might have explained it off
  the top of the console. It reports once, with the size, the amount in use and
  the start of the text that was dropped, counts the rest, and says how many
  were lost when there is room again.
- **The mouse wheel could flood the command buffer.** The wheel handling added
  in this release called `Key_Event` straight from the X event loop rather than
  through the key queue that everything else goes through. `Sys_SendKeyEvents`
  dispatches at most one queue's worth per frame, and that bound is what keeps
  a frame's key events from outgrowing the command buffer — which `Cbuf_Execute`
  drains only once per frame. Bypassing it meant one frame could take an
  unbounded number of notches, each writing its binding into the buffer. All
  four wheel events go through the queue now, as do the two key paths, so there
  is one way in.
- The X11 driver ignored `MappingNotify`. Xlib caches the keyboard mapping when
  the connection opens, and x11vnc types a character the keymap does not have
  by binding it to a spare keycode and putting the keymap back afterwards, so
  those characters arrived as whatever the stale cache said that keycode used
  to mean.
- A size arriving from outside was taken as given, and the renderer's static
  tables are bounded by `MAXWIDTH` and `MAXHEIGHT`. It is clamped now, and the
  aspect ratio is recomputed, which the resize path never did.

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

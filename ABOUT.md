# What this actually is

id Software published these sources in December 1999.
[`readme.txt`](readme.txt) is Carmack's note from that release and is left
exactly as it was — it describes the code, not this repository. It says the
projects were tested with Visual C++ 6.0 and that masm is required for the
assembly files. Neither of those has been true of anybody's machine for a very
long time.

What it does not say is that the code no longer compiles, and did not run once
it did. Getting from there to a playable game took:

- **Repairs to the sources themselves.** A QuakeC string offset made by
  subtracting two pointers that are in different parts of the address space
  here, so it truncates — which is what stopped a map ever loading, and which
  crashed or did not depending on the resolution. A texture-coordinate loop
  that walks off the end of its array — which gcc diagnoses as undefined behaviour and is
  entitled to delete, taking the second texture axis of every surface in the
  map with it. The same shape of bug in the particle code. A framebuffer
  pointer set to the header of the structure describing the framebuffer, so
  every frame overwrote the description of where to draw it. A 24-bit pixel
  type declared `unsigned long`, which is eight bytes here and wrote twice the
  length of every scanline. A scoreboard row of 20 bytes filled from a player
  name of 32. And `getenv` used as if it returned a copy, with a nul written
  into the middle of the process's own environment.
  [`PORTING-NOTES.md`](PORTING-NOTES.md) has all of them, with the original
  code.

- **Sound.** The 1996 backend mmaps `/dev/dsp` and asks the driver where the
  playback pointer is. No current kernel provides `/dev/dsp`: OSS was replaced
  by ALSA before this source was released, and the emulation that stood in for
  it afterwards is gone too. A container is worse off still — it has no sound
  card, and the only packaged PulseAudio brings systemd, GStreamer and a set of
  video codecs with it to do a job that is here "put these bytes on a socket".
  So the engine keeps its own clock instead of asking a driver for one, and
  hands the mixed result to a pipe.

- **Music, which was never in the game data.** Quake's soundtrack is audio
  tracks 2 to 11 of the CD-ROM, and `cd_linux.c` plays them by opening
  `/dev/cdrom` and issuing `CDROMPLAYTRKIND` — a command to a drive, which
  tells its own DAC to play. The music never passed through Quake's mixer at
  all, which is why `bgmvolume` in the 1996 code sets the drive's volume rather
  than scaling anything. Here the same track numbers are looked up as files,
  decoded with libsndfile and mixed into the engine's own output.

- **A way for any of that to reach you.** VNC carries a picture and nothing
  else. So `audiostream` reads the engine's pipe on a real-time schedule and
  serves the result to the page, which plays it through an `AudioWorklet` where
  the browser allows one and a `ScriptProcessorNode` where it does not. Nothing
  to mount, which matters when the container is on a server in another room.

- **A display the engine will accept.** The software renderer writes palette
  indices and uploads a colormap; it only ever supported an 8-bit PseudoColor X
  visual, which no current X server offers. The container brings its own Xvfb
  at depth 8 and exports it over noVNC. It also installs its colormap itself,
  because installing one is the window manager's job and there is no window
  manager — without that, everything reading the display got the right palette
  indices through the wrong 256 colours.

- **A browser client that captures the mouse.** noVNC is a remote desktop
  client and reports where the pointer is; a game needs to know how far it
  moved. `play.html` locks the pointer instead and walks the remote pointer
  around the screen by the difference, so turning never runs out of screen and
  the cursor cannot wander off into the rest of your desktop.

- **A way into the game's menu, given that `Esc` is spoken for.** Pointer Lock
  reserves `Esc` for the browser, and `Esc` is Quake's menu key. The DOOM
  container answered this by moving its menu onto `` ` ``; Quake cannot, because
  `` ` `` is the console and the console is how you load a map, change the skill
  or start the music. So the start screen has a Game menu button that sends one
  Escape and hands the picture straight back, and a controller's B does the same
  without leaving the game.

- **A game directory the engine can write to.** Quake writes `config.cfg`,
  savegames and screenshots into `com_gamedir` — the same directory the pak
  files are in. Mount your game data read-only, as everybody does and as the
  README says to, and the engine cannot save your game or remember a single
  setting; it does not say so either, it prints "failed on config.cfg" into a
  console nobody is looking at and carries on. The container builds a writable
  game directory in its state volume and links the mounted pak files into it.

- **A game controller, on a phone as well as a desktop.** An Xbox pad, a
  Backbone One, anything the browser calls a standard gamepad: the page turns
  its buttons into the keysyms the engine already reads and its right stick
  into the same relative pointer motion a captured mouse produces, so the 1996
  code needed no change at all. The page also reads the engine's own
  `config.cfg`, because Quake binds keys to commands and a pad that presses the
  stock keys goes dead the moment anybody rebinds anything.

## What this is not

It is not a modern source port. There is no OpenGL, no higher-precision
lightmaps, no protocol extension, no BSP2 support and no fix for the engine's
own limits — `MAX_EDICTS`, the 64-unit lightmap grid, the eight-bit palette.
If you want to play a 2024 megawad-equivalent with a modern renderer, use
QuakeSpasm or Ironwail; they are excellent and this is not trying to be them.

What it is: the code id published, changed only where it was wrong or where the
thing it talked to no longer exists, with every change written down.

## The difference from the DOOM container

This is the same idea as [Krippler/DOOM](https://github.com/Krippler/DOOM) and
much of the plumbing is shared, but three things came out differently:

- **No bundled game data.** DOOM's shareware IWAD may be copied freely on its
  own and ships in the image, so `docker run` with nothing mounted is a playable
  game. Quake's shareware licence permits redistributing the release as a whole,
  in its original archive; it does not permit shipping a pak file out of it. So
  this container refuses to start with nothing mounted, and says why.

- **The resolution is yours to pick.** DOOM renders 320x200 and the container
  scales it. Quake's renderer takes a resolution, so there is nothing to scale:
  `QUAKE_WIDTH` and `QUAKE_HEIGHT` go straight to the engine and to the Xvfb
  screen.

- **The engine mixes its own sound.** DOOM's release mixed effects in a separate
  process and never implemented music at all, so `audiostream` had to mix two
  streams itself. Quake mixes everything in-process, so `audiostream` here is
  only a pipe-to-socket pump that keeps time — the same discipline about
  frame alignment and silence padding, a third of the code.

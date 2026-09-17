# Every change made to the 1999 sources, and why

The rule was: change nothing that works. Everything below is either a bug the
sources always had and a modern compiler or a 64-bit ABI makes fatal, or a
thing the code talked to that no longer exists.

Files that are new rather than changed — `WinQuake/Makefile`,
`WinQuake/snd_stream.c`, `WinQuake/cd_stream.c`, `audiostream/` — are in their
own section at the end.

Nothing under `QW/` or `qw-qc/` has been touched. QuakeWorld is not part of
this port.

---

## The build

### `Makefile.linuxi386` builds none of this any more

The original wants egcs 1.1.2 from `/usr/local/egcs-1.1.2/bin/gcc`, sources
from `/grog/Projects/WinQuake`, SVGAlib, 3dfx Glide and Mesa 2.6, and it lists
no header dependencies at all — editing `quakedef.h` rebuilds nothing and
leaves a binary half-built against the old one. It is left in place as the
historical record.

`WinQuake/Makefile` is new and builds one target: the X11 software renderer, as
portable C. Two flags in it are load-bearing rather than stylistic:

* **`-fno-strict-aliasing`.** The renderer reads floats through integer
  pointers to get at their exponents — `r_main.c` and `d_polyse.c` both do it —
  which gcc 3 and later are entitled to assume never happens.
* **`-fwrapv`.** The fixed-point arithmetic overflows deliberately in places.

### The assembler is gone, and was already optional

`quakedef.h` sets `id386` to 1 only when `__i386__` is defined, which on x86-64
it is not, so every `.s` file drops out and the C paths in `nonintel.c`,
`d_scan.c`, `d_polyse.c`, `r_draw.c`, `r_edge.c`, `snd_mix.c` and `mathlib.c`
are used instead. id wrote those as the reference implementation for the Alpha
and SPARC ports, and they were already correct. Nothing had to be written.

The `readme.txt` warning that the software renderer "loses almost half its
speed" without the assembler was true of a Pentium. It is not interesting now:
at 640x480 the renderer idles at a few per cent of one core of anything modern.

---

## Bugs in the sources

### `model.c` — the second texture axis of every surface

```c
for (j=0 ; j<8 ; j++)
        out->vecs[0][j] = LittleFloat (in->vecs[0][j]);
```

`mtexinfo_t::vecs` is `float[2][4]`. Walking it as eight floats from
`vecs[0][0]` runs off the end of the first row into the second, which is
undefined behaviour — and gcc says so, in as many words:

```
model.c:666: warning: iteration 4 invokes undefined behavior [-Waggressive-loop-optimizations]
```

It is entitled to drop iterations 4 to 7 entirely. Then `vecs[1]` is whatever
the hunk happened to hold, which is the S/T basis for every wall, floor and
ceiling in the map. Fixed by walking four and doing both rows.

This one is worth dwelling on because it fails silently and catastrophically,
and the compiler tells you about it in a warning that most builds do not print.

### `sv_main.c` — a string offset that is not an offset

```c
ent->v.model = sv.worldmodel->name - pr_strings;
```

This is the one that stopped the game starting, and it is the most instructive
thing in the port.

A QuakeC `string_t` is an `int` offset into `pr_strings`, and this makes one by
subtracting two pointers. That is only a valid offset while the two are close
enough for the difference to fit in an `int` — and in 1996 they always were,
because a 32-bit address space cannot hold two objects further apart than an
`int` can express. The round trip through `pr_strings + n` could not fail.

Here it can. `pr_strings` is inside the progs block, which comes from the hunk
— one `malloc`, so somewhere in the mmap region — while `sv.name` and
`mod_known`, which `sv.worldmodel->name` points into, are in bss. Those are
terabytes apart. The difference truncates to 32 bits, and `pr_strings` plus the
truncation is an address with the right low half and nothing else right.

Three fields were set this way: `world.model`, `mapname`, and (under `QUAKE2`)
`startspot`. `pr_cmds.c` had the same bug for the buffer `ftos`, `vtos` and
`etos` all return, which is every number QuakeC ever prints.

What made it interesting to find:

* **Nothing faults at the assignment.** The first thing to touch the value is
  QuakeC comparing two strings, which compiles to a `strcmp`, in `worldspawn`'s
  `if (world.model == "maps/e1m8.bsp")`. So the backtrace names `pr_exec.c` and
  the cause is in `sv_main.c`, several thousand instructions earlier.
* **Whether it faults at all depends on the resolution.** The wrong address is
  only a crash if it happens to be unmapped, and how much the hunk holds
  changes what is mapped. At 1280x800 it read rubbish and carried on — a
  string compare quietly returning the wrong answer. At 640x480, which is the
  container's default, it segfaulted every time. The first run against real
  game data was at 1280x800 and looked perfect.
* **Demo playback never touches it.** `SV_SpawnServer` is what sets these, and
  a demo is a recording of a server, not one being run. So the engine played
  three demos through E1M3 faultlessly and then died the moment anybody started
  a game.

Fixed by copying engine-owned strings into a small block of the hunk beside
`pr_strings`, where the offset means what it says — `PR_SetEngineString` in
`pr_edict.c`, which also checks the offset round-trips rather than assuming it.
The four remaining subtractions in the tree are all hunk-to-hunk or exact round
trips, and now say so in a comment, because "safe for a reason nobody wrote
down" is how this one survived.

### `pr_edict.c` — a progs slot is four bytes everywhere

```c
int type_size[8] = {1,sizeof(string_t)/4,1,3,1,1,sizeof(func_t)/4,sizeof(void *)/4};
```

These are counts of 32-bit progs slots, which is a property of `progs.dat` and
not of the host. `sizeof(void *)/4` is 2 here, so `ED_Write` and
`ED_ParseEdict` would read one slot past an `ev_pointer` field. No field in
id's `progs.dat` has that type, which is why it never showed.

### `r_part.c` — the same bug, in the particle field

```c
if (!avelocities[0][0])
{
for (i=0 ; i<NUMVERTEXNORMALS*3 ; i++)
avelocities[0][i] = (rand()&255) * 0.01;
}
```

`avelocities` is `vec3_t[NUMVERTEXNORMALS]`, so `avelocities[0][i]` is off the
end from `i == 3`. Dropped, the effect entity's particle halo sits still
instead of swirling. Fixed by walking it through a flat `float *`, which is
what the code meant. (The indentation is id's, not a transcription error.)

### `vid_x.c` — the framebuffer pointed at its own header

```c
vid.buffer = (byte*) (x_framebuffer[0]);
```

That is the `XImage`, not the pixels behind it. Every frame the renderer drew
overwrote the structure describing where to draw, and the first `XPutImage`
read width, height and stride back out of whatever the title screen happened to
put there.

It survived to the source release because this is the non-shared-memory path,
and MIT-SHM was available on every machine id ran it on. It is the path a
container takes when `DISPLAY` names a host — and the one anybody takes with
`-noshm`. Fixed to `x_framebuffer[0]->data`.

### `vid_x.c` — `PIXEL24` is eight bytes here

```c
typedef unsigned long PIXEL24;
```

`st3_fixup` expands the 8-bit frame in place into a buffer whose stride the X
server sized at four bytes per pixel, writing one `PIXEL24` per pixel
*backwards from the end of the line*. At eight bytes each, every scanline wrote
twice its own length and walked into the next. Now `unsigned int`.

Only reachable on a depth-24 visual, which the container does not use — but
"only when you point it at your desktop" is not a good place for a heap
overwrite.

### `vid_x.c` — a nul written into the environment

```c
displayname = (char *) getenv("DISPLAY");
if (displayname)
{
        char *d = displayname;
        while (*d && (*d != ':')) d++;
        if (*d) *d = 0;
```

The host part of `DISPLAY` is everything before the colon, and this finds it by
writing a nul over the colon — in the string `getenv` handed back, which is the
process's own environment. `DISPLAY` becomes empty for this process and for
everything it goes on to exec. Now copied into a local buffer first.

### `sbar.c` — a 20-byte row filled from a 32-byte name

```c
sprintf (&scoreboardtext[i][1], "%3i %s", s->frags, s->name);
```

`scoreboardtext` is `[MAX_SCOREBOARD][20]` and `s->name` is up to
`MAX_SCOREBOARDNAME` (32). Three digits, a space and 31 characters is 36 bytes
written from offset 1 of a 20-byte row; for the last row that is off the end of
the array. The server picks the names, so it is reachable from the network.
Now `snprintf` bounded to the row.

### `common.h` — a member offset truncated to `int`

```c
#define STRUCT_FROM_LINK(l,t,m) ((t *)((byte *)l - (int)&(((t *)0)->m)))
```

The offsets are all small so the arithmetic came out right, but it is a
diagnosed pointer/integer size mismatch and the compiler is within its rights
to refuse it. Now `offsetof`. Same treatment for `d_surf.c`'s
`(int)&((surfcache_t *)0)->data[size]`.

### `d_edge.c` — a pointer through an `int`

```c
D_DrawSolidSurface (s, (int)s->data & 0xFF);
```

`r_drawflat` colours each surface by the low byte of its `msurface_t` pointer.
Harmless in intent, but it has to go through an integer wide enough to hold
one. Now `(uintptr_t)`.

### `net_udp.c` — declarations that disagree with the C library

```c
extern int gethostname (char *, int);
extern int close (int);
```

These predate `<unistd.h>` being somewhere you could rely on. glibc's
`gethostname` takes a `size_t`, which on LP64 is a different size, and the
conflict is a hard error. Removed in favour of the header, with
`<arpa/inet.h>` added for the `inet_addr` that was being called without a
prototype.

### `chase.c` — an implicit declaration

`SV_RecursiveHullCheck` is declared in `world.h`, which `chase.c` does not
include. It returns `qboolean`, which is the same width as the `int` an
implicit declaration assumes, so it worked — and a compiler that treats an
implicit declaration as the error C99 made it will not build the file at all.

### `common.c` — unbounded path construction

`COM_WriteFile` and `COM_FindFile` build `"<gamedir>/<file>"` with `sprintf`
into `char[MAX_OSPATH]`. `MAX_OSPATH` was 128, which was generous for a 1996
filesystem and is not generous for a container where the user chooses the mount
point. Raised to 512 and the writes bounded.

### `sv_move.c` — `abs()` on a float

```c
if ( ((rand()&3) & 1) ||  abs(deltay)>abs(deltax))
```

`abs` converts to `int` first, so a monster comparing deltas below 1.0 saw 0
for both. This is what the 1996 build did as well — `<stdlib.h>` was in scope
there too — so the truncation stays, and is now written out explicitly. Changing
it would change how monsters walk, which is not a porting decision.

---

## Things that no longer exist

### `sys_linux.c` — the clock

`Sys_FloatTime` used `gettimeofday`, which is the wall clock. It steps when NTP
corrects it and when the host suspends, and the engine reads a step as elapsed
frame time: backwards it stops dead, forwards it runs a single frame of physics
for however long the jump was. In a container, on a host that sleeps, this is
not hypothetical. Now `CLOCK_MONOTONIC`, falling back to `gettimeofday` if that
somehow fails.

This matters more than it looks, because `snd_stream.c` derives the playback
position from the same clock.

### `sys_linux.c` — the frame loop span at 100% of a core

`Host_Frame` returns without doing anything until a frame's worth of time has
passed, so the loop simply spins. On a desktop in 1996 that was the whole
machine anyway. In a container it is a core pinned at 100% whether or not
anything is happening, which on a shared host is somebody else's problem as
well as yours. Now it sleeps out the remainder of the frame; `sys_nosleep 1`
puts the old behaviour back.

Measured at the console, 640x480: 100% of a core before, 1.9% after.

### `sys_linux.c` — 8 MB of heap

```c
parms.memsize = 8*1024*1024;
```

Every pointer in the model, edict and surface caches is twice the width it was,
and the hunk is where all of them live. The high end of the same hunk holds the
z-buffer and the surface cache, which at 1920x1200 are 12 MB between them where
at 320x200 they were under a megabyte. Now 64 MB, with `-mem <megabytes>` still
overriding it and a floor at `MINIMUM_MEMORY`.

`malloc` failing is also checked now, rather than the hunk being initialised on
a null pointer.

### `vid_x.c` — `XSynchronize(x_disp, True)`

Left on in the release, with "for debugging only" written above it. It makes
every Xlib call a round trip to the server and waits for the reply. Over a
socket to a VNC-backed Xvfb that is the difference between a playable picture
and a slideshow. Now behind `-verbose`.

### `vid_x.c` — nothing installs the colormap

The engine creates a private colormap at depth 8 and calls
`XSetWindowColormap`, which states a preference. Something has to *install* it
in the hardware, and on a desktop that is the window manager's job. The
container runs one Xvfb and one window and no window manager, so nothing
installed it and everything reading the display got the right palette indices
through the root window's default 256 colours. `XInstallColormap` added.

x11vnc's `-8to24` reads each window's own colormap and would have got this
right either way; a native VNC client at depth 8, and `xwd`, would not.

### `vid_x.c` — a random shared memory key

```c
key = random();
x_shminfo[frm].shmid = shmget((key_t)key, size, IPC_CREAT|0777);
```

`random()` collides, and a collision here hands you somebody else's segment
rather than failing. Now `IPC_PRIVATE` and mode 0600. The failures are checked
too: `shmat` returning `(void *)-1` was being stored and used.

### `vid_x.c` — signal handling from inside Xlib

```c
void TragicDeath(int signal_num)
{
        XAutoRepeatOn(x_disp);
        XCloseDisplay(x_disp);
        Sys_Error("This death brought to you by the number %d\n", signal_num);
}
```

`Sys_Error` runs `Host_Shutdown`, which writes `config.cfg`, shuts the sound
down and calls back into Xlib — on a connection this handler has just closed,
from a context where none of `malloc`, stdio or Xlib may be called at all. It
got away with it when the signal arrived at an idle moment and segfaulted when
it did not, which under `docker stop` is most of the time; the config the
player just changed is lost either way.

The handler now raises a flag and the frame loop acts on it, which is the only
thing a handler may safely do. `sigaction` is also given a zeroed `struct
sigaction` — the original read the current disposition into it first and
inherited whatever flags came back.

`WM_DELETE_WINDOW` is handled for the same reason: closing the window used to
end the process from inside Xlib's default I/O error handler, with the
connection already gone.

### `vid_x.c` — the mouse, for a client that cannot send relative motion

The VNC protocol carries absolute pointer positions. A game needs deltas. The
page locks the pointer and walks the remote pointer around the screen by the
movement it sees, putting it back in the middle before it would hit an edge —
and a report landing exactly on the centre is the re-base, not a movement.

The engine already had the delta path (`mouse_x = x - p_mouse_x`) for
`_windowed_mouse 0`. What it did not have was a way to tell a re-base from a
hard flick in the opposite direction. One test for the centre, in
`MotionNotify`, is the whole of the change; `_windowed_mouse 1` still does the
1996 grab-and-warp for a real X display.

### `vid_x.c` — the resolution was never checked

`-width` and `-height` were taken as given. The span drawers step the
framebuffer eight pixels at a time and the renderer's static tables are sized
by `MAXWIDTH` and `MAXHEIGHT`, so a width the caller picked freely either drew
a sheared picture or wrote off the end of `d_scantable`. Now clamped and
rounded, with a line saying so.

`MAXWIDTH` and `MAXHEIGHT` themselves went from 1280x1024 to 1920x1200, which
costs about 90 KB of bss and a slightly deeper frame, and makes 1080p possible.

A depth-8 visual that is not PseudoColor is now a clear error rather than a
picture in whatever 256 colours the server chose, and any other depth says
which one it got and that it is translating every frame.

A size arriving from `ConfigureNotify` was not clamped at all, only rounded,
and the resize path never recomputed `vid.aspect`. Both fixed.

### `vid_x.c` — only the first shifted character of a session arrived

Typing `vid_width` at the console produced `vidwidth`. So did every later
capital letter, colon and quote: each shifted character worked exactly once per
run and was then silently gone.

`XLateKey` called `XLookupString` on the event as it stood, so the keysym came
back with the modifier state applied — shift+minus as `XK_underscore`, key 95.
On the release, shift is already up, so the same physical key came back as
`XK_minus`, key 45. `Key_Event` counts autorepeats:

```c
	if (!down)
		key_repeats[key] = 0;
	...
	key_repeats[key]++;
	if (key != K_BACKSPACE && key != K_PAUSE && key_repeats[key] > 1)
		return;			// ignore most autorepeats
```

The press incremented `key_repeats[95]`; the release cleared
`key_repeats[45]`. Nothing ever cleared 95 again, so from the second `_`
onwards every one looked like a held key and was dropped.

The keysym is now looked up with `ShiftMask` and `LockMask` taken out of a copy
of the event. That is what the engine expects — `keys.h` says "normal keys
should be passed as lowercased ascii", `keys.c` carries a `keyshift[]` table to
apply shift itself, and a binding belongs to a physical key rather than to the
character it happens to produce. The disabled block of hand-written
`case 0x05f: key = '-'` lines a little further down `XLateKey` is the 1996
attempt at the same problem; it is left where it is, as a comment on the fix.

### `vid_x.c` — a resize crashed the engine when the new mode was larger

The backtrace, from the handler added in 1.0.1:

```
Draw_ConsoleBackground <- Con_DrawConsole <- SCR_DrawConsole
  <- SCR_UpdateScreen <- Con_Printf <- D_InitCaches
  <- ResetSharedFrameBuffers <- VID_Update <- SCR_UpdateScreen
```

`SCR_UpdateScreen` twice in one stack. `D_InitCaches` announces the new surface
cache size with `Con_Printf`, and `Con_Printf` draws the screen when the
console is up — re-entering the renderer from the middle of `VID_Update`'s
reallocation, at the one moment when `vid.width` is already the new size and
`vid.buffer` is still the old, smaller framebuffer. `Draw_ConsoleBackground`
wrote a 640-pixel row into a 512-pixel one. Growing crashed; shrinking did not,
which is why it took a resize in the right direction to find.

`Con_Printf` does guard against this, with an `inupdate` flag — but only
against itself. Here the outer `SCR_UpdateScreen` was not entered through
`Con_Printf`, so the flag was clear.

The fix is the engine's own: `block_drawing` is checked by the first line of
`SCR_UpdateScreen` and set by `vid_win.c` around a mode change for exactly this
reason. Nothing in this build had ever set it.

### `vid_x.c` — no video menu, and no way to change resolution

`menu.c` has had the whole video menu in it since 1996: an `m_video` state,
`M_Menu_Video_f`, and an Options line that is drawn only
`if (vid_menudrawfn)`. The X11 driver never assigned it, so the X build's
Options menu simply had no Video Options line, and the resolution was whatever
the command line said, for the life of the process.

Both function pointers were also *defined* in `vid_x.c` as well as in `menu.c`,
and only `-fcommon` merged the duplicate definitions into one symbol. `menu.c`
owns them; `vid_x.c` declares them `extern`.

The mode list is twenty sizes from 320x240 to 1920x1200, every width a multiple
of eight, filtered against what the X server says it will accept. Choosing one
writes the archived `vid_width` and `vid_height` cvars and nothing else;
`VID_Update` notices on the next frame and applies them. That is one code path
for the menu, for the console, and for `config.cfg` — which is also what makes
the choice persist, since the engine rewrites `config.cfg` in full on exit.
Because the cvars are archived and `VID_Init` runs long before `quake.rc` is
executed, the command line is the default and the config wins.

### `vid_x.c` — resizing the X screen, not just the window

The browser is shown the whole root window, so a window smaller than the screen
would sit in the corner of a framebuffer it cannot fill, with the rest black.
Changing resolution has to move the screen too.

RANDR does that, with two catches worth writing down.

**A server's maximum screen size is fixed when it starts.** Xvfb reports
`maximum 640 x 480` when started at 640x480, and `XRRAddOutputMode` answers a
larger mode with `BadMatch`. So the container starts Xvfb at 1920x1200 and the
engine shrinks the screen to the resolution in use; every mode below the
ceiling is then reachable.

**Xvfb starts knowing exactly one mode**, the size it was given, so every other
resolution has to be created with `XRRCreateMode` and `XRRAddOutputMode` before
a CRTC will take it. The timings are invented — nothing here drives a pixel
clock — but the server rejects a mode whose totals do not bound its visible
area.

Then, in order: when growing, `XRRSetScreenSize` first, because a CRTC will not
take a mode the screen cannot hold; `XRRSetCrtcConfig`; `XRRSetScreenSize`
again at the exact size, which is what does the work when shrinking. Every one
of these calls goes through a temporary `XSetErrorHandler`, because Xlib's
default handler answers `BadValue` or `BadMatch` by calling `exit()` — a menu
selection must not be able to end the process — and the result is judged by
asking the server what size the root window actually is rather than by
believing the request.

Two things that cost time here:

* **`XRRSizes` and `XRRSetScreenConfig`, the RANDR 1.1 interface, are a dead
  end on Xvfb.** They see the modes, return `RRSetConfigSuccess`, and leave the
  screen exactly where it was. Only the 1.2 CRTC interface actually moves it.

* **`DisplayWidth` and `DisplayHeight` do not follow a resize.** They read a
  value Xlib cached when the connection opened, and it is only updated by
  feeding each `RRScreenChangeNotify` to `XRRUpdateConfiguration`. A resize that
  had worked perfectly well read back as no change at all, which is what made
  the 1.1 interface look like it might be worth persisting with. The engine
  asks with `XGetGeometry` on the root, and handles the event as well so that
  nothing else is misled.

Only done when `-resizescreen` says the engine owns the display, which the
container's entrypoint passes and a desktop does not: picking a resolution in
Quake's menu has no business rearranging somebody's other windows.

### `vid_x.c` — `MappingNotify` was ignored

Xlib caches the keyboard mapping when the connection opens and rereads it only
when asked. x11vnc types a character the keymap does not have by binding it to
a spare keycode, sending it and putting the keymap back, so without
`XRefreshKeyboardMapping` those characters arrive as whatever the stale cache
says that keycode used to mean.

---

## New files

### `WinQuake/snd_stream.c` — a DMA backend with no DMA

The whole of Quake's sound backend interface is three questions — how big is
the buffer, where is the playback pointer, and here is some more audio — and
none of them need hardware to answer. This one allocates a ring, starts a
monotonic clock, and answers "where is the playback pointer" with "where it
would be if the clock were the card". `SNDDMA_Submit` hands the pipe whatever
has come due since last time.

Three details that are not obvious:

* **The FIFO is opened read-write.** Opening the write end of a FIFO blocks
  until a reader turns up, and with `O_NONBLOCK` fails outright with `ENXIO`
  instead — so starting the engine a moment before `audiostream` would mean no
  sound for the rest of the session. Holding a read end of our own also stops a
  `SIGPIPE` killing the engine when the reader restarts.

* **It never sends past `paintedtime`.** Beyond the mixer's write pointer the
  ring still holds whatever was there a buffer ago, and sending it plays the
  last second of the game again — which is what a map load would sound like.
  The gap goes out as silence instead, so the listener's clock stays in step.

* **A short write is kept, in whole frames.** What the pipe will not take is
  offered again next frame. A partial write that is not a multiple of four
  bytes leaves the listener assembling every sample after it from the wrong
  pair of bytes, which is not a glitch but full-scale noise that never
  recovers.

The ring is 16384 frames because `S_TransferPaintBuffer` indexes it with
`paintedtime * channels & (shm->samples - 1)`, which is only a modulo when the
size is a power of two.

### `WinQuake/cd_stream.c` — music from files

`CDAudio_Play(track, looping)` opens `<musicdir>/track02.ogg` and friends,
numbered the way the CD was, and `CDAudio_MixPaintBuffer` adds the decoded
samples to the mixer's paint buffer just before it is scaled and clipped.
Decoding is libsndfile's; resampling is linear, which for 44100 to 22050 is
almost always a straight 2:1 decimation and inaudible on music.

One behavioural difference, and it is unavoidable: the master volume now
applies to the music. On the CD the music never went through the mixer, so
`volume` could not touch it. Here it is part of what the mixer mixes.

The `cd` console command is kept, minus `eject`, `close` and `reset`, which
only ever meant something to a drive; those now say so rather than failing
silently.

### `audiostream/` — a pipe to a socket, keeping time

Reads the engine's FIFO one period (256 frames, 12 ms) at a time on an absolute
`clock_nanosleep` schedule, and fans the result out to up to four listeners.
Pads with silence when the engine is behind; keeps a per-listener backlog and
drops from the front of it, in whole frames, when a listener falls behind.

It exists as a separate process rather than a thread so that a browser
reconnecting cannot stall the game loop, and so that the engine restarting does
not drop the browser's socket.

### `WinQuake/Makefile`, `docker/`, `tools/`

The build, the container and its client, and a smoke test that makes its own
game data. `tools/make-test-data.py` is the only interesting one: see the
comment at the top of it for why it pads its pak directory to 339 entries and
steers its CRC.

---

## How this was tested

`tools/smoke-test.sh` builds its own game data and runs the engine as far as
the console, which is as far as anything without `progs.dat` can go. That is
what CI runs, and it is not enough on its own: it passed for hours while
`SV_SpawnServer` was still broken, because it never starts a server.

So the script has a second phase, `QUAKE_SMOKE_DATA=/path/to/quake`, which
loads E1M1. The shareware `pak0.pak` is enough for it — that pak has
`progs.dat`, the models and E1M1 to E1M8 — and it is what the bug above was
found with. Its two assertions are chosen against the failures actually seen:

* **The server version line.** `VERSION 1.09 SERVER (24778 CRC)` means
  `progs.dat` loaded and its CRC matched `progdefs.h`. Its absence is how a
  QuakeC problem shows up.
* **How many colours a frame holds.** A map whose texture basis is garbage
  still draws — walls, floors, a status bar — so "did not crash" says nothing.
  A frame of correctly mapped, correctly lit Quake uses most of a 256-colour
  palette; flat-shaded wreckage does not. This is the assertion that would have
  caught the `Mod_LoadTexinfo` bug.

The first phase also starts Xvfb deliberately larger than the window it asks
for, so that the screen resize is something the test can watch happen, and then
changes resolution at the console and checks the screen followed — down to
512x384 and back up to 640x480. That second transition is what found the
`block_drawing` crash, on the first run after the check was added.

Beyond the script, by hand: the three demos in the shareware pak, played
through; E1M1 and E1M2 walked, shot and saved, with `save`, `load`, `kill`,
`map` and `changelevel`; 640x480 and 1280x800.

The video menu, driven with `xdotool` and read back three ways: the root
window's real size from a fresh X connection, a screenshot of the window taken
with its own colormap, and — because what matters is what the browser is told —
a small RFB client that connects to x11vnc and prints every `NewFBSize` it
receives. Eight mode changes in a row with E1M1 loaded, up and down across the
whole range, each one arriving at the client with the right dimensions.

And the image itself, built and run: `docker run -p 6080:6080 -v
<data>:/quakedata:ro -v quake-state:/quake/state`, with a frame pulled through
the published port over the browser's own WebSocket and the sound read with the
header the page reads. What that covers and the native runs do not is the
Dockerfile — the package names, unpacking noVNC out of its `.deb` rather than
installing it, the forced removal of Mesa and numpy, the version stamp — and
the container's own edges: `docker stop` reaching the engine through tini and
`config.cfg` landing in the volume, the state surviving as uid 1001, and a
container with nothing mounted stopping with an explanation and status 1.

## What was deliberately left alone

* **The 8-bit palette and the software renderer.** They are the point.
* **`MAX_EDICTS`, `MAX_MODELS`, the lightmap grid, the protocol.** Raising any
  of them makes this a different engine and breaks compatibility with the
  demos and savegames the release came with.
* **Monster movement, physics, the QuakeC interpreter.** Untouched, including
  the `abs()` truncation above.
* **`QW/` and `qw-qc/`.** QuakeWorld is a separate program with its own copies
  of most of these files, and none of the work above has been applied to it.
* **The per-file 1996 copyright headers.** They say GPL because id relicensed
  them in 1999; the text is theirs and is left as it is.

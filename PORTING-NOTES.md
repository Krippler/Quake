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

### `r_draw.c` — a NaN walked through every clamp and indexed 16 GB out

`R_EmitEdge` projects each vertex, clamps the result to the viewport, takes
`ceil()` of it and uses that as a scanline index into `newedges[]` and
`removeedges[]`. The clamps are also the safety net, and they are written the
obvious way:

```c
	if (v0 < r_refdef.fvrecty_adj)      v0 = r_refdef.fvrecty_adj;
	if (v0 > r_refdef.fvrectbottom_adj) v0 = r_refdef.fvrectbottom_adj;
	ceilv0 = (int) ceil(v0);
```

Every comparison against a NaN is false. A NaN therefore passes *both* clamps
untouched, `ceil()` hands back a NaN, and the conversion to `int` is undefined —
on x86-64 it is `INT_MIN`. `newedges[INT_MIN]` is 16 GB below the array.

That is not theoretical: it came in as a SIGSEGV in `R_EmitEdge` on a
remastered Scourge of Armagon map, and the reported fault address was exactly
16.00 GB below the text segment, which is `INT_MIN * sizeof(edge_t *)`.

The clamps are negated — `if (!(v0 > lo)) v0 = lo;` — so a NaN takes the
assignment instead of skipping it. For every finite value the two spellings do
the same thing, which the rendered frame confirms: byte-for-byte identical
before and after.

Where the NaN comes from is upstream and map-specific; `R_RecursiveClipBPoly`
interpolates with `frac = dist / (dist - lastdist)`, which is `0/0` for a
degenerate edge. The fix does not depend on finding it. There is also a check
on the two indices immediately before they are used, so no arithmetic anywhere
above can put a write outside the arrays — it costs two comparisons on a path
that already does a division.

Worth noting what this was *not*. The first theory was the edge pool:
`R_RenderBmodelFace` reserves `psurf->numedges + 4` but then walks the polygon
*after* `R_RecursiveClipBPoly` has split it, which is a longer chain. That is a
real discrepancy — measured at 2 over on id's maps, inside the 4 of headroom —
but it was not this crash, and the fault address is what ruled it out.

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

### `vid_x.c`, `r_main.c` — every mode was drawn as 4:3

`vid.aspect` is the shape of one pixel. `R_ViewChanged` takes it as
`pixelAspect`, and the two places that matter are

```c
	screenAspect = r_refdef.vrect.width * pixelAspect / r_refdef.vrect.height;
	...
	yscale = xscale * pixelAspect;
```

id computed it as `(vid.height / vid.width) * (320.0 / 240.0)`. Substitute that
into the first line and the width cancels: `screenAspect` is 4:3 for every mode
there has ever been. That was right in 1996 — every mode was 4:3, and 320x200
was displayed on a CRT that made the pixels taller than they were wide, which
is what the `320/240` says. It is wrong for a framebuffer in a browser, where a
pixel is square: the renderer drew a 4:3 picture and the browser showed it at
16:9.

Measured, as the ratio of the renderer's own vertical scale to its horizontal
one — 1.0 is a square pixel:

| mode | before | after |
| --- | --- | --- |
| 640x480, 1280x960 | 1.0000 | 1.0000 |
| 1280x800 | 0.8333 | 1.0000 |
| 1920x1080 | 0.7500 | 1.0000 |

The error is exactly the mode's aspect divided by 4:3, which is what "a quarter
too wide" looks like at 16:9.

With square pixels and a fixed horizontal `fov`, a wider screen renders the
same width and crops the top and bottom off — a narrower view than the mode it
replaced, which is not what picking a widescreen resolution is for. So
`R_ViewChanged` widens `horizontalFieldOfView` by however much wider than 4:3
the screen is, which leaves the vertical field of view exactly where 4:3 puts
it.

Two things that had to be got right:

* **Measured against the screen, not `screenAspect`.** `screenAspect` is the
  viewport, and the status bar takes height off it, so it is wider than the
  screen even at 640x480 — 1.48 rather than 1.33. Keying the widening to it
  widened the view at 640x480 as well, where nothing should change at all. The
  first version of this did that, and 640x480's horizontal scale went from 320
  to 288 before the table above caught it.

* **`r_fov_greater_than_90` is left alone.** It reads the `fov` cvar, and all
  it does is hide the weapon model, which looks wrong past 90 degrees. Basing
  it on the widened angle instead would have made the weapon disappear on every
  widescreen mode.

### `menu.c` — three switch statements that had to agree

The options menu drew each row in one `switch`, adjusted it in a second and
acted on Enter in a third, and a row's identity was its position in all three:

```c
	M_Print (16, 56, "           Screen size");    // row 3, by counting
	...
	case 3:	// screen size
		scr_viewsize.value += dir * 10;
	...
		case 3:	// screen size  -- no, Enter fell through to M_AdjustSliders
```

Adding a setting meant editing three places and renumbering everything below
it in each. That is why every setting id added after 1996 went to the console
and stayed there, and why everything this port added — `freelook`, the sound
delay, the field of view that widescreen made worth changing — was
console-only too.

One table now. A row says what it is and which cvar it moves; the drawing and
the adjusting are written once against that, and the list scrolls the way the
controls menu does. Twenty-three rows, and adding one is a line.

Three things the rewrite had to get right:

* **Rows with nothing behind them.** `-nosound` makes `S_Init` return before it
  registers `volume`, `bgmvolume` and `_snd_mixahead`, so those three rows
  moved cvars that were not there — and `Cvar_Set` answers a name it cannot
  find with `Cvar_Set: variable volume not found`, once per press of an arrow
  key. id's menu did exactly that. A row now checks `Cvar_FindVar` first, shows
  `n/a` and does nothing.

* **Settings that did not last.** `fov`, `r_drawviewmodel`, `cl_bob`,
  `v_kicktime`, `r_waterwarp` and `d_mipcap` were not archived, because a
  console setting was not expected to survive a restart. A row in a menu is:
  archived now, so it is written to `config.cfg` on the way out.

* **`d_mipcap` did nothing.** It was read in `D_InitCaches`, which runs when
  the video mode changes and at no other time, so the cvar appeared to be
  ignored until a resolution change applied it as if by accident. Read in
  `D_SetupFrame` instead, once a frame.

`r_dynamic` and `r_shadows` were on the list until a check showed they are
declared only in `glquake.h` — GL-only, and not registered in this build at
all. They would have been two rows that did nothing.

### `vid_x.c` — `MappingNotify` was ignored

Xlib caches the keyboard mapping when the connection opens and rereads it only
when asked. x11vnc types a character the keymap does not have by binding it to
a spare keycode, sending it and putting the keymap back, so without
`XRefreshKeyboardMapping` those characters arrive as whatever the stale cache
says that keycode used to mean.

### `r_main.c` — the weapon was not drawn above a 90 degree field of view

`R_DrawViewModel` returned on `r_fov_greater_than_90`, which `R_ViewChanged`
sets straight from `scr_fov`. id's reasoning holds: the view model sits a foot
from the camera, where a wide projection stretches it across the screen and out
through the wall behind it. What changed is that the field of view is a slider
now rather than a console command nobody found, so the failure mode is a player
moving it and watching their weapon disappear.

Answered the way later ports answer it. The view model is drawn with its own
field of view, fixed at 90 and widened for the screen exactly as the world's is
in the Hor+ change above, so the gun keeps the size it has at 90 whatever the
world is drawn at. The software renderer reads the projection out of four
globals — `xscale` and `yscale` for the bounding-box check, `aliasxscale` and
`aliasyscale` for the vertices — and nothing but the view model is drawn between
saving them and putting them back.

### `common.c`, `sys_linux.c`, `menu.c` — no way to change game from inside

`-game`, `-hipnotic` and `-rogue` are read once, in `COM_InitFilesystem`, and
nothing rebuilds `com_searchpaths` afterwards. So a mission pack or a mod meant
stopping the container, editing a variable and starting it again, which for
something as ordinary as trying a mod is a lot to ask of somebody playing in a
browser.

Switching the path underneath a running game is not the answer: every model,
sound, texture and progs in the hunk belongs to the old path, so it would mean
throwing all of it away and loading it again — most of what starting over does,
with none of the certainty. The menu writes the chosen directory to a file
beside the game directories and quits instead. `COM_InitFilesystem` reads it
when the command line says nothing, and applies exactly what the switch for that
directory would have, `rogue` and `hipnotic` flags included, because those two
change the status bar and the menu as well as the search path. The container's
restart loop and the page's own reconnect make the restart a few dark seconds.

`Sys_ListGameDirs`, `Sys_SetGameChoice` and `Sys_GetGameChoice` are in
`sys_linux.c` because listing a directory is `opendir` here and `FindFirstFile`
on Windows; a revived `sys_win.c` would need its own three.

### `snd_dma.c` — the playback position was reconstructed, and got it wrong

`GetSoundtime` derives `soundtime` from `SNDDMA_GetDMAPos`, which is a position
inside the ring rather than a running count, by incrementing a wrap counter each
time the position goes backwards. That is one wrap per call, and id's comment
above it is candid about the consequence: *"it is possible to miscount buffers
if it has wrapped twice between calls to S_Update. Oh well."*

The ring here is 16384 frames, 0.74 s at 22050 Hz. Two wraps between calls means
a frame longer than a second and a half, which on a 1996 machine meant it had
stopped. It is not exotic here: a level load takes several seconds, and this
port logs picture gaps of one to eighteen seconds on a busy host as a matter of
course.

What made it fatal rather than cosmetic is what sits downstream. `paintedtime`
follows `soundtime`; `SNDDMA_Submit` will not send past `paintedtime`, because
beyond it the ring still holds the previous buffer's audio. So once `soundtime`
was a ring or more behind the clock, every frame was padded with silence instead
of carried from the mixer — permanently, and at exactly the right rate, so the
listener's buffer never underran and nothing anywhere had reason to complain.
The symptom was "the sound stopped", with a healthy log.

Measured: a 3.5 s level load in the demo loop left `paintedtime` 63331 frames
(3.87 rings) behind, and it was still exactly 63331 behind twenty seconds later.

`snd_stream.c` has no card and no wrapping to reconstruct — its clock is the
playback position — so it now offers `SNDDMA_GetSamples`, the running count,
and `SNDDMA_RebaseClock` to keep that count inside an `int` past twenty-seven
hours of uptime. `GetSoundtime` uses them when the backend declares
`SND_HAS_GETSAMPLES`, which the Makefile defines beside the choice of backend.
The reconstruction is still there for the OSS backend, which has a real DMA
pointer and no alternative.

### `cd_stream.c` — one music directory for every game

The music search put `$QUAKE_MUSICDIR` first, ahead of `<gamedir>/music`. That
is the wrong way round once there is more than one game installed: the mission
packs have soundtracks that are not Quake's, a rip of each goes beside its own
paks, and an environment variable can only ever name one directory for all of
them. The container made it concrete by setting `$QUAKE_MUSICDIR` to
`id1/music` whenever that existed, so Scourge of Armagon played Quake's music.

The order is `-musicdir` (explicit, one run), `<gamedir>/music` (specific),
`$QUAKE_MUSICDIR` (the cross-game default), `<basedir>/id1/music` (so a mod
with no music of its own still gets Quake's). The entrypoint no longer sets
`$QUAKE_MUSICDIR` from `id1/music`, because the last of those already covers it.

`CDAudio_DirHasFiles` also only checked `S_ISDIR`, so an empty `music`
directory won the search and produced silence with the fallbacks unreached. It
looks for a `track*` file in a readable format now.

### `model.c`, `model.h`, `world.c` — the second map format

BSP2 is what the Quake re-release ships and what every modern compiler emits.
It is not a redesign: the same fifteen lumps, with the indices and bounds
widened past what a short holds, which is the limit it exists to escape. Nine
lumps are byte-identical between the two; six are not.

| lump | BSP29 | BSP2 |
| --- | --- | --- |
| nodes | short children, short bounds | int children, float bounds |
| leafs | short bounds, ushort marksurfaces | float bounds, uint marksurfaces |
| faces | short planenum/side/numedges/texinfo | int throughout |
| clipnodes | short children | int children |
| edges | ushort vertices | uint vertices |
| marksurfaces | ushort | uint |

The readers take a branch on `loadmodel_bsp2`, set once from the version field.
The in-memory structures widened to match — `medge_t.v`, `mnode_t`'s surface
range, and the culling bounds, which are floats now because BSP2's are.

The part that is not mechanical is collision. id traced against `dclipnode_t`,
the on-disk record, directly: `hull_t.clipnodes` pointed into the loaded lump.
BSP2's on-disk clipnode has int children where BSP29's has short, so neither
can be the runtime type any more. Both are read into an `mclipnode_t`, and
`SV_HullPointContents` and `SV_RecursiveHullCheck` trace against that.

`MAX_MAP_LEAFS` went from 8192 to 65536. Four static PVS bitvectors are sized
from it — `mod_novis`, the decompression scratch, `checkpvs` and `fatpvs` — at
8 KB each, which was a quarter of a 1996 machine and is nothing now.

#### Proving it

A map is the same map in either layout, so the two readers have to produce the
same model. `tools/bsp29to2.py` rewrites a BSP29 map as BSP2 and `bspchecksum`
CRCs what the reader built, with pointers turned into indices because those are
hunk addresses and differ between runs by construction. All nine shareware maps
give identical checksums through both paths.

Comparing rendered frames was the first attempt and it does not work: Quake
animates textures and entities against the clock, so two runs of the *same* map
differ. Five of nine maps "failed" that way before the control run — the same
map against itself — showed the method was measuring the clock. On e1m1, whose
opening view happens to hold nothing animated, the frame is byte-for-byte
identical through both readers, and the player settles at the same position,
which is the collision hulls agreeing.

`tools/make-pop-pak.py` exists for that test. Until `COM_CheckRegistered`
succeeds, `COM_FindFile` refuses any name with a '/' in it when it reaches a
directory, so a test cannot drop a map in `maps/` and load it. The check reads
`gfx/pop.lmp` and compares it against the `pop[]` table compiled into
`common.c` — so the file it wants is fully described by the source, and the
tool writes it. It unlocks the loose-file path and nothing else; there is no
game content in 256 bytes of checksum table.

### `r_main.c` — the frame pools were sized for 1996

`surfaces[]` and `r_edges[]` hold what the renderer has accepted for the frame
it is building. They are not per map: they are reset every frame, so what
matters is how much is visible at once. id sized them at 800 and 2400, and
measured across the shareware episode at 800x600 the peak use is 458 surfaces
and 1162 edges — comfortable, for those maps.

When they run out, `R_RenderFace` and `R_RenderBmodelFace` return before
emitting anything and count it in `r_outofsurfaces` / `r_outofedges`. The face
is not drawn. There is no error and no fallback; the wall is just not there.

A map built for the Quake re-release puts far more in view at once, so the
machine campaigns exhausted both and rendered with holes in them. The defaults
are 32768 and 131072 now, which is past `NUMSTACKSURFACES` and `NUMSTACKEDGES`
and therefore off the stack and onto the hunk — what those two constants are
for. About 10 MB, measured as 15.7 MB to 25.7 MB resident, and the heap grew
from 64 MB to 192 MB to hold it alongside a BSP2 map. That heap is one malloc
and `Memory_Init` only records where it starts, so the pages a small map never
reaches are never resident: the virtual size grows and the footprint does not.

The worse half was that it happened silently. `r_reportsurfout` and
`r_reportedgeout` both default to 0, so the engine dropped geometry and said
nothing, and the only symptom was a view with parts of the level missing. It
now says so once per map and names the two cvars to raise.

Reproduced by building with the pools cut to 64 and 200 — most of e1m3's
opening room renders black with the torches hanging in the void, which is what
the report described.

### `client.h`, `quakedef.h`, `net.h`, `server.h` — the rest of the 1996 sizes

The frame pools were the visible half. Four more limits sit around them, and
they fail in four different ways:

`MAX_STATIC_ENTITIES` (128) is a `Host_Error` in `CL_ParseStatic` — the map
stops loading and you land at the console. That one at least says something.

`MAX_EFRAGS` (640) is not a count of entities. `R_AddEfrags` walks the BSP and
links the entity into every leaf it touches, one efrag each, so a single torch
in an open doorway takes several. Out of them, `R_SplitEntityOnNode` prints and
returns: the entity is not drawn *in that leaf*, so it appears and disappears
depending on where you stand. The print was inside the tree walk, which is why
one starved map produced hundreds of identical lines a frame and scrolled
everything else away. It is one line per map now.

`MAX_VISEDICTS` (256) is the quietest. The callers test it and stop adding,
with no counter and no message — entities past the 256th are simply absent.

`MAX_EDICTS` (600, and id's comment on it was "FIXME: ouch! ouch! ouch!") is a
`Sys_Error` from `ED_Alloc`: the map does not load at all. Nothing in the
protocol required 600 — `SV_WriteEntitiesToClient` sets `U_LONGENTITY` and
writes a short when the number needs one, so the wire format reaches 32767.

The one that cannot move is `MAX_MODELS`, and `MAX_SOUNDS` with it. Those
indices go out as bytes, in `svc_spawnbaseline` among others. 256 is the
format, not a buffer.

Raising the statics needed the buffers under them. Every static entity is
written into `sv.signon` during `SV_SpawnServer`, alongside a baseline for
every entity with a model, and `sv.signon` has `allowoverflow` clear — so
`SZ_GetSpace` calls `Sys_Error`. Going past 128 statics with an 8192-byte
signon buffer would have replaced a clean `Host_Error` with a hard exit.
`MAX_MSGLEN` and `NET_MAXMESSAGE` grew with it. `Datagram_SendMessage` already
splits a reliable message into `MAX_DATAGRAM` pieces behind a 32-bit length, so
the fragmenting did not have to change — but `Loop_SendMessage`, which is what
single player actually uses, appends into a fixed buffer and calls `Sys_Error`
rather than refusing when the next message will not fit. `NET_MAXMESSAGE` is
therefore two whole messages wide, which is the worst case with one reliable
message in flight at a time.

The signon buffer is what really caps the static entities, at 14 bytes each on
top of 16 for every baseline. `SV_SpawnServer` measures it and says so when a
map comes close, because the alternative is `SZ_GetSpace` calling `Sys_Error`
about a buffer the reader has no reason to have heard of.

Measured cost of all of it: 25.4 MB resident to 27.6 MB.

### `r_draw.c`, `r_main.c` — two failures counted as one

1.5.1 added a report for a frame that did not fit. It was wrong about one
case. `r_outofedges` was incremented both where the edge pool is genuinely
full and in the backstop that drops an edge whose scanline index came out
off the screen — and only the first has anything to do with `r_maxedges`.

A map that dropped six edges the second way reported "short 4 edges" against
a pool of 131072, and told the reader to raise a number that was already two
orders of magnitude larger than the frame needed. The backstop has its own
counter now, and its own message, which says that `r_maxedges` is not the
answer.

### `snd_dma.c` — a null dereference when there is no sound card

`S_Init` calls `S_Startup`, and `S_Startup` leaves `shm` NULL if `SNDDMA_Init`
fails. Twenty lines later:

```c
	Con_Printf ("Sound sampling rate: %i\n", shm->speed);
```

`speed` is at offset 0x20, which is exactly where the fault lands. Any machine
that cannot open a sound device gets a SIGSEGV during startup rather than a
silent game — and the rest of the engine was always ready for a silent game,
since `sound_started` stays false and every entry point tests it.

It survived this port because the container always provides the fifo the
backend writes to, so the failing branch was never taken. The smoke phase
added for visual depth runs the engine without `QUAKE_AUDIO_FIFO`, which took
it on the first try.

### `r_bsp.c` — the door that is not there

`R_DrawSolidClippedSubmodelPolygons` clips one brush model against the view in
two stack buffers, `MAX_BMODEL_VERTS` (500) and `MAX_BMODEL_EDGES` (1000). Over
either, `R_RecursiveClipBPoly` returns. The model is not partly drawn — it is
absent, which for a lift or a door is more confusing than a hole would be.

The vertex limit returned silently. The edge limit printed, and printed from
inside the clipping walk, so a map whose machinery exceeds it prints hundreds
of lines a frame. That is not just noise: the printing showed up as the picture
stalling for hundreds of milliseconds. Both are reported once a map now, and
the buffers are 8192 and 16384 — 98 KB and 393 KB in a frame that is not
recursive.

### `r_main.c`, `r_shared.h` — how far the surface pool can actually go

The machine campaigns came up 4128 surfaces short of 32768 on one frame, so
1.5.1's figure was not enough. The ceiling is not memory: `edge_t` carries the
index of the surface an edge belongs to in

```c
	unsigned short	surfs[2];
```

so the pool cannot exceed 65536 entries without an edge silently naming a
different surface. That draws wrong rather than failing, which is the kind of
thing that gets blamed on the map. 65536 is the default and also a clamp, with
a line printed if it is exceeded — necessary because the shortage message
itself tells the reader to raise `r_maxsurfs`.

### `protocol.h`, `sv_main.c`, `cl_parse.c` — 256 was the wire, not a table

`PF_precache_model: overflow` looks like a table filling up, and the obvious
fix is to make the table bigger. It is not a table. Protocol 15 sends a model
number as one byte in five places — an entity update, a baseline, a static
object, the view weapon in the client data — and a sound number as one byte in
two, so 256 is as far as the wire can count. Raising `MAX_MODELS` alone would
have loaded the map and then drawn model 300 as model 44.

FitzQuake solved this in 2009 with protocol 666, and every engine descended
from it reads it. It is 15 plus a way to send a second byte wherever 15 sends
one: `U_MODEL2`/`U_FRAME2` on entity updates behind an extension byte,
`SND_LARGESOUND` on sounds, `SU_WEAPON2` and friends on the client data, and
`svc_spawnbaseline2`/`svc_spawnstatic2`/`svc_spawnstaticsound2` for the
load-time messages, each with a flags byte saying which numbers are shorts.
The constants here are copied from QuakeSpasm's `protocol.h` and the write
order from its `sv_main.c`, so demos go both ways.

What is this engine's own is *when*. QuakeSpasm speaks 666 always. Here the
server starts every map on 15 and moves to 666 only if the map needs it: the
signon writers that run while the map spawns switch it the moment they write a
number that does not fit, and `SV_ChooseProtocol` checks the precache counts
once spawning is done. Both happen before any client has been told a protocol
— precaching is refused after spawn — so the serverinfo it sends is always
right. id's maps produce the same bytes they always did.

The client had one ordering problem. 15's update handler looks the model up the
moment it reads its number; 666's high byte is at the end of the message. The
lookup moved to the end.

### `vid_x.c` — a queue that replayed itself

Found while testing the Backspace change. `Sys_SendKeyEvents` handed each queued
key to `Key_Event` and only then stepped past it. A yes-or-no question
(`SCR_ModalMessage`) is opened from inside a key handler and pumps the same
queue until answered, starting on the event still being handled. Once it
returned, the outer loop stepped the tail one past the head, the queue looked
full, and all 64 slots of old key presses were replayed. Usually empty slots
and harmless keys, so it went unseen; with a `docker stop` pending, it
replayed the Return that opened the question, which opened it again, and the
engine had to be killed. Stepping past the event before handing it on is the
whole fix.

### `r_draw.c` — `-ffast-math` and the NaN it was told could not exist

A NaN guard written as `if (!(x > lo)) x = lo;` is correct C: any comparison
with a NaN is false, so the NaN takes the assignment. It is also the first thing
`-ffast-math` removes, because that flag tells the compiler NaNs do not occur
and so `!(x > lo)` is the same as `x <= lo`. A test program with the same
flags kept the guard; the renderer, compiled in context, did not — a NaN
injected into one vertex came out of the clamps as a NaN, `ceil()` made it
`INT_MIN`, and the range check dropped the edge.

The consequence is not a seam, which is what the console said it was. An edge
is the side of a surface where the span generator turns it on or off. Without
it the surface stays on until the next edge on that scanline, and draws its
texture there, clamped at its border. On the re-release maps this was the flat
band across the middle of the screen.

The guard now inspects the bits, and a face with a vertex that will not project
is left out of the frame: `R_DiscardFace` clears the face's surface from every
edge it touched this frame (an edge with both surface slots empty is skipped)
and disowns its own edges so the cache does not hand them to a neighbour. That
turns the band into a face missing for one frame. What makes the NaN on those
maps is still open; the first one per map is reported with its vertex, model
and viewpoint.

### `pr_edict.c`, `localize.c` — builtins by name, and messages by key

The 2021 re-release's QuakeC declares its print functions as
`void(entity client, string s, ...) centerprint = #0:ex_centerprint;`. FTEQCC
compiles that to a function named `ex_centerprint` with `first_statement` 0,
and the re-release engine resolves the name to its own builtin. id's engine
treats a non-negative `first_statement` as QuakeC, and statement 0 is the
compiler's dummy, so the call ran nothing and returned. The whole symptom was
silence: a trigger plays `misc/talk.wav` beside its centerprint, so the player
heard the message arrive and saw nothing.

`PR_PatchRereleaseBuiltins`, at the end of `PR_LoadProgs`, rewrites those
functions' `first_statement` to builtin numbers, using QuakeSpasm's table:
the three prints go to id's own 73, 23 and 24, and the rest to 79 to 92, added
to `pr_builtin`. The table also covers an earlier update that gave the three
prints the numbers 90 to 92.

The strings they print are keys. `localize.c` loads
`localization/loc_english.txt`: from the search path, then loose under the
base directory, then out of `QuakeEX.kpf`. It reads that zip's central
directory and inflates the one entry with zlib, which is the only new library.
The file is one `key = "value"` per line with `//` comments and backslash
escapes. `PF_VarString` looks up its first argument and, if the text has `{}`
or `{N}`, fills them from the arguments after it. Otherwise it joins the
pieces, looking each one up, which is what turns a map's `$key` message
handed to `centerprint` into text. `PF_WriteString` looks up too, for the
finale.

### `d_modech.c`, `d_part.c` — particle size above 640 wide

`D_DrawParticle` sizes a particle as `izi >> d_pix_shift`, where `izi` is
1/z scaled by 0x8000, and clamps the result to `d_pix_min` and `d_pix_max`.
The two clamps scale with the width in proportion: `width/320` and
`width/80`. The shift is `8 - (int)(width/320 + 0.5)`, one bit less for every
320 pixels. That doubles the size each step where it should grow linearly:
- right at 320 and 640;
- 4x instead of 3x at 960 and 8x instead of 4x at 1280;
- 32x instead of 6x at 1920.

At 1920 wide almost every particle was pinned to one clamp or the other. id's
DOS and Windows builds topped out around 640 wide, where it never showed.

`d_pix_mul` replaces the shift in the C drawer: `(izi * d_pix_mul) >> 16`, with
the multiplier at 512 x width/320. At 320 wide that is exactly `izi >> 7`, and
at 640 exactly `izi >> 6`. `izi` is at most 4096, since particles nearer than
`PARTICLE_Z_CLIP` are dropped, so the product stays in range. `d_pix_shift`
is still set for the i386 assembler, which this build doesn't compile.

### `r_draw.c` — an edge walked the same way twice

`R_RenderFace` shares screen edges between faces. When a face emits an edge,
`cachededgeoffset` on the `medge_t` records where. The next face to use that
`medge_t` in the same frame calls `R_EmitCachedEdge`, which puts it in
whichever of the `edge_t`'s two surface slots is empty. `surfs[0]` is the
surface the edge closes on each scanline and `surfs[1]` the one it opens,
decided by which way the edge runs down the screen. So the free slot is the
right one only if the second face walks the edge the opposite way. In id's
maps every edge has exactly one face on each side, so it always does.

The re-release maps don't hold to that. On start.bsp a crate face's closing
edge is walked by three faces, twice in the crate's own direction. The other
face that walks it that way got there first and took the "closes" slot. The
crate was given "opens", so it was opened twice and never closed, and its
span ran to the right edge of the screen. The `surface` edge list showed it
directly: `edge 45534 backward, faces 1+ 2-, edge_t 425 surfs 181/182`. A
third face would have been worse: `R_EmitCachedEdge` writes `surfs[1]` when
`surfs[0]` is taken, and so takes the edge from a face already on it.

`medge_t` now carries `cachedforward`, the direction of the face that emitted
the cached edge. `R_CanReuseEdge` hands the edge on only to a face walking it
the other way, and only while a slot is free. Otherwise the face emits its
own, and two coincident edges are harmless to the span generator. A load-time
count reports how many edges a map has that more than one face walks the same
way.

### `model.c` — node bounds that do not hold their faces

Two things in the renderer trust a node's bounding box. `R_RecursiveWorldNode`
drops a frustum plane from `clipflags` for a node wholly inside it, and every
face below inherits that: it is not clipped against that plane. And
`R_ClipEdge` caches an edge it found wholly outside a plane as
`FULLY_CLIPPED_CACHED` for the frame, so the next face that shares the edge
skips it without looking.

If a face lies outside its node's box, those two disagree. The face isn't
clipped against a plane it crosses. Its neighbour, whose node did keep the
plane, finds their shared edge off-screen and caches that. The face then skips
the edge and gets no clipped replacement, because it has no clip for that
plane. Its span opens on some scanlines and never closes, so the face runs on
sideways across the screen.

id's qbsp never produced such a face: every face is cut along the tree, so it
lies inside the node it hangs on. On e1m1 all 2750 nodes hold their faces and
their child nodes. Modern compilers leave some faces uncut on purpose. The
report that found this was a `crate0_side` face on the re-release start map,
covering a pixel 300 units in front of where the ray met its plane, outside
the crate.

`Mod_WidenBounds` grows each node's box, after the checksum, to hold every
vertex of its own faces and the boxes of the nodes below it. Leaves are left
out: they own no faces, and in BSP29 every solid leaf is leaf 0, whose box
folded into its parents would have widened nearly every node on id's maps by
thousands of units. That was the first version.

### `r_bsp.c` — a face that crossed a plane once

`R_RecursiveClipBPoly` cuts a brush model's faces along the world's BSP planes
so that the edge list can sort the pieces against the world. It walks a face's
edges, measures both ends of each against the plane, and makes a cut point
where an edge crosses. A convex face crosses twice or not at all, and the two
cut points, `pfrontenter` and `pfrontexit`, are joined by an edge that closes
each half.

Those two are file statics, and nothing cleared them. If a face crossed only
once, one of them was this cut's point and the other was whatever an earlier
cut had left, from another face, perhaps another model. The closing edge ran
from here to there, and the piece was drawn as a sliver stretched across the
screen.

A convex face crosses once when the same vertex is measured twice with
different results. Every vertex is the end of one edge and the start of the
next, and id wrote `DotProduct (v, normal) - dist` out separately for
`lastdist` and `dist`. `-ffast-math` lets the compiler reassociate each copy
on its own terms, so for a vertex lying on the plane the two can differ in the
last bit, and so in sign. With the frames fixed by `timedemo`, id's demo2 does
this 71 times. The measurement is now one function the compiler may not inline
(`R_BPlaneDist`), so the same vertex gets bit-for-bit the same answer, and
demo2 does it 0 times. The pointers are cleared per cut, and a face still
crossing an odd number of times is left out for the frame and counted
(`r_bmodelodd`), not closed with an edge from nowhere.

`R_ClipEdge` has the same pattern for the frustum's left and right planes. On
all three demos the left plane is always crossed an even number of times. The
right plane isn't, by design: an edge wholly off the right is cached as fully
clipped and skipped by the next face that shares it. The right edge only
supplies the nearest 1/z, not geometry.

### `r_draw.c` — the edge after a bad vertex

1.9.2 returns early from `R_EmitEdge` for a vertex that will not project and
later takes the face out. `R_RenderFace` marks the cached last vertex valid
after every edge, whatever happened inside. So when the bad vertex was the
face's first, the next edge started from the previous face's `r_u1` and `r_v1`.
If that face ended with a right-hand clip, `r_ceilv1` had never been updated
for them, because the `r_nearzionly` path returns before that line. The edge
got a zero height, and its step was a NaN that converts to `INT_MIN`. Once
stepped, its u ran off the left of the screen past `edge_head`, and
`R_StepActiveU` followed a null `prev`. Once a face is bad, `R_EmitEdge` now
emits nothing more for it, and an edge whose step or start is not a number is
handed back before it reaches the list.

Found by forcing one clip fraction in seven to `x / 0` with the fraction guard
turned off. That guard, `R_SafeFrac`, holds every cut fraction in [0, 1] and
maps a non-finite one to the midpoint. The `(inf -nan -nan)` in the 1.9.2
report is exactly what an infinite fraction makes from an edge along one axis:
`inf * dx` in x, `inf * 0` in the other two.

### `model.c` — indices that are unsigned in the file

BSP29 stores node children, a node's first face and face count, a leaf's
first mark-surface and count, clipnode children, and a face's plane, texinfo
and edge count as 16-bit values. The compilers treat them as unsigned; id's
loader read them as signed. Past 32767, a face's plane pointed before the
plane array and a node's child became a leaf that wasn't there. They're read
unsigned now, with the QuakeSpasm rule for node children: below the node count
it's a node, otherwise leaf `65535 - p`.

The loader also no longer trusts what it is given. An edge's vertices, a
surfedge's edge, a face's plane, texinfo and range of surfedges are checked
against what the map has. Vertices and planes are checked for being numbers.
Anything out of range is set to 0 and counted, and the first is printed as it
is found, before any error it might lead to.

### `r_fence.c` (new) — a hole is not a colour, and the edge list does not know

A texture named `{something` is a fence: palette index 255 is a hole. Grates,
vines, ladders, chainlink. id never shipped one — the convention grew up in the
editors afterwards — so this renderer had never heard of it and drew index 255
as what it literally is, palette entry 255, a flat dusty pink (159, 91, 83). A
ceiling grate rendered as a sheet of pink with the bars punched into it.

Skipping index 255 would have been a two-line change and wrong, because the
holes would then have shown the previous frame. This renderer resolves
visibility with an **edge list**: it sorts surfaces per screen span, the
nearest one wins, and the rest are never rasterised at all. A fence that keeps
its place in that list deletes the room behind it. Whether its own pixels get
written is beside the point — nothing was ever drawn there to see.

So the fence comes out of the edge list. `R_RenderFace` hands a masked surface
to `R_FenceDeferFace` and returns; the world is then built and drawn as though
the fence did not exist, which is exactly the picture its holes should show.
After `R_EdgeDrawing` finishes, `R_DrawFenceFaces` puts each fence back, one
polygon at a time, against the z-buffer the world has just written.

The drawing itself is not new code. Sprites have always been drawn this way
here — after the world, masked on index 255, z-tested and z-written per pixel —
and `D_SpriteDrawSpans` already does all of it, fog included. It wants spans
and a lit texture block. A world surface can supply both: `D_CacheSurface` for
the block, `D_CalcGradients` for the texture gradients, and the same
plane-to-1/z arithmetic `R_RenderFace` stores on a `surf_t` for the depth.

The polygon comes from `R_RenderPoly`, which id wrote to frustum-clip and
project a single surface and then hand it to `D_DrawPoly` — a function left
empty in 1996 with the comment *"this driver takes spans, not polygons"*, the
hook for a polygon driver that never shipped. Thirty years later it has one
caller. `D_DrawPoly` scan-converts the projected polygon into one span per
line and calls the sprite drawer.

Two facts make this cheap rather than fiddly:

- `colormap[l][255] == 255` at every one of the 64 light levels, and no other
  index ever maps to 255. So a texel's transparency survives lighting, and the
  mask test can be done on the **lit** cached block with no false holes.
- The sprite drawer's inner loop is already the fence inner loop. The whole
  feature is a scan converter and some setup.

Measured with a common shareware wall texture turned into a fence, so about a
third of the view is masked, at 1024x768: 507/552/543 fps without the pass
against 465/477/429 with it. A frame containing no fence never enters it.

A brush model has two ways into the edge list, and the first release hooked
one. One that sits inside a single BSP leaf goes through `R_RenderFace` like
the world. One that straddles the world's planes goes through
`R_DrawSolidClippedSubmodelPolygons`, which cuts each face along those planes
and hands the pieces to `R_RenderBmodelFace` — a function with no fence check.
That is where a `func_wall` walkway stayed pink beside a world-brush walkway
that did not. The fence is now taken before the cutting, whole; the cutting
exists for the edge list's sorting, which the z-buffer replaces. My door test
had missed it because each door sat in one leaf.

The first release of this missed something the test could not show. The test
fence had holes punched into all four mip levels by hand; a real one has them
only in the first. Texture tools build the smaller levels by averaging, which
turns a hole into a tan blend of bar and pink, so in the maps a grate went
solid as soon as it was far or oblique enough to drop a level.
`Mod_RebuildFenceMips` rebuilds those levels at load from level 0: a texel is
a hole if more than half its block is, otherwise the block's most common solid
colour. The test that should have caught it is `d_mipcap 2`, which forces the
smaller levels everywhere.

What this does not get you is partial transparency. An 8-bit palette has no
alpha; there is one index that means "not here" and no index that means "half
here". A texel is a hole or it is opaque.

### `r_fog.c` — the curve, which was guessed once and wrong

Getting fog onto the screen was the easy half. The half that matters to
anyone looking at it is how much fog there is at a given distance, and that is
not a free choice: the maps were authored against a particular curve, and
anything else is either invisible or opaque.

The first attempt used `1 - exp(-density * d)` on the grounds that this is what
`GL_EXP` fog does. It is, but it is not what the re-release does. That engine's
`fog` command comes from FitzQuake, which sets `GL_FOG_DENSITY` to `density/64`
and `GL_FOG_MODE` to `GL_EXP2` — so the fraction is

```
	1 - exp(-((density / 64) * d)^2)
```

Two mistakes at once, and they compound in the same direction. At 100 units
with the density a map actually sets, the guess gave 99% fog where the real
curve gives 1%, so every room rendered as a flat sheet of the fog colour.

The lesson worth keeping is that "implement fog" and "implement *this* fog" are
different jobs, and the screenshot that proves the first says nothing about the
second. `r_fogscale` exists because the correction is still inferred from
another engine's source rather than measured against the maps, and a number
that can be turned at runtime is worth more than a claim that it is right. It
is on the Options menu as **Fog thickness** for the same reason: judging a haze
means looking at it, and a console command makes you stop looking to type.

### `r_fog.c` (new) — fog without anything to blend with

Fog is a blend, and this renderer has nothing to blend. It writes palette
indices into an 8-bit buffer; there is no colour at the point a pixel is
written, only a number that stands for one.

But darkening a palette index is a problem the engine already solved.
`gfx/colormap.lmp` is 64 rows of "this index, at this light level, looks like
that index", and every surface in the game is drawn through it. Fog is the same
shape of table with a different question: "this index, blended this far toward
the fog colour, looks like that index". 32 rows, built at map load by a nearest
-colour search over the palette, and applied with one indexed byte load.

Depth is the other half. Every drawer already carries `1/z` — the world spans
for perspective correction, the model and sprite drawers for the z-buffer — and
already recompute it at a granularity that suits them: every eight pixels for a
world span, per span for a model, per particle. Fog is sampled at exactly those
points, so it costs a table lookup and no arithmetic of its own. Eight pixels of
a wall is a few world units at any distance fog is visible over, and banding at
that scale is finer than the palette can express anyway.

Every path that writes a pixel goes through it, which matters more than it
sounds: an unfogged monster in a fogged room reads as a bug, where uniformly
approximate fog reads as fog. That meant `D_DrawSpans8`, `Turbulent8`,
`D_PolysetDrawSpans8`, `D_PolysetDrawFinalVerts`, `D_PolysetRecursiveTriangle`,
`D_SpriteDrawSpans` and `D_DrawParticle` — the last three found only by looking
at a screenshot and asking why one torch flame was still orange.

Index 255 maps to itself at every level: it is the transparency index in skins
and sprites, and a nearest-colour search would otherwise hand it back as the
closest match to some fogged brown and punch holes in things.

Measured with `timerefresh` at 1024x768, three runs each: 497/420/466 fps
without fog, 480/467/408 with. There is no cost to find, which is what you
would expect of a renderer this memory-bound.

### `pr_edict.c` — a key you cannot honour is not a key you do not know

Every key in a map's entity lump that the progs does not define gets reported
as "not a field". For a mod wanting some field this engine has never heard of,
that is the right answer.

It is the wrong answer for `sky`. The re-release maps set it on worldspawn to
name a six-sided skybox -- a GLQuake feature this renderer does not have -- and
"'sky' is not a field" tells the reader nothing about why their sky looks
wrong. The same goes for `skyname`, `skybox` and `skyfog`.

Those four are now named explicitly, with what was asked for and what is drawn
instead. The general lesson is that "unknown" covers two different situations
and only one of them is genuinely unknown; the other is a decision this port
has made, and a reader deserves to be told which.

Alongside it, `developer 1` reports the sky texture and its dimensions at load.
When a sky looks wrong the first question is which texture it is and how big,
and before this there was no way to ask.

### `r_sky.c` — a sky texture is not always 256x128

`R_InitSky` takes the sky's two layers apart with the dimensions written into
it:

```c
	newsky[(i*256) + j + 128] = src[i*256 + j + 128];
```

`mt->width` and `mt->height` are never read. Every sky id shipped is 256x128,
so the constants were the dimensions and this was correct for thirty years.

A re-release map's sky is larger. At 512x256 that loop walks the top-left
quarter of the image at half the real stride; at anything below 256x128 it
reads past the end of the texture. Both give a sky made of whatever happened
to be there.

The layers are resampled from the real dimensions now, nearest neighbour --
this is a scrolling cloud layer seen through a warp, and anything better would
not survive the warp. Verified by running id's routine and the replacement over
the same synthetic texture: at 256x128 they produce byte-identical buffers, and
above it only the replacement reproduces the texture's own vertical ramp.

### `r_main.c` — a `fog` command that draws no fog

Every re-release map sets fog through the progs on every level load, and this
engine has no such command, so every level load printed `Unknown command
"fog"`. The map is not at fault: fog arrived with GLQuake, and the software
renderer writes palette indices into an 8-bit buffer, so there is nowhere to
put a per-distance blend short of rebuilding the colormap every frame.

The command exists now and takes what it is given. It keeps the values, so
`fog` with no arguments reports them the way the engines that do render it
report them, and says there that nothing is drawn from them. Reporting an
error once per level for a feature the engine was never going to have is worse
than silence; saying nothing at all, when asked directly, would be worse again.

### `d_surf.c` — the icon in the corner was the whole story

`SCR_DrawRam` draws `scr_ram` at the top left of the view whenever
`r_cache_thrash` is set. That flag means `D_SCAlloc` wrapped onto surface cache
blocks it had already built *during the same frame*: the frame needs more lit
surface than the cache holds, so every one of those is rebuilt next frame, and
the frame after, for as long as the view does not change. It is expensive
enough to be the stall rather than a symptom of one.

`D_SurfaceCacheForRes` sized it at `600*1024 + (pixels - 64000) * 3`, which is
1818 KB at 800x600 — and 1818 KB is exactly what the engine prints at startup,
which is how this was identified from a log. Generous in 1996, nothing now.

It is 8 MB plus 16 bytes a pixel, capped at 48 MB because it comes out of the
same heap as the map and the frame pools. And it says so in words once a map,
because `scr_showram` defaults on but can be off, and an unlabelled icon in the
corner is not a diagnosis.

### `cl_parse.c` — a short that had to be read unsigned

Raising `MAX_EDICTS` reached further than the edict array. `SV_StartSound`
packs both the entity and the channel into one field:

```c
	channel = (ent<<3) | channel;
	MSG_WriteShort (&sv.datagram, channel);
```

`MSG_ReadShort` returns a signed short. At 600 edicts the largest value that
could appear was 4800, so the sign bit was never reached and nobody noticed. At
8192 edicts, everything from entity 4096 up sets it, reads back negative, and
the sound is attributed to entity -1.

Nothing on the wire changes: the field is and always was 16 bits, and the fix
is to read it as unsigned. The bounds check under it also said `>` where it
meant `>=`.

The general shape is worth remembering when raising a 1996 limit: the array is
the easy half, and the places that packed a value into a field sized for the
old maximum are the half that fails quietly.

### `cl_parse.c` — "Illegible server message" naming nothing

The default case of `CL_ParseServerMessage`'s switch means the reader is no
longer on a message boundary: some handler above it read the wrong number of
bytes and everything after is misaligned. id's text says none of that and names
neither the byte nor the opcode.

It now prints the opcode it found, the offset it was at, the message size, and
the last opcode that parsed cleanly — which is the one to look at, since the
fault is almost always in that handler rather than where it was noticed.

### `pr_cmds.c`, `sv_main.c` — three writers, one fixed buffer

`sv.signon` is written by `PF_makestatic` (14 bytes a static), `PF_ambientsound`
(11 bytes an ambient loop) and `SV_CreateBaseline` (16 bytes an entity with a
model). Its `allowoverflow` is clear, so whichever one fills it calls
`Sys_Error` through `SZ_GetSpace` — at the point the map is nearly loaded,
which is the worst place to stop.

Each one checks for room now and drops what will not fit, saying so once. A map
too big for the protocol loses some torches or ambient loops and still plays.

Worth recording how the third one was found, because guessing missed it: a
check placed after `SV_CreateBaseline` could never fire, since `SZ_GetSpace`
had already exited. Guarding the two obvious writers left a map failing at 2500
statics anyway, and a breakpoint on `Sys_Error` named `PF_ambientsound` in one
line. The test data was synthetic — the re-release campaigns cannot be shipped
or tested here, so `e1m1`'s entity lump was rewritten with 400 to 4000 extra
wall torches at origins the map already used, which makes the same demand out
of data that is present.

### `docker/entrypoint.sh` — the colours did not survive the trip

The renderer draws palette indices. `vid_x.c` can hand those to an 8-bit
PseudoColor visual with the palette in a colormap, or translate each frame
through `st2d_8to24table` into a deeper one. The container took the first,
because it is what the renderer was written for and costs the engine nothing.

Everything downstream paid for it. A colormap belongs to a window; there is no
window manager here, so the engine installs its own; and x11vnc then has to
walk the window tree, read that colormap and transform the whole screen
through it (`-8to24`) to give the browser truecolour. That is the most
expensive thing x11vnc does here — its own manual says the mode "does hog
resources" — and when the mapping goes stale, which it does when the window it
belongs to is replaced, the browser gets the right picture in the wrong 256
colours with no way back short of reconnecting.

At depth 24 none of that exists. Measured at 1024x768 with the engine's own
`timerefresh`: 430 fps at depth 8, 360 at depth 24. The engine does more work
and x11vnc does much less, and both figures are several times what a browser
can display. `QUAKE_X_DEPTH=8` restores the old path, and the smoke suite
starts the engine on whichever depth the run is not using so neither rots.

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

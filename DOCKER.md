# Running it

Everything the container does is driven by environment variables and by
arguments passed through to the engine. This is all of them.

```
docker run --rm -p 6080:6080 \
  -v /path/to/quake:/quakedata:ro \
  -v quake-state:/quake/state \
  ghcr.io/krippler/quake
```

Then <http://localhost:6080/play.html>.

### Compose

[`docker-compose.yml`](docker-compose.yml) builds the image and mounts
`./quakedata` as the game data, with a named volume for the state:

```
mkdir -p quakedata && cp -r /path/to/quake/id1 quakedata/
docker compose up --build
```

The settings below go under `environment:` there.

### Unraid

The Community Applications template is
[`templates/unraid.xml`](templates/unraid.xml); see
[PUBLISHING.md](PUBLISHING.md#unraid).

---

## Game data

`/quakedata` is your Quake install directory: the one holding `id1/pak0.pak`.
Nothing is bundled and nothing can be — see
[ABOUT.md](ABOUT.md#no-bundled-game-data). Any of these will do:

- the CD, or the directory a CD install left behind;
- the Steam or GOG install (`.../Quake/id1/`);
- the shareware release unpacked, from id's own `quake106.zip`;
- the 2021 re-release's `rerelease` folder (see
  [below](#data-from-the-quake-re-release)).

The shareware `pak0.pak` is a complete game as far as this is concerned: it
holds `progs.dat`, every model and sound, and E1M1 to E1M8, so episode 1 plays
start to finish. It is also what the port was tested against — see
[PORTING-NOTES.md](PORTING-NOTES.md#how-this-was-tested).

```
/path/to/quake/
├── id1/pak0.pak          shareware: episode 1
│   └── pak1.pak          registered: all four episodes
├── hipnotic/pak0.pak     Scourge of Armagon
├── rogue/pak0.pak        Dissolution of Eternity
├── <mod>/                anything else
└── music/track02.ogg     one soundtrack shared by all of them, see below
```

Each game can have a `music/` directory of its own instead —
`id1/music/track02.ogg`, `hipnotic/music/track02.ogg` and so on — which is what
you want if you have the mission packs, because their soundtracks are not
Quake's.

Pak files sitting directly in `/quakedata` with no `id1` around them are
treated as `id1`, because nothing else is ever mounted there and guessing is
kinder than an error. Names are matched case-insensitively and linked
lowercase, since the engine only ever opens `pak0.pak`, `pak1.pak` and so on in
order — a file called `PAK0.PAK` is never opened at all, whatever is in it.

Mount it read-only. The container never writes to your game data: it builds a
writable game directory in the state volume and links your pak files into it,
because Quake writes `config.cfg`, savegames and screenshots next to the pak
files.

### Mission packs and mods

Everything beside `id1` in the mount is offered in the menu, under **Options →
Game / Mod**. Pick one and Quake restarts on it — the container brings
the engine straight back and the page reconnects on its own, so it looks like a
few dark seconds. The choice is kept in the state volume and survives a restart
of the container.

`QUAKE_GAME` says which one to start on:

| | |
| --- | --- |
| `QUAKE_GAME=hipnotic` | Scourge of Armagon — which also changes the status bar and the menu |
| `QUAKE_GAME=rogue` | Dissolution of Eternity — likewise |
| `QUAKE_GAME=<dir>` | any other mod, by directory name |

It is the starting point, not a lock: setting it, or changing it, overrides
whatever the menu last chose, and leaving it alone leaves the menu's choice
alone. So a container configured with `QUAKE_GAME=hipnotic` does not drag the
game back to Scourge of Armagon every time somebody switches in the menu. An
explicit `-game`, `-hipnotic` or `-rogue` in `QUAKE_ARGS` beats both.

The directory has to be in the mount. A mod that ships loose files rather than
a pak works too: a directory holding `progs.dat` or a `maps/` folder is
recognised as a game directory even with no pak in it.

The shareware data cannot run mission packs or mods — Quake refuses modified
games without the registered pak files, and the menu says so rather than
restarting into the refusal.

### Data from the Quake re-release

Pak files from the 2021 re-release work, **BSP2 maps included** — so
*Dimension of the Past* (`dopa`) and the machine campaigns load. BSP2 is the
same fifteen lumps as the original format with the indices and bounds widened
past what a short holds, and six of them are read both ways now.

Use the re-release's own `id1` (in a Steam install,
`steamapps/common/Quake/rerelease/id1`), not an `id1` from the 1996 release.
Its `pak0.pak` holds `localization/loc_english.txt`, the text for every message
the re-release shows: gate prompts, keys, pickups, deaths. The re-release's
progs and maps name their messages by key (`$qc_need_gold_key`) and the engine
looks the text up there. With a 1996 `pak0.pak` in its place the expansions
still play, but every message comes out as its key, and the console says so.
Mounting the `rerelease` folder itself is the simplest way to get it right.

Early versions of the re-release kept that text in `QuakeEX.kpf`, a zip beside
`id1`, and it is still read if the mount has one, anywhere up to four folders
down. The current `QuakeEX.kpf` holds only a placeholder, and the console says
so when that is all it finds. Both files are part of the paid re-release, like
the rest of the pak files, so the image cannot include either.

What to expect:

- Entity keys the progs do not define (`fog`, and `alpha` under id's 1996
  QuakeC) are reported once each per map and ignored. id's engine did the same; it just said it
  again for every entity.
- A cvar the re-release progs sets but this engine does not define — `campaign`
  is the one you will see — is created on demand rather than refused, so the
  progs reads back what it wrote.
- A map with more than 256 models or sounds — MG1 has several — is played over
  FitzQuake's protocol 666, which can count past a byte. So is any game whose
  progs have an `alpha` field (MG1 and MG3), because only 666 can carry it.
  Everything else stays on id's protocol 15. It is decided per map, and
  `developer 1` says when.
- Models, sprites and brush models the progs make translucent are drawn
  translucent: MG1's gas flares are a soft blue glow, its lanterns have grey
  glass with the flame showing through, and things fade in and out. Alpha is
  rounded to eighths. Sky and water faces on a translucent brush model are
  still drawn solid.
- Coloured light is drawn where the map has it, from a `.lit` file beside the
  map or an `RGBLIGHTING` lump inside it. Walls take the colour of the light
  on them, and monsters, items and your weapon the colour of the light they
  stand in. **Options → Display → Coloured Light** turns it on and off, and
  `r_rgblight` at the console sets how much: `1` is the map's colours, `0` is
  id's grey light, and anything between is paler. Maps without coloured light
  look exactly as they did.
- The re-release progs unlock Steam achievements by sending their own server
  message: a monster killed by another monster, a secret found, a level or
  episode finished. There is nothing here to unlock; `developer 1` shows the
  achievement's name.
- `scr_usekfont` is the re-release's scalable font and is not implemented; the
  one "Unknown command" line at startup is accurate.
- Masked ("fence") textures — the ones named `{something`, used for grates,
  vines, ladders and chainlink — are drawn with their holes. They are taken out
  of the renderer's edge list so they do not hide what is behind them, then
  drawn over the finished frame against its z-buffer. They are lit, fogged and
  z-sorted like anything else. What the palette cannot do is partial
  transparency: a texel is a hole or it is opaque, so the re-release's
  translucent surfaces are drawn solid.
- Skyboxes are not drawn. The maps name one on worldspawn; the engine says so
  once per map and draws the map's own sky texture instead. `developer 1` also
  reports which sky texture that is and its size.
- `fog` is drawn, on the curve the re-release maps were authored against
  (FitzQuake's: `1 - exp(-((density/64) * d)^2)`). `fog` with no arguments
  reports the current values and whether it is drawing. It is approximate by
  construction — a palette blend table sampled every eight pixels — and
  measures as costing nothing. `r_fogscale` multiplies the density a map sets,
  if it comes out thicker or thinner than it should — it is on **Options →
  Display** as **Fog Thickness**, so you can turn it while looking at the fog.
- The re-release campaigns are much larger than anything from 1996, and the
  renderer holds one frame's worth of geometry in fixed pools. Those are sized
  for the re-release now — 65536 surfaces and 131072 edges, against id's 800 and
  2400, which the whole shareware episode peaks at 458 and 1162 of. If a frame
  still does not fit, the engine says so once per map and names what to raise:

  ```
  This frame did not fit: short 1204 surface(s) and roughly 800 edge(s).
  Geometry is being left undrawn. Raise r_maxsurfs (now 65536) and
  r_maxedges (now 131072) and restart the map.
  ```

  A flickering icon in the top left corner is a different limit: the surface
  cache, which holds each visible surface with its lighting applied. When a
  frame needs more than it holds, everything in it is rebuilt every frame and
  the picture stalls. It is sized for these maps (14 MB at 800x600, 40 MB at
  1920x1080) and says so in words if it is still short; `-surfcachesize <kb>`
  in `QUAKE_ARGS` raises it further, and a lower resolution needs less.

  Undrawn geometry looks like walls missing from the view with the rest of the
  level still there. `r_maxsurfs` and `r_maxedges` take effect on the next map
  load, and the heap they come out of is 192 MB by default (`-mem` in
  `QUAKE_ARGS` changes it).

If part of a map is drawn wrong — black, stretched, missing — aim the
crosshair at it, open the console and type `surface`. It names the face and
the model it belongs to, its texture and how dark that texture is, and how the
face is lit at that point:

```
maps/e1m3.bsp face 3068, 922 units away at (-95 -944 199)
drawn at the crosshair: maps/e1m3.bsp face 3068, the same face
pixel there: palette 51 (19 19 0)
texture "wswamp2_1", 64x64
its pixels: 0% palette 0 (black), average brightness 29 of 255
lightmap 10x12, styles 0 (sample 52 x 264)
light here 53, where 0 is black and 255 is full
```

The first line is what the map says is there; `drawn at the crosshair` is
what the renderer put on that pixel. `NO SURFACE` there is a gap in the
geometry, and `NOT that face` is the renderer sorting the wrong face in front.
Either one means the renderer is at fault, and the lines after it say where
the right face was lost: not marked visible, not in the edge list, or sorted
behind. A texture that is mostly palette 0
is black art, and a `light here` near 0 is the lightmap. Bind it to a key
(`bind p surface`) to use it without the console covering the view.

**2PSB**, the RMQ variant of BSP2, is not read. The engine names it rather than
printing a number.

### Music

Quake's soundtrack is audio tracks 2 to 11 of the CD and is not in the pak
files. Put a rip in a `music` directory as `track02.ogg`, `track03.ogg` and so
on. Ogg Vorbis, Opus, FLAC and WAV always work; MP3 works where the installed
libsndfile was built with it.

Where the engine looks, in order:

| | |
| --- | --- |
| `-musicdir <path>` | an explicit override for one run, via `QUAKE_ARGS` |
| `<game>/music` | beside that game's paks — `/quakedata/hipnotic/music` and so on |
| `$QUAKE_MUSICDIR` | set from `/quakedata/music` when that exists: one rip for every game |
| `id1/music` | so a mod with no music of its own still gets Quake's |

The game's own directory beats `QUAKE_MUSICDIR` deliberately. Scourge of
Armagon and Dissolution of Eternity have soundtracks that are not Quake's, and
an environment variable can only name one directory for every game the
container can run — so if it won, a mission pack would play the wrong music. A
`music` directory that holds no `track*` files is skipped rather than taken,
so an empty one falls through instead of turning into silence.

The startup log says which games were found to have music of their own.

In the game, `cd info` says which directory was found and what is playing;
`cd play 4`, `cd loop 4`, `cd stop`, `cd pause` and `cd resume` work as they
always did. `eject`, `close` and `reset` do not, and say so.

`bgmvolume` sets the music level and the master `volume` applies to it as well
— on a CD it could not, because the music never went through the mixer.

---

## Environment

### Picture

| | | |
| --- | --- | --- |
| `QUAKE_WIDTH` | `640` | Starting render width. 320 to 1920, rounded down to a multiple of 8 |
| `QUAKE_HEIGHT` | `480` | Starting render height. 200 to 1200 |
| `QUAKE_MAX_WIDTH` | `1920` | The largest width the in-game menu can reach |
| `QUAKE_MAX_HEIGHT` | `1200` | The largest height the in-game menu can reach |
| `QUAKE_DISPLAY` | `:99` | Which X display the container runs internally |

Widescreen modes are drawn as widescreen: square pixels, and a wider screen
shows more to the left and right rather than the same picture stretched across
it. 4:3 modes are untouched, and `fov` still means the horizontal angle at 4:3.

The renderer is in software, so every pixel costs. 640x480 is comfortable
anywhere; 1280x800 is fine on a modern core; 1920x1200 is a choice. The browser
scales whatever it is given to fit the window, so a lower number is a softer
picture rather than a smaller one.

Text, menus and the status bar grow with the resolution so they stay
readable: they are laid out as if on a screen of at least 480x360 and drawn
as many whole times larger as fit, which is 3x at 1920x1080 and 1x at 640x480.
The 3D view keeps the full resolution. The menus are sized on their own, the
way the re-release sizes them: as large as fills the height of the screen,
which is 2x at 1280x720 and 3x at 1920x1080. `scr_scale` at the console
overrides both choices: `1` is id's original size, `2`, `3` and so on force a
factor, and `0` goes back to choosing. It is saved in `config.cfg`.

`QUAKE_WIDTH` and `QUAKE_HEIGHT` are only where it starts. **Options → Display
→ Video Modes** in the game lists every mode from 320x240 up to the maximum and
switches to the one you pick, and the choice is written to `config.cfg` — so
after the first run it is the config that decides, not these variables. Setting
`vid_width` and `vid_height` at the console does the same thing.

Changing resolution resizes the X screen, and the browser sees that as the
framebuffer changing size: the picture blinks once as noVNC rebuilds its canvas.

`QUAKE_MAX_WIDTH` and `QUAKE_MAX_HEIGHT` are the size Xvfb is started at, and
an X server's maximum screen size is fixed when it starts — so they are the
ceiling the menu can reach, not a limit on anything else. Setting them to the
starting size pins the resolution there and turns the menu's list into a list
of things that will not happen; leaving them alone costs about 2 MB.

### Ports

| | | |
| --- | --- | --- |
| `QUAKE_WEB_PORT` | `6080` | noVNC and the sound, on one port |
| `QUAKE_VNC_PORT` | `5900` | Raw VNC, for a native client |
| `QUAKE_AUDIO_PORT` | `5901` | The sound stream, behind the proxy |

Only `QUAKE_WEB_PORT` needs publishing. Publish `QUAKE_VNC_PORT` as well if you
would rather use a native VNC client — you get the picture but not the sound,
and the pointer is not captured.

### Sound

| | | |
| --- | --- | --- |
| `QUAKE_SOUND` | `1` | `0` turns the sound off entirely |
| `QUAKE_SND_MIXAHEAD` | `0.06` | Seconds of sound the engine mixes ahead. Most of the delay between firing a shot and hearing it. `0.1` is id's default |
| `QUAKE_AUDIOSTREAM_ARGS` | | Extra arguments for `audiostream`, e.g. `--verbose` |

22050 Hz, 16-bit stereo, fixed: the engine produces it and the page consumes
it. The browser plays it through an `AudioWorklet` where the page is a secure
context and a `ScriptProcessorNode` where it is not — which is most of the
time, because this is normally served over plain HTTP from a machine on your
network.

### Access

| | | |
| --- | --- | --- |
| `QUAKE_VNC_PASSWORD` | | Requires a password for the VNC connection |
| `PUID` / `PGID` | `1001` | Who to run as, when started as root. `99` / `100` on Unraid |

There is no authentication on the web port without `QUAKE_VNC_PASSWORD`, and
even with it the transport is plain HTTP. Do not put this on the internet; put
it behind whatever you already use to reach your network.

### Behaviour

| | | |
| --- | --- | --- |
| `QUAKE_RESTART` | `1` | `0` stops the container when the game exits instead of starting it again |
| `QUAKE_STATE` | `/quake/state` | Where the config, savegames and logs go |
| `QUAKE_DATADIR` | `/quakedata` | Where to look for game data |

Quitting from the menu brings the title screen back rather than stopping the
container, because otherwise picking QUIT leaves a container running with
nothing in it and a browser that can never reconnect. Stopping the container is
what stops the container.

A crash brings the engine back too, and the display, the sound and the session
all outlive it, so the browser reconnects on its own — but the game starts
again at the title screen. There is no crash recovery in 1996 code and this
does not pretend otherwise. Three runs in a row that end within seconds of
starting stop the container rather than loop.

### Tuning, and things not to touch unless something is wrong

| | | |
| --- | --- | --- |
| `QUAKE_X_DEPTH` | `24` | Bit depth of the X screen. `8` uses the colour-mapped visual the renderer was written for: slightly cheaper in the engine, but the palette then has to survive a trip through x11vnc, which is where wrong-colour pictures came from |
| `QUAKE_VNC_8TO24` | `1` | Only read at `QUAKE_X_DEPTH=8`. `0` sends the picture as colour-mapped depth 8: 64 colours in a browser, not 256. A diagnostic, not a way to play |
| `QUAKE_VNC_WAIT` | `1` | x11vnc poll interval, ms |
| `QUAKE_VNC_DEFER` | `1` | x11vnc update defer, ms |
| `QUAKE_VNC_ARGS` | | Extra x11vnc options. Each must start with a dash, and the container checks — x11vnc answers an option it does not recognise by exiting, which leaves the game running and the browser saying "connection lost" with nothing to explain it |

x11vnc also runs with `-noxdamage`: it scans the screen itself rather than
following the X server's damage reports, which a game changing most of the
screen every frame gains nothing from and which stalled a request for up to half
a second at a time. See [The picture stutters](#the-picture-stutters-or-lags).

`-wireframe` and `-scrollcopyrect` are off. Both are on by x11vnc's default and
both are for a desktop: they watch for a window being dragged or a pane
scrolled while a mouse button is held, and hold the picture back while they
decide. A game holds the fire button down.

---

## Playing

### Keyboard and mouse

| | |
| --- | --- |
| W A S D | move and sidestep |
| Mouse | look |
| Mouse 1, Ctrl | attack |
| Space | jump, and swim up |
| Shift | run |
| Wheel | next / previous weapon |
| 1–8 | select a weapon |
| E / Q | swim up / down |
| Tab | scores |
| `` ` `` | console |

Not Quake's 1996 defaults, which are the arrow keys to move, `,` and `.` to
sidestep, `a` to look up, `d` to swim up, and the mouse walking you forward
unless you hold `\` to look with it. That was normal then.

`freelook` is a cvar this port adds, set to 1: the mouse steers the view
without a key held. `+mlook` is untouched and still wins while it is held, and
`freelook 0` gives you 1996 back exactly.

The wheel works because the X11 driver now reports it — the 1996 code knew
about three mouse buttons and dropped the rest, while `keys.c` had
`K_MWHEELUP` and `K_MWHEELDOWN` in it the whole time waiting for something to
send them.

These are written into `config.cfg` once, on a state volume that has none, and
the engine owns the file after that: anything you change in **Options** or at
the console is saved over them on exit. Delete `config.cfg` to get them back,
or set `QUAKE_MODERN_CONTROLS=0` to start from id's defaults instead.

The mouse is captured by the page, so turning never runs out of screen and the
cursor cannot wander off into the rest of your desktop.

`Esc` lets it go and brings the start screen back — which is why the start
screen has a **Game menu** button. Pointer Lock reserves `Esc` for the browser
and there is no asking for it back outside a secure context, and `Esc` is
Quake's own menu key; the button sends one to the engine and hands the picture
straight back, so the game returns with its menu already up. A controller's
**Menu** or **B** button does the same thing without leaving the game at all.

Inside the menu, **Backspace** goes back a level and closes it from the top,
the way `Esc` does in id's engine — `Esc` being the browser's here. It still
deletes in the name and address fields and answers "no" to a yes-or-no
question. On the controls screen it goes back too; **Del** (or **Y** on a
controller) clears a binding there, as in the re-release.

The `` ` `` key opens the console, and passes through untouched — which is why
the menu is on a button rather than moved onto `` ` ``. The console is how you
load a map, change the skill or start the music, and it is worth more than a
second way to reach a menu.

**Options → Controls → Customize Controls** rebinds everything, a controller's
buttons included, up to three keys or buttons for each action. "Everything" is thirty-four
actions now, including the weapon keys, the console, the scoreboard, quick save
and load, chat, pause and the screenshot key: the 1996 menu stopped at eighteen because eighteen rows is
all that fits on a 320x200 screen, so the rest could only be bound by typing
`bind` at the console. The list scrolls, with a scrollbar down the right to
say where you are in it.

**Options** holds the settings, not just a handful of them: the field of view,
mouse look, smooth mouse, the crosshair, whether the weapon is drawn, view bob,
view kick, water warp, texture detail and the sound delay, alongside the screen
size, brightness, volumes, mouse speed and the rest. The list scrolls, and
everything in it is saved in the state volume.

Two of those are worth knowing about on a machine that is struggling:
**Texture detail** trades sharpness for frames, and **Sound delay** is how far
ahead the engine mixes — see the troubleshooting note below before shortening
it.

**Options → Display → Video Modes** changes the resolution while the game is running:
twenty modes from 320x240 up to `QUAKE_MAX_WIDTH`/`QUAKE_MAX_HEIGHT`, applied
as soon as you pick one and remembered in `config.cfg`. The X11 build never had
this menu — `menu.c` hides the line unless the video driver claims it, and the
driver never did — so the resolution used to be whatever the command line said
for the life of the process. The picture blinks once on a change: the X screen
really does resize, and the browser rebuilds its canvas to match.

`m_pitch`, `m_yaw` and `sensitivity` are in **Options** too, or in
`config.cfg`, and are saved in the state volume. `+mlook` still works if you
would rather hold a key than use `freelook`.

### Game controllers

The game reads the controller itself, as the re-release does, and this works
the same way in the Linux desktop build. The page passes the pad's state to
the engine over the same port as the picture and the sound. Everything else
happens in the game:
- **Bindings:** its buttons are keys, bound under **Options → Controls →
  Customize Controls** alongside the keyboard's, and shown as they are on the
  pad (Pad A, Pad RT…).
- **Sticks:** turn and look up/down speeds, the look curve, invert, a
  deadzone for each stick, swapping the sticks, and whether a full push runs
  are on the **Controls** page, under **Controller**. That heading also names
  the pad the game can see.
- **Vibration:** the pad rumbles when you're hit and when you fire, with an
  intensity setting on the same page. In the container the game sends the
  rumble back to the page, which plays it through the browser. Chrome and Edge
  can do this; Firefox mostly can't. On the desktop, SDL plays it.
- **Saved** in `config.cfg`, like every other setting.

The page's controller panel is gone. Anything set in it before needs setting
once more in the game.

Defaults, which are the old panel's, on a standard mapping (an Xbox pad, a
Backbone One, most others):

| | |
| --- | --- |
| Left stick | move and sidestep; a full push runs |
| Right stick | look |
| A | jump |
| B | menu |
| X / Y | shotgun / rocket launcher |
| LB / RB | previous / next weapon |
| LT | run |
| RT | fire |
| View | scores |
| Menu | menu, and it cannot be rebound, like `Esc` |
| Stick clicks | axe / thunderbolt |
| D-pad | forward, back, turn |

In the menus, A chooses, B goes back, and the D-pad or the left stick moves.
Y clears a binding on the Customize screen, and when that screen is waiting
for a key, a button binds itself.

A browser does not report a pad until a button on it has been pressed, so the
start screen mentions it once one has been. The page writes one line into the
container's log saying which pad it found and whether the game has it. The
engine logs `Controller: <name>` when it arrives.

---

## Troubleshooting

### It takes a long time to start

The container's own start takes well under a second from its first log line
to the engine running, on any machine with the game data on a local disk.
Every line the start-up script logs carries the seconds since it began,
`[quake 12.3s] ...`, so the log says where a slow start spends its time:

```
docker logs -t <container>
```

- A gap before `game data:` is the scan of the mount: the script lists every
  game directory and looks a few levels down for the re-release's
  `QuakeEX.kpf`. On Unraid, a user share on array disks that have spun down
  waits for them to spin up; putting the game data on a cache-only share, or
  mounting the disk path directly, avoids it.
- A gap before the first line at all is before the script runs: the image
  being pulled or updated, which Unraid does on start when auto-update is on.
- No gap anywhere, but the page is slow to appear: the time is in the browser
  loading the page and connecting, not in the container.

### The picture stutters or lags

The picture reaches the browser through x11vnc, which captures the screen,
compresses it and sends it; the engine renders far faster than that. The
browser log's `picture gap ... x11vnc answered N ms later` lines are this
stage falling behind. Measured at 1920x1200, with the defaults below, x11vnc
delivers about 34 pictures a second at 10 to 12 MB/s; at 1280x800 and below
about 36, at a quarter of the bandwidth or less.

Resolution is the biggest lever: the cost is per pixel, and the browser scales
the picture to the window either way. After that, the page takes two options:

| | | |
| --- | --- | --- |
| `play.html?compression=N` | `1` | How hard each picture is compressed, 0-9. Higher saves bandwidth for a slow link but costs x11vnc time: at `2`, noVNC's own default, 1920x1200 managed 14 pictures a second |
| `play.html?quality=N` | `6` | JPEG quality for the busy parts of the picture, 0-9. Lower saves bandwidth; it barely changes the rate |

`play.html?encoding=hextile` is the older switch: it avoids the browser's image
decoder altogether at roughly three times the bandwidth.

### The sound lags behind the picture

Some of it is fixed cost and some of it is the machine.

The fixed part is the engine mixing ahead of itself. Measured from a keystroke
to the sound reaching the socket: id's `0.1` gives about 130 ms, the `0.06`
this ships gives about 85 ms, and `0.04` gives about 70 ms. Below that there is
nothing left to win — the floor is one engine frame plus the chunk size — and
all a smaller number buys is underruns. `QUAKE_SND_MIXAHEAD` moves it.

The rest is the browser. It holds a small buffer, 25 ms to begin with, and
**grows it by 20 ms every time it runs dry**, because a longer delay is better
than a click. On a machine that cannot keep up it will climb to 250 ms, and
that is then most of what you hear. It says so in the container log:

```
[quake] sound: buffer grew to 105 ms after 4 underrun(s). That is added delay,
        and it is this machine not keeping up rather than the container
        sending late -- a lower resolution is what shortens it.
```

If you see that line, the answer is a lower resolution in **Options → Display
→ Video Modes**, not a sound setting. The same shortage shows up in the picture as
`picture gap` lines.

### The colours went wrong after quitting to the title screen

Fixed. If you are on an older image, **reload the browser tab** and they come
back.

Quitting makes the engine exit and the container start it again, which creates
a new window with a colormap of its own. x11vnc shows this 8-bit screen to the
browser by converting it through the window's colormap, and it went on
converting through the one that died with the old window — so the picture came
back in teal and magenta and stayed there. Its own manual owns the limitation:
"if there are multiple 8bpp windows using different colormaps, one may have to
iconify all but one for the colors to be correct."

A VNC session that connects afresh is correct every time, and nothing else
tried was: not `x11vnc -R refresh`, not `-fixscreen 8=t`, not re-uploading the
palette from the engine, not starting the engine at the resolution the config
asks for. So the container counts engine starts in a file the page can read at
`/quake-run`, and the page opens a new session when that count moves — about
five seconds after a restart. Nothing in the VNC protocol reports that a window
was replaced, which is why it has to be counted rather than noticed.


**"nothing to play", and the container stops.** No pak files were found in
`/quakedata`. The mount has to contain `id1/pak0.pak`, or pak files directly.
The log says which directories it looked at.

**The game starts but says "Playing shareware version" with your full install
mounted.** `pak1.pak` is missing or is not readable by the container's user.
The startup log says which of the two it found.

**Settings and savegames do not survive a restart.** There is no volume on
`/quake/state`. Add `-v quake-state:/quake/state`.

**"state directory is not writable".** A host directory is bind-mounted at
`/quake/state` and is owned by somebody else. Either run the container as
yourself (`--user "$(id -u):$(id -g)"`) or `chown` the directory. A named
volume needs neither.

**No sound.** Check the start screen says which build the page is running and
that it matches the container's log — a browser quietly running a cached client
from an older image looks identical from the container's side. Then check
`docker logs` for the `audiostream` line. iOS routes the sound through a media
element; if the phone is on silent, it is silent.

**The picture stutters.** `docker logs` reports a gap in the picture that
interrupted a stream that was flowing, and says whether the browser stopped
asking for frames or x11vnc stopped answering — those are opposite faults. A
software renderer at 1920x1200 on a starved container is a third possibility;
try `QUAKE_WIDTH=640`.

**The engine crashes at startup, or "Quake crashed (SIGSEGV, status 139)".**
The log now carries what it needs to diagnose that. Since 1.0.1 the engine
line-buffers its output, so everything it printed on the way down survives the
crash, and it prints a backtrace before it goes:

```
=== Quake died on signal SIGSEGV (bad address) at 0x... ===
/usr/local/games/xquake(Mod_LoadTexinfo+0x1f)[0x...]
...
```

Include that whole block and the twenty or so lines above it. The last thing
the engine printed says how far it got, and the top named frame says where it
went. Before 1.0.1 a crash during startup printed nothing at all, because
stdout is a pipe and the library held the log in a buffer the crash never
flushed.

Also worth checking in that log: the line listing what ended up in each game
directory. If your mount holds an extracted copy of the game as well as the
pak files, the engine searches the loose files first, and a stale or partial
one there will be found in preference to the good copy in the pak.

**"connection lost" with the container still running.** One of the supporting
processes exited. The container notices within a second and prints the reason
from that process's own log. The usual cause is a `QUAKE_VNC_ARGS` value that
x11vnc did not recognise.

**The whole picture is in the wrong colours.** Shapes right, palette wrong —
magenta and teal where the walls should be brown. This was the 8-bit path: the
engine's palette lived in a private X colormap, x11vnc read that colormap to
build the truecolour picture the browser gets, and when the window it belonged
to was replaced the mapping went stale with no way back short of reconnecting.
The screen is depth 24 by default now, so there is no colormap in the path at
all. If you have set `QUAKE_X_DEPTH=8`, this is the cost of it; unset it.

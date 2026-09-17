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

---

## Game data

`/quakedata` is your Quake install directory: the one holding `id1/pak0.pak`.
Nothing is bundled and nothing can be — see [README.md](README.md#your-own-game-data).

```
/path/to/quake/
├── id1/pak0.pak          shareware: episode 1
│   └── pak1.pak          registered: all four episodes
├── hipnotic/pak0.pak     Scourge of Armagon
├── rogue/pak0.pak        Dissolution of Eternity
├── <mod>/                anything else
└── music/track02.ogg     the soundtrack, see below
```

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

| | |
| --- | --- |
| `QUAKE_GAME=hipnotic` | Scourge of Armagon — passes `-hipnotic`, which also changes the status bar and the menu |
| `QUAKE_GAME=rogue` | Dissolution of Eternity — passes `-rogue`, likewise |
| `QUAKE_GAME=<dir>` | any other mod — passes `-game <dir>` |

The directory has to be in the mount. A mod that ships loose files rather than
a pak works too: a directory holding `progs.dat` or a `maps/` folder is
recognised as a game directory even with no pak in it.

### Music

Quake's soundtrack is audio tracks 2 to 11 of the CD and is not in the pak
files. Put a rip in `/quakedata/music` as `track02.ogg`, `track03.ogg` and so
on. Ogg Vorbis, Opus, FLAC and WAV always work; MP3 works where the installed
libsndfile was built with it.

`QUAKE_MUSICDIR` overrides where to look. In the game, `cd info` says which
directory was found and what is playing; `cd play 4`, `cd loop 4`, `cd stop`,
`cd pause` and `cd resume` work as they always did. `eject`, `close` and
`reset` do not, and say so.

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

The renderer is in software, so every pixel costs. 640x480 is comfortable
anywhere; 1280x800 is fine on a modern core; 1920x1200 is a choice. The browser
scales whatever it is given to fit the window, so a lower number is a softer
picture rather than a smaller one.

`QUAKE_WIDTH` and `QUAKE_HEIGHT` are only where it starts. **Options → Video
Options** in the game lists every mode from 320x240 up to the maximum and
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
| `QUAKE_VNC_8TO24` | `1` | `0` sends the picture as colour-mapped depth 8: 64 colours in a browser, not 256. A diagnostic, not a way to play |
| `QUAKE_VNC_WAIT` | `5` | x11vnc poll interval, ms |
| `QUAKE_VNC_DEFER` | `5` | x11vnc update defer, ms |
| `QUAKE_VNC_ARGS` | | Extra x11vnc options. Each must start with a dash, and the container checks — x11vnc answers an option it does not recognise by exiting, which leaves the game running and the browser saying "connection lost" with nothing to explain it |

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
**B** does the same thing without leaving the game at all.

The `` ` `` key opens the console, and passes through untouched — which is why
the menu is on a button rather than moved onto `` ` ``. The console is how you
load a map, change the skill or start the music, and it is worth more than a
second way to reach a menu.

**Options → Customize controls** rebinds everything, and what you set there is
also what the page's controller panel reads. "Everything" is thirty-one
actions now, including the weapon keys, the console, the scoreboard, pause and
the screenshot key: the 1996 menu stopped at eighteen because eighteen rows is
all that fits on a 320x200 screen, so the rest could only be bound by typing
`bind` at the console. The list scrolls, with `^ more above` and `v more below`
to say which way there is more.

**Options → Video Options** changes the resolution while the game is running:
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

A pad appears under the start screen's buttons once the browser has seen one.
Everything about it happens in the page: the container has never heard of a
controller, and the engine is sent the same keysyms and pointer reports a
keyboard and a captured mouse produce.

Defaults, on a standard mapping (an Xbox pad, a Backbone One, most others):

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
| View / Menu | scores / console |
| Stick clicks | axe / thunderbolt |
| D-pad | forward, back, turn |

Every button is rebindable in the browser rather than in `config.cfg`, because
the container cannot see the pad. Deadzone, turn speed, inversion, stick swap
and whether a full push runs are all there too. Saved per browser, so a phone
and a desktop keep their own layouts.

The page reads the engine's own `config.cfg` to find out which key each action
should press — Quake binds keys to commands, so a pad pressing the stock keys
goes dead in a level the moment anybody rebinds anything, while still working
perfectly in the menus, where the engine hardcodes the arrows and Return. If a
control is bound to something a browser cannot produce (a joystick button, the
mouse wheel), the panel says so rather than quietly pressing something else.

When a pad misbehaves the page writes what it is doing into the container's
log — which pad it found, what each control is going to send, and what it
actually sent — so `docker logs` answers the question on its own.

---

## Troubleshooting

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

**Colours are wrong in a native VNC client.** Use the browser, or accept 64
colours. The engine's palette is a private X colormap; x11vnc's `-8to24` reads
it and presents truecolour, which is what the browser gets.

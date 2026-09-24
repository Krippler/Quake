#!/bin/sh
#
# Start the engine against synthetic data, on a real X server, and check that
# it draws a frame and produces sound.
#
# There is no game data in this repository and there cannot be, so this makes
# its own: tools/make-test-data.py writes an id1/pak0.pak holding the palette,
# the colormap, a gfx.wad with every lump the startup path asks for by name,
# and a 440 Hz tone. None of it is id's and none of it is a level -- the engine
# reaches the console and stops there, which is as far as anything without
# progs.dat and a BSP can go.
#
# What it therefore proves: the pak and WAD readers, the zone and hunk
# allocators, the palette upload, the 2D blitter, the character cell layout,
# the X11 8-bit path, the mixer, the resampler, and a clean shutdown on a
# signal. What it does not touch: the 3D renderer, the server, and the
# QuakeC interpreter.
#
# Given real game data it goes further. Point QUAKE_SMOKE_DATA at a directory
# holding id1/pak0.pak and a second phase loads E1M1, which does reach all
# three: SV_SpawnServer, progs.dat and the QuakeC interpreter, and a frame of
# real geometry through the software renderer. The shareware pak is enough --
# it has progs.dat, the models and E1M1 to E1M8 -- and id distributes it free
# of charge, so anybody can run this phase; it just cannot live in the
# repository.
#
#   tools/smoke-test.sh [builddir]
#   QUAKE_SMOKE_DATA=/path/to/quake tools/smoke-test.sh
#
# Needs Xvfb. xwd and ImageMagick's convert are used for the screenshots if
# they are there, and skipped if not.
#
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
work="${1:-${TMPDIR:-/tmp}/quake-smoke}"
disp="${QUAKE_SMOKE_DISPLAY:-:97}"
engine="$here/WinQuake/linux/xquake"

say() { printf '[smoke] %s\n' "$*"; }
die() { printf '[smoke] FAILED: %s\n' "$*" >&2; exit 1; }

[ -x "$engine" ] || die "no engine at $engine -- run: make -C WinQuake"
command -v Xvfb >/dev/null 2>&1 || die "Xvfb is not installed"

rm -rf "$work"
mkdir -p "$work"
cd "$work"

say "building synthetic game data"
python3 "$here/tools/make-test-data.py" "$work" >/dev/null

# Deliberately larger than the window the engine is asked for, so that the
# screen resize -resizescreen does at startup is something this test can see
# happen. An X server's maximum screen size is fixed when it starts, which is
# why the container starts Xvfb at the largest mode rather than the one in use.
# The depth the container runs at by default. 8 is the other one the engine
# supports, and QUAKE_SMOKE_DEPTH=8 runs the whole suite through it; the
# last phase checks that path either way, so both stay covered.
depth="${QUAKE_SMOKE_DEPTH:-24}"
case "$depth" in
    8|24) ;;
    *) die "QUAKE_SMOKE_DEPTH: $depth is not 8 or 24" ;;
esac

say "starting Xvfb on $disp at 800x600x$depth"
# -noreset, as the container's entrypoint passes: without it Xvfb resets the
# server when the last client disconnects, and the second phase below -- which
# connects after the first engine has exited -- lands in the middle of that
# and cannot open the display at all.
Xvfb "$disp" -screen 0 "800x600x$depth" -nolisten tcp -noreset >"$work/xvfb.log" 2>&1 &
xvfb_pid=$!

cleanup() {
    [ -n "${game_pid:-}" ] && kill "$game_pid" 2>/dev/null || true
    [ -n "${cat_pid:-}" ] && kill "$cat_pid" 2>/dev/null || true
    kill "$xvfb_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

i=0
while [ ! -e "/tmp/.X11-unix/X${disp#:}" ]; do
    i=$((i + 1))
    [ "$i" -gt 100 ] && die "Xvfb did not start; see $work/xvfb.log"
    sleep 0.1
done

mkfifo "$work/audio.fifo"
cat "$work/audio.fifo" > "$work/pcm.raw" &
cat_pid=$!

say "running the engine"
DISPLAY="$disp" QUAKE_AUDIO_FIFO="$work/audio.fifo" \
    "$engine" -basedir "$work" -resizescreen -width 640 -height 480 \
    >"$work/quake.log" 2>&1 &
game_pid=$!

sleep 8

kill -0 "$game_pid" 2>/dev/null || die "the engine exited; see $work/quake.log"

# ------------------------------------------------------------------ video mode
#
# xdpyinfo opens a fresh connection each time it runs, which is the point:
# DisplayWidth/DisplayHeight are cached per connection and do not follow a
# RANDR resize, so anything holding one open reports the old size.
#
screen_size() {
    DISPLAY="$disp" xdpyinfo 2>/dev/null \
        | sed -n 's/^  dimensions: *\([0-9]*x[0-9]*\) .*/\1/p' | head -1
}

if command -v xdpyinfo >/dev/null 2>&1; then
    size=$(screen_size)
    [ "$size" = "640x480" ] \
        || die "the screen is $size; -resizescreen should have brought 800x600 down to 640x480"
    say "the engine resized the screen to $size"

    # And at runtime, which is what the Video Options menu does: it writes
    # vid_width and vid_height and the next frame picks them up.
    #
    # Waited for rather than slept on: typing goes in through XTEST and the
    # engine acts on it a frame later, and how long that takes is up to
    # whatever else the machine is doing. A fixed sleep here is a test that
    # passes on a laptop and flakes on a shared runner.
    wait_for_screen() {
        i=0
        while [ "$i" -lt 100 ]; do
            [ "$(screen_size)" = "$1" ] && return 0
            i=$((i + 1))
            sleep 0.2
        done
        return 1
    }

    if command -v xdotool >/dev/null 2>&1; then
        # XTEST delivers to whatever has the input focus, and with no window
        # manager that is the window under the pointer.
        DISPLAY="$disp" xdotool mousemove 100 100
        DISPLAY="$disp" xdotool key grave
        sleep 0.5
        DISPLAY="$disp" xdotool type --delay 25 "vid_width 512;vid_height 384"
        DISPLAY="$disp" xdotool key Return
        wait_for_screen 512x384 \
            || die "after vid_width/vid_height the screen is $(screen_size), not 512x384"
        say "a mode change at runtime took the screen to 512x384"

        # Back up again, which is the direction that used to crash: the
        # console is redrawn into the new, larger framebuffer part-way through
        # the reallocation. See block_drawing in vid_x.c.
        DISPLAY="$disp" xdotool type --delay 25 "vid_width 640;vid_height 480"
        DISPLAY="$disp" xdotool key Return
        wait_for_screen 640x480 \
            || die "the screen did not come back to 640x480 (it is $(screen_size))"
        kill -0 "$game_pid" 2>/dev/null \
            || die "the engine died changing mode to a larger one; see $work/quake.log"
        say "and back up to 640x480, which is the direction that used to crash"
        DISPLAY="$disp" xdotool key grave
        sleep 0.5
    else
        say "xdotool missing; skipping the runtime mode change"
    fi
else
    say "xdpyinfo missing; skipping the video mode checks"
fi

# --------------------------------------------------------------------- picture
if command -v xwd >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
    DISPLAY="$disp" xwd -name xquake > "$work/screen.xwd" 2>/dev/null \
        || die "could not capture the window -- is it there at all?"
    convert "$work/screen.xwd" "$work/screen.png"
    say "screenshot: $work/screen.png"

    # A console screen has the background, the text and the version string in
    # it. One colour means the engine drew nothing at all.
    colours=$(convert "$work/screen.png" -format %k info:)
    [ "$colours" -ge 3 ] || die "the window holds $colours colour(s); nothing was drawn"
    say "the window holds $colours distinct colours"
else
    say "xwd or convert missing; skipping the screenshot"
fi

# ---------------------------------------------------------------------- sound
kill -TERM "$game_pid"
i=0
while kill -0 "$game_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.1
done
game_pid=""
kill "$cat_pid" 2>/dev/null || true
cat_pid=""
sleep 1

grep -q "Received signal 15, shutting down" "$work/quake.log" \
    || die "the engine did not shut down through the signal handler"
grep -q "VID_Shutdown" "$work/quake.log" \
    || die "the engine did not get as far as VID_Shutdown"

python3 - "$work/pcm.raw" <<'PY'
import struct, sys

data = open(sys.argv[1], 'rb').read()
frames = len(data) // 4
if frames < 22050 * 4:
    raise SystemExit("[smoke] FAILED: only %.2f s of audio came out of the pipe"
                     % (frames / 22050.0))

left = struct.unpack('<%dh' % (frames * 2), data)[0::2]
loud = [i for i, v in enumerate(left) if abs(v) > 500]
if not loud:
    raise SystemExit("[smoke] FAILED: the pipe carried nothing but silence")

# Half a second from where the sound starts. The fixture plays one 0.6 s tone
# at startup and then nothing, so measuring from the first loud sample to the
# last stretched the window across however long the engine happened to stay up
# afterwards -- which made the pitch a function of the test's own runtime and
# read 440 Hz as 33 Hz once this phase grew a few seconds longer.
seg = left[loud[0]:loud[0] + 22050 // 2]
crossings = sum(1 for i in range(1, len(seg)) if (seg[i-1] < 0) != (seg[i] < 0))
hz = crossings / 2 / (len(seg) / 22050.0)

print("[smoke] %.2f s of audio, tone at %.0f Hz (the fixture's is 440)"
      % (frames / 22050.0, hz))

# Resampling 11025 to 22050 wrong is the failure this catches, and it shows up
# as the pitch being out by a factor of two rather than by a few per cent.
if not 400 <= hz <= 480:
    raise SystemExit("[smoke] FAILED: the tone came out at %.0f Hz, not 440" % hz)
PY

# ----------------------------------------------------------------- a long stall
#
# What a frame longer than the sound ring does to the sound.
#
# The ring is 0.74 s. GetSoundtime used to reconstruct the playback position by
# counting the times that ring wrapped, one per call -- so a frame that took
# longer than the ring lost a wrap, and lost it for good, because nothing ever
# recounted. paintedtime follows soundtime, SNDDMA_Submit will not send past
# paintedtime, and so every frame after that was padded with silence instead of
# carried from the mixer. At the right rate, for the rest of the run: the
# listener never underran and nothing reported a fault. A level load was long
# enough to do it.
#
# SIGSTOP is the shortest way to a frame that long. Three seconds is four rings.
#
say "phase one and a half: the mixer across a three-second stall"

rm -f "$work/stall.fifo" "$work/stall-pcm.raw"
mkfifo "$work/stall.fifo"
cat "$work/stall.fifo" > "$work/stall-pcm.raw" &
cat_pid=$!

DISPLAY="$disp" QUAKE_AUDIO_FIFO="$work/stall.fifo" \
    "$engine" -basedir "$work" -width 320 -height 200 \
    +volume 1 +"play misc/talk" >"$work/stall.log" 2>&1 &
game_pid=$!

sleep 6
kill -0 "$game_pid" 2>/dev/null || die "the engine exited; see $work/stall.log"

kill -STOP "$game_pid"
sleep 3
kill -CONT "$game_pid"

sleep 8

kill -TERM "$game_pid"
i=0
while kill -0 "$game_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.1
done
game_pid=""
kill "$cat_pid" 2>/dev/null || true
cat_pid=""
sleep 1

python3 - "$work/stall-pcm.raw" <<'PY'
import struct, sys

RATE = 22050

data = open(sys.argv[1], 'rb').read()
frames = len(data) // 4
if frames < RATE * 12:
    raise SystemExit("[smoke] FAILED: only %.2f s of audio across the stall"
                     % (frames / float(RATE)))

left = struct.unpack('<%dh' % (frames * 2), data)[0::2]


def peak(a, b):
    seg = left[a * RATE:b * RATE]
    return max(abs(v) for v in seg) if seg else 0


# The stall is at 6 s and lasts 3. Before it the tone is playing; the two
# seconds either side of the restart are where the engine is catching up and
# are not asserted on. What matters is that the tone is still arriving well
# after it.
before = peak(3, 5)
after = peak(11, frames // RATE)

if before < 500:
    raise SystemExit("[smoke] FAILED: no tone before the stall (peak %d), so "
                     "this phase proves nothing" % before)

if after < 500:
    raise SystemExit("[smoke] FAILED: the mixer stopped reaching the pipe "
                     "after a 3 s stall (peak %d before, %d after). The "
                     "playback position has fallen behind and does not "
                     "recover." % (before, after))

print("[smoke] tone peak %d before the stall, %d after it" % (before, after))
PY

# ------------------------------------------------------------------- the music
#
# Which music directory wins.
#
# cd_stream.c looks at -musicdir, then <gamedir>/music, then $QUAKE_MUSICDIR,
# then <basedir>/id1/music. The order matters because the mission packs have
# soundtracks of their own and a rip of each goes beside its own paks, while
# $QUAKE_MUSICDIR can only ever name one directory for every game the container
# can run. It used to come first, and the entrypoint used to set it to
# id1/music -- between them, a mission pack played Quake's music over its own.
#
# The fixture writes 880 Hz into the game's own directory and 440 into the
# shared one. With both present and the environment pointing at the shared one,
# what should come out is 880.
#
say "phase one and three quarters: the music directory the engine picks"

rm -f "$work/music.fifo" "$work/music-pcm.raw"
mkfifo "$work/music.fifo"
cat "$work/music.fifo" > "$work/music-pcm.raw" &
cat_pid=$!

DISPLAY="$disp" QUAKE_AUDIO_FIFO="$work/music.fifo" \
    QUAKE_MUSICDIR="$work/shared-music" \
    "$engine" -basedir "$work" -width 320 -height 200 \
    +volume 1 +bgmvolume 1 +"cd loop 2" >"$work/music.log" 2>&1 &
game_pid=$!

sleep 12

kill -TERM "$game_pid"
i=0
while kill -0 "$game_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.1
done
game_pid=""
kill "$cat_pid" 2>/dev/null || true
cat_pid=""
sleep 1

grep -q "music from $work/id1/music" "$work/music.log" \
    || die "the engine chose $(grep -i 'music from' "$work/music.log" | head -1)
        rather than the game's own directory; see $work/music.log"

python3 - "$work/music-pcm.raw" <<'PY'
import struct, sys

RATE = 22050

data = open(sys.argv[1], 'rb').read()
frames = len(data) // 4
if frames < RATE * 8:
    raise SystemExit("[smoke] FAILED: only %.2f s of audio while music played"
                     % (frames / float(RATE)))

left = struct.unpack('<%dh' % (frames * 2), data)[0::2]

# From four seconds in, well clear of the 0.6 s startup tone quake.rc plays.
seg = left[RATE * 4:RATE * 8]
peak = max(abs(v) for v in seg)
if peak < 500:
    raise SystemExit("[smoke] FAILED: the music never reached the mixer "
                     "(peak %d)" % peak)

crossings = sum(1 for i in range(1, len(seg)) if (seg[i-1] < 0) != (seg[i] < 0))
hz = crossings / 2 / (len(seg) / float(RATE))

print("[smoke] music came out at %.0f Hz (880 = the game's own directory, "
      "440 = $QUAKE_MUSICDIR)" % hz)

if not 800 <= hz <= 960:
    raise SystemExit("[smoke] FAILED: the music came out at %.0f Hz. The "
                     "game's own music directory did not win." % hz)
PY

# --------------------------------------------------------------------- BSP2
#
# The two map layouts, loading the same map.
#
# BSP29 is id's; BSP2 is what the Quake re-release and modern compilers emit,
# and it exists because the 1996 index and bounds widths were too small. Six of
# the fifteen lumps differ; the rest are byte-identical.
#
# So: take a map the engine already reads, rewrite it as BSP2, load both, and
# compare what the readers built. bspchecksum walks the loaded world and runs a
# CRC over the values -- with pointers turned into indices, since those are
# hunk addresses and differ between runs by construction.
#
# Comparing rendered frames cannot do this job: Quake animates textures and
# entities against the clock, so two runs of the *same* map do not match each
# other. That was tried first and it is why this compares the model instead.
#
# This phase runs before phase two sets $real up, so it finds the pak itself,
# accepting either of the two layouts phase two accepts.
smoke_pak=""
if [ -n "${QUAKE_SMOKE_DATA:-}" ]; then
    if [ -f "$QUAKE_SMOKE_DATA/id1/pak0.pak" ]; then
        smoke_pak="$QUAKE_SMOKE_DATA/id1/pak0.pak"
    elif [ -f "$QUAKE_SMOKE_DATA/pak0.pak" ]; then
        smoke_pak="$QUAKE_SMOKE_DATA/pak0.pak"
    fi
fi

if [ -n "$smoke_pak" ]; then
    say "phase one and seven eighths: BSP29 and BSP2 load the same"

    bsp2dir="$work/bsp2"
    rm -rf "$bsp2dir"
    mkdir -p "$bsp2dir/id1/maps"
    ln -sfn "$smoke_pak" "$bsp2dir/id1/pak0.pak"

    # gfx/pop.lmp is how the engine decides it is the registered game, and
    # without it a loose file with a '/' in its name is unreachable -- see
    # COM_FindFile. The contents are the pop[] table in common.c, so the
    # fixture can carry it without any of id's data.
    python3 "$here/tools/make-pop-pak.py" "$here/WinQuake/common.c" \
        "$bsp2dir/id1/pak1.pak" >/dev/null

    python3 - "$smoke_pak" "$bsp2dir/e1m1.bsp" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
ofs, ln = struct.unpack_from('<ii', d, 4)
for i in range(ln // 64):
    name = d[ofs+i*64:ofs+i*64+56].split(b'\x00')[0].decode()
    fo, fl = struct.unpack_from('<ii', d, ofs+i*64+56)
    if name == 'maps/e1m1.bsp':
        open(sys.argv[2], 'wb').write(d[fo:fo+fl])
        break
else:
    raise SystemExit("[smoke] FAILED: no maps/e1m1.bsp in the pak")
PY

    cp "$bsp2dir/e1m1.bsp" "$bsp2dir/id1/maps/b29.bsp"
    python3 "$here/tools/bsp29to2.py" "$bsp2dir/e1m1.bsp" \
        "$bsp2dir/id1/maps/b2.bsp" >/dev/null \
        || die "the BSP29 to BSP2 converter failed"

    # -bspchecksum prints as the world is loaded, so this needs no console and
    # no keystrokes; the engine is killed once it has said its piece.
    bsp_sum() {
        DISPLAY="$disp" "$engine" -basedir "$bsp2dir" -nosound -bspchecksum \
            -width 320 -height 200 +map "$1" >"$work/bsp-$1.log" 2>&1 &
        bsp_pid=$!
        i=0
        while [ "$i" -lt 60 ]; do
            grep -q 'bspchecksum' "$work/bsp-$1.log" 2>/dev/null && break
            kill -0 "$bsp_pid" 2>/dev/null || break
            i=$((i + 1))
            sleep 0.5
        done
        kill -TERM "$bsp_pid" 2>/dev/null || true
        wait "$bsp_pid" 2>/dev/null || true

        grep -o 'bspchecksum [0-9]*' "$work/bsp-$1.log" | head -1
        grep -o 'verts .*' "$work/bsp-$1.log" | head -1
    }

    sum29=$(bsp_sum b29)
    sum2=$(bsp_sum b2)

    [ -n "$sum29" ] || die "the BSP29 map produced no checksum; see $work/bsp-b29.log"
    [ -n "$sum2" ]  || die "the BSP2 map produced no checksum; see $work/bsp-b2.log"

    if [ "$sum29" != "$sum2" ]; then
        printf '[smoke] BSP29: %s\n[smoke] BSP2 : %s\n' "$sum29" "$sum2" >&2
        die "the BSP2 reader built a different model from the same map"
    fi

    say "both layouts built the same model ($(printf '%s' "$sum29" | head -1))"
fi

#
# Phase two: real game data, if there is any.
#
# Everything above runs without a byte of id's content, which is the point --
# but it stops at the console. A map is where the parts a 64-bit build most
# nearly broke actually get exercised: the BSP loader, the texture coordinate
# basis in Mod_LoadTexinfo, the surface cache, the alias model renderer, the
# particle field, the server and progs.dat.
#
DATA="${QUAKE_SMOKE_DATA:-}"

if [ -z "$DATA" ]; then
    say "PASSED (no QUAKE_SMOKE_DATA, so the console phase only)"
    exit 0
fi

[ -f "$DATA/id1/pak0.pak" ] || [ -f "$DATA/pak0.pak" ] \
    || die "QUAKE_SMOKE_DATA is $DATA, which holds no id1/pak0.pak"

say "phase two: a real map, from $DATA"

real="$work/real"
mkdir -p "$real/id1"
if [ -f "$DATA/id1/pak0.pak" ]; then
    src="$DATA/id1"
else
    src="$DATA"
fi
for f in "$src"/*.pak "$src"/*.PAK; do
    [ -f "$f" ] || continue
    ln -sfn "$f" "$real/id1/$(basename "$f" | tr 'A-Z' 'a-z')"
done

rm -f "$work/real-audio.fifo" "$work/real-pcm.raw"
mkfifo "$work/real-audio.fifo"
cat "$work/real-audio.fifo" > "$work/real-pcm.raw" &
cat_pid=$!

#
# A fixed map and a fixed skill, so the run is the same every time, and one
# sound played outright.
#
# Standing on E1M1's spawn point is quiet: the ambients are positioned and
# there is none within earshot, and nothing else happens while nobody moves.
# So the sound this phase checks for is asked for rather than waited on --
# `play` loads a real WAV lump out of the pak and hands it to the mixer, which
# is the thing the synthetic phase cannot cover.
#
DISPLAY="$disp" QUAKE_AUDIO_FIFO="$work/real-audio.fifo" \
    "$engine" -basedir "$real" -width 640 -height 480 +skill 1 +map e1m1 \
    +"play weapons/r_exp3" >"$work/real.log" 2>&1 &
game_pid=$!

sleep 10
if ! kill -0 "$game_pid" 2>/dev/null; then
    rc=0
    wait "$game_pid" || rc=$?
    say "engine exit status $rc; log follows"
    cat "$work/real.log" >&2 || true
    die "the engine exited during the map; see $work/real.log"
fi

# progs.dat loaded and its CRC matched what progdefs.h expects. Had the QuakeC
# interpreter not come up, this line would be a Sys_Error instead.
grep -q "SERVER (.*CRC)" "$work/real.log" \
    || die "no server version line: progs.dat did not load; see $work/real.log"
say "$(grep -o 'VERSION [0-9.]* SERVER (.*CRC)' "$work/real.log" | head -1)"

for bad in "Sys_Error" "Hunk_Alloc: failed" "not enough memory" "Bad surface"; do
    if grep -qi "$bad" "$work/real.log"; then
        die "the engine reported: $(grep -i "$bad" "$work/real.log" | head -1)"
    fi
done

if command -v xwd >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
    # Started without -resizescreen, this is a desktop window, which is titled
    # "Quake"; the container's is "xquake". See vid_x.c.
    DISPLAY="$disp" xwd -name Quake > "$work/map.xwd" 2>/dev/null \
        || die "could not capture the window during the map"
    convert "$work/map.xwd" "$work/map.png"
    say "screenshot: $work/map.png"

    #
    # How many colours a frame of real geometry holds.
    #
    # This is the assertion that would have caught the Mod_LoadTexinfo bug. A
    # map whose second texture axis is garbage still draws -- walls, floors,
    # a status bar -- so "the engine did not crash" says nothing at all. A
    # frame of correctly mapped, correctly lit Quake uses most of a 256-colour
    # palette; flat-shaded wreckage does not.
    #
    colours=$(convert "$work/map.png" -format %k info:)
    [ "$colours" -ge 64 ] \
        || die "the map frame holds only $colours colours; expected most of a palette"
    say "the map frame holds $colours distinct colours"
fi

kill -TERM "$game_pid"
i=0
while kill -0 "$game_pid" 2>/dev/null && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.1
done
game_pid=""
kill "$cat_pid" 2>/dev/null || true
cat_pid=""
sleep 1

# config.cfg goes to the game directory, which here has the same shape as the
# one the container assembles: symlinks to the paks, and the engine's own
# files written beside them.
[ -f "$real/id1/config.cfg" ] \
    || die "the engine did not write config.cfg to its game directory"
say "config.cfg written beside the linked paks"

python3 - "$work/real-pcm.raw" <<'ENDPY'
import struct, sys

data = open(sys.argv[1], 'rb').read()
frames = len(data) // 4
if frames < 22050 * 4:
    raise SystemExit("[smoke] FAILED: only %.2f s of audio during the map"
                     % (frames / 22050.0))

pairs = struct.unpack('<%dh' % (frames * 2), data)
left, right = pairs[0::2], pairs[1::2]

peak = max(abs(v) for v in left)
if peak < 500:
    raise SystemExit("[smoke] FAILED: nothing came out of the mixer; the WAV "
                     "lumps in the pak did not reach it")

# Reported, not asserted. A centred sound is meant to come out centred, so a
# low figure here says nothing was positioned rather than that the panner is
# broken -- which is exactly what standing still on a spawn point looks like.
apart = sum(1 for i in range(0, frames, 32) if abs(left[i] - right[i]) > 200)
print("[smoke] %.2f s of audio, peak %d, %d sampled points off centre"
      % (frames / 22050.0, peak, apart))
ENDPY

# ------------------------------------------------------------- game directories
#
# The menu's game switching, without the menu.
#
# Picking a mission pack or a mod writes its directory name beside the game
# directories and quits; COM_InitFilesystem reads it on the way back up, and
# the container's restart loop is what closes the circle. What is checked here
# is that half: a stored choice puts the directory on the search path, a stored
# choice for something that is not installed is ignored rather than followed
# into a search path with nothing behind it, and a path is not a directory
# name and is refused.
#
say "phase three: the stored game directory"

mkdir -p "$work/testmod"
cp "$work/id1/pak0.pak" "$work/testmod/pak0.pak"

game_run() {
    printf '%s\n' "$1" > "$work/nextgame"
    DISPLAY="$disp" "$engine" -basedir "$work" -width 320 -height 200 \
        >"$work/game-$2.log" 2>&1 &
    game_pid=$!
    sleep 5
    kill -TERM "$game_pid" 2>/dev/null || true
    wait "$game_pid" 2>/dev/null || true
    game_pid=""
}

game_run testmod chosen
grep -q "Added packfile $work/testmod/pak0.pak" "$work/game-chosen.log" \
    || die "a stored game directory did not reach the search path"
say "a stored 'testmod' put testmod/pak0.pak on the search path"

game_run nosuchmod missing
if grep -q "nosuchmod" "$work/game-missing.log"; then
    die "a stored game directory that is not installed was used anyway"
fi
say "a stored directory that is not installed was ignored"

game_run ../etc escape
if grep -q "etc" "$work/game-escape.log"; then
    die "a stored game directory reaching outside the base was used"
fi
say "a stored path rather than a directory name was refused"

rm -f "$work/nextgame"

# ---------------------------------------------------- phase four: the other depth
#
# The engine has two ways to reach the screen: write palette indices straight
# into an 8-bit PseudoColor visual, or translate every frame through a table
# into a deeper one. The container picks the second, which is why the suite
# above runs at depth 24 -- but the first is still there, still reachable with
# QUAKE_X_DEPTH=8, and nothing else exercises it.
#
# A whole second pass would double the run, so this is the cheap half: a
# separate X server at the depth the suite is not using, and a check that the
# engine gets as far as drawing the console on it.
#
other_depth=8
[ "$depth" = "8" ] && other_depth=24
say "phase four: depth $other_depth, the path the rest of this run did not take"

odisp=":$(( ${disp#:} + 1 ))"
Xvfb "$odisp" -screen 0 640x480x$other_depth -nolisten tcp -noreset \
    >"$work/xvfb-other.log" 2>&1 &
oxvfb_pid=$!
i=0
while [ ! -e "/tmp/.X11-unix/X${odisp#:}" ]; do
    i=$((i + 1))
    [ "$i" -gt 100 ] && die "Xvfb at depth $other_depth did not start"
    sleep 0.1
done

DISPLAY="$odisp" "$engine" -basedir "$work" -width 640 -height 480 \
    >"$work/quake-other.log" 2>&1 &
game_pid=$!
sleep 8
if ! kill -0 "$game_pid" 2>/dev/null; then
    kill "$oxvfb_pid" 2>/dev/null || true
    die "the engine exited at depth $other_depth; see $work/quake-other.log"
fi
kill -TERM "$game_pid" 2>/dev/null || true
wait "$game_pid" 2>/dev/null || true
game_pid=""
kill "$oxvfb_pid" 2>/dev/null || true

grep -q "Console initialized" "$work/quake-other.log" \
    || die "the engine did not reach the console at depth $other_depth"
say "the engine reached the console at depth $other_depth too"

say "PASSED (console, a real map, game directories, and both visual depths)"

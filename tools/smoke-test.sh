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

say "starting Xvfb on $disp at 640x480x8"
# -noreset, as the container's entrypoint passes: without it Xvfb resets the
# server when the last client disconnects, and the second phase below -- which
# connects after the first engine has exited -- lands in the middle of that
# and cannot open the display at all.
Xvfb "$disp" -screen 0 640x480x8 -nolisten tcp -noreset >"$work/xvfb.log" 2>&1 &
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
    "$engine" -basedir "$work" -width 640 -height 480 \
    >"$work/quake.log" 2>&1 &
game_pid=$!

sleep 8

kill -0 "$game_pid" 2>/dev/null || die "the engine exited; see $work/quake.log"

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

seg = left[loud[0]:loud[-1] + 1]
crossings = sum(1 for i in range(1, len(seg)) if (seg[i-1] < 0) != (seg[i] < 0))
hz = crossings / 2 / (len(seg) / 22050.0)

print("[smoke] %.2f s of audio, tone at %.0f Hz (the fixture's is 440)"
      % (frames / 22050.0, hz))

# Resampling 11025 to 22050 wrong is the failure this catches, and it shows up
# as the pitch being out by a factor of two rather than by a few per cent.
if not 400 <= hz <= 480:
    raise SystemExit("[smoke] FAILED: the tone came out at %.0f Hz, not 440" % hz)
PY

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
    DISPLAY="$disp" xwd -name xquake > "$work/map.xwd" 2>/dev/null \
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

say "PASSED (console, and a real map)"

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
#   tools/smoke-test.sh [builddir]
#
# Needs Xvfb. xwd and ImageMagick's convert are used for the screenshot if they
# are there, and skipped if not.
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
Xvfb "$disp" -screen 0 640x480x8 -nolisten tcp >"$work/xvfb.log" 2>&1 &
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

say "PASSED"

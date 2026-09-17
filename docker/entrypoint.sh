#!/bin/sh
#
# Bring up a private 8-bit X server, export it over VNC and noVNC, and run
# Quake on it. Any arguments given to the container are passed straight to the
# engine, e.g. `docker run ... quake +map e1m1 +skill 3`.
#
set -eu

WIDTH="${QUAKE_WIDTH:-640}"
HEIGHT="${QUAKE_HEIGHT:-480}"
DATADIR="${QUAKE_DATADIR:-/quakedata}"
STATE="${QUAKE_STATE:-/quake/state}"
VNC_PORT="${QUAKE_VNC_PORT:-5900}"
WEB_PORT="${QUAKE_WEB_PORT:-6080}"
AUDIO_PORT="${QUAKE_AUDIO_PORT:-5901}"
DISP="${QUAKE_DISPLAY:-:99}"
QUAKE_BIN="${QUAKE_BIN:-/usr/local/games/xquake}"
AUDIOSTREAM_BIN="${QUAKE_AUDIOSTREAM_BIN:-/usr/local/games/audiostream}"
WSPROXY_BIN="${QUAKE_WSPROXY_BIN:-/usr/local/bin/quake-wsproxy}"
NOVNC_ROOT="${QUAKE_NOVNC_ROOT:-/usr/share/novnc}"
RESTART="${QUAKE_RESTART:-1}"

# The engine's own rate, fixed in snd_stream.c. Here so the page and
# audiostream are told the same number the engine produces.
AUDIO_RATE=22050

log() { printf '[quake] %s\n' "$*" >&2; }
die() { printf '[quake] error: %s\n' "$*" >&2; exit 1; }

##############################################################################
# Drop privileges.
#
# Started as root -- which is how Unraid and most NAS front ends run a
# container -- take ownership of the writable directory as PUID:PGID and then
# run everything else as that user. On Unraid those are 99:100 (nobody:users),
# which is what its appdata share is owned by.
#
# Started as an ordinary user already, via --user or the image default, there
# is nothing to do and PUID/PGID are ignored.
##############################################################################
if [ "$(id -u)" = "0" ]; then
    PUID="${PUID:-1001}"
    PGID="${PGID:-1001}"

    case "$PUID$PGID" in
        *[!0-9]*) die "PUID and PGID must be numeric (got '$PUID' and '$PGID')" ;;
    esac

    mkdir -p "$STATE" 2>/dev/null || true
    chown "$PUID:$PGID" "$STATE" 2>/dev/null || true

    if [ "$PUID" = "0" ] && [ "$PGID" = "0" ]; then
        # Asking to stay root. Dropping to root is not a drop, and re-executing
        # would arrive back here as root and do it again, for ever.
        log "running as root (PUID=0)"
    else
        log "running as ${PUID}:${PGID}"
        exec setpriv --reuid "$PUID" --regid "$PGID" --clear-groups "$0" "$@"
    fi
fi

# Which build this is, printed after the drop so the re-exec above does not say
# it twice. The client carries the same stamp on its start screen, and the two
# disagreeing is the whole diagnosis when a browser is quietly running an older
# client than the container it is talking to -- which the container's own log
# cannot tell you, because nothing about it is wrong.
log "Quake ${QUAKE_VERSION:-dev}"

#
# How many cores this container is allowed, said out loud at startup.
#
# It is the one thing about how the container was started that the log could
# not otherwise tell you, and the first question anyone asks about a slow
# container is how much of the machine it was given. Quake's software renderer
# is the only thing in here that is genuinely CPU-hungry.
#
cpu_allowance () {
    cores=$(nproc 2>/dev/null || echo '?')

    # Pinning shows up in nproc, because it is an affinity mask. A quota does
    # not: it is a share of time across whatever cores are visible, so it has
    # to be read from the cgroup -- v2 first, then v1.
    quota=''

    if [ -r /sys/fs/cgroup/cpu.max ]; then
        read -r q p _ < /sys/fs/cgroup/cpu.max 2>/dev/null || q=max
        [ "$q" = max ] || quota=$(( q * 100 / p ))
    elif [ -r /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]; then
        q=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null || echo -1)
        p=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us 2>/dev/null || echo 100000)
        [ "$q" -le 0 ] 2>/dev/null || quota=$(( q * 100 / p ))
    fi

    if [ -n "$quota" ]; then
        log "${cores} core(s) visible, limited to ${quota}% of one"
    else
        log "${cores} core(s) available"
    fi
}

cpu_allowance

#
# The renderer's static tables are sized by MAXWIDTH and MAXHEIGHT in
# r_shared.h, and the span drawers step the framebuffer eight pixels at a time.
# The engine clamps and rounds these itself; catching it here means the Xvfb
# screen and the window end up the same size, which they would not if the
# engine quietly rounded 1023 down to 1016.
#
case "$WIDTH$HEIGHT" in
    *[!0-9]*) die "QUAKE_WIDTH and QUAKE_HEIGHT must be numbers (got '$WIDTH' and '$HEIGHT')" ;;
esac

[ "$WIDTH"  -ge 320 ] || die "QUAKE_WIDTH must be at least 320 (got $WIDTH)"
[ "$HEIGHT" -ge 200 ] || die "QUAKE_HEIGHT must be at least 200 (got $HEIGHT)"
[ "$WIDTH"  -le 1920 ] || die "QUAKE_WIDTH is limited to 1920 by the renderer (got $WIDTH)"
[ "$HEIGHT" -le 1200 ] || die "QUAKE_HEIGHT is limited to 1200 by the renderer (got $HEIGHT)"

if [ $(( WIDTH % 8 )) -ne 0 ]; then
    WIDTH=$(( WIDTH / 8 * 8 ))
    log "width rounded down to $WIDTH: the span drawers work eight pixels at a time"
fi

#
# An X server's maximum screen size is fixed when it starts and RANDR can only
# move around inside it, so Xvfb is started at the largest mode the renderer
# can draw and the engine shrinks the screen to whatever resolution is in play.
# That is what makes the in-game Video Options menu work: without it, only
# sizes below QUAKE_WIDTH/QUAKE_HEIGHT would be reachable.
#
# Setting these to the starting size pins the screen there and turns the menu's
# mode list into a list of things that will not happen. It costs about 2 MB of
# the server's memory to leave them alone.
#
MAX_WIDTH="${QUAKE_MAX_WIDTH:-1920}"
MAX_HEIGHT="${QUAKE_MAX_HEIGHT:-1200}"

case "$MAX_WIDTH$MAX_HEIGHT" in
    *[!0-9]*) die "QUAKE_MAX_WIDTH and QUAKE_MAX_HEIGHT must be numbers (got '$MAX_WIDTH' and '$MAX_HEIGHT')" ;;
esac

[ "$MAX_WIDTH"  -le 1920 ] || die "QUAKE_MAX_WIDTH is limited to 1920 by the renderer (got $MAX_WIDTH)"
[ "$MAX_HEIGHT" -le 1200 ] || die "QUAKE_MAX_HEIGHT is limited to 1200 by the renderer (got $MAX_HEIGHT)"

# The starting size has to fit inside it, or the engine could not reach it.
[ "$MAX_WIDTH"  -ge "$WIDTH" ]  || MAX_WIDTH="$WIDTH"
[ "$MAX_HEIGHT" -ge "$HEIGHT" ] || MAX_HEIGHT="$HEIGHT"


##############################################################################
# The state directory holds config.cfg, savegames, logs and the links to the
# game data, so it has to be writable. A bind-mounted host directory arrives
# owned by whoever created it, which is usually not the container's user.
##############################################################################
if ! mkdir -p "$STATE" 2>/dev/null || [ ! -w "$STATE" ]; then
    log "state directory '$STATE' is not writable by uid $(id -u)"
    log ""
    log "that happens when a host directory is bind-mounted there. Either run"
    log "the container as yourself:"
    log "    docker run --user \"\$(id -u):\$(id -g)\" ..."
    log "or hand the directory to the container's user:"
    log "    chown $(id -u):$(id -g) <that directory>"
    log ""
    log "a named volume, which is what docker-compose.yml uses, needs neither."
    die "cannot write to $STATE"
fi

##############################################################################
# Assemble a game directory the engine can write to.
#
# Quake writes config.cfg, savegames and screenshots into com_gamedir, which is
# <basedir>/id1 -- the same directory the pak files are in. Mount your game
# data read-only, as everybody does and as the README says to, and the engine
# cannot save your game or remember a single setting. It does not say so
# either: COM_WriteFile prints "failed on config.cfg" into a console nobody is
# looking at and carries on.
#
# So the basedir is in the state volume and the mounted pak files are linked
# into it. What gets written lands beside the links, in the volume, where it
# survives the container.
##############################################################################
BASEDIR="$STATE/game"
mkdir -p "$BASEDIR"

# Every subdirectory of the mount that holds pak files is a game directory:
# id1 for the base game, hipnotic and rogue for the mission packs, and whatever
# a mod calls itself.
link_game_dir() {
    src="$1"
    name="$2"
    dest="$BASEDIR/$name"
    n=0

    mkdir -p "$dest"

    for f in "$src"/*.pak "$src"/*.PAK "$src"/*.Pak; do
        [ -f "$f" ] || continue
        # The engine looks for pak0.pak, pak1.pak and so on, lowercase and in
        # order; anything not named that way is never opened, whatever it
        # contains. Link it under the name it already has and let the
        # lowercase-ing below deal with the rest.
        base=$(basename "$f" | tr 'A-Z' 'a-z')
        ln -sfn "$f" "$dest/$base"
        n=$((n + 1))
    done

    # Loose files too: a mod that ships progs.dat and a maps directory rather
    # than a pak is entirely normal, and so is an unpacked id1.
    for f in "$src"/*; do
        [ -e "$f" ] || continue
        base=$(basename "$f")
        case "$base" in
            *.pak|*.PAK|*.Pak) continue ;;
            config.cfg|CONFIG.CFG)
                # Linked, the engine would write the player's settings back
                # through the symlink into a read-only mount and fail, or into
                # the mount itself where it does not belong. Its own copy lives
                # beside the links and is written there.
                continue ;;
        esac
        ln -sfn "$f" "$dest/$(printf '%s' "$base" | tr 'A-Z' 'a-z')"
    done

    [ "$n" -gt 0 ] && return 0
    return 1
}

FOUND_GAMES=""

if [ -d "$DATADIR" ]; then
    # Paks sitting directly in the mount, with no id1 around them. A common
    # enough mistake that guessing is kinder than an error: nothing else is
    # ever mounted here.
    if [ -f "$DATADIR/pak0.pak" ] || [ -f "$DATADIR/PAK0.PAK" ]; then
        log "found pak files directly in $DATADIR; treating them as id1"
        link_game_dir "$DATADIR" id1 && FOUND_GAMES="id1"
    else
        for d in "$DATADIR"/*; do
            [ -d "$d" ] || continue
            name=$(basename "$d" | tr 'A-Z' 'a-z')

            # The soundtrack is not a game directory. Without this it becomes
            # one -- an empty mod called "music" in the engine's search path,
            # which is confusing in the log and would shadow a mod of that name.
            [ "$name" = music ] && continue

            if link_game_dir "$d" "$name"; then
                FOUND_GAMES="$FOUND_GAMES $name"
            else
                # No paks in it, but it may still be an unpacked mod.
                if [ -e "$BASEDIR/$name/progs.dat" ] || [ -d "$BASEDIR/$name/maps" ]; then
                    FOUND_GAMES="$FOUND_GAMES $name"
                else
                    # Not a game directory after all. Everything in there is a
                    # symlink this script has just made, so removing the lot is
                    # safe -- and rmdir alone would fail and leave it behind.
                    rm -rf "$BASEDIR/$name"
                fi
            fi
        done
    fi
fi

FOUND_GAMES=$(printf '%s' "$FOUND_GAMES" | sed 's/^ *//')

if [ -z "$FOUND_GAMES" ]; then
    log ""
    log "No game data found in $DATADIR."
    log ""
    log "Quake's data is not free to redistribute -- id's shareware licence"
    log "allows passing the shareware release along whole, in its original"
    log "archive, not a pak file lifted out of it -- so there is none in this"
    log "image and there never will be. You supply your own:"
    log ""
    log "    docker run --rm -p $WEB_PORT:$WEB_PORT \\"
    log "        -v /path/to/quake:$DATADIR:ro ghcr.io/krippler/quake"
    log ""
    log "where /path/to/quake is the directory holding id1/pak0.pak -- the"
    log "install directory from the CD, from Steam, from GOG, or the shareware"
    log "release unpacked. See README.md."
    log ""
    die "nothing to play"
fi

log "game data: $FOUND_GAMES"

#
# What actually ended up in each game directory.
#
# The engine searches the directory as well as the paks once it is running the
# registered game; for shareware, COM_FindFile skips any name with a '/' in it
# when it gets to a directory, so loose subdirectories are unreachable.
#
# The paks win. COM_AddGameDirectory pushes the directory onto com_searchpaths
# first and then each pak on top of it, so the list runs pak1, pak0, directory
# and a loose file is only reached when no pak holds that name. (An earlier
# version of this comment had that backwards.) It is still worth logging: a
# mount holding an extracted tree as well as the paks is a mount where
# something unexpected can be picked up, and "pak files directly in the mount"
# is the layout where that is most likely.
#
# Quiet for the ordinary case. A directory of nothing but pak links is what
# almost everybody has and needs no comment; anything else is worth seeing in
# the log of a container that then crashed.
#
for g in $FOUND_GAMES; do
    paks=$(find "$BASEDIR/$g" -maxdepth 1 -name '*.pak' 2>/dev/null | wc -l)
    other=$(find "$BASEDIR/$g" -maxdepth 1 -mindepth 1 ! -name '*.pak' \
                 ! -name 'config.cfg' ! -name '*.sav' ! -name '*.pcx' \
                 2>/dev/null | wc -l)

    if [ "$other" -gt 0 ]; then
        log "  $g: $paks pak(s) and $other loose entr(ies), which the engine"
        log "      falls back to for anything the paks do not hold:"
        find "$BASEDIR/$g" -maxdepth 1 -mindepth 1 ! -name '*.pak' \
             ! -name 'config.cfg' ! -name '*.sav' ! -name '*.pcx' \
             2>/dev/null | head -20 | while IFS= read -r entry; do
            log "        $(basename "$entry")"
        done
    fi
done

# The engine looks for gfx/pop.lmp to decide whether this is the registered
# game. Saying which it found up front saves the "why does it only have one
# episode" question.
if [ -f "$BASEDIR/id1/pak1.pak" ]; then
    log "  pak1.pak present: the registered game, all four episodes"
else
    log "  no pak1.pak: shareware, episode 1 only"
fi

##############################################################################
# Music.
#
# Quake's soundtrack was audio tracks 2 to 11 of the CD, so it is not in the
# pak files and never was. cd_stream.c looks for track02.ogg and friends in a
# music directory; point it at the mounted one if there is one.
##############################################################################
if [ -d "$DATADIR/music" ]; then
    export QUAKE_MUSICDIR="$DATADIR/music"
    log "music: $DATADIR/music"
elif [ -d "$DATADIR/id1/music" ]; then
    export QUAKE_MUSICDIR="$DATADIR/id1/music"
    log "music: $DATADIR/id1/music"
else
    log "music: none found (put track02.ogg and friends in $DATADIR/music)"
fi

##############################################################################
# Background services. Everything is torn down together.
##############################################################################
XVFB_PID=""
VNC_PID=""
WEB_PID=""
WEBLOG_PID=""
AUDIO_PID=""
GAME_PID=""
HELPERWATCH_PID=""

# Set once the stack is being torn down, so the restart loop at the bottom
# knows the engine's death was asked for rather than something to recover from.
SHUTTING_DOWN=0

cleanup() {
    trap - EXIT INT TERM
    SHUTTING_DOWN=1

    # The engine's signal handler raises a flag that the frame loop turns into
    # an ordinary quit -- writing config.cfg on the way out -- so ask it to
    # stop that way before pulling the display out from under it.
    if [ -n "$GAME_PID" ] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null || true
        i=0
        while kill -0 "$GAME_PID" 2>/dev/null && [ "$i" -lt 30 ]; do
            i=$((i + 1))
            sleep 0.1
        done
        kill -KILL "$GAME_PID" 2>/dev/null || true
    fi

    # First, so the teardown below cannot be reported as a fault: a normal quit
    # kills the helpers, and the watchdog would announce "noVNC has exited --
    # nothing works without it" on the way out, which is true and useless.
    if [ -n "$HELPERWATCH_PID" ]; then
        kill "$HELPERWATCH_PID" 2>/dev/null || true
        HELPERWATCH_PID=""
    fi

    for pid in "$WEBLOG_PID" "$WEB_PID" "$AUDIO_PID" "$VNC_PID" "$XVFB_PID"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

##############################################################################
# The resolution to start at.
#
# vid_width and vid_height are archived cvars, and config.cfg is exec'd well
# after VID_Init has run -- so the engine creates its window at the size on the
# command line and then resizes it a frame later to whatever the config says.
#
# That resize is harmless while the engine keeps running. It is not harmless
# just after the engine has been restarted: the window is new, and x11vnc goes
# on compositing this 8-bit screen through the colormap of the window that has
# gone. The browser then gets the right picture in the wrong 256 colours, and
# it does not recover, because x11vnc rebuilds that mapping only for a client
# that connects afresh. Measured at about 70% of pixels landing outside the
# palette; on screen it is the whole picture in the wrong colours.
#
# So start at the size the config asks for. Then nothing resizes, and the one
# sequence that produced it -- quit, restart, config restores a resolution --
# cannot arise. QUAKE_WIDTH and QUAKE_HEIGHT stay the default for a state
# volume that has no config yet, which is what the documentation says.
##############################################################################
config_size() {
    cfg="$BASEDIR/id1/config.cfg"
    [ -f "$cfg" ] || return 0

    cw=$(sed -n 's/^vid_width "\([0-9][0-9]*\)\.[0-9]*"$/\1/p' "$cfg" | tail -1)
    ch=$(sed -n 's/^vid_height "\([0-9][0-9]*\)\.[0-9]*"$/\1/p' "$cfg" | tail -1)

    [ -n "$cw" ] && [ -n "$ch" ] || return 0

    # The engine clamps these itself; doing it here too keeps the size the
    # engine ends up at and the size it was started at the same, which is the
    # whole point.
    [ "$cw" -ge 320 ] || cw=320
    [ "$ch" -ge 200 ] || ch=200
    [ "$cw" -le "$MAX_WIDTH" ]  || cw="$MAX_WIDTH"
    [ "$ch" -le "$MAX_HEIGHT" ] || ch="$MAX_HEIGHT"
    cw=$(( cw / 8 * 8 ))

    if [ "$cw" != "$WIDTH" ] || [ "$ch" != "$HEIGHT" ]; then
        log "config.cfg asks for ${cw}x${ch}; starting there rather than at"
        log "  ${WIDTH}x${HEIGHT} and resizing into it"
        WIDTH="$cw"
        HEIGHT="$ch"
    fi
}

config_size

##############################################################################
# The display.
#
# Quake's software renderer only ever learned to talk to an 8-bit PseudoColor
# visual -- it writes palette indices and uploads a colormap -- and no current
# X server offers one. Xvfb still does, which is the whole reason the picture
# goes through it rather than through something newer.
##############################################################################
log "starting Xvfb on $DISP at ${MAX_WIDTH}x${MAX_HEIGHT}x8, the largest mode the"
log "  renderer can draw; the engine brings the screen down to ${WIDTH}x${HEIGHT}"
Xvfb "$DISP" -screen 0 "${MAX_WIDTH}x${MAX_HEIGHT}x8" -nolisten tcp -noreset \
     >"$STATE/xvfb.log" 2>&1 &
XVFB_PID=$!

sock="/tmp/.X11-unix/X${DISP#:}"
i=0
while [ ! -e "$sock" ]; do
    i=$((i + 1))
    [ "$i" -gt 100 ] && die "Xvfb failed to start; see $STATE/xvfb.log"
    kill -0 "$XVFB_PID" 2>/dev/null || die "Xvfb exited; see $STATE/xvfb.log"
    sleep 0.1
done

export DISPLAY="$DISP"

##############################################################################
# VNC.
##############################################################################
vnc_auth=""
if [ -n "${QUAKE_VNC_PASSWORD:-}" ]; then
    x11vnc -storepasswd "$QUAKE_VNC_PASSWORD" "$STATE/.vncpasswd" >/dev/null 2>&1
    vnc_auth="-rfbauth $STATE/.vncpasswd"
    log "VNC password authentication enabled"
else
    vnc_auth="-nopw"
    log "no VNC password set (export QUAKE_VNC_PASSWORD to require one)"
fi

#
# -8to24 is how a depth 8 display is presented as truecolor.
#
# The engine creates its own colormap and installs it, because there is no
# window manager here to do it -- but noVNC cannot use a colour map at all, and
# at depth 8 it asks for two bits per channel, which is 64 colours out of
# Quake's 256. -8to24 walks the window tree, reads each window's colormap and
# transforms the screen, which is the most expensive thing x11vnc does here;
# its own manual says the mode "does hog resources". Turning it off is a
# diagnostic, not a way to play.
#
if [ "${QUAKE_VNC_8TO24:-1}" = "0" ]; then
    log "  -8to24 off by request: the picture will show 64 colours, not 256"
    vnc_8to24=""
else
    vnc_8to24="-8to24"
fi

#
# -wireframe and -scrollcopyrect are both on by default, and both are for a
# desktop: they watch for a window being dragged or a pane being scrolled while
# a mouse button is held, and hold the screen back while they decide. A game
# holds the fire button down. There is one window here, it never moves, and
# there is nothing to scroll -- so both are off, as they are in the DOOM
# container this one is modelled on, where the measurements were taken: with
# the button held, both on gave 257 KB/s at a 41 ms median, and both off gave
# 554 KB/s at 12 ms.
#
# x11vnc answers an option it does not recognise by printing "unrecognized
# option(s)" into its own log and exiting, which leaves the game running, the
# container looking healthy and a browser saying "connection lost" with no
# explanation anywhere anyone thinks to look. A value of "noxdamage" instead of
# "-noxdamage" is enough to do it.
#
for opt in ${QUAKE_VNC_ARGS:-}; do
    case "$opt" in
        -*) ;;
        *)  die "QUAKE_VNC_ARGS: '$opt' does not start with a dash; x11vnc would refuse the lot" ;;
    esac
done

#
# -xrandr resize: the engine resizes the X screen when the resolution changes
# in the Video Options menu. Without this x11vnc keeps serving a framebuffer of
# the size it first saw, so a change to a larger mode arrives cropped and a
# change to a smaller one leaves most of the picture stale. "resize" makes
# x11vnc rebuild its framebuffer and tell the client through NewFBSize, which
# noVNC handles by resizing its canvas -- visible as a brief reconnect.
#
log "starting x11vnc on port $VNC_PORT"
# shellcheck disable=SC2086
x11vnc -display "$DISP" -rfbport "$VNC_PORT" -forever -shared -quiet \
       $vnc_8to24 \
       -nowireframe -noscrollcopyrect \
       -xrandr resize \
       -nonap -wait "${QUAKE_VNC_WAIT:-5}" -defer "${QUAKE_VNC_DEFER:-5}" \
       ${QUAKE_VNC_ARGS:-} \
       $vnc_auth >"$STATE/x11vnc.log" 2>&1 &
VNC_PID=$!

##############################################################################
# Sound.
#
# The engine mixes everything itself -- effects in snd_mix.c, music in
# cd_stream.c -- and writes the finished stream to a FIFO. audiostream reads
# the FIFO on a real-time schedule and serves it on its own port, which the
# proxy below carries alongside the picture.
#
# audiostream has to be up first: it is the end that creates the FIFO.
##############################################################################
AUDIO_TO_BROWSER=0

if [ "${QUAKE_SOUND:-1}" != "1" ]; then
    log "sound: disabled"
elif [ -x "$AUDIOSTREAM_BIN" ]; then
    AUDIO_TO_BROWSER=1
    AUDIO_FIFO="$STATE/audio.fifo"
    rm -f "$AUDIO_FIFO"

    log "starting audiostream on port $AUDIO_PORT"
    "$AUDIOSTREAM_BIN" --fifo "$AUDIO_FIFO" --rate "$AUDIO_RATE" \
        --port "$AUDIO_PORT" ${QUAKE_AUDIOSTREAM_ARGS:-} \
        >"$STATE/audiostream.log" 2>&1 &
    AUDIO_PID=$!

    i=0
    while [ ! -p "$AUDIO_FIFO" ]; do
        i=$((i + 1))
        [ "$i" -gt 100 ] && die "audiostream failed to start; see $STATE/audiostream.log"
        kill -0 "$AUDIO_PID" 2>/dev/null || die "audiostream exited; see $STATE/audiostream.log"
        sleep 0.1
    done

    export QUAKE_AUDIO_FIFO="$AUDIO_FIFO"
    log "sound: to the browser, ${AUDIO_RATE} Hz stereo"
else
    log "sound: audiostream is missing, the game will be silent"
fi

##############################################################################
# One WebSocket port, two streams behind it: the screen and, when the sound is
# going to the browser, the sound. See docker/quake-wsproxy.py -- a connection
# that asks for neither gets the screen, so stock /vnc.html still works.
##############################################################################
{
    printf 'vnc: localhost:%s\n' "$VNC_PORT"
    [ "$AUDIO_TO_BROWSER" = "1" ] && printf 'audio: localhost:%s\n' "$AUDIO_PORT"
} > "$STATE/ws-targets"

##############################################################################
# Modern controls, on a fresh state volume only.
#
# Quake's own default.cfg is inside pak0.pak, so it cannot be changed and
# should not be: it is id's file. quake.rc execs it and then execs config.cfg,
# so config.cfg is where an override belongs -- and the engine rewrites
# config.cfg in full when it exits, so seeding it once is a default rather
# than a policy. Change anything in the game and your change is what persists.
#
# What the 1996 defaults actually are: the arrow keys move, `,` and `.`
# sidestep, `a` looks up, `d` swims up, and the mouse walks you forward unless
# you hold `\` to look with it. That was normal then. It reads as broken now.
##############################################################################
seed_config() {
    cfg="$BASEDIR/id1/config.cfg"

    [ -e "$cfg" ] && return 0
    [ "${QUAKE_MODERN_CONTROLS:-1}" = "1" ] || return 0

    cat > "$cfg" <<'CFGEOF'
// Written once, on a state volume with no config.cfg in it, and then owned by
// the engine: it rewrites this file whenever it exits, so anything changed in
// Options or at the console replaces what is here. Delete the file to get
// these back, or run the container with QUAKE_MODERN_CONTROLS=0 to start from
// id's 1996 defaults instead.
//
// quake.rc has already run id's default.cfg out of pak0.pak by this point, so
// these lines only need to cover what differs.

// WASD, and the mouse steering rather than walking. freelook is a cvar this
// port adds; +mlook still works and still wins while it is held.
freelook "1"
lookspring "0"
bind "w" "+forward"
bind "s" "+back"
bind "a" "+moveleft"
bind "d" "+moveright"

// The two keys WASD displaces. `a` was look up and `d` was swim up in 1996;
// jump already swims up, so these are the ones worth keeping somewhere.
bind "e" "+moveup"
bind "q" "+movedown"

// The wheel changes weapon, which needs the X11 driver to report it -- see
// the wheel handling in vid_x.c. impulse 12 has no default key at all in id's
// config.
bind "MWHEELUP" "impulse 10"
bind "MWHEELDOWN" "impulse 12"

// MOUSE2 is +forward in id's config, which is no use once the mouse looks.
bind "MOUSE2" "+attack"
CFGEOF

    log "seeded config.cfg with WASD and mouse look (QUAKE_MODERN_CONTROLS=0"
    log "  starts from id's 1996 defaults instead; whatever you change in the"
    log "  game is saved over this on exit)"
}

seed_config

#
# Hand the browser the engine's own key bindings.
#
# The controller in the page presses keys, so it has to press the keys *this*
# engine listens for -- and those live in config.cfg, which only this side can
# see. Without them a pad works perfectly in the menus, where the engine
# hardcodes the arrows and Return, and does nothing at all in a level as soon
# as anybody has been through Options -> Customize controls.
#
# Quake binds keys to commands, not the other way round, so the file is
# inverted on the way out: {"+attack": "CTRL"} is what the page wants, because
# the page knows which command a control means and needs the key that runs it.
#
# Read at startup, which is the right moment: the engine writes config.cfg when
# it exits, so what is on disk now is what it is about to load.
#
write_key_map() {
    cfg="$BASEDIR/id1/config.cfg"

    # The web root is read-only in the image, so the file lives in the state
    # directory and is reached through a symlink the Dockerfile made.
    out="$STATE/quake-keys.json"

    if [ ! -r "$cfg" ]; then
        # No config yet: a first run, so the engine will use its own defaults,
        # which are the page's defaults too. An empty object says "nothing to
        # override" rather than leaving a stale file from a previous container.
        printf '{}\n' >"$out" 2>/dev/null || true
        log "controller keys: no config.cfg yet, the page uses the stock binds"
        return
    fi

    #
    # bind "KEY" "COMMAND", which is exactly what Key_WriteBindings emits.
    #
    # First binding wins per command, except that a keyboard key beats a mouse
    # button: the stock config binds +attack to both CTRL and MOUSE1, and a key
    # is the thing the page can press most reliably. A mouse binding is still
    # passed through when it is the only one, and the page turns it back into a
    # button press.
    #
    if awk '
        function esc(v) { gsub(/\\/, "\\\\", v); gsub(/"/, "\\\"", v); return v }
        /^[ \t]*bind[ \t]+"/ {
            line = $0
            if (match(line, /"[^"]*"[ \t]+"[^"]*"/) == 0) next
            pair = substr(line, RSTART, RLENGTH)
            split(pair, q, "\"")
            key = q[2]; cmd = q[4]
            if (cmd == "") next
            ismouse = (key ~ /^MOUSE[0-9]+$/)
            if (!(cmd in seen) || (seen[cmd] == 1 && !ismouse)) {
                keys[cmd] = key
                seen[cmd] = ismouse ? 1 : 2
            }
        }
        END {
            printf "{"
            n = 0
            for (c in keys)
                printf "%s\"%s\":\"%s\"", (n++ ? "," : ""), esc(c), esc(keys[c])
            printf "}\n"
        }' "$cfg" >"$out.tmp" 2>/dev/null; then
        mv -f "$out.tmp" "$out" 2>/dev/null || rm -f "$out.tmp"
    else
        rm -f "$out.tmp"
        printf '{}\n' >"$out" 2>/dev/null || true
    fi

    #
    # Logged in full and sorted, and not as JSON.
    #
    # A report that the pad does nothing is answered by this line or by nothing
    # at all, so it is worth the width -- and sorted, because awk emits its keys
    # in no particular order and the two most likely to be asked about, +attack
    # and +speed, are the ones that would otherwise fall off the end.
    #
    log "controller keys: $(awk '
        /^[ \t]*bind[ \t]+"/ {
            if (match($0, /"[^"]*"[ \t]+"[^"]*"/) == 0) next
            split(substr($0, RSTART, RLENGTH), q, "\"")
            if (q[4] != "") printf "%s=%s\n", q[4], q[2]
        }' "$cfg" 2>/dev/null | sort | tr '\n' ' ')"
}

write_key_map

log "starting noVNC on port $WEB_PORT"
#
# websockify's own chatter goes to its log; the lines it prints about the
# picture going quiet, and the lines the page sends about its controller and
# its sound, are lifted into this one, because that is where anyone reading a
# stutter report is looking.
#
# Through a FIFO rather than a pipeline, because `cmd | while ... &` sets $! to
# the while loop, not to cmd. That left WEB_PID naming the reader: the watchdog
# below watched the wrong process, and the teardown killed the wrong one and
# left the proxy holding the port. Inside a container the orphan dies with the
# container and nobody notices; run the same script on a host and the next start
# fails with "Address already in use" and no explanation.
#
WSLOG="$STATE/websockify.fifo"
rm -f "$WSLOG"
mkfifo -m 600 "$WSLOG"

"$WSPROXY_BIN" --web="$NOVNC_ROOT" \
       --token-plugin=websockify.token_plugins.ReadOnlyTokenFile \
       --token-source="$STATE/ws-targets" \
       "$WEB_PORT" >"$WSLOG" 2>&1 &
WEB_PID=$!

while IFS= read -r wsline; do
    case "$wsline" in
        "picture gap"*) log "$wsline" ;;
        "controller:"*|"sound:"*) log "$wsline" ;;
        *) printf '%s\n' "$wsline" >>"$STATE/websockify.log" ;;
    esac
done < "$WSLOG" &
WEBLOG_PID=$!

log ""
log "  play at  http://localhost:$WEB_PORT/  (or /play.html)"
log ""
log "  plain noVNC (no mouse capture):  http://localhost:$WEB_PORT/vnc.html?autoconnect=1&resize=off"
log ""

##############################################################################
# Run the game.
##############################################################################
# The engine takes the window size from these; the Xvfb screen is already
# exactly that, so the window fills it and there is no desktop around the edge.
# Not folded into "$@" here: config_size runs again before each start below,
# because the engine rewrites config.cfg when it exits and the resolution it
# saved is the one the next run has to begin at.
MANAGE_SIZE=1
case " $* " in
    *" -width "*|*" -winsize "*) MANAGE_SIZE=0 ;;
esac

# Nothing else is on this display, so the engine may resize the screen itself
# rather than leaving a window in the corner of a framebuffer it cannot fill.
# Without this switch it only ever resizes its window, which is the right thing
# to do on somebody's desktop and the wrong thing here.
case " $* " in
    *" -resizescreen "*) ;;
    *) set -- -resizescreen "$@" ;;
esac

case " $* " in
    *" -basedir "*) ;;
    *) set -- -basedir "$BASEDIR" "$@" ;;
esac

# The mission packs want their own switch rather than -game: they set
# hipnotic/rogue inside the engine, which changes the status bar and the menu
# as well as the search path.
if [ -n "${QUAKE_GAME:-}" ]; then
    case " $* " in
        *" -game "*|*" -hipnotic "*|*" -rogue "*) ;;
        *)
            case "$QUAKE_GAME" in
                hipnotic) set -- -hipnotic "$@" ;;
                rogue)    set -- -rogue "$@" ;;
                id1|"")   ;;
                *)        set -- -game "$QUAKE_GAME" "$@" ;;
            esac
            ;;
    esac
fi

cd "$BASEDIR"

#
# Notice when one of the supporting processes dies, and say so.
#
# Only the engine is waited on, so if x11vnc exited -- a mistyped
# QUAKE_VNC_ARGS is enough -- the container would carry on looking healthy. The
# engine keeps drawing, the log keeps reporting frames, and the only sign is a
# browser saying "connection lost" with nothing in the container log to explain
# it. The reason is sitting in x11vnc's own log the whole time, which nobody
# knows to look at.
#
watch_helpers () {
    (
        while sleep 1; do
            for entry in "x11vnc:$VNC_PID:$STATE/x11vnc.log" \
                         "Xvfb:$XVFB_PID:$STATE/xvfb.log" \
                         "noVNC:$WEB_PID:$STATE/websockify.log" \
                         "audiostream:$AUDIO_PID:$STATE/audiostream.log"; do
                name=${entry%%:*}
                rest=${entry#*:}
                pid=${rest%%:*}
                logfile=${rest#*:}

                [ -n "$pid" ] || continue
                kill -0 "$pid" 2>/dev/null && continue

                log "$name has exited -- nothing works without it"

                # The reason, from its own log: the last line that looks like a
                # complaint, or the last few if none of them do. The last, not
                # the first: these logs outlive a restart, so the first match
                # can be minutes old and about something else entirely.
                if [ -n "$logfile" ] && [ -r "$logfile" ]; then
                    said=$(tail -n 40 "$logfile" 2>/dev/null \
                           | grep -iE "unrecognized|invalid|error|fatal|cannot|refused|no such" \
                           | tail -n 1)
                    [ -n "$said" ] || said=$(tail -n 2 "$logfile" 2>/dev/null)

                    printf '%s\n' "$said" | while IFS= read -r said_line; do
                        [ -n "$said_line" ] && log "  $said_line"
                    done

                    log "  the rest is in $logfile"
                fi

                # Bring the whole thing down rather than sit here looking well.
                kill "$GAME_PID" 2>/dev/null
                exit 0
            done
        done
    ) &
    HELPERWATCH_PID=$!
}

#
# Run the engine, and put it back on its feet if it falls over.
#
# A crash would otherwise take the container with it: the entrypoint exits,
# everything else is torn down, and the page is left saying "connection lost --
# reload to try again" where reloading cannot possibly work, because nothing is
# listening any more. Getting back in would mean a `docker restart` from
# somewhere that is not the phone in your hand.
#
# The display, the VNC server, the proxy and the sound all outlive the engine,
# so only the engine has to come back, and the browser reconnects to the same
# session on its own. The game itself starts again from the title screen; there
# is no crash recovery in 1996 code and this does not pretend otherwise.
#
# Quitting from the menu comes back the same way and for the same reason:
# picking it would otherwise leave a container running with nothing in it.
#
# Not restarted: a shutdown, or an error the engine reported itself -- those are
# decisions rather than falls. And three runs in a row that end within seconds
# of starting are a container that cannot run rather than a game that fell over;
# restarting that for ever would bury the reason under an endless loop.
#
crash_runs=0

while :; do
    # Re-read it every time round: the engine writes config.cfg on the way out,
    # so a resolution picked in the video menu is in there by now, and starting
    # at it is what keeps the window from being resized straight after it is
    # created. See the note on config_size above for what that costs.
    [ "$MANAGE_SIZE" = 1 ] && config_size

    started=$(date +%s 2>/dev/null || echo 0)

    # Run in the background and wait: a foreground child would block every trap
    # until it exited, so `docker stop` could not shut the stack down.
    if [ "$MANAGE_SIZE" = 1 ]; then
        log "running: xquake -width $WIDTH -height $HEIGHT $*"
        "$QUAKE_BIN" -width "$WIDTH" -height "$HEIGHT" "$@" &
    else
        log "running: xquake $*"
        "$QUAKE_BIN" "$@" &
    fi
    GAME_PID=$!

    watch_helpers

    status=0
    wait "$GAME_PID" || status=$?
    GAME_PID=""

    if [ -n "$HELPERWATCH_PID" ]; then
        kill "$HELPERWATCH_PID" 2>/dev/null || true
        HELPERWATCH_PID=""
    fi

    case "$status" in
        132) log "Quake hit an illegal instruction (SIGILL)" ;;
        134) log "Quake was aborted (SIGABRT)" ;;
        136) log "Quake hit an arithmetic error (SIGFPE)" ;;
        138) log "Quake died on a bad address (SIGBUS)" ;;
        139) log "Quake crashed (SIGSEGV, status 139)" ;;
        143) log "Quake was stopped (SIGTERM)" ;;
        130) log "Quake was interrupted (SIGINT)" ;;
        0)   log "Quake exited" ;;
        *)   log "Quake exited with status $status" ;;
    esac

    # Only a fall or a quit is worth getting up from.
    case "$status" in
        0)                   why="quit" ;;
        132|134|136|138|139) why="crash" ;;
        *)                   why="" ;;
    esac

    [ -n "$why" ] || break
    [ "$SHUTTING_DOWN" = "0" ] || break
    [ "$RESTART" = "1" ] || break

    now=$(date +%s 2>/dev/null || echo 0)

    if [ "$(( now - started ))" -lt 10 ]; then
        crash_runs=$(( crash_runs + 1 ))
    else
        crash_runs=0
    fi

    if [ "$crash_runs" -ge 3 ]; then
        log "that is three runs in a row that ended within seconds of starting,"
        log "so this is not something restarting will fix. Stopping, with the"
        log "reason above rather than buried under another hundred attempts."
        break
    fi

    if [ "$why" = "quit" ]; then
        log "starting the game again. Stopping the container is what stops the"
        log "container; quitting just brings you back to the title screen."
    else
        log "restarting the engine. The picture, the sound and this session all"
        log "stay up, so the browser comes back on its own -- but the game starts"
        log "again at the title screen, and only a savegame leads back to where"
        log "you were."
    fi

    log "Set QUAKE_RESTART=0 to have the container stop instead."

    sleep 1
done

exit "$status"

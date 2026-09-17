# Quake (the 1999 GPL release) in a container, playable in a browser.
#
# The software renderer only ever learned to talk to an 8-bit PseudoColor X
# visual, which no modern X server still offers. The container supplies one of
# its own with Xvfb, then exports it over VNC and noVNC, so the game is
# reachable from a browser on any host.
#
#   docker build -t quake .
#   docker run --rm -p 6080:6080 -v "$PWD/quakedata:/quakedata:ro" quake
#   # then open http://localhost:6080/play.html
#
# There is no game data in this image and there will not be: id's shareware
# licence allows passing the shareware release along whole, in its original
# compressed archive, with the agreement attached -- not a pak file lifted out
# of it. Mount your own; see README.md.
#
# See DOCKER.md for the full set of options.

##############################################################################
# Build stage: compile the engine against X11.
##############################################################################
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
        make \
        pkg-config \
        libx11-dev \
        libxext-dev \
        libxrandr-dev \
        libsndfile1-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY WinQuake/ ./WinQuake/
COPY audiostream/ ./audiostream/

# The engine, as portable C. The 1996 assembler is 32-bit only and quakedef.h
# already turns it off wherever __i386__ is not defined, which is here.
#
# SOUND=stream writes the mixed PCM to a pipe instead of to /dev/dsp, which no
# kernel has provided for twenty years. MUSIC=sndfile decodes track files,
# because Quake's music was CD audio and a container has no drive.
RUN make -C WinQuake -j"$(nproc)" SOUND=stream MUSIC=sndfile \
 && strip WinQuake/linux/xquake \
 && test -x WinQuake/linux/xquake

# Reads the engine's pipe and serves the sound to the browser, because the
# container has no sound card and cannot be given one: the only packaged
# PulseAudio brings systemd, GStreamer and a set of video codecs with it.
RUN make -C audiostream -j"$(nproc)" \
 && strip audiostream/linux/audiostream \
 && test -x audiostream/linux/audiostream

##############################################################################
# Runtime stage: the engine plus a private 8-bit X server and a web client.
##############################################################################
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        libx11-6 \
        libxext6 \
        libxrandr2 \
        libsndfile1 \
        xvfb \
        x11vnc \
        websockify \
        tini \
# noVNC is static HTML and JavaScript served to the browser, but the distro
# package depends on Node and net-tools for tooling this image never runs.
# Unpack just the files instead; websockify above is what actually serves them.
 && apt-get download novnc \
 && dpkg-deb -x novnc_*.deb / \
 && rm -f novnc_*.deb \
# Drop what nothing depends on any more, chiefly libxml2 and the ICU behind it.
# This has to happen before the forced removals below, which leave dpkg with
# unmet dependencies that apt then refuses to work around.
 && apt-get autoremove -y --purge \
# Xvfb is linked against libGL.so.1 so the dispatch library has to stay, but
# nothing in this image ever renders through GLX -- the whole point is that the
# renderer is in software -- and the Mesa driver behind it costs about 180 MB,
# most of it LLVM. Drop the driver, keep the dispatch.
 && dpkg --remove --force-depends \
        libglx-mesa0 mesa-libgallium libllvm20 libgl1-mesa-dri \
# websockify only uses numpy to unmask client-to-server WebSocket frames. Per
# RFC 6455 the server never masks what it sends, so for a VNC session that is
# just the keystrokes, not the video. It warns and carries on.
 && dpkg --remove --force-depends \
        python3-numpy liblapack3 libblas3 libgfortran5 \
 && rm -rf /var/lib/apt/lists/* /usr/share/doc/* /usr/share/man/*

COPY --from=build /src/WinQuake/linux/xquake /usr/local/games/xquake
COPY --from=build /src/audiostream/linux/audiostream /usr/local/games/audiostream
COPY docker/entrypoint.sh /usr/local/bin/quake-entrypoint

# websockify, taught to carry the sound alongside the picture on one port.
COPY docker/quake-wsproxy.py /usr/local/bin/quake-wsproxy

# Which build this is. Stamped into the client and printed at startup, so a
# report of "no sound" can be told apart from a browser quietly running the
# client from two releases ago -- the container's log looks identical either
# way, and everything that decides whether sound arrives happens in the page.
ARG QUAKE_VERSION=dev

# A client that captures the mouse. Stock noVNC reports absolute pointer
# positions, which a game cannot use: see the comment at the top of the file.
COPY docker/play.html /usr/share/novnc/play.html
COPY docker/quake-ring.js /usr/share/novnc/quake-ring.js
COPY docker/quake-audio.js /usr/share/novnc/quake-audio.js
# Controller support, which is entirely client side: the engine is sent the
# same keysyms and pointer reports either way. It sits beside noVNC's own
# modules because it imports the keysym table from them rather than hardcoding
# numbers.
COPY docker/quake-gamepad.js /usr/share/novnc/quake-gamepad.js
COPY docker/index.html /usr/share/novnc/index.html

# The engine's key bindings, for the controller in the page. The entrypoint
# writes them into the state directory, which it owns whatever PUID it runs as;
# this symlink is how they reach the browser without the web root having to be
# writable at runtime. websockify serves through it.
RUN ln -sfn /quake/state/quake-keys.json /usr/share/novnc/quake-keys.json

# Not sed: the stamp is whatever the build was told, and a branch name with a
# slash in it ends the s/// early. Python replaces the placeholder literally,
# and narrows the value to characters that cannot escape either the HTML text
# or the JavaScript string literal it lands in.
RUN python3 -c 'import os,re,pathlib; p=pathlib.Path("/usr/share/novnc/play.html"); v=re.sub(r"[^A-Za-z0-9._+-]","-",os.environ.get("QUAKE_VERSION") or "dev"); s=p.read_text().replace("__QUAKE_VERSION__",v); p.write_text(s); assert "__QUAKE_VERSION__" not in s, "version placeholder left in the client"'

# The directories come first so useradd does not warn about a home it cannot
# chown yet.
RUN chmod +x /usr/local/bin/quake-entrypoint /usr/local/bin/quake-wsproxy \
 && mkdir -p /quakedata /quake/state \
 && useradd --create-home --home-dir /quake/state --uid 1001 quaker \
 && chown -R quaker:quaker /quake

# config.cfg, savegames and screenshots are written next to the pak files, in
# com_gamedir. The entrypoint therefore builds a writable game directory inside
# the state volume and links the mounted, read-only pak files into it -- see
# docker/entrypoint.sh. Without that, mounting your game data read-only, which
# is what everybody does, silently loses every setting and every save.
ENV QUAKE_VERSION=${QUAKE_VERSION} \
    QUAKE_WIDTH=640 \
    QUAKE_HEIGHT=480 \
    QUAKE_DATADIR=/quakedata \
    QUAKE_STATE=/quake/state \
    QUAKE_VNC_PORT=5900 \
    QUAKE_AUDIO_PORT=5901 \
    QUAKE_WEB_PORT=6080 \
    QUAKE_DISPLAY=:99 \
    HOME=/quake/state

VOLUME ["/quake/state"]
EXPOSE 6080 5900

# No USER: the entrypoint starts as root only long enough to take ownership of
# the state directory as PUID:PGID, then drops to that user for the rest.
# Defaults to the quaker account created above. Pass --user to skip that and
# run as somebody specific from the outset.
WORKDIR /quake/state

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/quake-entrypoint"]

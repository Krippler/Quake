#!/usr/bin/env python3
#
# websockify, with two streams on the one port.
#
# The page needs both the VNC connection and, since 1.10.15, a second one
# carrying sound. Opening another port for it would mean another -p on every
# docker run and another mapping in the Unraid template, for what is one more
# socket to the same container.
#
# websockify can already route by a token in the query string, which is how
# multi-target deployments pick a backend. What it cannot do is carry on
# without one: a connection with no token is refused outright, and that would
# break /vnc.html, which is stock noVNC and knows nothing about tokens.
#
# So the token lookup is left alone and only the "no token" case is changed,
# to mean the screen -- the one thing every client that predates this wanted.
#
import os
import sys
import time
from urllib.parse import parse_qs, urlparse

from websockify.websocketproxy import ProxyRequestHandler, websockify_init

DEFAULT_TOKEN = 'vnc'

# Report a silence on the picture connection longer than this, in seconds.
GAP_REPORT_S = 0.2


#
# Make the browser check whether the page has changed.
#
# websockify serves the client with Python's SimpleHTTPRequestHandler, which
# sends Last-Modified and nothing else -- no Cache-Control, no ETag. With no
# freshness given, a browser is entitled to guess one and reuse what it has
# without asking, and it does: an image can be updated underneath a tab that
# goes on running the client it downloaded two releases ago. That failure is
# invisible from the container, whose log shows a perfectly healthy server
# talking to nobody.
#
# no-cache does not mean do not store it. It means ask first, which is a
# conditional request answered by a 304 in the ordinary case: the same traffic
# as before, minus the chance of serving a stale client for ever.
#
_end_headers = ProxyRequestHandler.end_headers


def end_headers(self):
    if not self.headers.get('Upgrade'):
        self.send_header('Cache-Control', 'no-cache')

    _end_headers(self)


ProxyRequestHandler.end_headers = end_headers


#
# Which side of the picture connection went quiet.
#
# The page can already tell that the picture stopped arriving while the sound,
# over the same link, kept coming -- so the link and the proxy are working and
# x11vnc alone went silent. What it cannot tell is why, because VNC is
# request-driven: the server sends an update only after the client asks for
# one. A silence means either the browser stopped asking, or it asked and
# x11vnc did not answer. Those are opposite faults and this is the only place
# that sees both halves of the conversation.
#
# The wrapper sits on the socket to x11vnc and times each direction. Marked
# lines so the entrypoint can lift them out of websockify's own chatter and
# into the container log, where anyone reading a stutter report will find them.
#
GAP_MARK = 'picture gap'


class _WatchedTarget(object):

    def __init__(self, sock):
        self._sock = sock
        self._last_in = time.time()     # x11vnc last said something
        self._last_ask = 0.0            # the browser last asked for a frame
        self._recent = []               # (when, how much), over the last second
        self._ask_carry = b''           # part of a client message, split across writes

    def __getattr__(self, name):
        return getattr(self._sock, name)

    def recv(self, *a, **k):
        data = self._sock.recv(*a, **k)

        if data:
            now = time.time()
            gap = now - self._last_in

            #
            # Only a gap that interrupts a stream that was flowing.
            #
            # VNC is request-driven and the server holds a request until there
            # is something to send, so a still screen produces silences of any
            # length that are not faults at all -- an idle game here reported
            # 200 ms gaps as "x11vnc answered 216 ms later", which is exactly
            # what it should have done. Only a stall in the middle of a moving
            # picture means anything.
            #
            # Measured in bytes rather than in reads: the first attempt counted
            # how many times the socket had been read, and an idle picture
            # still gets read several times a second, so it counted as busy and
            # the false reports carried on. A moving 320x200 picture is some
            # hundreds of kilobytes a second; an idle one is nearly nothing.
            #
            before = sum(n for t, n in self._recent if t > self._last_in - 1.0)

            if gap > GAP_REPORT_S and before > 50000:
                # Where in the silence the browser's request fell says whose
                # silence it was.
                if self._last_ask > self._last_in:
                    asked = (self._last_ask - self._last_in) * 1000
                    waited = (now - self._last_ask) * 1000
                    print('%s %.0f ms: the browser asked %.0f ms in, x11vnc '
                          'answered %.0f ms later'
                          % (GAP_MARK, gap * 1000, asked, waited),
                          file=sys.stderr, flush=True)
                else:
                    print('%s %.0f ms: the browser never asked during it'
                          % (GAP_MARK, gap * 1000),
                          file=sys.stderr, flush=True)

            self._last_in = now
            self._recent.append((now, len(data)))

            if len(self._recent) > 400:
                self._recent = [(t, n) for t, n in self._recent if t > now - 1.0]

        return data

    def send(self, data, *a, **k):
        n = self._sock.send(data, *a, **k)

        #
        # Only a request for a frame counts as asking for one.
        #
        # This used to take any byte from the browser, and during play most of
        # what the browser sends is pointer and key events -- a player
        # mouse-looking sends them continuously. One of those landing late in
        # a silence made the report read "the browser asked 422 ms in" when
        # the request for the frame had gone in at 5, which blames the browser
        # for a wait that was x11vnc's.
        #
        if n:
            if self._asked(data[:n]):
                self._last_ask = time.time()

        return n

    #
    # RFB client messages, by type and length. 3 is FramebufferUpdateRequest.
    # The stream is parsed rather than sniffed for a byte, because a pointer
    # event carries coordinates and any of those bytes can be a 3.
    #
    _MSG_LEN = {0: 20, 3: 10, 4: 8, 5: 6}

    def _asked(self, data):
        buf = self._ask_carry + data
        i = 0

        while i < len(buf):
            kind = buf[i]

            if kind == 3:
                self._ask_carry = b''
                return True

            if kind == 2:                       # SetEncodings, variable
                if i + 4 > len(buf):
                    break
                i += 4 + 4 * int.from_bytes(buf[i+2:i+4], 'big')
                continue

            if kind == 6:                       # ClientCutText, variable
                if i + 8 > len(buf):
                    break
                i += 8 + int.from_bytes(buf[i+4:i+8], 'big')
                continue

            size = self._MSG_LEN.get(kind)

            if size is None:
                # Lost the alignment. Say yes rather than start reporting
                # silences as x11vnc's fault on the strength of a bad parse.
                self._ask_carry = b''
                return True

            i += size

        # A message split across two writes: keep the tail for the next one.
        self._ask_carry = buf[i:] if i < len(buf) else b''
        if len(self._ask_carry) > 64:
            self._ask_carry = b''
        return False


#
# The controller's own log, from the page into the container log.
#
# Everything about a gamepad happens in the browser: which pad the Gamepad API
# admits to, what it calls its buttons, which of them the page saw move and
# which keysym it sent for each. None of that is visible from here, and a
# report of "the triggers do nothing" is unanswerable without it -- which is
# how five releases went out fixing things that were already right.
#
# So the page asks for this URL with its log in the query string and the lines
# are printed where the entrypoint can lift them into the container log. A URL
# is used rather than a POST because it works from a page served over plain
# http with no CORS preflight, and the body would have to be read here anyway.
#
# It cannot go through websockify's own request log: that is written by
# SimpleHTTPRequestHandler only under --verbose, and stdio through the
# entrypoint's pipe is block-buffered, so the lines would arrive in 4 KB
# clumps or not at all. This prints them itself, flushed, like the picture
# gap lines above.
#
PAD_PATH = '/quake-pad-log'

#
# Which run of the engine the page is looking at.
#
# The entrypoint bumps a counter in this file every time it starts the engine.
# The page reads it, and when it moves it opens a new VNC session, because
# x11vnc's 8-bit to 24-bit conversion does not survive the engine's window
# being destroyed and recreated: it goes on converting through the colormap
# that went with the old window, and the picture comes back in the wrong 256
# colours. Nothing in the VNC protocol reports that, so the page has to be
# told. See the note in DOCKER.md.
#
RUN_PATH = '/quake-run'
RUN_FILE = os.environ.get('QUAKE_RUN_ID_FILE', '')
PAD_MAX_LINES = 40
PAD_MAX_CHARS = 400

_do_GET = ProxyRequestHandler.do_GET


def do_GET(self):
    split = urlparse(self.path)

    if split.path == RUN_PATH:
        try:
            with open(RUN_FILE) as fh:
                body = fh.read().strip().encode('ascii', 'replace')[:32]
        except (OSError, ValueError):
            # No file yet, or no path configured. An empty answer means "do
            # not act on this", which is what the page does with it.
            body = b''

        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return

    if split.path != PAD_PATH:
        return _do_GET(self)

    query = parse_qs(split.query)
    lines = (query.get('l') or [''])[0]

    # Which part of the page is talking. Only a short word from a fixed set, so
    # a page cannot invent a prefix that reads like something else in the log.
    tag = (query.get('t') or ['controller'])[0]

    if tag not in ('controller', 'sound'):
        tag = 'controller'

    for line in lines.split('|')[:PAD_MAX_LINES]:
        # Whitespace-collapsed, which also drops anything that could forge a
        # second line of container log, and cut to a readable length. 200 was
        # too short: the keymap line is the longest and the most useful, and it
        # arrived cut off in the middle of the last setting's name.
        line = ' '.join(line.split())[:PAD_MAX_CHARS]

        if line:
            print('%s: %s' % (tag, line), file=sys.stderr, flush=True)

    # Nothing to send back. The page does not read the answer; it only needs
    # the request to have arrived.
    self.send_response(204)
    self.send_header('Content-Length', '0')
    self.end_headers()


ProxyRequestHandler.do_GET = do_GET


_do_proxy = ProxyRequestHandler.do_proxy


def do_proxy(self, target):
    # Only the picture; the sound is a plain one-way trickle with nothing to
    # ask for.
    if getattr(self, '_quake_is_screen', False):
        target = _WatchedTarget(target)

    return _do_proxy(self, target)


ProxyRequestHandler.do_proxy = do_proxy


def get_target(self, target_plugin):
    query = parse_qs(urlparse(self.path).query)
    token = (query.get('token') or [''])[0].strip() or DEFAULT_TOKEN

    pair = target_plugin.lookup(token)

    if pair is None:
        raise self.server.EClose("no stream called %r" % token)

    # Remembered for do_proxy above, which only watches the picture.
    self._quake_is_screen = (token == DEFAULT_TOKEN)

    return pair


ProxyRequestHandler.get_target = get_target

sys.exit(websockify_init())

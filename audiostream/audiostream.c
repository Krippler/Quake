/*
Copyright (C) 2026 the Quake container port contributors.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

//
// audiostream -- carries the engine's sound to the browser.
//
// The container has no sound card and cannot be given one: the only packaged
// PulseAudio brings systemd, GStreamer and a set of video codecs with it, to
// do a job that is, here, moving bytes from a pipe to a socket. And VNC
// carries a picture and nothing else, so the sound needs its own way out.
//
// This is that way out. The engine mixes everything itself -- sound effects
// in snd_mix.c, music in cd_stream.c, both into the one buffer -- and writes
// the finished 16-bit stereo stream to a FIFO. This reads the FIFO on a
// real-time schedule and hands what it finds to whoever is connected.
//
// Three things it has to get right, all of them learned the hard way:
//
//   A short write is not a lost period.  write() takes as much as it feels
//   like, which need not be a whole 4-byte frame. Dropping the remainder
//   leaves the listener holding half a frame, and every sample after that is
//   assembled from the wrong pair of bytes -- not a glitch but full-scale
//   noise, and nothing downstream ever notices it has slipped. So what the
//   socket would not take is kept, and what is dropped is dropped in whole
//   frames.
//
//   A pipe that has gone quiet is silence, not a stall.  The engine misses
//   mixing deadlines while a map loads. Waiting for the bytes would drift the
//   listener's clock by however long the load took; padding the gap with
//   silence keeps it in step.
//
//   The FIFO is opened read-write.  Opened read-only it would report end of
//   file every time the engine restarts, and block on open until the engine
//   started at all. Holding a write end of our own means the open returns
//   immediately and the pipe simply goes quiet between writers.
//

#define _GNU_SOURCE	// F_SETPIPE_SZ, clock_nanosleep

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// 16-bit stereo. Fixed on both sides: snd_stream.c produces it and the page's
// AudioWorklet consumes it.
#define CHANNELS		2
#define BYTES_PER_FRAME	(CHANNELS * 2)

// Frames per period. At 22050 Hz this is 12 ms, which is the granularity of
// everything downstream: it bounds the added latency and the wakeup rate.
#define PERIOD			256

// How many listeners at once. More than one is only ever a second browser
// tab, but refusing outright would leave a reloading page locked out until
// the old socket timed out.
#define MAXCLIENTS		4

// How much a listener may fall behind before frames are dropped. Eight
// periods is about 93 ms, which is long enough to ride out a scheduling
// hiccup and short enough that the sound does not visibly lag the picture.
#define BACKLOG_PERIODS	8

#define NSEC_PER_SEC	1000000000L


typedef struct
{
	int				fd;
	unsigned char	pend[PERIOD * BYTES_PER_FRAME * BACKLOG_PERIODS];
	size_t			pendlen;
} client_t;


static int	out_rate = 22050;
static int	verbose = 0;


static void die (const char *what)
{
	fprintf (stderr, "audiostream: %s: %s\n", what, strerror (errno));
	exit (1);
}


//
// Adds bytes to what a listener is owed, dropping the oldest whole frames if
// that is the only way to make room.
//
static void Owe (client_t *c, const unsigned char *buf, size_t len)
{
	if (len > sizeof(c->pend))
		return;					// cannot happen: a period is far smaller

	if (c->pendlen + len > sizeof(c->pend))
	{
		size_t	drop = c->pendlen + len - sizeof(c->pend);

		drop = (drop + BYTES_PER_FRAME - 1) & ~(size_t)(BYTES_PER_FRAME - 1);
		if (drop > c->pendlen)
			drop = c->pendlen & ~(size_t)(BYTES_PER_FRAME - 1);

		memmove (c->pend, c->pend + drop, c->pendlen - drop);
		c->pendlen -= drop;
	}

	memcpy (c->pend + c->pendlen, buf, len);
	c->pendlen += len;
}


//
// Hands over as much as the socket will take. Returns 0 if the listener has
// gone away.
//
static int Flush (client_t *c)
{
	while (c->pendlen)
	{
		ssize_t	w = write (c->fd, c->pend, c->pendlen);

		if (w > 0)
		{
			c->pendlen -= (size_t)w;
			if (c->pendlen)
				memmove (c->pend, c->pend + w, c->pendlen);
			continue;
		}

		if (w < 0 && errno == EINTR)
			continue;

		// Full socket. What is left stays owed, still frame-aligned.
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 1;

		return 0;
	}

	return 1;
}


typedef struct
{
	const char		*path;
	int				fd;
	unsigned char	partial[PERIOD * BYTES_PER_FRAME * 4];
	size_t			have;
	long			underruns;
	long			periods;
} source_t;


static void OpenSource (source_t *s, const char *path, int pipesize)
{
	s->path = path;
	s->have = 0;
	s->underruns = 0;
	s->periods = 0;

	unlink (path);

	if (mkfifo (path, 0600) < 0)
		die (path);

	s->fd = open (path, O_RDWR | O_NONBLOCK);
	if (s->fd < 0)
		die (path);

	// Latency is whatever the pipe holds, so keep it small. The kernel rounds
	// up to a page and will not go below one.
	if (fcntl (s->fd, F_SETPIPE_SZ, pipesize) < 0 && verbose)
		fprintf (stderr, "audiostream: %s: cannot set pipe size: %s\n",
				 path, strerror (errno));
}


//
// Fills buf with exactly want bytes, padding with silence if the engine has
// not kept up. Anything read beyond want stays for the next period.
//
static void ReadSource (source_t *s, unsigned char *buf, size_t want)
{
	while (s->have < want)
	{
		ssize_t	n = read (s->fd, s->partial + s->have, want - s->have);

		if (n > 0)
		{
			s->have += (size_t)n;
			continue;
		}

		if (n < 0 && errno == EINTR)
			continue;

		break;					// EAGAIN: nothing more this period
	}

	s->periods++;

	if (s->have < want)
	{
		// Pad to a whole number of frames of real audio, then silence. Cutting
		// mid-frame would swap the channels for the rest of the session.
		size_t	whole = s->have & ~(size_t)(BYTES_PER_FRAME - 1);

		memcpy (buf, s->partial, whole);
		memset (buf + whole, 0, want - whole);
		s->have -= whole;
		if (s->have)
			memmove (s->partial, s->partial + whole, s->have);
		s->underruns++;
		return;
	}

	memcpy (buf, s->partial, want);
	s->have -= want;
	if (s->have)
		memmove (s->partial, s->partial + want, s->have);
}


static int Listen (int port)
{
	struct sockaddr_in	addr;
	int					fd, one = 1;

	fd = socket (AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		die ("socket");

	setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset (&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl (INADDR_ANY);
	addr.sin_port = htons ((unsigned short)port);

	if (bind (fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die ("bind");
	if (listen (fd, MAXCLIENTS) < 0)
		die ("listen");
	if (fcntl (fd, F_SETFL, O_NONBLOCK) < 0)
		die ("listen socket");

	return fd;
}


static void AddNsec (struct timespec *t, long nsec)
{
	t->tv_nsec += nsec;
	while (t->tv_nsec >= NSEC_PER_SEC)
	{
		t->tv_nsec -= NSEC_PER_SEC;
		t->tv_sec++;
	}
}


int main (int argc, char **argv)
{
	const char		*fifo = "/tmp/quake-audio";
	int				port = 5901;
	int				pipesize = 8192;
	int				lfd, i;
	source_t		src;
	client_t		clients[MAXCLIENTS];
	int				nclients = 0;
	struct timespec	next;
	unsigned char	period[PERIOD * BYTES_PER_FRAME];
	long			reports = 0;

	for (i = 1; i < argc; i++)
	{
		if (!strcmp (argv[i], "--fifo") && i+1 < argc)
			fifo = argv[++i];
		else if (!strcmp (argv[i], "--port") && i+1 < argc)
			port = atoi (argv[++i]);
		else if (!strcmp (argv[i], "--rate") && i+1 < argc)
			out_rate = atoi (argv[++i]);
		else if (!strcmp (argv[i], "--pipe-size") && i+1 < argc)
			pipesize = atoi (argv[++i]);
		else if (!strcmp (argv[i], "--verbose"))
			verbose = 1;
		else
		{
			fprintf (stderr,
					 "usage: %s [--fifo PATH] [--port N] [--rate N]\n"
					 "          [--pipe-size N] [--verbose]\n", argv[0]);
			return 2;
		}
	}

	if (out_rate <= 0)
	{
		fprintf (stderr, "audiostream: --rate must be positive\n");
		return 2;
	}

	// A listener going away mid-write is normal and is handled by the return
	// value of write(). It must not be handled by killing the process.
	signal (SIGPIPE, SIG_IGN);

	OpenSource (&src, fifo, pipesize);
	lfd = Listen (port);

	fprintf (stderr, "audiostream: %d Hz, 16 bit, stereo; %s -> port %d\n",
			 out_rate, fifo, port);
	fflush (stderr);

	clock_gettime (CLOCK_MONOTONIC, &next);

	for (;;)
	{
		// One period every PERIOD/rate seconds, measured from a fixed origin
		// rather than from "now plus a period". Sleeping for a period at a
		// time accumulates every overshoot; this one absorbs them.
		AddNsec (&next, (long)((double)PERIOD * NSEC_PER_SEC / out_rate));
		while (clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL)
			   == EINTR)
			;

		// Read whether or not anybody is listening: this read is what drains
		// the pipe, and a pipe nobody drains backs up into the engine.
		ReadSource (&src, period, sizeof(period));

		for (;;)
		{
			int	c = accept (lfd, NULL, NULL);

			if (c < 0)
				break;

			if (nclients >= MAXCLIENTS)
			{
				close (c);
				continue;
			}

			fcntl (c, F_SETFL, O_NONBLOCK);
			// Sound is a steady trickle of small writes; waiting to fill a
			// segment would add up to a period of delay for nothing.
			i = 1;
			setsockopt (c, IPPROTO_TCP, TCP_NODELAY, &i, sizeof(i));

			clients[nclients].fd = c;
			clients[nclients].pendlen = 0;

			//
			// Sixteen bytes of preamble, then nothing but frames.
			//
			// The page has to know the rate before it can build its resampler,
			// and it cannot be told any other way: the sound arrives on a
			// WebSocket that carries bytes and nothing else. Baking 22050 into
			// the page instead would mean a page and a container that disagree
			// producing sound at the wrong pitch, silently, with nothing in
			// either log -- and the page is the half that gets cached.
			//
			// The magic is eight bytes because the page reads eight. Nine
			// would never match, and the failure is "what arrived on the audio
			// connection was not sound", which points at the wrong end.
			//
			{
				unsigned char	hdr[16];

				memcpy (hdr, "QUAKAUD1", 8);
				hdr[8]  = (unsigned char)(out_rate & 0xff);
				hdr[9]  = (unsigned char)((out_rate >> 8) & 0xff);
				hdr[10] = (unsigned char)((out_rate >> 16) & 0xff);
				hdr[11] = (unsigned char)((out_rate >> 24) & 0xff);
				hdr[12] = CHANNELS;
				hdr[13] = 0;
				hdr[14] = 16;			// bits per sample
				hdr[15] = 0;

				Owe (&clients[nclients], hdr, sizeof(hdr));

				if (!Flush (&clients[nclients]))
				{
					close (c);
					continue;
				}
			}

			nclients++;

			if (verbose)
				fprintf (stderr, "audiostream: listener connected (%d)\n",
						 nclients);
		}

		for (i = 0; i < nclients; )
		{
			Owe (&clients[i], period, sizeof(period));

			if (!Flush (&clients[i]))
			{
				close (clients[i].fd);
				clients[i] = clients[nclients - 1];
				nclients--;
				if (verbose)
					fprintf (stderr, "audiostream: listener gone (%d)\n",
							 nclients);
				continue;
			}
			i++;
		}

		if (verbose && ++reports >= (long)out_rate / PERIOD * 5)
		{
			fprintf (stderr,
					 "audiostream: %ld periods, %ld underruns, %d listening\n",
					 src.periods, src.underruns, nclients);
			reports = 0;
		}
	}
}

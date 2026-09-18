/*
Copyright (C) 1996-1997 Id Software, Inc.

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
// snd_stream.c -- a DMA backend that has no DMA, and no sound card either.
//
// snd_linux.c, the one the 1996 sources shipped, mmaps /dev/dsp and asks the
// driver where the playback pointer is. No current kernel provides /dev/dsp:
// OSS was replaced by ALSA before this source was released, and the OSS
// emulation that stood in for it afterwards is gone too -- PulseAudio's own
// shim was removed upstream in PulseAudio 16.
//
// A container is worse off still. It has no sound card to expose, and the
// only packaged PulseAudio brings systemd, GStreamer and a set of video
// codecs with it, to do a job that is here "put these bytes on a socket".
//
// So the engine keeps its own clock instead of asking a driver for one, and
// hands the mixed result to a pipe. The whole of the DMA interface is three
// questions -- how big is the buffer, where is the playback pointer, and here
// is some more audio -- and none of them need hardware to answer:
//
//   SNDDMA_Init       allocate a ring and start a monotonic clock
//   SNDDMA_GetDMAPos  where the pointer would be if the clock were the card
//   SNDDMA_Submit     hand the pipe whatever has come due since last time
//
// What reads the other end of the pipe is audiostream, which fans it out to
// the browser. See audiostream/audiostream.c.

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "quakedef.h"

// 16-bit stereo, which is what the browser wants and what the mixer is
// happiest producing. The 1996 default was 11025 Hz mono because that was
// what a Sound Blaster could keep up with.
#define STREAM_RATE			22050
#define STREAM_CHANNELS		2
#define STREAM_BITS			16
#define STREAM_BYTES_PER_FRAME	(STREAM_CHANNELS * (STREAM_BITS / 8))

// The ring, in frames. Must be a power of two: S_TransferPaintBuffer indexes
// it with `paintedtime * channels & (shm->samples - 1)`, which is only the
// same thing as a modulo when the size is a power of two. 16384 frames is
// 0.74 seconds, which is far more than anything is ever behind by, and it
// costs 64 KB of the hunk.
#define STREAM_FRAMES		16384

// How far behind the clock the pipe is allowed to get before the backlog is
// dropped rather than sent. Anything older than this is stale by definition:
// the ring has been written over since.
#define MAX_CATCHUP_FRAMES	(STREAM_RATE / 4)

static int		snd_fd = -1;
static qboolean	snd_inited = false;

// Where the clock started, and how many frames have gone down the pipe. The
// pipe is the only thing keeping time here, so these two are the playback
// position: everything else is derived from them.
static double			snd_starttime;
static unsigned int		snd_sent;

// Subtracted from the elapsed count so that what SNDDMA_GetSamples hands out
// stays inside an int. Moved only by SNDDMA_RebaseClock, which moves snd_sent
// with it so that nothing else notices.
static unsigned int		snd_base;

// Frames the pipe would not take last time, kept so that a partial write is
// resumed rather than restarted. A short write that is not a whole number of
// frames is worse than a gap: the listener assembles every sample after it
// from the wrong pair of bytes, which is full-scale noise rather than a click,
// and nothing downstream ever notices it has slipped.
static unsigned char	snd_pending[4096 * STREAM_BYTES_PER_FRAME];
static int				snd_pendinglen;


/*
==================
S_StreamPath

Where to write the audio. The entrypoint makes the FIFO and starts the reader
before the engine, and passes the path in the environment; -audiofifo is for
running it by hand.
==================
*/
static const char *S_StreamPath (void)
{
	int		i;

	i = COM_CheckParm ("-audiofifo");
	if (i && i < com_argc - 1)
		return com_argv[i+1];

	return getenv ("QUAKE_AUDIO_FIFO");
}


/*
==================
SNDDMA_Init
==================
*/
qboolean SNDDMA_Init (void)
{
	const char	*path;
	int			buffersize;

	path = S_StreamPath ();
	if (!path || !*path)
	{
		Con_Printf ("Sound: no audio pipe (set QUAKE_AUDIO_FIFO); running silent\n");
		return false;
	}

// O_RDWR, not O_WRONLY. Opening the write end of a FIFO blocks until a reader
// turns up, and with O_NONBLOCK it fails outright with ENXIO instead -- so
// starting the engine a moment before audiostream would mean no sound for the
// rest of the session. Holding a read end of our own means the open returns
// immediately and stays valid whether or not anybody is listening, and it is
// also what stops a SIGPIPE killing the engine when the reader restarts.
	snd_fd = open (path, O_RDWR | O_NONBLOCK);
	if (snd_fd == -1)
	{
		Con_Printf ("Sound: could not open %s (%s); running silent\n",
					path, strerror(errno));
		return false;
	}

	shm = &sn;
	shm->splitbuffer = 0;
	shm->channels = STREAM_CHANNELS;
	shm->samplebits = STREAM_BITS;
	shm->speed = STREAM_RATE;
	shm->samples = STREAM_FRAMES * STREAM_CHANNELS;		// in mono samples
	shm->samplepos = 0;
	shm->soundalive = true;
	shm->gamealive = true;

// One frame. The engine mixes on its own schedule and this backend does not
// care how much arrives at once, so there is no minimum worth enforcing --
// and a large one only delays a shot reaching the pipe.
	shm->submission_chunk = 1;

	buffersize = STREAM_FRAMES * STREAM_BYTES_PER_FRAME;
	shm->buffer = Hunk_AllocName (buffersize, "shmbuf");
	if (!shm->buffer)
	{
		Con_Printf ("Sound: no room for a %d byte mixing buffer\n", buffersize);
		close (snd_fd);
		snd_fd = -1;
		return false;
	}
	memset (shm->buffer, 0, buffersize);

	snd_starttime = Sys_FloatTime ();
	snd_sent = 0;
	snd_base = 0;
	snd_pendinglen = 0;
	snd_inited = true;

	Con_Printf ("Sound: %d Hz, %d bit, %s -> %s\n", STREAM_RATE, STREAM_BITS,
				STREAM_CHANNELS == 2 ? "stereo" : "mono", path);

	return true;
}


/*
==================
SNDDMA_ElapsedFrames

Where the playback pointer would be. A sound card counts samples it has
consumed; there is no card, so this counts the samples that should have been
consumed by now. Sys_FloatTime is monotonic, which matters more here than
anywhere else in the engine -- a clock that steps backwards reads as the
playback pointer going backwards, which GetSoundtime interprets as the buffer
having wrapped, and the mixer then races a whole buffer ahead.
==================
*/
static unsigned int SNDDMA_ElapsedFrames (void)
{
	double	elapsed;

	elapsed = (Sys_FloatTime () - snd_starttime) * (double)STREAM_RATE;
	if (elapsed < 0)
		elapsed = 0;

	return (unsigned int)elapsed;
}


/*
==================
SNDDMA_GetDMAPos
==================
*/
int SNDDMA_GetDMAPos (void)
{
	if (!snd_inited)
		return 0;

	shm->samplepos = ((SNDDMA_ElapsedFrames () - snd_base) * STREAM_CHANNELS)
					 % shm->samples;

	return shm->samplepos;
}


/*
==================
SNDDMA_GetSamples

The playback position as a running count of frames, rather than as a position
inside the ring.

GetSoundtime otherwise reconstructs that count from the position by watching
it wrap, and id's own comment there says what is wrong with that: "it is
possible to miscount buffers if it has wrapped twice between calls to
S_Update. Oh well." The ring is 0.74 seconds long, so a frame that takes
longer than that loses a whole ring -- and it is never found again, because
nothing recounts.

In 1996 a frame that long meant the machine had stopped. Here a level load is
that long, and a browser on a busy machine is worse: this port logs picture
gaps of one to eighteen seconds as an ordinary occurrence.

What made it more than a cosmetic slip is what SNDDMA_Submit does with it.
Submit will not send past paintedtime, and paintedtime follows soundtime; so
once soundtime is a ring or more behind the clock, every frame is padded with
silence rather than carried from the mixer -- for the rest of the run, at
exactly the right rate. The listener's buffer never underruns and nothing
anywhere reports a fault. The game goes quiet after the first level load and
stays quiet. Measured on a demo loop: a 3.5 second load left paintedtime
63331 frames behind the clock, and it was still exactly 63331 behind twenty
seconds later.

There is no sound card here and no wrapping to reconstruct. This backend's
clock is the playback position, so hand the count over and let GetSoundtime
stop guessing.
==================
*/
int SNDDMA_GetSamples (void)
{
	if (!snd_inited)
		return 0;

	return (int)(SNDDMA_ElapsedFrames () - snd_base);
}


/*
==================
SNDDMA_RebaseClock

Moves the origin forward, so the running count above stays inside an int.

soundtime and paintedtime are both ints, and 2^31 frames is twenty-seven
hours -- which a container on somebody's home server passes without anyone
thinking about it. snd_sent moves by the same amount, so SNDDMA_Submit's
arithmetic is untouched and no audio is lost across the move.
==================
*/
void SNDDMA_RebaseClock (int frames)
{
	snd_base += (unsigned int)frames;
	snd_sent -= (unsigned int)frames;
}


/*
==================
S_StreamFlushPending

Hands the pipe as much of the backlog as it will take, keeping whatever is
left whole-frame aligned.
==================
*/
static void S_StreamFlushPending (void)
{
	ssize_t	written;

	while (snd_pendinglen > 0)
	{
		written = write (snd_fd, snd_pending, snd_pendinglen);
		if (written <= 0)
		{
			if (written < 0 && errno == EINTR)
				continue;
			return;				// EAGAIN: the reader is behind, try next frame
		}

		snd_pendinglen -= (int)written;
		if (snd_pendinglen > 0)
			memmove (snd_pending, snd_pending + written, snd_pendinglen);
	}
}


/*
==================
S_StreamQueue

Adds frames to the backlog. Drops from the front when it will not fit, in
whole frames, because a gap is a click and a half-frame is noise for ever.
==================
*/
static void S_StreamQueue (const unsigned char *data, int len)
{
	int		room;

	S_StreamFlushPending ();

	room = (int)sizeof(snd_pending) - snd_pendinglen;
	if (len > room)
	{
		int		drop = len - room;

		drop = (drop + STREAM_BYTES_PER_FRAME - 1) / STREAM_BYTES_PER_FRAME;
		drop *= STREAM_BYTES_PER_FRAME;
		if (drop >= snd_pendinglen)
			snd_pendinglen = 0;
		else
		{
			snd_pendinglen -= drop;
			memmove (snd_pending, snd_pending + drop, snd_pendinglen);
		}
		room = (int)sizeof(snd_pending) - snd_pendinglen;
		if (len > room)
		{
		// More than the whole backlog in one go, which means the reader has
		// been gone for a while. Keep the newest.
			data += len - room;
			len = room;
		}
	}

	memcpy (snd_pending + snd_pendinglen, data, len);
	snd_pendinglen += len;

	S_StreamFlushPending ();
}


/*
==================
SNDDMA_Submit

Called once a frame, after the mixer has filled the ring up to paintedtime.
Everything between what has already gone out and where the clock says the
playback pointer is, is now due.
==================
*/
void SNDDMA_Submit (void)
{
	unsigned int	due, upto;
	unsigned int	first, count;

	if (!snd_inited)
		return;

	S_StreamFlushPending ();

	due = SNDDMA_ElapsedFrames ();

// Never send past what the mixer has actually written. paintedtime is where
// it has got to; beyond that the ring still holds whatever was there a buffer
// ago, and sending it would play the last second of the game again -- which
// is what happens while a map loads and the engine misses a few hundred
// frames of mixing. Silence is the right thing to send for that gap.
	upto = due;
	if ((int)(upto - (unsigned int)paintedtime) > 0)
		upto = (unsigned int)paintedtime;

	if ((int)(upto - snd_sent) < 0)
		upto = snd_sent;				// the mixer has been reset behind us

// If we are further behind than the ring is long, the frames in between have
// already been written over. Skip them rather than sending the wrong audio.
	if (upto - snd_sent > MAX_CATCHUP_FRAMES)
		snd_sent = upto - MAX_CATCHUP_FRAMES;

	while (snd_sent != upto)
	{
		first = snd_sent % STREAM_FRAMES;
		count = upto - snd_sent;
		if (count > STREAM_FRAMES - first)
			count = STREAM_FRAMES - first;		// stop at the wrap

		S_StreamQueue ((unsigned char *)shm->buffer
						   + first * STREAM_BYTES_PER_FRAME,
					   count * STREAM_BYTES_PER_FRAME);
		snd_sent += count;
	}

// Whatever the mixer did not get to, send as silence, so the listener's own
// clock keeps step with ours instead of drifting by however long the stall was.
	while (snd_sent != due)
	{
		static const unsigned char	quiet[512 * STREAM_BYTES_PER_FRAME] = { 0 };

		count = due - snd_sent;
		if (count > 512)
			count = 512;

		S_StreamQueue (quiet, count * STREAM_BYTES_PER_FRAME);
		snd_sent += count;
	}
}


/*
==================
SNDDMA_Shutdown
==================
*/
void SNDDMA_Shutdown (void)
{
	if (!snd_inited)
		return;

	snd_inited = false;
	if (snd_fd != -1)
	{
		close (snd_fd);
		snd_fd = -1;
	}
}

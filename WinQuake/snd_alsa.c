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
// snd_alsa.c -- sound through ALSA, for a Linux desktop.
//
// The container sends its sound down a pipe (snd_stream.c) and the 1996 code
// wanted /dev/dsp (snd_linux.c), which no current desktop has. What every
// current desktop does have is ALSA's "default" device: on a machine running
// PipeWire or PulseAudio it is a plugin that hands the audio to them, and on
// one running neither it is the sound card. So one backend covers all three.
//
// The engine's mixer thinks it is writing into a sound card's DMA buffer and
// asks where the card has got to. Here the "DMA buffer" is a ring of our own,
// and ALSA is fed from it once a frame, as much as it has room for:
//
//   SNDDMA_Init        open the device, allocate the ring
//   SNDDMA_GetSamples  frames played: frames written, less those still queued
//   SNDDMA_Submit      write from the ring up to what the mixer has painted
//
// Nothing blocks. The device is opened non-blocking, and a write it has no
// room for waits in the ring until the next frame.

#include <alsa/asoundlib.h>

#include "quakedef.h"

// 48 kHz because that is what a desktop's sound server runs at, so nothing
// between here and the speakers has to resample. 16-bit stereo, as the mixer
// prefers.
#define ALSA_RATE			48000
#define ALSA_CHANNELS		2
#define ALSA_BYTES_PER_FRAME	(ALSA_CHANNELS * 2)

// The ring, in frames. A power of two, because S_TransferPaintBuffer indexes it
// with a mask. 32768 frames is 0.68 seconds, several times what the mixer ever
// paints ahead.
#define ALSA_FRAMES			32768

// How much the device itself holds. Everything is written from the frame
// loop, so this has to cover the longest ordinary frame or the card runs dry
// in the middle of one; a tenth of a second does, and it is also the mixer's
// own default lead (_snd_mixahead).
#define ALSA_LATENCY_US		100000

static snd_pcm_t			*pcm;
static qboolean				snd_inited;
static snd_pcm_uframes_t	snd_bufsize;	// the device's buffer, in frames

// Frames handed to ALSA, as a running count. The mixer's clock is this less
// whatever the device has not played yet; snd_played keeps it from going
// backwards when a device reports its queue unevenly.
static unsigned int			snd_written;
static unsigned int			snd_played;


/*
==================
S_AlsaDevice

"default" unless told otherwise: -alsadevice on the command line, or
QUAKE_ALSA_DEVICE in the environment, for a machine with more than one card.
==================
*/
static const char *S_AlsaDevice (void)
{
	int			i;
	const char	*env;

	i = COM_CheckParm ("-alsadevice");
	if (i && i < com_argc - 1)
		return com_argv[i+1];

	env = getenv ("QUAKE_ALSA_DEVICE");
	if (env && *env)
		return env;

	return "default";
}


/*
==================
SNDDMA_Init
==================
*/
qboolean SNDDMA_Init (void)
{
	const char			*device;
	snd_pcm_uframes_t	period;
	int					err, buffersize;

	device = S_AlsaDevice ();

	err = snd_pcm_open (&pcm, device, SND_PCM_STREAM_PLAYBACK,
						SND_PCM_NONBLOCK);
	if (err < 0)
	{
		Con_Printf ("Sound: could not open ALSA device \"%s\" (%s); "
					"running silent\n", device, snd_strerror (err));
		pcm = NULL;
		return false;
	}

// soft_resample on: a card that cannot do 48 kHz gets it converted rather than
// refusing to open.
	err = snd_pcm_set_params (pcm, SND_PCM_FORMAT_S16,
							  SND_PCM_ACCESS_RW_INTERLEAVED, ALSA_CHANNELS,
							  ALSA_RATE, 1, ALSA_LATENCY_US);
	if (err < 0)
	{
		Con_Printf ("Sound: ALSA device \"%s\" will not take 16-bit stereo "
					"at %d Hz (%s); running silent\n", device, ALSA_RATE,
					snd_strerror (err));
		snd_pcm_close (pcm);
		pcm = NULL;
		return false;
	}

	snd_pcm_get_params (pcm, &snd_bufsize, &period);

	shm = &sn;
	shm->splitbuffer = 0;
	shm->channels = ALSA_CHANNELS;
	shm->samplebits = 16;
	shm->speed = ALSA_RATE;
	shm->samples = ALSA_FRAMES * ALSA_CHANNELS;		// in mono samples
	shm->samplepos = 0;
	shm->soundalive = true;
	shm->gamealive = true;
	shm->submission_chunk = 1;

	buffersize = ALSA_FRAMES * ALSA_BYTES_PER_FRAME;
	shm->buffer = Hunk_AllocName (buffersize, "shmbuf");
	if (!shm->buffer)
	{
		Con_Printf ("Sound: no room for a %d byte mixing buffer\n", buffersize);
		snd_pcm_close (pcm);
		pcm = NULL;
		return false;
	}
	memset (shm->buffer, 0, buffersize);

	snd_written = 0;
	snd_played = 0;
	snd_inited = true;

	Con_Printf ("Sound: %d Hz, 16 bit, stereo -> ALSA \"%s\", %d ms\n",
				ALSA_RATE, device, (int)(snd_bufsize * 1000 / ALSA_RATE));

	return true;
}


/*
==================
S_AlsaRecover

After an underrun (the engine stalled on a level load and the device ran
dry) or a suspend, the device has to be prepared again before it takes more.
Returns whether it will.
==================
*/
static qboolean S_AlsaRecover (int err)
{
	return snd_pcm_recover (pcm, err, 1) >= 0;
}


/*
==================
S_AlsaQueued

Frames written that the device has not played yet.
==================
*/
static unsigned int S_AlsaQueued (void)
{
	snd_pcm_sframes_t	avail;

	avail = snd_pcm_avail (pcm);
	if (avail < 0)
	{
	// An underrun: the device played everything and stopped.
		S_AlsaRecover ((int)avail);
		return 0;
	}
	if ((snd_pcm_uframes_t)avail >= snd_bufsize)
		return 0;

	return (unsigned int)(snd_bufsize - avail);
}


/*
==================
SNDDMA_GetSamples

The playback position as a running count of frames; see the same function in
snd_stream.c for why the mixer asks for a count rather than a position in the
ring.
==================
*/
int SNDDMA_GetSamples (void)
{
	unsigned int	played;

	if (!snd_inited)
		return 0;

	played = snd_written - S_AlsaQueued ();
	if ((int)(played - snd_played) > 0)
		snd_played = played;

	return (int)snd_played;
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

	shm->samplepos = (SNDDMA_GetSamples () * ALSA_CHANNELS) % shm->samples;
	return shm->samplepos;
}


/*
==================
SNDDMA_RebaseClock

Moves the running counts back together with the mixer's, so they stay inside
an int. See snd_stream.c.
==================
*/
void SNDDMA_RebaseClock (int frames)
{
	snd_written -= (unsigned int)frames;
	snd_played -= (unsigned int)frames;
}


/*
==================
SNDDMA_Submit

Called once a frame, after the mixer has filled the ring up to paintedtime.
Writes what the mixer has painted and the device has room for.
==================
*/
void SNDDMA_Submit (void)
{
	snd_pcm_sframes_t	avail, n;
	unsigned int		first, count, upto;
	int					tries;

	if (!snd_inited)
		return;

	upto = (unsigned int)paintedtime;

// The mixer has been reset behind what was sent: nothing new to send yet.
	if ((int)(upto - snd_written) <= 0)
		return;

// Further behind than the ring is long, and what was there has been painted
// over since. Skip to what is still there rather than playing the wrong audio.
	if (upto - snd_written > ALSA_FRAMES / 2)
		snd_written = upto - ALSA_FRAMES / 2;

	for (tries = 0 ; tries < 4 && snd_written != upto ; )
	{
		avail = snd_pcm_avail_update (pcm);
		if (avail < 0)
		{
			tries++;
			if (!S_AlsaRecover ((int)avail))
				return;
			continue;
		}
		if (avail == 0)
			break;

		first = snd_written % ALSA_FRAMES;
		count = upto - snd_written;
		if (count > ALSA_FRAMES - first)
			count = ALSA_FRAMES - first;		// stop at the wrap
		if (count > (unsigned int)avail)
			count = (unsigned int)avail;

		n = snd_pcm_writei (pcm, (unsigned char *)shm->buffer
							+ first * ALSA_BYTES_PER_FRAME, count);
		if (n == -EAGAIN)
			break;
		if (n < 0)
		{
			tries++;
			if (!S_AlsaRecover ((int)n))
				return;
			continue;
		}
		snd_written += (unsigned int)n;
	}

// ALSA starts a stream once its buffer is full. The mixer may not paint that
// far ahead -- _snd_mixahead is a setting -- so start it as soon as there is
// anything in it, rather than wait for a fill that may never come.
	if (snd_pcm_state (pcm) == SND_PCM_STATE_PREPARED && snd_written)
		snd_pcm_start (pcm);
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
	if (pcm)
	{
		snd_pcm_drop (pcm);
		snd_pcm_close (pcm);
		pcm = NULL;
	}
}

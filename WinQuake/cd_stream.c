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
// cd_stream.c -- music, from files rather than from a CD.
//
// Quake's music is not in the game data. It is audio tracks 2 to 11 of the
// CD-ROM, and cd_linux.c plays them by opening /dev/cdrom and issuing
// CDROMPLAYTRKIND -- a drive command, to a drive, which tells its own DAC to
// play. No container has one, and neither do most machines any more.
//
// So the same track numbers are looked up as files instead, and decoded and
// mixed into the engine's own output. That is a change of kind, not just of
// source: on the CD the music never went through Quake's mixer at all, which
// is why bgmvolume in the 1996 code sets the drive's volume rather than
// scaling anything. Here it scales samples, and the master volume applies to
// the music as well, because the music is now part of what the master volume
// is the volume of.
//
// Tracks are looked for as <dir>/track02.<ext>, numbered the way the CD was,
// so a rip made by any of the usual tools drops straight in. Which is also
// what every other source port settled on, so an existing music directory
// works without being renamed.
//
// Decoding is libsndfile's: Ogg Vorbis, FLAC, WAV, Opus and, where the
// installed build has it, MP3. Build with MUSIC=none to leave it out, and
// CDAudio_Init reports that there is no music rather than that there are no
// files.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "quakedef.h"

#ifdef QUAKE_MUSIC_SNDFILE
#include <sndfile.h>
#endif

extern	cvar_t	bgmvolume;

// Track 1 is the data track on every Quake CD; the music starts at 2.
#define FIRST_MUSIC_TRACK	2
#define MAX_MUSIC_TRACK		99

static qboolean	music_enabled = false;		// a music directory was found
static qboolean	music_paused = false;
static qboolean	music_looping = false;
static byte		music_track = 0;			// 0 = nothing playing
static char		music_dir[MAX_OSPATH];

// Extensions in the order they are tried. WAV last: a directory holding both
// a compressed rip and the wav it was made from wants the compressed one.
//
// Outside the sndfile block because the search for a music directory reads it
// too, and that search is common code -- a build with MUSIC=none or MUSIC=cd
// still has to compile.
static const char *music_exts[] = { "ogg", "opus", "flac", "mp3", "wav", NULL };

#ifdef QUAKE_MUSIC_SNDFILE

static SNDFILE	*music_sf;
static SF_INFO	music_info;

// Where we are between two decoded source frames, so that a source rate that
// is not the output rate still comes out at the right pitch and length.
static float	music_frac;
static float	music_prev[2];		// the frame behind music_frac
static float	music_next[2];		// the frame ahead of it
static float	music_step;			// source frames per output frame
static qboolean	music_eof;

#endif


/*
==================
CDAudio_FindMusicDir

The first of these that exists and holds a track:

  -musicdir <path>     an explicit override for this run
  <gamedir>/music      beside the pak files, which is where a rip usually goes
  $QUAKE_MUSICDIR      set by the container's entrypoint, or by hand
  <basedir>/id1/music  so that -game mod still finds the base game's music

The game directory beats the environment on purpose. Scourge of Armagon and
Dissolution of Eternity have soundtracks of their own, and a rip of each goes
in its own directory -- but $QUAKE_MUSICDIR is necessarily one directory for
every game the container can run, so if it won, a mission pack would play
Quake's music instead of its own. It is the cross-game default, which is what
it is left to be. -musicdir is still first, because it is aimed at one run.

"Holds a track" is checked rather than assumed. The container links a music
directory into every game directory it finds, so an empty one is easy to end
up with, and an empty directory that won would mean silence with no fallback
rather than the base game's soundtrack.

Which file is used for a given track is still decided per track, because a
partial rip is normal -- someone who has track 4 and not track 7 should get
track 4.
==================
*/
static qboolean CDAudio_DirHasFiles (const char *dir)
{
	struct stat		st;
	DIR				*d;
	struct dirent	*e;
	qboolean		found = false;

	if (!dir || !*dir)
		return false;

	if (stat (dir, &st) != 0 || !S_ISDIR (st.st_mode))
		return false;

	d = opendir (dir);
	if (!d)
		return false;

	while (!found && (e = readdir (d)) != NULL)
	{
		const char	*ext;
		int			i;

		if (Q_strncasecmp ((char *)e->d_name, "track", 5))
			continue;

		ext = strrchr (e->d_name, '.');
		if (!ext)
			continue;

		for (i = 0; music_exts[i]; i++)
			if (!Q_strcasecmp ((char *)ext + 1, (char *)music_exts[i]))
			{
				found = true;
				break;
			}
	}

	closedir (d);
	return found;
}

static void CDAudio_FindMusicDir (void)
{
	const char	*env;
	char		trial[MAX_OSPATH];
	int			i;

	music_dir[0] = 0;

	i = COM_CheckParm ("-musicdir");
	if (i && i < com_argc - 1 && CDAudio_DirHasFiles (com_argv[i+1]))
	{
		Q_strncpy (music_dir, com_argv[i+1], sizeof(music_dir) - 1);
		return;
	}

	snprintf (trial, sizeof(trial), "%s/music", com_gamedir);
	if (CDAudio_DirHasFiles (trial))
	{
		Q_strncpy (music_dir, trial, sizeof(music_dir) - 1);
		return;
	}

	env = getenv ("QUAKE_MUSICDIR");
	if (CDAudio_DirHasFiles (env))
	{
		Q_strncpy (music_dir, (char *)env, sizeof(music_dir) - 1);
		return;
	}

	snprintf (trial, sizeof(trial), "%s/%s/music", host_parms.basedir, GAMENAME);
	if (CDAudio_DirHasFiles (trial))
		Q_strncpy (music_dir, trial, sizeof(music_dir) - 1);
}


#ifdef QUAKE_MUSIC_SNDFILE

/*
==================
CDAudio_CloseTrack
==================
*/
static void CDAudio_CloseTrack (void)
{
	if (music_sf)
	{
		sf_close (music_sf);
		music_sf = NULL;
	}
	music_track = 0;
	music_eof = false;
}


/*
==================
CDAudio_OpenTrack

Tries each extension in turn. Returns false with nothing open if none of them
is there, which is not an error: a partial rip is the normal case.
==================
*/
static qboolean CDAudio_OpenTrack (int track)
{
	char	path[MAX_OSPATH];
	int		i;

	CDAudio_CloseTrack ();

	if (!music_dir[0])
		return false;

	for (i = 0; music_exts[i]; i++)
	{
		snprintf (path, sizeof(path), "%s/track%02d.%s", music_dir, track,
				  music_exts[i]);

		memset (&music_info, 0, sizeof(music_info));
		music_sf = sf_open (path, SFM_READ, &music_info);
		if (music_sf)
			break;
	}

	if (!music_sf)
		return false;

	if (music_info.channels < 1 || music_info.channels > 2)
	{
		Con_Printf ("CDAudio: track %d has %d channels; only mono and stereo\n",
					track, music_info.channels);
		CDAudio_CloseTrack ();
		return false;
	}

	if (music_info.samplerate < 1)
	{
		Con_Printf ("CDAudio: track %d reports a sample rate of %d\n",
					track, music_info.samplerate);
		CDAudio_CloseTrack ();
		return false;
	}

	music_step = (float)music_info.samplerate / (float)shm->speed;
	music_frac = 1.0f;				// forces both frames to be read first
	music_prev[0] = music_prev[1] = 0;
	music_next[0] = music_next[1] = 0;
	music_eof = false;
	music_track = (byte)track;

	Con_Printf ("CDAudio: playing %s (%d Hz, %s)\n", path,
				music_info.samplerate,
				music_info.channels == 2 ? "stereo" : "mono");
	return true;
}


/*
==================
CDAudio_NextSourceFrame

One frame from the file, as a stereo pair. Returns false at the end of the
file, having restarted it first if the track is looping.
==================
*/
static qboolean CDAudio_NextSourceFrame (float *out)
{
	float		raw[2];
	sf_count_t	got;

	got = sf_readf_float (music_sf, raw, 1);
	if (got < 1)
	{
		if (!music_looping)
			return false;

	// sf_seek back to the start rather than reopening: reopening an Ogg
	// costs a decoder reset and a file open in the middle of a frame, and
	// the loop is audible if it takes longer than the mixer's headroom.
		if (sf_seek (music_sf, 0, SEEK_SET) < 0)
			return false;

		got = sf_readf_float (music_sf, raw, 1);
		if (got < 1)
			return false;
	}

	if (music_info.channels == 1)
	{
		out[0] = raw[0];
		out[1] = raw[0];
	}
	else
	{
		out[0] = raw[0];
		out[1] = raw[1];
	}

	return true;
}


/*
==================
CDAudio_MixPaintBuffer

Called from S_PaintChannels with the stretch of paint buffer the sound effects
have just been mixed into. Music is added on top, at the same scale: the
transfer stage multiplies by the master volume and clips, so a full-scale
sample here is a full-scale sample out.

Resampling is linear. A CD rip is 44100 and the output is 22050, so this is
almost always a straight 2:1 decimation; linear interpolation is inaudible on
music at that ratio and costs two multiplies a sample, where a windowed
resampler would cost more than the rest of the mixer put together.
==================
*/
void CDAudio_MixPaintBuffer (portable_samplepair_t *buffer, int count)
{
	int		i;
	float	vol;

	if (!music_enabled || !music_sf || music_paused || music_eof)
		return;

	vol = bgmvolume.value;
	if (vol <= 0)
		return;
	if (vol > 1)
		vol = 1;

// 32767 rather than 32768: the transfer stage clips at 32767, and a track
// mastered to full scale would otherwise clip on every trough.
	vol *= 32767.0f;

	for (i = 0; i < count; i++)
	{
		float	l, r;

		while (music_frac >= 1.0f)
		{
			music_prev[0] = music_next[0];
			music_prev[1] = music_next[1];

			if (!CDAudio_NextSourceFrame (music_next))
			{
				music_eof = true;
				CDAudio_CloseTrack ();
				return;
			}

			music_frac -= 1.0f;
		}

		l = music_prev[0] + (music_next[0] - music_prev[0]) * music_frac;
		r = music_prev[1] + (music_next[1] - music_prev[1]) * music_frac;

		buffer[i].left  += (int)(l * vol);
		buffer[i].right += (int)(r * vol);

		music_frac += music_step;
	}
}

#else	// !QUAKE_MUSIC_SNDFILE

void CDAudio_MixPaintBuffer (portable_samplepair_t *buffer, int count)
{
}

static void CDAudio_CloseTrack (void)
{
	music_track = 0;
}

static qboolean CDAudio_OpenTrack (int track)
{
	return false;
}

#endif	// QUAKE_MUSIC_SNDFILE


/*
==================
CDAudio_Play
==================
*/
void CDAudio_Play (byte track, qboolean looping)
{
	if (!music_enabled)
		return;

	if (track < FIRST_MUSIC_TRACK || track > MAX_MUSIC_TRACK)
	{
		Con_DPrintf ("CDAudio: track %d out of range\n", track);
		return;
	}

	if (music_track == track && !music_paused)
	{
		music_looping = looping;
		return;						// already playing it
	}

	music_looping = looping;
	music_paused = false;

	if (!CDAudio_OpenTrack (track))
		Con_DPrintf ("CDAudio: no file for track %d in %s\n", track, music_dir);
}


void CDAudio_Stop (void)
{
	if (!music_enabled)
		return;

	CDAudio_CloseTrack ();
	music_paused = false;
}


void CDAudio_Pause (void)
{
	if (!music_enabled || !music_track)
		return;

	music_paused = true;
}


void CDAudio_Resume (void)
{
	if (!music_enabled || !music_track)
		return;

	music_paused = false;
}


/*
==================
CDAudio_Update

The 1996 version polled the drive here, because the drive was the only thing
that knew whether the track had finished. Nothing to poll now: the mixer
notices the end of the file as it reaches it.
==================
*/
void CDAudio_Update (void)
{
}


/*
==================
CD_f

The same console command the CD player had, so that anything binding or
scripting `cd play 4` still works, minus the commands that only meant
something to a drive: eject, close, reset.
==================
*/
static void CD_f (void)
{
	char	*command;
	int		n;

	if (Cmd_Argc() < 2)
	{
		Con_Printf ("cd play <track> | loop <track> | stop | pause | resume | info\n");
		return;
	}

	command = Cmd_Argv (1);

	if (Q_strcasecmp (command, "on") == 0 || Q_strcasecmp (command, "off") == 0)
	{
	// There is no drive to turn on or off; the music is whatever is in the
	// directory. Kept so old configs do not print an error at startup.
		return;
	}

	if (Q_strcasecmp (command, "play") == 0)
	{
		CDAudio_Play ((byte)Q_atoi (Cmd_Argv (2)), false);
		return;
	}

	if (Q_strcasecmp (command, "loop") == 0)
	{
		CDAudio_Play ((byte)Q_atoi (Cmd_Argv (2)), true);
		return;
	}

	if (Q_strcasecmp (command, "stop") == 0)
	{
		CDAudio_Stop ();
		return;
	}

	if (Q_strcasecmp (command, "pause") == 0)
	{
		CDAudio_Pause ();
		return;
	}

	if (Q_strcasecmp (command, "resume") == 0)
	{
		CDAudio_Resume ();
		return;
	}

	if (Q_strcasecmp (command, "eject") == 0
		|| Q_strcasecmp (command, "close") == 0
		|| Q_strcasecmp (command, "reset") == 0
		|| Q_strcasecmp (command, "remap") == 0)
	{
		Con_Printf ("cd %s: there is no drive; music comes from %s\n",
					command, music_dir[0] ? music_dir : "nowhere -- see below");
		if (!music_dir[0])
			Con_Printf ("  put track02.ogg and friends in <gamedir>/music\n");
		return;
	}

	if (Q_strcasecmp (command, "info") == 0)
	{
		if (!music_enabled)
			Con_Printf ("No music: this build has no decoder.\n");
		else if (!music_dir[0])
			Con_Printf ("No music directory. Looked for -musicdir,\n"
						"<gamedir>/music, $QUAKE_MUSICDIR and\n"
						"<basedir>/%s/music.\n", GAMENAME);
		else
		{
			Con_Printf ("Music directory: %s\n", music_dir);
			n = 0;
			for (n = FIRST_MUSIC_TRACK; n <= 11; n++)
				;
			if (music_track)
				Con_Printf ("Playing track %d%s%s\n", music_track,
							music_looping ? ", looping" : "",
							music_paused ? ", paused" : "");
			else
				Con_Printf ("Nothing playing\n");
			Con_Printf ("Volume is %f\n", bgmvolume.value);
		}
		return;
	}

	Con_Printf ("cd: unknown command \"%s\"\n", command);
}


/*
==================
CDAudio_Init
==================
*/
int CDAudio_Init (void)
{
	if (cls.state == ca_dedicated)
		return -1;

	if (COM_CheckParm ("-nocdaudio") || COM_CheckParm ("-nomusic"))
		return -1;

	Cmd_AddCommand ("cd", CD_f);

#ifndef QUAKE_MUSIC_SNDFILE
	Con_Printf ("CDAudio: built without a decoder; no music.\n");
	return -1;
#else
	CDAudio_FindMusicDir ();

	if (!music_dir[0])
	{
	// Not an error. The shareware game data has no music to go with it --
	// the tracks were on the CD -- so this is the ordinary case for anyone
	// who has not ripped their own.
		Con_Printf ("CDAudio: no music directory; put track02.ogg and\n"
					"         friends in <gamedir>/music to hear the soundtrack.\n");
		music_enabled = true;		// so `cd info` can say where it looked
		return 0;
	}

	Con_Printf ("CDAudio: music from %s\n", music_dir);
	music_enabled = true;
	return 0;
#endif
}


void CDAudio_Shutdown (void)
{
	if (!music_enabled)
		return;

	CDAudio_CloseTrack ();
	music_enabled = false;
}

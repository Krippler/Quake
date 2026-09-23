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
// localize.c -- the 2021 re-release's message strings

//
// The re-release's QuakeC does not print text. It prints keys -- "$qc_need_
// gold_key" -- and the engine looks them up in localization/loc_english.txt,
// filling in "{}" or "{N}" with the arguments that follow. Map entities do the
// same: a trigger's "message" is a key. Without this every such message came
// out as a key, and since the re-release also reaches its print functions by
// name (see PR_PatchRereleaseBuiltins), in practice as nothing at all.
//
// The file is one entry per line, key = "value", with // comments and C-style
// escapes; the same reading QuakeSpasm gives it. Every copy is read and they
// are merged, the first to define a key winning: the game directories in
// search order (a mod's own before id1's), then loose under the base
// directory, then inside QuakeEX.kpf -- a zip beside id1, which is where the
// re-release keeps the main one. Stopping at the first found would let a mod's
// few strings hide all of the base game's.
//

#include "quakedef.h"
#include <zlib.h>

#define LOC_FILE	"localization/loc_english.txt"

typedef struct
{
	char	*key;		// without the '$'
	char	*value;
} locentry_t;

static locentry_t	*loc_entries;
static int			loc_numentries, loc_maxentries;
static int			*loc_index;		// open addressing, entry number + 1
static int			loc_indexsize;

static unsigned LOC_Hash (const char *s)
{
	unsigned	h = 2166136261u;

	while (*s)
		h = (h ^ (byte)*s++) * 16777619u;
	return h;
}

static char *LOC_ReadLoose (const char *path)
{
	FILE	*f;
	long	len;
	char	*buf;

	f = fopen (path, "rb");
	if (!f)
		return NULL;
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	buf = len >= 0 ? malloc (len + 1) : NULL;
	if (buf && fread (buf, 1, len, f) == (size_t)len)
		buf[len] = 0;
	else
	{
		free (buf);
		buf = NULL;
	}
	fclose (f);
	return buf;
}

static unsigned LOC_Get16 (const byte *p) { return p[0] | (p[1] << 8); }
static unsigned LOC_Get32 (const byte *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long LOC_Get64 (const byte *p)
{
	return LOC_Get32 (p) | ((unsigned long long)LOC_Get32 (p + 4) << 32);
}

static char	loc_ziperror[128];	// why the last LOC_ReadFromZip found nothing

//
// One file out of a zip: the end record gives the central directory, the
// central directory gives the entry, and the entry is stored or deflated.
//
// QuakeEX.kpf holds the whole re-release, and past 65535 entries or 4 GB a zip
// keeps its counts and offsets in ZIP64 records instead: a second end record,
// found through a locator just before the first, and an extra field on each
// entry carrying whichever of its sizes and offset did not fit. Both are read.
//
static char *LOC_ReadFromZip (const char *zippath, const char *name)
{
	FILE		*f;
	long		size, tail, i, eocd;
	byte		*buf = NULL, *cd = NULL, *p, *x, *comp = NULL;
	byte		hdr[56];
	unsigned long long	cdsize, cdoff, count, n, csize, usize, lhoff;
	unsigned	method, namelen, extralen, commentlen, id, len;
	char		*out = NULL;
	z_stream	z;
	int			r;

	loc_ziperror[0] = 0;
	f = fopen (zippath, "rb");
	if (!f)
	{
		sprintf (loc_ziperror, "cannot be opened");
		return NULL;
	}

	fseek (f, 0, SEEK_END);
	size = ftell (f);
	tail = size < 65536 + 22 ? size : 65536 + 22;
	buf = malloc (tail);
	if (!buf || fseek (f, size - tail, SEEK_SET)
		|| fread (buf, 1, tail, f) != (size_t)tail)
	{
		sprintf (loc_ziperror, "cannot be read");
		goto done;
	}

	for (eocd = tail - 22 ; eocd >= 0 ; eocd--)
		if (LOC_Get32 (buf + eocd) == 0x06054b50)
			break;
	if (eocd < 0)
	{
		sprintf (loc_ziperror, "is not a zip");
		goto done;
	}
	count = LOC_Get16 (buf + eocd + 10);
	cdsize = LOC_Get32 (buf + eocd + 12);
	cdoff = LOC_Get32 (buf + eocd + 16);

	// ZIP64: a locator 20 bytes before the end record points at the real one
	if (eocd >= 20 && LOC_Get32 (buf + eocd - 20) == 0x07064b50)
	{
		unsigned long long	z64 = LOC_Get64 (buf + eocd - 20 + 8);

		if (fseek (f, (long)z64, SEEK_SET) || fread (hdr, 1, 56, f) != 56
			|| LOC_Get32 (hdr) != 0x06064b50)
		{
			sprintf (loc_ziperror, "has a broken ZIP64 end record");
			goto done;
		}
		count = LOC_Get64 (hdr + 32);
		cdsize = LOC_Get64 (hdr + 40);
		cdoff = LOC_Get64 (hdr + 48);
	}

	if (cdsize > 256*1024*1024 || (long)(cdoff + cdsize) > size)
	{
		sprintf (loc_ziperror, "has a central directory out of range");
		goto done;
	}
	cd = malloc (cdsize);
	if (!cd || fseek (f, (long)cdoff, SEEK_SET)
		|| fread (cd, 1, cdsize, f) != cdsize)
	{
		sprintf (loc_ziperror, "has a central directory that cannot be read");
		goto done;
	}

	for (p = cd, n = 0 ; n < count && p + 46 <= cd + cdsize ; n++)
	{
		if (LOC_Get32 (p) != 0x02014b50)
			break;
		method = LOC_Get16 (p + 10);
		csize = LOC_Get32 (p + 20);
		usize = LOC_Get32 (p + 24);
		namelen = LOC_Get16 (p + 28);
		extralen = LOC_Get16 (p + 30);
		commentlen = LOC_Get16 (p + 32);
		lhoff = LOC_Get32 (p + 42);

		if (namelen == strlen (name) && p + 46 + namelen <= cd + cdsize
			&& !Q_strncasecmp ((char *)p + 46, (char *)name, namelen))
		{
			// the ZIP64 extra field holds, in order, whichever did not fit
			for (x = p + 46 + namelen ; x + 4 <= p + 46 + namelen + extralen
				 && x + 4 <= cd + cdsize ; x += 4 + len)
			{
				id = LOC_Get16 (x);
				len = LOC_Get16 (x + 2);
				if (id != 0x0001)
					continue;
				i = 4;
				if (usize == 0xFFFFFFFF && i + 8 <= 4 + len)
					usize = LOC_Get64 (x + i), i += 8;
				if (csize == 0xFFFFFFFF && i + 8 <= 4 + len)
					csize = LOC_Get64 (x + i), i += 8;
				if (lhoff == 0xFFFFFFFF && i + 8 <= 4 + len)
					lhoff = LOC_Get64 (x + i), i += 8;
			}

			if (usize > 64*1024*1024 || csize > 64*1024*1024)
			{
				sprintf (loc_ziperror, "holds %s at an unlikely size", name);
				goto done;
			}
			if (fseek (f, (long)lhoff, SEEK_SET) || fread (hdr, 1, 30, f) != 30
				|| LOC_Get32 (hdr) != 0x04034b50)
			{
				sprintf (loc_ziperror, "has a broken entry for %s", name);
				goto done;
			}
			fseek (f, LOC_Get16 (hdr + 26) + LOC_Get16 (hdr + 28), SEEK_CUR);

			comp = malloc (csize ? csize : 1);
			out = malloc (usize + 1);
			if (!comp || !out || fread (comp, 1, csize, f) != csize)
			{
				sprintf (loc_ziperror, "cannot be read at %s", name);
				goto fail;
			}

			if (method == 0 && csize == usize)
				memcpy (out, comp, usize);
			else if (method == 8)
			{
				memset (&z, 0, sizeof(z));
				if (inflateInit2 (&z, -MAX_WBITS) != Z_OK)
					goto fail;
				z.next_in = comp;
				z.avail_in = csize;
				z.next_out = (byte *)out;
				z.avail_out = usize;
				r = inflate (&z, Z_FINISH);
				inflateEnd (&z);
				if (r != Z_STREAM_END)
				{
					sprintf (loc_ziperror, "holds %s, but it does not inflate",
							 name);
					goto fail;
				}
			}
			else
			{
				sprintf (loc_ziperror, "holds %s compressed by method %u, "
						 "which is not deflate", name, method);
				goto fail;
			}

			out[usize] = 0;
			goto done;
		}
		p += 46 + namelen + extralen + commentlen;
	}
	sprintf (loc_ziperror, "has no %s among its %llu files", name, n);

fail:
	free (out);
	out = NULL;
done:
	free (comp);
	free (cd);
	free (buf);
	fclose (f);
	return out;
}

//
// The file is UTF-8 and Quake's font is not. English needs little beyond
// ASCII; the typographic quotes, dashes and ellipsis are folded to their plain
// forms, and anything else becomes a '?' rather than two bytes of noise.
//
static void LOC_FoldUTF8 (char *s)
{
	byte		*in = (byte *)s, *out = (byte *)s;
	unsigned	c;
	int			extra;

	while (*in)
	{
		if (*in < 0x80)
		{
			*out++ = *in++;
			continue;
		}
		if ((*in & 0xE0) == 0xC0)
			c = *in & 0x1F, extra = 1;
		else if ((*in & 0xF0) == 0xE0)
			c = *in & 0x0F, extra = 2;
		else if ((*in & 0xF8) == 0xF0)
			c = *in & 0x07, extra = 3;
		else
		{
			in++;
			*out++ = '?';
			continue;
		}
		in++;
		while (extra-- && (*in & 0xC0) == 0x80)
			c = (c << 6) | (*in++ & 0x3F);

		switch (c)
		{
		case 0x2018: case 0x2019: case 0x201A: case 0x2032:
			*out++ = '\'';
			break;
		case 0x201C: case 0x201D: case 0x201E: case 0x2033:
			*out++ = '"';
			break;
		case 0x2013: case 0x2014: case 0x2212:
			*out++ = '-';
			break;
		case 0x00A0:
			*out++ = ' ';
			break;
		case 0x2026:	// three bytes in, three out
			*out++ = '.';
			*out++ = '.';
			*out++ = '.';
			break;
		case 0xFEFF:	// a byte order mark
			break;
		default:
			*out++ = '?';
			break;
		}
	}
	*out = 0;
}

//
// One file's entries, added to the rest. The text is kept: the entries point
// into it.
//
static void LOC_AddText (char *text, char *where)
{
	char		*cursor, *line, *equals, *key_end, *value, *src, *dst;
	int			before = loc_numentries;
	locentry_t	*grown;

	cursor = text;
	while (*cursor)
	{
		while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')
			cursor++;
		line = cursor;
		equals = NULL;
		while (*cursor && *cursor != '\n')
		{
			if (*cursor == '=' && !equals)
				equals = cursor;
			cursor++;
		}
		if (*cursor)
			*cursor++ = 0;

		if (line[0] == '/' || !equals)
			continue;

		key_end = equals;
		while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t'))
			key_end--;
		*key_end = 0;
		if (line[0] == '$')
			line++;
		if (!line[0])
			continue;

		value = equals + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		if (*value == '"')
			value++;

		for (src = dst = value ; *src ; )
		{
			if (*src == '\\' && src[1])
			{
				switch (src[1])
				{
				case 'n': *dst++ = '\n'; break;
				case 't': *dst++ = '\t'; break;
				default: *dst++ = src[1]; break;
				}
				src += 2;
				continue;
			}
			if (*src == '"')
				break;
			*dst++ = *src++;
		}
		*dst = 0;
		if (*src != '"')	// unquoted: trim the end
			while (dst > value && (dst[-1] == ' ' || dst[-1] == '\t'
				   || dst[-1] == '\r'))
				*--dst = 0;

		LOC_FoldUTF8 (value);

		if (loc_numentries == loc_maxentries)
		{
			loc_maxentries = loc_maxentries ? loc_maxentries * 2 : 1024;
			grown = realloc (loc_entries, loc_maxentries * sizeof(*loc_entries));
			if (!grown)
				break;
			loc_entries = grown;
		}
		loc_entries[loc_numentries].key = line;
		loc_entries[loc_numentries].value = value;
		loc_numentries++;
	}

	Con_Printf ("Localization: %d strings from %s\n", loc_numentries - before,
				where);

	// the current re-release's QuakeEX.kpf carries only this, in every language
	if (loc_numentries - before == 1
		&& !strcmp (loc_entries[before].key, "placeholder"))
		Con_Printf ("Localization: that is a placeholder with no messages in "
					"it. This version of\nthe re-release keeps the text "
					"elsewhere; any loc_english.txt in the\ngame directories "
					"is read.\n");
}

// the hash over every entry; where two define a key, the first read wins
static void LOC_BuildIndex (void)
{
	int			i, j;
	unsigned	pos;

	loc_indexsize = loc_numentries * 2;
	loc_index = calloc (loc_indexsize, sizeof(*loc_index));
	if (!loc_index)
	{
		loc_numentries = 0;
		return;
	}
	for (i=0 ; i<loc_numentries ; i++)
	{
		pos = LOC_Hash (loc_entries[i].key) % loc_indexsize;
		while ((j = loc_index[pos]))
		{
			if (!strcmp (loc_entries[j-1].key, loc_entries[i].key))
				break;
			pos = (pos + 1) % loc_indexsize;
		}
		if (!j)
			loc_index[pos] = i + 1;
	}
}

void LOC_Init (void)
{
	char	path[MAX_OSPATH*2];
	char	*text;

	COM_ForEachFile (LOC_FILE, LOC_AddText);

	sprintf (path, "%s/%s", com_basedir, LOC_FILE);
	if ((text = LOC_ReadLoose (path)))
		LOC_AddText (text, path);

	sprintf (path, "%s/quakeex.kpf", com_basedir);
	text = LOC_ReadFromZip (path, LOC_FILE);
	if (!text && !strcmp (loc_ziperror, "cannot be opened"))
	{
		sprintf (path, "%s/QuakeEX.kpf", com_basedir);
		text = LOC_ReadFromZip (path, LOC_FILE);
	}
	if (text)
		LOC_AddText (text, path);
	else if (strcmp (loc_ziperror, "cannot be opened"))
		Con_Printf ("Localization: %s %s\n", path, loc_ziperror);

	if (loc_numentries)
		LOC_BuildIndex ();
	// with none, id's data: nothing to look up, and nothing asks
}

//
// The value for "$key", or NULL if it is not a key or not known.
//
const char *LOC_GetRaw (const char *key)
{
	unsigned	pos, start;
	int			i;

	if (!key || key[0] != '$' || !loc_numentries)
		return NULL;
	key++;

	pos = start = LOC_Hash (key) % loc_indexsize;
	while ((i = loc_index[pos]))
	{
		if (!strcmp (loc_entries[i-1].key, key))
			return loc_entries[i-1].value;
		pos = (pos + 1) % loc_indexsize;
		if (pos == start)
			break;
	}
	return NULL;
}

//
// The text for a string the progs or a map hands over: its value if it is a
// known key, otherwise the string itself. A key with no value is printed
// without its '$', which is at least readable, and said once to the console.
//
const char *LOC_GetString (const char *key)
{
	const char	*value;
	static char	missing[64];

	value = LOC_GetRaw (key);
	if (value)
		return value;
	if (!key || key[0] != '$' || !key[1] || strchr (key, ' '))
		return key;

	if (!loc_numentries)
	{
		static qboolean	said;
		if (!said)
		{
			said = true;
			Con_Printf ("This game's messages are re-release keys (%s), and "
						"the text for\nthem is in localization/loc_english.txt, "
						"inside QuakeEX.kpf. None was\nfound: not in a game "
						"directory, nor in %s, nor in\n%s/QuakeEX.kpf. Put "
						"QuakeEX.kpf beside id1 to see the messages.\n", key,
						com_basedir, com_basedir);
		}
	}
	else
	{
		// loaded, but not this one: name the first few, which says which
		// file it should have been in
		static int	said;
		if (said < 8)
		{
			said++;
			Con_Printf ("Localization: no text for %s\n", key);
		}
	}
	Q_strncpy (missing, (char *)key + 1, sizeof(missing) - 1);
	missing[sizeof(missing) - 1] = 0;
	return missing;
}

// the argument number of a placeholder at *s -- {} is 0, {N} is N -- or -1
static int LOC_ParseArg (const char **s)
{
	const char	*p = *s;
	int			n = 0;

	if (*p != '{')
		return -1;
	p++;
	while (*p >= '0' && *p <= '9')
		n = n*10 + *p++ - '0';
	if (*p != '}')
		return -1;
	*s = p + 1;
	return n;
}

qboolean LOC_HasPlaceholders (const char *s)
{
	if (!loc_numentries)
		return false;
	while (*s)
	{
		if (LOC_ParseArg (&s) >= 0)
			return true;
		s++;
	}
	return false;
}

//
// format with its placeholders replaced by getarg's strings, into out
//
void LOC_Format (const char *format, const char *(*getarg) (int n, void *data),
				 void *data, char *out, int outsize)
{
	int			len = 0, n, l;
	const char	*arg;

	while (*format && len < outsize - 1)
	{
		n = LOC_ParseArg (&format);
		if (n < 0)
		{
			out[len++] = *format++;
			continue;
		}
		arg = getarg (n, data);
		l = strlen (arg);
		if (l > outsize - 1 - len)
			l = outsize - 1 - len;
		memcpy (out + len, arg, l);
		len += l;
	}
	out[len] = 0;
}

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
// escapes; the same reading QuakeSpasm gives it. It is looked for in the game
// directories first, then loose under the base directory, then inside
// QuakeEX.kpf -- a zip beside id1, which is where the re-release keeps it.
//

#include "quakedef.h"
#include <zlib.h>

#define LOC_FILE	"localization/loc_english.txt"

typedef struct
{
	char	*key;		// without the '$'
	char	*value;
} locentry_t;

static char			*loc_text;
static locentry_t	*loc_entries;
static int			loc_numentries;
static int			*loc_index;		// open addressing, entry number + 1
static int			loc_indexsize;

static unsigned LOC_Hash (const char *s)
{
	unsigned	h = 2166136261u;

	while (*s)
		h = (h ^ (byte)*s++) * 16777619u;
	return h;
}

//
// the file from the search path: a game directory, loose or in a pak
//
static char *LOC_ReadSearchPath (const char *name)
{
	FILE	*f;
	int		len;
	char	*buf;

	len = COM_FOpenFile ((char *)name, &f);
	if (!f)
		return NULL;
	buf = malloc (len + 1);
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

//
// One file out of a zip: the end record gives the central directory, the
// central directory gives the entry, and the entry is stored or deflated.
//
static char *LOC_ReadFromZip (const char *zippath, const char *name)
{
	FILE		*f;
	long		size, tail, i;
	byte		*buf = NULL, *cd = NULL, *p, *comp = NULL;
	byte		hdr[30];
	unsigned	cdsize, cdoff, count, n;
	unsigned	method, csize, usize, namelen, extralen, commentlen, lhoff;
	char		*out = NULL;
	z_stream	z;

	f = fopen (zippath, "rb");
	if (!f)
		return NULL;

	fseek (f, 0, SEEK_END);
	size = ftell (f);
	tail = size < 65536 + 22 ? size : 65536 + 22;
	buf = malloc (tail);
	if (!buf || fseek (f, size - tail, SEEK_SET)
		|| fread (buf, 1, tail, f) != (size_t)tail)
		goto done;

	for (i = tail - 22 ; i >= 0 ; i--)
		if (LOC_Get32 (buf + i) == 0x06054b50)
			break;
	if (i < 0)
		goto done;
	count = LOC_Get16 (buf + i + 10);
	cdsize = LOC_Get32 (buf + i + 12);
	cdoff = LOC_Get32 (buf + i + 16);

	cd = malloc (cdsize);
	if (!cd || fseek (f, cdoff, SEEK_SET) || fread (cd, 1, cdsize, f) != cdsize)
		goto done;

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
			if (fseek (f, lhoff, SEEK_SET) || fread (hdr, 1, 30, f) != 30
				|| LOC_Get32 (hdr) != 0x04034b50)
				goto done;
			fseek (f, LOC_Get16 (hdr + 26) + LOC_Get16 (hdr + 28), SEEK_CUR);

			comp = malloc (csize ? csize : 1);
			out = malloc (usize + 1);
			if (!comp || !out || fread (comp, 1, csize, f) != csize)
				goto fail;

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
				i = inflate (&z, Z_FINISH);
				inflateEnd (&z);
				if (i != Z_STREAM_END)
					goto fail;
			}
			else
				goto fail;

			out[usize] = 0;
			goto done;
		}
		p += 46 + namelen + extralen + commentlen;
	}

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

static void LOC_Parse (char *text)
{
	char		*cursor, *line, *equals, *key_end, *value, *src, *dst;
	int			max, i;
	unsigned	pos;

	max = 1024;
	loc_entries = malloc (max * sizeof(*loc_entries));
	loc_numentries = 0;

	cursor = text;
	while (*cursor && loc_entries)
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

		if (loc_numentries == max)
		{
			max *= 2;
			loc_entries = realloc (loc_entries, max * sizeof(*loc_entries));
			if (!loc_entries)
				break;
		}
		loc_entries[loc_numentries].key = line;
		loc_entries[loc_numentries].value = value;
		loc_numentries++;
	}

	if (!loc_entries || !loc_numentries)
	{
		loc_numentries = 0;
		return;
	}

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
		while (loc_index[pos])
			pos = (pos + 1) % loc_indexsize;
		loc_index[pos] = i + 1;
	}
}

void LOC_Init (void)
{
	char	path[MAX_OSPATH*2];
	char	*from;

	from = "the game directories";
	loc_text = LOC_ReadSearchPath (LOC_FILE);
	if (!loc_text)
	{
		sprintf (path, "%s/%s", com_basedir, LOC_FILE);
		loc_text = LOC_ReadLoose (path);
		from = "the base directory";
	}
	if (!loc_text)
	{
		sprintf (path, "%s/quakeex.kpf", com_basedir);
		loc_text = LOC_ReadFromZip (path, LOC_FILE);
		if (!loc_text)
		{
			sprintf (path, "%s/QuakeEX.kpf", com_basedir);
			loc_text = LOC_ReadFromZip (path, LOC_FILE);
		}
		from = "QuakeEX.kpf";
	}
	if (!loc_text)
		return;		// id's data: nothing to look up, and nothing asks

	LOC_Parse (loc_text);
	Con_Printf ("Localization: %d strings from %s\n", loc_numentries, from);
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
						"inside QuakeEX.kpf. Put\nQuakeEX.kpf beside id1 to see "
						"the messages.\n", key);
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

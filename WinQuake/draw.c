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

// draw.c -- this is the only file outside the refresh that touches the
// vid buffer

#include "quakedef.h"

typedef struct {
	vrect_t	rect;
	int		width;
	int		height;
	byte	*ptexbytes;
	int		rowbytes;
} rectdesc_t;

static rectdesc_t	r_rectdesc;

byte		*draw_chars;				// 8*8 graphic characters
qpic_t		*draw_disc;
qpic_t		*draw_backtile;

//=============================================================================
/* Support Routines */

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	cache_user_t	cache;
} cachepic_t;

#define	MAX_CACHED_PICS		128
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;

// the 2D canvas; see Draw_SetScale
cvar_t	scr_scale = {"scr_scale", "0", true};
int		draw_scale = 1;		// screen pixels per canvas pixel
int		draw_yoff;			// screen rows above the canvas



qpic_t	*Draw_PicFromWad (char *name)
{
	return W_GetLumpName (name);
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	
	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			break;

	if (i == menu_numcachepics)
	{
		if (menu_numcachepics == MAX_CACHED_PICS)
			Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
		menu_numcachepics++;
		strcpy (pic->name, path);
	}

	dat = Cache_Check (&pic->cache);

	if (dat)
		return dat;

//
// load the pic from disk
//
	COM_LoadCacheFile (path, &pic->cache);
	
	dat = (qpic_t *)pic->cache.data;
	if (!dat)
	{
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	}

	SwapPic (dat);

	return dat;
}



/*
===============
Draw_Init
===============
*/
void Draw_Init (void)
{
	int		i;

	draw_chars = W_GetLumpName ("conchars");
	draw_disc = W_GetLumpName ("disc");
	draw_backtile = W_GetLumpName ("backtile");

	Cvar_RegisterVariable (&scr_scale);

	r_rectdesc.width = draw_backtile->width;
	r_rectdesc.height = draw_backtile->height;
	r_rectdesc.ptexbytes = draw_backtile->data;
	r_rectdesc.rowbytes = draw_backtile->width;
}



/*
==============================================================================

THE 2D CANVAS

id drew the console, menus, status bar and centre text one screen pixel per
art pixel, which is right at 320x200 and unreadable at 1920x1080, where an
8x8 character is a quarter of a percent of the screen's height. So the 2D
layer is laid out on a smaller canvas, vid.conwidth by vid.conheight, and
every pixel of it is drawn draw_scale screen pixels square. Everything that
lays out 2D (console.c, menu.c, sbar.c, the 2D parts of screen.c) works in
canvas units; the 3D view keeps the full resolution.

scr_scale picks the factor: 0, the default, chooses the largest whole number
that leaves the canvas at least 480x360 (3 at 1920x1080, which is a 640x360
canvas); 1 is id's size. The canvas never falls below the 320x200 the menus
are drawn for. When the screen is not a whole multiple of the factor, the
spare rows go at the top, so the status bar still sits on the bottom edge.

==============================================================================
*/

/*
================
Draw_SetScale

Called every frame, before anything is laid out, so that a change of video
mode or of scr_scale lands on the next frame.
================
*/
void Draw_SetScale (void)
{
	extern int	scr_fullupdate;
	int			s, w, h;

	s = (int)scr_scale.value;
	if (s <= 0)
	{
		s = vid.height / 360;
		if (s > (int)vid.width / 480)
			s = vid.width / 480;
	}
	while (s > 1 && ((int)vid.width / s < 320 || (int)vid.height / s < 200))
		s--;
	if (s < 1)
		s = 1;

	w = vid.width / s;
	h = vid.height / s;
	if (s == draw_scale && w == vid.conwidth && h == vid.conheight)
		return;

	draw_scale = s;
	vid.conwidth = w;
	vid.conheight = h;
	draw_yoff = vid.height - h * s;

// the status bar's height on screen has changed, and with it the 3D view
	vid.recalc_refdef = 1;
	scr_fullupdate = 0;
	Sbar_Changed ();
}

/*
================
Draw_ScreenRect

A canvas rectangle in screen pixels. An edge on the canvas's own edge goes to
the screen's, so a fill or tile clear across the whole canvas leaves no
rows or columns of the remainder untouched.
================
*/
static void Draw_ScreenRect (int x, int y, int w, int h, vrect_t *r)
{
	int		x1, y1;

	x1 = x + w;
	y1 = y + h;
	r->x = x <= 0 ? 0 : x * draw_scale;
	r->y = y <= 0 ? 0 : y * draw_scale + draw_yoff;
	x1 = x1 >= (int)vid.conwidth ? (int)vid.width : x1 * draw_scale;
	y1 = y1 >= (int)vid.conheight ? (int)vid.height : y1 * draw_scale + draw_yoff;
	if (r->x > (int)vid.width)
		r->x = vid.width;
	if (r->y > (int)vid.height)
		r->y = vid.height;
	r->width = x1 > r->x ? x1 - r->x : 0;
	r->height = y1 > r->y ? y1 - r->y : 0;
}

/*
================
Draw_Blit

Draws w*h art pixels, srcrow apart, at canvas (x, y), each one mult canvas
pixels square, clipped to the screen. A source pixel equal to key is left out
(-1 draws them all), and translation, if given, recolours the rest.
================
*/
static void Draw_BlitMult (int x, int y, byte *src, int w, int h, int srcrow,
	int key, byte *translation, int mult)
{
	int		s, sx, sy, x0, x1, y0, y1, u, v, px, k, c;
	byte	*row, *dest;

	s = draw_scale * mult;
	sx = x * draw_scale;
	sy = y * draw_scale + draw_yoff;

	y0 = sy < 0 ? 0 : sy;
	y1 = sy + h * s;
	if (y1 > (int)vid.height)
		y1 = vid.height;
	x0 = sx < 0 ? 0 : sx;
	x1 = sx + w * s;
	if (x1 > (int)vid.width)
		x1 = vid.width;
	if (x0 >= x1 || y0 >= y1)
		return;

	for ( ; y0 < y1 ; y0++)
	{
		v = (y0 - sy) / s;
		row = src + v * srcrow;
		dest = vid.buffer + y0 * vid.rowbytes;

		u = (x0 - sx) / s;
		px = sx + u * s;		// first screen column of art pixel u
		for ( ; px < x1 ; u++, px += s)
		{
			c = row[u];
			if (c == key)
				continue;
			if (translation)
				c = translation[c];
			for (k = px < x0 ? x0 : px ; k < px + s && k < x1 ; k++)
				dest[k] = c;
		}
	}
}

static void Draw_Blit (int x, int y, byte *src, int w, int h, int srcrow,
	int key, byte *translation)
{
	Draw_BlitMult (x, y, src, w, h, srcrow, key, translation, 1);
}

/*
================
Draw_PicPart

Rows top to top+h of a picture, each art pixel mult canvas pixels square,
transparent where the art is, and recoloured by translation if it is given.
Clipped to the screen rather than refused, since the menus lay out on a
canvas whose size is the window's.
================
*/
void Draw_PicPart (int x, int y, qpic_t *pic, int top, int h, int mult,
	byte *translation)
{
	if (top < 0)
		top = 0;
	if (top + h > pic->height)
		h = pic->height - top;
	if (h <= 0)
		return;

	Draw_BlitMult (x, y, pic->data + top * pic->width, pic->width, h,
		pic->width, TRANSPARENT_COLOR, translation, mult);
}

/*
================
Draw_CharacterEx

Draw_Character, mult canvas pixels to the art pixel, and recoloured.
================
*/
void Draw_CharacterEx (int x, int y, int num, int mult, byte *translation)
{
	num &= 255;

	Draw_BlitMult (x, y, draw_chars + ((num>>4)<<10) + ((num&15)<<3), 8, 8,
		128, 0, translation, mult);
}

/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character (int x, int y, int num)
{
	num &= 255;

	if (y <= -8)
		return;			// totally off screen

	Draw_Blit (x, y, draw_chars + ((num>>4)<<10) + ((num&15)<<3), 8, 8, 128,
		0, NULL);
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, char *str)
{
	while (*str)
	{
		Draw_Character (x, y, *str);
		str++;
		x += 8;
	}
}

/*
================
Draw_DebugChar

Draws a single character directly to the upper right corner of the screen.
This is for debugging lockups by drawing different chars in different parts
of the code.
================
*/
void Draw_DebugChar (char num)
{
	byte			*dest;
	byte			*source;
	int				drawline;	
	extern byte		*draw_chars;
	int				row, col;

	if (!vid.direct)
		return;		// don't have direct FB access, so no debugchars...

	drawline = 8;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	dest = vid.direct + 312;

	while (drawline--)
	{
		dest[0] = source[0];
		dest[1] = source[1];
		dest[2] = source[2];
		dest[3] = source[3];
		dest[4] = source[4];
		dest[5] = source[5];
		dest[6] = source[6];
		dest[7] = source[7];
		source += 128;
		dest += 320;
	}
}

/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	if ((x < 0) ||
		(x + pic->width > vid.conwidth) ||
		(y < 0) ||
		(y + pic->height > vid.conheight))
	{
		Sys_Error ("Draw_Pic: bad coordinates");
	}

	Draw_Blit (x, y, pic->data, pic->width, pic->height, pic->width, -1, NULL);
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	if (x < 0 || (unsigned)(x + pic->width) > vid.conwidth || y < 0 ||
		 (unsigned)(y + pic->height) > vid.conheight)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}

	Draw_Blit (x, y, pic->data, pic->width, pic->height, pic->width,
		TRANSPARENT_COLOR, NULL);
}


/*
=============
Draw_TransPicTranslate
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation)
{
	if (x < 0 || (unsigned)(x + pic->width) > vid.conwidth || y < 0 ||
		 (unsigned)(y + pic->height) > vid.conheight)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}

	Draw_Blit (x, y, pic->data, pic->width, pic->height, pic->width,
		TRANSPARENT_COLOR, translation);
}


void Draw_CharToConback (int num, byte *dest)
{
	int		row, col;
	byte	*source;
	int		drawline;
	int		x;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x])
				dest[x] = 0x60 + source[x];
		source += 128;
		dest += 320;
	}

}

/*
================
Draw_ConsoleBackground

================
*/
static void Draw_ConsoleBackgroundStamp (int lines, qboolean stamp);

void Draw_ConsoleBackground (int lines)
{
	Draw_ConsoleBackgroundStamp (lines, true);
}

/*
================
Draw_MenuBackground

The console background behind the whole screen, for the menus, without the
version stamp: the re-release's key hints run along the bottom where it is.
================
*/
void Draw_MenuBackground (void)
{
	Draw_ConsoleBackgroundStamp (vid.conheight, false);
}

static void Draw_ConsoleBackgroundStamp (int lines, qboolean stamp)
{
	int				x, y, v;
	byte			*src, *dest, *data;
	int				f, fstep;
	qpic_t			*conback;
	char			ver[100];
	static byte		plain[320*200], stamped[320*200];
	static qboolean	copied;

	conback = Draw_CachePic ("gfx/conback.lmp");
	data = conback->data;

// hack the version number into the pic
//
// The art has a dark plate under the id logo, cut for four characters, and
// DOS Quake stamps "1.09" on it. The X11 build stamped "(X11 Quake 1.10) 1.09"
// ending at the same margin, so the plate held only the last four characters
// and the rest ran across the texture to its left: a black box behind half a
// line of text, which read as a rendering fault. It gets the DOS stamp now.
// The port's own version is on the launch page.
//
// id wrote the stamp into the cached picture itself. The menus draw the same
// picture without it, since their key hints run along the bottom where it is,
// and the cache moves and reloads pictures when it likes -- so there is no
// telling, later, whether what is in the cache has been stamped. The picture
// is copied the first time instead, before anything has written on it, and
// both versions are drawn from the copies.
//
// Every offset here assumes id's 320x200 picture; a game directory with a
// different one is drawn as it is, stamped or not.
//
	if (conback->width == 320 && conback->height == 200)
	{
		if (!copied)
		{
			memcpy (plain, conback->data, sizeof(plain));
			memcpy (stamped, conback->data, sizeof(stamped));
			sprintf (ver, "%4.2f", VERSION);
			dest = stamped + 320 - 43 + 320*186;
			for (x=0 ; x<strlen(ver) ; x++)
				Draw_CharToConback (ver[x], dest+(x<<3));
			copied = true;
		}
		data = stamp ? stamped : plain;
	}

// draw the pic, at the screen's own resolution: it is a picture, not text,
// and scaling it by whole pixels would only make it blockier. lines is in
// canvas rows, and the whole canvas is the whole screen.
	if (lines >= (int)vid.conheight)
		lines = vid.height;
	else
		lines = lines * draw_scale + draw_yoff;
	dest = vid.buffer;
	fstep = 320*0x10000/vid.width;

	for (y=0 ; y<lines ; y++, dest += vid.rowbytes)
	{
		v = (vid.height - lines + y)*200/vid.height;
		src = data + v*320;
		f = 0;
		for (x=0 ; x<vid.width ; x++, f += fstep)
			dest[x] = src[f>>16];
	}
}


/*
==============
R_DrawRect8
==============
*/
void R_DrawRect8 (vrect_t *prect, int rowbytes, byte *psrc,
	int transparent)
{
	byte	t;
	int		i, j, srcdelta, destdelta;
	byte	*pdest;

	pdest = vid.buffer + (prect->y * vid.rowbytes) + prect->x;

	srcdelta = rowbytes - prect->width;
	destdelta = vid.rowbytes - prect->width;

	if (transparent)
	{
		for (i=0 ; i<prect->height ; i++)
		{
			for (j=0 ; j<prect->width ; j++)
			{
				t = *psrc;
				if (t != TRANSPARENT_COLOR)
				{
					*pdest = t;
				}

				psrc++;
				pdest++;
			}

			psrc += srcdelta;
			pdest += destdelta;
		}
	}
	else
	{
		for (i=0 ; i<prect->height ; i++)
		{
			memcpy (pdest, psrc, prect->width);
			psrc += rowbytes;
			pdest += vid.rowbytes;
		}
	}
}


/*
==============
R_DrawRect16
==============
*/
void R_DrawRect16 (vrect_t *prect, int rowbytes, byte *psrc,
	int transparent)
{
	byte			t;
	int				i, j, srcdelta, destdelta;
	unsigned short	*pdest;

// FIXME: would it be better to pre-expand native-format versions?

	pdest = (unsigned short *)vid.buffer +
			(prect->y * (vid.rowbytes >> 1)) + prect->x;

	srcdelta = rowbytes - prect->width;
	destdelta = (vid.rowbytes >> 1) - prect->width;

	if (transparent)
	{
		for (i=0 ; i<prect->height ; i++)
		{
			for (j=0 ; j<prect->width ; j++)
			{
				t = *psrc;
				if (t != TRANSPARENT_COLOR)
				{
					*pdest = d_8to16table[t];
				}

				psrc++;
				pdest++;
			}

			psrc += srcdelta;
			pdest += destdelta;
		}
	}
	else
	{
		for (i=0 ; i<prect->height ; i++)
		{
			for (j=0 ; j<prect->width ; j++)
			{
				*pdest = d_8to16table[*psrc];
				psrc++;
				pdest++;
			}

			psrc += srcdelta;
			pdest += destdelta;
		}
	}
}


/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	int				width, height, tileoffsetx, tileoffsety;
	byte			*psrc;
	vrect_t			vr;

// the tile is backdrop rather than art, so it repeats at screen resolution
	Draw_ScreenRect (x, y, w, h, &r_rectdesc.rect);

	vr.y = r_rectdesc.rect.y;
	height = r_rectdesc.rect.height;

	tileoffsety = vr.y % r_rectdesc.height;

	while (height > 0)
	{
		vr.x = r_rectdesc.rect.x;
		width = r_rectdesc.rect.width;

		if (tileoffsety != 0)
			vr.height = r_rectdesc.height - tileoffsety;
		else
			vr.height = r_rectdesc.height;

		if (vr.height > height)
			vr.height = height;

		tileoffsetx = vr.x % r_rectdesc.width;

		while (width > 0)
		{
			if (tileoffsetx != 0)
				vr.width = r_rectdesc.width - tileoffsetx;
			else
				vr.width = r_rectdesc.width;

			if (vr.width > width)
				vr.width = width;

			psrc = r_rectdesc.ptexbytes +
					(tileoffsety * r_rectdesc.rowbytes) + tileoffsetx;

			if (r_pixbytes == 1)
			{
				R_DrawRect8 (&vr, r_rectdesc.rowbytes, psrc, 0);
			}
			else
			{
				R_DrawRect16 (&vr, r_rectdesc.rowbytes, psrc, 0);
			}

			vr.x += vr.width;
			width -= vr.width;
			tileoffsetx = 0;	// only the left tile can be left-clipped
		}

		vr.y += vr.height;
		height -= vr.height;
		tileoffsety = 0;		// only the top tile can be top-clipped
	}
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c)
{
	byte			*dest;
	int				u, v;
	vrect_t			r;

	Draw_ScreenRect (x, y, w, h, &r);
	dest = vid.buffer + r.y*vid.rowbytes + r.x;
	for (v=0 ; v<r.height ; v++, dest += vid.rowbytes)
		for (u=0 ; u<r.width ; u++)
			dest[u] = c;
}
//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	int			x,y;
	byte		*pbuf;

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();

	for (y=0 ; y<vid.height ; y++)
	{
		int	t;

		pbuf = (byte *)(vid.buffer + vid.rowbytes*y);
		t = (y & 1) << 1;

		for (x=0 ; x<vid.width ; x++)
		{
			if ((x & 3) != t)
				pbuf[x] = 0;
		}
	}

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();
}

//=============================================================================

/*
================
Draw_BeginDisc

Draws the little blue disc in the corner of the screen.
Call before beginning any disc IO.
================
*/
void Draw_BeginDisc (void)
{

	D_BeginDirectRect (vid.width - 24, 0, draw_disc->data, 24, 24);
}


/*
================
Draw_EndDisc

Erases the disc icon.
Call after completing any disc IO
================
*/
void Draw_EndDisc (void)
{

	D_EndDirectRect (vid.width - 24, 0, 24, 24);
}


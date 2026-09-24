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
// r_tint.c -- coloured light for the software renderer
//
// id's renderer lights with gfx/colormap.lmp: 64 rows, one per light level,
// saying what each palette index becomes at that level. Light is a single
// number, so it can only make a colour darker or brighter.
//
// Maps built with modern tools, the re-release's among them, also carry the
// colour of their light (model.c reads it). Here that becomes more tables of
// the same shape, one per tint: "this palette index, at this light level, under
// this colour of light, is that index". A surface picks a table for each of
// its lightmap samples; a model picks one for the light where it stands. The
// rest of the renderer does not know the difference -- a table is a table.
//
// A tint is the light's colour with its brightness taken out, since the
// brightness is already in the level: red, green and blue as shares of their
// sum, rounded to fifteenths. That is 136 tints, and white -- a third each --
// is one of them, which uses id's colormap itself: a map without coloured
// light, or with white light, is drawn exactly as it always was.
//
// A table is built from id's colormap rather than from scratch, so the
// brightness curve is id's: what id's table says a texel becomes at a level,
// multiplied by the tint, as the nearest palette colour. id's table only ever
// produces 224 colours, so a tint is 224 nearest-colour searches and a 16K
// table, made the first time the tint is seen. Fullbright colours are left
// alone, as id's table leaves them.
//

#include "quakedef.h"
#include "r_local.h"

#define	TINT_STEPS		15
#define	TINT_COUNT		136		// (TINT_STEPS+1)(TINT_STEPS+2)/2
#define	TINT_WHITE_R	5		// a third each
#define	TINT_WHITE_G	5

cvar_t	r_rgblight = {"r_rgblight", "1", true};

static byte		*r_tintmaps[TINT_COUNT];
static byte		r_tintstore[TINT_COUNT][64*256];
static int		r_tintindex[TINT_STEPS+1][TINT_STEPS+1];
static int		r_tintr[TINT_COUNT], r_tintg[TINT_COUNT];
int				r_tintwhite;
static float	r_oldrgblight = -1;


/*
================
R_TintInit
================
*/
void R_TintInit (void)
{
	int		i, j, n;

	Cvar_RegisterVariable (&r_rgblight);

	n = 0;
	for (i=0 ; i<=TINT_STEPS ; i++)
		for (j=0 ; i+j<=TINT_STEPS ; j++)
		{
			r_tintindex[i][j] = n;
			r_tintr[n] = i;
			r_tintg[n] = j;
			n++;
		}
	r_tintwhite = r_tintindex[TINT_WHITE_R][TINT_WHITE_G];
	r_lightpointtint = r_tintwhite;
}


/*
================
R_TintIndex

The tint for a light of this colour, in any units.
================
*/
int R_TintIndex (int r, int g, int b)
{
	int		sum, i, j;

	sum = r + g + b;
	if (sum <= 0 || r_rgblight.value <= 0)
		return r_tintwhite;

// r_rgblight below 1 draws the colour toward grey
	if (r_rgblight.value < 1)
	{
		float	s = r_rgblight.value, m = sum / 3.0;

		r = m + (r - m) * s;
		g = m + (g - m) * s;
		b = m + (b - m) * s;
		sum = r + g + b;
	}

	i = (r * TINT_STEPS * 2 + sum) / (sum * 2);		// rounded
	j = (g * TINT_STEPS * 2 + sum) / (sum * 2);
	if (i + j > TINT_STEPS)
	{
	// rounding both up can overshoot; give the excess back from the smaller
		if (i > j)
			j = TINT_STEPS - i;
		else
			i = TINT_STEPS - j;
	}
	return r_tintindex[i][j];
}


/*
================
R_TintMap

The colormap for a tint, built the first time it is asked for.
================
*/
byte *R_TintMap (int tint)
{
	int		c, k, best, bestdist, dist, dr, dg, db;
	float	mr, mg, mb;
	byte	tinted[256], *pal = host_basepal, *out;
	int		r, g, b;

	if (tint == r_tintwhite || tint < 0 || tint >= TINT_COUNT)
		return vid.colormap;
	if (r_tintmaps[tint])
		return r_tintmaps[tint];

// the tint as a multiplier that averages 1, so it moves the hue, not the level
	mr = r_tintr[tint] * 3.0 / TINT_STEPS;
	mg = r_tintg[tint] * 3.0 / TINT_STEPS;
	mb = (TINT_STEPS - r_tintr[tint] - r_tintg[tint]) * 3.0 / TINT_STEPS;

	for (c=0 ; c<256 ; c++)
	{
		if (c >= 224)
		{
			tinted[c] = c;		// fullbright, and 255 the hole in a fence
			continue;
		}
		r = pal[c*3+0] * mr;	if (r > 255) r = 255;
		g = pal[c*3+1] * mg;	if (g > 255) g = 255;
		b = pal[c*3+2] * mb;	if (b > 255) b = 255;

		best = 0;
		bestdist = 0x7FFFFFFF;
		for (k=0 ; k<224 ; k++)
		{
			dr = r - pal[k*3+0];
			dg = g - pal[k*3+1];
			db = b - pal[k*3+2];
			dist = dr*dr + dg*dg + db*db;
			if (dist < bestdist)
			{
				bestdist = dist;
				best = k;
			}
		}
		tinted[c] = best;
	}

	out = r_tintstore[tint];
	for (c=0 ; c<64*256 ; c++)
		out[c] = tinted[vid.colormap[c]];

	r_tintmaps[tint] = out;
	return out;
}


/*
================
R_TintFrame

Once a frame: turning r_rgblight on or off changes every lit surface, and
those are cached, so the cache goes.
================
*/
void R_TintFrame (void)
{
	if (r_rgblight.value != r_oldrgblight)
	{
		r_oldrgblight = r_rgblight.value;
		D_FlushCaches ();
	}
}

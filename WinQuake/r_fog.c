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
// r_fog.c -- distance fog for the software renderer
//
// Fog arrived with GLQuake, where it is a blend the hardware does per pixel
// between the fragment's colour and the fog colour. This renderer has no
// colours to blend: it writes palette indices, and the only way to darken or
// tint one has always been to look it up in a table that says what some other
// index looks like. That is exactly what gfx/colormap.lmp is -- 64 rows of
// "this index at this light level is that index".
//
// So fog here is another such table, built the same way and used the same way.
// FOG_LEVELS rows, one per depth band, each row saying what every palette
// index becomes when blended that far toward the fog colour. Applying it costs
// one indexed byte load per pixel, which is the same thing the light table
// already costs, and nothing at all when a map sets no fog.
//
// What it is not is per-pixel. Depth comes from the 1/z the span drawers
// already carry and is sampled where they already recompute it -- every eight
// pixels for a world span, per pixel for an alias model, since the model
// drawer carries z for the z-buffer anyway. Eight pixels of a wall is a few
// world units at the distances fog is visible over, and banding at that scale
// is below what the palette can express in the first place.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

//
// Set by the "fog" command in r_main.c. Density is the GL sense: the fraction
// blended in at distance d is 1 - exp(-density * d), so 0.05 is thick enough
// to lose a room and 0.002 is a haze on the horizon.
//
extern float	r_fogdensity;
extern float	r_fogcolor[3];

qboolean	r_fogenabled;			// tested in the span drawers' hot path
byte		r_fogmap[FOG_LEVELS][256];

//
// Depth to fog level, so the drawers do not call exp() every eight pixels.
// One entry per FOG_DIST_UNIT world units; past the end everything is the
// densest level, which is what exp() converges to anyway.
//
byte		r_fogdistmap[FOG_DIST_ENTRIES];

static float	fog_builtdensity = -1;
static float	fog_builtcolor[3];


/*
================
R_FogPaletteIndex

The nearest palette entry to an rgb triple, by squared distance in rgb. Slow
and only ever called while building the table.
================
*/
static int R_FogPaletteIndex (byte *pal, int r, int g, int b)
{
	int		i, best, bestdist, dist, dr, dg, db;

	best = 0;
	bestdist = 0x7FFFFFFF;

	for (i=0 ; i<256 ; i++)
	{
	//
	// index 255 is transparent in sprites and alias skins, and handing it back
	// as the nearest colour to some fogged brown would punch holes in things.
	//
		if (i == 255)
			continue;

		dr = r - pal[i*3+0];
		dg = g - pal[i*3+1];
		db = b - pal[i*3+2];

		dist = dr*dr + dg*dg + db*db;

		if (dist < bestdist)
		{
			bestdist = dist;
			best = i;

			if (!dist)
				break;
		}
	}

	return best;
}


/*
================
R_BuildFogMap

Rebuilds both tables. Called when the fog settings change and when a map
loads, never per frame: it is 256 nearest-colour searches per level.
================
*/
void R_BuildFogMap (void)
{
	int		level, i;
	float	frac, d;
	byte	*pal;
	int		r, g, b;
	int		fr, fg, fb;

	r_fogenabled = (r_fogdensity > 0);

	if (!r_fogenabled)
		return;

// nothing to do if it is the same fog we already built for
	if (r_fogdensity == fog_builtdensity
		&& r_fogcolor[0] == fog_builtcolor[0]
		&& r_fogcolor[1] == fog_builtcolor[1]
		&& r_fogcolor[2] == fog_builtcolor[2])
		return;

	pal = host_basepal;
	if (!pal)
	{
		r_fogenabled = false;
		return;
	}

	fr = (int)(r_fogcolor[0] * 255); if (fr < 0) fr = 0; if (fr > 255) fr = 255;
	fg = (int)(r_fogcolor[1] * 255); if (fg < 0) fg = 0; if (fg > 255) fg = 255;
	fb = (int)(r_fogcolor[2] * 255); if (fb < 0) fb = 0; if (fb > 255) fb = 255;

	for (level=0 ; level<FOG_LEVELS ; level++)
	{
		frac = (float)level / (FOG_LEVELS - 1);

		for (i=0 ; i<256 ; i++)
		{
			r = (int)(pal[i*3+0] * (1 - frac) + fr * frac);
			g = (int)(pal[i*3+1] * (1 - frac) + fg * frac);
			b = (int)(pal[i*3+2] * (1 - frac) + fb * frac);

			r_fogmap[level][i] = R_FogPaletteIndex (pal, r, g, b);
		}
	}

//
// 255 is the skin and sprite transparency index and has to stay itself at
// every level, or a fogged monster grows a fogged outline where its edges
// should have been see-through.
//
	for (level=0 ; level<FOG_LEVELS ; level++)
		r_fogmap[level][255] = 255;

	for (i=0 ; i<FOG_DIST_ENTRIES ; i++)
	{
		d = (float)i * FOG_DIST_UNIT;
		frac = 1 - exp (-r_fogdensity * d);

		if (frac < 0)
			frac = 0;
		if (frac > 1)
			frac = 1;

		r_fogdistmap[i] = (byte)(frac * (FOG_LEVELS - 1) + 0.5);
	}

	fog_builtdensity = r_fogdensity;
	fog_builtcolor[0] = r_fogcolor[0];
	fog_builtcolor[1] = r_fogcolor[1];
	fog_builtcolor[2] = r_fogcolor[2];
}


/*
================
R_FogClear

A map that sets no fog must not inherit the last one's. Called from R_NewMap
before the level's own fog command, if it has one, runs.
================
*/
void R_FogClear (void)
{
	r_fogdensity = 0;
	r_fogcolor[0] = r_fogcolor[1] = r_fogcolor[2] = 0.5;
	r_fogenabled = false;
	fog_builtdensity = -1;
}

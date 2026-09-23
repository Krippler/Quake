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
// r_alpha.c -- translucent models and sprites for the software renderer
//
// The re-release's progs give entities an .alpha: MG1's gas flares are a blue
// flame drawn twice at 0.6 and 0.4, and walls, ghosts and runes fade in and
// out. GLQuake engines blend in hardware. This renderer writes palette indices,
// so a blend is a table, as light and fog already are here: for a given alpha,
// 256 rows of 256, saying which palette colour is nearest to "this colour over
// that one". A translucent pixel costs one more byte load than an opaque one.
//
// Alpha is rounded to eighths, which is finer than the palette can show the
// difference between. A table is 64K and built the first time its alpha is
// drawn, from a 32x32x32 cube of nearest colours built once, so building one
// costs a fraction of a frame rather than 16 million colour comparisons.
//

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#define	ALPHA_LEVELS	8		// tables for 1/8 .. 7/8; 8/8 is opaque

byte		*d_blendmap;		// the table the drawers blend through, or NULL

static byte		r_blendmaps[ALPHA_LEVELS][256*256];
static qboolean	r_blendbuilt[ALPHA_LEVELS];
static byte		r_inversecube[32*32*32];
static qboolean	r_inversebuilt;

// what R_BlendMap returns for an entity too faint to draw at all
byte		r_blendinvisible[1];


/*
================
R_BuildInverseCube

The nearest palette colour to the middle of every 8x8x8 cell of rgb. Index
255 is left out: it is the transparent colour in skins and sprites, and a
blend that produced it would punch a hole.
================
*/
static void R_BuildInverseCube (void)
{
	int		r, g, b, i, best, bestdist, dist, dr, dg, db;
	byte	*pal = host_basepal;

	for (r=0 ; r<32 ; r++)
		for (g=0 ; g<32 ; g++)
			for (b=0 ; b<32 ; b++)
			{
				best = 0;
				bestdist = 0x7FFFFFFF;
				for (i=0 ; i<255 ; i++)
				{
					dr = (r<<3) + 4 - pal[i*3+0];
					dg = (g<<3) + 4 - pal[i*3+1];
					db = (b<<3) + 4 - pal[i*3+2];
					dist = dr*dr + dg*dg + db*db;
					if (dist < bestdist)
					{
						bestdist = dist;
						best = i;
					}
				}
				r_inversecube[(r<<10) | (g<<5) | b] = best;
			}

	r_inversebuilt = true;
}


/*
================
R_BuildBlendMap

table[src*256 + dst]: src drawn at level/8 over dst.
================
*/
static void R_BuildBlendMap (int level)
{
	int		src, dst, r, g, b;
	int		a = level * 32, na = 256 - level * 32;	// weights out of 256
	byte	*pal = host_basepal;
	byte	*out = r_blendmaps[level];

	if (!r_inversebuilt)
		R_BuildInverseCube ();

	for (src=0 ; src<256 ; src++)
		for (dst=0 ; dst<256 ; dst++)
		{
			r = (pal[src*3+0] * a + pal[dst*3+0] * na) >> 8;
			g = (pal[src*3+1] * a + pal[dst*3+1] * na) >> 8;
			b = (pal[src*3+2] * a + pal[dst*3+2] * na) >> 8;
			out[(src<<8) | dst] = r_inversecube[((r>>3)<<10) | ((g>>3)<<5) | (b>>3)];
		}

	r_blendbuilt[level] = true;
}


/*
================
R_BlendMap

The table for an entity's encoded alpha: NULL to draw it opaque, as id's
entities always are, or r_blendinvisible to not draw it at all.
================
*/
byte *R_BlendMap (int alpha)
{
	int		level;

	if (alpha == ENTALPHA_DEFAULT || !host_basepal)
		return NULL;

	level = (int)(ENTALPHA_DECODE(alpha) * ALPHA_LEVELS + 0.5);
	if (level >= ALPHA_LEVELS)
		return NULL;
	if (level <= 0)
		return r_blendinvisible;

	if (!r_blendbuilt[level])
		R_BuildBlendMap (level);
	return r_blendmaps[level];
}

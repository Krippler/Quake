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
// r_surf.c: surface-related refresh code

#include "quakedef.h"
#include "r_local.h"

drawsurf_t	r_drawsurf;

int				lightleft, sourcesstep, blocksize, sourcetstep;
int				lightdelta, lightdeltastep;
int				lightright, lightleftstep, lightrightstep, blockdivshift;
unsigned		blockdivmask;
void			*prowdestbase;
unsigned char	*pbasesource;
int				surfrowbytes;	// used by ASM files
unsigned		*r_lightptr;
int				r_stepback;
int				r_lightwidth;
int				r_numhblocks, r_numvblocks;
unsigned char	*r_source, *r_sourcemax;

void R_DrawSurfaceBlock8_mip0 (void);
void R_DrawSurfaceBlock8_mip1 (void);
void R_DrawSurfaceBlock8_mip2 (void);
void R_DrawSurfaceBlock8_mip3 (void);
void R_DrawSurfaceBlock8_tint (void);

static void	(*surfmiptable[4])(void) = {
	R_DrawSurfaceBlock8_mip0,
	R_DrawSurfaceBlock8_mip1,
	R_DrawSurfaceBlock8_mip2,
	R_DrawSurfaceBlock8_mip3
};



unsigned		blocklights[18*18];

/*
===============
R_EntityTurning, R_TurningLight

A brush model's lightmaps were baked with it at angle zero. Rotated, each face
carries the light and shadow of where it was, not where it is: a fan's blades
were lit on one side and shadowed on the other when the map was built, and
spinning they trade places every few frames, bright and dark -- a flicker that
is not in the map. So a brush model that has been seen at any other angle is
lit, every face of it, with the light of its lit side: the average of those of
its samples brighter than its overall average, grey and colour. The shadows a
fan throws cannot come back this way; the flicker at least goes.

It stays that way for the rest of the map (entity_t.turned, cleared with the
entities on a map change). 1.15.1 asked instead whether the last two angle
updates differed, and a slow fan -- angles go as whole 1.4-degree steps --
often sends the same one twice: its baked light came back for a fraction of a
second at a time, which was the flicker again.
===============
*/
qboolean	r_surfturning;

qboolean R_EntityTurning (entity_t *ent)
{
	if (!ent || ent == &cl_entities[0] || !ent->model
		|| ent->model->type != mod_brush)
		return false;
	if (ent->angles[0] || ent->angles[1] || ent->angles[2])
		ent->turned = true;
	return ent->turned;
}

static void R_TurningLight (model_t *m, unsigned *mono, int *tint)
{
	static model_t	*lastmodel;
	static int		lastframe = -1;
	static unsigned	lastmono;
	static int		lasttint;
	msurface_t		*s;
	int				i, j, maps, size, count;
	double			sum, r, g, b;
	byte			*lm, *rgb;

	if (m == lastmodel && r_framecount == lastframe)
	{
		*mono = lastmono;
		*tint = lasttint;
		return;
	}

// twice over the samples: the average, then the average of those above it
	{
	double	mean = 0;
	int		pass;

	for (pass=0 ; pass<2 ; pass++)
	{
	sum = r = g = b = 0;
	count = 0;
	s = m->surfaces + m->firstmodelsurface;
	for (i=0 ; i<m->nummodelsurfaces ; i++, s++)
	{
		if (!s->samples)
			continue;
		size = ((s->extents[0]>>4)+1) * ((s->extents[1]>>4)+1);
		for (j=0 ; j<size ; j++)
		{
			double	v = 0, sr = 0, sg = 0, sb = 0;

			lm = s->samples + j;
			rgb = s->rgbsamples ? s->rgbsamples + j*3 : NULL;
			for (maps = 0 ; maps < MAXLIGHTMAPS && s->styles[maps] != 255 ;
				 maps++, lm += size)
			{
				unsigned	scale = d_lightstylevalue[s->styles[maps]];

				v += *lm * scale;
				if (rgb)
				{
					sr += rgb[0] * scale;
					sg += rgb[1] * scale;
					sb += rgb[2] * scale;
					rgb += size*3;
				}
			}
			if (pass == 1 && v < mean)
				continue;
			sum += v;
			r += sr;
			g += sg;
			b += sb;
			count++;
		}
	}
	mean = count ? sum / count : 0;
	}
	}

	lastmono = count ? (unsigned)(sum / count) : 0;
	lasttint = (r + g + b > 0 && r_rgblight.value > 0)
		? R_TintIndex ((int)(r / 256), (int)(g / 256), (int)(b / 256))
		: r_tintwhite;
	lastmodel = m;
	lastframe = r_framecount;
	*mono = lastmono;
	*tint = lasttint;
}

// coloured light (r_tint.c): the tint of each lightmap sample, when the surface
// has one, and where the block drawer is up to in it
static byte		blocktints[18*18];
static qboolean	r_surftinted;
static byte		*r_tintptr;

/*
===============
R_AddDynamicLights
===============
*/
/*
===============
R_DlightOrigin

Where a dynamic light is in the space the current surface's model was built
in. For the world that is where it is. A brush model's surfaces are stored
where the map compiler left them -- for one built to rotate, around its own
centre near the world's origin -- and the entity's origin and angles carry
it to where it is drawn. id lit brush models with the light's world position
all the same: a door that had slid 64 units was lit as if it had not moved,
and a fan spinning at the far end of a map was lit by whatever flashed near
the middle of it, on and off, blade by blade. So the light is brought into
the model's space the way the view is, by R_RotateBmodel's matrix, which is
the current entity's whenever its surfaces are being cached.
===============
*/
void R_DlightOrigin (dlight_t *dl, vec3_t out)
{
	VectorCopy (dl->origin, out);
	if (currententity && currententity != &cl_entities[0]
		&& currententity->model && currententity->model->type == mod_brush)
	{
		VectorSubtract (out, currententity->origin, out);
		R_EntityRotate (out);
	}
}

void R_AddDynamicLights (void)
{
	msurface_t *surf;
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local, origin;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !(surf->dlightbits & (1<<lnum) ) )
			continue;		// not lit by this light

		R_DlightOrigin (&cl_dlights[lnum], origin);
		rad = cl_dlights[lnum].radius;
		dist = DotProduct (origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];
		
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
#ifdef QUAKE2
				{
					unsigned temp;
					temp = (rad - dist)*256;
					i = t*smax + s;
					if (!cl_dlights[lnum].dark)
						blocklights[i] += temp;
					else
					{
						if (blocklights[i] > temp)
							blocklights[i] -= temp;
						else
							blocklights[i] = 0;
					}
				}
#else
					blocklights[t*smax + s] += (rad - dist)*256;
#endif
			}
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (void)
{
	int			smax, tmax;
	int			t;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;

	surf = r_drawsurf.surf;
	r_surftinted = false;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	if (r_fullbright.value || !cl.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 0;
		return;
	}

// clear to ambient
	for (i=0 ; i<size ; i++)
		blocklights[i] = r_refdef.ambientlight<<8;


// a turning brush model: its average, everywhere
	if (r_surfturning && lightmap)
	{
		unsigned	mono;
		int			tint;

		R_TurningLight (currententity->model, &mono, &tint);
		for (i=0 ; i<size ; i++)
		{
			blocklights[i] += mono;
			blocktints[i] = tint;
		}
		r_surftinted = tint != r_tintwhite;
		lightmap = NULL;		// the per-sample passes below are skipped
	}

// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fraction		
			for (i=0 ; i<size ; i++)
				blocklights[i] += lightmap[i] * scale;
			lightmap += size;	// skip to next lightmap
		}

// the colour of that light, if the map has it: a tint per sample
	if (surf->rgbsamples && r_rgblight.value > 0 && !r_surfturning)
	{
		byte	*rgb;
		int		r, g, b;

		for (i=0 ; i<size ; i++)
		{
			r = g = b = 0;
			rgb = surf->rgbsamples + i*3;
			for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
				 maps++, rgb += size*3)
			{
				scale = r_drawsurf.lightadj[maps];
				r += rgb[0] * scale;
				g += rgb[1] * scale;
				b += rgb[2] * scale;
			}
			blocktints[i] = R_TintIndex (r, g, b);
			if (blocktints[i] != r_tintwhite)
				r_surftinted = true;
		}
	}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights ();

// bound, invert, and shift
	for (i=0 ; i<size ; i++)
	{
		t = (255*256 - (int)blocklights[i]) >> (8 - VID_CBITS);

		if (t < (1 << 6))
			t = (1 << 6);

		blocklights[i] = t;
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	int		reletive;
	int		count;

	if (currententity->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
===============
R_DrawSurface
===============
*/
void R_DrawSurface (void)
{
	unsigned char	*basetptr;
	int				smax, tmax, twidth;
	int				u;
	int				soffset, basetoffset, texwidth;
	int				horzblockstep;
	unsigned char	*pcolumndest;
	void			(*pblockdrawer)(void);
	texture_t		*mt;

// calculate the lightings
	R_BuildLightMap ();
	
	surfrowbytes = r_drawsurf.rowbytes;

	mt = r_drawsurf.texture;
	
	r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];
	
// the fractional light values should range from 0 to (VID_GRADES - 1) << 16
// from a source range of 0 - 255
	
	texwidth = mt->width >> r_drawsurf.surfmip;

	blocksize = 16 >> r_drawsurf.surfmip;
	blockdivshift = 4 - r_drawsurf.surfmip;
	blockdivmask = (1 << blockdivshift) - 1;
	
	r_lightwidth = (r_drawsurf.surf->extents[0]>>4)+1;

	r_numhblocks = r_drawsurf.surfwidth >> blockdivshift;
	r_numvblocks = r_drawsurf.surfheight >> blockdivshift;

//==============================

	if (r_surftinted)
	{
		pblockdrawer = R_DrawSurfaceBlock8_tint;
		horzblockstep = blocksize;
	}
	else if (r_pixbytes == 1)
	{
		pblockdrawer = surfmiptable[r_drawsurf.surfmip];
	// TODO: only needs to be set when there is a display settings change
		horzblockstep = blocksize;
	}
	else
	{
		pblockdrawer = R_DrawSurfaceBlock16;
	// TODO: only needs to be set when there is a display settings change
		horzblockstep = blocksize << 1;
	}

	smax = mt->width >> r_drawsurf.surfmip;
	twidth = texwidth;
	tmax = mt->height >> r_drawsurf.surfmip;
	sourcetstep = texwidth;
	r_stepback = tmax * twidth;

	r_sourcemax = r_source + (tmax * smax);

	soffset = r_drawsurf.surf->texturemins[0];
	basetoffset = r_drawsurf.surf->texturemins[1];

// << 16 components are to guarantee positive values for %
	soffset = ((soffset >> r_drawsurf.surfmip) + (smax << 16)) % smax;
	basetptr = &r_source[((((basetoffset >> r_drawsurf.surfmip) 
		+ (tmax << 16)) % tmax) * twidth)];

	pcolumndest = r_drawsurf.surfdat;

	for (u=0 ; u<r_numhblocks; u++)
	{
		r_lightptr = blocklights + u;
		r_tintptr = blocktints + u;

		prowdestbase = pcolumndest;

		pbasesource = basetptr + soffset;

		(*pblockdrawer)();

		soffset = soffset + blocksize;
		if (soffset >= smax)
			soffset = 0;

		pcolumndest += horzblockstep;
	}
}


//=============================================================================

/*
================
R_DrawSurfaceBlock8_tint

id's block drawer, for a surface lit in colour: any mip level, and a colormap
per texel instead of one. Each 16x16 block has a tint at each corner, the
colour of the lightmap sample there. A block whose corners agree -- almost all
of them -- is drawn with that one table as id's drawer would. Where they
differ, each texel takes a corner's table by an ordered dither weighted by how
near it is to that corner, so the colour grades across the block the way the
brightness does rather than stepping in quarters.
================
*/
static const byte r_tintdither[4][4] =
{
	{ 0,  8,  2, 10},
	{12,  4, 14,  6},
	{ 3, 11,  1,  9},
	{15,  7, 13,  5}
};

static int		r_tintselbs;
static byte		r_tintsel[16][16];	// which corner each texel of a block takes

void R_DrawSurfaceBlock8_tint (void)
{
	int				v, i, b, bs, sh, lightstep, lighttemp, light;
	int				right, below;
	unsigned char	*psource, *prowdest;
	byte			*maps[4];

	bs = blocksize;
	sh = blockdivshift;

// texel centre past a dither threshold: that corner's side. The same for
// every block of a mip level, so worked out once per level.
	if (bs != r_tintselbs)
	{
		for (i=0 ; i<bs ; i++)
			for (b=0 ; b<bs ; b++)
			{
				right = (2*b + 1) * 16 > (2*r_tintdither[i&3][b&3] + 1) * bs;
				below = (2*i + 1) * 16 > (2*r_tintdither[b&3][(i+2)&3] + 1) * bs;
				r_tintsel[i][b] = (below<<1) | right;
			}
		r_tintselbs = bs;
	}
	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		maps[0] = R_TintMap (r_tintptr[0]);
		maps[1] = R_TintMap (r_tintptr[1]);
		r_lightptr += r_lightwidth;
		r_tintptr += r_lightwidth;
		maps[2] = R_TintMap (r_tintptr[0]);
		maps[3] = R_TintMap (r_tintptr[1]);
		lightleftstep = (r_lightptr[0] - lightleft) >> sh;
		lightrightstep = (r_lightptr[1] - lightright) >> sh;

		for (i=0 ; i<bs ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> sh;

			light = lightright;

			if (maps[0] == maps[1] && maps[0] == maps[2] && maps[0] == maps[3])
			{
				byte	*cm = maps[0];

				for (b=bs-1 ; b>=0 ; b--)
				{
					prowdest[b] = cm[(light & 0xFF00) + psource[b]];
					light += lightstep;
				}
			}
			else
			{
				byte	*sel = r_tintsel[i];

				for (b=bs-1 ; b>=0 ; b--)
				{
					prowdest[b] = maps[sel[b]][(light & 0xFF00) + psource[b]];
					light += lightstep;
				}
			}

			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}

#if	!id386

/*
================
R_DrawSurfaceBlock8_mip0
================
*/
void R_DrawSurfaceBlock8_mip0 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 4;
		lightrightstep = (r_lightptr[1] - lightright) >> 4;

		for (i=0 ; i<16 ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 4;

			light = lightright;

			for (b=15; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip1
================
*/
void R_DrawSurfaceBlock8_mip1 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 3;
		lightrightstep = (r_lightptr[1] - lightright) >> 3;

		for (i=0 ; i<8 ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 3;

			light = lightright;

			for (b=7; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip2
================
*/
void R_DrawSurfaceBlock8_mip2 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 2;
		lightrightstep = (r_lightptr[1] - lightright) >> 2;

		for (i=0 ; i<4 ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 2;

			light = lightright;

			for (b=3; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip3
================
*/
void R_DrawSurfaceBlock8_mip3 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 1;
		lightrightstep = (r_lightptr[1] - lightright) >> 1;

		for (i=0 ; i<2 ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 1;

			light = lightright;

			for (b=1; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock16

FIXME: make this work
================
*/
void R_DrawSurfaceBlock16 (void)
{
	int				k;
	unsigned char	*psource;
	int				lighttemp, lightstep, light;
	unsigned short	*prowdest;

	prowdest = (unsigned short *)prowdestbase;

	for (k=0 ; k<blocksize ; k++)
	{
		unsigned short	*pdest;
		unsigned char	pix;
		int				b;

		psource = pbasesource;
		lighttemp = lightright - lightleft;
		lightstep = lighttemp >> blockdivshift;

		light = lightleft;
		pdest = prowdest;

		for (b=0; b<blocksize; b++)
		{
			pix = *psource;
			*pdest = vid.colormap16[(light & 0xFF00) + pix];
			psource += sourcesstep;
			pdest++;
			light += lightstep;
		}

		pbasesource += sourcetstep;
		lightright += lightrightstep;
		lightleft += lightleftstep;
		prowdest = (unsigned short *)((long)prowdest + surfrowbytes);
	}

	prowdestbase = prowdest;
}

#endif


//============================================================================

/*
================
R_GenTurbTile
================
*/
void R_GenTurbTile (pixel_t *pbasetex, void *pdest)
{
	int		*turb;
	int		i, j, s, t;
	byte	*pd;
	
	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (byte *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = *(pbasetex + (t<<6) + s);
		}
	}
}


/*
================
R_GenTurbTile16
================
*/
void R_GenTurbTile16 (pixel_t *pbasetex, void *pdest)
{
	int				*turb;
	int				i, j, s, t;
	unsigned short	*pd;

	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (unsigned short *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = d_8to16table[*(pbasetex + (t<<6) + s)];
		}
	}
}


/*
================
R_GenTile
================
*/
void R_GenTile (msurface_t *psurf, void *pdest)
{
	if (psurf->flags & SURF_DRAWTURB)
	{
		if (r_pixbytes == 1)
		{
			R_GenTurbTile ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
		}
		else
		{
			R_GenTurbTile16 ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
		}
	}
	else if (psurf->flags & SURF_DRAWSKY)
	{
		if (r_pixbytes == 1)
		{
			R_GenSkyTile (pdest);
		}
		else
		{
			R_GenSkyTile16 (pdest);
		}
	}
	else
	{
		Sys_Error ("Unknown tile type");
	}
}


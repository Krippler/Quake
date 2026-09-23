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
// r_light.c

#include "quakedef.h"
#include "r_local.h"

int	r_dlightframecount;


/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int			i,j,k;
	
//
// light animations
// 'm' is normal light, 'a' is no light, 'z' is double bright
	i = (int)(cl.time*10);
	for (j=0 ; j<MAX_LIGHTSTYLES ; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256;
			continue;
		}
		k = i % cl_lightstyle[j].length;
		k = cl_lightstyle[j].map[k] - 'a';
		k = k*22;
		d_lightstylevalue[j] = k;
	}	

//
// A face can name any style up to 254, and only 0 to 63 can be set over the
// network. The rest were left at 0 -- no light at all -- so a face lit only by
// one was black. GLQuake gives them normal light, and so does an unset style
// below 64 just above; they get the same here.
//
	for ( ; j<255 ; j++)
		d_lightstylevalue[j] = 256;
}


/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights
=============
*/
void R_MarkLights (dlight_t *light, int bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;
	
	if (node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;
	
	if (dist > light->radius)
	{
		R_MarkLights (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->radius)
	{
		R_MarkLights (light, bit, node->children[1]);
		return;
	}
		
// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_dlightframecount;
		}
		surf->dlightbits |= bit;
	}

	R_MarkLights (light, bit, node->children[0]);
	R_MarkLights (light, bit, node->children[1]);
}


/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int		i;
	dlight_t	*l;

	r_dlightframecount = r_framecount + 1;	// because the count hasn't
											//  advanced yet for this frame
	l = cl_dlights;

	for (i=0 ; i<MAX_DLIGHTS ; i++, l++)
	{
		if (l->die < cl.time || !l->radius)
			continue;
		R_MarkLights ( l, 1<<i, cl.worldmodel->nodes );
	}
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

int RecursiveLightPoint (mnode_t *node, vec3_t start, vec3_t end)
{
	int			r;
	float		front, back, frac;
	int			side;
	mplane_t	*plane;
	vec3_t		mid;
	msurface_t	*surf;
	int			s, t, ds, dt;
	int			i;
	mtexinfo_t	*tex;
	byte		*lightmap;
	unsigned	scale;
	int			maps;

	if (node->contents < 0)
		return -1;		// didn't hit anything
	
// calculate mid point

// FIXME: optimize for axial
	plane = node->plane;
	front = DotProduct (start, plane->normal) - plane->dist;
	back = DotProduct (end, plane->normal) - plane->dist;
	side = front < 0;
	
	if ( (back < 0) == side)
		return RecursiveLightPoint (node->children[side], start, end);
	
	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;
	
// go down front side	
	r = RecursiveLightPoint (node->children[side], start, mid);
	if (r >= 0)
		return r;		// hit something
		
	if ( (back < 0) == side )
		return -1;		// didn't hit anuthing
		
// check for impact on this node

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->flags & SURF_DRAWTILED)
			continue;	// no lightmaps

		tex = surf->texinfo;
		
		s = DotProduct (mid, tex->vecs[0]) + tex->vecs[0][3];
		t = DotProduct (mid, tex->vecs[1]) + tex->vecs[1][3];;

		if (s < surf->texturemins[0] ||
		t < surf->texturemins[1])
			continue;
		
		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];
		
		if ( ds > surf->extents[0] || dt > surf->extents[1] )
			continue;

		if (!surf->samples)
			return 0;

		ds >>= 4;
		dt >>= 4;

		lightmap = surf->samples;
		r = 0;
		if (lightmap)
		{

			lightmap += dt * ((surf->extents[0]>>4)+1) + ds;

			for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
					maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				r += *lightmap * scale;
				lightmap += ((surf->extents[0]>>4)+1) *
						((surf->extents[1]>>4)+1);
			}
			
			r >>= 8;
		}
		
		return r;
	}

// go down back side
	return RecursiveLightPoint (node->children[!side], mid, end);
}

int R_LightPoint (vec3_t p)
{
	vec3_t		end;
	int			r;
	
	if (!cl.worldmodel->lightdata)
		return 255;
	
	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;
	
	r = RecursiveLightPoint (cl.worldmodel->nodes, p, end);
	
	if (r == -1)
		r = 0;

	if (r < r_refdef.ambientlight)
		r = r_refdef.ambientlight;

	return r;
}



/*
=============================================================================

WHAT IS UNDER THE CROSSHAIR

"surface" names the face the crosshair is on and says how it is drawn: its
texture and how dark that is, and its lighting. It is for reports: when part of
a map comes out wrong, this is what tells a missing face from a black texture
from a lightmap that says black.

=============================================================================
*/

// true if p, on the face's plane, is inside the face (faces are convex)
static qboolean R_PointInFace (msurface_t *surf, vec3_t p)
{
	int			i, lindex, sign;
	float		d;
	float		*v0, *v1;
	medge_t		*edge;
	vec3_t		dir, n, rel;

	if (surf->numedges < 3)
		return false;

	sign = 0;
	for (i=0 ; i<surf->numedges ; i++)
	{
		lindex = cl.worldmodel->surfedges[surf->firstedge + i];
		edge = &cl.worldmodel->edges[lindex > 0 ? lindex : -lindex];
		v0 = cl.worldmodel->vertexes[edge->v[lindex > 0 ? 0 : 1]].position;
		v1 = cl.worldmodel->vertexes[edge->v[lindex > 0 ? 1 : 0]].position;

		VectorSubtract (v1, v0, dir);
		CrossProduct (surf->plane->normal, dir, n);
		VectorSubtract (p, v0, rel);
		d = DotProduct (rel, n);

		if (d > 0.01)
		{
			if (sign < 0)
				return false;
			sign = 1;
		}
		else if (d < -0.01)
		{
			if (sign > 0)
				return false;
			sign = -1;
		}
	}
	return true;
}

// the first face the segment meets, front to back, and where
static msurface_t *R_ProbeNode (mnode_t *node, vec3_t start, vec3_t end,
								vec3_t hit)
{
	float		front, back, frac;
	int			i, side;
	vec3_t		mid;
	msurface_t	*surf;

	if (node->contents < 0)
		return NULL;

	front = DotProduct (start, node->plane->normal) - node->plane->dist;
	back = DotProduct (end, node->plane->normal) - node->plane->dist;
	side = front < 0;

	if ((back < 0) == side)
		return R_ProbeNode (node->children[side], start, end, hit);

	frac = R_SafeFrac (front / (front - back));
	for (i=0 ; i<3 ; i++)
		mid[i] = start[i] + (end[i] - start[i])*frac;

	surf = R_ProbeNode (node->children[side], start, mid, hit);
	if (surf)
		return surf;

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (R_PointInFace (surf, mid))
		{
			VectorCopy (mid, hit);
			return surf;
		}
	}

	return R_ProbeNode (node->children[!side], mid, end, hit);
}

// world space to a brush model's own, given its axes
static void R_ToModel (vec3_t p, entity_t *ent, vec3_t f, vec3_t r, vec3_t u,
					   vec3_t out)
{
	vec3_t		local;

	VectorSubtract (p, ent->origin, local);
	out[0] = DotProduct (local, f);
	out[1] = -DotProduct (local, r);
	out[2] = DotProduct (local, u);
}

void R_Surface_f (void)
{
	int			i, j, maps, total, black, bright, light;
	int			s, t, ds, dt, smax, tmax;
	float		dist, bestdist;
	vec3_t		forward, right, up, start, end, hit, besthit, local;
	vec3_t		lstart, lend, f, r, u, bestf, bestr, bestu;
	msurface_t	*surf, *best;
	entity_t	*ent, *bestent;
	model_t		*mod;
	texture_t	*tx;
	mtexinfo_t	*tex;
	byte		*pix, *lightmap;

	if (cls.state != ca_connected || !cl.worldmodel)
	{
		Con_Printf ("No map is running.\n");
		return;
	}

	AngleVectors (r_refdef.viewangles, forward, right, up);
	VectorCopy (r_refdef.vieworg, start);
	VectorMA (start, 8192, forward, end);

	bestent = NULL;
	bestdist = 1e30;
	best = R_ProbeNode (cl.worldmodel->nodes, start, end, besthit);
	if (best)
	{
		VectorSubtract (besthit, start, local);
		bestdist = Length (local);
	}

// doors, lifts and walls share the world's faces but have their own tree
	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		ent = cl_visedicts[i];
		mod = ent->model;
		if (!mod || mod->type != mod_brush || mod->name[0] != '*')
			continue;

		AngleVectors (ent->angles, f, r, u);
		R_ToModel (start, ent, f, r, u, lstart);
		R_ToModel (end, ent, f, r, u, lend);

		surf = R_ProbeNode (mod->nodes + mod->hulls[0].firstclipnode,
							lstart, lend, hit);
		if (!surf)
			continue;
		VectorSubtract (hit, lstart, local);
		dist = Length (local);
		if (dist < bestdist)
		{
			best = surf;
			bestent = ent;
			bestdist = dist;
			VectorMA (start, dist, forward, besthit);
			VectorCopy (f, bestf);
			VectorCopy (r, bestr);
			VectorCopy (u, bestu);
		}
	}

	if (!best)
	{
		Con_Printf ("No face under the crosshair within 8192 units. Anything "
					"drawn there\nis r_clearcolor (%g), the colour of no "
					"surface at all.\n", r_clearcolor.value);
		return;
	}

	tex = best->texinfo;
	tx = tex->texture;

	Con_Printf ("\n%s face %d, %.0f units away at (%.0f %.0f %.0f)\n",
				bestent ? bestent->model->name : cl.worldmodel->name,
				(int)(best - cl.worldmodel->surfaces), bestdist,
				besthit[0], besthit[1], besthit[2]);

	Con_Printf ("texture \"%s\", %dx%d%s%s%s%s\n", tx->name, tx->width,
				tx->height,
				(best->flags & SURF_DRAWSKY) ? ", sky" : "",
				(best->flags & SURF_DRAWTURB) ? ", liquid" : "",
				(best->flags & SURF_DRAWMASKED) ? ", fence" : "",
				tx->anim_total ? ", animated" : "");

// how dark the texture itself is
	pix = (byte *)tx + tx->offsets[0];
	total = tx->width * tx->height;
	black = bright = 0;
	for (i=0 ; i<total ; i++)
	{
		if (!pix[i])
			black++;
		bright += (host_basepal[pix[i]*3] + host_basepal[pix[i]*3+1] +
				   host_basepal[pix[i]*3+2]) / 3;
	}
	if (total)
		Con_Printf ("its pixels: %d%% palette 0 (black), average brightness "
					"%d of 255\n", black*100 / total, bright / total);

// how it is lit
	if (best->flags & SURF_DRAWTILED)
	{
		Con_Printf ("not lightmapped: sky and liquids are drawn unlit\n");
		return;
	}
	if (!cl.worldmodel->lightdata)
	{
		Con_Printf ("the map has no light data: drawn at full brightness\n");
		return;
	}
	if (!best->samples)
	{
		Con_Printf ("no lightmap: drawn at full brightness\n");
		return;
	}

// the texture axes are in the face's own model's space
	if (bestent)
		R_ToModel (besthit, bestent, bestf, bestr, bestu, local);
	else
		VectorCopy (besthit, local);
	s = DotProduct (local, tex->vecs[0]) + tex->vecs[0][3];
	t = DotProduct (local, tex->vecs[1]) + tex->vecs[1][3];

	smax = (best->extents[0]>>4)+1;
	tmax = (best->extents[1]>>4)+1;
	ds = (s - best->texturemins[0]) >> 4;
	dt = (t - best->texturemins[1]) >> 4;
	ds = ds < 0 ? 0 : ds >= smax ? smax - 1 : ds;
	dt = dt < 0 ? 0 : dt >= tmax ? tmax - 1 : dt;

	Con_Printf ("lightmap %dx%d, styles", smax, tmax);
	light = 0;
	lightmap = best->samples + dt*smax + ds;
	for (maps = 0 ; maps < MAXLIGHTMAPS && best->styles[maps] != 255 ; maps++)
	{
		j = best->styles[maps];
		Con_Printf (" %d (sample %d x %d)", j, *lightmap, d_lightstylevalue[j]);
		light += *lightmap * d_lightstylevalue[j];
		lightmap += smax*tmax;
	}
	if (!maps)
		Con_Printf (" none");
	Con_Printf ("\nlight here %d, where 0 is black and 255 is full\n",
				light >> 8);
}

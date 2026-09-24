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
// r_fence.c -- masked ("fence") world textures for the software renderer
//
// A texture whose name begins with '{' is a fence: every texel that is palette
// index 255 is a hole you see through. Grates, ladders, vines, chainlink. id
// never shipped one, and this renderer was written before the convention
// existed, so it drew index 255 as what it literally is -- palette entry 255,
// a flat dusty pink (159, 91, 83) -- and a grate came out as a solid pink
// sheet with the bars punched into it as dark shapes.
//
// The awkward part is not the holes. It is that this renderer decides
// visibility with an edge list: for each screen span, the nearest surface wins
// and everything behind it is never drawn at all. A fence that keeps its place
// in that list deletes the room behind it whether or not its pixels are drawn,
// so skipping index 255 on its own would leave the holes showing the previous
// frame.
//
// So a fence is taken out of the edge list entirely -- the room behind it is
// then drawn as if the fence were not there -- and the fence itself is drawn
// afterwards, over the finished picture, against the z-buffer the world has
// just written. That is the same order, and very nearly the same drawing code,
// that sprites have always used here: D_SpriteDrawSpans already skips index
// 255, tests and writes z per pixel, and applies fog. It wants spans and a
// lit texture block, and a world surface can hand it both.
//
// What that buys, beyond the holes: a fence is correctly hidden by anything in
// front of it, correctly hides anything behind it, is lit by its own lightmap
// and dynamic lights like any other surface, and fogs with the rest of the
// scene.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

//
// Deferred from R_RenderFace, drawn by R_DrawFenceFaces once the world and the
// brush models have finished writing colour and z.
//
typedef struct
{
	msurface_t	*face;
	int			clipflags;
	entity_t	*entity;		// NULL for the world
} fenceface_t;

#define	MAX_FENCE_FACES		4096

static fenceface_t	fencefaces[MAX_FENCE_FACES];
static int			numfencefaces;

static qboolean		reported_full;
static qboolean		reported_complex;

//
// R_RenderPoly clips into fixed 100-vertex working buffers, with one of id's
// own FIXMEs where the guard should be. Clipping can add a vertex per plane,
// so a face is only safe well under that.
//
#define	MAX_FENCE_VERTS		90

//
// One span per scan line of the polygon, in the form D_SpriteDrawSpans wants.
//
static sspan_t		fence_spans[MAXHEIGHT + 1];


/*
================
R_FenceClearFrame
================
*/
void R_FenceClearFrame (void)
{
	numfencefaces = 0;
}


/*
================
R_DeferFace

Whether a face goes to this pass rather than the edge list: a fence, or any
face of a brush model the progs have made translucent (r_alpha.c), which has
to leave the edge list for the same reason a fence does -- it must not hide
what is behind it -- and is blended instead of holed when it is drawn. Sky and
liquids are drawn by their own span drawers and stay where they are.
================
*/
qboolean R_DeferFace (msurface_t *fa)
{
	if (fa->flags & SURF_DRAWMASKED)
		return true;
	if (!insubmodel || currententity->alpha == ENTALPHA_DEFAULT)
		return false;
	if (fa->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
		return false;
	return R_BlendMap (currententity->alpha) != NULL;
}


/*
================
R_FenceDeferFace

Called from R_RenderFace. Returns true if the face has been taken over, in
which case the caller must not put it in the edge list; false means draw it
the ordinary way, holes and all, which is what happens if there are more
fences in one frame than this can hold.
================
*/
qboolean R_FenceDeferFace (msurface_t *fa, int clipflags)
{
	if (numfencefaces >= MAX_FENCE_FACES)
	{
		if (!reported_full)
		{
			reported_full = true;
			Con_Printf ("\nMore than %d masked (fence) surfaces are visible at "
						"once.\nThe rest are drawn without their holes.\n",
						MAX_FENCE_FACES);
		}
		return false;
	}

	if (fa->numedges > MAX_FENCE_VERTS)
	{
		if (!reported_complex)
		{
			reported_complex = true;
			Con_Printf ("\nA masked (fence) surface has %d edges, more than "
						"the %d the\nclipper can hold; it is drawn without "
						"its holes.\n", fa->numedges, MAX_FENCE_VERTS);
		}
		return false;
	}

	fencefaces[numfencefaces].face = fa;
	fencefaces[numfencefaces].clipflags = clipflags;
	fencefaces[numfencefaces].entity = insubmodel ? currententity : NULL;
	numfencefaces++;

	return true;
}


/*
================
D_ScanFencePoly

Turns the projected polygon R_RenderPoly left in r_polydesc into one span per
scan line it covers. The polygon is convex, but which way round its vertices
run depends on which way the surface faces, so this takes the cheap
winding-independent route: for each scan line, intersect every edge that
crosses it and keep the leftmost and rightmost crossing. Fence polygons are a
handful of vertices, and this runs once per fence per frame.

Returns the number of spans, or 0 if the polygon covers no whole scan line.
================
*/
static int D_ScanFencePoly (void)
{
	int			i, v, top, bottom, numspans;
	float		vmin, vmax, u, ul, ur;
	polyvert_t	*pv, *pnext;

	vmin = 999999;
	vmax = -999999;

	for (i=0 ; i<r_polydesc.numverts ; i++)
	{
		if (r_polydesc.pverts[i].v < vmin)
			vmin = r_polydesc.pverts[i].v;
		if (r_polydesc.pverts[i].v > vmax)
			vmax = r_polydesc.pverts[i].v;
	}

	top = (int)ceil (vmin);
	bottom = (int)ceil (vmax);

	if (top < r_refdef.vrect.y)
		top = r_refdef.vrect.y;
	if (bottom > r_refdef.vrectbottom)
		bottom = r_refdef.vrectbottom;

	if (top >= bottom)
		return 0;

	numspans = 0;

	for (v=top ; v<bottom ; v++)
	{
		float	fv = (float)v;

		ul = 999999;
		ur = -999999;

		for (i=0 ; i<r_polydesc.numverts ; i++)
		{
			pv = &r_polydesc.pverts[i];
			pnext = &r_polydesc.pverts[(i + 1) % r_polydesc.numverts];

		//
		// half-open in v, so a vertex shared by two edges is counted once and
		// a horizontal edge not at all
		//
			if ((pv->v <= fv && pnext->v > fv)
				|| (pnext->v <= fv && pv->v > fv))
			{
				u = pv->u + (pnext->u - pv->u)
						* ((fv - pv->v) / (pnext->v - pv->v));

				if (u < ul)
					ul = u;
				if (u > ur)
					ur = u;
			}
		}

		if (ul > ur)
			continue;				// this scan line misses the polygon

		if (ul < r_refdef.fvrectx_adj)
			ul = r_refdef.fvrectx_adj;
		if (ur > r_refdef.fvrectright_adj)
			ur = r_refdef.fvrectright_adj;

		fence_spans[numspans].u = (int)ceil (ul);
		fence_spans[numspans].v = v;
		fence_spans[numspans].count = (int)ceil (ur) - fence_spans[numspans].u;

		if (fence_spans[numspans].count > 0)
			numspans++;
	}

	return numspans;
}


/*
================
D_DrawPoly

id left this empty -- "this driver takes spans, not polygons" -- as the hook
for a polygon driver that never shipped. The fence pass is the one caller that
wants a single world surface drawn on its own, outside the edge list, so this
is where that happens: mip and light it exactly as D_DrawSurfaces would, work
out the same 1/z gradients R_RenderFace stores on a surf_t, then hand the
scan-converted spans to the sprite drawer.
================
*/
void D_DrawPoly (void)
{
	msurface_t		*fa;
	surfcache_t		*pcurrentcache;
	mplane_t		*pplane;
	vec3_t			p_normal;
	float			distinv;
	int				numspans;

	fa = r_polydesc.pcurrentface;

	if (r_polydesc.numverts < 3)
		return;

	numspans = D_ScanFencePoly ();

	if (!numspans)
		return;

	fence_spans[numspans].count = DS_SPAN_LIST_END;

	miplevel = D_MipLevelForScale (r_polydesc.nearzi * scale_for_mip
			* fa->texinfo->mipadjust);

	pcurrentcache = D_CacheSurface (fa, miplevel);

	cacheblock = (pixel_t *)pcurrentcache->data;
	cachewidth = pcurrentcache->width;

	D_CalcGradients (fa);

//
// D_CalcGradients does the texture gradients but not 1/z, which the span path
// takes from the surf_t R_RenderFace filled in. This is that same arithmetic.
//
	pplane = fa->plane;
	TransformVector (pplane->normal, p_normal);
	distinv = 1.0 / (pplane->dist - DotProduct (modelorg, pplane->normal));

	d_zistepu = p_normal[0] * xscaleinv * distinv;
	d_zistepv = -p_normal[1] * yscaleinv * distinv;
	d_ziorigin = p_normal[2] * distinv - xcenter * d_zistepu
			- ycenter * d_zistepv;

	D_SpriteDrawSpans (fence_spans);
}


/*
================
R_DrawFenceFaces

Called once the world and the brush models have written the frame, for the
opaque faces, and again from R_DrawTranslucentEntities for the faces of
translucent brush models, blended over everything else. Each face goes
through R_RenderPoly, which frustum-clips and projects it and then calls
D_DrawPoly above.
================
*/
void R_DrawFenceFaces (qboolean translucent)
{
	int			i;
	entity_t	*saveentity;
	vec3_t		saveorigin;
	byte		*blend;

	if (!numfencefaces)
		return;

	saveentity = currententity;
	VectorCopy (modelorg, saveorigin);

	for (i=0 ; i<numfencefaces ; i++)
	{
		msurface_t	*fa = fencefaces[i].face;
		entity_t	*ent = fencefaces[i].entity;

		blend = ent ? R_BlendMap (ent->alpha) : NULL;
		if ((blend != NULL) != translucent || blend == r_blendinvisible)
			continue;
		d_blendmap = blend;

		if (ent)
		{
		//
		// A fence on a door or a platform. Put the renderer back into that
		// model's space the way R_DrawBEntitiesOnList does, including the
		// separately transformed origin D_CalcGradients reads.
		//
			vec3_t	local_modelorg;

			currententity = ent;
			r_pcurrentvertbase = ent->model->vertexes;

			VectorCopy (ent->origin, r_entorigin);
			VectorSubtract (r_origin, r_entorigin, local_modelorg);
			TransformVector (local_modelorg, transformed_modelorg);

			VectorCopy (local_modelorg, modelorg);
			VectorCopy (modelorg, r_worldmodelorg);

			R_RotateBmodel ();

			R_RenderPoly (fa, fencefaces[i].clipflags);

			VectorCopy (base_vpn, vpn);
			VectorCopy (base_vup, vup);
			VectorCopy (base_vright, vright);
			VectorCopy (base_modelorg, modelorg);
			R_TransformFrustum ();
		}
		else
		{
			currententity = &cl_entities[0];
			r_pcurrentvertbase = cl.worldmodel->vertexes;

			VectorCopy (r_origin, modelorg);
			TransformVector (modelorg, transformed_modelorg);

			R_RenderPoly (fa, fencefaces[i].clipflags);
		}
	}

	d_blendmap = NULL;
	currententity = saveentity;
	VectorCopy (saveorigin, modelorg);
}

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

// r_draw.c

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"	// FIXME: shouldn't need to include this

#define MAXLEFTCLIPEDGES		100

// !!! if these are changed, they must be changed in asm_draw.h too !!!
#define FULLY_CLIPPED_CACHED	0x80000000
#define FRAMECOUNT_MASK			0x7FFFFFFF

unsigned int	cacheoffset;

int			c_faceclip;					// number of faces clipped

zpointdesc_t	r_zpointdesc;

polydesc_t		r_polydesc;



clipplane_t	*entity_clipplanes;
clipplane_t	view_clipplanes[4];
clipplane_t	world_clipplanes[16];

medge_t			*r_pedge;

qboolean		r_leftclipped, r_rightclipped;
static qboolean	makeleftedge, makerightedge;
qboolean		r_nearzionly;

int		sintable[SIN_BUFFER_SIZE];
int		intsintable[SIN_BUFFER_SIZE];

mvertex_t	r_leftenter, r_leftexit;
mvertex_t	r_rightenter, r_rightexit;

typedef struct
{
	float	u,v;
	int		ceilv;
} evert_t;

int				r_emitted;
float			r_nearzi;
float			r_u1, r_v1, r_lzi1;
int				r_ceilv1;

qboolean	r_lastvertvalid;

//
// A vertex that projects to something that is not a number.
//
// It happens on the re-release maps, a few dozen edges in a frame, and the
// source is not yet known. What it did is known: every clamp below compared the
// value and let it through, since a comparison with a NaN is always false, and
// ceil() of it came out as INT_MIN. The range check further down caught that
// and dropped the edge -- and an edge is one side of a surface, so the surface
// was left open on those scanlines and ran on to the far side of the screen,
// its texture clamped at its own border: a flat band, or a streak.
//
// An earlier fix wrote the clamps as "if (!(x > lo))" so that a NaN would take
// the assignment. That is correct C, and the build's -ffast-math lets the
// compiler assume there are no NaNs and undo it; a NaN injected into a vertex
// here went straight through. So this looks at the bits, which no optimisation
// is entitled to reason away, and treats infinities the same way.
//
// Nothing useful can be drawn for a face with such a vertex, and anything
// drawn for it would be wrong, so the whole face is left out of this frame.
//
qboolean	r_facebad;			// set by R_EmitEdge, read by the face callers
int			r_facesdiscarded;	// this frame, for the report in r_main.c

// what the first bad projection on this map looked like, for the report
qboolean	r_badrecorded;
vec3_t		r_badvertex, r_badorigin, r_badtransformed;
char		r_badmodel[64];

// clip fractions R_SafeFrac had to correct, this frame (see r_local.h)
int			r_clampedfrac;

// the first input found not to be a number on this map, named
char		r_badsource[128];

void R_NoteBadSource (const char *fmt, ...)
{
	va_list		argptr;

	if (r_badsource[0])
		return;

	va_start (argptr, fmt);
	vsnprintf (r_badsource, sizeof(r_badsource), fmt, argptr);
	va_end (argptr);
}

static qboolean R_NotFinite (const vec3_t v)
{
	int				i;
	unsigned int	bits;

	for (i=0 ; i<3 ; i++)
	{
		memcpy (&bits, &v[i], sizeof(bits));
		if ((bits & 0x7F800000) == 0x7F800000)
			return true;
	}
	return false;
}

static void R_RecordBad (const float *world, const vec3_t transformed)
{
	r_facebad = true;

	if (r_badrecorded)
		return;
	r_badrecorded = true;

	VectorCopy (world, r_badvertex);
	VectorCopy (modelorg, r_badorigin);
	VectorCopy (transformed, r_badtransformed);
	Q_strncpy (r_badmodel, currententity && currententity->model
				? currententity->model->name : "?", sizeof(r_badmodel) - 1);
	r_badmodel[sizeof(r_badmodel) - 1] = 0;
}

/*
================
R_DiscardFace

The face being built has a vertex that would not project. Take back what it
has put into the edge list so far, so that it opens and closes nothing: an edge
whose surface slots are both empty is skipped when spans are generated. Its own
edges also stop being offered to the faces that share them, since the slot it
held is empty and a sharer would otherwise take the wrong side of it. Called
only on the rare frame this happens, so walking the frame's edges is fine.
================
*/
static void R_DiscardFace (edge_t *firstown)
{
	edge_t	*e;
	int		self;

	self = surface_p - surfaces;

	for (e = r_edges ; e < edge_p ; e++)
	{
		if (e->surfs[0] == self)
			e->surfs[0] = 0;
		if (e->surfs[1] == self)
			e->surfs[1] = 0;
		if (e >= firstown)
			e->owner = NULL;
	}

	r_facesdiscarded++;
}


#if	!id386

/*
================
R_EmitEdge
================
*/
void R_EmitEdge (mvertex_t *pv0, mvertex_t *pv1)
{
	edge_t	*edge, *pcheck;
	int		u_check;
	float	u, u_step;
	vec3_t	local, transformed;
	float	*world;
	int		v, v2, ceilv0;
	float	scale, lzi0, u0, v0;
	int		side;

//
// Once a vertex of this face has failed, emit nothing more for it: the face is
// coming out anyway. And it has to stop here, because the cached vertex is no
// longer to be trusted -- the face loop marks it valid after every edge, so
// the edge after a bad first vertex would start from whatever vertex the
// previous face left in r_u1 and r_v1. For a face that ended with a right-hand
// clip, that is a vertex whose r_ceilv1 was never updated, and an edge built
// from it steps by a NaN; its u runs off the left of the screen and the scan
// walks past edge_head into a null pointer. That was a crash, found by forcing
// bad clip points.
//
	if (r_facebad)
		return;

	if (r_lastvertvalid)
	{
		u0 = r_u1;
		v0 = r_v1;
		lzi0 = r_lzi1;
		ceilv0 = r_ceilv1;
	}
	else
	{
		world = &pv0->position[0];
	
	// transform and project
		VectorSubtract (world, modelorg, local);
		TransformVector (local, transformed);

		if (R_NotFinite (transformed))
		{
			R_RecordBad (world, transformed);
			return;
		}
	
	//
	// The comparisons are negated so that a NaN is caught.
	//
	// Every clamp here is also the safety net for what follows, and
	// "if (x < lo) x = lo;" is not a net at all when x is a NaN: every
	// comparison against a NaN is false, so it passes both clamps untouched,
	// ceil() hands back a NaN, and the cast to int is undefined -- in practice
	// INT_MIN. That index then reaches newedges[v] below, sixteen gigabytes
	// out of bounds, which is the crash this came from. Written as
	// "if (!(x > lo)) x = lo;" a NaN takes the assignment instead. For any
	// finite value the two spellings do the same thing.
	//
		if (!(transformed[2] > NEAR_CLIP))
			transformed[2] = NEAR_CLIP;
	
		lzi0 = 1.0 / transformed[2];
	
	// FIXME: build x/yscale into transform?
		scale = xscale * lzi0;
		u0 = (xcenter + scale*transformed[0]);
		if (!(u0 > r_refdef.fvrectx_adj))
			u0 = r_refdef.fvrectx_adj;
		if (!(u0 < r_refdef.fvrectright_adj))
			u0 = r_refdef.fvrectright_adj;
	
		scale = yscale * lzi0;
		v0 = (ycenter - scale*transformed[1]);
		if (!(v0 > r_refdef.fvrecty_adj))
			v0 = r_refdef.fvrecty_adj;
		if (!(v0 < r_refdef.fvrectbottom_adj))
			v0 = r_refdef.fvrectbottom_adj;
	
		ceilv0 = (int) ceil(v0);
	}

	world = &pv1->position[0];

// transform and project
	VectorSubtract (world, modelorg, local);
	TransformVector (local, transformed);

	if (R_NotFinite (transformed))
	{
		R_RecordBad (world, transformed);
		r_lastvertvalid = false;
		return;
	}

// Negated for the same reason as the block above: these clamps are what keeps
// a NaN out of ceil() and out of the scanline index.
	if (!(transformed[2] > NEAR_CLIP))
		transformed[2] = NEAR_CLIP;

	r_lzi1 = 1.0 / transformed[2];

	scale = xscale * r_lzi1;
	r_u1 = (xcenter + scale*transformed[0]);
	if (!(r_u1 > r_refdef.fvrectx_adj))
		r_u1 = r_refdef.fvrectx_adj;
	if (!(r_u1 < r_refdef.fvrectright_adj))
		r_u1 = r_refdef.fvrectright_adj;

	scale = yscale * r_lzi1;
	r_v1 = (ycenter - scale*transformed[1]);
	if (!(r_v1 > r_refdef.fvrecty_adj))
		r_v1 = r_refdef.fvrecty_adj;
	if (!(r_v1 < r_refdef.fvrectbottom_adj))
		r_v1 = r_refdef.fvrectbottom_adj;

	if (r_lzi1 > lzi0)
		lzi0 = r_lzi1;

	if (lzi0 > r_nearzi)	// for mipmap finding
		r_nearzi = lzi0;

// for right edges, all we want is the effect on 1/z
	if (r_nearzionly)
		return;

	r_emitted = 1;

	r_ceilv1 = (int) ceil(r_v1);


// create the edge
	if (ceilv0 == r_ceilv1)
	{
	// we cache unclipped horizontal edges as fully clipped
		if (cacheoffset != 0x7FFFFFFF)
		{
			cacheoffset = FULLY_CLIPPED_CACHED |
					(r_framecount & FRAMECOUNT_MASK);
		}

		return;		// horizontal edge
	}

	side = ceilv0 > r_ceilv1;

	if (edge_p >= edge_max)
	{
	// R_RenderBmodelFace reserves psurf->numedges + 4, but the chain it walks
	// is the polygon after R_RecursiveClipBPoly has split it, which is longer.
	// Measured at 2 over on id's maps, inside the 4 of headroom -- a deeper
	// BSP has more room to exceed it, and this is a stack array.
		r_outofedges++;
		return;
	}

	edge = edge_p++;

	edge->owner = r_pedge;

	edge->nearzi = lzi0;

	if (side == 0)
	{
	// trailing edge (go from p1 to p2)
		v = ceilv0;
		v2 = r_ceilv1 - 1;

		edge->surfs[0] = surface_p - surfaces;
		edge->surfs[1] = 0;

		u_step = ((r_u1 - u0) / (r_v1 - v0));
		u = u0 + ((float)v - v0) * u_step;
	}
	else
	{
	// leading edge (go from p2 to p1)
		v2 = ceilv0 - 1;
		v = r_ceilv1;

		edge->surfs[0] = 0;
		edge->surfs[1] = surface_p - surfaces;

		u_step = ((u0 - r_u1) / (v0 - r_v1));
		u = r_u1 + ((float)v - r_v1) * u_step;
	}

//
// The same NaN from any other route: an edge whose step is not a number
// cannot be stepped, and in the active list it corrupts the sort for every
// edge after it. Hand it back and leave the face out.
//
	if (R_BadFloat (u_step) || R_BadFloat (u))
	{
		edge_p--;
		R_RecordBad (pv1->position, transformed);
		return;
	}

	edge->u_step = u_step*0x100000;
	edge->u = u*0x100000 + 0xFFFFF;

// we need to do this to avoid stepping off the edges if a very nearly
// horizontal edge is less than epsilon above a scan, and numeric error causes
// it to incorrectly extend to the scan, and the extension of the line goes off
// the edge of the screen
// FIXME: is this actually needed?
	if (edge->u < r_refdef.vrect_x_adj_shift20)
		edge->u = r_refdef.vrect_x_adj_shift20;
	if (edge->u > r_refdef.vrectright_adj_shift20)
		edge->u = r_refdef.vrectright_adj_shift20;

//
// The scanline indices, checked before anything is indexed by them.
//
// newedges[] and removeedges[] hold one pointer per scanline. v and v2 come
// from ceil() of the clamped projection, so with the clamps above they are
// already in range -- ceilv0 and r_ceilv1 land in [vrect.y, vrectbottom], and
// the side that would put v at vrectbottom cannot arise. This is the check
// that makes that reasoning unnecessary: whatever a map does to the arithmetic,
// the write stays inside the arrays. A dropped edge is a seam on one frame.
//
// It tests v and v2 rather than ceilv0 and r_ceilv1 because those two are not
// the indices: ceilv0 legitimately reaches vrectbottom, where it becomes
// v2 = vrectbottom - 1. Checking them instead dropped real edges and changed
// the picture.
//
	if (v < 0 || v >= MAXHEIGHT || v2 < 0 || v2 >= MAXHEIGHT)
	{
		edge_p--;			// hand the edge back
		r_edgesoutofrange++;
		return;
	}

//
// sort the edge in normally
//
	u_check = edge->u;
	if (edge->surfs[0])
		u_check++;	// sort trailers after leaders

	if (!newedges[v] || newedges[v]->u >= u_check)
	{
		edge->next = newedges[v];
		newedges[v] = edge;
	}
	else
	{
		pcheck = newedges[v];
		while (pcheck->next && pcheck->next->u < u_check)
			pcheck = pcheck->next;
		edge->next = pcheck->next;
		pcheck->next = edge;
	}

	edge->nextremove = removeedges[v2];
	removeedges[v2] = edge;
}


/*
================
R_ClipEdge
================
*/
void R_ClipEdge (mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip)
{
	float		d0, d1, f;
	mvertex_t	clipvert;

	if (clip)
	{
		do
		{
			d0 = DotProduct (pv0->position, clip->normal) - clip->dist;
			d1 = DotProduct (pv1->position, clip->normal) - clip->dist;

			if (d0 >= 0)
			{
			// point 0 is unclipped
				if (d1 >= 0)
				{
				// both points are unclipped
					continue;
				}

			// only point 1 is clipped

			// we don't cache clipped edges
				cacheoffset = 0x7FFFFFFF;

				f = R_SafeFrac (d0 / (d0 - d1));
				clipvert.position[0] = pv0->position[0] +
						f * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						f * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						f * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					r_leftclipped = true;
					r_leftexit = clipvert;
				}
				else if (clip->rightedge)
				{
					r_rightclipped = true;
					r_rightexit = clipvert;
				}

				R_ClipEdge (pv0, &clipvert, clip->next);
				return;
			}
			else
			{
			// point 0 is clipped
				if (d1 < 0)
				{
				// both points are clipped
				// we do cache fully clipped edges
					if (!r_leftclipped)
						cacheoffset = FULLY_CLIPPED_CACHED |
								(r_framecount & FRAMECOUNT_MASK);
					return;
				}

			// only point 0 is clipped
				r_lastvertvalid = false;

			// we don't cache partially clipped edges
				cacheoffset = 0x7FFFFFFF;

				f = R_SafeFrac (d0 / (d0 - d1));
				clipvert.position[0] = pv0->position[0] +
						f * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						f * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						f * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					r_leftclipped = true;
					r_leftenter = clipvert;
				}
				else if (clip->rightedge)
				{
					r_rightclipped = true;
					r_rightenter = clipvert;
				}

				R_ClipEdge (&clipvert, pv1, clip->next);
				return;
			}
		} while ((clip = clip->next) != NULL);
	}

// add the edge
	R_EmitEdge (pv0, pv1);
}

#endif	// !id386


/*
================
R_EmitCachedEdge
================
*/
//
// An edge_t has two slots: the surface it closes on each scanline and the
// surface it opens. id's cache hands an edge one face made to the next face
// that uses it, and puts that face in whichever slot is free. That is right
// when the second face walks the edge the other way, which in id's maps it
// always does: every edge has one face on each side.
//
// Maps from modern compilers can have an edge walked by three faces, two of
// them the same way. The re-release start map has one on a crate: the crate's
// side and a face beside it both close on the same edge, the neighbour took
// that slot first, and the crate was put in the other and opened there
// instead. Nothing closed it, and it ran on to the right edge of the screen as
// a black bar. So the medge now remembers which way the face that cached it
// walked it, and a face walking it the same way makes an edge of its own. So
// does a third face when both slots are taken, which id's code would have
// written over, taking the edge from one of the first two.
//
static qboolean R_CanReuseEdge (medge_t *pedge, qboolean forward)
{
	edge_t	*e;

	if (pedge->cachedforward == forward)
		return false;
	if (((unsigned long)edge_p - (unsigned long)r_edges) <=
		pedge->cachededgeoffset)
		return false;
	e = (edge_t *)((unsigned long)r_edges + pedge->cachededgeoffset);
	if (e->owner != pedge)
		return false;
	return !e->surfs[0] || !e->surfs[1];
}

void R_EmitCachedEdge (void)
{
	edge_t		*pedge_t;

	pedge_t = (edge_t *)((unsigned long)r_edges + r_pedge->cachededgeoffset);

	if (!pedge_t->surfs[0])
		pedge_t->surfs[0] = surface_p - surfaces;
	else
		pedge_t->surfs[1] = surface_p - surfaces;

	if (pedge_t->nearzi > r_nearzi)	// for mipmap finding
		r_nearzi = pedge_t->nearzi;

	r_emitted = 1;
}


/*
================
R_RenderFace
================
*/
void R_RenderFace (msurface_t *fa, int clipflags)
{
	int			i, lindex;
	edge_t		*firstown;
	unsigned	mask;
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;
	medge_t		*pedges, tedge;
	clipplane_t	*pclip;

//
// A fence must not go in the edge list: it would hide the room behind it,
// which is the room its holes are supposed to show. R_DrawFenceFaces draws it
// over the finished frame instead.
//
	if (R_DeferFace (fa) && R_FenceDeferFace (fa, clipflags))
		return;

// skip out if no more surfs
	if ((surface_p) >= surf_max)
	{
		r_outofsurfaces++;
		return;
	}

// ditto if not enough edges left, or switch to auxedges if possible
	if ((edge_p + fa->numedges + 4) >= edge_max)
	{
		r_outofedges += fa->numedges;
		return;
	}

	c_faceclip++;

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (clipflags & mask)
		{
			view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

// push the edges through
	r_emitted = 0;
	r_nearzi = 0;
	r_nearzionly = false;
	r_facebad = false;
	firstown = edge_p;
	makeleftedge = makerightedge = false;
	pedges = currententity->model->edges;
	r_lastvertvalid = false;

	for (i=0 ; i<fa->numedges ; i++)
	{
		lindex = currententity->model->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];

		// if the edge is cached, we can just reuse the edge
			if (!insubmodel)
			{
				if (r_pedge->cachededgeoffset & FULLY_CLIPPED_CACHED)
				{
					if ((r_pedge->cachededgeoffset & FRAMECOUNT_MASK) ==
						r_framecount)
					{
						r_lastvertvalid = false;
						continue;
					}
				}
				else
				{
					if (R_CanReuseEdge (r_pedge, true))
					{
						R_EmitCachedEdge ();
						r_lastvertvalid = false;
						continue;
					}
				}
			}

		// assume it's cacheable
			cacheoffset = (byte *)edge_p - (byte *)r_edges;
			r_leftclipped = r_rightclipped = false;
			R_ClipEdge (&r_pcurrentvertbase[r_pedge->v[0]],
						&r_pcurrentvertbase[r_pedge->v[1]],
						pclip);
			r_pedge->cachededgeoffset = cacheoffset;
			r_pedge->cachedforward = true;

			if (r_leftclipped)
				makeleftedge = true;
			if (r_rightclipped)
				makerightedge = true;
			r_lastvertvalid = true;
		}
		else
		{
			lindex = -lindex;
			r_pedge = &pedges[lindex];
		// if the edge is cached, we can just reuse the edge
			if (!insubmodel)
			{
				if (r_pedge->cachededgeoffset & FULLY_CLIPPED_CACHED)
				{
					if ((r_pedge->cachededgeoffset & FRAMECOUNT_MASK) ==
						r_framecount)
					{
						r_lastvertvalid = false;
						continue;
					}
				}
				else
				{
					if (R_CanReuseEdge (r_pedge, false))
					{
						R_EmitCachedEdge ();
						r_lastvertvalid = false;
						continue;
					}
				}
			}

		// assume it's cacheable
			cacheoffset = (byte *)edge_p - (byte *)r_edges;
			r_leftclipped = r_rightclipped = false;
			R_ClipEdge (&r_pcurrentvertbase[r_pedge->v[1]],
						&r_pcurrentvertbase[r_pedge->v[0]],
						pclip);
			r_pedge->cachededgeoffset = cacheoffset;
			r_pedge->cachedforward = false;

			if (r_leftclipped)
				makeleftedge = true;
			if (r_rightclipped)
				makerightedge = true;
			r_lastvertvalid = true;
		}
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (makeleftedge)
	{
		r_pedge = &tedge;
		r_lastvertvalid = false;
		R_ClipEdge (&r_leftexit, &r_leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right r_nearzi
	if (makerightedge)
	{
		r_pedge = &tedge;
		r_lastvertvalid = false;
		r_nearzionly = true;
		R_ClipEdge (&r_rightexit, &r_rightenter, view_clipplanes[1].next);
	}

// a vertex that would not project: take the face back out (see R_DiscardFace)
	if (r_facebad)
	{
		R_DiscardFace (firstown);
		return;
	}

// if no edges made it out, return without posting the surface
	if (!r_emitted)
		return;

	r_polycount++;

	surface_p->data = (void *)fa;
	surface_p->nearzi = r_nearzi;
	surface_p->flags = fa->flags;
	surface_p->insubmodel = insubmodel;
	surface_p->spanstate = 0;
	surface_p->entity = currententity;
	surface_p->key = r_currentkey++;
	surface_p->spans = NULL;

	pplane = fa->plane;
// FIXME: cache this?
	TransformVector (pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0 / (pplane->dist - DotProduct (modelorg, pplane->normal));

	surface_p->d_zistepu = p_normal[0] * xscaleinv * distinv;
	surface_p->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	surface_p->d_ziorigin = p_normal[2] * distinv -
			xcenter * surface_p->d_zistepu -
			ycenter * surface_p->d_zistepv;

//JDC	VectorCopy (r_worldmodelorg, surface_p->modelorg);
	surface_p++;
}


/*
================
R_RenderBmodelFace
================
*/
void R_RenderBmodelFace (bedge_t *pedges, msurface_t *psurf)
{
	edge_t		*firstown;
	int			i;
	unsigned	mask;
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;
	medge_t		tedge;
	clipplane_t	*pclip;

// skip out if no more surfs
	if (surface_p >= surf_max)
	{
		r_outofsurfaces++;
		return;
	}

// ditto if not enough edges left, or switch to auxedges if possible
	if ((edge_p + psurf->numedges + 4) >= edge_max)
	{
		r_outofedges += psurf->numedges;
		return;
	}

	c_faceclip++;

// this is a dummy to give the caching mechanism someplace to write to
	r_pedge = &tedge;

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (r_clipflags & mask)
		{
			view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

// push the edges through
	r_emitted = 0;
	r_nearzi = 0;
	r_nearzionly = false;
	r_facebad = false;
	firstown = edge_p;
	makeleftedge = makerightedge = false;
// FIXME: keep clipped bmodel edges in clockwise order so last vertex caching
// can be used?
	r_lastvertvalid = false;

	for ( ; pedges ; pedges = pedges->pnext)
	{
		r_leftclipped = r_rightclipped = false;
		R_ClipEdge (pedges->v[0], pedges->v[1], pclip);

		if (r_leftclipped)
			makeleftedge = true;
		if (r_rightclipped)
			makerightedge = true;
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (makeleftedge)
	{
		r_pedge = &tedge;
		R_ClipEdge (&r_leftexit, &r_leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right r_nearzi
	if (makerightedge)
	{
		r_pedge = &tedge;
		r_nearzionly = true;
		R_ClipEdge (&r_rightexit, &r_rightenter, view_clipplanes[1].next);
	}

// a vertex that would not project: take the face back out (see R_DiscardFace)
	if (r_facebad)
	{
		R_DiscardFace (firstown);
		return;
	}

// if no edges made it out, return without posting the surface
	if (!r_emitted)
		return;

	r_polycount++;

	surface_p->data = (void *)psurf;
	surface_p->nearzi = r_nearzi;
	surface_p->flags = psurf->flags;
	surface_p->insubmodel = true;
	surface_p->spanstate = 0;
	surface_p->entity = currententity;
	surface_p->key = r_currentbkey;
	surface_p->spans = NULL;

	pplane = psurf->plane;
// FIXME: cache this?
	TransformVector (pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0 / (pplane->dist - DotProduct (modelorg, pplane->normal));

	surface_p->d_zistepu = p_normal[0] * xscaleinv * distinv;
	surface_p->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	surface_p->d_ziorigin = p_normal[2] * distinv -
			xcenter * surface_p->d_zistepu -
			ycenter * surface_p->d_zistepv;

//JDC	VectorCopy (r_worldmodelorg, surface_p->modelorg);
	surface_p++;
}


/*
================
R_RenderPoly
================
*/
void R_RenderPoly (msurface_t *fa, int clipflags)
{
	int			i, lindex, lnumverts, s_axis, t_axis;
	float		dist, lastdist, lzi, scale, u, v, frac;
	unsigned	mask;
	vec3_t		local, transformed;
	clipplane_t	*pclip;
	medge_t		*pedges;
	mplane_t	*pplane;
	mvertex_t	verts[2][100];	//FIXME: do real number
	polyvert_t	pverts[100];	//FIXME: do real number, safely
	int			vertpage, newverts, newpage, lastvert;
	qboolean	visible;

//
// id's FIXME above, made real: clipping can add a vertex per plane, so a face
// anywhere near the size of those buffers would walk off the end of them.
// R_FenceDeferFace refuses such a face before it gets here; this is the
// backstop for any other caller.
//
	if (fa->numedges > 90)
		return;

// FIXME: clean this up and make it faster
// FIXME: guard against running out of vertices

	s_axis = t_axis = 0;	// keep compiler happy

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (clipflags & mask)
		{
			view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

// reconstruct the polygon
// FIXME: these should be precalculated and loaded off disk
	pedges = currententity->model->edges;
	lnumverts = fa->numedges;
	vertpage = 0;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currententity->model->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			verts[0][i] = r_pcurrentvertbase[r_pedge->v[0]];
		}
		else
		{
			r_pedge = &pedges[-lindex];
			verts[0][i] = r_pcurrentvertbase[r_pedge->v[1]];
		}
	}

// clip the polygon, done if not visible
	while (pclip)
	{
		lastvert = lnumverts - 1;
		lastdist = DotProduct (verts[vertpage][lastvert].position,
							   pclip->normal) - pclip->dist;

		visible = false;
		newverts = 0;
		newpage = vertpage ^ 1;

		for (i=0 ; i<lnumverts ; i++)
		{
			dist = DotProduct (verts[vertpage][i].position, pclip->normal) -
					pclip->dist;

			if ((lastdist > 0) != (dist > 0))
			{
				frac = R_SafeFrac (dist / (dist - lastdist));
				verts[newpage][newverts].position[0] =
						verts[vertpage][i].position[0] +
						((verts[vertpage][lastvert].position[0] -
						  verts[vertpage][i].position[0]) * frac);
				verts[newpage][newverts].position[1] =
						verts[vertpage][i].position[1] +
						((verts[vertpage][lastvert].position[1] -
						  verts[vertpage][i].position[1]) * frac);
				verts[newpage][newverts].position[2] =
						verts[vertpage][i].position[2] +
						((verts[vertpage][lastvert].position[2] -
						  verts[vertpage][i].position[2]) * frac);
				newverts++;
			}

			if (dist >= 0)
			{
				verts[newpage][newverts] = verts[vertpage][i];
				newverts++;
				visible = true;
			}

			lastvert = i;
			lastdist = dist;
		}

		if (!visible || (newverts < 3))
			return;

		lnumverts = newverts;
		vertpage ^= 1;
		pclip = pclip->next;
	}

// transform and project, remembering the z values at the vertices and
// r_nearzi, and extract the s and t coordinates at the vertices
	pplane = fa->plane;
	switch (pplane->type)
	{
	case PLANE_X:
	case PLANE_ANYX:
		s_axis = 1;
		t_axis = 2;
		break;
	case PLANE_Y:
	case PLANE_ANYY:
		s_axis = 0;
		t_axis = 2;
		break;
	case PLANE_Z:
	case PLANE_ANYZ:
		s_axis = 0;
		t_axis = 1;
		break;
	}

	r_nearzi = 0;

	for (i=0 ; i<lnumverts ; i++)
	{
	// transform and project
		VectorSubtract (verts[vertpage][i].position, modelorg, local);
		TransformVector (local, transformed);

	// see r_facebad above: no polygon at all beats one with a NaN corner
		if (R_NotFinite (transformed))
		{
			R_RecordBad (verts[vertpage][i].position, transformed);
			return;
		}

		if (transformed[2] < NEAR_CLIP)
			transformed[2] = NEAR_CLIP;

		lzi = 1.0 / transformed[2];

		if (lzi > r_nearzi)	// for mipmap finding
			r_nearzi = lzi;

	// FIXME: build x/yscale into transform?
		scale = xscale * lzi;
		u = (xcenter + scale*transformed[0]);
		if (u < r_refdef.fvrectx_adj)
			u = r_refdef.fvrectx_adj;
		if (u > r_refdef.fvrectright_adj)
			u = r_refdef.fvrectright_adj;

		scale = yscale * lzi;
		v = (ycenter - scale*transformed[1]);
		if (v < r_refdef.fvrecty_adj)
			v = r_refdef.fvrecty_adj;
		if (v > r_refdef.fvrectbottom_adj)
			v = r_refdef.fvrectbottom_adj;

		pverts[i].u = u;
		pverts[i].v = v;
		pverts[i].zi = lzi;
		pverts[i].s = verts[vertpage][i].position[s_axis];
		pverts[i].t = verts[vertpage][i].position[t_axis];
	}

// build the polygon descriptor, including fa, r_nearzi, and u, v, s, t, and z
// for each vertex
	r_polydesc.numverts = lnumverts;
	r_polydesc.nearzi = r_nearzi;
	r_polydesc.pcurrentface = fa;
	r_polydesc.pverts = pverts;

// draw the polygon
	D_DrawPoly ();
}


/*
================
R_ZDrawSubmodelPolys
================
*/
void R_ZDrawSubmodelPolys (model_t *pmodel)
{
	int			i, numsurfaces;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;

	psurf = &pmodel->surfaces[pmodel->firstmodelsurface];
	numsurfaces = pmodel->nummodelsurfaces;

	for (i=0 ; i<numsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
		// FIXME: use bounding-box-based frustum clipping info?
			R_RenderPoly (psurf, 15);
		}
	}
}



/*
================
R_FaceEdgeReport

For "surface", when a face was drawn where it is not: what became of each of
its edges this frame. A face whose span runs on across the screen has lost the
edge that should close it, and this says which edge and how -- skipped as
off-screen, never emitted, or emitted with the face in the wrong slot.
================
*/
void R_FaceEdgeReport (msurface_t *face)
{
	int			i, j, k, lindex, idx, self, fwd, back, nedges;
	unsigned	off;
	medge_t		*pedge, *pedges;
	edge_t		*e;
	msurface_t	*f;
	surf_t		*s;
	float		*v;
	vec3_t		local, t;
	model_t		*m;

	m = cl.worldmodel;
	pedges = m->edges;

	self = 0;
	for (s = &surfaces[1] ; s<surface_p ; s++)
		if (s->data == face)
			self = s - surfaces;
	Con_Printf ("  its surface this frame is %d; its edges:\n", self);

	nedges = edge_p - r_edges;
	for (i=0 ; i<face->numedges ; i++)
	{
		lindex = m->surfedges[face->firstedge + i];
		idx = lindex > 0 ? lindex : -lindex;
		pedge = &pedges[idx];

	// how many faces walk this edge each way; a closed surface has one each
		fwd = back = 0;
		for (j=0, f=m->surfaces ; j<m->numsurfaces ; j++, f++)
			for (k=0 ; k<f->numedges ; k++)
			{
				if (m->surfedges[f->firstedge + k] == idx)
					fwd++;
				else if (m->surfedges[f->firstedge + k] == -idx)
					back++;
			}

		Con_Printf ("  %d: edge %d %s, faces %d+ %d-,", i, idx,
					lindex > 0 ? "forward" : "backward", fwd, back);

		off = pedge->cachededgeoffset;
		if (off & FULLY_CLIPPED_CACHED)
			Con_Printf (" skipped as off-screen or flat%s",
						(off & FRAMECOUNT_MASK) == (r_framecount & FRAMECOUNT_MASK)
						? "" : " (another frame)");
		else if (off == 0x7FFFFFFF)
			Con_Printf (" clipped");
		else if (off / sizeof(edge_t) < (unsigned)nedges
			&& (e = (edge_t *)((byte *)r_edges + off))->owner == pedge)
			Con_Printf (" edge_t %d surfs %d/%d%s", (int)(off / sizeof(edge_t)),
						e->surfs[0], e->surfs[1],
						(e->surfs[0] == self || e->surfs[1] == self)
						? "" : " WITHOUT this face");
		else
			Con_Printf (" not emitted");

		for (j=0 ; j<2 ; j++)
		{
			v = m->vertexes[pedge->v[j]].position;
			VectorSubtract (v, r_origin, local);
			t[0] = DotProduct (local, vright);
			t[1] = DotProduct (local, vup);
			t[2] = DotProduct (local, vpn);
			if (t[2] < NEAR_CLIP)
				Con_Printf (j ? " to behind" : " from behind");
			else
				Con_Printf (j ? " to (%.0f %.0f)" : " from (%.0f %.0f)",
							xcenter + xscale*t[0]/t[2],
							ycenter - yscale*t[1]/t[2]);
		}
		Con_Printf ("\n");
	}
}

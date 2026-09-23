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
// r_bsp.c

#include "quakedef.h"
#include "r_local.h"

//
// current entity info
//
qboolean		insubmodel;
entity_t		*currententity;
vec3_t			modelorg, base_modelorg;
								// modelorg is the viewpoint reletive to
								// the currently rendering entity
vec3_t			r_entorigin;	// the currently rendering entity in world
								// coordinates

float			entity_rotation[3][3];

vec3_t			r_worldmodelorg;

int				r_currentbkey;

typedef enum {touchessolid, drawnode, nodrawnode} solidstate_t;

//
// Scratch for clipping one brush model -- a door, a platform, a lift, any of
// the moving world geometry -- against the view. Over the limit
// R_RecursiveClipBPoly returns and the model is not drawn at all: a door that
// is simply absent, not a door with a hole in it.
//
// id's 500 and 1000 were sized for id's doors. A re-release map's machinery is
// far heavier, and this was the flood of "Out of edges for bmodel" -- once per
// clipped polygon per frame, which is hundreds of lines a frame and slow
// enough on its own to be felt.
//
// 8192 and 16384 were still not enough for the machine campaigns, so they are
// larger again and no longer on the stack: R_DrawSolidClippedSubmodelPolygons
// is called once per entity and is not itself recursive, so one static pair
// serves it. That is 786 KB of bss rather than a 2 MB stack frame.
//
#define MAX_BMODEL_VERTS	65536		// 786K
#define MAX_BMODEL_EDGES	131072		// 3M

static mvertex_t	*pbverts;
static bedge_t		*pbedges;
static int			numbverts, numbedges;

static mvertex_t	*pfrontenter, *pfrontexit;

static qboolean		makeclippededge;


//===========================================================================

/*
================
R_EntityRotate
================
*/
void R_EntityRotate (vec3_t vec)
{
	vec3_t	tvec;

	VectorCopy (vec, tvec);
	vec[0] = DotProduct (entity_rotation[0], tvec);
	vec[1] = DotProduct (entity_rotation[1], tvec);
	vec[2] = DotProduct (entity_rotation[2], tvec);
}


/*
================
R_RotateBmodel
================
*/
void R_RotateBmodel (void)
{
	float	angle, s, c, temp1[3][3], temp2[3][3], temp3[3][3];

// TODO: should use a look-up table
// TODO: should really be stored with the entity instead of being reconstructed
// TODO: could cache lazily, stored in the entity
// TODO: share work with R_SetUpAliasTransform

// yaw
	angle = currententity->angles[YAW];		
	angle = angle * M_PI*2 / 360;
	s = sin(angle);
	c = cos(angle);

	temp1[0][0] = c;
	temp1[0][1] = s;
	temp1[0][2] = 0;
	temp1[1][0] = -s;
	temp1[1][1] = c;
	temp1[1][2] = 0;
	temp1[2][0] = 0;
	temp1[2][1] = 0;
	temp1[2][2] = 1;


// pitch
	angle = currententity->angles[PITCH];		
	angle = angle * M_PI*2 / 360;
	s = sin(angle);
	c = cos(angle);

	temp2[0][0] = c;
	temp2[0][1] = 0;
	temp2[0][2] = -s;
	temp2[1][0] = 0;
	temp2[1][1] = 1;
	temp2[1][2] = 0;
	temp2[2][0] = s;
	temp2[2][1] = 0;
	temp2[2][2] = c;

	R_ConcatRotations (temp2, temp1, temp3);

// roll
	angle = currententity->angles[ROLL];		
	angle = angle * M_PI*2 / 360;
	s = sin(angle);
	c = cos(angle);

	temp1[0][0] = 1;
	temp1[0][1] = 0;
	temp1[0][2] = 0;
	temp1[1][0] = 0;
	temp1[1][1] = c;
	temp1[1][2] = s;
	temp1[2][0] = 0;
	temp1[2][1] = -s;
	temp1[2][2] = c;

	R_ConcatRotations (temp1, temp3, entity_rotation);

//
// rotate modelorg and the transformation matrix
//
	R_EntityRotate (modelorg);
	R_EntityRotate (vpn);
	R_EntityRotate (vright);
	R_EntityRotate (vup);

	R_TransformFrustum ();
}


//
// Said once a map rather than once a clipped polygon.
//
// The original printed inside the clipping walk, so a map whose machinery
// exceeds these buffers prints hundreds of lines a frame. That buries
// everything else on the console, and the printing itself is slow enough to
// show up as the picture stalling.
//
qboolean	r_reportedbmodel;

static void R_ReportBmodelShort (void)
{
	if (r_reportedbmodel)
		return;

	r_reportedbmodel = true;
	Con_Printf ("\nA brush model (door, platform, lift) is too complex for the "
				"clipping\nbuffers and is not being drawn. Raising "
				"MAX_BMODEL_VERTS and\nMAX_BMODEL_EDGES in r_bsp.c is the "
				"fix; there is no cvar for it.\n");
}

//
// A brush model cut along a world plane: each edge's two ends are measured
// against the plane, and a cut point is made where an edge crosses. A convex
// face crosses a plane twice or not at all, and the two cut points are joined
// by a new edge that closes each half.
//
// Every vertex is measured twice, as the end of one edge and the start of the
// next, and id wrote the measurement out twice. Under -ffast-math the compiler
// is free to evaluate the two copies differently -- reassociated, one of them
// folded into other arithmetic -- and for a vertex lying on the plane the two
// answers can differ in sign. The face then crosses the plane once. Only one
// of pfrontenter and pfrontexit was set by this cut, the other still pointed at
// a cut point from some earlier face, and the closing edge ran from here to
// there: a sliver of this face stretched across the screen to wherever that
// was. With no instrumentation other than a counter, id's demo2 does it once
// in about twelve thousand cuts; the re-release maps, with far more doors and
// lifts spanning the world's BSP, far more often.
//
// Measured once, by one function the compiler may not inline, the same vertex
// gets the same answer both times. The pointers are also cleared per cut.
//
// After that a closed loop of edges cannot cross a plane an odd number of
// times, but a face whose edges do not close can: one whose last edge does not
// end where its first begins, or with a gap in the middle, which the
// re-release maps have. There is no second cut point to join, so such a face
// is not cut at that plane at all: it goes whole to the side most of it is on,
// and is drawn as it is when nothing cuts it. It was left out of the frame
// before, and flickered.
//
int		r_bmodelodd;	// this frame, for the report in r_main.c
char	*r_bmodeloddname;	// the model it was on

#ifdef __GNUC__
__attribute__((noinline))
#endif
static float R_BPlaneDist (const mvertex_t *v, const mplane_t *plane)
{
	return DotProduct (v->position, plane->normal) - plane->dist;
}

/*
================
R_RecursiveClipBPoly
================
*/
void R_RecursiveClipBPoly (bedge_t *pedges, mnode_t *pnode, msurface_t *psurf)
{
	bedge_t		*psideedges[2], *pnextedge, *ptedge;
	int			i, side, lastside;
	float		dist, frac, lastdist;
	int			crossings, front, count;
	mplane_t	*splitplane, tplane;
	mvertex_t	*pvert, *plastvert, *ptvert;
	mnode_t		*pn;

	psideedges[0] = psideedges[1] = NULL;

	makeclippededge = false;
	pfrontenter = pfrontexit = NULL;

// transform the BSP plane into model space
// FIXME: cache these?
	splitplane = pnode->plane;
	tplane.dist = splitplane->dist -
			DotProduct(r_entorigin, splitplane->normal);
	tplane.normal[0] = DotProduct (entity_rotation[0], splitplane->normal);
	tplane.normal[1] = DotProduct (entity_rotation[1], splitplane->normal);
	tplane.normal[2] = DotProduct (entity_rotation[2], splitplane->normal);

//
// A plane that is not a number cuts nothing sensibly, and every point made
// against it would be a NaN. Say which node and which model, and leave this
// piece undrawn rather than make them.
//
	if (R_BadFloat (tplane.normal[0]) || R_BadFloat (tplane.normal[1])
		|| R_BadFloat (tplane.normal[2]) || R_BadFloat (tplane.dist))
	{
		R_NoteBadSource ("the world plane at node %d, clipping %s at "
						 "(%g %g %g) angles (%g %g %g)",
						 (int)(pnode - cl.worldmodel->nodes),
						 currententity->model->name,
						 r_entorigin[0], r_entorigin[1], r_entorigin[2],
						 currententity->angles[0], currententity->angles[1],
						 currententity->angles[2]);
		return;
	}

// count the crossings first, while the edge list is still whole
	crossings = front = count = 0;
	for (ptedge = pedges ; ptedge ; ptedge = ptedge->pnext)
	{
		lastside = R_BPlaneDist (ptedge->v[0], &tplane) > 0 ? 0 : 1;
		side = R_BPlaneDist (ptedge->v[1], &tplane) > 0 ? 0 : 1;
		crossings += side != lastside;
		front += !lastside;
		count++;
	}

	if (crossings & 1)
	{
		r_bmodelodd++;
		r_bmodeloddname = currententity->model->name;
		psideedges[front*2 >= count ? 0 : 1] = pedges;
		goto recurse;
	}

// clip edges to BSP plane
	for ( ; pedges ; pedges = pnextedge)
	{
		pnextedge = pedges->pnext;

	// set the status for the last point as the previous point
	// FIXME: cache this stuff somehow?
		plastvert = pedges->v[0];
		lastdist = R_BPlaneDist (plastvert, &tplane);

		if (lastdist > 0)
			lastside = 0;
		else
			lastside = 1;

		pvert = pedges->v[1];

		dist = R_BPlaneDist (pvert, &tplane);

		if (dist > 0)
			side = 0;
		else
			side = 1;

		if (side != lastside)
		{
		// clipped
			if (numbverts >= MAX_BMODEL_VERTS)
			{
				R_ReportBmodelShort ();
				return;
			}

		// generate the clipped vertex
			frac = R_SafeFrac (lastdist / (lastdist - dist));
			ptvert = &pbverts[numbverts++];
			ptvert->position[0] = plastvert->position[0] +
					frac * (pvert->position[0] -
					plastvert->position[0]);
			ptvert->position[1] = plastvert->position[1] +
					frac * (pvert->position[1] -
					plastvert->position[1]);
			ptvert->position[2] = plastvert->position[2] +
					frac * (pvert->position[2] -
					plastvert->position[2]);

		// split into two edges, one on each side, and remember entering
		// and exiting points
		// FIXME: share the clip edge by having a winding direction flag?
			if (numbedges >= (MAX_BMODEL_EDGES - 1))
			{
				R_ReportBmodelShort ();
				return;
			}

			ptedge = &pbedges[numbedges];
			ptedge->pnext = psideedges[lastside];
			psideedges[lastside] = ptedge;
			ptedge->v[0] = plastvert;
			ptedge->v[1] = ptvert;

			ptedge = &pbedges[numbedges + 1];
			ptedge->pnext = psideedges[side];
			psideedges[side] = ptedge;
			ptedge->v[0] = ptvert;
			ptedge->v[1] = pvert;

			numbedges += 2;

			if (side == 0)
			{
			// entering for front, exiting for back
				pfrontenter = ptvert;
				makeclippededge = true;
			}
			else
			{
				pfrontexit = ptvert;
				makeclippededge = true;
			}
		}
		else
		{
		// add the edge to the appropriate side
			pedges->pnext = psideedges[side];
			psideedges[side] = pedges;
		}
	}

// if anything was clipped, reconstitute and add the edges along the clip
// plane to both sides (but in opposite directions)
	if (makeclippededge)
	{
		if (!pfrontenter || !pfrontexit)
		{
			r_bmodelodd++;
			return;
		}

		if (numbedges >= (MAX_BMODEL_EDGES - 2))
		{
			R_ReportBmodelShort ();
			return;
		}

		ptedge = &pbedges[numbedges];
		ptedge->pnext = psideedges[0];
		psideedges[0] = ptedge;
		ptedge->v[0] = pfrontexit;
		ptedge->v[1] = pfrontenter;

		ptedge = &pbedges[numbedges + 1];
		ptedge->pnext = psideedges[1];
		psideedges[1] = ptedge;
		ptedge->v[0] = pfrontenter;
		ptedge->v[1] = pfrontexit;

		numbedges += 2;
	}

// draw or recurse further
recurse:
	for (i=0 ; i<2 ; i++)
	{
		if (psideedges[i])
		{
		// draw if we've reached a non-solid leaf, done if all that's left is a
		// solid leaf, and continue down the tree if it's not a leaf
			pn = pnode->children[i];

		// we're done with this branch if the node or leaf isn't in the PVS
			if (pn->visframe == r_visframecount)
			{
				if (pn->contents < 0)
				{
					if (pn->contents != CONTENTS_SOLID)
					{
						r_currentbkey = ((mleaf_t *)pn)->key;
						R_RenderBmodelFace (psideedges[i], psurf);
					}
				}
				else
				{
					R_RecursiveClipBPoly (psideedges[i], pnode->children[i],
									  psurf);
				}
			}
		}
	}
}


/*
================
R_DrawSolidClippedSubmodelPolygons
================
*/
void R_DrawSolidClippedSubmodelPolygons (model_t *pmodel)
{
	int			i, j, lindex;
	vec_t		dot;
	msurface_t	*psurf;
	int			numsurfaces;
	mplane_t	*pplane;
	static mvertex_t	bverts[MAX_BMODEL_VERTS];
	static bedge_t		bedges[MAX_BMODEL_EDGES];
	bedge_t		*pbedge;
	medge_t		*pedge, *pedges;

// FIXME: use bounding-box-based frustum clipping info?

	psurf = &pmodel->surfaces[pmodel->firstmodelsurface];
	numsurfaces = pmodel->nummodelsurfaces;
	pedges = pmodel->edges;

	for (i=0 ; i<numsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
		//
		// A fence on a brush model that straddles the world's BSP comes through
		// here, not through R_RenderFace: it is cut into pieces along the world's
		// planes and each piece goes into the edge list by R_RenderBmodelFace,
		// which has no idea what a fence is. So it was drawn solid, holes and
		// all, in palette 255's pink -- a walkway made of a func_wall, next to
		// one made of world brushes that looked right.
		//
		// The cutting is there so the edge list can sort the pieces against the
		// world. The fence pass sorts with the z-buffer instead and needs none
		// of it, so the whole face goes to it here, before it is cut.
		//
			if ((psurf->flags & SURF_DRAWMASKED)
				&& R_FenceDeferFace (psurf, r_clipflags))
				continue;

		// FIXME: use bounding-box-based frustum clipping info?

		// copy the edges to bedges, flipping if necessary so always
		// clockwise winding
		// FIXME: if edges and vertices get caches, these assignments must move
		// outside the loop, and overflow checking must be done here
			pbverts = bverts;
			pbedges = bedges;
			numbverts = numbedges = 0;

			if (psurf->numedges > 0)
			{
				pbedge = &bedges[numbedges];
				numbedges += psurf->numedges;

				for (j=0 ; j<psurf->numedges ; j++)
				{
				   lindex = pmodel->surfedges[psurf->firstedge+j];

					if (lindex > 0)
					{
						pedge = &pedges[lindex];
						pbedge[j].v[0] = &r_pcurrentvertbase[pedge->v[0]];
						pbedge[j].v[1] = &r_pcurrentvertbase[pedge->v[1]];
					}
					else
					{
						lindex = -lindex;
						pedge = &pedges[lindex];
						pbedge[j].v[0] = &r_pcurrentvertbase[pedge->v[1]];
						pbedge[j].v[1] = &r_pcurrentvertbase[pedge->v[0]];
					}

					pbedge[j].pnext = &pbedge[j+1];
				}

				pbedge[j-1].pnext = NULL;	// mark end of edges

				R_RecursiveClipBPoly (pbedge, currententity->topnode, psurf);
			}
			else
			{
				Sys_Error ("no edges in bmodel");
			}
		}
	}
}


/*
================
R_DrawSubmodelPolygons
================
*/
void R_DrawSubmodelPolygons (model_t *pmodel, int clipflags)
{
	int			i;
	vec_t		dot;
	msurface_t	*psurf;
	int			numsurfaces;
	mplane_t	*pplane;

// FIXME: use bounding-box-based frustum clipping info?

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
			r_currentkey = ((mleaf_t *)currententity->topnode)->key;

		// FIXME: use bounding-box-based frustum clipping info?
			R_RenderFace (psurf, clipflags);
		}
	}
}


/*
================
R_RecursiveWorldNode
================
*/
void R_RecursiveWorldNode (mnode_t *node, int clipflags)
{
	int			i, c, side, *pindex;
	vec3_t		acceptpt, rejectpt;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		d, dot;

	if (node->contents == CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != r_visframecount)
		return;

// cull the clipping planes if not trivial accept
// FIXME: the compiler is doing a lousy job of optimizing here; it could be
//  twice as fast in ASM
	if (clipflags)
	{
		for (i=0 ; i<4 ; i++)
		{
			if (! (clipflags & (1<<i)) )
				continue;	// don't need to clip against it

		// generate accept and reject points
		// FIXME: do with fast look-ups or integer tests based on the sign bit
		// of the floating point values

			pindex = pfrustum_indexes[i];

			rejectpt[0] = (float)node->minmaxs[pindex[0]];
			rejectpt[1] = (float)node->minmaxs[pindex[1]];
			rejectpt[2] = (float)node->minmaxs[pindex[2]];
			
			d = DotProduct (rejectpt, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d <= 0)
				return;

			acceptpt[0] = (float)node->minmaxs[pindex[3+0]];
			acceptpt[1] = (float)node->minmaxs[pindex[3+1]];
			acceptpt[2] = (float)node->minmaxs[pindex[3+2]];

			d = DotProduct (acceptpt, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d >= 0)
				clipflags &= ~(1<<i);	// node is entirely on screen
		}
	}
	
// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			} while (--c);
		}

	// deal with model fragments in this leaf
		if (pleaf->efrags)
		{
			R_StoreEfrags (&pleaf->efrags);
		}

		pleaf->key = r_currentkey;
		r_currentkey++;		// all bmodels in a leaf share the same key
	}
	else
	{
	// node is just a decision point, so go down the apropriate sides

	// find which side of the node we are on
		plane = node->plane;

		switch (plane->type)
		{
		case PLANE_X:
			dot = modelorg[0] - plane->dist;
			break;
		case PLANE_Y:
			dot = modelorg[1] - plane->dist;
			break;
		case PLANE_Z:
			dot = modelorg[2] - plane->dist;
			break;
		default:
			dot = DotProduct (modelorg, plane->normal) - plane->dist;
			break;
		}
	
		if (dot >= 0)
			side = 0;
		else
			side = 1;

	// recurse down the children, front side first
		R_RecursiveWorldNode (node->children[side], clipflags);

	// draw stuff
		c = node->numsurfaces;

		if (c)
		{
			surf = cl.worldmodel->surfaces + node->firstsurface;

			if (dot < -BACKFACE_EPSILON)
			{
				do
				{
					if ((surf->flags & SURF_PLANEBACK) &&
						(surf->visframe == r_framecount))
					{
						if (r_drawpolys)
						{
							if (r_worldpolysbacktofront)
							{
								if (numbtofpolys < MAX_BTOFPOLYS)
								{
									pbtofpolys[numbtofpolys].clipflags =
											clipflags;
									pbtofpolys[numbtofpolys].psurf = surf;
									numbtofpolys++;
								}
							}
							else
							{
								R_RenderPoly (surf, clipflags);
							}
						}
						else
						{
							R_RenderFace (surf, clipflags);
						}
					}

					surf++;
				} while (--c);
			}
			else if (dot > BACKFACE_EPSILON)
			{
				do
				{
					if (!(surf->flags & SURF_PLANEBACK) &&
						(surf->visframe == r_framecount))
					{
						if (r_drawpolys)
						{
							if (r_worldpolysbacktofront)
							{
								if (numbtofpolys < MAX_BTOFPOLYS)
								{
									pbtofpolys[numbtofpolys].clipflags =
											clipflags;
									pbtofpolys[numbtofpolys].psurf = surf;
									numbtofpolys++;
								}
							}
							else
							{
								R_RenderPoly (surf, clipflags);
							}
						}
						else
						{
							R_RenderFace (surf, clipflags);
						}
					}

					surf++;
				} while (--c);
			}

		// all surfaces on the same node share the same sequence number
			r_currentkey++;
		}

	// recurse down the back side
		R_RecursiveWorldNode (node->children[!side], clipflags);
	}
}



/*
================
R_RenderWorld
================
*/
void R_RenderWorld (void)
{
	int			i;
	model_t		*clmodel;
	btofpoly_t	btofpolys[MAX_BTOFPOLYS];

	pbtofpolys = btofpolys;

	currententity = &cl_entities[0];
	VectorCopy (r_origin, modelorg);
	clmodel = currententity->model;
	r_pcurrentvertbase = clmodel->vertexes;

	R_RecursiveWorldNode (clmodel->nodes, 15);

// if the driver wants the polygons back to front, play the visible ones back
// in that order
	if (r_worldpolysbacktofront)
	{
		for (i=numbtofpolys-1 ; i>=0 ; i--)
		{
			R_RenderPoly (btofpolys[i].psurf, btofpolys[i].clipflags);
		}
	}
}



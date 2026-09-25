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
//
// model_md5.c -- the re-release's enhanced models, as alias models.
//
// The re-release draws its monsters and weapons from better models than
// 1996's when Enhanced Models is on. They sit beside the originals in
// id1/pak0.pak: progs/soldier.mdl has progs/soldier.md5mesh, a skeletal mesh,
// and progs/soldier.md5anim, one skeleton pose for each of the .mdl's frames,
// so the frame numbers the game sets mean the same pose in either. The skins
// are .lmp pictures, progs/<shader>_<skin>_<frame>.lmp, in Quake's palette.
//
// This renderer draws alias models, one set of vertex positions a frame. So
// that is what an enhanced model is turned into, once, as it is loaded: each
// pose of the skeleton is worked out into vertex positions, stored as a frame
// of a .mdl written in memory, and that .mdl goes through Mod_LoadAliasModel
// like any other. Everything after -- lighting, interpolation, the drawing --
// sees an alias model with more triangles than usual.
//
// What is kept from the original .mdl is everything the game sees of it: its
// frame count, which the conversion refuses to change; its flags, which give
// rockets their trails and items their spin; and the eye position. An enhanced
// model found further down the search path than its .mdl -- a mod's own model
// over the base game's enhanced one -- is not used, as the re-release does not.
//

#include "quakedef.h"
#include "r_local.h"

cvar_t	r_enhancedmodels = {"r_enhancedmodels", "0", true};

#define	NUMVERTEXNORMALS	162
extern float	r_avertexnormals[NUMVERTEXNORMALS][3];	// r_alias.c

#define	MD5_MAXJOINTS	2048
#define	MD5_MAXMESHES	16
#define	MD5_MAXSKINS	8

typedef struct
{
	vec3_t	pos;
	float	q[4];			// x y z w
} md5joint_t;

typedef struct
{
	float	s, t;
	int		firstweight, numweights;
} md5vert_t;

typedef struct
{
	int		joint;
	float	bias;
	vec3_t	pos;
} md5weight_t;

typedef struct
{
	char		shader[MAX_QPATH];
	int			numverts, numtris, numweights;
	md5vert_t	*verts;
	int			*tris;			// 3 a triangle
	md5weight_t	*weights;
	int			firstvert;		// in the model's single vertex list
	int			skiny;			// where its skin starts in the model's
} md5mesh_t;

typedef struct
{
	int			numjoints, nummeshes;
	md5joint_t	bind[MD5_MAXJOINTS];
	int			parent[MD5_MAXJOINTS];
	md5mesh_t	mesh[MD5_MAXMESHES];
} md5model_t;

typedef struct
{
	int			numframes, numjoints, numcomponents;
	int			parent[MD5_MAXJOINTS], flags[MD5_MAXJOINTS], start[MD5_MAXJOINTS];
	md5joint_t	base[MD5_MAXJOINTS];
	float		*components;	// numframes * numcomponents
} md5anim_t;

static char	*md5_name;		// for the messages
static qboolean	md5_bad;

// A file from the search path into a malloc'd, NUL-terminated buffer, and how
// far down the path it was.
static char *MD5_LoadFile (char *name, int *depth)
{
	FILE	*f;
	int		len;
	char	*buf;

	len = COM_FOpenFile (name, &f);
	if (len < 0 || !f)
		return NULL;
	*depth = com_filedepth;
	buf = malloc (len + 1);
	if (!buf || (int)fread (buf, 1, len, f) != len)
	{
		free (buf);
		fclose (f);
		return NULL;
	}
	fclose (f);
	buf[len] = 0;
	return buf;
}

//
// The text, a token at a time. A token that is not the one expected makes the
// file bad; the parse runs on to the end without doing harm and the model
// falls back to its .mdl.
//
static char	*md5_p;

static char *MD5_Token (void)
{
	md5_p = COM_Parse (md5_p);
	if (!md5_p)
	{
		md5_bad = true;
		com_token[0] = 0;
	}
	return com_token;
}

static void MD5_Expect (char *what)
{
	if (strcmp (MD5_Token (), what))
	{
		if (!md5_bad)
			Con_DPrintf ("%s: expected \"%s\", found \"%s\"\n", md5_name, what,
						 com_token);
		md5_bad = true;
	}
}

static float MD5_Float (void)
{
	return atof (MD5_Token ());
}

static int MD5_Int (void)
{
	return atoi (MD5_Token ());
}

// Doom 3's quaternions leave w out; it is the negative root that makes them unit.
static void MD5_CompleteQuat (float *q)
{
	float	t = 1 - q[0]*q[0] - q[1]*q[1] - q[2]*q[2];

	q[3] = t < 0 ? 0 : -sqrt (t);
}

static void MD5_QuatMul (float *a, float *b, float *out)
{
	float	r[4];

	r[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
	r[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
	r[1] = a[3]*b[1] + a[1]*b[3] + a[2]*b[0] - a[0]*b[2];
	r[2] = a[3]*b[2] + a[2]*b[3] + a[0]*b[1] - a[1]*b[0];
	out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = r[3];
}

static void MD5_Rotate (float *q, vec3_t v, vec3_t out)
{
	vec3_t	t, c;

// v + 2w(q x v) + 2 q x (q x v)
	t[0] = 2 * (q[1]*v[2] - q[2]*v[1]);
	t[1] = 2 * (q[2]*v[0] - q[0]*v[2]);
	t[2] = 2 * (q[0]*v[1] - q[1]*v[0]);
	c[0] = q[1]*t[2] - q[2]*t[1];
	c[1] = q[2]*t[0] - q[0]*t[2];
	c[2] = q[0]*t[1] - q[1]*t[0];
	out[0] = v[0] + q[3]*t[0] + c[0];
	out[1] = v[1] + q[3]*t[1] + c[1];
	out[2] = v[2] + q[3]*t[2] + c[2];
}

static void MD5_FreeModel (md5model_t *m)
{
	int		i;

	for (i = 0 ; i < m->nummeshes ; i++)
	{
		free (m->mesh[i].verts);
		free (m->mesh[i].tris);
		free (m->mesh[i].weights);
	}
	free (m);
}

/*
================
MD5_ParseMesh
================
*/
static md5model_t *MD5_ParseMesh (char *text)
{
	md5model_t	*m;
	md5mesh_t	*mesh;
	int			i, j, n;

	m = calloc (1, sizeof(*m));
	if (!m)
		return NULL;
	md5_p = text;
	md5_bad = false;

	MD5_Expect ("MD5Version");
	MD5_Expect ("10");
	MD5_Token ();
	if (!strcmp (com_token, "commandline"))
		MD5_Token (), MD5_Token ();
	if (strcmp (com_token, "numJoints"))
		md5_bad = true;
	m->numjoints = MD5_Int ();
	MD5_Expect ("numMeshes");
	m->nummeshes = MD5_Int ();
	if (m->numjoints < 1 || m->numjoints > MD5_MAXJOINTS
		|| m->nummeshes < 1 || m->nummeshes > MD5_MAXMESHES)
	{
		Con_DPrintf ("%s: %d joints and %d meshes; at most %d and %d\n",
					 md5_name, m->numjoints, m->nummeshes, MD5_MAXJOINTS,
					 MD5_MAXMESHES);
		m->nummeshes = 0;
		MD5_FreeModel (m);
		return NULL;
	}

	MD5_Expect ("joints");
	MD5_Expect ("{");
	for (i = 0 ; i < m->numjoints && !md5_bad ; i++)
	{
		MD5_Token ();			// the name
		m->parent[i] = MD5_Int ();
		MD5_Expect ("(");
		for (j = 0 ; j < 3 ; j++)
			m->bind[i].pos[j] = MD5_Float ();
		MD5_Expect (")");
		MD5_Expect ("(");
		for (j = 0 ; j < 3 ; j++)
			m->bind[i].q[j] = MD5_Float ();
		MD5_Expect (")");
		MD5_CompleteQuat (m->bind[i].q);
	}
	MD5_Expect ("}");

	for (n = 0 ; n < m->nummeshes && !md5_bad ; n++)
	{
		mesh = &m->mesh[n];
		MD5_Expect ("mesh");
		MD5_Expect ("{");
		MD5_Expect ("shader");
		Q_strncpy (mesh->shader, MD5_Token (), sizeof(mesh->shader) - 1);

		MD5_Expect ("numverts");
		mesh->numverts = MD5_Int ();
		if (mesh->numverts < 1 || mesh->numverts > MAXALIASVERTS)
			md5_bad = true;
		mesh->verts = calloc (mesh->numverts > 0 ? mesh->numverts : 1,
							  sizeof(md5vert_t));
		for (i = 0 ; i < mesh->numverts && !md5_bad ; i++)
		{
			MD5_Expect ("vert");
			j = MD5_Int ();
			if (j < 0 || j >= mesh->numverts)
			{
				md5_bad = true;
				break;
			}
			MD5_Expect ("(");
			mesh->verts[j].s = MD5_Float ();
			mesh->verts[j].t = MD5_Float ();
			MD5_Expect (")");
			mesh->verts[j].firstweight = MD5_Int ();
			mesh->verts[j].numweights = MD5_Int ();
		}

		MD5_Expect ("numtris");
		mesh->numtris = MD5_Int ();
		if (mesh->numtris < 1)
			md5_bad = true;
		mesh->tris = calloc (mesh->numtris > 0 ? mesh->numtris*3 : 3, sizeof(int));
		for (i = 0 ; i < mesh->numtris && !md5_bad ; i++)
		{
			MD5_Expect ("tri");
			j = MD5_Int ();
			if (j < 0 || j >= mesh->numtris)
			{
				md5_bad = true;
				break;
			}
			mesh->tris[j*3+0] = MD5_Int ();
			mesh->tris[j*3+1] = MD5_Int ();
			mesh->tris[j*3+2] = MD5_Int ();
		}

		MD5_Expect ("numweights");
		mesh->numweights = MD5_Int ();
		if (mesh->numweights < 1)
			md5_bad = true;
		mesh->weights = calloc (mesh->numweights > 0 ? mesh->numweights : 1,
								sizeof(md5weight_t));
		for (i = 0 ; i < mesh->numweights && !md5_bad ; i++)
		{
			MD5_Expect ("weight");
			j = MD5_Int ();
			if (j < 0 || j >= mesh->numweights)
			{
				md5_bad = true;
				break;
			}
			mesh->weights[j].joint = MD5_Int ();
			mesh->weights[j].bias = MD5_Float ();
			MD5_Expect ("(");
			mesh->weights[j].pos[0] = MD5_Float ();
			mesh->weights[j].pos[1] = MD5_Float ();
			mesh->weights[j].pos[2] = MD5_Float ();
			MD5_Expect (")");
		}
		MD5_Expect ("}");

	// every index in range, now rather than while drawing
		for (i = 0 ; i < mesh->numtris*3 && !md5_bad ; i++)
			if (mesh->tris[i] < 0 || mesh->tris[i] >= mesh->numverts)
				md5_bad = true;
		for (i = 0 ; i < mesh->numverts && !md5_bad ; i++)
			if (mesh->verts[i].firstweight < 0 || mesh->verts[i].numweights < 0
				|| mesh->verts[i].firstweight + mesh->verts[i].numweights
					> mesh->numweights)
				md5_bad = true;
		for (i = 0 ; i < mesh->numweights && !md5_bad ; i++)
			if (mesh->weights[i].joint < 0
				|| mesh->weights[i].joint >= m->numjoints)
				md5_bad = true;
	}
	m->nummeshes = n;

	if (md5_bad)
	{
		Con_DPrintf ("%s: not a mesh this can read\n", md5_name);
		MD5_FreeModel (m);
		return NULL;
	}
	return m;
}

/*
================
MD5_ParseAnim
================
*/
static qboolean MD5_ParseAnim (char *text, md5model_t *m, md5anim_t *a)
{
	int		i, j, k;

	md5_p = text;
	md5_bad = false;
	memset (a, 0, sizeof(*a));

	MD5_Expect ("MD5Version");
	MD5_Expect ("10");
	MD5_Token ();
	if (!strcmp (com_token, "commandline"))
		MD5_Token (), MD5_Token ();
	if (strcmp (com_token, "numFrames"))
		md5_bad = true;
	a->numframes = MD5_Int ();
	MD5_Expect ("numJoints");
	a->numjoints = MD5_Int ();
	MD5_Expect ("frameRate");
	MD5_Token ();
	MD5_Expect ("numAnimatedComponents");
	a->numcomponents = MD5_Int ();

	if (md5_bad || a->numframes < 1 || a->numjoints != m->numjoints
		|| a->numcomponents < 0 || a->numcomponents > 6 * MD5_MAXJOINTS)
	{
		Con_DPrintf ("%s: its animation does not fit its mesh\n", md5_name);
		return false;
	}

	MD5_Expect ("hierarchy");
	MD5_Expect ("{");
	for (i = 0 ; i < a->numjoints && !md5_bad ; i++)
	{
		MD5_Token ();			// the name
		a->parent[i] = MD5_Int ();
		a->flags[i] = MD5_Int ();
		a->start[i] = MD5_Int ();
		if (a->parent[i] >= i || a->start[i] < 0)
			md5_bad = true;		// parents come first, as they must
		for (j = 0, k = 0 ; j < 6 ; j++)
			if (a->flags[i] & (1 << j))
				k++;
		if (a->start[i] + k > a->numcomponents)
			md5_bad = true;
	}
	MD5_Expect ("}");

	MD5_Expect ("bounds");
	MD5_Expect ("{");
	for (i = 0 ; i < a->numframes && !md5_bad ; i++)
		for (j = 0 ; j < 10 ; j++)		// ( min ) ( max ), unused
			MD5_Token ();
	MD5_Expect ("}");

	MD5_Expect ("baseframe");
	MD5_Expect ("{");
	for (i = 0 ; i < a->numjoints && !md5_bad ; i++)
	{
		MD5_Expect ("(");
		for (j = 0 ; j < 3 ; j++)
			a->base[i].pos[j] = MD5_Float ();
		MD5_Expect (")");
		MD5_Expect ("(");
		for (j = 0 ; j < 3 ; j++)
			a->base[i].q[j] = MD5_Float ();
		MD5_Expect (")");
	}
	MD5_Expect ("}");

	if (md5_bad)
		return false;

	a->components = calloc ((size_t)a->numframes * (a->numcomponents ? a->numcomponents : 1),
							sizeof(float));
	if (!a->components)
		return false;
	for (i = 0 ; i < a->numframes && !md5_bad ; i++)
	{
		MD5_Expect ("frame");
		k = MD5_Int ();
		if (k < 0 || k >= a->numframes)
		{
			md5_bad = true;
			break;
		}
		MD5_Expect ("{");
		for (j = 0 ; j < a->numcomponents ; j++)
			a->components[k * a->numcomponents + j] = MD5_Float ();
		MD5_Expect ("}");
	}

	if (md5_bad)
	{
		Con_DPrintf ("%s: its animation is not one this can read\n", md5_name);
		free (a->components);
		a->components = NULL;
		return false;
	}
	return true;
}

// The skeleton in one frame, each joint in the model's own space.
static void MD5_PoseFrame (md5anim_t *a, int frame, md5joint_t *out)
{
	int			i, k, bit;
	float		*c;
	md5joint_t	local;

	c = a->components + (size_t)frame * a->numcomponents;
	for (i = 0 ; i < a->numjoints ; i++)
	{
		local = a->base[i];
		k = a->start[i];
		for (bit = 0 ; bit < 3 ; bit++)
			if (a->flags[i] & (1 << bit))
				local.pos[bit] = c[k++];
		for (bit = 0 ; bit < 3 ; bit++)
			if (a->flags[i] & (8 << bit))
				local.q[bit] = c[k++];
		MD5_CompleteQuat (local.q);

		if (a->parent[i] < 0)
			out[i] = local;
		else
		{
			md5joint_t	*p = &out[a->parent[i]];
			vec3_t		r;
			float		len;

			MD5_Rotate (p->q, local.pos, r);
			VectorAdd (p->pos, r, out[i].pos);
			MD5_QuatMul (p->q, local.q, out[i].q);
			len = sqrt (out[i].q[0]*out[i].q[0] + out[i].q[1]*out[i].q[1]
						+ out[i].q[2]*out[i].q[2] + out[i].q[3]*out[i].q[3]);
			if (len > 0)
				for (k = 0 ; k < 4 ; k++)
					out[i].q[k] /= len;
		}
	}
}

// Where a vertex is, given the skeleton: each weight is a point in its joint's
// space, and the vertex is their blend.
static void MD5_SkinVertex (md5mesh_t *mesh, md5vert_t *v, md5joint_t *joints,
	vec3_t out)
{
	int			i;
	md5weight_t	*w;
	vec3_t		p;

	out[0] = out[1] = out[2] = 0;
	for (i = 0 ; i < v->numweights ; i++)
	{
		w = &mesh->weights[v->firstweight + i];
		MD5_Rotate (joints[w->joint].q, w->pos, p);
		VectorAdd (p, joints[w->joint].pos, p);
		VectorMA (out, w->bias, p, out);
	}
}

//
// The nearest of the 162 lighting normals the renderer knows, through a
// 32x32x32 table of directions built the first time it is needed: each frame
// of an enhanced model has thousands of vertices, and searching the 162 for
// each is too slow to do for every model a map loads.
//
#define	NTAB	32
static byte		md5_normtab[NTAB][NTAB][NTAB];
static qboolean	md5_normtabmade;

static int MD5_NormalIndex (vec3_t n)
{
	int		i, j, k, x, best;
	float	d, bestd;
	vec3_t	dir;

	if (!md5_normtabmade)
	{
		for (i = 0 ; i < NTAB ; i++)
			for (j = 0 ; j < NTAB ; j++)
				for (k = 0 ; k < NTAB ; k++)
				{
					dir[0] = (i + 0.5) / NTAB * 2 - 1;
					dir[1] = (j + 0.5) / NTAB * 2 - 1;
					dir[2] = (k + 0.5) / NTAB * 2 - 1;
					best = 0;
					bestd = -2;
					for (x = 0 ; x < NUMVERTEXNORMALS ; x++)
					{
						d = DotProduct (dir, r_avertexnormals[x]);
						if (d > bestd)
						{
							bestd = d;
							best = x;
						}
					}
					md5_normtab[i][j][k] = best;
				}
		md5_normtabmade = true;
	}

	d = sqrt (DotProduct (n, n));
	if (d <= 0)
		return 0;
	i = (int)((n[0] / d + 1) * 0.5 * NTAB);
	j = (int)((n[1] / d + 1) * 0.5 * NTAB);
	k = (int)((n[2] / d + 1) * 0.5 * NTAB);
	i = i < 0 ? 0 : (i >= NTAB ? NTAB - 1 : i);
	j = j < 0 ? 0 : (j >= NTAB ? NTAB - 1 : j);
	k = k < 0 ? 0 : (k >= NTAB ? NTAB - 1 : k);
	return md5_normtab[i][j][k];
}

// A skin: an .lmp, two ints and then the pixels.
static byte *MD5_LoadSkin (char *shader, int skin, int *w, int *h)
{
	char	name[MAX_QPATH * 2], base[MAX_QPATH];
	char	*data, *e;
	int		depth, width, height;
	byte	*pix;

// "progs/soldier" or "soldier", either way the file is progs/soldier_00_00
	Q_strncpy (base, shader, sizeof(base) - 1);
	base[sizeof(base) - 1] = 0;
	e = strrchr (base, '.');
	if (e && !strchr (e, '/'))
		*e = 0;
	if (!strncmp (base, "progs/", 6))
		snprintf (name, sizeof(name), "%s_%02d_00.lmp", base, skin);
	else
		snprintf (name, sizeof(name), "progs/%s_%02d_00.lmp", base, skin);

	data = MD5_LoadFile (name, &depth);
	if (!data)
		return NULL;
	width = LittleLong (((int *)data)[0]);
	height = LittleLong (((int *)data)[1]);
	if (width < 1 || height < 1 || width > 4096 || height > 4096
		|| com_filesize < 8 + width * height)
	{
		Con_DPrintf ("%s is not a picture this can read\n", name);
		free (data);
		return NULL;
	}
	pix = malloc (width * height);
	if (pix)
		memcpy (pix, data + 8, width * height);
	free (data);
	*w = width;
	*h = height;
	return pix;
}

/*
================
MD5_BuildMDL

The .mdl, in memory, from the mesh, its animation and its skins; mdl is the
original's header, for what is kept of it. Returns a malloc'd file, or NULL.
================
*/
static byte *MD5_BuildMDL (md5model_t *m, md5anim_t *a, mdl_t *orig,
	daliasframetype_t *origframes, int *outsize)
{
	int					i, j, k, n, f, numverts, numtris, numframes, numskins;
	int					skinw, skinh, w[MD5_MAXMESHES], h[MD5_MAXMESHES];
	byte				*skins[MD5_MAXSKINS][MD5_MAXMESHES];
	vec3_t				*pos, *norm, mins, maxs, e1, e2, fn;
	md5joint_t			*joints;
	float				radius, d;
	int					size;
	byte				*file, *p;
	mdl_t				*hdr;
	stvert_t			*st;
	dtriangle_t			*tri;
	md5mesh_t			*mesh;

	memset (skins, 0, sizeof(skins));

// the skins: as many as every mesh has, each mesh's stacked below the last
	numskins = 0;
	for (k = 0 ; k < MD5_MAXSKINS ; k++)
	{
		for (n = 0 ; n < m->nummeshes ; n++)
		{
			int		sw, sh;

			skins[k][n] = MD5_LoadSkin (m->mesh[n].shader, k, &sw, &sh);
			if (!skins[k][n])
				break;
			if (k == 0)
			{
				w[n] = sw;
				h[n] = sh;
			}
			else if (sw != w[n] || sh != h[n])
			{
				free (skins[k][n]);
				skins[k][n] = NULL;
				break;
			}
		}
		if (n < m->nummeshes)
		{
			for (n-- ; n >= 0 ; n--)
				free (skins[k][n]), skins[k][n] = NULL;
			break;
		}
		numskins++;
	}
	if (!numskins)
	{
		Con_DPrintf ("%s: no skin for it (progs/%s_00_00.lmp)\n", md5_name,
					 m->mesh[0].shader);
		return NULL;
	}

	skinw = 0;
	skinh = 0;
	numverts = numtris = 0;
	for (n = 0 ; n < m->nummeshes ; n++)
	{
		if (w[n] > skinw)
			skinw = w[n];
		m->mesh[n].skiny = skinh;
		skinh += h[n];
		m->mesh[n].firstvert = numverts;
		numverts += m->mesh[n].numverts;
		numtris += m->mesh[n].numtris;
	}
	skinw = (skinw + 3) & ~3;		// the renderer's rule
	numframes = a ? a->numframes : 1;

	if (numverts > MAXALIASVERTS || skinh > MAX_LBM_HEIGHT)
	{
		Con_DPrintf ("%s: %d vertices and a skin %d high; at most %d and %d\n",
					 md5_name, numverts, skinh, MAXALIASVERTS, MAX_LBM_HEIGHT);
		for (k = 0 ; k < numskins ; k++)
			for (n = 0 ; n < m->nummeshes ; n++)
				free (skins[k][n]);
		return NULL;
	}

// every frame's vertices, and the box around all of them
	pos = malloc (sizeof(vec3_t) * numverts * numframes);
	norm = malloc (sizeof(vec3_t) * numverts);
	joints = malloc (sizeof(md5joint_t) * m->numjoints);
	if (!pos || !norm || !joints)
	{
		free (pos); free (norm); free (joints);
		for (k = 0 ; k < numskins ; k++)
			for (n = 0 ; n < m->nummeshes ; n++)
				free (skins[k][n]);
		return NULL;
	}

	mins[0] = mins[1] = mins[2] = 999999;
	maxs[0] = maxs[1] = maxs[2] = -999999;
	radius = 0;
	for (f = 0 ; f < numframes ; f++)
	{
		if (a)
			MD5_PoseFrame (a, f, joints);
		else
			memcpy (joints, m->bind, sizeof(md5joint_t) * m->numjoints);

		for (n = 0 ; n < m->nummeshes ; n++)
		{
			mesh = &m->mesh[n];
			for (i = 0 ; i < mesh->numverts ; i++)
			{
				float	*v = pos[f * numverts + mesh->firstvert + i];

				MD5_SkinVertex (mesh, &mesh->verts[i], joints, v);
				for (j = 0 ; j < 3 ; j++)
				{
					if (v[j] < mins[j])
						mins[j] = v[j];
					if (v[j] > maxs[j])
						maxs[j] = v[j];
				}
				d = DotProduct (v, v);
				if (d > radius)
					radius = d;
			}
		}
	}
	radius = sqrt (radius);

// the file
	size = sizeof(mdl_t)
		+ numskins * (sizeof(daliasskintype_t) + skinw * skinh)
		+ numverts * sizeof(stvert_t)
		+ numtris * sizeof(dtriangle_t)
		+ numframes * (sizeof(daliasframetype_t) + sizeof(daliasframe_t)
					   + numverts * sizeof(trivertx_t));
	file = calloc (1, size);
	if (!file)
	{
		free (pos); free (norm); free (joints);
		for (k = 0 ; k < numskins ; k++)
			for (n = 0 ; n < m->nummeshes ; n++)
				free (skins[k][n]);
		return NULL;
	}

	hdr = (mdl_t *)file;
	hdr->ident = LittleLong (IDPOLYHEADER);
	hdr->version = LittleLong (ALIAS_VERSION);
	for (j = 0 ; j < 3 ; j++)
	{
		float	range = maxs[j] - mins[j];

		hdr->scale[j] = LittleFloat (range > 0 ? range / 255 : 1.0 / 255);
		hdr->scale_origin[j] = LittleFloat (mins[j]);
		hdr->eyeposition[j] = orig->eyeposition[j];		// still little-endian
	}
	hdr->boundingradius = LittleFloat (radius);
	hdr->numskins = LittleLong (numskins);
	hdr->skinwidth = LittleLong (skinw);
	hdr->skinheight = LittleLong (skinh);
	hdr->numverts = LittleLong (numverts);
	hdr->numtris = LittleLong (numtris);
	hdr->numframes = LittleLong (numframes);
	hdr->synctype = orig->synctype;
	hdr->flags = orig->flags;
	hdr->size = orig->size;
	p = (byte *)&hdr[1];

	for (k = 0 ; k < numskins ; k++)
	{
		((daliasskintype_t *)p)->type = LittleLong (ALIAS_SKIN_SINGLE);
		p += sizeof(daliasskintype_t);
		for (n = 0 ; n < m->nummeshes ; n++)
			for (i = 0 ; i < h[n] ; i++)
				memcpy (p + (m->mesh[n].skiny + i) * skinw,
						skins[k][n] + i * w[n], w[n]);
		p += skinw * skinh;
	}

// Texture coordinates run 0 to 1 down and across the mesh's own skin, and a
// .mdl's are texels across the whole of it. A vertex on a seam is already two
// vertices in an MD5, so nothing is on a seam here.
	st = (stvert_t *)p;
	for (n = 0 ; n < m->nummeshes ; n++)
	{
		mesh = &m->mesh[n];
		for (i = 0 ; i < mesh->numverts ; i++)
		{
			int		s = (int)(mesh->verts[i].s * w[n]);
			int		t = (int)(mesh->verts[i].t * h[n]);

			s = s < 0 ? 0 : (s >= w[n] ? w[n] - 1 : s);
			t = t < 0 ? 0 : (t >= h[n] ? h[n] - 1 : t);
			st[mesh->firstvert + i].onseam = 0;
			st[mesh->firstvert + i].s = LittleLong (s);
			st[mesh->firstvert + i].t = LittleLong (t + mesh->skiny);
		}
	}
	p = (byte *)&st[numverts];

	tri = (dtriangle_t *)p;
	for (n = 0, k = 0 ; n < m->nummeshes ; n++)
	{
		mesh = &m->mesh[n];
		for (i = 0 ; i < mesh->numtris ; i++, k++)
		{
			tri[k].facesfront = LittleLong (1);
			for (j = 0 ; j < 3 ; j++)
				tri[k].vertindex[j] = LittleLong (mesh->firstvert + mesh->tris[i*3+j]);
		}
	}
	p = (byte *)&tri[numtris];

	for (f = 0 ; f < numframes ; f++)
	{
		daliasframe_t	*fr;
		trivertx_t		*tv;
		vec3_t			*fp = &pos[f * numverts];
		int				bmin[3] = {255, 255, 255}, bmax[3] = {0, 0, 0};

		((daliasframetype_t *)p)->type = LittleLong (ALIAS_SINGLE);
		p += sizeof(daliasframetype_t);
		fr = (daliasframe_t *)p;
		p += sizeof(daliasframe_t);
		tv = (trivertx_t *)p;
		p += numverts * sizeof(trivertx_t);

	// the name the original gave the frame, where it is a single frame
		snprintf (fr->name, sizeof(fr->name), "frame%d", f);
		if (origframes && LittleLong (origframes->type) == ALIAS_SINGLE)
		{
			daliasframe_t	*of = (daliasframe_t *)&origframes[1];

			memcpy (fr->name, of->name, sizeof(fr->name));
			origframes = (daliasframetype_t *)((byte *)&of[1]
				+ LittleLong (orig->numverts) * sizeof(trivertx_t));
		}
		else
			origframes = NULL;

	// normals: each vertex, the sum of the faces around it
		memset (norm, 0, sizeof(vec3_t) * numverts);
		for (k = 0 ; k < numtris ; k++)
		{
			int	*vi = tri[k].vertindex;		// written little-endian above

			VectorSubtract (fp[LittleLong (vi[1])], fp[LittleLong (vi[0])], e1);
			VectorSubtract (fp[LittleLong (vi[2])], fp[LittleLong (vi[0])], e2);
		// as QuakeSpasm-Spiked works them out for these models, each face
		// counting the same whatever its size
			CrossProduct (e1, e2, fn);
			VectorNormalize (fn);
			for (j = 0 ; j < 3 ; j++)
				VectorAdd (norm[LittleLong (vi[j])], fn, norm[LittleLong (vi[j])]);
		}

		for (i = 0 ; i < numverts ; i++)
		{
			for (j = 0 ; j < 3 ; j++)
			{
				int	v = (int)((fp[i][j] - mins[j])
							  / LittleFloat (hdr->scale[j]) + 0.5);

				v = v < 0 ? 0 : (v > 255 ? 255 : v);
				tv[i].v[j] = v;
				if (v < bmin[j])
					bmin[j] = v;
				if (v > bmax[j])
					bmax[j] = v;
			}
			tv[i].lightnormalindex = MD5_NormalIndex (norm[i]);
		}
		for (j = 0 ; j < 3 ; j++)
		{
			fr->bboxmin.v[j] = bmin[j];
			fr->bboxmax.v[j] = bmax[j];
		}
	}

	free (pos);
	free (norm);
	free (joints);
	for (k = 0 ; k < numskins ; k++)
		for (n = 0 ; n < m->nummeshes ; n++)
			free (skins[k][n]);

	*outsize = size;
	return file;
}

/*
================
Mod_EnhancedModel

Called with an alias model's .mdl as it was read. Returns a malloc'd .mdl made
from its enhanced model to load instead, or NULL to load the original. mdldepth
is how far down the search path the .mdl was found.
================
*/
byte *Mod_EnhancedModel (model_t *mod, byte *mdlbuf, int mdldepth)
{
	char			name[MAX_QPATH * 2], *e, *meshtext, *animtext;
	int				depth, animdepth, size;
	mdl_t			*orig = (mdl_t *)mdlbuf;
	md5model_t		*m;
	md5anim_t		a;
	qboolean		haveanim;
	byte			*file;
	daliasframetype_t	*origframes;
	byte			*p;
	int				i;

	if (!r_enhancedmodels.value)
		return NULL;
	if (LittleLong (orig->ident) != IDPOLYHEADER
		|| LittleLong (orig->version) != ALIAS_VERSION)
		return NULL;

	Q_strncpy (name, mod->name, sizeof(name) - 16);
	e = strrchr (name, '.');
	if (!e || strcmp (e, ".mdl"))
		return NULL;

	strcpy (e, ".md5mesh");
	meshtext = MD5_LoadFile (name, &depth);
	if (!meshtext)
		return NULL;
	if (depth > mdldepth)
	{
	// a mod's own .mdl over the base game's enhanced one: the mod's wins
		free (meshtext);
		return NULL;
	}
	md5_name = mod->name;

	m = MD5_ParseMesh (meshtext);
	free (meshtext);
	if (!m)
		return NULL;

	strcpy (e, ".md5anim");
	animtext = MD5_LoadFile (name, &animdepth);
	haveanim = false;
	if (animtext)
	{
		haveanim = MD5_ParseAnim (animtext, m, &a);
		free (animtext);
		if (!haveanim)
		{
			MD5_FreeModel (m);
			return NULL;
		}
	}

// the frames the game asks for by number have to be the same frames
	if ((haveanim ? a.numframes : 1) != LittleLong (orig->numframes))
	{
		Con_DPrintf ("%s: its enhanced model has %d frames and it has %d; "
					 "not used\n", mod->name, haveanim ? a.numframes : 1,
					 LittleLong (orig->numframes));
		if (haveanim)
			free (a.components);
		MD5_FreeModel (m);
		return NULL;
	}

// where the original's frames start, for their names
	p = (byte *)&orig[1];
	for (i = 0 ; i < LittleLong (orig->numskins) ; i++)
	{
		daliasskintype_t	*st = (daliasskintype_t *)p;

		p += sizeof(daliasskintype_t);
		if (LittleLong (st->type) == ALIAS_SKIN_SINGLE)
			p += LittleLong (orig->skinwidth) * LittleLong (orig->skinheight);
		else
		{
			int	n = LittleLong (((daliasskingroup_t *)p)->numskins);

			p += sizeof(daliasskingroup_t) + n * sizeof(daliasskininterval_t)
				+ n * LittleLong (orig->skinwidth) * LittleLong (orig->skinheight);
		}
	}
	p += LittleLong (orig->numverts) * sizeof(stvert_t)
		+ LittleLong (orig->numtris) * sizeof(dtriangle_t);
	origframes = (daliasframetype_t *)p;

	file = MD5_BuildMDL (m, haveanim ? &a : NULL, orig, origframes, &size);
	if (haveanim)
		free (a.components);
	MD5_FreeModel (m);
	if (file)
		Con_DPrintf ("%s: enhanced model\n", mod->name);
	return file;
}

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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "quakedef.h"
#include "r_local.h"

model_t	*loadmodel;
char	loadname[32];	// for hunk tags

void Mod_LoadSpriteModel (model_t *mod, void *buffer);
void Mod_LoadBrushModel (model_t *mod, void *buffer);
void Mod_LoadAliasModel (model_t *mod, void *buffer);
model_t *Mod_LoadModel (model_t *mod, qboolean crash);

byte	mod_novis[MAX_MAP_LEAFS/8];

//
// Which of the two layouts the map being loaded uses.
//
// Set once from the version field in Mod_LoadBrushModel and read by the six
// lump readers that differ. Everything else about a BSP2 map is identical to a
// BSP29 one, so this is the whole of the switch.
//
static qboolean	loadmodel_bsp2;

//
// References in a map that point outside what the map has, or numbers in it
// that are not numbers. id's loader trusted every index, so one of these read
// whatever lay past the end of an array -- and when that was a vertex or a
// plane, what came back could be anything, NaNs included, which is how a face
// ends up stretched across the screen. They are set to 0 as they are read,
// counted, and the first is named once the map has loaded.
//
static int		mod_badrefs;
static char		mod_badfirst[160];

static void Mod_NoteBad (const char *fmt, ...)
{
	va_list		argptr;

	if (!mod_badrefs++)
	{
		va_start (argptr, fmt);
		vsnprintf (mod_badfirst, sizeof(mod_badfirst), fmt, argptr);
		va_end (argptr);

	// said now as well as at the end, in case what it leads to is fatal
		Con_Printf ("%s: %s\n", loadmodel->name, mod_badfirst);
	}
}

static qboolean Mod_BadFloat (float f)
{
	unsigned int	bits;

	memcpy (&bits, &f, sizeof(bits));
	return (bits & 0x7F800000) == 0x7F800000;
}

// Every model a map precaches takes a slot, a brush model's submodels
// included, so this has to be comfortably above MAX_MODELS.
#define	MAX_MOD_KNOWN	4096
model_t	mod_known[MAX_MOD_KNOWN];
int		mod_numknown;

// values for model_t's needload
#define NL_PRESENT		0
#define NL_NEEDS_LOADED	1
#define NL_UNREFERENCED	2

/*
===============
Mod_BspChecksum_f

A structural fingerprint of the loaded world, so that the two BSP layouts can
be shown to produce the same model.

Comparing rendered frames cannot do this: Quake animates textures and entities
against the clock, so two runs of the same map do not even match each other.
This walks what the readers actually filled in and runs a CRC over the values.
Pointers are turned into indices first -- they are hunk addresses and differ
between runs by construction.

Load a map as BSP29, note the number; convert it to BSP2, load that, and the
number has to be the same.
===============
*/
static void Mod_CRCLong (unsigned short *crc, int v)
{
	int		i;

	for (i = 0; i < 4; i++)
		CRC_ProcessByte (crc, (v >> (i*8)) & 0xff);
}

static void Mod_CRCFloat (unsigned short *crc, float f)
{
	union { float f; int i; } u;

	u.f = f;
	Mod_CRCLong (crc, u.i);
}

void Mod_BspChecksum (model_t *m)
{
	unsigned short	crc;
	int				i, j;

	CRC_Init (&crc);

	for (i = 0; i < m->numvertexes; i++)
		for (j = 0; j < 3; j++)
			Mod_CRCFloat (&crc, m->vertexes[i].position[j]);

	for (i = 0; i < m->numedges; i++)
	{
		Mod_CRCLong (&crc, m->edges[i].v[0]);
		Mod_CRCLong (&crc, m->edges[i].v[1]);
	}

	for (i = 0; i < m->numsurfaces; i++)
	{
		msurface_t	*sf = &m->surfaces[i];

		Mod_CRCLong (&crc, sf->firstedge);
		Mod_CRCLong (&crc, sf->numedges);
		Mod_CRCLong (&crc, sf->flags);
		Mod_CRCLong (&crc, sf->plane - m->planes);
		Mod_CRCLong (&crc, sf->texinfo - m->texinfo);
		for (j = 0; j < 2; j++)
		{
			Mod_CRCLong (&crc, sf->texturemins[j]);
			Mod_CRCLong (&crc, sf->extents[j]);
		}
	}

	for (i = 0; i < m->numnodes; i++)
	{
		mnode_t	*n = &m->nodes[i];

		Mod_CRCLong (&crc, n->plane - m->planes);
		Mod_CRCLong (&crc, n->firstsurface);
		Mod_CRCLong (&crc, n->numsurfaces);
		for (j = 0; j < 6; j++)
			Mod_CRCFloat (&crc, n->minmaxs[j]);
		for (j = 0; j < 2; j++)
		{
		// a child is either a node or a leaf; record which and where
			mnode_t	*c = n->children[j];

			if (c->contents < 0)
				Mod_CRCLong (&crc, -1 - (int)((mleaf_t *)c - m->leafs));
			else
				Mod_CRCLong (&crc, (int)(c - m->nodes));
		}
	}

	for (i = 0; i < m->numleafs; i++)
	{
		mleaf_t	*l = &m->leafs[i];

		Mod_CRCLong (&crc, l->contents);
		Mod_CRCLong (&crc, l->nummarksurfaces);
		Mod_CRCLong (&crc, (int)(l->firstmarksurface - m->marksurfaces));
		for (j = 0; j < 6; j++)
			Mod_CRCFloat (&crc, l->minmaxs[j]);
		for (j = 0; j < NUM_AMBIENTS; j++)
			CRC_ProcessByte (&crc, l->ambient_sound_level[j]);
	}

	for (i = 0; i < m->numclipnodes; i++)
	{
		Mod_CRCLong (&crc, m->clipnodes[i].planenum);
		Mod_CRCLong (&crc, m->clipnodes[i].children[0]);
		Mod_CRCLong (&crc, m->clipnodes[i].children[1]);
	}

	for (i = 0; i < m->nummarksurfaces; i++)
		Mod_CRCLong (&crc, (int)(m->marksurfaces[i] - m->surfaces));

	Con_Printf ("%s: verts %d edges %d surfs %d nodes %d leafs %d "
				"clipnodes %d marks %d\n",
				m->name, m->numvertexes, m->numedges, m->numsurfaces,
				m->numnodes, m->numleafs, m->numclipnodes,
				m->nummarksurfaces);
	Con_Printf ("bspchecksum %u\n", (unsigned)CRC_Value (crc));
}

void Mod_BspChecksum_f (void)
{
	model_t	*m;

// sv.worldmodel, not cl.worldmodel: the server has it as soon as the map
// command returns, where the client only gets it once it has connected over
// the loopback a frame or two later.
	m = sv.worldmodel;
	if (!m)
		m = cl.worldmodel;
	if (!m)
	{
		Con_Printf ("no world loaded\n");
		return;
	}

	Mod_BspChecksum (m);
}


/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
	memset (mod_novis, 0xff, sizeof(mod_novis));
	Cmd_AddCommand ("bspchecksum", Mod_BspChecksum_f);
	Cvar_RegisterVariable (&r_enhancedmodels);
}

/*
===============
Mod_FlushAliasModels

Drops every alias model's data, so that each is read again the next time it is
drawn -- for Enhanced Models, which decides what an alias model is made from as
it is read. The model_t stays where it is, so every entity that uses it still
does; what each remembers of the last pose it was drawn in, as an offset into
the old data, is forgotten.
===============
*/
void Mod_FlushAliasModels (void)
{
	int		i;
	model_t	*mod;

	for (i = 0, mod = mod_known ; i < mod_numknown ; i++, mod++)
		if (mod->type == mod_alias && mod->cache.data)
			Cache_Free (&mod->cache);

	for (i = 0 ; i < MAX_EDICTS ; i++)
		cl_entities[i].lerpmodel = NULL;
	for (i = 0 ; i < MAX_STATIC_ENTITIES ; i++)
		cl_static_entities[i].lerpmodel = NULL;
	cl.viewent.lerpmodel = NULL;
}

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *Mod_Extradata (model_t *mod)
{
	void	*r;
	
	r = Cache_Check (&mod->cache);
	if (r)
		return r;

	Mod_LoadModel (mod, true);
	
	if (!mod->cache.data)
		Sys_Error ("Mod_Extradata: caching failed");
	return mod->cache.data;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;
	
	if (!model || !model->nodes)
		Sys_Error ("Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}
	
	return NULL;	// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
byte *Mod_DecompressVis (byte *in, model_t *model)
{
	static byte	decompressed[MAX_MAP_LEAFS/8];
	int		c;
	byte	*out;
	int		row;

	row = (model->numleafs+7)>>3;	
	out = decompressed;

	if (!in)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return decompressed;		
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}
	
		c = in[1];
		in += 2;
		while (c)
		{
			*out++ = 0;
			c--;
		}
	} while (out - decompressed < row);
	
	return decompressed;
}

byte *Mod_LeafPVS (mleaf_t *leaf, model_t *model)
{
	if (leaf == model->leafs)
		return mod_novis;
	return Mod_DecompressVis (leaf->compressed_vis, model);
}

/*
===================
Mod_ClearAll
===================
*/
void Mod_ClearAll (void)
{
	int		i;
	model_t	*mod;


	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++) {
		mod->needload = NL_UNREFERENCED;
//FIX FOR CACHE_ALLOC ERRORS:
		if (mod->type == mod_sprite) mod->cache.data = NULL;
	}
}

/*
==================
Mod_FindName

==================
*/
model_t *Mod_FindName (char *name)
{
	int		i;
	model_t	*mod;
	model_t	*avail = NULL;

	if (!name[0])
		Sys_Error ("Mod_ForName: NULL name");
		
//
// search the currently loaded models
//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!strcmp (mod->name, name) )
			break;
		if (mod->needload == NL_UNREFERENCED)
			if (!avail || mod->type != mod_alias)
				avail = mod;
	}
			
	if (i == mod_numknown)
	{
		if (mod_numknown == MAX_MOD_KNOWN)
		{
			if (avail)
			{
				mod = avail;
				if (mod->type == mod_alias)
					if (Cache_Check (&mod->cache))
						Cache_Free (&mod->cache);
			}
			else
				Sys_Error ("mod_numknown == MAX_MOD_KNOWN");
		}
		else
			mod_numknown++;
		strcpy (mod->name, name);
		mod->needload = NL_NEEDS_LOADED;
	}

	return mod;
}

/*
==================
Mod_TouchModel

==================
*/
void Mod_TouchModel (char *name)
{
	model_t	*mod;
	
	mod = Mod_FindName (name);
	
	if (mod->needload == NL_PRESENT)
	{
		if (mod->type == mod_alias)
			Cache_Check (&mod->cache);
	}
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
model_t *Mod_LoadModel (model_t *mod, qboolean crash)
{
	unsigned *buf;
	byte	stackbuf[1024];		// avoid dirtying the cache heap
	int		mdldepth = 0;

	if (mod->type == mod_alias)
	{
		if (Cache_Check (&mod->cache))
		{
			mod->needload = NL_PRESENT;
			return mod;
		}
	}
	else
	{
		if (mod->needload == NL_PRESENT)
			return mod;
	}

//
// because the world is so huge, load it one piece at a time
//
	
//
// load the file
//
	buf = (unsigned *)COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf));
	if (!buf)
	{
		if (crash)
			Sys_Error ("Mod_NumForName: %s not found", mod->name);
		return NULL;
	}
	mdldepth = com_filedepth;
	
//
// allocate a new model
//
	COM_FileBase (mod->name, loadname);
	
	loadmodel = mod;

//
// fill it in
//

// call the apropriate loader
	mod->needload = NL_PRESENT;

	switch (LittleLong(*(unsigned *)buf))
	{
	case IDPOLYHEADER:
		{
		// the re-release's enhanced model instead, if it has one and
		// Enhanced Models is on (model_md5.c)
			byte	*enhanced = Mod_EnhancedModel (mod, (byte *)buf, mdldepth);

			if (enhanced)
			{
				Mod_LoadAliasModel (mod, enhanced);
				free (enhanced);
			}
			else
				Mod_LoadAliasModel (mod, buf);
		}
		break;
		
	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buf);
		break;
	
	default:
		Mod_LoadBrushModel (mod, buf);
		break;
	}

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName (char *name, qboolean crash)
{
	model_t	*mod;

	mod = Mod_FindName (name);

	return Mod_LoadModel (mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

byte	*mod_base;


/*
=================
Mod_RebuildFenceMips

A fence texture's holes are palette index 255 -- in its first mip level.
The levels below that were built by the texture tool, which averages each
block of texels into one colour and has no idea 255 means "not here". So the
holes are gone from them: filled with a blend of the bars and index 255's
pink, which comes out a flat tan. The renderer switches to those levels as a
surface gets farther away or more oblique, so a grate showed its holes up
close and snapped to a solid sheet at a distance.

So they are rebuilt here, from level 0, in a way that knows about holes.
Each texel of a smaller level covers a square block of level 0: it is a hole
if more than half of that block is, and otherwise it takes the most common
of the block's solid colours. Picking a colour that is already there, rather
than averaging, keeps fullbright texels fullbright and ordinary ones
ordinary, and needs no palette search.
=================
*/
static void Mod_RebuildFenceMips (texture_t *tx)
{
	byte	*src, *dst;
	int		level, x, y, bx, by, step, w, h, holes, total, i, best;
	int		counts[256];

	src = (byte *)tx + tx->offsets[0];

	for (level=1 ; level<MIPLEVELS ; level++)
	{
		step = 1 << level;
		w = tx->width >> level;
		h = tx->height >> level;
		dst = (byte *)tx + tx->offsets[level];
		total = step * step;

		for (y=0 ; y<h ; y++)
		{
			for (x=0 ; x<w ; x++)
			{
				memset (counts, 0, sizeof(counts));

				for (by=0 ; by<step ; by++)
					for (bx=0 ; bx<step ; bx++)
						counts[src[(y*step + by) * tx->width + x*step + bx]]++;

				holes = counts[255];

				if (holes * 2 > total)
				{
					dst[y*w + x] = 255;
					continue;
				}

				best = 0;
				for (i=1 ; i<255 ; i++)
					if (counts[i] > counts[best])
						best = i;

				dst[y*w + x] = best;
			}
		}
	}
}


/*
=================
Mod_LoadTextures
=================
*/
void Mod_LoadTextures (lump_t *l)
{
	int		i, j, pixels, num, max, altmax;
	miptex_t	*mt;
	texture_t	*tx, *tx2;
	texture_t	*anims[10];
	texture_t	*altanims[10];
	dmiptexlump_t *m;

	if (!l->filelen)
	{
		loadmodel->textures = NULL;
		return;
	}
	m = (dmiptexlump_t *)(mod_base + l->fileofs);
	
	m->nummiptex = LittleLong (m->nummiptex);
	
	loadmodel->numtextures = m->nummiptex;
	loadmodel->textures = Hunk_AllocName (m->nummiptex * sizeof(*loadmodel->textures) , loadname);

	for (i=0 ; i<m->nummiptex ; i++)
	{
		m->dataofs[i] = LittleLong(m->dataofs[i]);
		if (m->dataofs[i] == -1)
			continue;
		mt = (miptex_t *)((byte *)m + m->dataofs[i]);
		mt->width = LittleLong (mt->width);
		mt->height = LittleLong (mt->height);
		for (j=0 ; j<MIPLEVELS ; j++)
			mt->offsets[j] = LittleLong (mt->offsets[j]);
		
		if ( (mt->width & 15) || (mt->height & 15) )
			Sys_Error ("Texture %s is not 16 aligned", mt->name);
		pixels = mt->width*mt->height/64*85;
		tx = Hunk_AllocName (sizeof(texture_t) +pixels, loadname );
		loadmodel->textures[i] = tx;

		memcpy (tx->name, mt->name, sizeof(tx->name));
		tx->width = mt->width;
		tx->height = mt->height;
		for (j=0 ; j<MIPLEVELS ; j++)
			tx->offsets[j] = mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t);
		// the pixels immediately follow the structures
		memcpy ( tx+1, mt+1, pixels);
		
		if (!Q_strncmp(mt->name,"sky",3))	
			R_InitSky (tx);

		if (tx->name[0] == '{')
			Mod_RebuildFenceMips (tx);
	}

//
// sequence the animations
//
	for (i=0 ; i<m->nummiptex ; i++)
	{
		tx = loadmodel->textures[i];
		if (!tx || tx->name[0] != '+')
			continue;
		if (tx->anim_next)
			continue;	// allready sequenced

	// find the number of frames in the animation
		memset (anims, 0, sizeof(anims));
		memset (altanims, 0, sizeof(altanims));

		max = tx->name[1];
		altmax = 0;
		if (max >= 'a' && max <= 'z')
			max -= 'a' - 'A';
		if (max >= '0' && max <= '9')
		{
			max -= '0';
			altmax = 0;
			anims[max] = tx;
			max++;
		}
		else if (max >= 'A' && max <= 'J')
		{
			altmax = max - 'A';
			max = 0;
			altanims[altmax] = tx;
			altmax++;
		}
		else
			Sys_Error ("Bad animating texture %s", tx->name);

		for (j=i+1 ; j<m->nummiptex ; j++)
		{
			tx2 = loadmodel->textures[j];
			if (!tx2 || tx2->name[0] != '+')
				continue;
			if (strcmp (tx2->name+2, tx->name+2))
				continue;

			num = tx2->name[1];
			if (num >= 'a' && num <= 'z')
				num -= 'a' - 'A';
			if (num >= '0' && num <= '9')
			{
				num -= '0';
				anims[num] = tx2;
				if (num+1 > max)
					max = num + 1;
			}
			else if (num >= 'A' && num <= 'J')
			{
				num = num - 'A';
				altanims[num] = tx2;
				if (num+1 > altmax)
					altmax = num+1;
			}
			else
				Sys_Error ("Bad animating texture %s", tx->name);
		}
		
#define	ANIM_CYCLE	2
	// link them all together
		for (j=0 ; j<max ; j++)
		{
			tx2 = anims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s",j, tx->name);
			tx2->anim_total = max * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = anims[ (j+1)%max ];
			if (altmax)
				tx2->alternate_anims = altanims[0];
		}
		for (j=0 ; j<altmax ; j++)
		{
			tx2 = altanims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s",j, tx->name);
			tx2->anim_total = altmax * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = altanims[ (j+1)%altmax ];
			if (max)
				tx2->alternate_anims = anims[0];
		}
	}
}

static int	mod_lightlen;	// bytes of light data, for Mod_LoadFaces to check against

/*
=================
Mod_LoadColouredLight

Coloured light, which the re-release's maps and most maps built with modern
tools carry beside the grey lightmap id's renderer reads: three bytes, red,
green and blue, for every byte of the grey one, in the same order, so a
surface's colour is at three times its grey offset. It comes one of two ways:

 - maps/NAME.lit, a file beside the map: "QLIT", version 1, then the bytes.
 - an RGBLIGHTING lump in the map's BSPX block, which is a table of extra
   named lumps that follows the standard fifteen: "BSPX", a count, then 24
   bytes of name, an offset and a length for each.

Either has to be exactly three times the grey data or it is not trusted; a
.lit left behind from an older build of the map would light it wrongly.
r_tint.c turns it into tinted colormaps.
=================
*/
static void Mod_LoadColouredLight (void)
{
	char		litname[MAX_QPATH];
	byte		*lit;
	int			i, end, count, mark;
	dheader_t	*header = (dheader_t *)mod_base;

	if (strlen (loadmodel->name) < 5 || strlen (loadmodel->name) >= MAX_QPATH)
		return;

// the map's own BSPX lump first: it cannot be stale
	end = 0;
	for (i=0 ; i<HEADER_LUMPS ; i++)
		if (header->lumps[i].fileofs + header->lumps[i].filelen > end)
			end = header->lumps[i].fileofs + header->lumps[i].filelen;
	end = (end + 3) & ~3;
	if (end + 8 <= com_filesize && !memcmp (mod_base + end, "BSPX", 4))
	{
		count = LittleLong (*(int *)(mod_base + end + 4));
		for (i=0 ; i<count && end + 8 + (i+1)*32 <= com_filesize ; i++)
		{
			byte	*e = mod_base + end + 8 + i*32;
			int		ofs = LittleLong (*(int *)(e + 24));
			int		len = LittleLong (*(int *)(e + 28));

			if (strncmp ((char *)e, "RGBLIGHTING", 24))
				continue;
			if (len != mod_lightlen * 3 || ofs < 0 || ofs + len > com_filesize)
			{
				Con_DPrintf ("%s: BSPX RGBLIGHTING is %d bytes, not %d; "
							 "grey light only\n", loadmodel->name, len,
							 mod_lightlen * 3);
				return;
			}
			loadmodel->rgblightdata = Hunk_AllocName (len, loadname);
			memcpy (loadmodel->rgblightdata, mod_base + ofs, len);
			Con_DPrintf ("%s: coloured light from its BSPX lump\n",
						 loadmodel->name);
			return;
		}
	}

// then a .lit beside it
	strcpy (litname, loadmodel->name);
	strcpy (litname + strlen (litname) - 4, ".lit");
	mark = Hunk_LowMark ();
	lit = COM_LoadHunkFile (litname);
	if (!lit)
		return;
	if (com_filesize != 8 + mod_lightlen * 3 || memcmp (lit, "QLIT", 4)
		|| LittleLong (*(int *)(lit + 4)) != 1)
	{
		Con_Printf ("%s is %d bytes for %d of light data, or not version 1; "
					"%s keeps its grey light\n", litname, com_filesize,
					mod_lightlen, loadmodel->name);
		Hunk_FreeToLowMark (mark);
		return;
	}
	loadmodel->rgblightdata = lit + 8;
	Con_DPrintf ("%s: coloured light from %s\n", loadmodel->name, litname);
}


/*
=================
Mod_LoadLighting
=================
*/

void Mod_LoadLighting (lump_t *l)
{
	mod_lightlen = l->filelen;
	loadmodel->rgblightdata = NULL;
	if (!l->filelen)
	{
		loadmodel->lightdata = NULL;
		return;
	}
	loadmodel->lightdata = Hunk_AllocName ( l->filelen, loadname);	
	memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
	Mod_LoadColouredLight ();
}


/*
=================
Mod_LoadVisibility
=================
*/
void Mod_LoadVisibility (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->visdata = NULL;
		return;
	}
	loadmodel->visdata = Hunk_AllocName ( l->filelen, loadname);	
	memcpy (loadmodel->visdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadEntities
=================
*/
void Mod_LoadEntities (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->entities = NULL;
		return;
	}
	loadmodel->entities = Hunk_AllocName ( l->filelen, loadname);	
	memcpy (loadmodel->entities, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVertexes
=================
*/
void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t	*in;
	mvertex_t	*out;
	int			i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);

		if (Mod_BadFloat (out->position[0]) || Mod_BadFloat (out->position[1])
			|| Mod_BadFloat (out->position[2]))
		{
			Mod_NoteBad ("vertex %d is not a number", i);
			out->position[0] = out->position[1] = out->position[2] = 0;
		}
	}
}

/*
=================
Mod_LoadSubmodels
=================
*/
void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t	*in;
	dmodel_t	*out;
	int			i, j, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		for (j=0 ; j<MAX_MAP_HULLS ; j++)
			out->headnode[j] = LittleLong (in->headnode[j]);
		out->visleafs = LittleLong (in->visleafs);
		out->firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
void Mod_LoadEdges (lump_t *l)
{
	medge_t *out;
	byte	*inbase;
	int 	i, count, recsize;

	recsize = loadmodel_bsp2 ? sizeof(dedge2_t) : sizeof(dedge_t);
	inbase = (byte *)(mod_base + l->fileofs);
	if (l->filelen % recsize)
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / recsize;
	out = Hunk_AllocName ( (count + 1) * sizeof(*out), loadname);	

	loadmodel->edges = out;
	loadmodel->numedges = count;

	for ( i=0 ; i<count ; i++, out++)
	{
		if (loadmodel_bsp2)
		{
			dedge2_t	*in2 = (dedge2_t *)(inbase + i*recsize);

			out->v[0] = LittleLong (in2->v[0]);
			out->v[1] = LittleLong (in2->v[1]);
		}
		else
		{
			dedge_t		*in = (dedge_t *)(inbase + i*recsize);

			out->v[0] = (unsigned short)LittleShort(in->v[0]);
			out->v[1] = (unsigned short)LittleShort(in->v[1]);
		}

		if (out->v[0] >= loadmodel->numvertexes
			|| out->v[1] >= loadmodel->numvertexes)
		{
			Mod_NoteBad ("edge %d names vertex %u or %u; the map has %d",
						 i, out->v[0], out->v[1], loadmodel->numvertexes);
			if (out->v[0] >= loadmodel->numvertexes)
				out->v[0] = 0;
			if (out->v[1] >= loadmodel->numvertexes)
				out->v[1] = 0;
		}
	}
}

/*
=================
Mod_LoadTexinfo
=================
*/
void Mod_LoadTexinfo (lump_t *l)
{
	texinfo_t *in;
	mtexinfo_t *out;
	int 	i, j, count;
	int		miptex;
	float	len1, len2;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<4 ; j++)
		{
			out->vecs[0][j] = LittleFloat (in->vecs[0][j]);
			out->vecs[1][j] = LittleFloat (in->vecs[1][j]);
		}
		len1 = Length (out->vecs[0]);
		len2 = Length (out->vecs[1]);
		len1 = (len1 + len2)/2;
		if (len1 < 0.32)
			out->mipadjust = 4;
		else if (len1 < 0.49)
			out->mipadjust = 3;
		else if (len1 < 0.99)
			out->mipadjust = 2;
		else
			out->mipadjust = 1;
#if 0
		if (len1 + len2 < 0.001)
			out->mipadjust = 1;		// don't crash
		else
			out->mipadjust = 1 / floor( (len1+len2)/2 + 0.1 );
#endif

		miptex = LittleLong (in->miptex);
		out->flags = LittleLong (in->flags);
	
		if (!loadmodel->textures)
		{
			out->texture = r_notexture_mip;	// checkerboard texture
			out->flags = 0;
		}
		else
		{
			if (miptex >= loadmodel->numtextures)
				Sys_Error ("miptex >= loadmodel->numtextures");
			out->texture = loadmodel->textures[miptex];
			if (!out->texture)
			{
				out->texture = r_notexture_mip; // texture not found
				out->flags = 0;
			}
		}
	}
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
void CalcSurfaceExtents (msurface_t *s)
{
	float	mins[2], maxs[2], val;
	int		i,j, e;
	mvertex_t	*v;
	mtexinfo_t	*tex;
	int		bmins[2], bmaxs[2];

	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;

	tex = s->texinfo;
	
	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];
		
		for (j=0 ; j<2 ; j++)
		{
			val = v->position[0] * tex->vecs[j][0] + 
				v->position[1] * tex->vecs[j][1] +
				v->position[2] * tex->vecs[j][2] +
				tex->vecs[j][3];
			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i=0 ; i<2 ; i++)
	{	
		bmins[i] = floor(mins[i]/16);
		bmaxs[i] = ceil(maxs[i]/16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;

	// Only the first vertex of each edge is measured, as the light tools do,
	// so the lightmap here is the size they made. A face whose edges do not
	// close can have every one of those on the same line of the texture, and
	// then this is 0: a surface 0 texels across, which D_SCAlloc refuses with
	// a fatal error the first time the face is drawn. It is made one
	// lightmap step across instead.
		if (s->extents[i] < 16)
			s->extents[i] = 16;
		if ( !(tex->flags & TEX_SPECIAL) && s->extents[i] > 256)
			Sys_Error ("Bad surface extents");
	}
}


/*
=================
Mod_LoadFaces
=================
*/
void Mod_LoadFaces (lump_t *l)
{
	msurface_t 	*out;
	byte		*inbase;
	int			i, count, surfnum, recsize;
	int			planenum, side;

	recsize = loadmodel_bsp2 ? sizeof(dface2_t) : sizeof(dface_t);
	inbase = (byte *)(mod_base + l->fileofs);
	if (l->filelen % recsize)
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / recsize;
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	for ( surfnum=0 ; surfnum<count ; surfnum++, out++)
	{
		int		texinfo;
		byte	*styles;
		int		lightofs;

		if (loadmodel_bsp2)
		{
			dface2_t	*in = (dface2_t *)(inbase + surfnum*recsize);

			out->firstedge = LittleLong(in->firstedge);
			out->numedges = LittleLong(in->numedges);
			planenum = LittleLong(in->planenum);
			side = LittleLong(in->side);
			texinfo = LittleLong(in->texinfo);
			styles = in->styles;
			lightofs = LittleLong(in->lightofs);
		}
		else
		{
			dface_t		*in = (dface_t *)(inbase + surfnum*recsize);

			out->firstedge = LittleLong(in->firstedge);
			// unsigned, as the compilers write them: a big map has more
			// than 32767 planes, and read signed those point before the array
			out->numedges = (unsigned short)LittleShort(in->numedges);
			planenum = (unsigned short)LittleShort(in->planenum);
			side = LittleShort(in->side);
			texinfo = (unsigned short)LittleShort(in->texinfo);
			styles = in->styles;
			lightofs = LittleLong(in->lightofs);
		}

		out->flags = 0;

		if (planenum < 0 || planenum >= loadmodel->numplanes)
		{
			Mod_NoteBad ("face %d names plane %d; the map has %d",
						 surfnum, planenum, loadmodel->numplanes);
			planenum = 0;
		}
		if (texinfo < 0 || texinfo >= loadmodel->numtexinfo)
		{
			Mod_NoteBad ("face %d names texinfo %d; the map has %d",
						 surfnum, texinfo, loadmodel->numtexinfo);
			texinfo = 0;
		}
		if (out->firstedge < 0 || out->numedges < 0
			|| out->firstedge > loadmodel->numsurfedges - out->numedges)
		{
			Mod_NoteBad ("face %d uses surfedges %d to %d; the map has %d",
						 surfnum, out->firstedge,
						 out->firstedge + out->numedges - 1,
						 loadmodel->numsurfedges);
			out->firstedge = 0;
			out->numedges = 0;
		}

		if (side)
			out->flags |= SURF_PLANEBACK;			

		out->plane = loadmodel->planes + planenum;

		out->texinfo = loadmodel->texinfo + texinfo;

		CalcSurfaceExtents (out);
				
	// lighting info

		for (i=0 ; i<MAXLIGHTMAPS ; i++)
			out->styles[i] = styles[i];
		out->rgbsamples = NULL;
		if (lightofs == -1)
			out->samples = NULL;
		else
		{
			out->samples = loadmodel->lightdata + lightofs;
			if (loadmodel->rgblightdata)
				out->rgbsamples = loadmodel->rgblightdata + lightofs*3;
		}

	//
	// A lightmap that runs past the end of the light data would be read from
	// whatever follows it in memory. Sky and water have none to check.
	//
		if (out->samples && !(out->texinfo->flags & TEX_SPECIAL))
		{
			int		nstyles, size;

			for (nstyles=0 ; nstyles<MAXLIGHTMAPS && styles[nstyles] != 255
				 ; nstyles++)
				;
			size = ((out->extents[0]>>4)+1) * ((out->extents[1]>>4)+1);
			if (lightofs < 0 || lightofs > mod_lightlen - size*nstyles)
			{
				Mod_NoteBad ("face %d's lightmap runs from %d to %d; the light "
							 "data is %d bytes", surfnum, lightofs,
							 lightofs + size*nstyles, mod_lightlen);
				out->samples = NULL;
				out->rgbsamples = NULL;
			}
		}
		
	// set the drawing flags flag
		
		if (!Q_strncmp(out->texinfo->texture->name,"sky",3))	// sky
		{
			out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
			continue;
		}
		
	//
	// A '{' name is a fence: index 255 is see-through. It is otherwise an
	// ordinary lit, subdivided surface, so this sets the flag and falls
	// through rather than continuing like sky and water do.
	//
		if (out->texinfo->texture->name[0] == '{')
			out->flags |= SURF_DRAWMASKED;

		if (!Q_strncmp(out->texinfo->texture->name,"*",1))		// turbulent
		{
			out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
			for (i=0 ; i<2 ; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}
			continue;
		}
	}
}


/*
=================
Mod_SetParent
=================
*/
void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents < 0)
		return;
	Mod_SetParent (node->children[0], node);
	Mod_SetParent (node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
void Mod_LoadNodes (lump_t *l)
{
	int			i, j, count, p, recsize;
	byte		*inbase;
	mnode_t 	*out;

	recsize = loadmodel_bsp2 ? sizeof(dnode2_t) : sizeof(dnode_t);
	inbase = (byte *)(mod_base + l->fileofs);
	if (l->filelen % recsize)
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / recsize;
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for ( i=0 ; i<count ; i++, out++)
	{
		if (loadmodel_bsp2)
		{
			dnode2_t	*in = (dnode2_t *)(inbase + i*recsize);

			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j] = LittleFloat (in->mins[j]);
				out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
			}

			out->plane = loadmodel->planes + LittleLong(in->planenum);

			out->firstsurface = LittleLong (in->firstface);
			out->numsurfaces = LittleLong (in->numfaces);

			for (j=0 ; j<2 ; j++)
			{
				p = LittleLong (in->children[j]);
				if (p >= 0)
					out->children[j] = loadmodel->nodes + p;
				else
					out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
			}
		}
		else
		{
			dnode_t		*in = (dnode_t *)(inbase + i*recsize);

			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j] = LittleShort (in->mins[j]);
				out->minmaxs[3+j] = LittleShort (in->maxs[j]);
			}
	
			p = LittleLong(in->planenum);
			out->plane = loadmodel->planes + p;

		//
		// These are unsigned in the file. Read as signed, a node's face range
		// past 32767 went negative, and a child number past 32767 was taken
		// for a leaf -- a wrong one, or one past the end of the leaf array,
		// which the brush-model clipper then walked into as if it were a node
		// and read a "plane" out of. The re-release maps are big enough to
		// have that many. This is QuakeSpasm's (and DarkPlaces') reading: a
		// child below the node count is a node, anything else counts down
		// from 65535 as a leaf, and a leaf that does not exist is said so and
		// pointed at leaf 0, which is solid.
		//
			out->firstsurface = (unsigned short)LittleShort (in->firstface);
			out->numsurfaces = (unsigned short)LittleShort (in->numfaces);
		
			for (j=0 ; j<2 ; j++)
			{
				p = (unsigned short)LittleShort (in->children[j]);
				if (p < count)
					out->children[j] = loadmodel->nodes + p;
				else
				{
					p = 65535 - p;
					if (p >= loadmodel->numleafs)
					{
						Con_Printf ("Mod_LoadNodes: node %d names leaf %d; the "
									"map has %d\n", i, p, loadmodel->numleafs);
						p = 0;
					}
					out->children[j] = (mnode_t *)(loadmodel->leafs + p);
				}
			}
		}
	}
	
	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
void Mod_LoadLeafs (lump_t *l)
{
	mleaf_t 	*out;
	byte		*inbase;
	int			i, j, count, p, recsize;

	recsize = loadmodel_bsp2 ? sizeof(dleaf2_t) : sizeof(dleaf_t);
	inbase = (byte *)(mod_base + l->fileofs);
	if (l->filelen % recsize)
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / recsize;
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for ( i=0 ; i<count ; i++, out++)
	{
		byte	*ambient;

		if (loadmodel_bsp2)
		{
			dleaf2_t	*in = (dleaf2_t *)(inbase + i*recsize);

			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j] = LittleFloat (in->mins[j]);
				out->minmaxs[3+j] = LittleFloat (in->maxs[j]);
			}

			out->contents = LittleLong(in->contents);

			out->firstmarksurface = loadmodel->marksurfaces +
				LittleLong(in->firstmarksurface);
			out->nummarksurfaces = LittleLong(in->nummarksurfaces);

			p = LittleLong(in->visofs);
			ambient = in->ambient_level;
		}
		else
		{
			dleaf_t		*in = (dleaf_t *)(inbase + i*recsize);

			for (j=0 ; j<3 ; j++)
			{
				out->minmaxs[j] = LittleShort (in->mins[j]);
				out->minmaxs[3+j] = LittleShort (in->maxs[j]);
			}

			out->contents = LittleLong(in->contents);

		// unsigned in the file, like the node fields above
			out->firstmarksurface = loadmodel->marksurfaces +
				(unsigned short)LittleShort(in->firstmarksurface);
			out->nummarksurfaces = (unsigned short)LittleShort(in->nummarksurfaces);

			p = LittleLong(in->visofs);
			ambient = in->ambient_level;
		}

		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + p;
		out->efrags = NULL;
		
		for (j=0 ; j<4 ; j++)
			out->ambient_sound_level[j] = ambient[j];
	}	
}

/*
=================
Mod_LoadClipnodes
=================
*/
void Mod_LoadClipnodes (lump_t *l)
{
	mclipnode_t *out;
	byte		*inbase;
	int			i, count, recsize;
	hull_t		*hull;

	recsize = loadmodel_bsp2 ? sizeof(dclipnode2_t) : sizeof(dclipnode_t);
	inbase = (byte *)(mod_base + l->fileofs);
	if (l->filelen % recsize)
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / recsize;
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->clipnodes = out;
	loadmodel->numclipnodes = count;

	hull = &loadmodel->hulls[1];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -16;
	hull->clip_mins[1] = -16;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 16;
	hull->clip_maxs[1] = 16;
	hull->clip_maxs[2] = 32;

	hull = &loadmodel->hulls[2];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;
	hull->clip_mins[0] = -32;
	hull->clip_mins[1] = -32;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 32;
	hull->clip_maxs[1] = 32;
	hull->clip_maxs[2] = 64;

	for (i=0 ; i<count ; i++, out++)
	{
		if (loadmodel_bsp2)
		{
			dclipnode2_t	*in = (dclipnode2_t *)(inbase + i*recsize);

			out->planenum = LittleLong(in->planenum);
			out->children[0] = LittleLong(in->children[0]);
			out->children[1] = LittleLong(in->children[1]);
		}
		else
		{
			dclipnode_t		*in = (dclipnode_t *)(inbase + i*recsize);

		// Unsigned in the file. A child below the clipnode count is a
		// clipnode; anything else is a contents value (-1 .. -15), which is
		// what it read as when the file had fewer than 32768 clipnodes and
		// the short was simply sign-extended. QuakeSpasm's reading.
			out->planenum = LittleLong(in->planenum);
			out->children[0] = (unsigned short)LittleShort(in->children[0]);
			out->children[1] = (unsigned short)LittleShort(in->children[1]);
			if (out->children[0] >= count)
				out->children[0] -= 65536;
			if (out->children[1] >= count)
				out->children[1] -= 65536;
		}
	}
}

/*
=================
Mod_MakeHull0

Deplicate the drawing hull structure as a clipping hull
=================
*/
void Mod_MakeHull0 (void)
{
	mnode_t		*in, *child;
	mclipnode_t *out;
	int			i, j, count;
	hull_t		*hull;
	
	hull = &loadmodel->hulls[0];	
	
	in = loadmodel->nodes;
	count = loadmodel->numnodes;
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count-1;
	hull->planes = loadmodel->planes;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		out->planenum = in->plane - loadmodel->planes;
		for (j=0 ; j<2 ; j++)
		{
			child = in->children[j];
			if (child->contents < 0)
				out->children[j] = child->contents;
			else
				out->children[j] = child - loadmodel->nodes;
		}
	}
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
void Mod_LoadMarksurfaces (lump_t *l)
{	
	int		i, j, count, recsize;
	byte		*inbase;
	msurface_t **out;
	
	recsize = loadmodel_bsp2 ? sizeof(unsigned int) : sizeof(unsigned short);
	inbase = (byte *)(mod_base + l->fileofs);
	if (l->filelen % recsize)
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / recsize;
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for ( i=0 ; i<count ; i++)
	{
		if (loadmodel_bsp2)
			j = LittleLong (((unsigned int *)inbase)[i]);
		else
			j = (unsigned short)LittleShort (((short *)inbase)[i]);

		if (j >= loadmodel->numsurfaces)
			Sys_Error ("Mod_ParseMarksurfaces: bad surface number");
		out[i] = loadmodel->surfaces + j;
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
void Mod_LoadSurfedges (lump_t *l)
{	
	int		i, count;
	int		*in, *out;
	
	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( count*sizeof(*out), loadname);	

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for ( i=0 ; i<count ; i++)
	{
		out[i] = LittleLong (in[i]);

		// the sign is which way round the edge is walked; 0 is never used
		// negatively, so -numedges is already one too far
		if (out[i] >= loadmodel->numedges || out[i] <= -loadmodel->numedges)
		{
			Mod_NoteBad ("surfedge %d names edge %d; the map has %d",
						 i, out[i], loadmodel->numedges);
			out[i] = 0;
		}
	}
}

/*
=================
Mod_LoadPlanes
=================
*/
void Mod_LoadPlanes (lump_t *l)
{
	int			i, j;
	mplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;
	
	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_AllocName ( count*2*sizeof(*out), loadname);	
	
	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;

		if (Mod_BadFloat (out->normal[0]) || Mod_BadFloat (out->normal[1])
			|| Mod_BadFloat (out->normal[2]) || Mod_BadFloat (out->dist))
		{
			Mod_NoteBad ("plane %d is not a number", i);
			out->normal[0] = out->normal[1] = 0;
			out->normal[2] = 1;
			out->dist = 0;
			out->type = PLANE_Z;
			out->signbits = 0;
		}
	}
}

/*
=================
RadiusFromBounds
=================
*/
float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	int		i;
	vec3_t	corner;

	for (i=0 ; i<3 ; i++)
	{
		corner[i] = fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
	}

	return Length (corner);
}

//
// Node and leaf bounds are how the renderer decides which screen edges a face
// has to be clipped against: a node wholly inside one edge's plane hands its
// faces no clip for it. It also shares edges between faces within a frame, and
// an edge one face found wholly off-screen is skipped by every other face that
// uses it. Both are right only if every face lies inside its node's bounds.
//
// id's compiler guaranteed that by cutting every face along the tree. Modern
// ones have faces the tree does not cut -- ericw-tools' func_detail_wall and
// func_detail_illusionary exist to make them -- and those can stick out of
// the node they hang on. Such a face goes unclipped against an edge it
// crosses; a neighbour that did clip marks their shared edge as off-screen;
// the face skips it and has nothing to close it, and its span runs on across
// the screen as a bar of its texture. GL engines have neither mechanism and
// never notice.
//
// So each node's bounds are widened here to hold every vertex of its own faces
// and the bounds of the nodes below it, which is what its clip flags promise.
// Leaves are left alone: they hold no faces, only references to them.
//
static int		mod_widened;		// nodes that grew by more than a unit
static float	mod_widest;			// the most any of them grew

static void Mod_AddFaceToBounds (msurface_t *surf, float *mins, float *maxs)
{
	int		i, j, lindex;
	medge_t	*edge;
	float	*v;

	for (i=0 ; i<surf->numedges ; i++)
	{
		lindex = loadmodel->surfedges[surf->firstedge + i];
		edge = &loadmodel->edges[lindex > 0 ? lindex : -lindex];
		v = loadmodel->vertexes[edge->v[lindex > 0 ? 0 : 1]].position;
		for (j=0 ; j<3 ; j++)
		{
			if (v[j] < mins[j])
				mins[j] = v[j];
			if (v[j] > maxs[j])
				maxs[j] = v[j];
		}
	}
}

static void Mod_WidenBounds (mnode_t *node)
{
	int			i, j;
	float		mins[3], maxs[3], grew;
	float		*box;

	for (j=0 ; j<3 ; j++)
	{
		mins[j] = node->minmaxs[j];
		maxs[j] = node->minmaxs[3+j];
	}

	if (node->contents < 0)
		return;
	else
	{
		for (i=0 ; i<2 ; i++)
		{
		// a leaf holds no faces of its own, and every solid leaf is leaf 0,
		// whose box is not this node's business
			if (node->children[i]->contents < 0)
				continue;
			Mod_WidenBounds (node->children[i]);
			box = node->children[i]->minmaxs;
			for (j=0 ; j<3 ; j++)
			{
				if (box[j] < mins[j])
					mins[j] = box[j];
				if (box[3+j] > maxs[j])
					maxs[j] = box[3+j];
			}
		}
		for (i=0 ; i<node->numsurfaces ; i++)
			Mod_AddFaceToBounds (loadmodel->surfaces + node->firstsurface + i,
								 mins, maxs);
	}

	grew = 0;
	for (j=0 ; j<3 ; j++)
	{
		if (node->minmaxs[j] - mins[j] > grew)
			grew = node->minmaxs[j] - mins[j];
		if (maxs[j] - node->minmaxs[3+j] > grew)
			grew = maxs[j] - node->minmaxs[3+j];
		node->minmaxs[j] = mins[j];
		node->minmaxs[3+j] = maxs[j];
	}
	if (grew > 1)
	{
		mod_widened++;
		if (grew > mod_widest)
			mod_widest = grew;
	}
}

//
// A face is a loop of edges, and everything that draws one relies on the loop
// closing: the edge list opens a face's span at one edge and shuts it at
// another, and cutting a brush model along a world plane joins the two points
// where the loop crosses it. The re-release maps have faces whose loop does
// not close -- one edge ends where the next does not begin -- and on a brush
// model such a face was drawn with a span that nothing shut, or shut against a
// point left over from another face: a thin bar of it stretched sideways
// across the screen, lit by whatever was at the face's edge, which was black.
//
// So each gap is bridged here with an edge of its own, from where the loop
// stops to where it picks up again. Only a face whose edges do not balance --
// some point is left more often than it is reached -- is touched: a face that
// balances is closed whatever order its edges come in, and bridging between
// them would add edges that are not there.
//
static void Mod_EdgeEnds (model_t *mod, int lindex, mvertex_t **a, mvertex_t **b)
{
	medge_t	*e;

	e = &mod->edges[lindex > 0 ? lindex : -lindex];
	*a = &mod->vertexes[e->v[lindex > 0 ? 0 : 1]];
	*b = &mod->vertexes[e->v[lindex > 0 ? 1 : 0]];
}

static qboolean Mod_SamePoint (mvertex_t *a, mvertex_t *b)
{
	return a == b || (a->position[0] == b->position[0]
					  && a->position[1] == b->position[1]
					  && a->position[2] == b->position[2]);
}

// How many bridges face s needs, or 0 when its edges balance.
static int Mod_FaceGaps (model_t *mod, msurface_t *s)
{
	int			i, j, n, bal, gaps;
	mvertex_t	*a, *b, *c, *d;
	int			*se;

	n = s->numedges;
	if (n < 2)
		return 0;
	se = mod->surfedges + s->firstedge;

	gaps = 0;
	for (i=0 ; i<n ; i++)
	{
		Mod_EdgeEnds (mod, se[i], &a, &b);
		Mod_EdgeEnds (mod, se[(i+1) % n], &c, &d);
		if (!Mod_SamePoint (b, c))
			gaps++;
	}
	if (!gaps)
		return 0;

// out of order but balanced: every point left as often as it is reached
	for (i=0 ; i<n ; i++)
	{
		Mod_EdgeEnds (mod, se[i], &a, &b);
		bal = 0;
		for (j=0 ; j<n ; j++)
		{
			Mod_EdgeEnds (mod, se[j], &c, &d);
			bal += Mod_SamePoint (a, c) - Mod_SamePoint (a, d);
		}
		if (bal)
			return gaps;
		Mod_EdgeEnds (mod, se[i], &a, &b);
		bal = 0;
		for (j=0 ; j<n ; j++)
		{
			Mod_EdgeEnds (mod, se[j], &c, &d);
			bal += Mod_SamePoint (b, c) - Mod_SamePoint (b, d);
		}
		if (bal)
			return gaps;
	}
	return 0;
}

static void Mod_CloseFaces (model_t *mod)
{
	int			i, k, n, gaps, faces, extra, extrase;
	int			*newse, *se, *out;
	medge_t		*newedges, *e;
	mvertex_t	*a, *b, *c, *d;
	msurface_t	*s;
	int			numedges;

	faces = extra = extrase = 0;
	for (i=0 ; i<mod->numsurfaces ; i++)
	{
		gaps = Mod_FaceGaps (mod, &mod->surfaces[i]);
		if (gaps)
		{
			faces++;
			extra += gaps;
			extrase += mod->surfaces[i].numedges + gaps;
		}
	}
	if (!faces)
		return;

// new edges go on the end of the edge array, and each face is given a run of
// surfedges of its own with its bridges in their places
	numedges = mod->numedges;
	newedges = Hunk_AllocName ((numedges + extra + 1) * sizeof(medge_t), loadname);
	memcpy (newedges, mod->edges, (numedges + 1) * sizeof(medge_t));
	newse = Hunk_AllocName ((mod->numsurfedges + extrase) * sizeof(int), loadname);
	memcpy (newse, mod->surfedges, mod->numsurfedges * sizeof(int));

	out = newse + mod->numsurfedges;
	for (i=0 ; i<mod->numsurfaces ; i++)
	{
		s = &mod->surfaces[i];
		if (!Mod_FaceGaps (mod, s))
			continue;

		n = s->numedges;
		se = mod->surfedges + s->firstedge;
		s->firstedge = out - newse;
		for (k=0 ; k<n ; k++)
		{
			*out++ = se[k];
			Mod_EdgeEnds (mod, se[k], &a, &b);
			Mod_EdgeEnds (mod, se[(k+1) % n], &c, &d);
			if (Mod_SamePoint (b, c))
				continue;
			e = &newedges[numedges];
			e->v[0] = b - mod->vertexes;
			e->v[1] = c - mod->vertexes;
			*out++ = numedges++;
		}
		s->numedges = out - newse - s->firstedge;
	}

	if (out - newse != mod->numsurfedges + extrase
		|| numedges != mod->numedges + extra)
		Sys_Error ("Mod_CloseFaces: miscounted in %s", mod->name);

	mod->edges = newedges;
	mod->numedges = numedges;
	mod->surfedges = newse;
	mod->numsurfedges = out - newse;

	Con_DPrintf ("%s: %d face(s) had edges that did not close; %d edge(s) "
				 "added to close them.\n", mod->name, faces, extra);
}

/*
=================
Mod_LoadBrushModel
=================
*/
void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
	int			i, j;
	dheader_t	*header;
	dmodel_t 	*bm;
	
	loadmodel->type = mod_brush;
	
	header = (dheader_t *)buffer;

	i = LittleLong (header->version);

//
// BSP29 is id's; BSP2 is what modern compilers and the Quake re-release emit,
// and the difference is the width of the indices and bounds in six of the
// fifteen lumps. loadmodel_bsp2 is how the readers below tell them apart.
//
// "2PSB", the RMQ variant, keeps short bounds in nodes and leafs and is a
// third layout again; it is named here so that meeting one says what it is
// rather than printing a number.
//
	loadmodel_bsp2 = false;

	if (i == BSP2VERSION)
		loadmodel_bsp2 = true;
	else if (i == (int)(('B'<<24) + ('S'<<16) + ('P'<<8) + '2'))
		Sys_Error ("Mod_LoadBrushModel: %s is a 2PSB map.\n"
				   "That is the RMQ variant of BSP2, which this engine does\n"
				   "not read. BSP2 and the original version %i are both fine.",
				   mod->name, BSPVERSION);
	else if (i != BSPVERSION)
		Sys_Error ("Mod_LoadBrushModel: %s has wrong version number (%i should be %i)",
				   mod->name, i, BSPVERSION);

// swap all the lumps
	mod_base = (byte *)header;
	mod_badrefs = 0;

	for (i=0 ; i<sizeof(dheader_t)/4 ; i++)
		((int *)header)[i] = LittleLong ( ((int *)header)[i]);

// load into heap
	
	Mod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (&header->lumps[LUMP_EDGES]);
	Mod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);
	Mod_LoadTextures (&header->lumps[LUMP_TEXTURES]);
	Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (&header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces (&header->lumps[LUMP_FACES]);
	Mod_LoadMarksurfaces (&header->lumps[LUMP_MARKSURFACES]);
	Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (&header->lumps[LUMP_LEAFS]);
	Mod_LoadNodes (&header->lumps[LUMP_NODES]);
	Mod_LoadClipnodes (&header->lumps[LUMP_CLIPNODES]);
	Mod_LoadEntities (&header->lumps[LUMP_ENTITIES]);
	Mod_LoadSubmodels (&header->lumps[LUMP_MODELS]);

	Mod_MakeHull0 ();

	if (mod_badrefs)
		Con_Printf ("%s: %d number(s) in the map were out of range or not "
					"a number,\nand were set to 0. The first is above.\n",
					mod->name, mod_badrefs);
	
	mod->numframes = 2;		// regular and alternate animation
	mod->flags = 0;

//
// -bspchecksum prints the fingerprint as each world is loaded.
//
// The console command needs a map already running and a way to type; this
// needs neither, which is what lets a script compare the same map through both
// readers without driving the game.
//
	if (COM_CheckParm ("-bspchecksum"))
		Mod_BspChecksum (mod);

//
// Edges that more than one face walks the same way. id's maps have none: every
// edge has one face on each side. The renderer gives each such face an edge of
// its own (see R_EmitCachedEdge); this says how many there are, since they are
// what the black bars on the re-release maps came from.
//
	{
		byte	*uses;
		int		k, lindex, same;

		uses = calloc (mod->numedges + 1, 2);
		same = 0;
		if (uses)
		{
			for (i=0 ; i<mod->numsurfaces ; i++)
				for (k=0 ; k<mod->surfaces[i].numedges ; k++)
				{
					lindex = mod->surfedges[mod->surfaces[i].firstedge + k];
					j = (lindex > 0 ? lindex : -lindex)*2 + (lindex < 0);
					if (uses[j] < 255 && ++uses[j] == 2)
						same++;
				}
			free (uses);
		}
		if (same)
			Con_Printf ("%s: %d edge(s) walked the same way by more than one "
						"face. Each\nsuch face is given an edge of its own when "
						"drawn.\n", mod->name, same);
	}

// after the checksum, which is of what the file says
	Mod_CloseFaces (mod);

	mod_widened = 0;
	mod_widest = 0;
	for (i=0 ; i<mod->numsubmodels ; i++)
		if (mod->submodels[i].headnode[0] >= 0
			&& mod->submodels[i].headnode[0] < mod->numnodes)
			Mod_WidenBounds (mod->nodes + mod->submodels[i].headnode[0]);
	if (mod_widened)
		Con_Printf ("%s: %d BSP node(s) had faces outside their bounds, by up "
					"to %.0f units.\nWidened, so those faces are clipped to the "
					"screen.\n", mod->name, mod_widened, mod_widest);
	
//
// set up the submodels (FIXME: this is confusing)
//
	for (i=0 ; i<mod->numsubmodels ; i++)
	{
		bm = &mod->submodels[i];

		mod->hulls[0].firstclipnode = bm->headnode[0];
		for (j=1 ; j<MAX_MAP_HULLS ; j++)
		{
			mod->hulls[j].firstclipnode = bm->headnode[j];
			mod->hulls[j].lastclipnode = mod->numclipnodes-1;
		}
		
		mod->firstmodelsurface = bm->firstface;
		mod->nummodelsurfaces = bm->numfaces;
		
		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);
		mod->radius = RadiusFromBounds (mod->mins, mod->maxs);
		
		mod->numleafs = bm->visleafs;

		if (i < mod->numsubmodels-1)
		{	// duplicate the basic information
			char	name[10];

			sprintf (name, "*%i", i+1);
			loadmodel = Mod_FindName (name);
			*loadmodel = *mod;
			strcpy (loadmodel->name, name);
			mod = loadmodel;
		}
	}
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

/*
=================
Mod_LoadAliasFrame
=================
*/
void * Mod_LoadAliasFrame (void * pin, int *pframeindex, int numv,
	trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader, char *name)
{
	trivertx_t		*pframe, *pinframe;
	int				i, j;
	daliasframe_t	*pdaliasframe;

	pdaliasframe = (daliasframe_t *)pin;

	strcpy (name, pdaliasframe->name);

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about
	// endianness
		pbboxmin->v[i] = pdaliasframe->bboxmin.v[i];
		pbboxmax->v[i] = pdaliasframe->bboxmax.v[i];
	}

	pinframe = (trivertx_t *)(pdaliasframe + 1);
	pframe = Hunk_AllocName (numv * sizeof(*pframe), loadname);

	*pframeindex = (byte *)pframe - (byte *)pheader;

	for (j=0 ; j<numv ; j++)
	{
		int		k;

	// these are all byte values, so no need to deal with endianness
		pframe[j].lightnormalindex = pinframe[j].lightnormalindex;

		for (k=0 ; k<3 ; k++)
		{
			pframe[j].v[k] = pinframe[j].v[k];
		}
	}

	pinframe += numv;

	return (void *)pinframe;
}


/*
=================
Mod_LoadAliasGroup
=================
*/
void * Mod_LoadAliasGroup (void * pin, int *pframeindex, int numv,
	trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader, char *name)
{
	daliasgroup_t		*pingroup;
	maliasgroup_t		*paliasgroup;
	int					i, numframes;
	daliasinterval_t	*pin_intervals;
	float				*poutintervals;
	void				*ptemp;
	
	pingroup = (daliasgroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	paliasgroup = Hunk_AllocName (sizeof (maliasgroup_t) +
			(numframes - 1) * sizeof (paliasgroup->frames[0]), loadname);

	paliasgroup->numframes = numframes;

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about endianness
		pbboxmin->v[i] = pingroup->bboxmin.v[i];
		pbboxmax->v[i] = pingroup->bboxmax.v[i];
	}

	*pframeindex = (byte *)paliasgroup - (byte *)pheader;

	pin_intervals = (daliasinterval_t *)(pingroup + 1);

	poutintervals = Hunk_AllocName (numframes * sizeof (float), loadname);

	paliasgroup->intervals = (byte *)poutintervals - (byte *)pheader;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadAliasGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadAliasFrame (ptemp,
									&paliasgroup->frames[i].frame,
									numv,
									&paliasgroup->frames[i].bboxmin,
									&paliasgroup->frames[i].bboxmax,
									pheader, name);
	}

	return ptemp;
}


/*
=================
Mod_LoadAliasSkin
=================
*/
void * Mod_LoadAliasSkin (void * pin, int *pskinindex, int skinsize,
	aliashdr_t *pheader)
{
	int		i;
	byte	*pskin, *pinskin;
	unsigned short	*pusskin;

	pskin = Hunk_AllocName (skinsize * r_pixbytes, loadname);
	pinskin = (byte *)pin;
	*pskinindex = (byte *)pskin - (byte *)pheader;

	if (r_pixbytes == 1)
	{
		Q_memcpy (pskin, pinskin, skinsize);
	}
	else if (r_pixbytes == 2)
	{
		pusskin = (unsigned short *)pskin;

		for (i=0 ; i<skinsize ; i++)
			pusskin[i] = d_8to16table[pinskin[i]];
	}
	else
	{
		Sys_Error ("Mod_LoadAliasSkin: driver set invalid r_pixbytes: %d\n",
				 r_pixbytes);
	}

	pinskin += skinsize;

	return ((void *)pinskin);
}


/*
=================
Mod_LoadAliasSkinGroup
=================
*/
void * Mod_LoadAliasSkinGroup (void * pin, int *pskinindex, int skinsize,
	aliashdr_t *pheader)
{
	daliasskingroup_t		*pinskingroup;
	maliasskingroup_t		*paliasskingroup;
	int						i, numskins;
	daliasskininterval_t	*pinskinintervals;
	float					*poutskinintervals;
	void					*ptemp;

	pinskingroup = (daliasskingroup_t *)pin;

	numskins = LittleLong (pinskingroup->numskins);

	paliasskingroup = Hunk_AllocName (sizeof (maliasskingroup_t) +
			(numskins - 1) * sizeof (paliasskingroup->skindescs[0]),
			loadname);

	paliasskingroup->numskins = numskins;

	*pskinindex = (byte *)paliasskingroup - (byte *)pheader;

	pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

	poutskinintervals = Hunk_AllocName (numskins * sizeof (float),loadname);

	paliasskingroup->intervals = (byte *)poutskinintervals - (byte *)pheader;

	for (i=0 ; i<numskins ; i++)
	{
		*poutskinintervals = LittleFloat (pinskinintervals->interval);
		if (*poutskinintervals <= 0)
			Sys_Error ("Mod_LoadAliasSkinGroup: interval<=0");

		poutskinintervals++;
		pinskinintervals++;
	}

	ptemp = (void *)pinskinintervals;

	for (i=0 ; i<numskins ; i++)
	{
		ptemp = Mod_LoadAliasSkin (ptemp,
				&paliasskingroup->skindescs[i].skin, skinsize, pheader);
	}

	return ptemp;
}


/*
=================
Mod_LoadAliasModel
=================
*/
void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
	int					i;
	mdl_t				*pmodel, *pinmodel;
	stvert_t			*pstverts, *pinstverts;
	aliashdr_t			*pheader;
	mtriangle_t			*ptri;
	dtriangle_t			*pintriangles;
	int					version, numframes, numskins;
	int					size;
	daliasframetype_t	*pframetype;
	daliasskintype_t	*pskintype;
	maliasskindesc_t	*pskindesc;
	int					skinsize;
	int					start, end, total;
	
	start = Hunk_LowMark ();

	pinmodel = (mdl_t *)buffer;

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		Sys_Error ("%s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size = 	sizeof (aliashdr_t) + (LittleLong (pinmodel->numframes) - 1) *
			 sizeof (pheader->frames[0]) +
			sizeof (mdl_t) +
			LittleLong (pinmodel->numverts) * sizeof (stvert_t) +
			LittleLong (pinmodel->numtris) * sizeof (mtriangle_t);

	pheader = Hunk_AllocName (size, loadname);
	pmodel = (mdl_t *) ((byte *)&pheader[1] +
			(LittleLong (pinmodel->numframes) - 1) *
			 sizeof (pheader->frames[0]));
	
//	mod->cache.data = pheader;
	mod->flags = LittleLong (pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
	pmodel->boundingradius = LittleFloat (pinmodel->boundingradius);
	pmodel->numskins = LittleLong (pinmodel->numskins);
	pmodel->skinwidth = LittleLong (pinmodel->skinwidth);
	pmodel->skinheight = LittleLong (pinmodel->skinheight);

	if (pmodel->skinheight > MAX_LBM_HEIGHT)
		Sys_Error ("model %s has a skin taller than %d", mod->name,
				   MAX_LBM_HEIGHT);

	pmodel->numverts = LittleLong (pinmodel->numverts);

	if (pmodel->numverts <= 0)
		Sys_Error ("model %s has no vertices", mod->name);

	if (pmodel->numverts > MAXALIASVERTS)
		Sys_Error ("model %s has too many vertices", mod->name);

	pmodel->numtris = LittleLong (pinmodel->numtris);

	if (pmodel->numtris <= 0)
		Sys_Error ("model %s has no triangles", mod->name);

	pmodel->numframes = LittleLong (pinmodel->numframes);
	pmodel->size = LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
	mod->synctype = LittleLong (pinmodel->synctype);
	mod->numframes = pmodel->numframes;

	for (i=0 ; i<3 ; i++)
	{
		pmodel->scale[i] = LittleFloat (pinmodel->scale[i]);
		pmodel->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
		pmodel->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
	}

	numskins = pmodel->numskins;
	numframes = pmodel->numframes;

	if (pmodel->skinwidth & 0x03)
		Sys_Error ("Mod_LoadAliasModel: skinwidth not multiple of 4");

	pheader->model = (byte *)pmodel - (byte *)pheader;

//
// load the skins
//
	skinsize = pmodel->skinheight * pmodel->skinwidth;

	if (numskins < 1)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of skins: %d\n", numskins);

	pskintype = (daliasskintype_t *)&pinmodel[1];

	pskindesc = Hunk_AllocName (numskins * sizeof (maliasskindesc_t),
								loadname);

	pheader->skindesc = (byte *)pskindesc - (byte *)pheader;

	for (i=0 ; i<numskins ; i++)
	{
		aliasskintype_t	skintype;

		skintype = LittleLong (pskintype->type);
		pskindesc[i].type = skintype;

		if (skintype == ALIAS_SKIN_SINGLE)
		{
			pskintype = (daliasskintype_t *)
					Mod_LoadAliasSkin (pskintype + 1,
									   &pskindesc[i].skin,
									   skinsize, pheader);
		}
		else
		{
			pskintype = (daliasskintype_t *)
					Mod_LoadAliasSkinGroup (pskintype + 1,
											&pskindesc[i].skin,
											skinsize, pheader);
		}
	}

//
// set base s and t vertices
//
	pstverts = (stvert_t *)&pmodel[1];
	pinstverts = (stvert_t *)pskintype;

	pheader->stverts = (byte *)pstverts - (byte *)pheader;

	for (i=0 ; i<pmodel->numverts ; i++)
	{
		pstverts[i].onseam = LittleLong (pinstverts[i].onseam);
	// put s and t in 16.16 format
		pstverts[i].s = LittleLong (pinstverts[i].s) << 16;
		pstverts[i].t = LittleLong (pinstverts[i].t) << 16;
	}

//
// set up the triangles
//
	ptri = (mtriangle_t *)&pstverts[pmodel->numverts];
	pintriangles = (dtriangle_t *)&pinstverts[pmodel->numverts];

	pheader->triangles = (byte *)ptri - (byte *)pheader;

	for (i=0 ; i<pmodel->numtris ; i++)
	{
		int		j;

		ptri[i].facesfront = LittleLong (pintriangles[i].facesfront);

		for (j=0 ; j<3 ; j++)
		{
			ptri[i].vertindex[j] =
					LittleLong (pintriangles[i].vertindex[j]);
		}
	}

//
// load the frames
//
	if (numframes < 1)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of frames: %d\n", numframes);

	pframetype = (daliasframetype_t *)&pintriangles[pmodel->numtris];

	for (i=0 ; i<numframes ; i++)
	{
		aliasframetype_t	frametype;

		frametype = LittleLong (pframetype->type);
		pheader->frames[i].type = frametype;

		if (frametype == ALIAS_SINGLE)
		{
			pframetype = (daliasframetype_t *)
					Mod_LoadAliasFrame (pframetype + 1,
										&pheader->frames[i].frame,
										pmodel->numverts,
										&pheader->frames[i].bboxmin,
										&pheader->frames[i].bboxmax,
										pheader, pheader->frames[i].name);
		}
		else
		{
			pframetype = (daliasframetype_t *)
					Mod_LoadAliasGroup (pframetype + 1,
										&pheader->frames[i].frame,
										pmodel->numverts,
										&pheader->frames[i].bboxmin,
										&pheader->frames[i].bboxmax,
										pheader, pheader->frames[i].name);
		}
	}

	mod->type = mod_alias;

// FIXME: do this right
	mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
	mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;

//
// move the complete, relocatable alias model to the cache
//	
	end = Hunk_LowMark ();
	total = end - start;
	
	Cache_Alloc (&mod->cache, total, loadname);
	if (!mod->cache.data)
		return;
	memcpy (mod->cache.data, pheader, total);

	Hunk_FreeToLowMark (start);
}

//=============================================================================

/*
=================
Mod_LoadSpriteFrame
=================
*/
void * Mod_LoadSpriteFrame (void * pin, mspriteframe_t **ppframe)
{
	dspriteframe_t		*pinframe;
	mspriteframe_t		*pspriteframe;
	int					i, width, height, size, origin[2];
	unsigned short		*ppixout;
	byte				*ppixin;

	pinframe = (dspriteframe_t *)pin;

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height;

	pspriteframe = Hunk_AllocName (sizeof (mspriteframe_t) + size*r_pixbytes,
								   loadname);

	Q_memset (pspriteframe, 0, sizeof (mspriteframe_t) + size);
	*ppframe = pspriteframe;

	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong (pinframe->origin[0]);
	origin[1] = LittleLong (pinframe->origin[1]);

	pspriteframe->up = origin[1];
	pspriteframe->down = origin[1] - height;
	pspriteframe->left = origin[0];
	pspriteframe->right = width + origin[0];

	if (r_pixbytes == 1)
	{
		Q_memcpy (&pspriteframe->pixels[0], (byte *)(pinframe + 1), size);
	}
	else if (r_pixbytes == 2)
	{
		ppixin = (byte *)(pinframe + 1);
		ppixout = (unsigned short *)&pspriteframe->pixels[0];

		for (i=0 ; i<size ; i++)
			ppixout[i] = d_8to16table[ppixin[i]];
	}
	else
	{
		Sys_Error ("Mod_LoadSpriteFrame: driver set invalid r_pixbytes: %d\n",
				 r_pixbytes);
	}

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
void * Mod_LoadSpriteGroup (void * pin, mspriteframe_t **ppframe)
{
	dspritegroup_t		*pingroup;
	mspritegroup_t		*pspritegroup;
	int					i, numframes;
	dspriteinterval_t	*pin_intervals;
	float				*poutintervals;
	void				*ptemp;

	pingroup = (dspritegroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	pspritegroup = Hunk_AllocName (sizeof (mspritegroup_t) +
				(numframes - 1) * sizeof (pspritegroup->frames[0]), loadname);

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	pin_intervals = (dspriteinterval_t *)(pingroup + 1);

	poutintervals = Hunk_AllocName (numframes * sizeof (float), loadname);

	pspritegroup->intervals = poutintervals;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadSpriteGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i]);
	}

	return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	int					i;
	int					version;
	dsprite_t			*pin;
	msprite_t			*psprite;
	int					numframes;
	int					size;
	dspriteframetype_t	*pframetype;
	
	pin = (dsprite_t *)buffer;

	version = LittleLong (pin->version);
	if (version != SPRITE_VERSION)
		Sys_Error ("%s has wrong version number "
				 "(%i should be %i)", mod->name, version, SPRITE_VERSION);

	numframes = LittleLong (pin->numframes);

	size = sizeof (msprite_t) +	(numframes - 1) * sizeof (psprite->frames);

	psprite = Hunk_AllocName (size, loadname);

	mod->cache.data = psprite;

	psprite->type = LittleLong (pin->type);
	psprite->maxwidth = LittleLong (pin->width);
	psprite->maxheight = LittleLong (pin->height);
	psprite->beamlength = LittleFloat (pin->beamlength);
	mod->synctype = LittleLong (pin->synctype);
	psprite->numframes = numframes;

	mod->mins[0] = mod->mins[1] = -psprite->maxwidth/2;
	mod->maxs[0] = mod->maxs[1] = psprite->maxwidth/2;
	mod->mins[2] = -psprite->maxheight/2;
	mod->maxs[2] = psprite->maxheight/2;
	
//
// load the frames
//
	if (numframes < 1)
		Sys_Error ("Mod_LoadSpriteModel: Invalid # of frames: %d\n", numframes);

	mod->numframes = numframes;
	mod->flags = 0;

	pframetype = (dspriteframetype_t *)(pin + 1);

	for (i=0 ; i<numframes ; i++)
	{
		spriteframetype_t	frametype;

		frametype = LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		if (frametype == SPR_SINGLE)
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteFrame (pframetype + 1,
										 &psprite->frames[i].frameptr);
		}
		else
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteGroup (pframetype + 1,
										 &psprite->frames[i].frameptr);
		}
	}

	mod->type = mod_sprite;
}

//=============================================================================

/*
================
Mod_Print
================
*/
void Mod_Print (void)
{
	int		i;
	model_t	*mod;

	Con_Printf ("Cached models:\n");
	for (i=0, mod=mod_known ; i < mod_numknown ; i++, mod++)
	{
		Con_Printf ("%8p : %s",mod->cache.data, mod->name);
		if (mod->needload & NL_UNREFERENCED)
			Con_Printf (" (!R)");
		if (mod->needload & NL_NEEDS_LOADED)
			Con_Printf (" (!P)");
		Con_Printf ("\n");
	}
}



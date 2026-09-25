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
#include "quakedef.h"

#ifdef _WIN32
#include "winquake.h"
#endif

void (*vid_menudrawfn)(void);
void (*vid_menukeyfn)(int key);

enum {m_none, m_main, m_singleplayer, m_load, m_save, m_multiplayer, m_setup, m_net, m_options, m_video, m_keys, m_help, m_quit, m_game, m_serialconfig, m_modemconfig, m_lanconfig, m_gameoptions, m_search, m_slist, m_optpage} m_state;

void M_Menu_Main_f (void);
	void M_Menu_SinglePlayer_f (void);
		void M_Menu_Load_f (void);
		void M_Menu_Save_f (void);
	void M_Menu_MultiPlayer_f (void);
		void M_Menu_Setup_f (void);
		void M_Menu_Net_f (void);
	void M_Menu_Options_f (void);
	void M_Menu_Game_f (void);
		void M_Menu_Keys_f (void);
		void M_Menu_Video_f (void);
	void M_Menu_Help_f (void);
	void M_Menu_Quit_f (void);
void M_Menu_SerialConfig_f (void);
	void M_Menu_ModemConfig_f (void);
void M_Menu_LanConfig_f (void);
void M_Menu_GameOptions_f (void);
void M_Menu_Search_f (void);
void M_Menu_ServerList_f (void);

void M_Main_Draw (void);
	void M_SinglePlayer_Draw (void);
		void M_Load_Draw (void);
		void M_Save_Draw (void);
	void M_MultiPlayer_Draw (void);
		void M_Setup_Draw (void);
		void M_Net_Draw (void);
	void M_Options_Draw (void);
	void M_Game_Draw (void);
		void M_Keys_Draw (void);
		void M_Video_Draw (void);
	void M_Help_Draw (void);
	void M_Quit_Draw (void);
void M_SerialConfig_Draw (void);
	void M_ModemConfig_Draw (void);
void M_LanConfig_Draw (void);
void M_GameOptions_Draw (void);
void M_Search_Draw (void);
void M_ServerList_Draw (void);

void M_Main_Key (int key);
	void M_SinglePlayer_Key (int key);
		void M_Load_Key (int key);
		void M_Save_Key (int key);
	void M_MultiPlayer_Key (int key);
		void M_Setup_Key (int key);
		void M_Net_Key (int key);
	void M_Options_Key (int key);
	void M_Game_Key (int key);
		void M_Keys_Key (int key);
		void M_Video_Key (int key);
	void M_Help_Key (int key);
	void M_Quit_Key (int key);
void M_SerialConfig_Key (int key);
	void M_ModemConfig_Key (int key);
void M_LanConfig_Key (int key);
void M_GameOptions_Key (int key);
void M_Search_Key (int key);
void M_ServerList_Key (int key);

qboolean	m_entersound;		// play after drawing a frame, so caching
								// won't disrupt the sound
qboolean	m_recursiveDraw;

static int	opt_resume;		// the options page a submenu goes back to; see there

int			m_return_state;
qboolean	m_return_onerror;
char		m_return_reason [32];

#define StartingGame	(m_multiplayer_cursor == 1)
#define JoiningGame		(m_multiplayer_cursor == 0)
#define SerialConfig	(m_net_cursor == 0)
#define DirectConfig	(m_net_cursor == 1)
#define	IPXConfig		(m_net_cursor == 2)
#define	TCPIPConfig		(m_net_cursor == 3)

void M_ConfigureNetSubsystem(void);

/*
==============================================================================

THE MENU CANVAS

The re-release lays its menus out as one picture that fills the height of the
screen: id's plaque, the vertical QUAKE and id, runs from near the top to
near the bottom along the left edge; the title plaque sits at the top in the
middle; lists start just right of the plaque; and a line along the bottom
says which keys do what. Everything in it is sized against the text, and the
art is drawn larger than the text -- the plaque at twice the size of a letter
pixel, the title plaques at about one and a half.

Measured off the re-release at 1129x702, where a letter pixel is two screen
pixels, that picture is 564 by 351 letter pixels. So that is the box the menus
here are laid out in: MBOX_W by MBOX_H, in the same units, and drawn at
whatever whole number of screen pixels per unit makes it fill the most of the
screen's height (M_BeginCanvas). A screen of another shape has the box in its
middle. The plaque is drawn at twice the scale, as there; the title plaques at
id's own size, since one and a half is not a whole number of pixels.

id's menus were drawn in a 320x200 space. The ones that have not been redrawn
for the box still are: M_Print, M_DrawPic and the rest take id's coordinates
and place them at m_ox, m_oy, which puts id's text column, x = 56 on its
screen, at the box's, and its first line under the title where the box's
first line is.

==============================================================================
*/

#define	MBOX_W		564
#define	MBOX_H		351
#define	MBOX_MINH	310		// as short as it may be squeezed

extern cvar_t	scr_scale;

int			m_bx, m_by, m_bw, m_bh;		// the box, on the canvas
static int	m_ox, m_oy;					// where id's 320x200 starts

static int	m_depth;
static int	m_oldscale, m_oldwidth, m_oldheight, m_oldyoff;

// Colours for text in the re-release's scheme: id's white characters, run
// through a table that keeps their shading and changes their colour.
static byte	m_tint_dim[256];		// headings, and slots with nothing in
static byte	m_tint_orange[256];		// the cursor, and rows that do something
static byte	m_tint_foot[256];		// the key hints along the bottom
static int	m_col_rule, m_col_box, m_col_track, m_col_thumb;
static qboolean	m_tints_built;

static int M_NearestColor (int r, int g, int b)
{
	int		i, best, bestdist, dist, dr, dg, db;
	byte	*pal = host_basepal;

	best = 1;
	bestdist = 0x7fffffff;
	for (i = 1 ; i < 255 ; i++)
	{
		dr = r - pal[i*3+0];
		dg = g - pal[i*3+1];
		db = b - pal[i*3+2];
		dist = dr*dr + dg*dg + db*db;
		if (dist < bestdist)
		{
			bestdist = dist;
			best = i;
		}
	}
	return best;
}

static int M_TintChannel (int c, int l, int lmax)
{
	c = c * l / lmax;
	return c > 255 ? 255 : c;
}

static void M_BuildTint (byte *table, int r, int g, int b, int lmax)
{
	int		c, l;
	byte	*pal = host_basepal;

	for (c = 0 ; c < 256 ; c++)
	{
		l = (pal[c*3+0] + pal[c*3+1] + pal[c*3+2]) / 3;
		table[c] = M_NearestColor (M_TintChannel (r, l, lmax),
			M_TintChannel (g, l, lmax), M_TintChannel (b, l, lmax));
	}
}

static void M_BuildTints (void)
{
	extern byte	*draw_chars;
	int			i, c, l, lmax;
	byte		*pal = host_basepal;

// the brightest pixel in id's white characters: the tints give that one
// their colour, and the rest in proportion
	lmax = 1;
	for (i = 0 ; i < 128*64 ; i++)
	{
		c = draw_chars[i];
		if (!c)
			continue;
		l = (pal[c*3+0] + pal[c*3+1] + pal[c*3+2]) / 3;
		if (l > lmax)
			lmax = l;
	}

	M_BuildTint (m_tint_dim, 138, 118, 100, lmax);
	M_BuildTint (m_tint_orange, 222, 128, 40, lmax);
	M_BuildTint (m_tint_foot, 150, 150, 156, lmax);
	m_col_rule = M_NearestColor (68, 58, 48);
	m_col_box = M_NearestColor (40, 27, 18);
	m_col_track = M_NearestColor (30, 23, 17);
	m_col_thumb = M_NearestColor (112, 70, 36);
	m_tints_built = true;
}

/*
================
M_BeginCanvas

The canvas M_Draw draws on. Nested calls (the quit prompt draws the menu it
was opened from underneath it) leave the outermost one in charge.
================
*/
static void M_BeginCanvas (void)
{
	int		s;

	if (m_depth++)
		return;

	m_oldscale = draw_scale;
	m_oldwidth = vid.conwidth;
	m_oldheight = vid.conheight;
	m_oldyoff = draw_yoff;

// As large as fits, allowing the box to lose some of its height: the plaque
// moves up into its top margin to keep clear of the key hints (M_DrawFrame),
// and a page shows fewer rows. A browser window is often a little short of a
// whole multiple of the box -- 1920x969 is 323 rows at three pixels -- and
// the menus are better a size larger and a little tighter than a size smaller.
// scr_scale set by hand is the size somebody asked for, and the menus keep
// to it.
	if (scr_scale.value > 0)
		s = draw_scale;
	else
	{
		s = vid.height / MBOX_MINH;
		while (s > 1 && (int)vid.width / s < 400)
			s--;
		if (s < 1)
			s = 1;
	}

	draw_scale = s;
	vid.conwidth = vid.width / s;
	vid.conheight = vid.height / s;
	draw_yoff = (vid.height - vid.conheight * s) / 2;

	m_bw = vid.conwidth < MBOX_W ? vid.conwidth : MBOX_W;
	m_bh = vid.conheight < MBOX_H ? vid.conheight : MBOX_H;
	m_bx = (vid.conwidth - m_bw) / 2;
	m_by = (vid.conheight - m_bh) / 2;

	m_ox = m_bx + 38;
	m_oy = m_by + 40;

	if (!m_tints_built)
		M_BuildTints ();
}

static void M_EndCanvas (void)
{
	if (--m_depth)
		return;

	draw_scale = m_oldscale;
	vid.conwidth = m_oldwidth;
	vid.conheight = m_oldheight;
	draw_yoff = m_oldyoff;
}

/*
================
M_Centre320

For the screens that are id's 320x200 pictures, or boxes drawn to sit in the
middle of one (help, the quit prompt, the video modes): id's space in the
middle of the canvas, rather than under the title.
================
*/
void M_Centre320 (void)
{
	m_ox = (vid.conwidth - 320) / 2;
	m_oy = (vid.conheight - 200) / 2;
}


/*
================
M_DrawCharacter

Draws one solid graphics character, in id's coordinates
================
*/
void M_DrawCharacter (int cx, int line, int num)
{
	Draw_CharacterEx (cx + m_ox, line + m_oy, num, 1, NULL);
}

void M_Print (int cx, int cy, char *str)
{
	while (*str)
	{
		M_DrawCharacter (cx, cy, (*str)+128);
		str++;
		cx += 8;
	}
}

void M_PrintWhite (int cx, int cy, char *str)
{
	while (*str)
	{
		M_DrawCharacter (cx, cy, *str);
		str++;
		cx += 8;
	}
}

void M_DrawTransPic (int x, int y, qpic_t *pic)
{
	Draw_PicPart (x + m_ox, y + m_oy, pic, 0, pic->height, 1, NULL);
}

void M_DrawPic (int x, int y, qpic_t *pic)
{
	x += m_ox;
	y += m_oy;
	if (x >= 0 && y >= 0 && x + pic->width <= (int)vid.conwidth
		&& y + pic->height <= (int)vid.conheight)
		Draw_Pic (x, y, pic);
	else
		Draw_PicPart (x, y, pic, 0, pic->height, 1, NULL);
}

byte identityTable[256];
byte translationTable[256];

void M_BuildTranslationTable(int top, int bottom)
{
	int		j;
	byte	*dest, *source;

	for (j = 0; j < 256; j++)
		identityTable[j] = j;
	dest = translationTable;
	source = identityTable;
	memcpy (dest, source, 256);

	if (top < 128)	// the artists made some backwards ranges.  sigh.
		memcpy (dest + TOP_RANGE, source + top, 16);
	else
		for (j=0 ; j<16 ; j++)
			dest[TOP_RANGE+j] = source[top+15-j];

	if (bottom < 128)
		memcpy (dest + BOTTOM_RANGE, source + bottom, 16);
	else
		for (j=0 ; j<16 ; j++)
			dest[BOTTOM_RANGE+j] = source[bottom+15-j];
}


void M_DrawTransPicTranslate (int x, int y, qpic_t *pic)
{
	Draw_PicPart (x + m_ox, y + m_oy, pic, 0, pic->height, 1,
		translationTable);
}


/*
==============================================================================

DRAWING IN THE BOX

Coordinates in the box, 0,0 its top left.

==============================================================================
*/

// where things go, measured off the re-release
#define	MB_PLAQUE_X		22
#define	MB_PLAQUE_Y		24
#define	MB_TITLE_Y		29
#define	MB_LIST_X		106		// big lettering
#define	MB_LIST_Y		73
#define	MB_LIST_ROW		26
#define	MB_DOT_X		84		// the spinning Quake symbol, left of a row
#define	MB_LABEL_X		94		// small lettering
#define	MB_ARROW_X		84		// the arrow, left of a row of it
#define	MB_FOOT_X		20		// the key hints, this far from the sides
#define	MB_FOOT_Y		11		// and this far up from the bottom

static void M_BoxText (int x, int y, char *s, byte *tint)
{
	for ( ; *s ; s++, x += 8)
		Draw_CharacterEx (m_bx + x, m_by + y, *s, 1, tint);
}

static void M_BoxTextRight (int right, int y, char *s, byte *tint)
{
	M_BoxText (right - 8 * strlen (s), y, s, tint);
}

// id's gold characters at twice the size, for a list in big lettering that id
// has no picture of; white for the one to pick out
static void M_BoxBigText (int x, int y, char *s, qboolean white)
{
	for ( ; *s ; s++, x += 16)
		Draw_CharacterEx (m_bx + x, m_by + y, white ? *s : (*s) | 128, 2,
			NULL);
}

static void M_BoxFill (int x, int y, int w, int h, int c)
{
	Draw_Fill (m_bx + x, m_by + y, w, h, c);
}

static void M_BoxPic (int x, int y, qpic_t *pic, int mult)
{
	Draw_PicPart (m_bx + x, m_by + y, pic, 0, pic->height, mult, NULL);
}

/*
================
M_DrawFrame

The plaque, and the title if there is one.
================
*/
static void M_DrawFrame (char *title)
{
	qpic_t	*p;
	int		y;

	p = Draw_CachePic ("gfx/qplaque.lmp");
	y = m_bh - MB_FOOT_Y - 3 - 2 * p->height;
	M_BoxPic (MB_PLAQUE_X, y < MB_PLAQUE_Y ? y : MB_PLAQUE_Y, p, 2);

	if (title)
	{
		p = Draw_CachePic (title);
		M_BoxPic ((m_bw + 18 - p->width) / 2, MB_TITLE_Y, p, 1);
	}
}

/*
================
M_DrawFooter

What the keys do, along the bottom: going back on the left, going on on the
right. Either may be NULL.
================
*/
static void M_DrawFooter (char *left, char *right)
{
	if (left)
		M_BoxText (MB_FOOT_X, m_bh - MB_FOOT_Y, left, m_tint_foot);
	if (right)
		M_BoxTextRight (m_bw - MB_FOOT_X + 6, m_bh - MB_FOOT_Y, right,
			m_tint_foot);
}

/*
================
M_DrawDot

The spinning Quake symbol, beside row `row` of a list in big lettering.
================
*/
static void M_DrawDot (int row)
{
	int		f = (int)(host_time * 10) % 6;

	M_BoxPic (MB_DOT_X, MB_LIST_Y + row * MB_LIST_ROW,
		Draw_CachePic (va ("gfx/menudot%i.lmp", f+1)), 1);
}

/*
================
M_DrawListPic

id's lists in big lettering are single pictures, one item every 20 rows, with
the letters of one item touching the next where a descender meets a capital.
The re-release spaces them 26 apart. So the picture is taken apart the first
time it is drawn: each connected shape goes with the item its middle is in,
and each item becomes a picture of its own, drawn a row of the re-release's
apart.
================
*/
#define	MAX_LISTPICS	4
#define	MAX_LISTITEMS	8

typedef struct
{
	char	name[MAX_QPATH];
	int		count;
	qpic_t	*item[MAX_LISTITEMS];
	int		top[MAX_LISTITEMS];		// its first row, in the whole picture
} listpic_t;

static listpic_t	m_listpics[MAX_LISTPICS];
static int			m_numlistpics;

static listpic_t *M_SplitListPic (char *name, int count)
{
	listpic_t	*lp;
	qpic_t		*pic, *ip;
	int			w, h, n, i, x, y, c, head, tail, miny, maxy, owner, ncomp, ih;
	int			*label, *queue, *owners;
	int			top[MAX_LISTITEMS], bottom[MAX_LISTITEMS];

	for (i = 0 ; i < m_numlistpics ; i++)
		if (!strcmp (m_listpics[i].name, name))
			return &m_listpics[i];
	if (m_numlistpics == MAX_LISTPICS || count > MAX_LISTITEMS)
		return NULL;

	pic = Draw_CachePic (name);
	w = pic->width;
	h = pic->height;
	n = w * h;
	label = malloc (n * sizeof(*label));
	queue = malloc (n * sizeof(*queue));
	owners = malloc (n * sizeof(*owners));
	if (!label || !queue || !owners)
		Sys_Error ("M_SplitListPic: out of memory");

// label the shapes, eight ways connected, and give each one to the item its
// middle row falls in
	for (i = 0 ; i < n ; i++)
		label[i] = -1;
	ncomp = 0;
	for (i = 0 ; i < n ; i++)
	{
		if (label[i] != -1 || pic->data[i] == TRANSPARENT_COLOR)
			continue;
		head = tail = 0;
		queue[tail++] = i;
		label[i] = ncomp;
		miny = maxy = i / w;
		while (head < tail)
		{
			int		p = queue[head++], px = p % w, py = p / w, dx, dy, q;

			if (py < miny)
				miny = py;
			if (py > maxy)
				maxy = py;
			for (dy = -1 ; dy <= 1 ; dy++)
				for (dx = -1 ; dx <= 1 ; dx++)
				{
					if (px + dx < 0 || px + dx >= w || py + dy < 0
						|| py + dy >= h)
						continue;
					q = p + dy * w + dx;
					if (label[q] != -1 || pic->data[q] == TRANSPARENT_COLOR)
						continue;
					label[q] = ncomp;
					queue[tail++] = q;
				}
		}
		owner = ((miny + maxy) / 2) / 20;
		if (owner >= count)
			owner = count - 1;
		owners[ncomp++] = owner;
	}

	for (i = 0 ; i < count ; i++)
	{
		top[i] = h;
		bottom[i] = -1;
	}
	for (i = 0 ; i < n ; i++)
		if (label[i] != -1)
		{
			owner = owners[label[i]];
			y = i / w;
			if (y < top[owner])
				top[owner] = y;
			if (y > bottom[owner])
				bottom[owner] = y;
		}

	lp = &m_listpics[m_numlistpics++];
	Q_strncpy (lp->name, name, sizeof(lp->name) - 1);
	lp->count = count;
	for (i = 0 ; i < count ; i++)
	{
		if (bottom[i] < top[i])
			top[i] = bottom[i] = i * 20;	// an item with nothing in it
		ih = bottom[i] - top[i] + 1;
		ip = malloc (sizeof(qpic_t) + w * ih);
		if (!ip)
			Sys_Error ("M_SplitListPic: out of memory");
		ip->width = w;
		ip->height = ih;
		for (y = 0 ; y < ih ; y++)
			for (x = 0 ; x < w ; x++)
			{
				c = (top[i] + y) * w + x;
				ip->data[y * w + x] = (label[c] != -1
					&& owners[label[c]] == i) ? pic->data[c]
					: TRANSPARENT_COLOR;
			}
		lp->item[i] = ip;
		lp->top[i] = top[i];
	}

	free (label);
	free (queue);
	free (owners);
	return lp;
}

static void M_DrawListPic (char *name, int count)
{
	listpic_t	*lp = M_SplitListPic (name, count);
	int			i;

	if (!lp)
		return;
	for (i = 0 ; i < lp->count ; i++)
		M_BoxPic (MB_LIST_X, MB_LIST_Y + i * MB_LIST_ROW + lp->top[i] - i * 20,
			lp->item[i], 1);
}


void M_DrawTextBox (int x, int y, int width, int lines)
{
	qpic_t	*p;
	int		cx, cy;
	int		n;

	// draw left side
	cx = x;
	cy = y;
	p = Draw_CachePic ("gfx/box_tl.lmp");
	M_DrawTransPic (cx, cy, p);
	p = Draw_CachePic ("gfx/box_ml.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		M_DrawTransPic (cx, cy, p);
	}
	p = Draw_CachePic ("gfx/box_bl.lmp");
	M_DrawTransPic (cx, cy+8, p);

	// draw middle
	cx += 8;
	while (width > 0)
	{
		cy = y;
		p = Draw_CachePic ("gfx/box_tm.lmp");
		M_DrawTransPic (cx, cy, p);
		p = Draw_CachePic ("gfx/box_mm.lmp");
		for (n = 0; n < lines; n++)
		{
			cy += 8;
			if (n == 1)
				p = Draw_CachePic ("gfx/box_mm2.lmp");
			M_DrawTransPic (cx, cy, p);
		}
		p = Draw_CachePic ("gfx/box_bm.lmp");
		M_DrawTransPic (cx, cy+8, p);
		width -= 2;
		cx += 16;
	}

	// draw right side
	cy = y;
	p = Draw_CachePic ("gfx/box_tr.lmp");
	M_DrawTransPic (cx, cy, p);
	p = Draw_CachePic ("gfx/box_mr.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		M_DrawTransPic (cx, cy, p);
	}
	p = Draw_CachePic ("gfx/box_br.lmp");
	M_DrawTransPic (cx, cy+8, p);
}

//=============================================================================

int m_save_demonum;

/*
================
M_ToggleMenu_f
================
*/
void M_ToggleMenu_f (void)
{
	m_entersound = true;

	if (key_dest == key_menu)
	{
		if (m_state != m_main)
		{
			M_Menu_Main_f ();
			return;
		}
		key_dest = key_game;
		m_state = m_none;
		return;
	}
	if (key_dest == key_console)
	{
		Con_ToggleConsole_f ();
	}
	else
	{
		M_Menu_Main_f ();
	}
}


//=============================================================================
/* MAIN MENU */

int	m_main_cursor;
#define	MAIN_ITEMS	5


void M_Menu_Main_f (void)
{
	opt_resume = -1;
	if (key_dest != key_menu)
	{
		m_save_demonum = cls.demonum;
		cls.demonum = -1;
	}
	key_dest = key_menu;
	m_state = m_main;
	m_entersound = true;
}


void M_Main_Draw (void)
{
	M_DrawFrame ("gfx/ttl_main.lmp");
	M_DrawListPic ("gfx/mainmenu.lmp", MAIN_ITEMS);
	M_DrawDot (m_main_cursor);
	M_DrawFooter ("Backspace: Back", "Enter: Select");
}


void M_Main_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		key_dest = key_game;
		m_state = m_none;
		cls.demonum = m_save_demonum;
		if (cls.demonum != -1 && !cls.demoplayback && cls.state != ca_connected)
			CL_NextDemo ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_main_cursor >= MAIN_ITEMS)
			m_main_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_main_cursor < 0)
			m_main_cursor = MAIN_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;

		switch (m_main_cursor)
		{
		case 0:
			M_Menu_SinglePlayer_f ();
			break;

		case 1:
			M_Menu_MultiPlayer_f ();
			break;

		case 2:
			M_Menu_Options_f ();
			break;

		case 3:
			M_Menu_Help_f ();
			break;

		case 4:
			M_Menu_Quit_f ();
			break;
		}
	}
}

//=============================================================================
/* SINGLE PLAYER MENU */

int	m_singleplayer_cursor;
#define	SINGLEPLAYER_ITEMS	3


void M_Menu_SinglePlayer_f (void)
{
	key_dest = key_menu;
	m_state = m_singleplayer;
	m_entersound = true;
}


void M_SinglePlayer_Draw (void)
{
	M_DrawFrame ("gfx/ttl_sgl.lmp");
	M_DrawListPic ("gfx/sp_menu.lmp", SINGLEPLAYER_ITEMS);
	M_DrawDot (m_singleplayer_cursor);
	M_DrawFooter ("Backspace: Back", "Enter: Select");
}


void M_SinglePlayer_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_singleplayer_cursor >= SINGLEPLAYER_ITEMS)
			m_singleplayer_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_singleplayer_cursor < 0)
			m_singleplayer_cursor = SINGLEPLAYER_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;

		switch (m_singleplayer_cursor)
		{
		case 0:
			if (sv.active)
				if (!SCR_ModalMessage("Are you sure you want to\nstart a new game?\n"))
					break;
			key_dest = key_game;
			if (sv.active)
				Cbuf_AddText ("disconnect\n");
			Cbuf_AddText ("maxplayers 1\n");
			Cbuf_AddText ("map start\n");
			break;

		case 1:
			M_Menu_Load_f ();
			break;

		case 2:
			M_Menu_Save_f ();
			break;
		}
	}
}

//=============================================================================
/* LOAD/SAVE MENU */

int		load_cursor;		// 0 < load_cursor < SAVE_SLOTS

#define	MAX_SAVEGAMES		12

//
// The quicksave, as a row of its own.
//
// F6 and F9 are bound to "save quick" and "load quick", which writes
// quick.sav -- and this menu only ever looked for s0.sav to s11.sav, so the
// save everybody actually uses was the one save the menu would not show. In
// 1996 that was survivable, because F9 was right there. In a browser it is
// not: the page may never see F9, and a player who has quicksaved has no way
// back to it at all.
//
// One more row, after a blank line so it reads as separate, and drawn in white
// rather than gold so it is still identifiable once it holds a real comment.
//
#define	QUICK_SLOT			MAX_SAVEGAMES
#define	SAVE_SLOTS			(MAX_SAVEGAMES + 1)

char	m_filenames[SAVE_SLOTS][SAVEGAME_COMMENT_LENGTH+1];
int		loadable[SAVE_SLOTS];

// What the load and save commands call the slot, and where its row sits.
static char *M_SaveSlotName (int i)
{
	static char	name[16];

	if (i == QUICK_SLOT)
		return "quick";

	sprintf (name, "s%i", i);
	return name;
}

static int M_SaveSlotY (int i)
{
	return 69 + 16*i + (i == QUICK_SLOT ? 8 : 0);
}

void M_ScanSaves (void)
{
	int		i, j;
	char	name[MAX_OSPATH];
	FILE	*f;
	int		version;

	for (i=0 ; i<SAVE_SLOTS ; i++)
	{
		strcpy (m_filenames[i], i == QUICK_SLOT
				? "--- QUICKSAVE SLOT ---" : "--- UNUSED SLOT ---");
		loadable[i] = false;
		snprintf (name, sizeof(name), "%s/%s.sav", com_gamedir,
				  M_SaveSlotName (i));
		f = fopen (name, "r");
		if (!f)
			continue;
		fscanf (f, "%i\n", &version);
		fscanf (f, "%79s\n", name);
		strncpy (m_filenames[i], name, sizeof(m_filenames[i])-1);

	// change _ back to space
		for (j=0 ; j<SAVEGAME_COMMENT_LENGTH ; j++)
			if (m_filenames[i][j] == '_')
				m_filenames[i][j] = ' ';
		loadable[i] = true;
		fclose (f);
	}
}

void M_Menu_Load_f (void)
{
	m_entersound = true;
	m_state = m_load;
	key_dest = key_menu;
	M_ScanSaves ();
}


void M_Menu_Save_f (void)
{
	if (!sv.active)
		return;
	if (cl.intermission)
		return;
	if (svs.maxclients != 1)
		return;
	m_entersound = true;
	m_state = m_save;
	key_dest = key_menu;
	M_ScanSaves ();
}


/*
================
M_DrawSaves

The slots, a row of the re-release's apart. Empty ones dimmed, and the
quicksave labelled, since its comment reads like any other.
================
*/
static void M_DrawSaves (char *title, char *enter)
{
	int		i;

	M_DrawFrame (title);

	for (i=0 ; i<SAVE_SLOTS ; i++)
		M_BoxText (MB_LABEL_X, M_SaveSlotY (i), m_filenames[i],
			loadable[i] ? NULL : m_tint_dim);
	M_BoxTextRight (m_bw - 12, M_SaveSlotY (QUICK_SLOT), "quicksave",
		m_tint_dim);

	M_BoxText (MB_ARROW_X, M_SaveSlotY (load_cursor), "\015", m_tint_orange);
	M_DrawFooter ("Backspace: Back", enter);
}

void M_Load_Draw (void)
{
	M_DrawSaves ("gfx/p_load.lmp", "Enter: Load");
}


void M_Save_Draw (void)
{
	M_DrawSaves ("gfx/p_save.lmp", "Enter: Save");
}


void M_Load_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_SinglePlayer_f ();
		break;

	case K_ENTER:
		S_LocalSound ("misc/menu2.wav");
		if (!loadable[load_cursor])
			return;
		m_state = m_none;
		key_dest = key_game;

	// Host_Loadgame_f can't bring up the loading plaque because too much
	// stack space has been used, so do it now
		SCR_BeginLoadingPlaque ();

	// issue the load command
		Cbuf_AddText (va ("load %s\n", M_SaveSlotName (load_cursor)) );
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = SAVE_SLOTS-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor++;
		if (load_cursor >= SAVE_SLOTS)
			load_cursor = 0;
		break;
	}
}


void M_Save_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_SinglePlayer_f ();
		break;

	case K_ENTER:
		m_state = m_none;
		key_dest = key_game;
		Cbuf_AddText (va("save %s\n", M_SaveSlotName (load_cursor)));
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = SAVE_SLOTS-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor++;
		if (load_cursor >= SAVE_SLOTS)
			load_cursor = 0;
		break;
	}
}

//=============================================================================
/* MULTIPLAYER MENU */

int	m_multiplayer_cursor;
#define	MULTIPLAYER_ITEMS	3


void M_Menu_MultiPlayer_f (void)
{
	key_dest = key_menu;
	m_state = m_multiplayer;
	m_entersound = true;
}


void M_MultiPlayer_Draw (void)
{
	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawListPic ("gfx/mp_menu.lmp", MULTIPLAYER_ITEMS);
	M_DrawDot (m_multiplayer_cursor);
	M_DrawFooter ("Backspace: Back", "Enter: Select");

	if (serialAvailable || ipxAvailable || tcpipAvailable)
		return;
	M_PrintWhite ((320/2) - ((27*8)/2), 148, "No Communications Available");
}


void M_MultiPlayer_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_multiplayer_cursor >= MULTIPLAYER_ITEMS)
			m_multiplayer_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_multiplayer_cursor < 0)
			m_multiplayer_cursor = MULTIPLAYER_ITEMS - 1;
		break;

	case K_ENTER:
		m_entersound = true;
		switch (m_multiplayer_cursor)
		{
		case 0:
			if (serialAvailable || ipxAvailable || tcpipAvailable)
				M_Menu_Net_f ();
			break;

		case 1:
			if (serialAvailable || ipxAvailable || tcpipAvailable)
				M_Menu_Net_f ();
			break;

		case 2:
			M_Menu_Setup_f ();
			break;
		}
	}
}

//=============================================================================
/* SETUP MENU */

int		setup_cursor = 4;
int		setup_cursor_table[] = {40, 56, 80, 104, 140};

char	setup_hostname[16];
char	setup_myname[16];
int		setup_oldtop;
int		setup_oldbottom;
int		setup_top;
int		setup_bottom;

#define	NUM_SETUP_CMDS	5

void M_Menu_Setup_f (void)
{
	key_dest = key_menu;
	m_state = m_setup;
	m_entersound = true;
	Q_strcpy(setup_myname, cl_name.string);
	Q_strcpy(setup_hostname, hostname.string);
	setup_top = setup_oldtop = ((int)cl_color.value) >> 4;
	setup_bottom = setup_oldbottom = ((int)cl_color.value) & 15;
}


void M_Setup_Draw (void)
{
	qpic_t	*p;

	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Escape: Back", "Enter: Select");

	M_Print (64, 40, "Hostname");
	M_DrawTextBox (160, 32, 16, 1);
	M_Print (168, 40, setup_hostname);

	M_Print (64, 56, "Your name");
	M_DrawTextBox (160, 48, 16, 1);
	M_Print (168, 56, setup_myname);

	M_Print (64, 80, "Shirt color");
	M_Print (64, 104, "Pants color");

	M_DrawTextBox (64, 140-8, 14, 1);
	M_Print (72, 140, "Accept Changes");

	p = Draw_CachePic ("gfx/bigbox.lmp");
	M_DrawTransPic (160, 64, p);
	p = Draw_CachePic ("gfx/menuplyr.lmp");
	M_BuildTranslationTable(setup_top*16, setup_bottom*16);
	M_DrawTransPicTranslate (172, 72, p);

	M_DrawCharacter (56, setup_cursor_table [setup_cursor], 12+((int)(realtime*4)&1));

	if (setup_cursor == 0)
		M_DrawCharacter (168 + 8*strlen(setup_hostname), setup_cursor_table [setup_cursor], 10+((int)(realtime*4)&1));

	if (setup_cursor == 1)
		M_DrawCharacter (168 + 8*strlen(setup_myname), setup_cursor_table [setup_cursor], 10+((int)(realtime*4)&1));
}


void M_Setup_Key (int k)
{
	int			l;

	switch (k)
	{
	case K_ESCAPE:
		M_Menu_MultiPlayer_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		setup_cursor--;
		if (setup_cursor < 0)
			setup_cursor = NUM_SETUP_CMDS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		setup_cursor++;
		if (setup_cursor >= NUM_SETUP_CMDS)
			setup_cursor = 0;
		break;

	case K_LEFTARROW:
		if (setup_cursor < 2)
			return;
		S_LocalSound ("misc/menu3.wav");
		if (setup_cursor == 2)
			setup_top = setup_top - 1;
		if (setup_cursor == 3)
			setup_bottom = setup_bottom - 1;
		break;
	case K_RIGHTARROW:
		if (setup_cursor < 2)
			return;
forward:
		S_LocalSound ("misc/menu3.wav");
		if (setup_cursor == 2)
			setup_top = setup_top + 1;
		if (setup_cursor == 3)
			setup_bottom = setup_bottom + 1;
		break;

	case K_ENTER:
		if (setup_cursor == 0 || setup_cursor == 1)
			return;

		if (setup_cursor == 2 || setup_cursor == 3)
			goto forward;

		// setup_cursor == 4 (OK)
		if (Q_strcmp(cl_name.string, setup_myname) != 0)
			Cbuf_AddText ( va ("name \"%s\"\n", setup_myname) );
		if (Q_strcmp(hostname.string, setup_hostname) != 0)
			Cvar_Set("hostname", setup_hostname);
		if (setup_top != setup_oldtop || setup_bottom != setup_oldbottom)
			Cbuf_AddText( va ("color %i %i\n", setup_top, setup_bottom) );
		m_entersound = true;
		M_Menu_MultiPlayer_f ();
		break;

	case K_BACKSPACE:
		if (setup_cursor == 0)
		{
			if (strlen(setup_hostname))
				setup_hostname[strlen(setup_hostname)-1] = 0;
		}

		if (setup_cursor == 1)
		{
			if (strlen(setup_myname))
				setup_myname[strlen(setup_myname)-1] = 0;
		}
		break;

	default:
		if (k < 32 || k > 127)
			break;
		if (setup_cursor == 0)
		{
			l = strlen(setup_hostname);
			if (l < 15)
			{
				setup_hostname[l+1] = 0;
				setup_hostname[l] = k;
			}
		}
		if (setup_cursor == 1)
		{
			l = strlen(setup_myname);
			if (l < 15)
			{
				setup_myname[l+1] = 0;
				setup_myname[l] = k;
			}
		}
	}

	if (setup_top > 13)
		setup_top = 0;
	if (setup_top < 0)
		setup_top = 13;
	if (setup_bottom > 13)
		setup_bottom = 0;
	if (setup_bottom < 0)
		setup_bottom = 13;
}

//=============================================================================
/* NET MENU */

int	m_net_cursor;
int m_net_items;
int m_net_saveHeight;

char *net_helpMessage [] =
{
/* .........1.........2.... */
  "                        ",
  " Two computers connected",
  "   through two modems.  ",
  "                        ",

  "                        ",
  " Two computers connected",
  " by a null-modem cable. ",
  "                        ",

  " Novell network LANs    ",
  " or Windows 95 DOS-box. ",
  "                        ",
  "(LAN=Local Area Network)",

  " Commonly used to play  ",
  " over the Internet, but ",
  " also used on a Local   ",
  " Area Network.          "
};

void M_Menu_Net_f (void)
{
	key_dest = key_menu;
	m_state = m_net;
	m_entersound = true;
	m_net_items = 4;

	if (m_net_cursor >= m_net_items)
		m_net_cursor = 0;
	m_net_cursor--;
	M_Net_Key (K_DOWNARROW);
}


void M_Net_Draw (void)
{
	int		f;
	qpic_t	*p;

	M_DrawFrame ("gfx/p_multi.lmp");

// id's pictures for these are one item each, with the letters 6 rows down
	f = MB_LIST_Y - 3;

	if (serialAvailable)
	{
		p = Draw_CachePic ("gfx/netmen1.lmp");
	}
	else
	{
#ifdef _WIN32
		p = NULL;
#else
		p = Draw_CachePic ("gfx/dim_modm.lmp");
#endif
	}

	if (p)
		M_BoxPic (MB_LIST_X, f, p, 1);

	f += MB_LIST_ROW;

	if (serialAvailable)
	{
		p = Draw_CachePic ("gfx/netmen2.lmp");
	}
	else
	{
#ifdef _WIN32
		p = NULL;
#else
		p = Draw_CachePic ("gfx/dim_drct.lmp");
#endif
	}

	if (p)
		M_BoxPic (MB_LIST_X, f, p, 1);

	f += MB_LIST_ROW;
	if (ipxAvailable)
		p = Draw_CachePic ("gfx/netmen3.lmp");
	else
		p = Draw_CachePic ("gfx/dim_ipx.lmp");
	M_BoxPic (MB_LIST_X, f, p, 1);

	f += MB_LIST_ROW;
	if (tcpipAvailable)
		p = Draw_CachePic ("gfx/netmen4.lmp");
	else
		p = Draw_CachePic ("gfx/dim_tcp.lmp");
	M_BoxPic (MB_LIST_X, f, p, 1);

	if (m_net_items == 5)	// JDC, could just be removed
	{
		f += MB_LIST_ROW;
		p = Draw_CachePic ("gfx/netmen5.lmp");
		M_BoxPic (MB_LIST_X, f, p, 1);
	}

// what the highlighted one is, in a box under the list
	f = 37 + m_net_items * MB_LIST_ROW;
	M_DrawTextBox (56, f, 24, 4);
	M_Print (64, f + 8, net_helpMessage[m_net_cursor*4+0]);
	M_Print (64, f + 16, net_helpMessage[m_net_cursor*4+1]);
	M_Print (64, f + 24, net_helpMessage[m_net_cursor*4+2]);
	M_Print (64, f + 32, net_helpMessage[m_net_cursor*4+3]);

	M_DrawDot (m_net_cursor);
	M_DrawFooter ("Backspace: Back", "Enter: Select");
}


void M_Net_Key (int k)
{
again:
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_MultiPlayer_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_net_cursor >= m_net_items)
			m_net_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_net_cursor < 0)
			m_net_cursor = m_net_items - 1;
		break;

	case K_ENTER:
		m_entersound = true;

		switch (m_net_cursor)
		{
		case 0:
			M_Menu_SerialConfig_f ();
			break;

		case 1:
			M_Menu_SerialConfig_f ();
			break;

		case 2:
			M_Menu_LanConfig_f ();
			break;

		case 3:
			M_Menu_LanConfig_f ();
			break;

		case 4:
// multiprotocol
			break;
		}
	}

	if (m_net_cursor == 0 && !serialAvailable)
		goto again;
	if (m_net_cursor == 1 && !serialAvailable)
		goto again;
	if (m_net_cursor == 2 && !ipxAvailable)
		goto again;
	if (m_net_cursor == 3 && !tcpipAvailable)
		goto again;
}

//=============================================================================
/* OPTIONS MENU */
//
// One table, rather than three switch statements that had to agree.
//
// id's options menu drew each row in one switch, adjusted it in a second and
// acted on Enter in a third, with the row's identity being its position in all
// three. Adding a setting meant editing three places and getting the numbering
// right in each, and every setting id added after 1996 went to the console
// instead. This port had added several of its own -- freelook, the sound
// delay, the field of view that widescreen made worth changing -- and they
// were all console-only for the same reason.
//
// A row now says what it is and which cvar it moves. The drawing and the
// adjusting are written once against that, and adding a setting is one line.
//
typedef enum
{
	o_action,		// Enter does something; there is no value to show
	o_slider,		// a number between min and max
	o_toggle,		// a cvar that is off or on
	o_custom,		// the handful that are not simply a cvar
	o_heading		// not a setting: the name of the group below it
} otype_t;

// o_action and o_custom rows, by name rather than by row number.
#define	OPT_KEYS		0
#define	OPT_VIDEO		3
#define	OPT_VIEWSIZE	4
#define	OPT_GAMMA		5
#define	OPT_ALWAYSRUN	6
#define	OPT_INVERT		7
#define	OPT_BOB			8
#define	OPT_KICK		9
#define	OPT_DETAIL		10
#define	OPT_PADNAME		12
#define	OPT_CROSSHAIR	13
#define	OPT_HUD			14
#define	OPT_MAXFPS		15
#define	OPT_WEAPONPICKUP	16

typedef struct
{
	char	*label;
	otype_t	type;
	char	*cvar;			// what a slider or a toggle moves
	float	min, max, step;
	int		id;				// which action, or which custom row
} option_t;


/*
================
M_Game_Dir

The game directory being played. com_gamedir is the last one
COM_AddGameDirectory was given, so its final component is the mission pack or
the mod if there is one, and id1 if there is not.
================
*/
static char *M_Game_Dir (void)
{
	char	*p = strrchr (com_gamedir, '/');

	return p ? p + 1 : com_gamedir;
}

// Six of these were not archived cvars, because id never offered them
// anywhere but the console and a console setting was not expected to last.
// A row in a menu is: set it, and it is still set tomorrow. fov,
// r_drawviewmodel, cl_bob, v_kicktime, r_waterwarp and d_mipcap are archived
// now, which is what puts them in config.cfg on the way out.
//
// Grouped into pages the way the re-release groups its own: a list of them in
// big lettering, and on each page, headings over the rows that belong
// together. Every page starts with a heading naming it.
//
static option_t	opt_controls[] =
{
	{"Controls",				o_heading, NULL,            0,     0,    0,    0},
	{"Customize Controls...",	o_action, NULL,             0,     0,    0,    OPT_KEYS},

	{"Mouse",					o_heading, NULL,            0,     0,    0,    0},
	{"Mouse Speed",				o_slider, "sensitivity",    1,     11,   0.5,  0},
	{"Mouse Look",				o_toggle, "freelook",       0,     0,    0,    0},
	{"Invert Mouse",			o_custom, NULL,             0,     0,    0,    OPT_INVERT},
	{"Smooth Mouse",			o_toggle, "m_filter",       0,     0,    0,    0},
	{"Lookspring",				o_toggle, "lookspring",     0,     0,    0,    0},
	{"Lookstrafe",				o_toggle, "lookstrafe",     0,     0,    0,    0},

// A controller's own settings (in_pad.c); its buttons are bound under Customize
// Controls with everything else.
	{"Controller",				o_heading, NULL,            0,     0,    0,    0},
	{"Connected",				o_custom, NULL,             0,     0,    0,    OPT_PADNAME},
	{"Turn Speed",				o_slider, "joy_lookspeed",  60,    400,  20,   0},
	{"Look Up/Down Speed",		o_slider, "joy_lookspeed_y",40,    400,  20,   0},
// the exponent on how far the stick is pushed: 1 is straight, higher aims
// more finely near the middle
	{"Look Curve",				o_slider, "joy_lookcurve",  1,     4,    0.25, 0},
	{"Invert Look",				o_toggle, "joy_invert",     0,     0,    0,    0},
	{"Move Deadzone",			o_slider, "joy_deadzone",   0.04,  0.5,  0.02, 0},
	{"Look Deadzone",			o_slider, "joy_deadzone_look",0.04, 0.5, 0.02, 0},
	{"Vibration",				o_toggle, "joy_rumble",     0,     0,    0,    0},
	{"Vibration Intensity",		o_slider, "joy_rumble_intensity",0, 10,   1,    0},
	{"Swap Sticks",				o_toggle, "joy_swapsticks", 0,     0,    0,    0},
	{"Full Push Runs",			o_toggle, "joy_pushrun",    0,     0,    0,    0},
};

static option_t	opt_gameplay[] =
{
	{"Gameplay",				o_heading, NULL,            0,     0,    0,    0},
	{"Always Run",				o_custom, NULL,             0,     0,    0,    OPT_ALWAYSRUN},
	{"View Bob",				o_custom, NULL,             0,     0,    0,    OPT_BOB},
	{"View Kick",				o_custom, NULL,             0,     0,    0,    OPT_KICK},
	{"HUD Style",				o_custom, NULL,             0,     0,    0,    OPT_HUD},
	{"Show Weapon",				o_toggle, "r_drawviewmodel",0,     0,    0,    0},
// Asked by the re-release's progs (pr_cmds.c); a 1996 progs always switches.
	{"Change Weapon on Pickup",	o_custom, NULL,             0,     0,    0,    OPT_WEAPONPICKUP},
	{"Toggle Scoreboard",		o_toggle, "cl_togglescores",0,     0,    0,    0},
	{"Classic Quit Prompt",		o_toggle, "m_classicquit",  0,     0,    0,    0},

	{"Crosshair",				o_heading, NULL,            0,     0,    0,    0},
	{"Crosshair Style",			o_custom, NULL,             0,     0,    0,    OPT_CROSSHAIR},
	{"Red",						o_slider, "crosshair_r",    0,     255,  15,   0},
	{"Green",					o_slider, "crosshair_g",    0,     255,  15,   0},
	{"Blue",					o_slider, "crosshair_b",    0,     255,  15,   0},
};

static option_t	opt_sound[] =
{
	{"Sound",					o_heading, NULL,            0,     0,    0,    0},
	{"Sound Volume",			o_slider, "volume",         0,     1,    0.1,  0},
	{"Music Volume",			o_slider, "bgmvolume",      0,     1,    0.1,  0},
	{"Sound Delay",				o_slider, "_snd_mixahead",  0.04,  0.2,  0.02, 0},
};

static option_t	opt_display[] =
{
	{"Display",					o_heading, NULL,            0,     0,    0,    0},
	{"Video Modes...",			o_action, NULL,             0,     0,    0,    OPT_VIDEO},
// A desktop's; there is no such thing in the container, whose window is the
// screen, so it reads n/a there (vid_x.c registers it only on a desktop).
	{"Fullscreen",				o_toggle, "vid_fullscreen", 0,     0,    0,    0},
	{"Screen Size",				o_custom, NULL,             0,     0,    0,    OPT_VIEWSIZE},
	{"Brightness",				o_custom, NULL,             0,     0,    0,    OPT_GAMMA},
	{"Field of View",			o_slider, "fov",            75,    130,  5,    0},
// above 72 only the drawing goes faster; the game still runs at 72 (host.c)
	{"Max FPS",					o_custom, NULL,             0,     0,    0,    OPT_MAXFPS},

	{"Enhancements",			o_heading, NULL,            0,     0,    0,    0},
	{"Texture Detail",			o_custom, NULL,             0,     0,    0,    OPT_DETAIL},
	{"Water Warp",				o_toggle, "r_waterwarp",    0,     0,    0,    0},
	{"Model Interpolation",		o_toggle, "r_lerpmodels",   0,     0,    0,    0},
// the re-release's own, where its id1/pak0.pak has them (model_md5.c)
	{"Enhanced Models",			o_toggle, "r_enhancedmodels",0,    0,    0,    0},

// Only the re-release maps set fog at all, and how thick it should look is a
// judgement rather than a fact: the density they set is interpreted through a
// curve taken from another engine's source. 0 turns it off, 1 is that curve.
	{"Fog Thickness",			o_slider, "r_fogscale",     0,     4,    0.25, 0},

// Coloured light, where a map has it (r_tint.c). r_rgblight is a strength, and
// the console can set it anywhere between; the menu offers on and off.
	{"Coloured Light",			o_toggle, "r_rgblight",     0,     0,    0,    0},
};

typedef struct
{
	option_t	*rows;
	int			count;
	int			cursor;		// kept, so a page opens where it was left
} optpage_t;

#define	OPTPAGE(rows)	{rows, sizeof(rows) / sizeof(rows[0]), 0}

static optpage_t	optpages[] =
{
	OPTPAGE(opt_controls),
	OPTPAGE(opt_gameplay),
	OPTPAGE(opt_sound),
	OPTPAGE(opt_display),
};

#define	NUM_OPTPAGES	((int)(sizeof(optpages) / sizeof(optpages[0])))

// The first level: the pages, then the things that are not settings.
static char	*optcats[] =
{
	"Controls",
	"Gameplay",
	"Sound",
	"Display",
	"Game / Mod",
	"Console",
	"Reset Defaults",
};

#define	OPTCATS			((int)(sizeof(optcats) / sizeof(optcats[0])))
#define	OPTCAT_GAME		4
#define	OPTCAT_CONSOLE	5
#define	OPTCAT_RESET	6

// A page, measured off the re-release: rows 16 apart, a heading and its rule
// taking 20, and a blank row before every heading but the first.
#define	PAGE_TOP		69		// box y of the first row
#define	PAGE_ROW		16
#define	PAGE_HEAD		20
#define	PAGE_GAP		16
#define	PAGE_BOTTOM		35		// rows stop this far above the box's bottom
#define	PAGE_SLIDER		160		// a slider's width

int				options_cursor;		// on the first level
static int		optpage;			// the page open, when m_state is m_optpage
static int		optpage_scroll;		// how far down the page is scrolled
static int		opt_resume = -1;	// the page a submenu goes back to

// Texture detail is d_mipcap, which is how blurry the far end of a wall may
// get. Named rather than numbered, because "2" says nothing.
static char	*options_detail[] = { "Sharp", "Softer", "Soft", "Softest" };
static int	options_maxfps[] = { 60, 72, 100, 120, 144, 165, 200, 240, 300 };
#define	NUM_MAXFPS	((int)(sizeof(options_maxfps)/sizeof(options_maxfps[0])))
static char	*options_crosshair[] =
	{ "Off", "Classic", "Cross", "Dot", "Circle", "Gap Cross", "Circle Dot" };
#define	NUM_CROSSHAIRS	((int)(sizeof(options_crosshair)/sizeof(options_crosshair[0])))

static void M_OptPage_Open (int page)
{
	optpage_t	*pg = &optpages[page];

	key_dest = key_menu;
	m_state = m_optpage;
	m_entersound = true;
	optpage = page;
	optpage_scroll = 0;
	while (pg->cursor < pg->count && pg->rows[pg->cursor].type == o_heading)
		pg->cursor++;
}

void M_Menu_Options_f (void)
{
// back from the controls or the video modes, to the page they were opened from
	if (opt_resume >= 0)
	{
		int	page = opt_resume;

		opt_resume = -1;
		M_OptPage_Open (page);
		return;
	}

	key_dest = key_menu;
	m_state = m_options;
	m_entersound = true;
}


/*
================
M_Options_Live

Whether the setting behind a row exists at all.

-nosound makes S_Init return before it registers volume, bgmvolume and
_snd_mixahead, so on a run with the sound off those three rows move a cvar
that is not there -- and Cvar_Set answers a name it cannot find by printing
"variable volume not found", once per press of an arrow key. id's menu did
that too. A row with nothing behind it says so and does nothing instead.
================
*/
static qboolean M_Options_Live (option_t *o)
{
	if (o->type == o_action || o->type == o_custom || o->type == o_heading)
		return true;

	return Cvar_FindVar (o->cvar) != NULL;
}


/*
================
M_Options_Value

What a row currently reads, as a number. Sliders and toggles are their cvar;
the custom rows are whatever id decided they were.
================
*/
static float M_Options_Value (option_t *o)
{
	switch (o->type)
	{
	case o_slider:
	case o_toggle:
		return Cvar_VariableValue (o->cvar);

	case o_custom:
		switch (o->id)
		{
		case OPT_VIEWSIZE:	return scr_viewsize.value;
		case OPT_GAMMA:		return v_gamma.value;
		case OPT_ALWAYSRUN:	return cl_forwardspeed.value > 200;
		case OPT_INVERT:	return m_pitch.value < 0;
		case OPT_BOB:		return Cvar_VariableValue ("cl_bob") != 0;
		case OPT_KICK:		return Cvar_VariableValue ("v_kicktime") != 0;
		case OPT_DETAIL:	return Cvar_VariableValue ("d_mipcap");
		case OPT_CROSSHAIR:	return Cvar_VariableValue ("crosshair");
		case OPT_HUD:		return Cvar_VariableValue ("hud_style");
		case OPT_MAXFPS:	return Cvar_VariableValue ("host_maxfps");
		case OPT_WEAPONPICKUP:	return Cvar_VariableValue ("cl_weaponpickup");
		}
		break;

	default:
		break;
	}

	return 0;
}


/*
================
M_AdjustSliders

dir is -1 for left and 1 for right. Enter comes through here as 1 as well, so
a toggle answers Enter and the arrows the same way.
================
*/
void M_AdjustSliders (int dir)
{
	option_t	*o = &optpages[optpage].rows[optpages[optpage].cursor];
	float		v;

	if (!M_Options_Live (o))
		return;

	S_LocalSound ("misc/menu3.wav");

	switch (o->type)
	{
	case o_action:
	case o_heading:
		return;

	case o_slider:
		v = Cvar_VariableValue (o->cvar) + dir * o->step;
		if (v < o->min)
			v = o->min;
		if (v > o->max)
			v = o->max;
		Cvar_SetValue (o->cvar, v);
		return;

	case o_toggle:
		Cvar_SetValue (o->cvar, !Cvar_VariableValue (o->cvar));
		return;

	case o_custom:
		break;
	}

	switch (o->id)
	{
	case OPT_VIEWSIZE:
		v = scr_viewsize.value + dir * 10;
		if (v < 30)
			v = 30;
		if (v > 120)
			v = 120;
		Cvar_SetValue ("viewsize", v);
		break;

	case OPT_GAMMA:
	// Backwards on purpose: right is brighter, and a smaller gamma is
	// brighter.
		v = v_gamma.value - dir * 0.05;
		if (v < 0.5)
			v = 0.5;
		if (v > 1)
			v = 1;
		Cvar_SetValue ("gamma", v);
		break;

	case OPT_ALWAYSRUN:
		if (cl_forwardspeed.value > 200)
		{
			Cvar_SetValue ("cl_forwardspeed", 200);
			Cvar_SetValue ("cl_backspeed", 200);
		}
		else
		{
			Cvar_SetValue ("cl_forwardspeed", 400);
			Cvar_SetValue ("cl_backspeed", 400);
		}
		break;

	case OPT_INVERT:
		Cvar_SetValue ("m_pitch", -m_pitch.value);
		break;

	case OPT_BOB:
	// id's default, and off. The cvar is a distance, not a flag.
		Cvar_SetValue ("cl_bob", Cvar_VariableValue ("cl_bob") ? 0 : 0.02);
		break;

	case OPT_KICK:
	// How long the view is thrown by a hit. Zero is no throw at all.
		Cvar_SetValue ("v_kicktime", Cvar_VariableValue ("v_kicktime") ? 0 : 0.5);
		break;

	case OPT_HUD:
		Cvar_SetValue ("hud_style", Cvar_VariableValue ("hud_style") ? 0 : 1);
		break;

	case OPT_WEAPONPICKUP:
		v = (int)Cvar_VariableValue ("cl_weaponpickup") + dir;
		if (v < 0)
			v = 2;
		if (v > 2)
			v = 0;
		Cvar_SetValue ("cl_weaponpickup", v);
		break;

	case OPT_MAXFPS:
		{
			int		i, cur = (int)Cvar_VariableValue ("host_maxfps");

		// the step after the nearest one at or below what it is set to
			for (i = 0 ; i < NUM_MAXFPS - 1 && options_maxfps[i + 1] <= cur ; i++)
				;
			i += dir;
			if (i < 0)
				i = NUM_MAXFPS - 1;
			if (i >= NUM_MAXFPS)
				i = 0;
			Cvar_SetValue ("host_maxfps", options_maxfps[i]);
		}
		break;

	case OPT_CROSSHAIR:
	// round the styles, off included, either way (view.c draws them)
		v = (int)Cvar_VariableValue ("crosshair") + dir;
		if (v < 0)
			v = NUM_CROSSHAIRS - 1;
		if (v >= NUM_CROSSHAIRS)
			v = 0;
		Cvar_SetValue ("crosshair", v);
		break;

	case OPT_DETAIL:
		v = Cvar_VariableValue ("d_mipcap") + dir;
		if (v < 0)
			v = 0;
		if (v > 3)
			v = 3;
		Cvar_SetValue ("d_mipcap", v);
		break;
	}
}


/*
================
M_Options_Number

A slider says how far along it is, not what it is set to, and for a field of
view or a mouse speed the number is the thing somebody actually wants. Printed
to the right of the bar with as few decimals as the value needs: 90 rather than
90.00, 0.06 rather than 0.060000.
================
*/
static char *M_Options_Number (float v)
{
	static char	buf[16];
	int			i;

	snprintf (buf, sizeof(buf), "%.2f", v);

	i = strlen (buf) - 1;
	while (i > 0 && buf[i] == '0')
		buf[i--] = 0;
	if (i > 0 && buf[i] == '.')
		buf[i] = 0;

	return buf;
}


/*
================
M_DrawScrollbar

Down the right of the box, beside a list of `total` units of which `view`
show, from `scroll`. Nothing when it all fits.
================
*/
static void M_DrawScrollbar (int top, int height, int total, int view,
	int scroll)
{
	int		th, ty;

	if (total <= view)
		return;

	th = height * view / total;
	if (th < 8)
		th = 8;
	ty = top + (height - th) * scroll / (total - view);
	M_BoxFill (m_bw - 9, top, 3, height, m_col_track);
	M_BoxFill (m_bw - 9, ty, 3, th, m_col_thumb);
}

// id's slider characters, stretched to the re-release's width, ending at right
static void M_PageSlider (int right, int y, float range, char *number)
{
	int		x = right - PAGE_SLIDER, i;

	if (range < 0)
		range = 0;
	if (range > 1)
		range = 1;
	Draw_CharacterEx (m_bx + x, m_by + y, 128, 1, NULL);
	for (i = 8 ; i < PAGE_SLIDER - 8 ; i += 8)
		Draw_CharacterEx (m_bx + x + i, m_by + y, 129, 1, NULL);
	Draw_CharacterEx (m_bx + x + i, m_by + y, 130, 1, NULL);
	Draw_CharacterEx (m_bx + x + 8 + (int)((PAGE_SLIDER - 24) * range),
		m_by + y, 131, 1, NULL);
	M_BoxTextRight (x - 8, y, number, NULL);
}

// a value picked from a list, on a dark plate as the re-release has them
static void M_PageChoice (int right, int y, char *text)
{
	int		w = 8 * strlen (text);

	M_BoxFill (right - w - 3, y - 2, w + 6, 12, m_col_box);
	M_BoxTextRight (right, y, text, NULL);
}

static void M_PageToggle (int right, int y, qboolean on)
{
	M_BoxTextRight (right, y, on ? "On" : "Off", NULL);
}

/*
================
M_OptPage_DrawValue
================
*/
static void M_OptPage_DrawValue (int right, int y, option_t *o)
{
	float	v;

	if (!M_Options_Live (o))
	{
		M_BoxTextRight (right, y, "n/a", m_tint_dim);
		return;
	}

	v = M_Options_Value (o);

	switch (o->type)
	{
	case o_action:
	case o_heading:
		return;

	case o_slider:
		M_PageSlider (right, y, (v - o->min) / (o->max - o->min),
			M_Options_Number (v));
		return;

	case o_toggle:
		M_PageToggle (right, y, v != 0);
		return;

	case o_custom:
		break;
	}

	switch (o->id)
	{
	case OPT_VIEWSIZE:
		M_PageSlider (right, y, (v - 30) / (120 - 30), M_Options_Number (v));
		break;

	case OPT_GAMMA:
		M_PageSlider (right, y, (1.0 - v) / 0.5, M_Options_Number (v));
		break;

	case OPT_DETAIL:
		M_PageChoice (right, y,
			options_detail[(int)v < 0 ? 0 : ((int)v > 3 ? 3 : (int)v)]);
		break;

	case OPT_HUD:
		M_PageChoice (right, y, v ? "Minimal" : "Classic");
		break;

	case OPT_MAXFPS:
		M_PageChoice (right, y, va ("%d", (int)v));
		break;

	case OPT_WEAPONPICKUP:
		M_PageChoice (right, y, (int)v == 1 ? "Only If New"
			: ((int)v == 2 ? "Never" : "Always"));
		break;

	case OPT_CROSSHAIR:
		M_PageChoice (right, y, options_crosshair[(int)v < 0 ? 0
			: ((int)v >= NUM_CROSSHAIRS ? 1 : (int)v)]);
		break;

	case OPT_PADNAME:
		{
			char	name[32], *n = PAD_Name ();

			Q_strncpy (name, n ? n : "None", sizeof(name) - 1);
			name[sizeof(name) - 1] = 0;
			M_BoxTextRight (right, y, name, n ? NULL : m_tint_dim);
		}
		break;

	default:
		M_PageToggle (right, y, v != 0);
		break;
	}
}

/*
================
M_PageY

Where row i of a page is, from the top of the page.
================
*/
static int M_PageY (optpage_t *pg, int i)
{
	int		k, y;

	y = 0;
	for (k = 0 ; k <= i && k < pg->count ; k++)
	{
		if (pg->rows[k].type == o_heading && k > 0)
			y += PAGE_GAP;
		if (k == i)
			break;
		y += pg->rows[k].type == o_heading ? PAGE_HEAD : PAGE_ROW;
	}
	return y;
}

void M_OptPage_Draw (void)
{
	optpage_t	*pg = &optpages[optpage];
	option_t	*o;
	int			i, y, top, total, view, right;

	M_DrawFrame ("gfx/p_option.lmp");

	view = m_bh - PAGE_BOTTOM - PAGE_TOP;
	total = M_PageY (pg, pg->count - 1) + PAGE_ROW;

// Keep the cursor on the page, and the heading over it too when it is the
// first of a group. Done here rather than in the key handler so that it is
// also right the first time the page is opened.
	y = M_PageY (pg, pg->cursor);
	top = y;
	if (pg->cursor > 0 && pg->rows[pg->cursor - 1].type == o_heading)
		top = M_PageY (pg, pg->cursor - 1);
	if (pg->cursor == 1)
		top = 0;
	if (top < optpage_scroll)
		optpage_scroll = top;
	if (y + 8 > optpage_scroll + view)
		optpage_scroll = y + 8 - view;
	if (optpage_scroll > total - view)
		optpage_scroll = total - view;
	if (optpage_scroll < 0)
		optpage_scroll = 0;

	right = m_bw - (total > view ? 19 : 12);

	for (i = 0 ; i < pg->count ; i++)
	{
		o = &pg->rows[i];
		y = M_PageY (pg, i) - optpage_scroll;
		if (y < 0 || y + (o->type == o_heading ? 13 : 8) > view)
			continue;
		y += PAGE_TOP;

		if (o->type == o_heading)
		{
			M_BoxText (MB_LABEL_X, y, o->label, m_tint_dim);
			M_BoxFill (MB_LABEL_X, y + 12, m_bw - 5 - MB_LABEL_X, 1,
				m_col_rule);
			continue;
		}

		M_BoxText (MB_LABEL_X, y, o->label,
			M_Options_Live (o) ? NULL : m_tint_dim);
		M_OptPage_DrawValue (right, y, o);

		if (i == pg->cursor)
			M_BoxText (MB_ARROW_X, y, "\015", m_tint_orange);
	}

	M_DrawScrollbar (PAGE_TOP - 4, view + 4, total, view, optpage_scroll);
	M_DrawFooter ("Backspace: Back", "Enter: Select");
}


void M_OptPage_Key (int k)
{
	optpage_t	*pg = &optpages[optpage];
	int			i;

	switch (k)
	{
	case K_ESCAPE:
		options_cursor = optpage;
		M_Menu_Options_f ();
		break;

	case K_ENTER:
		m_entersound = true;

		if (pg->rows[pg->cursor].type != o_action)
		{
			M_AdjustSliders (1);
			return;
		}

		switch (pg->rows[pg->cursor].id)
		{
		case OPT_KEYS:
			opt_resume = optpage;
			M_Menu_Keys_f ();
			break;

		case OPT_VIDEO:
		// The old menu hid this row when the video driver claimed no menu.
		// This one shows it and does nothing, which is a worse answer, so
		// say why instead. Every driver this builds against sets it.
			if (vid_menudrawfn)
			{
				opt_resume = optpage;
				M_Menu_Video_f ();
			}
			else
				Con_Printf ("This video driver has no mode menu.\n");
			break;
		}
		return;

	case K_UPARROW:
	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		i = pg->cursor;
		do
		{
			i += k == K_UPARROW ? -1 : 1;
			if (i < 0)
				i = pg->count - 1;
			if (i >= pg->count)
				i = 0;
		} while (pg->rows[i].type == o_heading);
		pg->cursor = i;
		break;

	case K_LEFTARROW:
		M_AdjustSliders (-1);
		break;

	case K_RIGHTARROW:
		M_AdjustSliders (1);
		break;
	}
}


/*
================
M_Options_Draw

The first level: the pages, in big lettering.
================
*/
void M_Options_Draw (void)
{
	int		i;

	M_DrawFrame ("gfx/p_option.lmp");
	for (i = 0 ; i < OPTCATS ; i++)
		M_BoxBigText (MB_LIST_X, MB_LIST_Y + i * MB_LIST_ROW + 2, optcats[i],
			false);
	M_DrawDot (options_cursor);
	M_DrawFooter ("Backspace: Back", "Enter: Select");
}


void M_Options_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_ENTER:
		m_entersound = true;

		if (options_cursor < NUM_OPTPAGES)
		{
			M_OptPage_Open (options_cursor);
			return;
		}

		switch (options_cursor)
		{
		case OPTCAT_GAME:
			M_Menu_Game_f ();
			break;

		case OPTCAT_CONSOLE:
			m_state = m_none;
			Con_ToggleConsole_f ();
			break;

		case OPTCAT_RESET:
		// default.cfg starts with unbindall, which takes the controller's
		// buttons with it; joy_bound 0 has in_pad.c bind them again.
			Cbuf_AddText ("exec default.cfg\njoy_bound 0\n");
			break;
		}
		return;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		options_cursor--;
		if (options_cursor < 0)
			options_cursor = OPTCATS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		options_cursor++;
		if (options_cursor >= OPTCATS)
			options_cursor = 0;
		break;
	}
}

//=============================================================================
/* KEYS MENU */

//
// Everything the menu can rebind.
//
// The 1996 list stopped at eighteen because eighteen rows is all that fits
// between the title and the bottom of a 320x200 screen, so the weapon keys,
// the console and the scoreboard could only be bound by typing `bind` at the
// console. The menu scrolls now -- see M_Keys_Draw -- so the limit is gone and
// the list is everything a player actually reaches for.
//
// Order is by how often it is wanted, not by category: the movement and
// shooting keys are what somebody opening this menu came for, and they should
// not have to scroll to reach them.
//
char *bindnames[][2] =
{
{"+attack", 		"Attack"},
{"+forward", 		"Move Forward"},
{"+back", 			"Move Back"},
{"+moveleft", 		"Move Left"},
{"+moveright", 		"Move Right"},
{"+jump", 			"Jump / Swim Up"},
{"+speed", 			"Run"},
{"impulse 10", 		"Next Weapon"},
{"impulse 12", 		"Previous Weapon"},
{"+mlook", 			"Mouse Look"},
{"+strafe", 		"Sidestep Modifier"},
{"+moveup",			"Swim Up"},
{"+movedown",		"Swim Down"},
{"impulse 1", 		"Axe"},
{"impulse 2", 		"Shotgun"},
{"impulse 3", 		"Super Shotgun"},
{"impulse 4", 		"Nailgun"},
{"impulse 5", 		"Super Nailgun"},
{"impulse 6", 		"Grenade Launcher"},
{"impulse 7", 		"Rocket Launcher"},
{"impulse 8", 		"Thunderbolt"},
{"+showscores", 	"Show Scores"},
{"echo Quicksaving...; wait; save quick", "Quick Save"},
{"echo Quickloading...; wait; load quick", "Quick Load"},
{"messagemode", 	"Chat"},
{"toggleconsole", 	"Console"},
{"screenshot", 		"Screenshot"},
{"pause", 			"Pause"},
{"+left", 			"Turn Left"},
{"+right", 			"Turn Right"},
{"+lookup", 		"Look Up"},
{"+lookdown", 		"Look Down"},
{"centerview", 		"Center View"},
{"+klook", 			"Keyboard Look"}
};

#define	NUMCOMMANDS	(sizeof(bindnames)/sizeof(bindnames[0]))

// The rows are a page's (see PAGE_TOP), and the keys are in a column a little
// right of the middle, where the re-release has them.
#define	KEYS_COLUMN(w)	((w) * 306 / MBOX_W)

// The first row on screen. Scrolled by M_Keys_Draw to keep the cursor visible.
int		keys_top;

int		keys_cursor;
int		bind_grab;

// Whether the controls screen is waiting for a key to bind, when a controller's
// buttons have to arrive as themselves rather than as menu keys (in_pad.c).
qboolean M_KeysGrabbing (void)
{
	return m_state == m_keys && bind_grab;
}

void M_Menu_Keys_f (void)
{
	key_dest = key_menu;
	m_state = m_keys;
	m_entersound = true;
}


//
// The keys bound to a command. id compared only as many characters as the
// command has, so "impulse 1" -- the axe -- also found the keys for impulse 10
// and 12, next and previous weapon: the axe's row showed the next-weapon key,
// and clearing the axe cleared weapon switching with it. A binding is stored
// exactly as it was made, so it is compared exactly.
//
// Three, not id's two: a keyboard key, a spare, and a controller's button.
//
#define	MAX_KEYS_SHOWN	3

void M_FindKeysForCommand (char *command, int *keys)
{
	int		count;
	int		j;

	for (j = 0 ; j < MAX_KEYS_SHOWN ; j++)
		keys[j] = -1;
	count = 0;

	for (j=0 ; j<256 && count < MAX_KEYS_SHOWN ; j++)
		if (keybindings[j] && !strcmp (keybindings[j], command))
			keys[count++] = j;
}

void M_UnbindCommand (char *command)
{
	int		j;

	for (j=0 ; j<256 ; j++)
		if (keybindings[j] && !strcmp (keybindings[j], command))
			Key_SetBinding (j, "");
}

//
// A key as the controls screen shows it. A controller's buttons are PAD_A and
// so on in config.cfg, and named as the pad labels them here.
//
static char *M_KeyName (int k)
{
	static char	*pad[] =
	{
		"Pad A", "Pad B", "Pad X", "Pad Y", "Pad LB", "Pad RB", "Pad LT",
		"Pad RT", "Pad View", "Pad Menu", "Pad LS", "Pad RS", "Pad Up",
		"Pad Down", "Pad Left", "Pad Right", "Pad Guide"
	};

	if (k >= K_AUX1 && k < K_AUX1 + (int)(sizeof(pad) / sizeof(pad[0])))
		return pad[k - K_AUX1];
	return Key_KeynumToString (k);
}

void M_Keys_Draw (void)
{
	int		i, j, row, y, visible, x, cx;
	int		keys[MAX_KEYS_SHOWN];
	char	*name;

	M_DrawFrame ("gfx/ttl_cstm.lmp");

	visible = (m_bh - PAGE_BOTTOM - PAGE_TOP - 8) / PAGE_ROW + 1;

// Keep the cursor inside the window. Done here rather than in M_Keys_Key so
// that it is also right the first time the menu is opened, and after the list
// itself changes.
	if (keys_cursor < keys_top)
		keys_top = keys_cursor;
	if (keys_cursor >= keys_top + visible)
		keys_top = keys_cursor - visible + 1;
	if (keys_top > (int)NUMCOMMANDS - visible)
		keys_top = (int)NUMCOMMANDS - visible;
	if (keys_top < 0)
		keys_top = 0;

	x = KEYS_COLUMN (m_bw);

	for (row = 0 ; row < visible ; row++)
	{
		i = keys_top + row;
		if (i >= (int)NUMCOMMANDS)
			break;

		y = PAGE_TOP + PAGE_ROW * row;

		M_BoxText (MB_LABEL_X, y, bindnames[i][1], NULL);

		if (i == keys_cursor)
			M_BoxText (MB_ARROW_X, y, "\015", m_tint_orange);

		if (bind_grab && i == keys_cursor)
		{
			M_BoxText (x, y, "Press a key", m_tint_orange);
			continue;
		}

		M_FindKeysForCommand (bindnames[i][0], keys);

		if (keys[0] == -1)
		{
			M_BoxText (x, y, "???", m_tint_dim);
			continue;
		}

		for (j = 0, cx = x ; j < MAX_KEYS_SHOWN && keys[j] != -1 ; j++)
		{
			name = va ("%s%s", j ? ", " : "", M_KeyName (keys[j]));
			if (cx + 8 * (int)strlen (name) > m_bw - 20)
				break;			// no room for another; the rest are still bound
			M_BoxText (cx, y, name, NULL);
			cx += 8 * strlen (name);
		}
	}

	M_DrawScrollbar (PAGE_TOP - 4, visible * PAGE_ROW, (int)NUMCOMMANDS
		* PAGE_ROW, visible * PAGE_ROW, keys_top * PAGE_ROW);

	if (bind_grab)
		M_DrawFooter ("Escape: Cancel", NULL);
	else
		M_DrawFooter ("Backspace: Back   Del: Unbind", "Enter: Change");
}


void M_Keys_Key (int k)
{
	char	cmd[80];
	int		keys[MAX_KEYS_SHOWN];

	if (bind_grab)
	{	// defining a key
		S_LocalSound ("misc/menu1.wav");
		if (k == K_ESCAPE)
		{
			bind_grab = false;
		}
		else if (k != '`')
		{
			sprintf (cmd, "bind \"%s\" \"%s\"\n", Key_KeynumToString (k), bindnames[keys_cursor][0]);
			Cbuf_InsertText (cmd);
		}

		bind_grab = false;
		return;
	}

	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;

	case K_LEFTARROW:
	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		keys_cursor--;
		if (keys_cursor < 0)
			keys_cursor = NUMCOMMANDS-1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		keys_cursor++;
		if (keys_cursor >= NUMCOMMANDS)
			keys_cursor = 0;
		break;

	case K_ENTER:		// go into bind mode
		M_FindKeysForCommand (bindnames[keys_cursor][0], keys);
		S_LocalSound ("misc/menu2.wav");
		if (keys[MAX_KEYS_SHOWN-1] != -1)
			M_UnbindCommand (bindnames[keys_cursor][0]);
		bind_grab = true;
		break;

	case K_DEL:				// delete bindings; Backspace goes back (M_Keydown)
		S_LocalSound ("misc/menu2.wav");
		M_UnbindCommand (bindnames[keys_cursor][0]);
		break;
	}
}

//=============================================================================
/* GAME MENU */

//
// Mission packs and mods.
//
// The search path is built once, in COM_InitFilesystem, and rebuilding it
// underneath a running game would mean throwing away every model, sound,
// texture and progs the hunk holds and loading them again -- which is most of
// what starting over does anyway, with none of the certainty. So this menu
// does not switch anything. It stores the choice and quits, and the next run
// comes up on it.
//
// In the container that is close to invisible: the engine is run in a restart
// loop and the page reconnects by itself, so the screen goes dark for a few
// seconds and comes back on the new game. Started by hand it simply quits, and
// the choice applies the next time.
//

// Whether this is the full game. common.h is included before cvar.h, so the
// declaration cannot live beside com_basedir where it belongs.
extern cvar_t	registered;

#define	MAX_GAMEDIRS	32


static char	gamedirs[MAX_GAMEDIRS][MAX_QPATH];
static int	numgamedirs;
static int	game_cursor;
static int	game_top;
static int	game_current;		// the one being played, -1 if it is not in the list
static int	game_chosen;		// what Enter picked; -1 while the list is up
static qboolean	game_denied;	// Enter on something the shareware data cannot run

//
// A directory name is not what the thing is called.
//
// These are the ones somebody is likely to have. Anything else is a mod, and a
// mod is known by its directory anyway -- there is no manifest in a Quake mod
// to read a name out of, so inventing one would mean guessing.
//
static char	*gametitles[][2] =
{
	{"id1",			"Quake"},
	{"hipnotic",	"Scourge of Armagon"},
	{"rogue",		"Dissolution of Eternity"},
	{"dopa",		"Dimension of the Past"},
	{"mg1",			"Dimension of the Machine"},
};

static char *M_Game_Title (char *dir)
{
	int		i;

	for (i = 0 ; i < (int)(sizeof(gametitles) / sizeof(gametitles[0])) ; i++)
		if (!Q_strcmp (dir, gametitles[i][0]))
			return gametitles[i][1];

	return dir;
}


void M_Menu_Game_f (void)
{
	char	*playing;
	int		i;

	key_dest = key_menu;
	m_state = m_game;
	m_entersound = true;

	game_chosen = -1;
	game_denied = false;

// Read every time the menu opens rather than once at startup: a mod mounted
// while the container was running is there the next time somebody looks.
	numgamedirs = Sys_ListGameDirs (com_basedir, gamedirs, MAX_GAMEDIRS);

	playing = M_Game_Dir ();
	game_current = -1;
	for (i = 0 ; i < numgamedirs ; i++)
		if (!Q_strcmp (gamedirs[i], playing))
			game_current = i;

	game_cursor = game_current > 0 ? game_current : 0;
}


void M_Game_Draw (void)
{
	int		i, row, y, last, visible;
	char	*title;

	M_DrawFrame ("gfx/p_option.lmp");
	M_DrawFooter ("Backspace: Back", "Enter: Select");

	if (!numgamedirs)
	{
		M_BoxText (MB_LABEL_X, PAGE_TOP, "No mission packs or mods found.",
			NULL);
		M_BoxText (MB_LABEL_X, PAGE_TOP + 16, "Each is a folder of its own",
			NULL);
		M_BoxText (MB_LABEL_X, PAGE_TOP + 24, "beside id1.", NULL);
		return;
	}

	if (game_denied)
	{
	// COM_CheckRegistered would refuse this a second after the restart, and the
	// container would sit in a restart loop with the reason scrolling past in a
	// log nobody is reading. Say it here instead, while there is a screen.
		M_DrawTextBox (56, 72, 30, 4);
		M_Print (64, 80, "The shareware data cannot run");
		M_Print (64, 88, "mission packs or mods. The full");
		M_Print (64, 96, "version of Quake is needed.");
		M_Print (64, 104, "Press any key.");
		return;
	}

	if (game_chosen >= 0)
	{
		M_DrawTextBox (56, 72, 30, 4);
		M_Print (64, 80, "Switch to");
		M_PrintWhite (64, 88, M_Game_Title (gamedirs[game_chosen]));
		M_Print (64, 104, "Quake must restart.  Y / N");
		return;
	}

	visible = (m_bh - PAGE_BOTTOM - MB_LIST_Y - 16) / MB_LIST_ROW + 1;

// Keep the cursor inside the window, as the controls and options menus do.
	if (game_cursor < game_top)
		game_top = game_cursor;
	if (game_cursor >= game_top + visible)
		game_top = game_cursor - visible + 1;
	if (game_top > numgamedirs - visible)
		game_top = numgamedirs - visible;
	if (game_top < 0)
		game_top = 0;

	last = game_top + visible;
	if (last > numgamedirs)
		last = numgamedirs;

	for (i = game_top, row = 0 ; i < last ; i++, row++)
	{
		y = MB_LIST_Y + row * MB_LIST_ROW;
		title = M_Game_Title (gamedirs[i]);

	// The one running now in white, so that the list says where you are as
	// well as where you could go.
		M_BoxBigText (MB_LIST_X, y + 2, title, i == game_current);

	// A mod is listed by its directory already; printing it twice says
	// nothing. A mission pack is listed by name, and the directory is worth
	// showing because that is what -game and QUAKE_GAME want.
		if (Q_strcmp (title, gamedirs[i]))
			M_BoxTextRight (m_bw - 12, y + 6, gamedirs[i], m_tint_dim);

		if (i == game_cursor)
			M_DrawDot (row);
	}

	M_DrawScrollbar (MB_LIST_Y, visible * MB_LIST_ROW, numgamedirs
		* MB_LIST_ROW, visible * MB_LIST_ROW, game_top * MB_LIST_ROW);
}


void M_Game_Key (int k)
{
	if (game_denied)
	{
		game_denied = false;
		m_entersound = true;
		return;
	}

	if (game_chosen >= 0)
	{
		switch (k)
		{
		case K_ENTER:
		case 'y':
		case 'Y':
			Sys_SetGameChoice (com_basedir, gamedirs[game_chosen]);
			key_dest = key_console;
			Host_Quit_f ();
			break;

		case K_ESCAPE:
		case 'n':
		case 'N':
			game_chosen = -1;
			m_entersound = true;
			break;
		}
		return;
	}

	switch (k)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;

	case K_ENTER:
		if (!numgamedirs)
			break;
		m_entersound = true;
		if (game_cursor == game_current)
			M_Menu_Options_f ();		// already playing it; nothing to restart for
		else if (!registered.value && Q_strcmp (gamedirs[game_cursor], GAMENAME))
			game_denied = true;
		else
			game_chosen = game_cursor;
		break;

	case K_UPARROW:
		if (!numgamedirs)
			break;
		S_LocalSound ("misc/menu1.wav");
		if (--game_cursor < 0)
			game_cursor = numgamedirs - 1;
		break;

	case K_DOWNARROW:
		if (!numgamedirs)
			break;
		S_LocalSound ("misc/menu1.wav");
		if (++game_cursor >= numgamedirs)
			game_cursor = 0;
		break;
	}
}

//=============================================================================
/* VIDEO MENU */

void M_Menu_Video_f (void)
{
	key_dest = key_menu;
	m_state = m_video;
	m_entersound = true;
}


void M_Video_Draw (void)
{
	M_Centre320 ();
	(*vid_menudrawfn) ();
}


void M_Video_Key (int key)
{
	(*vid_menukeyfn) (key);
}

//=============================================================================
/* HELP MENU */

int		help_page;
#define	NUM_HELP_PAGES	6


void M_Menu_Help_f (void)
{
	key_dest = key_menu;
	m_state = m_help;
	m_entersound = true;
	help_page = 0;
}



void M_Help_Draw (void)
{
	M_Centre320 ();
	M_DrawPic (0, 0, Draw_CachePic ( va("gfx/help%i.lmp", help_page)) );
}


void M_Help_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Main_f ();
		break;

	case K_UPARROW:
	case K_RIGHTARROW:
		m_entersound = true;
		if (++help_page >= NUM_HELP_PAGES)
			help_page = 0;
		break;

	case K_DOWNARROW:
	case K_LEFTARROW:
		m_entersound = true;
		if (--help_page < 0)
			help_page = NUM_HELP_PAGES-1;
		break;
	}

}

//=============================================================================
/* QUIT MENU */

int		msgNumber;
int		m_quit_prevstate;
qboolean	wasInMenus;

//
// The re-release asks plainly, and keeps id's jokes behind Show Classic Quit
// Prompt; so does this. Enter answers yes as well as Y, which is what lets a
// controller (A is Enter in the menus) quit at all.
//
cvar_t	m_classicquit = {"m_classicquit", "0", true};

static char *quitPlain[4] =
{
/* .........1.........2.... */
  "                        ",
  "      Quit Quake?       ",
  "                        ",
  "  Y: Quit      N: Stay  "
};

#ifndef	_WIN32
char *quitMessage [] = 
{
/* .........1.........2.... */
  "  Are you gonna quit    ",
  "  this game just like   ",
  "   everything else?     ",
  "                        ",
 
  " Milord, methinks that  ",
  "   thou art a lowly     ",
  " quitter. Is this true? ",
  "                        ",

  " Do I need to bust your ",
  "  face open for trying  ",
  "        to quit?        ",
  "                        ",

  " Man, I oughta smack you",
  "   for trying to quit!  ",
  "     Press Y to get     ",
  "      smacked out.      ",
 
  " Press Y to quit like a ",
  "   big loser in life.   ",
  "  Press N to stay proud ",
  "    and successful!     ",
 
  "   If you press Y to    ",
  "  quit, I will summon   ",
  "  Satan all over your   ",
  "      hard drive!       ",
 
  "  Um, Asmodeus dislikes ",
  " his children trying to ",
  " quit. Press Y to return",
  "   to your Tinkertoys.  ",
 
  "  If you quit now, I'll ",
  "  throw a blanket-party ",
  "   for you next time!   ",
  "                        "
};
#endif

void M_Menu_Quit_f (void)
{
	if (m_state == m_quit)
		return;
	wasInMenus = (key_dest == key_menu);
	key_dest = key_menu;
	m_quit_prevstate = m_state;
	m_state = m_quit;
	m_entersound = true;
	msgNumber = rand()&7;
}


void M_Quit_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
	case 'n':
	case 'N':
		if (wasInMenus)
		{
			m_state = m_quit_prevstate;
			m_entersound = true;
		}
		else
		{
			key_dest = key_game;
			m_state = m_none;
		}
		break;

	case 'Y':
	case 'y':
	case K_ENTER:
		key_dest = key_console;
		Host_Quit_f ();
		break;

	default:
		break;
	}

}


void M_Quit_Draw (void)
{
	if (wasInMenus)
	{
		m_state = m_quit_prevstate;
		m_recursiveDraw = true;
		M_Draw ();
		m_state = m_quit;
	}
	M_Centre320 ();

#ifdef _WIN32
	M_DrawTextBox (0, 0, 38, 23);
	M_PrintWhite (16, 12,  "  Quake version 1.09 by id Software\n\n");
	M_PrintWhite (16, 28,  "Programming        Art \n");
	M_Print (16, 36,  " John Carmack       Adrian Carmack\n");
	M_Print (16, 44,  " Michael Abrash     Kevin Cloud\n");
	M_Print (16, 52,  " John Cash          Paul Steed\n");
	M_Print (16, 60,  " Dave 'Zoid' Kirsch\n");
	M_PrintWhite (16, 68,  "Design             Biz\n");
	M_Print (16, 76,  " John Romero        Jay Wilbur\n");
	M_Print (16, 84,  " Sandy Petersen     Mike Wilson\n");
	M_Print (16, 92,  " American McGee     Donna Jackson\n");
	M_Print (16, 100,  " Tim Willits        Todd Hollenshead\n");
	M_PrintWhite (16, 108, "Support            Projects\n");
	M_Print (16, 116, " Barrett Alexander  Shawn Green\n");
	M_PrintWhite (16, 124, "Sound Effects\n");
	M_Print (16, 132, " Trent Reznor and Nine Inch Nails\n\n");
	M_PrintWhite (16, 140, "Quake is a trademark of Id Software,\n");
	M_PrintWhite (16, 148, "inc., (c)1996 Id Software, inc. All\n");
	M_PrintWhite (16, 156, "rights reserved. NIN logo is a\n");
	M_PrintWhite (16, 164, "registered trademark licensed to\n");
	M_PrintWhite (16, 172, "Nothing Interactive, Inc. All rights\n");
	M_PrintWhite (16, 180, "reserved. Press y to exit\n");
#else
	M_DrawTextBox (56, 76, 24, 4);
	if (m_classicquit.value)
	{
		M_Print (64, 84,  quitMessage[msgNumber*4+0]);
		M_Print (64, 92,  quitMessage[msgNumber*4+1]);
		M_Print (64, 100, quitMessage[msgNumber*4+2]);
		M_Print (64, 108, quitMessage[msgNumber*4+3]);
	}
	else
	{
		M_Print (64, 84,  quitPlain[0]);
		M_PrintWhite (64, 92,  quitPlain[1]);
		M_Print (64, 100, quitPlain[2]);
		M_Print (64, 108, quitPlain[3]);
	}
#endif
}

//=============================================================================

/* SERIAL CONFIG MENU */

int		serialConfig_cursor;
int		serialConfig_cursor_table[] = {48, 64, 80, 96, 112, 132};
#define	NUM_SERIALCONFIG_CMDS	6

static int ISA_uarts[]	= {0x3f8,0x2f8,0x3e8,0x2e8};
static int ISA_IRQs[]	= {4,3,4,3};
int serialConfig_baudrate[] = {9600,14400,19200,28800,38400,57600};

int		serialConfig_comport;
int		serialConfig_irq ;
int		serialConfig_baud;
char	serialConfig_phone[16];

void M_Menu_SerialConfig_f (void)
{
	int		n;
	int		port;
	int		baudrate;
	qboolean	useModem;

	key_dest = key_menu;
	m_state = m_serialconfig;
	m_entersound = true;
	if (JoiningGame && SerialConfig)
		serialConfig_cursor = 4;
	else
		serialConfig_cursor = 5;

	(*GetComPortConfig) (0, &port, &serialConfig_irq, &baudrate, &useModem);

	// map uart's port to COMx
	for (n = 0; n < 4; n++)
		if (ISA_uarts[n] == port)
			break;
	if (n == 4)
	{
		n = 0;
		serialConfig_irq = 4;
	}
	serialConfig_comport = n + 1;

	// map baudrate to index
	for (n = 0; n < 6; n++)
		if (serialConfig_baudrate[n] == baudrate)
			break;
	if (n == 6)
		n = 5;
	serialConfig_baud = n;

	m_return_onerror = false;
	m_return_reason[0] = 0;
}


void M_SerialConfig_Draw (void)
{
	qpic_t	*p;
	int		basex;
	char	*startJoin;
	char	*directModem;

	p = Draw_CachePic ("gfx/p_multi.lmp");
	basex = (320-p->width)/2;
	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Escape: Back", "Enter: Select");

	if (StartingGame)
		startJoin = "New Game";
	else
		startJoin = "Join Game";
	if (SerialConfig)
		directModem = "Modem";
	else
		directModem = "Direct Connect";
	M_Print (basex, 32, va ("%s - %s", startJoin, directModem));
	basex += 8;

	M_Print (basex, serialConfig_cursor_table[0], "Port");
	M_DrawTextBox (160, 40, 4, 1);
	M_Print (168, serialConfig_cursor_table[0], va("COM%u", serialConfig_comport));

	M_Print (basex, serialConfig_cursor_table[1], "IRQ");
	M_DrawTextBox (160, serialConfig_cursor_table[1]-8, 1, 1);
	M_Print (168, serialConfig_cursor_table[1], va("%u", serialConfig_irq));

	M_Print (basex, serialConfig_cursor_table[2], "Baud");
	M_DrawTextBox (160, serialConfig_cursor_table[2]-8, 5, 1);
	M_Print (168, serialConfig_cursor_table[2], va("%u", serialConfig_baudrate[serialConfig_baud]));

	if (SerialConfig)
	{
		M_Print (basex, serialConfig_cursor_table[3], "Modem Setup...");
		if (JoiningGame)
		{
			M_Print (basex, serialConfig_cursor_table[4], "Phone number");
			M_DrawTextBox (160, serialConfig_cursor_table[4]-8, 16, 1);
			M_Print (168, serialConfig_cursor_table[4], serialConfig_phone);
		}
	}

	if (JoiningGame)
	{
		M_DrawTextBox (basex, serialConfig_cursor_table[5]-8, 7, 1);
		M_Print (basex+8, serialConfig_cursor_table[5], "Connect");
	}
	else
	{
		M_DrawTextBox (basex, serialConfig_cursor_table[5]-8, 2, 1);
		M_Print (basex+8, serialConfig_cursor_table[5], "OK");
	}

	M_DrawCharacter (basex-8, serialConfig_cursor_table [serialConfig_cursor], 12+((int)(realtime*4)&1));

	if (serialConfig_cursor == 4)
		M_DrawCharacter (168 + 8*strlen(serialConfig_phone), serialConfig_cursor_table [serialConfig_cursor], 10+((int)(realtime*4)&1));

	if (*m_return_reason)
		M_PrintWhite (basex, 148, m_return_reason);
}


void M_SerialConfig_Key (int key)
{
	int		l;

	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		serialConfig_cursor--;
		if (serialConfig_cursor < 0)
			serialConfig_cursor = NUM_SERIALCONFIG_CMDS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		serialConfig_cursor++;
		if (serialConfig_cursor >= NUM_SERIALCONFIG_CMDS)
			serialConfig_cursor = 0;
		break;

	case K_LEFTARROW:
		if (serialConfig_cursor > 2)
			break;
		S_LocalSound ("misc/menu3.wav");

		if (serialConfig_cursor == 0)
		{
			serialConfig_comport--;
			if (serialConfig_comport == 0)
				serialConfig_comport = 4;
			serialConfig_irq = ISA_IRQs[serialConfig_comport-1];
		}

		if (serialConfig_cursor == 1)
		{
			serialConfig_irq--;
			if (serialConfig_irq == 6)
				serialConfig_irq = 5;
			if (serialConfig_irq == 1)
				serialConfig_irq = 7;
		}

		if (serialConfig_cursor == 2)
		{
			serialConfig_baud--;
			if (serialConfig_baud < 0)
				serialConfig_baud = 5;
		}

		break;

	case K_RIGHTARROW:
		if (serialConfig_cursor > 2)
			break;
forward:
		S_LocalSound ("misc/menu3.wav");

		if (serialConfig_cursor == 0)
		{
			serialConfig_comport++;
			if (serialConfig_comport > 4)
				serialConfig_comport = 1;
			serialConfig_irq = ISA_IRQs[serialConfig_comport-1];
		}

		if (serialConfig_cursor == 1)
		{
			serialConfig_irq++;
			if (serialConfig_irq == 6)
				serialConfig_irq = 7;
			if (serialConfig_irq == 8)
				serialConfig_irq = 2;
		}

		if (serialConfig_cursor == 2)
		{
			serialConfig_baud++;
			if (serialConfig_baud > 5)
				serialConfig_baud = 0;
		}

		break;

	case K_ENTER:
		if (serialConfig_cursor < 3)
			goto forward;

		m_entersound = true;

		if (serialConfig_cursor == 3)
		{
			(*SetComPortConfig) (0, ISA_uarts[serialConfig_comport-1], serialConfig_irq, serialConfig_baudrate[serialConfig_baud], SerialConfig);

			M_Menu_ModemConfig_f ();
			break;
		}

		if (serialConfig_cursor == 4)
		{
			serialConfig_cursor = 5;
			break;
		}

		// serialConfig_cursor == 5 (OK/CONNECT)
		(*SetComPortConfig) (0, ISA_uarts[serialConfig_comport-1], serialConfig_irq, serialConfig_baudrate[serialConfig_baud], SerialConfig);

		M_ConfigureNetSubsystem ();

		if (StartingGame)
		{
			M_Menu_GameOptions_f ();
			break;
		}

		m_return_state = m_state;
		m_return_onerror = true;
		key_dest = key_game;
		m_state = m_none;

		if (SerialConfig)
			Cbuf_AddText (va ("connect \"%s\"\n", serialConfig_phone));
		else
			Cbuf_AddText ("connect\n");
		break;

	case K_BACKSPACE:
		if (serialConfig_cursor == 4)
		{
			if (strlen(serialConfig_phone))
				serialConfig_phone[strlen(serialConfig_phone)-1] = 0;
		}
		break;

	default:
		if (key < 32 || key > 127)
			break;
		if (serialConfig_cursor == 4)
		{
			l = strlen(serialConfig_phone);
			if (l < 15)
			{
				serialConfig_phone[l+1] = 0;
				serialConfig_phone[l] = key;
			}
		}
	}

	if (DirectConfig && (serialConfig_cursor == 3 || serialConfig_cursor == 4))
		if (key == K_UPARROW)
			serialConfig_cursor = 2;
		else
			serialConfig_cursor = 5;

	if (SerialConfig && StartingGame && serialConfig_cursor == 4)
		if (key == K_UPARROW)
			serialConfig_cursor = 3;
		else
			serialConfig_cursor = 5;
}

//=============================================================================
/* MODEM CONFIG MENU */

int		modemConfig_cursor;
int		modemConfig_cursor_table [] = {40, 56, 88, 120, 156};
#define NUM_MODEMCONFIG_CMDS	5

char	modemConfig_dialing;
char	modemConfig_clear [16];
char	modemConfig_init [32];
char	modemConfig_hangup [16];

void M_Menu_ModemConfig_f (void)
{
	key_dest = key_menu;
	m_state = m_modemconfig;
	m_entersound = true;
	(*GetModemConfig) (0, &modemConfig_dialing, modemConfig_clear, modemConfig_init, modemConfig_hangup);
}


void M_ModemConfig_Draw (void)
{
	qpic_t	*p;
	int		basex;

	p = Draw_CachePic ("gfx/p_multi.lmp");
	basex = (320-p->width)/2;
	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Escape: Back", "Enter: Select");
	basex += 8;

	if (modemConfig_dialing == 'P')
		M_Print (basex, modemConfig_cursor_table[0], "Pulse Dialing");
	else
		M_Print (basex, modemConfig_cursor_table[0], "Touch Tone Dialing");

	M_Print (basex, modemConfig_cursor_table[1], "Clear");
	M_DrawTextBox (basex, modemConfig_cursor_table[1]+4, 16, 1);
	M_Print (basex+8, modemConfig_cursor_table[1]+12, modemConfig_clear);
	if (modemConfig_cursor == 1)
		M_DrawCharacter (basex+8 + 8*strlen(modemConfig_clear), modemConfig_cursor_table[1]+12, 10+((int)(realtime*4)&1));

	M_Print (basex, modemConfig_cursor_table[2], "Init");
	M_DrawTextBox (basex, modemConfig_cursor_table[2]+4, 30, 1);
	M_Print (basex+8, modemConfig_cursor_table[2]+12, modemConfig_init);
	if (modemConfig_cursor == 2)
		M_DrawCharacter (basex+8 + 8*strlen(modemConfig_init), modemConfig_cursor_table[2]+12, 10+((int)(realtime*4)&1));

	M_Print (basex, modemConfig_cursor_table[3], "Hangup");
	M_DrawTextBox (basex, modemConfig_cursor_table[3]+4, 16, 1);
	M_Print (basex+8, modemConfig_cursor_table[3]+12, modemConfig_hangup);
	if (modemConfig_cursor == 3)
		M_DrawCharacter (basex+8 + 8*strlen(modemConfig_hangup), modemConfig_cursor_table[3]+12, 10+((int)(realtime*4)&1));

	M_DrawTextBox (basex, modemConfig_cursor_table[4]-8, 2, 1);
	M_Print (basex+8, modemConfig_cursor_table[4], "OK");

	M_DrawCharacter (basex-8, modemConfig_cursor_table [modemConfig_cursor], 12+((int)(realtime*4)&1));
}


void M_ModemConfig_Key (int key)
{
	int		l;

	switch (key)
	{
	case K_ESCAPE:
		M_Menu_SerialConfig_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		modemConfig_cursor--;
		if (modemConfig_cursor < 0)
			modemConfig_cursor = NUM_MODEMCONFIG_CMDS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		modemConfig_cursor++;
		if (modemConfig_cursor >= NUM_MODEMCONFIG_CMDS)
			modemConfig_cursor = 0;
		break;

	case K_LEFTARROW:
	case K_RIGHTARROW:
		if (modemConfig_cursor == 0)
		{
			if (modemConfig_dialing == 'P')
				modemConfig_dialing = 'T';
			else
				modemConfig_dialing = 'P';
			S_LocalSound ("misc/menu1.wav");
		}
		break;

	case K_ENTER:
		if (modemConfig_cursor == 0)
		{
			if (modemConfig_dialing == 'P')
				modemConfig_dialing = 'T';
			else
				modemConfig_dialing = 'P';
			m_entersound = true;
		}

		if (modemConfig_cursor == 4)
		{
			(*SetModemConfig) (0, va ("%c", modemConfig_dialing), modemConfig_clear, modemConfig_init, modemConfig_hangup);
			m_entersound = true;
			M_Menu_SerialConfig_f ();
		}
		break;

	case K_BACKSPACE:
		if (modemConfig_cursor == 1)
		{
			if (strlen(modemConfig_clear))
				modemConfig_clear[strlen(modemConfig_clear)-1] = 0;
		}

		if (modemConfig_cursor == 2)
		{
			if (strlen(modemConfig_init))
				modemConfig_init[strlen(modemConfig_init)-1] = 0;
		}

		if (modemConfig_cursor == 3)
		{
			if (strlen(modemConfig_hangup))
				modemConfig_hangup[strlen(modemConfig_hangup)-1] = 0;
		}
		break;

	default:
		if (key < 32 || key > 127)
			break;

		if (modemConfig_cursor == 1)
		{
			l = strlen(modemConfig_clear);
			if (l < 15)
			{
				modemConfig_clear[l+1] = 0;
				modemConfig_clear[l] = key;
			}
		}

		if (modemConfig_cursor == 2)
		{
			l = strlen(modemConfig_init);
			if (l < 29)
			{
				modemConfig_init[l+1] = 0;
				modemConfig_init[l] = key;
			}
		}

		if (modemConfig_cursor == 3)
		{
			l = strlen(modemConfig_hangup);
			if (l < 15)
			{
				modemConfig_hangup[l+1] = 0;
				modemConfig_hangup[l] = key;
			}
		}
	}
}

//=============================================================================
/* LAN CONFIG MENU */

int		lanConfig_cursor = -1;
int		lanConfig_cursor_table [] = {72, 92, 124};
#define NUM_LANCONFIG_CMDS	3

int 	lanConfig_port;
char	lanConfig_portname[6];
char	lanConfig_joinname[22];

void M_Menu_LanConfig_f (void)
{
	key_dest = key_menu;
	m_state = m_lanconfig;
	m_entersound = true;
	if (lanConfig_cursor == -1)
	{
		if (JoiningGame && TCPIPConfig)
			lanConfig_cursor = 2;
		else
			lanConfig_cursor = 1;
	}
	if (StartingGame && lanConfig_cursor == 2)
		lanConfig_cursor = 1;
	lanConfig_port = DEFAULTnet_hostport;
	sprintf(lanConfig_portname, "%u", lanConfig_port);

	m_return_onerror = false;
	m_return_reason[0] = 0;
}


void M_LanConfig_Draw (void)
{
	qpic_t	*p;
	int		basex;
	char	*startJoin;
	char	*protocol;

	p = Draw_CachePic ("gfx/p_multi.lmp");
	basex = (320-p->width)/2;
	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Escape: Back", "Enter: Select");

	if (StartingGame)
		startJoin = "New Game";
	else
		startJoin = "Join Game";
	if (IPXConfig)
		protocol = "IPX";
	else
		protocol = "TCP/IP";
	M_Print (basex, 32, va ("%s - %s", startJoin, protocol));
	basex += 8;

	M_Print (basex, 52, "Address:");
	if (IPXConfig)
		M_Print (basex+9*8, 52, my_ipx_address);
	else
		M_Print (basex+9*8, 52, my_tcpip_address);

	M_Print (basex, lanConfig_cursor_table[0], "Port");
	M_DrawTextBox (basex+8*8, lanConfig_cursor_table[0]-8, 6, 1);
	M_Print (basex+9*8, lanConfig_cursor_table[0], lanConfig_portname);

	if (JoiningGame)
	{
		M_Print (basex, lanConfig_cursor_table[1], "Search for local games...");
		M_Print (basex, 108, "Join game at:");
		M_DrawTextBox (basex+8, lanConfig_cursor_table[2]-8, 22, 1);
		M_Print (basex+16, lanConfig_cursor_table[2], lanConfig_joinname);
	}
	else
	{
		M_DrawTextBox (basex, lanConfig_cursor_table[1]-8, 2, 1);
		M_Print (basex+8, lanConfig_cursor_table[1], "OK");
	}

	M_DrawCharacter (basex-8, lanConfig_cursor_table [lanConfig_cursor], 12+((int)(realtime*4)&1));

	if (lanConfig_cursor == 0)
		M_DrawCharacter (basex+9*8 + 8*strlen(lanConfig_portname), lanConfig_cursor_table [0], 10+((int)(realtime*4)&1));

	if (lanConfig_cursor == 2)
		M_DrawCharacter (basex+16 + 8*strlen(lanConfig_joinname), lanConfig_cursor_table [2], 10+((int)(realtime*4)&1));

	if (*m_return_reason)
		M_PrintWhite (basex, 148, m_return_reason);
}


void M_LanConfig_Key (int key)
{
	int		l;

	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		lanConfig_cursor--;
		if (lanConfig_cursor < 0)
			lanConfig_cursor = NUM_LANCONFIG_CMDS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		lanConfig_cursor++;
		if (lanConfig_cursor >= NUM_LANCONFIG_CMDS)
			lanConfig_cursor = 0;
		break;

	case K_ENTER:
		if (lanConfig_cursor == 0)
			break;

		m_entersound = true;

		M_ConfigureNetSubsystem ();

		if (lanConfig_cursor == 1)
		{
			if (StartingGame)
			{
				M_Menu_GameOptions_f ();
				break;
			}
			M_Menu_Search_f();
			break;
		}

		if (lanConfig_cursor == 2)
		{
			m_return_state = m_state;
			m_return_onerror = true;
			key_dest = key_game;
			m_state = m_none;
			Cbuf_AddText ( va ("connect \"%s\"\n", lanConfig_joinname) );
			break;
		}

		break;

	case K_BACKSPACE:
		if (lanConfig_cursor == 0)
		{
			if (strlen(lanConfig_portname))
				lanConfig_portname[strlen(lanConfig_portname)-1] = 0;
		}

		if (lanConfig_cursor == 2)
		{
			if (strlen(lanConfig_joinname))
				lanConfig_joinname[strlen(lanConfig_joinname)-1] = 0;
		}
		break;

	default:
		if (key < 32 || key > 127)
			break;

		if (lanConfig_cursor == 2)
		{
			l = strlen(lanConfig_joinname);
			if (l < 21)
			{
				lanConfig_joinname[l+1] = 0;
				lanConfig_joinname[l] = key;
			}
		}

		if (key < '0' || key > '9')
			break;
		if (lanConfig_cursor == 0)
		{
			l = strlen(lanConfig_portname);
			if (l < 5)
			{
				lanConfig_portname[l+1] = 0;
				lanConfig_portname[l] = key;
			}
		}
	}

	if (StartingGame && lanConfig_cursor == 2)
		if (key == K_UPARROW)
			lanConfig_cursor = 1;
		else
			lanConfig_cursor = 0;

	l =  Q_atoi(lanConfig_portname);
	if (l > 65535)
		l = lanConfig_port;
	else
		lanConfig_port = l;
	sprintf(lanConfig_portname, "%u", lanConfig_port);
}

//=============================================================================
/* GAME OPTIONS MENU */

typedef struct
{
	char	*name;
	char	*description;
} level_t;

level_t		levels[] =
{
	{"start", "Entrance"},	// 0

	{"e1m1", "Slipgate Complex"},				// 1
	{"e1m2", "Castle of the Damned"},
	{"e1m3", "The Necropolis"},
	{"e1m4", "The Grisly Grotto"},
	{"e1m5", "Gloom Keep"},
	{"e1m6", "The Door To Chthon"},
	{"e1m7", "The House of Chthon"},
	{"e1m8", "Ziggurat Vertigo"},

	{"e2m1", "The Installation"},				// 9
	{"e2m2", "Ogre Citadel"},
	{"e2m3", "Crypt of Decay"},
	{"e2m4", "The Ebon Fortress"},
	{"e2m5", "The Wizard's Manse"},
	{"e2m6", "The Dismal Oubliette"},
	{"e2m7", "Underearth"},

	{"e3m1", "Termination Central"},			// 16
	{"e3m2", "The Vaults of Zin"},
	{"e3m3", "The Tomb of Terror"},
	{"e3m4", "Satan's Dark Delight"},
	{"e3m5", "Wind Tunnels"},
	{"e3m6", "Chambers of Torment"},
	{"e3m7", "The Haunted Halls"},

	{"e4m1", "The Sewage System"},				// 23
	{"e4m2", "The Tower of Despair"},
	{"e4m3", "The Elder God Shrine"},
	{"e4m4", "The Palace of Hate"},
	{"e4m5", "Hell's Atrium"},
	{"e4m6", "The Pain Maze"},
	{"e4m7", "Azure Agony"},
	{"e4m8", "The Nameless City"},

	{"end", "Shub-Niggurath's Pit"},			// 31

	{"dm1", "Place of Two Deaths"},				// 32
	{"dm2", "Claustrophobopolis"},
	{"dm3", "The Abandoned Base"},
	{"dm4", "The Bad Place"},
	{"dm5", "The Cistern"},
	{"dm6", "The Dark Zone"}
};

//MED 01/06/97 added hipnotic levels
level_t     hipnoticlevels[] =
{
   {"start", "Command HQ"},  // 0

   {"hip1m1", "The Pumping Station"},          // 1
   {"hip1m2", "Storage Facility"},
   {"hip1m3", "The Lost Mine"},
   {"hip1m4", "Research Facility"},
   {"hip1m5", "Military Complex"},

   {"hip2m1", "Ancient Realms"},          // 6
   {"hip2m2", "The Black Cathedral"},
   {"hip2m3", "The Catacombs"},
   {"hip2m4", "The Crypt"},
   {"hip2m5", "Mortum's Keep"},
   {"hip2m6", "The Gremlin's Domain"},

   {"hip3m1", "Tur Torment"},       // 12
   {"hip3m2", "Pandemonium"},
   {"hip3m3", "Limbo"},
   {"hip3m4", "The Gauntlet"},

   {"hipend", "Armagon's Lair"},       // 16

   {"hipdm1", "The Edge of Oblivion"}           // 17
};

//PGM 01/07/97 added rogue levels
//PGM 03/02/97 added dmatch level
level_t		roguelevels[] =
{
	{"start",	"Split Decision"},
	{"r1m1",	"Deviant's Domain"},
	{"r1m2",	"Dread Portal"},
	{"r1m3",	"Judgement Call"},
	{"r1m4",	"Cave of Death"},
	{"r1m5",	"Towers of Wrath"},
	{"r1m6",	"Temple of Pain"},
	{"r1m7",	"Tomb of the Overlord"},
	{"r2m1",	"Tempus Fugit"},
	{"r2m2",	"Elemental Fury I"},
	{"r2m3",	"Elemental Fury II"},
	{"r2m4",	"Curse of Osiris"},
	{"r2m5",	"Wizard's Keep"},
	{"r2m6",	"Blood Sacrifice"},
	{"r2m7",	"Last Bastion"},
	{"r2m8",	"Source of Evil"},
	{"ctf1",    "Division of Change"}
};

typedef struct
{
	char	*description;
	int		firstLevel;
	int		levels;
} episode_t;

episode_t	episodes[] =
{
	{"Welcome to Quake", 0, 1},
	{"Doomed Dimension", 1, 8},
	{"Realm of Black Magic", 9, 7},
	{"Netherworld", 16, 7},
	{"The Elder World", 23, 8},
	{"Final Level", 31, 1},
	{"Deathmatch Arena", 32, 6}
};

//MED 01/06/97  added hipnotic episodes
episode_t   hipnoticepisodes[] =
{
   {"Scourge of Armagon", 0, 1},
   {"Fortress of the Dead", 1, 5},
   {"Dominion of Darkness", 6, 6},
   {"The Rift", 12, 4},
   {"Final Level", 16, 1},
   {"Deathmatch Arena", 17, 1}
};

//PGM 01/07/97 added rogue episodes
//PGM 03/02/97 added dmatch episode
episode_t	rogueepisodes[] =
{
	{"Introduction", 0, 1},
	{"Hell's Fortress", 1, 7},
	{"Corridors of Time", 8, 8},
	{"Deathmatch Arena", 16, 1}
};

int	startepisode;
int	startlevel;
int maxplayers;
qboolean m_serverInfoMessage = false;
double m_serverInfoMessageTime;

void M_Menu_GameOptions_f (void)
{
	key_dest = key_menu;
	m_state = m_gameoptions;
	m_entersound = true;
	if (maxplayers == 0)
		maxplayers = svs.maxclients;
	if (maxplayers < 2)
		maxplayers = svs.maxclientslimit;
}


int gameoptions_cursor_table[] = {40, 56, 64, 72, 80, 88, 96, 112, 120};
#define	NUM_GAMEOPTIONS	9

// id right-aligned these labels to end at 136, which started "Max players" at
// 48, touching the plaque (x = 16 to 48). They are 8 further right, and the
// cursor moves from 144 to 148, between the labels and the values. The values
// stay at 160, so "Nightmare difficulty" still fits on the screen.
#define	GAMEOPT_LABEL_X		8
#define	GAMEOPT_CURSOR_X	148
int		gameoptions_cursor;

void M_GameOptions_Draw (void)
{
	int		x;

	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Escape: Back", "Enter: Select");

	M_DrawTextBox (152, 32, 10, 1);
	M_Print (160, 40, "begin game");

	M_Print (GAMEOPT_LABEL_X, 56, "      Max players");
	M_Print (160, 56, va("%i", maxplayers) );

	M_Print (GAMEOPT_LABEL_X, 64, "        Game Type");
	if (coop.value)
		M_Print (160, 64, "Cooperative");
	else
		M_Print (160, 64, "Deathmatch");

	M_Print (GAMEOPT_LABEL_X, 72, "        Teamplay");
	if (rogue)
	{
		char *msg;

		switch((int)teamplay.value)
		{
			case 1: msg = "No Friendly Fire"; break;
			case 2: msg = "Friendly Fire"; break;
			case 3: msg = "Tag"; break;
			case 4: msg = "Capture the Flag"; break;
			case 5: msg = "One Flag CTF"; break;
			case 6: msg = "Three Team CTF"; break;
			default: msg = "Off"; break;
		}
		M_Print (160, 72, msg);
	}
	else
	{
		char *msg;

		switch((int)teamplay.value)
		{
			case 1: msg = "No Friendly Fire"; break;
			case 2: msg = "Friendly Fire"; break;
			default: msg = "Off"; break;
		}
		M_Print (160, 72, msg);
	}

	M_Print (GAMEOPT_LABEL_X, 80, "            Skill");
	if (skill.value == 0)
		M_Print (160, 80, "Easy difficulty");
	else if (skill.value == 1)
		M_Print (160, 80, "Normal difficulty");
	else if (skill.value == 2)
		M_Print (160, 80, "Hard difficulty");
	else
		M_Print (160, 80, "Nightmare difficulty");

	M_Print (GAMEOPT_LABEL_X, 88, "       Frag Limit");
	if (fraglimit.value == 0)
		M_Print (160, 88, "none");
	else
		M_Print (160, 88, va("%i frags", (int)fraglimit.value));

	M_Print (GAMEOPT_LABEL_X, 96, "       Time Limit");
	if (timelimit.value == 0)
		M_Print (160, 96, "none");
	else
		M_Print (160, 96, va("%i minutes", (int)timelimit.value));

	M_Print (GAMEOPT_LABEL_X, 112, "         Episode");
   //MED 01/06/97 added hipnotic episodes
   if (hipnotic)
      M_Print (160, 112, hipnoticepisodes[startepisode].description);
   //PGM 01/07/97 added rogue episodes
   else if (rogue)
      M_Print (160, 112, rogueepisodes[startepisode].description);
   else
      M_Print (160, 112, episodes[startepisode].description);

	M_Print (GAMEOPT_LABEL_X, 120, "           Level");
   //MED 01/06/97 added hipnotic episodes
   if (hipnotic)
   {
      M_Print (160, 120, hipnoticlevels[hipnoticepisodes[startepisode].firstLevel + startlevel].description);
      M_Print (160, 128, hipnoticlevels[hipnoticepisodes[startepisode].firstLevel + startlevel].name);
   }
   //PGM 01/07/97 added rogue episodes
   else if (rogue)
   {
      M_Print (160, 120, roguelevels[rogueepisodes[startepisode].firstLevel + startlevel].description);
      M_Print (160, 128, roguelevels[rogueepisodes[startepisode].firstLevel + startlevel].name);
   }
   else
   {
      M_Print (160, 120, levels[episodes[startepisode].firstLevel + startlevel].description);
      M_Print (160, 128, levels[episodes[startepisode].firstLevel + startlevel].name);
   }

// line cursor
	M_DrawCharacter (GAMEOPT_CURSOR_X, gameoptions_cursor_table[gameoptions_cursor], 12+((int)(realtime*4)&1));

	if (m_serverInfoMessage)
	{
		if ((realtime - m_serverInfoMessageTime) < 5.0)
		{
			x = (320-26*8)/2;
			M_DrawTextBox (x, 138, 24, 4);
			x += 8;
			M_Print (x, 146, "  More than 4 players   ");
			M_Print (x, 154, " requires using command ");
			M_Print (x, 162, "line parameters; please ");
			M_Print (x, 170, "   see techinfo.txt.    ");
		}
		else
		{
			m_serverInfoMessage = false;
		}
	}
}


void M_NetStart_Change (int dir)
{
	int count;

	switch (gameoptions_cursor)
	{
	case 1:
		maxplayers += dir;
		if (maxplayers > svs.maxclientslimit)
		{
			maxplayers = svs.maxclientslimit;
			m_serverInfoMessage = true;
			m_serverInfoMessageTime = realtime;
		}
		if (maxplayers < 2)
			maxplayers = 2;
		break;

	case 2:
		Cvar_SetValue ("coop", coop.value ? 0 : 1);
		break;

	case 3:
		if (rogue)
			count = 6;
		else
			count = 2;

		Cvar_SetValue ("teamplay", teamplay.value + dir);
		if (teamplay.value > count)
			Cvar_SetValue ("teamplay", 0);
		else if (teamplay.value < 0)
			Cvar_SetValue ("teamplay", count);
		break;

	case 4:
		Cvar_SetValue ("skill", skill.value + dir);
		if (skill.value > 3)
			Cvar_SetValue ("skill", 0);
		if (skill.value < 0)
			Cvar_SetValue ("skill", 3);
		break;

	case 5:
		Cvar_SetValue ("fraglimit", fraglimit.value + dir*10);
		if (fraglimit.value > 100)
			Cvar_SetValue ("fraglimit", 0);
		if (fraglimit.value < 0)
			Cvar_SetValue ("fraglimit", 100);
		break;

	case 6:
		Cvar_SetValue ("timelimit", timelimit.value + dir*5);
		if (timelimit.value > 60)
			Cvar_SetValue ("timelimit", 0);
		if (timelimit.value < 0)
			Cvar_SetValue ("timelimit", 60);
		break;

	case 7:
		startepisode += dir;
	//MED 01/06/97 added hipnotic count
		if (hipnotic)
			count = 6;
	//PGM 01/07/97 added rogue count
	//PGM 03/02/97 added 1 for dmatch episode
		else if (rogue)
			count = 4;
		else if (registered.value)
			count = 7;
		else
			count = 2;

		if (startepisode < 0)
			startepisode = count - 1;

		if (startepisode >= count)
			startepisode = 0;

		startlevel = 0;
		break;

	case 8:
		startlevel += dir;
    //MED 01/06/97 added hipnotic episodes
		if (hipnotic)
			count = hipnoticepisodes[startepisode].levels;
	//PGM 01/06/97 added hipnotic episodes
		else if (rogue)
			count = rogueepisodes[startepisode].levels;
		else
			count = episodes[startepisode].levels;

		if (startlevel < 0)
			startlevel = count - 1;

		if (startlevel >= count)
			startlevel = 0;
		break;
	}
}

void M_GameOptions_Key (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		gameoptions_cursor--;
		if (gameoptions_cursor < 0)
			gameoptions_cursor = NUM_GAMEOPTIONS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		gameoptions_cursor++;
		if (gameoptions_cursor >= NUM_GAMEOPTIONS)
			gameoptions_cursor = 0;
		break;

	case K_LEFTARROW:
		if (gameoptions_cursor == 0)
			break;
		S_LocalSound ("misc/menu3.wav");
		M_NetStart_Change (-1);
		break;

	case K_RIGHTARROW:
		if (gameoptions_cursor == 0)
			break;
		S_LocalSound ("misc/menu3.wav");
		M_NetStart_Change (1);
		break;

	case K_ENTER:
		S_LocalSound ("misc/menu2.wav");
		if (gameoptions_cursor == 0)
		{
			if (sv.active)
				Cbuf_AddText ("disconnect\n");
			Cbuf_AddText ("listen 0\n");	// so host_netport will be re-examined
			Cbuf_AddText ( va ("maxplayers %u\n", maxplayers) );
			SCR_BeginLoadingPlaque ();

			if (hipnotic)
				Cbuf_AddText ( va ("map %s\n", hipnoticlevels[hipnoticepisodes[startepisode].firstLevel + startlevel].name) );
			else if (rogue)
				Cbuf_AddText ( va ("map %s\n", roguelevels[rogueepisodes[startepisode].firstLevel + startlevel].name) );
			else
				Cbuf_AddText ( va ("map %s\n", levels[episodes[startepisode].firstLevel + startlevel].name) );

			return;
		}

		M_NetStart_Change (1);
		break;
	}
}

//=============================================================================
/* SEARCH MENU */

qboolean	searchComplete = false;
double		searchCompleteTime;

void M_Menu_Search_f (void)
{
	key_dest = key_menu;
	m_state = m_search;
	m_entersound = false;
	slistSilent = true;
	slistLocal = false;
	searchComplete = false;
	NET_Slist_f();

}


void M_Search_Draw (void)
{
	int x;

	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Backspace: Back", "Enter: Select");
	x = (320/2) - ((12*8)/2) + 4;
	M_DrawTextBox (x-8, 32, 12, 1);
	M_Print (x, 40, "Searching...");

	if(slistInProgress)
	{
		NET_Poll();
		return;
	}

	if (! searchComplete)
	{
		searchComplete = true;
		searchCompleteTime = realtime;
	}

	if (hostCacheCount)
	{
		M_Menu_ServerList_f ();
		return;
	}

	M_PrintWhite ((320/2) - ((22*8)/2), 64, "No Quake servers found");
	if ((realtime - searchCompleteTime) < 3.0)
		return;

	M_Menu_LanConfig_f ();
}


void M_Search_Key (int key)
{
}

//=============================================================================
/* SLIST MENU */

int		slist_cursor;
qboolean slist_sorted;

void M_Menu_ServerList_f (void)
{
	key_dest = key_menu;
	m_state = m_slist;
	m_entersound = true;
	slist_cursor = 0;
	m_return_onerror = false;
	m_return_reason[0] = 0;
	slist_sorted = false;
}


void M_ServerList_Draw (void)
{
	int		n;
	char	string [64];

	if (!slist_sorted)
	{
		if (hostCacheCount > 1)
		{
			int	i,j;
			hostcache_t temp;
			for (i = 0; i < hostCacheCount; i++)
				for (j = i+1; j < hostCacheCount; j++)
					if (strcmp(hostcache[j].name, hostcache[i].name) < 0)
					{
						Q_memcpy(&temp, &hostcache[j], sizeof(hostcache_t));
						Q_memcpy(&hostcache[j], &hostcache[i], sizeof(hostcache_t));
						Q_memcpy(&hostcache[i], &temp, sizeof(hostcache_t));
					}
		}
		slist_sorted = true;
	}

	M_DrawFrame ("gfx/p_multi.lmp");
	M_DrawFooter ("Backspace: Back", "Enter: Select");
	for (n = 0; n < hostCacheCount; n++)
	{
		if (hostcache[n].maxusers)
			sprintf(string, "%-15.15s %-15.15s %2u/%2u\n", hostcache[n].name, hostcache[n].map, hostcache[n].users, hostcache[n].maxusers);
		else
			sprintf(string, "%-15.15s %-15.15s\n", hostcache[n].name, hostcache[n].map);
		M_Print (56, 32 + 8*n, string);
	}
	M_DrawCharacter (48, 32 + slist_cursor*8, 12+((int)(realtime*4)&1));

	if (*m_return_reason)
		M_PrintWhite (56, 148, m_return_reason);
}


void M_ServerList_Key (int k)
{
	switch (k)
	{
	case K_ESCAPE:
		M_Menu_LanConfig_f ();
		break;

	case K_SPACE:
		M_Menu_Search_f ();
		break;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("misc/menu1.wav");
		slist_cursor--;
		if (slist_cursor < 0)
			slist_cursor = hostCacheCount - 1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		slist_cursor++;
		if (slist_cursor >= hostCacheCount)
			slist_cursor = 0;
		break;

	case K_ENTER:
		S_LocalSound ("misc/menu2.wav");
		m_return_state = m_state;
		m_return_onerror = true;
		slist_sorted = false;
		key_dest = key_game;
		m_state = m_none;
		Cbuf_AddText ( va ("connect \"%s\"\n", hostcache[slist_cursor].cname) );
		break;

	default:
		break;
	}

}

//=============================================================================
/* Menu Subsystem */


void M_Init (void)
{
	Cvar_RegisterVariable (&m_classicquit);
	Cmd_AddCommand ("togglemenu", M_ToggleMenu_f);

	Cmd_AddCommand ("menu_main", M_Menu_Main_f);
	Cmd_AddCommand ("menu_singleplayer", M_Menu_SinglePlayer_f);
	Cmd_AddCommand ("menu_load", M_Menu_Load_f);
	Cmd_AddCommand ("menu_save", M_Menu_Save_f);
	Cmd_AddCommand ("menu_multiplayer", M_Menu_MultiPlayer_f);
	Cmd_AddCommand ("menu_setup", M_Menu_Setup_f);
	Cmd_AddCommand ("menu_options", M_Menu_Options_f);
	Cmd_AddCommand ("menu_keys", M_Menu_Keys_f);
	Cmd_AddCommand ("menu_video", M_Menu_Video_f);
	Cmd_AddCommand ("menu_game", M_Menu_Game_f);
	Cmd_AddCommand ("help", M_Menu_Help_f);
	Cmd_AddCommand ("menu_quit", M_Menu_Quit_f);
}


void M_Draw (void)
{
	if (m_state == m_none || key_dest != key_menu)
		return;

	if (!m_recursiveDraw)
	{
		scr_copyeverything = 1;

		if (scr_con_current || m_state == m_optpage || m_state == m_keys)
		{
			Draw_MenuBackground ();
			VID_UnlockBuffer ();
			S_ExtraUpdate ();
			VID_LockBuffer ();
		}
		else
			Draw_FadeScreen ();

		scr_fullupdate = 0;
	}
	else
	{
		m_recursiveDraw = false;
	}

	M_BeginCanvas ();

	switch (m_state)
	{
	case m_none:
		break;

	case m_main:
		M_Main_Draw ();
		break;

	case m_singleplayer:
		M_SinglePlayer_Draw ();
		break;

	case m_load:
		M_Load_Draw ();
		break;

	case m_save:
		M_Save_Draw ();
		break;

	case m_multiplayer:
		M_MultiPlayer_Draw ();
		break;

	case m_setup:
		M_Setup_Draw ();
		break;

	case m_net:
		M_Net_Draw ();
		break;

	case m_options:
		M_Options_Draw ();
		break;

	case m_optpage:
		M_OptPage_Draw ();
		break;

	case m_keys:
		M_Keys_Draw ();
		break;

	case m_video:
		M_Video_Draw ();
		break;

	case m_game:
		M_Game_Draw ();
		break;

	case m_help:
		M_Help_Draw ();
		break;

	case m_quit:
		M_Quit_Draw ();
		break;

	case m_serialconfig:
		M_SerialConfig_Draw ();
		break;

	case m_modemconfig:
		M_ModemConfig_Draw ();
		break;

	case m_lanconfig:
		M_LanConfig_Draw ();
		break;

	case m_gameoptions:
		M_GameOptions_Draw ();
		break;

	case m_search:
		M_Search_Draw ();
		break;

	case m_slist:
		M_ServerList_Draw ();
		break;
	}

	M_EndCanvas ();

	if (m_entersound)
	{
		S_LocalSound ("misc/menu2.wav");
		m_entersound = false;
	}

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();
}


//
// Backspace goes back a menu level, the way Escape does, and closes the menu
// from the top one. It is the key a browser, a phone keyboard and most other
// software use for "back", and Escape is a stretch away or, in a browser in
// fullscreen, taken by the browser itself to leave fullscreen.
//
// Except where Backspace already has a job. These are the places it does, and
// they keep it: the text fields (your name, the server address, a modem
// string), and the key bindings screen while it is waiting for a key, where
// Backspace is a key like any other to bind. Clearing a binding there is Del
// (Y on a controller), as the re-release has it, so that Backspace goes back
// from that screen as it does from every other. Escape still goes back from
// all of them.
//
static qboolean M_BackspaceEdits (void)
{
	switch (m_state)
	{
	case m_keys:
		return M_KeysGrabbing ();
	case m_setup:
		return setup_cursor == 0 || setup_cursor == 1;
	case m_serialconfig:
		return serialConfig_cursor == 4;
	case m_modemconfig:
		return modemConfig_cursor == 1 || modemConfig_cursor == 2;
	case m_lanconfig:
		return lanConfig_cursor == 0 || lanConfig_cursor == 2;
	default:
		return false;
	}
}

void M_Keydown (int key)
{
	if (key == K_BACKSPACE && !M_BackspaceEdits ())
	{
	//
	// Backspace auto-repeats, where every other key's repeats are dropped
	// before they get here, so that holding it deletes a line of text. As
	// "back", a held key would empty the whole menu stack in a blink: one
	// level per press.
	//
		if (key_repeats[K_BACKSPACE] > 1)
			return;

		key = K_ESCAPE;
	}

	switch (m_state)
	{
	case m_none:
		return;

	case m_main:
		M_Main_Key (key);
		return;

	case m_singleplayer:
		M_SinglePlayer_Key (key);
		return;

	case m_load:
		M_Load_Key (key);
		return;

	case m_save:
		M_Save_Key (key);
		return;

	case m_multiplayer:
		M_MultiPlayer_Key (key);
		return;

	case m_setup:
		M_Setup_Key (key);
		return;

	case m_net:
		M_Net_Key (key);
		return;

	case m_options:
		M_Options_Key (key);
		return;

	case m_optpage:
		M_OptPage_Key (key);
		return;

	case m_keys:
		M_Keys_Key (key);
		return;

	case m_video:
		M_Video_Key (key);
		return;

	case m_game:
		M_Game_Key (key);
		return;

	case m_help:
		M_Help_Key (key);
		return;

	case m_quit:
		M_Quit_Key (key);
		return;

	case m_serialconfig:
		M_SerialConfig_Key (key);
		return;

	case m_modemconfig:
		M_ModemConfig_Key (key);
		return;

	case m_lanconfig:
		M_LanConfig_Key (key);
		return;

	case m_gameoptions:
		M_GameOptions_Key (key);
		return;

	case m_search:
		M_Search_Key (key);
		break;

	case m_slist:
		M_ServerList_Key (key);
		return;
	}
}


void M_ConfigureNetSubsystem(void)
{
// enable/disable net systems to match desired config

	Cbuf_AddText ("stopdemo\n");
	if (SerialConfig || DirectConfig)
	{
		Cbuf_AddText ("com1 enable\n");
	}

	if (IPXConfig || TCPIPConfig)
		net_hostport = lanConfig_port;
}

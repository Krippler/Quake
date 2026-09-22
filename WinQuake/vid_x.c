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
// vid_x.c -- general x video driver

#define _BSD


#include <ctype.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>

#include "quakedef.h"
#include "d_local.h"

cvar_t		_windowed_mouse = {"_windowed_mouse","0", true};
cvar_t		m_filter = {"m_filter","0", true};
float old_windowed_mouse;

qboolean        mouse_avail;
int             mouse_buttons=3;
int             mouse_oldbuttonstate;
int             mouse_buttonstate;
float   mouse_x, mouse_y;
float   old_mouse_x, old_mouse_y;
int p_mouse_x;
int p_mouse_y;
int ignorenext;
int bits_per_pixel;

typedef struct
{
	int input;
	int output;
} keymap_t;

viddef_t vid; // global video state
unsigned short d_8to16table[256];

int		num_shades=32;

int	d_con_indirect = 0;

int		vid_buffersize;

static qboolean			doShm;
static Display			*x_disp;
static Colormap			x_cmap;
static Window			x_win;
static GC				x_gc;
static Visual			*x_vis;
static XVisualInfo		*x_visinfo;
//static XImage			*x_image;

static int				x_shmeventtype;
//static XShmSegmentInfo	x_shminfo;

static qboolean			oktodraw = false;
static Atom				x_wm_delete_window;

int XShmQueryExtension(Display *);
int XShmGetEventBase(Display *);

int current_framebuffer;
static XImage			*x_framebuffer[2] = { 0, 0 };
static XShmSegmentInfo	x_shminfo[2];

static int verbose=0;

static byte current_palette[768];

static long X11_highhunkmark;
static long X11_buffersize;

int vid_surfcachesize;
void *vid_surfcache;

// menu.c defines these. vid_x.c defined them a second time and only
// -fcommon merged the two; say which file owns them.
extern void (*vid_menudrawfn)(void);
extern void (*vid_menukeyfn)(int key);

void VID_MenuDraw (void);
void VID_MenuKey (int key);

// vid.h declares a VID_SetMode with the DOS/Windows signature that nothing in
// this build implements, so the mode setter here has its own name.
static void VID_ApplyMode (int width, int height);
static void VID_ClampMode (int *width, int *height);

/*
================
VID_PixelAspect

The shape of one pixel, which is what the renderer means by vid.aspect: it
multiplies the vertical scale by it, so 1.0 is a square pixel and anything else
is a picture that has been stretched.

id computed it as (height / width) * (320 / 240), which cancels to a constant
4:3 whatever the mode is -- because in 1996 every mode was 4:3. 320x200 really
was displayed as 4:3, on a CRT that made the pixels taller than they were wide,
and this is the number that said so.

Every mode here is a framebuffer in a browser, where a pixel is square. Leaving
the 1996 formula in place made the renderer draw a 4:3 picture and the browser
show it at 16:9 -- a quarter wider than it should be, which is what
"widescreen resolutions appear stretched" is.
================
*/
static float VID_PixelAspect (void)
{
	return 1.0;
}

// As vid_win.c and vid_dos.c do: menu.h declares none of these.
extern void M_Menu_Options_f (void);
extern void M_Print (int cx, int cy, char *str);
extern void M_PrintWhite (int cx, int cy, char *str);
extern void M_DrawCharacter (int cx, int line, int num);
extern void M_DrawPic (int x, int y, qpic_t *pic);

// The resolution survives a restart, and the video menu sets these rather
// than switching the mode itself, so that a mode picked in the menu and a
// mode set from the console take exactly the same path.
cvar_t		vid_width = {"vid_width", "640", true};
cvar_t		vid_height = {"vid_height", "480", true};

static qboolean		x_randr;		// the extension is there and usable
static int			x_randr_event;	// its first event code
static qboolean		x_own_screen;	// -resizescreen: this process owns the
									// X server, so the screen may be resized
									// along with the window
static qboolean		x_resize_warned;
static int			x_error_seen;

static void		VID_InitRandr (void);
static void		VID_RootSize (int *width, int *height);
static qboolean	VID_SetScreenSize (int width, int height);

typedef unsigned short PIXEL16;
typedef unsigned int PIXEL24;	// 32 bits, matching the X server's stride
								// at depth 24 -- an unsigned long is eight bytes
								// here and overran every scanline
static PIXEL16 st2d_8to16table[256];
static PIXEL24 st2d_8to24table[256];
static int shiftmask_fl=0;
static long r_shift,g_shift,b_shift;
static unsigned long r_mask,g_mask,b_mask;

void shiftmask_init()
{
    unsigned int x;
    r_mask=x_vis->red_mask;
    g_mask=x_vis->green_mask;
    b_mask=x_vis->blue_mask;
    for(r_shift=-8,x=1;x<r_mask;x=x<<1)r_shift++;
    for(g_shift=-8,x=1;x<g_mask;x=x<<1)g_shift++;
    for(b_shift=-8,x=1;x<b_mask;x=x<<1)b_shift++;
    shiftmask_fl=1;
}

PIXEL16 xlib_rgb16(int r,int g,int b)
{
    PIXEL16 p;
    if(shiftmask_fl==0) shiftmask_init();
    p=0;

    if(r_shift>0) {
        p=(r<<(r_shift))&r_mask;
    } else if(r_shift<0) {
        p=(r>>(-r_shift))&r_mask;
    } else p|=(r&r_mask);

    if(g_shift>0) {
        p|=(g<<(g_shift))&g_mask;
    } else if(g_shift<0) {
        p|=(g>>(-g_shift))&g_mask;
    } else p|=(g&g_mask);

    if(b_shift>0) {
        p|=(b<<(b_shift))&b_mask;
    } else if(b_shift<0) {
        p|=(b>>(-b_shift))&b_mask;
    } else p|=(b&b_mask);

    return p;
}

PIXEL24 xlib_rgb24(int r,int g,int b)
{
    PIXEL24 p;
    if(shiftmask_fl==0) shiftmask_init();
    p=0;

    if(r_shift>0) {
        p=(r<<(r_shift))&r_mask;
    } else if(r_shift<0) {
        p=(r>>(-r_shift))&r_mask;
    } else p|=(r&r_mask);

    if(g_shift>0) {
        p|=(g<<(g_shift))&g_mask;
    } else if(g_shift<0) {
        p|=(g>>(-g_shift))&g_mask;
    } else p|=(g&g_mask);

    if(b_shift>0) {
        p|=(b<<(b_shift))&b_mask;
    } else if(b_shift<0) {
        p|=(b>>(-b_shift))&b_mask;
    } else p|=(b&b_mask);

    return p;
}

void st2_fixup( XImage *framebuf, int x, int y, int width, int height)
{
	int xi,yi;
	unsigned char *src;
	PIXEL16 *dest;
	register int count, n;

	if( (x<0)||(y<0) )return;

	for (yi = y; yi < (y+height); yi++) {
		src = &framebuf->data [yi * framebuf->bytes_per_line];

		// Duff's Device
		count = width;
		n = (count + 7) / 8;
		dest = ((PIXEL16 *)src) + x+width - 1;
		src += x+width - 1;

		switch (count % 8) {
		case 0:	do {	*dest-- = st2d_8to16table[*src--];
		case 7:			*dest-- = st2d_8to16table[*src--];
		case 6:			*dest-- = st2d_8to16table[*src--];
		case 5:			*dest-- = st2d_8to16table[*src--];
		case 4:			*dest-- = st2d_8to16table[*src--];
		case 3:			*dest-- = st2d_8to16table[*src--];
		case 2:			*dest-- = st2d_8to16table[*src--];
		case 1:			*dest-- = st2d_8to16table[*src--];
				} while (--n > 0);
		}

//		for(xi = (x+width-1); xi >= x; xi--) {
//			dest[xi] = st2d_8to16table[src[xi]];
//		}
	}
}

void st3_fixup( XImage *framebuf, int x, int y, int width, int height)
{
	int xi,yi;
	unsigned char *src;
	PIXEL24 *dest;
	register int count, n;

	if( (x<0)||(y<0) )return;

	for (yi = y; yi < (y+height); yi++) {
		src = &framebuf->data [yi * framebuf->bytes_per_line];

		// Duff's Device
		count = width;
		n = (count + 7) / 8;
		dest = ((PIXEL24 *)src) + x+width - 1;
		src += x+width - 1;

		switch (count % 8) {
		case 0:	do {	*dest-- = st2d_8to24table[*src--];
		case 7:			*dest-- = st2d_8to24table[*src--];
		case 6:			*dest-- = st2d_8to24table[*src--];
		case 5:			*dest-- = st2d_8to24table[*src--];
		case 4:			*dest-- = st2d_8to24table[*src--];
		case 3:			*dest-- = st2d_8to24table[*src--];
		case 2:			*dest-- = st2d_8to24table[*src--];
		case 1:			*dest-- = st2d_8to24table[*src--];
				} while (--n > 0);
		}

//		for(xi = (x+width-1); xi >= x; xi--) {
//			dest[xi] = st2d_8to16table[src[xi]];
//		}
	}
}


// ========================================================================
// Tragic death handler
// ========================================================================

// The original closed the X display and called Sys_Error from inside the
// signal handler. Sys_Error runs Host_Shutdown, which writes config.cfg,
// shuts the sound down and calls back into Xlib -- on a connection this
// handler has just closed, from a context where none of malloc, stdio or
// Xlib may be called at all. It got away with it when the signal arrived at
// an idle moment and segfaulted when it did not, which under `docker stop`
// is most of the time: SIGTERM at a random instruction, and the config the
// player just changed is lost.
//
// Raise a flag and let the frame loop see it. That is the only thing a
// handler may safely do, and it means the shutdown happens on the main stack
// with everything still valid.
volatile sig_atomic_t	sys_signalquit = 0;

void TragicDeath(int signal_num)
{
	sys_signalquit = signal_num;
}

// ========================================================================
// makes a null cursor
// ========================================================================

static Cursor CreateNullCursor(Display *display, Window root)
{
    Pixmap cursormask; 
    XGCValues xgc;
    GC gc;
    XColor dummycolour;
    Cursor cursor;

    cursormask = XCreatePixmap(display, root, 1, 1, 1/*depth*/);
    xgc.function = GXclear;
    gc =  XCreateGC(display, cursormask, GCFunction, &xgc);
    XFillRectangle(display, cursormask, gc, 0, 0, 1, 1);
    dummycolour.pixel = 0;
    dummycolour.red = 0;
    dummycolour.flags = 04;
    cursor = XCreatePixmapCursor(display, cursormask, cursormask,
          &dummycolour,&dummycolour, 0,0);
    XFreePixmap(display,cursormask);
    XFreeGC(display,gc);
    return cursor;
}

void ResetFrameBuffer(void)
{
	int mem;
	int pwidth;

	if (x_framebuffer[0])
	{
		free(x_framebuffer[0]->data);
		free(x_framebuffer[0]);
	}

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (X11_highhunkmark);
		d_pzbuffer = NULL;
	}
	X11_highhunkmark = Hunk_HighMark ();

// alloc an extra line in case we want to wrap, and allocate the z-buffer
	X11_buffersize = vid.width * vid.height * sizeof (*d_pzbuffer);

	vid_surfcachesize = D_SurfaceCacheForRes (vid.width, vid.height);

	X11_buffersize += vid_surfcachesize;

	d_pzbuffer = Hunk_HighAllocName (X11_buffersize, "video");
	if (d_pzbuffer == NULL)
		Sys_Error ("Not enough memory for video mode\n");

	vid_surfcache = (byte *) d_pzbuffer
		+ vid.width * vid.height * sizeof (*d_pzbuffer);

	D_InitCaches(vid_surfcache, vid_surfcachesize);

	pwidth = x_visinfo->depth / 8;
	if (pwidth == 3) pwidth = 4;
	mem = ((vid.width*pwidth+7)&~7) * vid.height;

	{
		char	*fbmem = malloc (mem);

		if (!fbmem)
			Sys_Error ("VID: out of memory for a %d byte framebuffer\n", mem);

		x_framebuffer[0] = XCreateImage(	x_disp,
			x_vis,
			x_visinfo->depth,
			ZPixmap,
			0,
			fbmem,
			vid.width, vid.height,
			32,
			0);

		if (!x_framebuffer[0])
			Sys_Error("VID: XCreateImage failed\n");
	}

// This said (byte *)x_framebuffer[0], which is the XImage header, not the
// pixels behind it. Every frame the renderer drew overwrote the structure
// describing where to draw, and the first XPutImage read width, height and
// stride back out of whatever the title screen happened to put there. The
// shared-memory path below is the one the original ever ran, which is why it
// survived to the source release.
	vid.buffer = (pixel_t *) x_framebuffer[0]->data;
	vid.conbuffer = vid.buffer;

}

void ResetSharedFrameBuffers(void)
{

	int size;
	int key;
	int minsize = getpagesize();
	int frm;

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (X11_highhunkmark);
		d_pzbuffer = NULL;
	}

	X11_highhunkmark = Hunk_HighMark ();

// alloc an extra line in case we want to wrap, and allocate the z-buffer
	X11_buffersize = vid.width * vid.height * sizeof (*d_pzbuffer);

	vid_surfcachesize = D_SurfaceCacheForRes (vid.width, vid.height);

	X11_buffersize += vid_surfcachesize;

	d_pzbuffer = Hunk_HighAllocName (X11_buffersize, "video");
	if (d_pzbuffer == NULL)
		Sys_Error ("Not enough memory for video mode\n");

	vid_surfcache = (byte *) d_pzbuffer
		+ vid.width * vid.height * sizeof (*d_pzbuffer);

	D_InitCaches(vid_surfcache, vid_surfcachesize);

	for (frm=0 ; frm<2 ; frm++)
	{

	// free up old frame buffer memory

		if (x_framebuffer[frm])
		{
			XShmDetach(x_disp, &x_shminfo[frm]);
			free(x_framebuffer[frm]);
			shmdt(x_shminfo[frm].shmaddr);
		}

	// create the image

		x_framebuffer[frm] = XShmCreateImage(	x_disp,
						x_vis,
						x_visinfo->depth,
						ZPixmap,
						0,
						&x_shminfo[frm],
						vid.width,
						vid.height );

	// grab shared memory

		size = x_framebuffer[frm]->bytes_per_line
			* x_framebuffer[frm]->height;
		if (size < minsize)
			Sys_Error("VID: Window must use at least %d bytes\n", minsize);

// IPC_PRIVATE, not a random key: random() collides, and a collision here
// hands you somebody else's segment rather than failing. 0600 rather than
// 0777 for the same reason -- these are this process's pixels.
		key = IPC_PRIVATE;
		x_shminfo[frm].shmid = shmget((key_t)key, size, IPC_CREAT|0600);
		if (x_shminfo[frm].shmid==-1)
			Sys_Error("VID: Could not get any shared memory (%s)\n",
					  strerror(errno));

		// attach to the shared memory segment
		x_shminfo[frm].shmaddr =
			(void *) shmat(x_shminfo[frm].shmid, 0, 0);

		if (x_shminfo[frm].shmaddr == (void *)-1)
			Sys_Error("VID: Could not attach shared memory (%s)\n",
					  strerror(errno));

		if (verbose)
			printf("VID: shared memory id=%d, addr=%p\n",
				   x_shminfo[frm].shmid, x_shminfo[frm].shmaddr);

		x_framebuffer[frm]->data = x_shminfo[frm].shmaddr;

	// get the X server to attach to it

		if (!XShmAttach(x_disp, &x_shminfo[frm]))
			Sys_Error("VID: XShmAttach() failed\n");
		XSync(x_disp, 0);
		shmctl(x_shminfo[frm].shmid, IPC_RMID, 0);

	}

}

// Called at startup to set up translation tables, takes 256 8 bit RGB values
// the palette data will go away after the call, so it must be copied off if
// the video driver will need it again

void	VID_Init (unsigned char *palette)
{

   int pnum, i;
   XVisualInfo template;
   int num_visuals;
   int template_mask;
   
   ignorenext=0;
   vid.width = 320;
   vid.height = 200;
   vid.maxwarpwidth = WARP_WIDTH;
   vid.maxwarpheight = WARP_HEIGHT;
   vid.numpages = 2;
   vid.colormap = host_colormap;
   //	vid.cbits = VID_CBITS;
   //	vid.grades = VID_GRADES;
   vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
   
	srandom(getpid());

	verbose=COM_CheckParm("-verbose");

// open the display
	x_disp = XOpenDisplay(0);
	if (!x_disp)
	{
		if (getenv("DISPLAY"))
			Sys_Error("VID: Could not open display [%s]\n",
				getenv("DISPLAY"));
		else
			Sys_Error("VID: Could not open local display\n");
	}

// catch signals so i can turn on auto-repeat

	{
		struct sigaction sa;

	// sigaction(SIGINT, 0, &sa) reads the current disposition and the rest of
	// sa is then whatever came back, flags included. Start from a known state.
		memset (&sa, 0, sizeof(sa));
		sa.sa_handler = TragicDeath;
		sigemptyset (&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sigaction(SIGINT, &sa, 0);
		sigaction(SIGTERM, &sa, 0);
		sigaction(SIGHUP, &sa, 0);
	}

	XAutoRepeatOff(x_disp);

// The 1996 sources left this on with "for debugging only" written above it,
// which makes every Xlib call a round trip to the server and waits for the
// reply. Over a socket to a VNC-backed Xvfb that is the difference between a
// playable picture and a slideshow. -verbose puts it back, because a protocol
// error is otherwise reported against whatever call happens to be in flight
// rather than the one that caused it.
	if (verbose)
		XSynchronize(x_disp, True);

// check for command-line window size
	if ((pnum=COM_CheckParm("-winsize")))
	{
		if (pnum >= com_argc-2)
			Sys_Error("VID: -winsize <width> <height>\n");
		vid.width = Q_atoi(com_argv[pnum+1]);
		vid.height = Q_atoi(com_argv[pnum+2]);
		if (!vid.width || !vid.height)
			Sys_Error("VID: Bad window width/height\n");
	}
	if ((pnum=COM_CheckParm("-width"))) {
		if (pnum >= com_argc-1)
			Sys_Error("VID: -width <width>\n");
		vid.width = Q_atoi(com_argv[pnum+1]);
		if (!vid.width)
			Sys_Error("VID: Bad window width\n");
	}
	if ((pnum=COM_CheckParm("-height"))) {
		if (pnum >= com_argc-1)
			Sys_Error("VID: -height <height>\n");
		vid.height = Q_atoi(com_argv[pnum+1]);
		if (!vid.height)
			Sys_Error("VID: Bad window height\n");
	}

// The span drawers step the framebuffer eight pixels at a time and the
// renderer's static tables are sized by MAXWIDTH and MAXHEIGHT. The original
// checked neither, so a width the caller picked freely either drew a sheared
// picture or wrote off the end of d_scantable. Say what happened rather than
// crashing three frames later.
	if (vid.width < 320)
		vid.width = 320;
	if (vid.height < 200)
		vid.height = 200;
	if (vid.width > MAXWIDTH || vid.height > MAXHEIGHT)
	{
		Con_Printf ("VID: %dx%d is larger than this build's %dx%d limit\n",
					vid.width, vid.height, MAXWIDTH, MAXHEIGHT);
		if (vid.width > MAXWIDTH)
			vid.width = MAXWIDTH;
		if (vid.height > MAXHEIGHT)
			vid.height = MAXHEIGHT;
	}
	vid.width &= ~7;

// Registered here so -winsize/-width/-height act as the default and a
// vid_width in config.cfg overrides it on the first frame -- which is what
// makes a resolution picked in the video menu survive a restart.
	Cvar_RegisterVariable (&vid_width);
	Cvar_RegisterVariable (&vid_height);

	VID_InitRandr ();
	VID_ClampMode (&vid.width, &vid.height);

// Before the window is created, so it is created at the right size. The
// entrypoint starts Xvfb at the largest mode this build can draw, because a
// server's maximum screen size is fixed when it starts.
	if (x_own_screen && !VID_SetScreenSize (vid.width, vid.height))
	{
		int	root_width, root_height;

		VID_RootSize (&root_width, &root_height);
		if (root_width != vid.width || root_height != vid.height)
		{
		// -resizescreen says nothing else is using this display, so a window
		// that does not fill the screen leaves the rest of the picture black.
		// Take the screen as it is instead, and say why the video menu will
		// not be able to change it.
			Con_Printf ("VID: this X server will not resize its screen;"
						" using %dx%d\n"
						"     as it stands. Video Options cannot change it.\n",
						root_width, root_height);
			vid.width = root_width;
			vid.height = root_height;
			VID_ClampMode (&vid.width, &vid.height);
			x_resize_warned = true;
		}
	}

	Cvar_SetValue ("vid_width", vid.width);
	Cvar_SetValue ("vid_height", vid.height);

	template_mask = 0;

// specify a visual id
	if ((pnum=COM_CheckParm("-visualid")))
	{
		if (pnum >= com_argc-1)
			Sys_Error("VID: -visualid <id#>\n");
		template.visualid = Q_atoi(com_argv[pnum+1]);
		template_mask = VisualIDMask;
	}

// If not specified, use default visual
	else
	{
		int screen;
		screen = XDefaultScreen(x_disp);
		template.visualid =
			XVisualIDFromVisual(XDefaultVisual(x_disp, screen));
		template_mask = VisualIDMask;
	}

// pick a visual- warn if more than one was available
	x_visinfo = XGetVisualInfo(x_disp, template_mask, &template, &num_visuals);
	if (num_visuals > 1)
	{
		printf("Found more than one visual id at depth %d:\n", template.depth);
		for (i=0 ; i<num_visuals ; i++)
			printf("	-visualid %d\n", (int)(x_visinfo[i].visualid));
	}
	else if (num_visuals == 0)
	{
		if (template_mask == VisualIDMask)
			Sys_Error("VID: Bad visual id %d\n", template.visualid);
		else
			Sys_Error("VID: No visuals at depth %d\n", template.depth);
	}

	if (verbose)
	{
		printf("Using visualid %d:\n", (int)(x_visinfo->visualid));
		printf("	screen %d\n", x_visinfo->screen);
		printf("	red_mask 0x%x\n", (int)(x_visinfo->red_mask));
		printf("	green_mask 0x%x\n", (int)(x_visinfo->green_mask));
		printf("	blue_mask 0x%x\n", (int)(x_visinfo->blue_mask));
		printf("	colormap_size %d\n", x_visinfo->colormap_size);
		printf("	bits_per_rgb %d\n", x_visinfo->bits_per_rgb);
	}

	x_vis = x_visinfo->visual;

// setup attributes for main window
	{
	   int attribmask = CWEventMask  | CWColormap | CWBorderPixel;
	   XSetWindowAttributes attribs;
	   Colormap tmpcmap;
	   
	   tmpcmap = XCreateColormap(x_disp, XRootWindow(x_disp,
							 x_visinfo->screen), x_vis, AllocNone);
	   
           attribs.event_mask = StructureNotifyMask | KeyPressMask
	     | KeyReleaseMask | ExposureMask | PointerMotionMask |
	     ButtonPressMask | ButtonReleaseMask;
	   attribs.border_pixel = 0;
	   attribs.colormap = tmpcmap;

// create the main window
		x_win = XCreateWindow(	x_disp,
			XRootWindow(x_disp, x_visinfo->screen),
			0, 0,	// x, y
			vid.width, vid.height,
			0, // borderwidth
			x_visinfo->depth,
			InputOutput,
			x_vis,
			attribmask,
			&attribs );
		XStoreName( x_disp,x_win,"xquake");


		if (x_visinfo->class != TrueColor)
			XFreeColormap(x_disp, tmpcmap);
	}

	if (x_visinfo->depth == 8)
	{
		// create and upload the palette
		if (x_visinfo->class == PseudoColor)
		{
			x_cmap = XCreateColormap(x_disp, x_win, x_vis, AllocAll);
			VID_SetPalette(palette);
			XSetWindowColormap(x_disp, x_win, x_cmap);
		// Setting the window's colormap only states a preference; something
		// has to install it in the hardware, and on a desktop that is the
		// window manager's job. There is no window manager here -- the
		// container runs one Xvfb and one window -- so nothing installed it
		// and everything reading the display got the root's default map
		// instead: the right palette indices through the wrong 256 colours.
			XInstallColormap(x_disp, x_cmap);
		}
		else
		{
		// Depth 8 without a writable colormap means the palette cannot be
		// uploaded, and the renderer's output is palette indices. The picture
		// would come out as whatever 256 colours the server chose.
			Sys_Error ("VID: the 8-bit visual is %s, not PseudoColor; the\n"
					   "palette cannot be set. Start the X server with a\n"
					   "PseudoColor visual at depth 8.\n",
					   x_visinfo->class == StaticColor ? "StaticColor"
													   : "GrayScale/other");
		}
	}
	else
	{
	// st2_fixup and st3_fixup expand each frame through a lookup table built
	// from the same palette, rebuilt whenever the palette changes -- which
	// includes the damage flash and the underwater tint. It costs a pass over
	// every pixel and no colormap at all, which is the trade the container
	// wants: a colormap has to be read back out of the window by whatever is
	// watching the screen, and that is where wrong-colour pictures came from.
		Con_Printf ("VID: depth %d visual; translating each frame from 8-bit.\n",
					x_visinfo->depth);
	}

// Closing the window should reach the engine rather than killing the X
// connection underneath it.
	x_wm_delete_window = XInternAtom (x_disp, "WM_DELETE_WINDOW", False);
	XSetWMProtocols (x_disp, x_win, &x_wm_delete_window, 1);

// inviso cursor
	XDefineCursor(x_disp, x_win, CreateNullCursor(x_disp, x_win));

// create the GC
	{
		XGCValues xgcvalues;
		int valuemask = GCGraphicsExposures;
		xgcvalues.graphics_exposures = False;
		x_gc = XCreateGC(x_disp, x_win, valuemask, &xgcvalues );
	}

// map the window
	XMapWindow(x_disp, x_win);

// wait for first exposure event
	{
		XEvent event;
		do
		{
			XNextEvent(x_disp, &event);
			if (event.type == Expose && !event.xexpose.count)
				oktodraw = true;
		} while (!oktodraw);
	}
// now safe to draw

// even if MITSHM is available, make sure it's a local connection
	if (XShmQueryExtension(x_disp))
	{
		const char *displayname;

		doShm = true;
		displayname = getenv("DISPLAY");
		if (displayname)
		{
		// The host part is everything before the colon. The original wrote a
		// nul over that colon -- in the string getenv handed back, which is
		// the environment itself, so DISPLAY became empty for this process
		// and everything it went on to exec. Read it without writing to it.
			char	host[64];
			size_t	n = 0;

			while (displayname[n] && displayname[n] != ':'
				   && n < sizeof(host) - 1)
			{
				host[n] = displayname[n];
				n++;
			}
			host[n] = 0;

			if (!(!strcasecmp(host, "unix") || !host[0]))
				doShm = false;
		}
	}

	if (COM_CheckParm("-noshm"))
		doShm = false;

	if (doShm)
	{
		x_shmeventtype = XShmGetEventBase(x_disp) + ShmCompletion;
		ResetSharedFrameBuffers();
	}
	else
		ResetFrameBuffer();

	current_framebuffer = 0;
	vid.rowbytes = x_framebuffer[0]->bytes_per_line;
	vid.buffer = x_framebuffer[0]->data;
	vid.direct = 0;
	vid.conbuffer = x_framebuffer[0]->data;
	vid.conrowbytes = vid.rowbytes;
	vid.conwidth = vid.width;
	vid.conheight = vid.height;
	vid.aspect = VID_PixelAspect ();

// menu.c hides the Video Options line when these are null, which is why the
// X build never had one. It has the whole mode list now.
	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

//	XSynchronize(x_disp, False);

}

void VID_ShiftPalette(unsigned char *p)
{
	VID_SetPalette(p);
}



void VID_SetPalette(unsigned char *palette)
{

	int i;
	XColor colors[256];

	for(i=0;i<256;i++) {
		st2d_8to16table[i]= xlib_rgb16(palette[i*3], palette[i*3+1],palette[i*3+2]);
		st2d_8to24table[i]= xlib_rgb24(palette[i*3], palette[i*3+1],palette[i*3+2]);
	}

	if (x_visinfo->class == PseudoColor && x_visinfo->depth == 8)
	{
		if (palette != current_palette)
			memcpy(current_palette, palette, 768);
		for (i=0 ; i<256 ; i++)
		{
			colors[i].pixel = i;
			colors[i].flags = DoRed|DoGreen|DoBlue;
			colors[i].red = palette[i*3] * 257;
			colors[i].green = palette[i*3+1] * 257;
			colors[i].blue = palette[i*3+2] * 257;
		}
		XStoreColors(x_disp, x_cmap, colors, 256);
	}

}

// Called at shutdown

void	VID_Shutdown (void)
{
	Con_Printf("VID_Shutdown\n");

// Sys_Error calls Host_Shutdown, which calls this -- and VID_Init calls
// Sys_Error when it cannot open the display, which is the one case where
// there is no display to close. XAutoRepeatOn then dereferenced a null
// Display and the engine died on SIGSEGV while it was in the middle of
// printing why it was stopping.
//
// The message was already out ("VID: Could not open display"), but what the
// container saw afterwards was a segfault, so it reported a crash, restarted,
// crashed the same way, and gave up three runs later with the signal in the
// log and the reason above it looking like part of the previous run.
	if (!x_disp)
		return;

	XAutoRepeatOn(x_disp);
	XCloseDisplay(x_disp);
	x_disp = NULL;
}

int XLateKey(XKeyEvent *ev)
{

	int key;
	char buf[64];
	KeySym keysym;
	XKeyEvent unshifted;

	key = 0;

// Look the keysym up with shift taken out of the event's modifier state.
//
// Quake wants the unshifted key. keys.h says so -- "normal keys should be
// passed as lowercased ascii" -- keys.c owns the shift table that turns '-'
// into '_' for the console, and a binding belongs to a physical key rather
// than to the character it happens to produce.
//
// Taking the keysym with shift applied also made the press and the release
// disagree: shift+minus arrived as '_' going down and, because shift is
// already up by then, as '-' coming up. Key_Event counts autorepeats in
// key_repeats[key] and only clears the entry on the release, so key_repeats
// ['_'] went to 1 and stayed there. The first shifted character of a session
// went through and every one after it was dropped as an autorepeat -- every
// capital letter, colon, quote and underscore, for the whole run. The
// disabled block of hand-written cases further down was the 1996 attempt at
// the same problem.
	unshifted = *ev;
	unshifted.state &= ~(ShiftMask | LockMask);

	XLookupString(&unshifted, buf, sizeof buf, &keysym, 0);

	switch(keysym)
	{
		case XK_KP_Page_Up:
		case XK_Page_Up:	 key = K_PGUP; break;

		case XK_KP_Page_Down:
		case XK_Page_Down:	 key = K_PGDN; break;

		case XK_KP_Home:
		case XK_Home:	 key = K_HOME; break;

		case XK_KP_End:
		case XK_End:	 key = K_END; break;

		case XK_KP_Left:
		case XK_Left:	 key = K_LEFTARROW; break;

		case XK_KP_Right:
		case XK_Right:	key = K_RIGHTARROW;		break;

		case XK_KP_Down:
		case XK_Down:	 key = K_DOWNARROW; break;

		case XK_KP_Up:
		case XK_Up:		 key = K_UPARROW;	 break;

		case XK_Escape: key = K_ESCAPE;		break;

		case XK_KP_Enter:
		case XK_Return: key = K_ENTER;		 break;

		case XK_Tab:		key = K_TAB;			 break;

		case XK_F1:		 key = K_F1;				break;

		case XK_F2:		 key = K_F2;				break;

		case XK_F3:		 key = K_F3;				break;

		case XK_F4:		 key = K_F4;				break;

		case XK_F5:		 key = K_F5;				break;

		case XK_F6:		 key = K_F6;				break;

		case XK_F7:		 key = K_F7;				break;

		case XK_F8:		 key = K_F8;				break;

		case XK_F9:		 key = K_F9;				break;

		case XK_F10:		key = K_F10;			 break;

		case XK_F11:		key = K_F11;			 break;

		case XK_F12:		key = K_F12;			 break;

		case XK_BackSpace: key = K_BACKSPACE; break;

		case XK_KP_Delete:
		case XK_Delete: key = K_DEL; break;

		case XK_Pause:	key = K_PAUSE;		 break;

		case XK_Shift_L:
		case XK_Shift_R:	key = K_SHIFT;		break;

		case XK_Execute: 
		case XK_Control_L: 
		case XK_Control_R:	key = K_CTRL;		 break;

		case XK_Alt_L:	
		case XK_Meta_L: 
		case XK_Alt_R:	
		case XK_Meta_R: key = K_ALT;			break;

		case XK_KP_Begin: key = K_AUX30;	break;

		case XK_Insert:
		case XK_KP_Insert: key = K_INS; break;

		case XK_KP_Multiply: key = '*'; break;
		case XK_KP_Add: key = '+'; break;
		case XK_KP_Subtract: key = '-'; break;
		case XK_KP_Divide: key = '/'; break;

#if 0
		case 0x021: key = '1';break;/* [!] */
		case 0x040: key = '2';break;/* [@] */
		case 0x023: key = '3';break;/* [#] */
		case 0x024: key = '4';break;/* [$] */
		case 0x025: key = '5';break;/* [%] */
		case 0x05e: key = '6';break;/* [^] */
		case 0x026: key = '7';break;/* [&] */
		case 0x02a: key = '8';break;/* [*] */
		case 0x028: key = '9';;break;/* [(] */
		case 0x029: key = '0';break;/* [)] */
		case 0x05f: key = '-';break;/* [_] */
		case 0x02b: key = '=';break;/* [+] */
		case 0x07c: key = '\'';break;/* [|] */
		case 0x07d: key = '[';break;/* [}] */
		case 0x07b: key = ']';break;/* [{] */
		case 0x022: key = '\'';break;/* ["] */
		case 0x03a: key = ';';break;/* [:] */
		case 0x03f: key = '/';break;/* [?] */
		case 0x03e: key = '.';break;/* [>] */
		case 0x03c: key = ',';break;/* [<] */
#endif

		default:
			key = *(unsigned char*)buf;
			if (key >= 'A' && key <= 'Z')
				key = key - 'A' + 'a';
//			fprintf(stdout, "case 0x0%x: key = ___;break;/* [%c] */\n", keysym);
			break;
	} 

	return key;
}

struct
{
	int key;
	int down;
} keyq[64];
int keyq_head=0;
int keyq_tail=0;

/*
================
Keyq_Add

Everything that turns into a key goes through here, and Sys_SendKeyEvents
dispatches at most one ring's worth per frame. That bound matters: Key_Event
writes the key's binding into the command buffer, and the command buffer is
only drained once per frame by Cbuf_Execute. Calling Key_Event straight from
the event loop -- which the wheel handling below used to do -- lets one frame
feed it an unbounded amount of text, and a stall long enough to queue a few
hundred notches then fills the 8 KB buffer and turns the console into a column
of "Cbuf_AddText: overflow".
================
*/
static void Keyq_Add (int key, qboolean down)
{
	keyq[keyq_head].key = key;
	keyq[keyq_head].down = down;
	keyq_head = (keyq_head + 1) & 63;
}

int config_notify=0;
int config_notify_width;
int config_notify_height;
						      
void GetEvent(void)
{
	XEvent x_event;
	int b;
   
	XNextEvent(x_disp, &x_event);
	switch(x_event.type) {
	case KeyPress:
		Keyq_Add (XLateKey(&x_event.xkey), true);
		break;
	case KeyRelease:
		Keyq_Add (XLateKey(&x_event.xkey), false);
		break;

	case MotionNotify:
	// A report landing exactly on the centre of the window is a re-base, not
	// a movement. The browser client walks the remote pointer around the
	// screen by sending absolute positions -- that is all the VNC protocol
	// carries -- and puts it back in the middle before it would hit an edge.
	// Measuring the delta from the previous position would read that jump
	// back to the middle as a hard flick in the opposite direction.
		if (!_windowed_mouse.value
			&& x_event.xmotion.x == (int)(vid.width/2)
			&& x_event.xmotion.y == (int)(vid.height/2))
		{
			p_mouse_x = x_event.xmotion.x;
			p_mouse_y = x_event.xmotion.y;
			break;
		}

		if (_windowed_mouse.value) {
			mouse_x = (float) ((int)x_event.xmotion.x - (int)(vid.width/2));
			mouse_y = (float) ((int)x_event.xmotion.y - (int)(vid.height/2));
//printf("m: x=%d,y=%d, mx=%3.2f,my=%3.2f\n", 
//	x_event.xmotion.x, x_event.xmotion.y, mouse_x, mouse_y);

			/* move the mouse to the window center again */
			XSelectInput(x_disp,x_win,StructureNotifyMask|KeyPressMask
				|KeyReleaseMask|ExposureMask
				|ButtonPressMask
				|ButtonReleaseMask);
			XWarpPointer(x_disp,None,x_win,0,0,0,0, 
				(vid.width/2),(vid.height/2));
			XSelectInput(x_disp,x_win,StructureNotifyMask|KeyPressMask
				|KeyReleaseMask|ExposureMask
				|PointerMotionMask|ButtonPressMask
				|ButtonReleaseMask);
		} else {
			mouse_x = (float) (x_event.xmotion.x-p_mouse_x);
			mouse_y = (float) (x_event.xmotion.y-p_mouse_y);
			p_mouse_x=x_event.xmotion.x;
			p_mouse_y=x_event.xmotion.y;
		}
		break;

//
// X delivers the wheel as buttons 4 and 5, one press and one release per
// notch. The 1996 code knew about three buttons and dropped the rest, so the
// wheel did nothing -- and the engine has had K_MWHEELUP and K_MWHEELDOWN in
// keys.c the whole time, waiting for something to send them.
//
// Straight to Key_Event rather than through mouse_buttonstate: a notch is
// momentary, and IN_Commands only reports a change between frames, so a press
// and release inside one frame would cancel out and never be seen.
//
	case ButtonPress:
		if (x_event.xbutton.button == 4)
		{
			Keyq_Add (K_MWHEELUP, true);
			break;
		}
		if (x_event.xbutton.button == 5)
		{
			Keyq_Add (K_MWHEELDOWN, true);
			break;
		}
		b=-1;
		if (x_event.xbutton.button == 1)
			b = 0;
		else if (x_event.xbutton.button == 2)
			b = 2;
		else if (x_event.xbutton.button == 3)
			b = 1;
		if (b>=0)
			mouse_buttonstate |= 1<<b;
		break;

	case ButtonRelease:
		if (x_event.xbutton.button == 4)
		{
			Keyq_Add (K_MWHEELUP, false);
			break;
		}
		if (x_event.xbutton.button == 5)
		{
			Keyq_Add (K_MWHEELDOWN, false);
			break;
		}
		b=-1;
		if (x_event.xbutton.button == 1)
			b = 0;
		else if (x_event.xbutton.button == 2)
			b = 2;
		else if (x_event.xbutton.button == 3)
			b = 1;
		if (b>=0)
			mouse_buttonstate &= ~(1<<b);
		break;
	
	case ConfigureNotify:
//printf("config notify\n");
		config_notify_width = x_event.xconfigure.width;
		config_notify_height = x_event.xconfigure.height;
		config_notify = 1;
		break;

	case MappingNotify:
	// Xlib caches the keyboard mapping when the connection opens and only
	// rereads it when asked. x11vnc types a character the keymap does not
	// have by binding it to a spare keycode, sending it and putting the
	// keymap back, so without this those characters arrive as whatever the
	// stale cache says that keycode used to mean.
		XRefreshKeyboardMapping (&x_event.xmapping);
		break;

	case ClientMessage:
	// Closing the window used to end the process from inside Xlib's default
	// I/O error handler, with the connection already gone -- so no config
	// was written and no savegame flushed. Ask for WM_DELETE_WINDOW instead
	// and quit through the engine.
		if ((Atom)x_event.xclient.data.l[0] == x_wm_delete_window)
			Sys_Quit ();
		break;

	default:
	// Keeps Xlib's cached screen dimensions in step with the resizes
	// VID_SetScreenSize makes; nothing else reads them, but a stale value is
	// the kind of thing that costs an afternoon later.
		if (x_randr && x_event.type == x_randr_event + RRScreenChangeNotify)
		{
			XRRUpdateConfiguration (&x_event);
			break;
		}
		if (doShm && x_event.type == x_shmeventtype)
			oktodraw = true;
	}
   
	if (old_windowed_mouse != _windowed_mouse.value) {
		old_windowed_mouse = _windowed_mouse.value;

		if (!_windowed_mouse.value) {
			/* ungrab the pointer */
			XUngrabPointer(x_disp,CurrentTime);
		} else {
			/* grab the pointer */
			XGrabPointer(x_disp,x_win,True,0,GrabModeAsync,
				GrabModeAsync,x_win,None,CurrentTime);
		}
	}
}

/*
================================================================================

VIDEO MODES

The X server in the container is a private Xvfb with one window on it, and the
picture the browser sees is the whole root window. So changing resolution means
changing the size of the screen itself, not just the window -- otherwise the
window sits in the corner of a framebuffer it cannot fill and the rest stays
black.

RANDR can do that, with two catches. A server's maximum screen size is fixed
when it starts, so the entrypoint starts Xvfb at the largest mode this build
can draw and every mode below it is reached by shrinking. And Xvfb starts
knowing exactly one mode -- the size it was given -- so every other resolution
has to be created here before a CRTC will take it.

Only done when -resizescreen says this process owns the server. On someone's
desktop, picking a resolution in Quake's menu has no business rearranging the
screen their other windows are on; there the window alone changes size.

================================================================================
*/

typedef struct
{
	int		width;
	int		height;
} vmode_t;

// Bounded above by MAXWIDTH/MAXHEIGHT from r_shared.h, which size the
// renderer's static tables; every width is a multiple of eight because the
// span drawers step the framebuffer eight pixels at a time.
static vmode_t	vid_modes[] =
{
	{  320,  240 },
	{  400,  300 },
	{  512,  384 },
	{  640,  400 },
	{  640,  480 },
	{  800,  600 },
	{  960,  720 },
	{ 1024,  768 },
	{ 1152,  864 },
	{ 1280,  720 },
	{ 1280,  800 },
	{ 1280,  960 },
	{ 1280, 1024 },
	{ 1360,  768 },
	{ 1440,  900 },
	{ 1600,  900 },
	{ 1600, 1200 },
	{ 1680, 1050 },
	{ 1920, 1080 },
	{ 1920, 1200 },
};

#define	NUM_VID_MODES	(sizeof(vid_modes) / sizeof(vid_modes[0]))

// How large a mode the server will accept. Offering one it will refuse only
// produces a mode change that silently does not happen.
static int		vid_maxscreenwidth = MAXWIDTH;
static int		vid_maxscreenheight = MAXHEIGHT;

/*
================
VID_XErrorTrap

RRAddOutputMode answers a mode larger than the screen maximum with BadMatch,
and RRSetCrtcConfig answers one that will not fit with BadValue. Xlib's default
handler prints those and calls exit(), which would end the game over a menu
selection. Count them and carry on; the caller checks whether the screen
actually changed size rather than trusting the request.
================
*/
static int VID_XErrorTrap (Display *disp, XErrorEvent *err)
{
	x_error_seen++;
	return 0;
}

/*
================
VID_RootSize

Asks the server. DisplayWidth/DisplayHeight read a value Xlib cached when the
connection opened, and it does not follow a screen resize unless every
RRScreenChangeNotify is fed to XRRUpdateConfiguration -- so a resize that
worked perfectly well reads back as no change at all.
================
*/
static void VID_RootSize (int *width, int *height)
{
	Window			root, dummy;
	int				x, y;
	unsigned int	w, h, border, depth;

	root = XDefaultRootWindow (x_disp);
	if (XGetGeometry (x_disp, root, &dummy, &x, &y, &w, &h, &border, &depth))
	{
		*width = w;
		*height = h;
	}
	else
	{
		*width = vid.width;
		*height = vid.height;
	}
}

/*
================
VID_InitRandr
================
*/
static void VID_InitRandr (void)
{
	int		error_base, major, minor;
	int		minw, minh, maxw, maxh;

	x_own_screen = COM_CheckParm ("-resizescreen") != 0;

	if (!XRRQueryExtension (x_disp, &x_randr_event, &error_base)
		|| !XRRQueryVersion (x_disp, &major, &minor))
		return;

// XRRSetCrtcConfig and the mode-creation calls are 1.2. The 1.1 interface
// (XRRSizes/XRRSetScreenConfig) reports success on Xvfb and leaves the screen
// where it was, so there is no point falling back to it.
	if (major < 1 || (major == 1 && minor < 2))
		return;

	x_randr = true;

	if (XRRGetScreenSizeRange (x_disp, XDefaultRootWindow (x_disp),
							   &minw, &minh, &maxw, &maxh))
	{
		if (maxw > 0 && maxw < vid_maxscreenwidth)
			vid_maxscreenwidth = maxw;
		if (maxh > 0 && maxh < vid_maxscreenheight)
			vid_maxscreenheight = maxh;
	}

	if (verbose)
		Con_Printf ("VID: RANDR %d.%d, screen up to %dx%d\n",
					major, minor, vid_maxscreenwidth, vid_maxscreenheight);

// So Xlib's cached screen dimensions follow the resizes below.
	XRRSelectInput (x_disp, XDefaultRootWindow (x_disp),
					RRScreenChangeNotifyMask);
}

/*
================
VID_SetPhysSize

Quake never asks the server for a physical size, but RRSetScreenSize wants one
and a zero makes every client that computes a DPI divide by it. 96 dpi.
================
*/
static void VID_SetPhysSize (Window root, int width, int height)
{
	XRRSetScreenSize (x_disp, root, width, height,
					  (width * 254) / 960, (height * 254) / 960);
}

/*
================
VID_SetScreenSize

Returns whether the screen is now the size asked for.
================
*/
static qboolean VID_SetScreenSize (int width, int height)
{
	XRRScreenResources	*res;
	XRROutputInfo		*output = NULL;
	XRRModeInfo			mode_info;
	char				mode_name[32];
	RRMode				mode = None;
	RROutput			out = None;
	RRCrtc				crtc = None;
	Window				root;
	int					(*old_handler)(Display *, XErrorEvent *);
	int					i, cur_width, cur_height;

	if (!x_randr || !x_own_screen)
		return false;

	VID_RootSize (&cur_width, &cur_height);
	if (cur_width == width && cur_height == height)
		return true;

	root = XDefaultRootWindow (x_disp);

	res = XRRGetScreenResources (x_disp, root);
	if (!res)
		return false;

// The first connected output and whichever CRTC drives it. Xvfb has exactly
// one of each; a desktop may have more, but -resizescreen says this is not
// one of those.
	for (i = 0 ; i < res->noutput ; i++)
	{
		output = XRRGetOutputInfo (x_disp, res, res->outputs[i]);
		if (output && output->connection == RR_Connected)
		{
			out = res->outputs[i];
			crtc = output->crtc ? output->crtc
								: (output->ncrtc ? output->crtcs[0] : None);
			break;
		}
		if (output)
		{
			XRRFreeOutputInfo (output);
			output = NULL;
		}
	}

	if (out == None || crtc == None)
	{
		if (output)
			XRRFreeOutputInfo (output);
		XRRFreeScreenResources (res);
		return false;
	}

	for (i = 0 ; i < res->nmode ; i++)
		if (res->modes[i].width == width && res->modes[i].height == height)
		{
			mode = res->modes[i].id;
			break;
		}

	x_error_seen = 0;
	old_handler = XSetErrorHandler (VID_XErrorTrap);

	if (mode == None)
	{
	// The timings are never used -- nothing here drives a pixel clock -- but
	// the server rejects a mode whose totals do not bound its visible area.
		sprintf (mode_name, "%dx%d", width, height);
		memset (&mode_info, 0, sizeof(mode_info));
		mode_info.name = mode_name;
		mode_info.nameLength = strlen (mode_name);
		mode_info.width = width;
		mode_info.height = height;
		mode_info.hSyncStart = width + 8;
		mode_info.hSyncEnd = width + 40;
		mode_info.hTotal = width + 80;
		mode_info.vSyncStart = height + 3;
		mode_info.vSyncEnd = height + 9;
		mode_info.vTotal = height + 20;
		mode_info.dotClock = (unsigned long)mode_info.hTotal
							 * mode_info.vTotal * 60;

		mode = XRRCreateMode (x_disp, root, &mode_info);
		if (mode != None)
			XRRAddOutputMode (x_disp, out, mode);
		XSync (x_disp, False);
	}
	else
	{
	// Known to the screen, but perhaps not yet offered on this output.
		for (i = 0 ; i < output->nmode ; i++)
			if (output->modes[i] == mode)
				break;
		if (i == output->nmode)
		{
			XRRAddOutputMode (x_disp, out, mode);
			XSync (x_disp, False);
		}
	}

	if (mode != None && !x_error_seen)
	{
	// Growing: the screen has to be large enough before the CRTC will take
	// the mode. Shrinking: the CRTC has to come down first or the screen
	// would no longer cover it. Doing both, in that order, covers either.
		if (width > cur_width || height > cur_height)
		{
			VID_SetPhysSize (root,
							 width > cur_width ? width : cur_width,
							 height > cur_height ? height : cur_height);
			XSync (x_disp, False);
		}

		XRRSetCrtcConfig (x_disp, res, crtc, CurrentTime, 0, 0,
						  mode, RR_Rotate_0, &out, 1);
		VID_SetPhysSize (root, width, height);
		XSync (x_disp, False);
	}

	XSetErrorHandler (old_handler);

	XRRFreeOutputInfo (output);
	XRRFreeScreenResources (res);

	VID_RootSize (&cur_width, &cur_height);
	return cur_width == width && cur_height == height;
}

/*
================
VID_ClampMode
================
*/
static void VID_ClampMode (int *width, int *height)
{
	int		maxw = MAXWIDTH, maxh = MAXHEIGHT;

	if (vid_maxscreenwidth < maxw)
		maxw = vid_maxscreenwidth;
	if (vid_maxscreenheight < maxh)
		maxh = vid_maxscreenheight;

	if (*width > maxw)
		*width = maxw;
	if (*height > maxh)
		*height = maxh;
	if (*width < 320)
		*width = 320;
	if (*height < 200)
		*height = 200;

	*width &= ~7;
}

/*
================
VID_ApplyMode

Resizes the screen and the window. The reallocation of the framebuffer, the
z-buffer and the surface cache is left to the config_notify path in
VID_Update, which already does exactly that for a resize from outside.
================
*/
static void VID_ApplyMode (int width, int height)
{
	VID_ClampMode (&width, &height);

// Write the clamped values back, or a mode this build cannot draw leaves the
// cvars disagreeing with the screen and VID_Update retrying every frame.
	if ((int)vid_width.value != width)
		Cvar_SetValue ("vid_width", width);
	if ((int)vid_height.value != height)
		Cvar_SetValue ("vid_height", height);

	if (width == vid.width && height == vid.height)
		return;

	if (!VID_SetScreenSize (width, height) && x_own_screen && !x_resize_warned)
	{
		x_resize_warned = true;
		Con_Printf ("VID: this X server will not resize its screen, so only\n"
					"     the window changes size and the rest of the\n"
					"     picture stays black.\n");
	}

	XMoveResizeWindow (x_disp, x_win, 0, 0, width, height);
	XSync (x_disp, False);

// Do not wait for the ConfigureNotify to come back round; the size is known.
	config_notify_width = width;
	config_notify_height = height;
	config_notify = 1;
}

/*
================
VID_CheckModeChange

vid_width and vid_height are archived, so config.cfg sets them long after
VID_Init has run -- and the video menu only writes them. Both arrive here.
================
*/
static void VID_CheckModeChange (void)
{
	int		width = (int)vid_width.value;
	int		height = (int)vid_height.value;

	if (width == vid.width && height == vid.height)
		return;

	VID_ApplyMode (width, height);
}

/*
================================================================================

VIDEO MENU

================================================================================
*/

#define	VID_MENU_TOP	48
#define	VID_MENU_ROWS	13

static int		vid_menu_cursor;
static int		vid_menu_top;

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw (void)
{
	qpic_t	*p;
	char	line[64];
	int		i, row, count, last;

// Modes this server cannot give are not offered.
	count = 0;
	for (i = 0 ; i < NUM_VID_MODES ; i++)
		if (vid_modes[i].width <= vid_maxscreenwidth
			&& vid_modes[i].height <= vid_maxscreenheight)
			count++;

// In pak0 since the shareware release; Draw_CachePic calls Sys_Error rather
// than returning when a lump is missing, so there is nothing to check.
	p = Draw_CachePic ("gfx/vidmodes.lmp");
	M_DrawPic ((320 - p->width) / 2, 4, p);

	sprintf (line, "current: %dx%d", vid.width, vid.height);
	M_Print (16, 32, line);

	if (vid_menu_cursor >= count)
		vid_menu_cursor = count - 1;
	if (vid_menu_cursor < 0)
		vid_menu_cursor = 0;

// Keep the cursor inside the window of rows there is room to print.
	if (vid_menu_cursor < vid_menu_top)
		vid_menu_top = vid_menu_cursor;
	if (vid_menu_cursor >= vid_menu_top + VID_MENU_ROWS)
		vid_menu_top = vid_menu_cursor - VID_MENU_ROWS + 1;
	last = vid_menu_top + VID_MENU_ROWS;
	if (last > count)
		last = count;

	row = 0;
	for (i = 0 ; i < NUM_VID_MODES ; i++)
	{
		if (vid_modes[i].width > vid_maxscreenwidth
			|| vid_modes[i].height > vid_maxscreenheight)
			continue;

		if (row >= vid_menu_top && row < last)
		{
			int	y = VID_MENU_TOP + (row - vid_menu_top) * 8;

			sprintf (line, "%4d x %-4d", vid_modes[i].width,
					 vid_modes[i].height);
			if (vid_modes[i].width == vid.width
				&& vid_modes[i].height == vid.height)
				M_PrintWhite (56, y, line);
			else
				M_Print (56, y, line);

			if (row == vid_menu_cursor)
				M_DrawCharacter (40, y, 12 + ((int)(realtime*4) & 1));
		}
		row++;
	}

	if (vid_menu_top > 0)
		M_Print (56, VID_MENU_TOP - 8, "^ more above");
	if (last < count)
		M_Print (56, VID_MENU_TOP + VID_MENU_ROWS * 8, "v more below");

	M_Print (16, VID_MENU_TOP + VID_MENU_ROWS * 8 + 16,
			 "Enter to apply, Esc to go back");
}

/*
================
VID_MenuKey
================
*/
void VID_MenuKey (int key)
{
	int		i, row, count;

	count = 0;
	for (i = 0 ; i < NUM_VID_MODES ; i++)
		if (vid_modes[i].width <= vid_maxscreenwidth
			&& vid_modes[i].height <= vid_maxscreenheight)
			count++;

	switch (key)
	{
	case K_ESCAPE:
		S_LocalSound ("misc/menu1.wav");
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		vid_menu_cursor--;
		if (vid_menu_cursor < 0)
			vid_menu_cursor = count - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		vid_menu_cursor++;
		if (vid_menu_cursor >= count)
			vid_menu_cursor = 0;
		break;

	case K_ENTER:
		S_LocalSound ("misc/menu1.wav");
		row = 0;
		for (i = 0 ; i < NUM_VID_MODES ; i++)
		{
			if (vid_modes[i].width > vid_maxscreenwidth
				|| vid_modes[i].height > vid_maxscreenheight)
				continue;
			if (row == vid_menu_cursor)
			{
			// Written, not applied: VID_CheckModeChange picks it up on the
			// next frame, the same way it picks up config.cfg. It is also
			// what makes the choice stick across a restart.
				Cvar_SetValue ("vid_width", vid_modes[i].width);
				Cvar_SetValue ("vid_height", vid_modes[i].height);
				break;
			}
			row++;
		}
		break;

	default:
		break;
	}
}

// flushes the given rectangles from the view buffer to the screen

void	VID_Update (vrect_t *rects)
{
	vrect_t full;

	VID_CheckModeChange ();

// if the window changes dimension, skip this frame

	if (config_notify)
	{
		config_notify = 0;
		vid.width = config_notify_width;
		vid.height = config_notify_height;
	// A size from outside is not bounded by anything, and the renderer's
	// static tables are. The original took it as given and wrote off the end
	// of d_scantable.
		VID_ClampMode (&vid.width, &vid.height);

	// D_InitCaches, at the end of the reset below, announces the new surface
	// cache size with Con_Printf -- and Con_Printf draws the screen when the
	// console is up. That re-entered SCR_UpdateScreen from inside this
	// function, with vid.width already the new size and vid.buffer still the
	// old, smaller framebuffer, so Draw_ConsoleBackground wrote a 640-pixel
	// row into a 512-pixel one and off the end of the allocation. It only
	// crashed when the new mode was the larger of the two.
	//
	// block_drawing is the engine's own answer to this: vid_win.c sets it
	// around a mode change for the same reason, and SCR_UpdateScreen checks
	// it first thing. Nothing in this build had ever set it.
		block_drawing = true;

		if (doShm)
			ResetSharedFrameBuffers();
		else
			ResetFrameBuffer();
		vid.rowbytes = x_framebuffer[0]->bytes_per_line;
		vid.buffer = x_framebuffer[current_framebuffer]->data;
		vid.conbuffer = vid.buffer;
		vid.conwidth = vid.width;
		vid.conheight = vid.height;
		vid.conrowbytes = vid.rowbytes;

		block_drawing = false;

		if (verbose)
			Con_Printf ("VID: now %dx%d\n", vid.width, vid.height);
		vid.aspect = VID_PixelAspect ();
		vid.recalc_refdef = 1;				// force a surface cache flush
		Cvar_SetValue ("vid_width", vid.width);
		Cvar_SetValue ("vid_height", vid.height);
		Con_CheckResize();
		Con_Clear_f();
		return;
	}

	// force full update if not 8bit
	if (x_visinfo->depth != 8) {
		extern int scr_fullupdate;

		scr_fullupdate = 0;
	}


	if (doShm)
	{

		while (rects)
		{
			if (x_visinfo->depth == 16)
				st2_fixup( x_framebuffer[current_framebuffer], 
					rects->x, rects->y, rects->width,
					rects->height);
			else if (x_visinfo->depth == 24)
				st3_fixup( x_framebuffer[current_framebuffer], 
					rects->x, rects->y, rects->width,
					rects->height);
			if (!XShmPutImage(x_disp, x_win, x_gc,
				x_framebuffer[current_framebuffer], rects->x, rects->y,
				rects->x, rects->y, rects->width, rects->height, True))
					Sys_Error("VID_Update: XShmPutImage failed\n");
			oktodraw = false;
			while (!oktodraw) GetEvent();
			rects = rects->pnext;
		}
		current_framebuffer = !current_framebuffer;
		vid.buffer = x_framebuffer[current_framebuffer]->data;
		vid.conbuffer = vid.buffer;
		XSync(x_disp, False);
	}
	else
	{
		while (rects)
		{
			if (x_visinfo->depth == 16)
				st2_fixup( x_framebuffer[current_framebuffer], 
					rects->x, rects->y, rects->width,
					rects->height);
			else if (x_visinfo->depth == 24)
				st3_fixup( x_framebuffer[current_framebuffer], 
					rects->x, rects->y, rects->width,
					rects->height);
			XPutImage(x_disp, x_win, x_gc, x_framebuffer[0], rects->x,
				rects->y, rects->x, rects->y, rects->width, rects->height);
			rects = rects->pnext;
		}
		XSync(x_disp, False);
	}

}

static int dither;

void VID_DitherOn(void)
{
    if (dither == 0)
    {
		vid.recalc_refdef = 1;
        dither = 1;
    }
}

void VID_DitherOff(void)
{
    if (dither)
    {
		vid.recalc_refdef = 1;
        dither = 0;
    }
}

int Sys_OpenWindow(void)
{
	return 0;
}

void Sys_EraseWindow(int window)
{
}

void Sys_DrawCircle(int window, int x, int y, int r)
{
}

void Sys_DisplayWindow(int window)
{
}

void Sys_SendKeyEvents(void)
{
// get events from x server
	if (x_disp)
	{
		while (XPending(x_disp)) GetEvent();
	//
	// Take the event off the queue before handing it on, not after. Key_Event
	// can re-enter this function: a menu key that opens a yes/no question
	// (SCR_ModalMessage) pumps events from inside the handler until it is
	// answered. With the tail advanced afterwards, that inner pump started on
	// the very event being handled, drained the queue, and the outer loop then
	// stepped the tail one past the head -- so the queue looked full and the
	// whole ring of old key presses was replayed. Mostly empty slots and
	// harmless keys, which is why it went unnoticed; with a stop signal
	// pending it replayed the Return that opened the question, which opened
	// it again, forever.
	//
		while (keyq_head != keyq_tail)
		{
			int		key = keyq[keyq_tail].key;
			int		down = keyq[keyq_tail].down;

			keyq_tail = (keyq_tail + 1) & 63;
			Key_Event (key, down);
		}
	}
}

#if 0
char *Sys_ConsoleInput (void)
{

	static char	text[256];
	int		len;
	fd_set  readfds;
	int		ready;
	struct timeval timeout;

	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	FD_ZERO(&readfds);
	FD_SET(0, &readfds);
	ready = select(1, &readfds, 0, 0, &timeout);

	if (ready>0)
	{
		len = read (0, text, sizeof(text));
		if (len >= 1)
		{
			text[len-1] = 0;	// rip off the /n and terminate
			return text;
		}
	}

	return 0;
	
}
#endif

void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
// direct drawing of the "accessing disk" icon isn't supported under Linux
}

void D_EndDirectRect (int x, int y, int width, int height)
{
// direct drawing of the "accessing disk" icon isn't supported under Linux
}

void IN_Init (void)
{
	Cvar_RegisterVariable (&_windowed_mouse);
	Cvar_RegisterVariable (&m_filter);
   if ( COM_CheckParm ("-nomouse") )
     return;
   mouse_x = mouse_y = 0.0;
   mouse_avail = 1;
}

void IN_Shutdown (void)
{
   mouse_avail = 0;
}

void IN_Commands (void)
{
	int i;
   
	if (!mouse_avail) return;
   
	for (i=0 ; i<mouse_buttons ; i++) {
		if ( (mouse_buttonstate & (1<<i)) && !(mouse_oldbuttonstate & (1<<i)) )
			Key_Event (K_MOUSE1 + i, true);

		if ( !(mouse_buttonstate & (1<<i)) && (mouse_oldbuttonstate & (1<<i)) )
			Key_Event (K_MOUSE1 + i, false);
	}
	mouse_oldbuttonstate = mouse_buttonstate;
}

void IN_Move (usercmd_t *cmd)
{
	if (!mouse_avail)
		return;
   
	if (m_filter.value) {
		mouse_x = (mouse_x + old_mouse_x) * 0.5;
		mouse_y = (mouse_y + old_mouse_y) * 0.5;
	}

	old_mouse_x = mouse_x;
	old_mouse_y = mouse_y;
   
	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;
   
	if ( (in_strafe.state & 1) || (lookstrafe.value && IN_LOOKING()) )
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;
	if (IN_LOOKING())
		V_StopPitchDrift ();
   
	if ( IN_LOOKING() && !(in_strafe.state & 1)) {
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	} else {
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}
	mouse_x = mouse_y = 0.0;
}

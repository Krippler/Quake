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
// in_pad.c -- a game controller, the same in the container and on a desktop.
//
// A controller's buttons are keys here, PAD_A to PAD_GUIDE, bound like any
// other in config.cfg and in Options -> Controls -> Customize Controls. Its
// sticks move and look, tuned by the joy_ cvars on the Controls page. That is
// how the re-release does it, and it means a binding or a setting is made once,
// in the game, whichever way the game is being played.
//
// Where the controller's state comes from is the only thing that differs:
//
//   on a desktop    SDL2's game controller API, loaded when the engine starts
//                   if the system has it. SDL knows the button layout of
//                   nearly every pad there is, which reading /dev/input
//                   directly would have to relearn pad by pad. It is opened
//                   with dlopen, so a machine without it still runs the game.
//
//   in the          the browser, which has the pad. The page reads it through
//   container       the Gamepad API and sends its state here over the same
//                   WebSocket port as the picture and the sound; websockify
//                   hands it on to a TCP port on localhost, which is this
//                   file's (QUAKE_PAD_PORT, set by the entrypoint).
//
// Both give the same thing: which buttons are down, and where the sticks and
// triggers are, in the standard layout (A at the bottom, B on the right).
//

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "quakedef.h"

// The buttons, in the order of the browser's standard layout; SDL numbers
// them differently after the first four, which PAD_SDLRead translates.
enum
{
	PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_BACK, PB_START,
	PB_LS, PB_RS, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_GUIDE,
	PB_COUNT
};

cvar_t	joy_enable = {"joy_enable", "1", true};
cvar_t	joy_deadzone = {"joy_deadzone", "0.18", true};	// the stick that moves
cvar_t	joy_deadzone_look = {"joy_deadzone_look", "0.18", true};	// and the one that looks
cvar_t	joy_lookspeed = {"joy_lookspeed", "160", true};	// degrees a second, turning
cvar_t	joy_lookspeed_y = {"joy_lookspeed_y", "105", true};	// and looking up and down
cvar_t	joy_lookcurve = {"joy_lookcurve", "2", true};
cvar_t	joy_invert = {"joy_invert", "0", true};
cvar_t	joy_swapsticks = {"joy_swapsticks", "0", true};
cvar_t	joy_pushrun = {"joy_pushrun", "1", true};
cvar_t	joy_bound = {"joy_bound", "0", true};

typedef struct
{
	qboolean	connected;
	unsigned	buttons;				// 1 << PB_*
	float		lx, ly, rx, ry;			// -1 .. 1, y down
	float		lt, rt;					// 0 .. 1
	char		name[64];
} padstate_t;

static padstate_t	pad;
static unsigned		pad_prev;			// buttons, as last turned into keys
static int			pad_sentas[PB_COUNT];	// the key each held button went down as

// Navigation in a menu, with a held direction repeating as a keyboard's would.
static int			pad_navkey;
static double		pad_navnext;

// The default bindings, applied the first time a controller turns up (and
// after Reset Defaults, whose default.cfg starts with unbindall). The same
// layout the browser's controller panel had, so nobody has to relearn it.
static const char	*pad_defaults[][2] =
{
	{"PAD_A",		"+jump"},
	{"PAD_B",		"togglemenu"},
	{"PAD_X",		"impulse 2"},
	{"PAD_Y",		"impulse 7"},
	{"PAD_LB",		"impulse 12"},
	{"PAD_RB",		"impulse 10"},
	{"PAD_LT",		"+speed"},
	{"PAD_RT",		"+attack"},
	{"PAD_BACK",	"+showscores"},
	{"PAD_LS",		"impulse 1"},
	{"PAD_RS",		"impulse 8"},
	{"PAD_UP",		"+forward"},
	{"PAD_DOWN",	"+back"},
	{"PAD_LEFT",	"+left"},
	{"PAD_RIGHT",	"+right"},
};

qboolean M_KeysGrabbing (void);


/*
==============================================================================

FROM THE BROWSER

A TCP listener on localhost, for websockify to connect the page's WebSocket to.
Two kinds of message, each starting with a letter:

	'P' connected buttons(4) lx ly rx ry (2 each, signed) lt rt (1 each)
		16 bytes; little-endian; sticks -32767..32767, triggers 0..255
	'N' length name
		the pad's name, for the Controls page

One page at a time: a new connection replaces the old, which is what a reload
of the page looks like from here.

==============================================================================
*/

static int		br_listen = -1;
static int		br_conn = -1;
static byte		br_buf[256];
static int		br_len;

static void PAD_BridgeInit (int port)
{
	struct sockaddr_in	addr;
	int					one = 1;

	br_listen = socket (AF_INET, SOCK_STREAM, 0);
	if (br_listen < 0)
		return;
	setsockopt (br_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset (&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons (port);
	addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

	if (bind (br_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0
		|| listen (br_listen, 2) < 0)
	{
		Con_Printf ("Controller: cannot listen on port %d (%s)\n", port,
					strerror (errno));
		close (br_listen);
		br_listen = -1;
		return;
	}
	fcntl (br_listen, F_SETFL, O_NONBLOCK);
	Con_Printf ("Controller: from the browser, port %d\n", port);
}

static void PAD_BridgeDrop (void)
{
	if (pad.connected)
		Con_Printf ("Controller: disconnected\n");
	if (br_conn >= 0)
		close (br_conn);
	br_conn = -1;
	br_len = 0;
	pad.connected = false;
}

static short PAD_Short (byte *p)
{
	return (short)(p[0] | (p[1] << 8));
}

static void PAD_BridgeRead (void)
{
	int		c, n, used, one = 1;

	if (br_listen < 0)
		return;

	c = accept (br_listen, NULL, NULL);
	if (c >= 0)
	{
		PAD_BridgeDrop ();
		br_conn = c;
		fcntl (br_conn, F_SETFL, O_NONBLOCK);
		setsockopt (br_conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}

	if (br_conn < 0)
		return;

	for (;;)
	{
		n = recv (br_conn, br_buf + br_len, sizeof(br_buf) - br_len, 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		{
			PAD_BridgeDrop ();		// the page went away
			return;
		}
		if (n < 0)
			break;
		br_len += n;

		used = 0;
		while (used < br_len)
		{
			byte	*m = br_buf + used;
			int		left = br_len - used;

			if (m[0] == 'P')
			{
				if (left < 16)
					break;
				if (pad.connected != (m[1] != 0))
					Con_Printf ("Controller: %s\n", m[1] ? (pad.name[0]
								? pad.name : "connected") : "disconnected");
				pad.connected = m[1] != 0;
				pad.buttons = m[2] | (m[3] << 8) | (m[4] << 16)
					| ((unsigned)m[5] << 24);
				pad.lx = PAD_Short (m + 6) / 32767.0;
				pad.ly = PAD_Short (m + 8) / 32767.0;
				pad.rx = PAD_Short (m + 10) / 32767.0;
				pad.ry = PAD_Short (m + 12) / 32767.0;
				pad.lt = m[14] / 255.0;
				pad.rt = m[15] / 255.0;
				used += 16;
			}
			else if (m[0] == 'N')
			{
				if (left < 2 || left < 2 + m[1])
					break;
				n = m[1] < sizeof(pad.name) - 1 ? m[1] : sizeof(pad.name) - 1;
				memcpy (pad.name, m + 2, n);
				pad.name[n] = 0;
				used += 2 + m[1];
			}
			else
			{
				used = br_len;		// lost the thread; start again
				break;
			}
		}
		memmove (br_buf, br_buf + used, br_len - used);
		br_len -= used;
		if (br_len == sizeof(br_buf))
			br_len = 0;
	}
}


/*
==============================================================================

ON A DESKTOP

SDL2, through dlopen. Only the game controller calls are used, and they are
declared here rather than taken from SDL's headers, so building needs nothing
installed; the ABI has been stable since SDL 2.0.

==============================================================================
*/

typedef struct _SDL_GameController	SDL_GameController;

#define SDL_INIT_GAMECONTROLLER	0x00002000u

static int					(*pSDL_Init) (unsigned);
static int					(*pSDL_NumJoysticks) (void);
static int					(*pSDL_IsGameController) (int);
static SDL_GameController	*(*pSDL_GameControllerOpen) (int);
static void					(*pSDL_GameControllerClose) (SDL_GameController *);
static int					(*pSDL_GameControllerGetAttached) (SDL_GameController *);
static const char			*(*pSDL_GameControllerName) (SDL_GameController *);
static unsigned char		(*pSDL_GameControllerGetButton) (SDL_GameController *, int);
static short				(*pSDL_GameControllerGetAxis) (SDL_GameController *, int);
static void					(*pSDL_GameControllerUpdate) (void);
static void					(*pSDL_PumpEvents) (void);
static void					(*pSDL_FlushEvents) (unsigned, unsigned);

static qboolean				sdl_ok;
static SDL_GameController	*sdl_pad;
static double				sdl_nextscan;

static void PAD_SDLInit (void)
{
	static const char	*names[] = { "libSDL2-2.0.so.0", "libSDL2-2.0.so",
									 "libSDL2.so" };
	void	*lib = NULL;
	int		i;

	for (i = 0 ; i < 3 && !lib ; i++)
		lib = dlopen (names[i], RTLD_NOW | RTLD_LOCAL);
	if (!lib)
	{
		Con_Printf ("Controller: SDL2 is not installed, so no controllers\n");
		return;
	}

#define SYM(n)	if (!(p##n = dlsym (lib, #n))) goto missing
	SYM(SDL_Init);
	SYM(SDL_NumJoysticks);
	SYM(SDL_IsGameController);
	SYM(SDL_GameControllerOpen);
	SYM(SDL_GameControllerClose);
	SYM(SDL_GameControllerGetAttached);
	SYM(SDL_GameControllerName);
	SYM(SDL_GameControllerGetButton);
	SYM(SDL_GameControllerGetAxis);
	SYM(SDL_GameControllerUpdate);
	SYM(SDL_PumpEvents);
	SYM(SDL_FlushEvents);
#undef SYM

	if (pSDL_Init (SDL_INIT_GAMECONTROLLER) < 0)
	{
		Con_Printf ("Controller: SDL2 would not start its controller support\n");
		return;
	}
	sdl_ok = true;
	Con_Printf ("Controller: through SDL2\n");
	return;

missing:
	Con_Printf ("Controller: this SDL2 is missing a function it needs\n");
}

static void PAD_SDLRead (void)
{
	static const int	sdlbutton[PB_COUNT] =
	{
		0, 1, 2, 3,			// A B X Y
		9, 10,				// LB RB
		-1, -1,				// LT RT, which SDL reports as axes
		4, 6,				// Back, Start
		7, 8,				// stick clicks
		11, 12, 13, 14,		// d-pad
		5					// Guide
	};
	int		i;

	if (!sdl_ok)
		return;

	pSDL_PumpEvents ();
	pSDL_GameControllerUpdate ();
	pSDL_FlushEvents (0, 0xFFFF);	// nobody reads SDL's queue; keep it empty

	if (sdl_pad && !pSDL_GameControllerGetAttached (sdl_pad))
	{
		pSDL_GameControllerClose (sdl_pad);
		sdl_pad = NULL;
		pad.connected = false;
		Con_Printf ("Controller: disconnected\n");
	}

// Look for one to open, once a second while there is none.
	if (!sdl_pad && realtime >= sdl_nextscan)
	{
		sdl_nextscan = realtime + 1;
		for (i = 0 ; i < pSDL_NumJoysticks () ; i++)
			if (pSDL_IsGameController (i)
				&& (sdl_pad = pSDL_GameControllerOpen (i)) != NULL)
			{
				const char	*n = pSDL_GameControllerName (sdl_pad);

				Q_strncpy (pad.name, (char *)(n ? n : "Controller"), sizeof(pad.name) - 1);
				Con_Printf ("Controller: %s\n", pad.name);
				break;
			}
	}

	if (!sdl_pad)
		return;

	pad.connected = true;
	pad.buttons = 0;
	for (i = 0 ; i < PB_COUNT ; i++)
		if (sdlbutton[i] >= 0
			&& pSDL_GameControllerGetButton (sdl_pad, sdlbutton[i]))
			pad.buttons |= 1u << i;
	pad.lx = pSDL_GameControllerGetAxis (sdl_pad, 0) / 32767.0;
	pad.ly = pSDL_GameControllerGetAxis (sdl_pad, 1) / 32767.0;
	pad.rx = pSDL_GameControllerGetAxis (sdl_pad, 2) / 32767.0;
	pad.ry = pSDL_GameControllerGetAxis (sdl_pad, 3) / 32767.0;
	pad.lt = pSDL_GameControllerGetAxis (sdl_pad, 4) / 32767.0;
	pad.rt = pSDL_GameControllerGetAxis (sdl_pad, 5) / 32767.0;
}


/*
==============================================================================

INTO THE GAME

==============================================================================
*/

/*
================
PAD_Name

For the Controls page: which pad, or NULL when there is none.
================
*/
char *PAD_Name (void)
{
	if (!pad.connected)
		return NULL;
	return pad.name[0] ? pad.name : "Controller";
}

/*
================
PAD_BindDefaults
================
*/
static void PAD_BindDefaults (void)
{
	int		i, k;

	for (i = 0 ; i < (int)(sizeof(pad_defaults) / sizeof(pad_defaults[0])) ; i++)
	{
		k = Key_StringToKeynum ((char *)pad_defaults[i][0]);
		if (k >= 0 && !keybindings[k])
			Key_SetBinding (k, (char *)pad_defaults[i][1]);
	}
	Cvar_Set ("joy_bound", "1");
}

/*
================
PAD_MenuKey

What a button does while a menu is up: the keys the menus already answer to.
A is Return and B is Escape, as on every console; Y clears a binding on the
controls screen, as Delete does. Anything else does nothing there.
================
*/
static int PAD_MenuKey (int b)
{
	switch (b)
	{
	case PB_A:		return K_ENTER;
	case PB_B:		return K_ESCAPE;
	case PB_Y:		return K_DEL;
	case PB_UP:		return K_UPARROW;
	case PB_DOWN:	return K_DOWNARROW;
	case PB_LEFT:	return K_LEFTARROW;
	case PB_RIGHT:	return K_RIGHTARROW;
	case PB_LB:		return K_LEFTARROW;
	case PB_RB:		return K_RIGHTARROW;
	}
	return 0;
}

/*
================
PAD_Button

A button went down or up. Start is the menu key, as Escape is, and can no more
be unbound than Escape can. In a menu the others are menu keys, except while
the controls screen is waiting for one to bind. Everywhere else a button is
its own key, and does what it is bound to.

The key a button went down as is the key it comes up as, whatever has opened
or closed in between -- otherwise a trigger held for +attack and let go in the
menu would leave the player firing.
================
*/
static void PAD_Button (int b, qboolean down)
{
	int		k;

	if (!down)
	{
		if (pad_sentas[b])
			Key_Event (pad_sentas[b], false);
		pad_sentas[b] = 0;
		return;
	}

	if (b == PB_START)
		k = K_ESCAPE;
	else if (key_dest == key_menu && !M_KeysGrabbing ())
		k = PAD_MenuKey (b);
	else
		k = K_AUX1 + b;

	pad_sentas[b] = k;
	if (k)
		Key_Event (k, true);
}

/*
================
PAD_Nav

The left stick as arrow keys in a menu, repeating while it is held over, the
way the d-pad does not have to (the menus take each press as one step).
================
*/
static void PAD_Nav (void)
{
	int		k = 0;

	if (key_dest == key_menu && !M_KeysGrabbing ())
	{
		if (pad.ly < -0.6)
			k = K_UPARROW;
		else if (pad.ly > 0.6)
			k = K_DOWNARROW;
		else if (pad.lx < -0.6)
			k = K_LEFTARROW;
		else if (pad.lx > 0.6)
			k = K_RIGHTARROW;
	}

	if (k != pad_navkey)
	{
		pad_navkey = k;
		pad_navnext = realtime + 0.4;
		if (k)
		{
			Key_Event (k, true);
			Key_Event (k, false);
		}
		return;
	}

	if (k && realtime >= pad_navnext)
	{
		pad_navnext = realtime + 0.12;
		Key_Event (k, true);
		Key_Event (k, false);
	}
}

/*
================
IN_PadCommands

Once a frame, from IN_Commands: read the pad and turn its buttons into keys.
================
*/
void IN_PadCommands (void)
{
	unsigned	now, change;
	int			b;

	PAD_BridgeRead ();
	PAD_SDLRead ();

	now = 0;
	if (pad.connected && joy_enable.value)
	{
		if (!joy_bound.value)
			PAD_BindDefaults ();

		now = pad.buttons & ~((1u << PB_LT) | (1u << PB_RT));
	// Triggers are analog; a third of the way is a press, and it has to come
	// back past a fifth to let go, so one resting near the mark does not
	// chatter.
		if (pad.lt > 0.33 || ((pad_prev & (1u << PB_LT)) && pad.lt > 0.2))
			now |= 1u << PB_LT;
		if (pad.rt > 0.33 || ((pad_prev & (1u << PB_RT)) && pad.rt > 0.2))
			now |= 1u << PB_RT;
	}

	change = now ^ pad_prev;
	for (b = 0 ; b < PB_COUNT ; b++)
		if (change & (1u << b))
			PAD_Button (b, (now & (1u << b)) != 0);
	pad_prev = now;

	if (pad.connected && joy_enable.value)
		PAD_Nav ();
	else
		pad_navkey = 0;
}

/*
================
PAD_Stick

A stick past its deadzone, taken as a stick rather than two axes so a diagonal
is not held to a higher bar, and rescaled so the first movement past it is a
small one. Returns how far over it is, 0 to 1.
================
*/
static float PAD_Stick (float x, float y, float dz, float *ox, float *oy)
{
	float	m, s;

	m = sqrt (x*x + y*y);
	if (m <= dz || m <= 0)
	{
		*ox = *oy = 0;
		return 0;
	}
	s = (m - dz) / (1 - dz);
	if (s > 1)
		s = 1;
	*ox = x / m * s;
	*oy = y / m * s;
	return s;
}

/*
================
IN_PadMove

From IN_Move: the left stick walks and sidesteps, the right one looks.
================
*/
void IN_PadMove (usercmd_t *cmd)
{
	float	mx, my, lx, ly, m, speed, c, t;

	if (!pad.connected || !joy_enable.value || key_dest != key_game)
		return;

// The deadzones go with what a stick does, not which side it is on, so that
// swapping the sticks swaps them too.
	if (joy_swapsticks.value)
	{
		m = PAD_Stick (pad.rx, pad.ry, joy_deadzone.value, &mx, &my);
		PAD_Stick (pad.lx, pad.ly, joy_deadzone_look.value, &lx, &ly);
	}
	else
	{
		m = PAD_Stick (pad.lx, pad.ly, joy_deadzone.value, &mx, &my);
		PAD_Stick (pad.rx, pad.ry, joy_deadzone_look.value, &lx, &ly);
	}

// Walking: the stick is a speed, not a key. Pushed all the way it runs, if the
// run key is not held already and Always Run is not on.
// A frame between the game's ticks (Max FPS) only turns the view: the stick's
// walking is a speed, which the tick's own command carries.
	speed = 1;
	if (in_speed.state & 1)
		speed = cl_movespeedkey.value;
	else if (joy_pushrun.value && m > 0.9 && cl_forwardspeed.value <= 200)
		speed = cl_movespeedkey.value;
	if (!in_accumulating)
	{
		cmd->forwardmove -= my * cl_forwardspeed.value * speed;
		cmd->sidemove += mx * cl_sidespeed.value * speed;
	}

// Looking: degrees a second, on a curve so a small push aims finely. Turning
// and looking up and down have speeds of their own, as the re-release's Aim X
// and Aim Y; the vertical one defaults to two thirds of the horizontal, which
// is what it was fixed at before it could be set.
	c = joy_lookcurve.value > 0 ? joy_lookcurve.value : 1;
	t = joy_lookspeed.value * host_frametime;
	if (lx)
		cl.viewangles[YAW] -= (lx < 0 ? -1 : 1) * pow (fabs (lx), c) * t;
	if (ly)
	{
		cl.viewangles[PITCH] += (ly < 0 ? -1 : 1) * pow (fabs (ly), c)
			* joy_lookspeed_y.value * host_frametime
			* (joy_invert.value ? -1 : 1);
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
		V_StopPitchDrift ();
	}
}

/*
================
IN_PadInit
================
*/
void IN_PadInit (void)
{
	int		i, port = 0;
	char	*env;

	Cvar_RegisterVariable (&joy_enable);
	Cvar_RegisterVariable (&joy_deadzone);
	Cvar_RegisterVariable (&joy_deadzone_look);
	Cvar_RegisterVariable (&joy_lookspeed_y);
	Cvar_RegisterVariable (&joy_lookspeed);
	Cvar_RegisterVariable (&joy_lookcurve);
	Cvar_RegisterVariable (&joy_invert);
	Cvar_RegisterVariable (&joy_swapsticks);
	Cvar_RegisterVariable (&joy_pushrun);
	Cvar_RegisterVariable (&joy_bound);

	if (COM_CheckParm ("-nojoy"))
		return;

// The browser, when the container has said where to listen for it; a desktop
// otherwise.
	i = COM_CheckParm ("-padport");
	if (i && i < com_argc - 1)
		port = Q_atoi (com_argv[i+1]);
	else if ((env = getenv ("QUAKE_PAD_PORT")) && *env)
		port = Q_atoi (env);

	if (port > 0)
		PAD_BridgeInit (port);
	else
		PAD_SDLInit ();
}

/*
================
IN_PadShutdown
================
*/
void IN_PadShutdown (void)
{
	PAD_BridgeDrop ();
	if (br_listen >= 0)
		close (br_listen);
	br_listen = -1;
	if (sdl_pad)
		pSDL_GameControllerClose (sdl_pad);
	sdl_pad = NULL;
}

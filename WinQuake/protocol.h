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
// protocol.h -- communications protocols

#define	PROTOCOL_VERSION	15

//
// 15 is id's protocol and what the server speaks unless a map needs more.
// 666 is FitzQuake's, which QuakeSpasm and most engines since read: 15 with
// every number that 15 sends as a byte given a way to carry a second byte, and
// nothing else changed. A map with more than 256 models or sounds -- the
// re-release's do -- cannot be described in 15 at all, so the server switches
// to 666 for it and only for it. See PORTING-NOTES.md.
//
// The constants below are FitzQuake's, taken from QuakeSpasm's protocol.h, so
// that a demo recorded here plays in those engines and theirs play here.
//
#define	PROTOCOL_NETQUAKE	15
#define	PROTOCOL_FITZQUAKE	666

// if the high bit of the servercmd is set, the low bits are fast update flags:
#define	U_MOREBITS	(1<<0)
#define	U_ORIGIN1	(1<<1)
#define	U_ORIGIN2	(1<<2)
#define	U_ORIGIN3	(1<<3)
#define	U_ANGLE2	(1<<4)
#define	U_NOLERP	(1<<5)		// don't interpolate movement
#define	U_FRAME		(1<<6)
#define U_SIGNAL	(1<<7)		// just differentiates from other updates

// svc_update can pass all of the fast update bits, plus more
#define	U_ANGLE1	(1<<8)
#define	U_ANGLE3	(1<<9)
#define	U_MODEL		(1<<10)
#define	U_COLORMAP	(1<<11)
#define	U_SKIN		(1<<12)
#define	U_EFFECTS	(1<<13)
#define	U_LONGENTITY	(1<<14)

// PROTOCOL_FITZQUAKE
#define	U_EXTEND1		(1<<15)		// another byte of bits follows
#define	U_ALPHA			(1<<16)		// 1 byte; read and ignored here
#define	U_FRAME2		(1<<17)		// 1 byte, frame >> 8
#define	U_MODEL2		(1<<18)		// 1 byte, modelindex >> 8
#define	U_LERPFINISH	(1<<19)		// 1 byte; read and ignored here
#define	U_SCALE			(1<<20)		// 1 byte; read and ignored here
#define	U_EXTEND2		(1<<23)		// another byte of bits follows


#define	SU_VIEWHEIGHT	(1<<0)
#define	SU_IDEALPITCH	(1<<1)
#define	SU_PUNCH1		(1<<2)
#define	SU_PUNCH2		(1<<3)
#define	SU_PUNCH3		(1<<4)
#define	SU_VELOCITY1	(1<<5)
#define	SU_VELOCITY2	(1<<6)
#define	SU_VELOCITY3	(1<<7)
//define	SU_AIMENT		(1<<8)  AVAILABLE BIT
#define	SU_ITEMS		(1<<9)
#define	SU_ONGROUND		(1<<10)		// no data follows, the bit is it
#define	SU_INWATER		(1<<11)		// no data follows, the bit is it
#define	SU_WEAPONFRAME	(1<<12)
#define	SU_ARMOR		(1<<13)
#define	SU_WEAPON		(1<<14)

// PROTOCOL_FITZQUAKE: the high bytes of what 15 sends as bytes
#define	SU_EXTEND1		(1<<15)
#define	SU_WEAPON2		(1<<16)		// weaponmodel >> 8
#define	SU_ARMOR2		(1<<17)
#define	SU_AMMO2		(1<<18)
#define	SU_SHELLS2		(1<<19)
#define	SU_NAILS2		(1<<20)
#define	SU_ROCKETS2		(1<<21)
#define	SU_CELLS2		(1<<22)
#define	SU_EXTEND2		(1<<23)
#define	SU_WEAPONFRAME2	(1<<24)
#define	SU_WEAPONALPHA	(1<<25)		// 1 byte; read and ignored here
#define	SU_EXTEND3		(1<<31)

// a sound with no channel is a local only sound
#define	SND_VOLUME		(1<<0)		// a byte
#define	SND_ATTENUATION	(1<<1)		// a byte
#define	SND_LOOPING		(1<<2)		// a long

// PROTOCOL_FITZQUAKE
#define	SND_LARGEENTITY	(1<<3)		// entity as a short and channel as a byte
#define	SND_LARGESOUND	(1<<4)		// sound number as a short

// PROTOCOL_FITZQUAKE: flags byte of svc_spawnbaseline2 and svc_spawnstatic2
#define	B_LARGEMODEL	(1<<0)		// modelindex is a short
#define	B_LARGEFRAME	(1<<1)		// frame is a short
#define	B_ALPHA			(1<<2)		// 1 byte; read and ignored here
#define	B_SCALE			(1<<3)		// 1 byte; read and ignored here


// defaults for clientinfo messages
#define	DEFAULT_VIEWHEIGHT	22


// game types sent by serverinfo
// these determine which intermission screen plays
#define	GAME_COOP			0
#define	GAME_DEATHMATCH		1

//==================
// note that there are some defs.qc that mirror to these numbers
// also related to svc_strings[] in cl_parse
//==================

//
// server to client
//
#define	svc_bad				0
#define	svc_nop				1
#define	svc_disconnect		2
#define	svc_updatestat		3	// [byte] [long]
#define	svc_version			4	// [long] server version
#define	svc_setview			5	// [short] entity number
#define	svc_sound			6	// <see code>
#define	svc_time			7	// [float] server time
#define	svc_print			8	// [string] null terminated string
#define	svc_stufftext		9	// [string] stuffed into client's console buffer
								// the string should be \n terminated
#define	svc_setangle		10	// [angle3] set the view angle to this absolute value
	
#define	svc_serverinfo		11	// [long] version
						// [string] signon string
						// [string]..[0]model cache
						// [string]...[0]sounds cache
#define	svc_lightstyle		12	// [byte] [string]
#define	svc_updatename		13	// [byte] [string]
#define	svc_updatefrags		14	// [byte] [short]
#define	svc_clientdata		15	// <shortbits + data>
#define	svc_stopsound		16	// <see code>
#define	svc_updatecolors	17	// [byte] [byte]
#define	svc_particle		18	// [vec3] <variable>
#define	svc_damage			19
	
#define	svc_spawnstatic		20
//	svc_spawnbinary		21
#define	svc_spawnbaseline	22
	
#define	svc_temp_entity		23

#define	svc_setpause		24	// [byte] on / off
#define	svc_signonnum		25	// [byte]  used for the signon sequence

#define	svc_centerprint		26	// [string] to put in center of the screen

#define	svc_killedmonster	27
#define	svc_foundsecret		28

#define	svc_spawnstaticsound	29	// [coord3] [byte] samp [byte] vol [byte] aten

#define	svc_intermission	30		// [string] music
#define	svc_finale			31		// [string] music [string] text

#define	svc_cdtrack			32		// [byte] track [byte] looptrack
#define svc_sellscreen		33

#define svc_cutscene		34

// PROTOCOL_FITZQUAKE
#define	svc_skybox				37	// [string] name; read and ignored here
#define	svc_bf					40	// the bonus flash
#define	svc_fog					41	// [byte] density [byte] r g b [short] time
#define	svc_spawnbaseline2		42	// svc_spawnbaseline with a flags byte
#define	svc_spawnstatic2		43	// svc_spawnstatic with a flags byte
#define	svc_spawnstaticsound2	44	// svc_spawnstaticsound, sound as a short

// the 2021 re-release. Its QuakeC writes this one itself, with WriteByte and
// WriteString, so any server running those progs sends it.
#define	svc_achievement			52	// [string] achievement id

//
// client to server
//
#define	clc_bad			0
#define	clc_nop 		1
#define	clc_disconnect	2
#define	clc_move		3			// [usercmd_t]
#define	clc_stringcmd	4		// [string] message


//
// temp entity events
//
#define	TE_SPIKE			0
#define	TE_SUPERSPIKE		1
#define	TE_GUNSHOT			2
#define	TE_EXPLOSION		3
#define	TE_TAREXPLOSION		4
#define	TE_LIGHTNING1		5
#define	TE_LIGHTNING2		6
#define	TE_WIZSPIKE			7
#define	TE_KNIGHTSPIKE		8
#define	TE_LIGHTNING3		9
#define	TE_LAVASPLASH		10
#define	TE_TELEPORT			11
#define TE_EXPLOSION2		12

// PGM 01/21/97 
#define TE_BEAM				13
// PGM 01/21/97 

#ifdef QUAKE2
#define TE_IMPLOSION		14
#define TE_RAILTRAIL		15
#endif

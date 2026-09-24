# Quake (WinQuake, 1999 GPL release) for a current 64-bit Linux.
#
# The original Makefile.linuxi386 built five targets against egcs 1.1.2, an
# i386 assembler, SVGAlib, 3dfx Glide and Mesa 2.6. This builds the one that
# still makes sense -- the X11 software renderer -- as portable C, because the
# hand-written assembler is 32-bit only and quakedef.h already turns it off
# (id386 is 0 wherever __i386__ is not defined, which is here).
#
#   make            # -> linux/xquake
#   make SOUND=oss  # the 1996 /dev/dsp backend instead of the streaming one
#   make clean
#
# See ../PORTING-NOTES.md for what had to change in the sources themselves.

BUILDDIR = linux

CC      ?= gcc
STRIP   ?= strip

# -fno-strict-aliasing is not optional. The renderer reads floats through
# integer pointers to get at their exponents -- see r_main.c and d_polyse.c --
# which gcc 3 and later are entitled to assume never happens.
# -fwrapv likewise: the fixed-point maths overflows deliberately in places.
BASE_CFLAGS = -Dstricmp=strcasecmp -DX11 -D_GNU_SOURCE \
              -fno-strict-aliasing -fwrapv -fcommon

# Warnings the 1996 sources trip over constantly and that say nothing about
# whether the port works. Everything else is left on and has been dealt with.
QUIET_WARNINGS = -Wno-unused-result -Wno-unused-but-set-variable \
                 -Wno-unused-variable -Wno-unused-function \
                 -Wno-stringop-truncation -Wno-format-truncation \
                 -Wno-misleading-indentation -Wno-char-subscripts

OPT_CFLAGS = -O2 -ffast-math -funroll-loops -fomit-frame-pointer
DEBUG_CFLAGS = -O0 -g3 -fno-omit-frame-pointer

ifeq ($(DEBUG),1)
CFLAGS ?= $(DEBUG_CFLAGS)
else
CFLAGS ?= $(OPT_CFLAGS)
endif

ALL_CFLAGS = $(BASE_CFLAGS) $(QUIET_WARNINGS) $(CFLAGS) $(MUSIC_CFLAGS) $(SND_CFLAGS) $(EXTRA_CFLAGS)

# -lXrandr is what lets the video menu resize the X screen as well as the
# window; see VID_SetScreenSize in vid_x.c.
XLDFLAGS = -lX11 -lXext -lXrandr
# -rdynamic puts the engine's own symbols in the dynamic symbol table, which is
# where backtrace_symbols_fd looks. Without it a crash backtrace is a column of
# addresses; with it, function names. See Sys_CrashHandler in sys_linux.c.
LDFLAGS_ALL = $(XLDFLAGS) -rdynamic -lm -lz $(MUSIC_LDFLAGS) $(EXTRA_LDFLAGS)

# Which sound backend to link. "stream" writes the mixed PCM to a pipe for
# audiostream to pick up, which is how sound reaches the browser; "oss" is the
# 1996 /dev/dsp code, kept for a machine that still has one; "none" is silence.
SOUND ?= stream

# Where the music comes from. "sndfile" decodes Ogg Vorbis, FLAC, WAV, Opus
# and, where the installed libsndfile has it, MP3, from a music directory --
# because Quake's music was CD audio tracks and the container has no drive.
# "cd" is the 1996 /dev/cdrom code; "none" leaves music out entirely.
MUSIC ?= sndfile

# snd_null.c is not a DMA backend: it stubs out the whole of the sound system,
# S_StartSound and all, so it replaces the mixer rather than sitting under it.
MIXER_OBJS = $(BUILDDIR)/snd_dma.o $(BUILDDIR)/snd_mem.o $(BUILDDIR)/snd_mix.o

ifeq ($(SOUND),oss)
SND_OBJ = $(MIXER_OBJS) $(BUILDDIR)/snd_linux.o
else
ifeq ($(SOUND),none)
SND_OBJ = $(BUILDDIR)/snd_null.o
else
SND_OBJ = $(MIXER_OBJS) $(BUILDDIR)/snd_stream.o
# snd_stream.c keeps the playback position as a running count, so GetSoundtime
# can ask for it instead of reconstructing it from a ring position. See
# SNDDMA_GetSamples there for why that is not a tidying-up.
SND_CFLAGS = -DSND_HAS_GETSAMPLES
endif
endif

# Music is mixed into the sound mixer's paint buffer, so with no mixer there is
# nowhere for it to go: cd_stream.c reads shm->speed to know what to resample
# to, and snd_null.c does not define shm at all. Rather than fail at the link
# with "undefined reference to shm", say what happened.
#
# override, because a plain assignment loses to a value given on the command
# line -- which is exactly where MUSIC comes from when this matters.
ifeq ($(SOUND),none)
ifneq ($(MUSIC),none)
$(warning SOUND=none has no mixer for the music to go into; building MUSIC=none)
endif
override MUSIC := none
endif

ifeq ($(MUSIC),cd)
CD_OBJ = $(BUILDDIR)/cd_linux.o
else
ifeq ($(MUSIC),none)
CD_OBJ = $(BUILDDIR)/cd_stream.o
else
CD_OBJ = $(BUILDDIR)/cd_stream.o
MUSIC_CFLAGS = -DQUAKE_MUSIC_SNDFILE $(shell pkg-config --cflags sndfile)
MUSIC_LDFLAGS = $(shell pkg-config --libs sndfile)
endif
endif

COMMON_SRCS = \
	cl_demo.c cl_input.c cl_main.c cl_parse.c cl_tent.c chase.c cmd.c \
	common.c console.c crc.c cvar.c draw.c d_edge.c d_fill.c d_init.c \
	d_modech.c d_part.c d_polyse.c d_scan.c d_sky.c d_sprite.c d_surf.c \
	d_vars.c d_zpoint.c host.c host_cmd.c keys.c menu.c mathlib.c model.c \
	net_dgrm.c net_loop.c net_main.c net_vcr.c net_udp.c net_bsd.c \
	nonintel.c pr_cmds.c pr_edict.c pr_exec.c r_aclip.c r_alias.c r_bsp.c \
	r_light.c r_draw.c r_efrag.c r_edge.c r_misc.c r_main.c r_sky.c r_fog.c r_alpha.c r_tint.c \
	r_fence.c \
	localize.c \
	r_sprite.c r_surf.c r_part.c r_vars.c screen.c sbar.c sv_main.c \
	sv_phys.c sv_move.c sv_user.c zone.c view.c wad.c world.c \
	sys_linux.c vid_x.c

OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(COMMON_SRCS)) $(SND_OBJ) $(CD_OBJ)

TARGET = $(BUILDDIR)/xquake

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $(OBJS) $(LDFLAGS_ALL)

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ -c $<

# The original Makefile listed no header dependencies at all, so editing
# quakedef.h rebuilt nothing and left a binary half-built against the old one.
-include $(OBJS:.o=.d)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

strip: $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean strip

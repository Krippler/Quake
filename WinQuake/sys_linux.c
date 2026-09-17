#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <execinfo.h>
#include <stdint.h>

#include "quakedef.h"

qboolean			isDedicated;

// Raised by the signal handler in vid_x.c, acted on by the frame loop below.
extern volatile sig_atomic_t	sys_signalquit;

int nostdout = 0;

char *basedir = ".";
char *cachedir = "/tmp";

cvar_t  sys_linerefresh = {"sys_linerefresh","0"};// set for entity display

// Set to 1 to go back to the original behaviour of spinning between frames.
// Worth a try if a frame arrives late on a machine whose timer slices coarsely.
cvar_t  sys_nosleep = {"sys_nosleep","0"};

// =======================================================================
// General routines
// =======================================================================

void Sys_DebugNumber(int y, int val)
{
}

/*
void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;
	char		text[1024];
	
	va_start (argptr,fmt);
	vsprintf (text,fmt,argptr);
	va_end (argptr);
	fprintf(stderr, "%s", text);
	
	Con_Print (text);
}

void Sys_Printf (char *fmt, ...)
{

    va_list     argptr;
    char        text[1024], *t_p;
    int         l, r;

	if (nostdout)
		return;

    va_start (argptr,fmt);
    vsprintf (text,fmt,argptr);
    va_end (argptr);

    l = strlen(text);
    t_p = text;

// make sure everything goes through, even though we are non-blocking
    while (l)
    {
        r = write (1, text, l);
        if (r != l)
            sleep (0);
        if (r > 0)
        {
            t_p += r;
            l -= r;
        }
    }

}
*/

void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;
	char		text[1024];
	unsigned char		*p;

// The original wrote with vsprintf and then checked whether it had
// overflowed, which it could only do from inside the wreckage. Bound the
// write instead; nothing here needs more than a kilobyte of message.
	va_start (argptr,fmt);
	vsnprintf (text,sizeof(text),fmt,argptr);
	va_end (argptr);

    if (nostdout)
        return;

	for (p = (unsigned char *)text; *p; p++) {
		*p &= 0x7f;
		if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
			printf("[%02x]", *p);
		else
			putc(*p, stdout);
	}
}

#if 0
static char end1[] =
"\x1b[?7h\x1b[40m\x1b[2J\x1b[0;1;41m\x1b[1;1H                QUAKE: The Doomed Dimension \x1b[33mby \x1b[44mid\x1b[41m Software                      \x1b[2;1H  ----------------------------------------------------------------------------  \x1b[3;1H           CALL 1-800-IDGAMES TO ORDER OR FOR TECHNICAL SUPPORT                 \x1b[4;1H             PRICE: $45.00 (PRICES MAY VARY OUTSIDE THE US.)                    \x1b[5;1H                                                                                \x1b[6;1H  \x1b[37mYes! You only have one fourth of this incredible epic. That is because most   \x1b[7;1H   of you have paid us nothing or at most, very little. You could steal the     \x1b[8;1H   game from a friend. But we both know you'll be punished by God if you do.    \x1b[9;1H        \x1b[33mWHY RISK ETERNAL DAMNATION? CALL 1-800-IDGAMES AND BUY NOW!             \x1b[10;1H             \x1b[37mRemember, we love you almost as much as He does.                   \x1b[11;1H                                                                                \x1b[12;1H            \x1b[33mProgramming: \x1b[37mJohn Carmack, Michael Abrash, John Cash                \x1b[13;1H       \x1b[33mDesign: \x1b[37mJohn Romero, Sandy Petersen, American McGee, Tim Willits         \x1b[14;1H                     \x1b[33mArt: \x1b[37mAdrian Carmack, Kevin Cloud                           \x1b[15;1H               \x1b[33mBiz: \x1b[37mJay Wilbur, Mike Wilson, Donna Jackson                      \x1b[16;1H            \x1b[33mProjects: \x1b[37mShawn Green   \x1b[33mSupport: \x1b[37mBarrett Alexander                  \x1b[17;1H              \x1b[33mSound Effects: \x1b[37mTrent Reznor and Nine Inch Nails                   \x1b[18;1H  For other information or details on ordering outside the US, check out the    \x1b[19;1H     files accompanying QUAKE or our website at http://www.idsoftware.com.      \x1b[20;1H    \x1b[0;41mQuake is a trademark of Id Software, inc., (c)1996 Id Software, inc.        \x1b[21;1H     All rights reserved. NIN logo is a registered trademark licensed to        \x1b[22;1H                 Nothing Interactive, Inc. All rights reserved.                 \x1b[40m\x1b[23;1H\x1b[0m";
static char end2[] =
"\x1b[?7h\x1b[40m\x1b[2J\x1b[0;1;41m\x1b[1;1H        QUAKE \x1b[33mby \x1b[44mid\x1b[41m Software                                                    \x1b[2;1H -----------------------------------------------------------------------------  \x1b[3;1H        \x1b[37mWhy did you quit from the registered version of QUAKE? Did the          \x1b[4;1H        scary monsters frighten you? Or did Mr. Sandman tug at your             \x1b[5;1H        little lids? No matter! What is important is you love our               \x1b[6;1H        game, and gave us your money. Congratulations, you are probably         \x1b[7;1H        not a thief.                                                            \x1b[8;1H                                                           Thank You.           \x1b[9;1H        \x1b[33;44mid\x1b[41m Software is:                                                         \x1b[10;1H        PROGRAMMING: \x1b[37mJohn Carmack, Michael Abrash, John Cash                    \x1b[11;1H        \x1b[33mDESIGN: \x1b[37mJohn Romero, Sandy Petersen, American McGee, Tim Willits        \x1b[12;1H        \x1b[33mART: \x1b[37mAdrian Carmack, Kevin Cloud                                        \x1b[13;1H        \x1b[33mBIZ: \x1b[37mJay Wilbur, Mike Wilson     \x1b[33mPROJECTS MAN: \x1b[37mShawn Green              \x1b[14;1H        \x1b[33mBIZ ASSIST: \x1b[37mDonna Jackson        \x1b[33mSUPPORT: \x1b[37mBarrett Alexander             \x1b[15;1H        \x1b[33mSOUND EFFECTS AND MUSIC: \x1b[37mTrent Reznor and Nine Inch Nails               \x1b[16;1H                                                                                \x1b[17;1H        If you need help running QUAKE refer to the text files in the           \x1b[18;1H        QUAKE directory, or our website at http://www.idsoftware.com.           \x1b[19;1H        If all else fails, call our technical support at 1-800-IDGAMES.         \x1b[20;1H      \x1b[0;41mQuake is a trademark of Id Software, inc., (c)1996 Id Software, inc.      \x1b[21;1H        All rights reserved. NIN logo is a registered trademark licensed        \x1b[22;1H             to Nothing Interactive, Inc. All rights reserved.                  \x1b[23;1H\x1b[40m\x1b[0m";

#endif
void Sys_Quit (void)
{
	Host_Shutdown();
    fcntl (0, F_SETFL, fcntl (0, F_GETFL, 0) & ~FNDELAY);
#if 0
	if (registered.value)
		printf("%s", end2);
	else
		printf("%s", end1);
#endif
	fflush(stdout);
	exit(0);
}

void Sys_Init(void)
{
	Cvar_RegisterVariable (&sys_nosleep);
#if id386
	Sys_SetFPCW();
#endif
}

void Sys_Error (char *error, ...)
{ 
    va_list     argptr;
    char        string[1024];

// change stdin to non blocking
    fcntl (0, F_SETFL, fcntl (0, F_GETFL, 0) & ~FNDELAY);
    
    va_start (argptr,error);
    vsnprintf (string,sizeof(string),error,argptr);
    va_end (argptr);
	fprintf(stderr, "Error: %s\n", string);
	fflush (stderr);

	Host_Shutdown ();
	exit (1);

} 

void Sys_Warn (char *warning, ...)
{ 
    va_list     argptr;
    char        string[1024];
    
    va_start (argptr,warning);
    vsnprintf (string,sizeof(string),warning,argptr);
    va_end (argptr);
	fprintf(stderr, "Warning: %s", string);
} 

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int	Sys_FileTime (char *path)
{
	struct	stat	buf;
	
	if (stat (path,&buf) == -1)
		return -1;
	
	return buf.st_mtime;
}


void Sys_mkdir (char *path)
{
    mkdir (path, 0777);
}

int Sys_FileOpenRead (char *path, int *handle)
{
	int	h;
	struct stat	fileinfo;
    
	
	h = open (path, O_RDONLY, 0666);
	*handle = h;
	if (h == -1)
		return -1;
	
	if (fstat (h,&fileinfo) == -1)
		Sys_Error ("Error fstating %s", path);

	return fileinfo.st_size;
}

int Sys_FileOpenWrite (char *path)
{
	int     handle;

	umask (0);
	
	handle = open(path,O_RDWR | O_CREAT | O_TRUNC
	, 0666);

	if (handle == -1)
		Sys_Error ("Error opening %s: %s", path,strerror(errno));

	return handle;
}

int Sys_FileWrite (int handle, void *src, int count)
{
	return write (handle, src, count);
}

void Sys_FileClose (int handle)
{
	close (handle);
}

void Sys_FileSeek (int handle, int position)
{
	lseek (handle, position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
    return read (handle, dest, count);
}

void Sys_DebugLog(char *file, char *fmt, ...)
{
    va_list argptr; 
    static char data[1024];
    int fd;
    
    va_start(argptr, fmt);
    vsnprintf(data, sizeof(data), fmt, argptr);
    va_end(argptr);
//    fd = open(file, O_WRONLY | O_BINARY | O_CREAT | O_APPEND, 0666);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
}

void Sys_EditFile(char *filename)
{

	char cmd[256];
	char *term;
	char *editor;

	term = getenv("TERM");
	if (term && !strcmp(term, "xterm"))
	{
		editor = getenv("VISUAL");
		if (!editor)
			editor = getenv("EDITOR");
		if (!editor)
			editor = getenv("EDIT");
		if (!editor)
			editor = "vi";
		sprintf(cmd, "xterm -e %s %s", editor, filename);
		system(cmd);
	}

}

double Sys_FloatTime (void)
{
	struct timespec tp;
	static time_t	secbase;

// CLOCK_MONOTONIC, not gettimeofday: the wall clock steps when NTP corrects
// it or the host suspends, and the engine reads a step as elapsed frame time.
// Backwards it stops dead; forwards it runs a single frame of physics for
// however long the jump was.
	if (clock_gettime (CLOCK_MONOTONIC, &tp) != 0)
	{
		struct timeval tv;

		gettimeofday (&tv, NULL);
		tp.tv_sec = tv.tv_sec;
		tp.tv_nsec = tv.tv_usec * 1000;
	}

	if (!secbase)
	{
		secbase = tp.tv_sec;
		return tp.tv_nsec / 1000000000.0;
	}

	return (tp.tv_sec - secbase) + tp.tv_nsec / 1000000000.0;
}

// =======================================================================
// Sleeps for microseconds
// =======================================================================

static volatile int oktogo;

void alarm_handler(int x)
{
	oktogo=1;
}

void Sys_LineRefresh(void)
{
}

void floating_point_exception_handler(int whatever)
{
//	Sys_Warn("floating point exception\n");
	signal(SIGFPE, floating_point_exception_handler);
}

char *Sys_ConsoleInput(void)
{
    static char text[256];
    int     len;
	fd_set	fdset;
    struct timeval timeout;

	if (cls.state == ca_dedicated) {
		FD_ZERO(&fdset);
		FD_SET(0, &fdset); // stdin
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		if (select (1, &fdset, NULL, NULL, &timeout) == -1 || !FD_ISSET(0, &fdset))
			return NULL;

		len = read (0, text, sizeof(text));
		if (len < 1)
			return NULL;
		text[len-1] = 0;    // rip off the /n and terminate

		return text;
	}
	return NULL;
}

#if !id386
void Sys_HighFPPrecision (void)
{
}

void Sys_LowFPPrecision (void)
{
}
#endif

/*
================
Crash reporting.

Two reasons a crash in the container told nobody anything.

The engine's stdout is a pipe -- docker's -- so the C library block-buffers
it, and the whole startup log sits in a 4 KB buffer that a SIGSEGV never
flushes. A container that died during Host_Init printed literally nothing
between the entrypoint's "running: xquake" and the shell's "Segmentation
fault", so the one question worth asking -- how far did it get -- had no
answer. That is what the setvbuf below is for.

And there was no handler, so there was no backtrace either. This one is
written with write() and no formatting library, because a signal handler may
not call printf: backtrace_symbols_fd is the variant that does not allocate,
which is exactly why it exists. Frame names need -rdynamic and an unstripped
binary; the Makefile passes the first and the Dockerfile no longer strips.

It re-raises rather than exiting, so the exit status is still 139 and the
entrypoint's restart logic reads the signal as it always did.
================
*/
static void Sys_WriteStr (const char *s)
{
	size_t	n = 0;

	while (s[n])
		n++;
	if (write (2, s, n) < 0)
		return;					// nothing useful to do about it in here
}

static void Sys_WriteHex (unsigned long v)
{
	char	buf[2 + sizeof(v) * 2];
	int		i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < (int)sizeof(v) * 2; i++)
		buf[2 + i] = "0123456789abcdef"[(v >> ((sizeof(v) * 2 - 1 - i) * 4)) & 15];

	if (write (2, buf, sizeof(buf)) < 0)
		return;
}

static void Sys_CrashHandler (int sig, siginfo_t *info, void *ucontext)
{
	void	*frames[32];
	int		n;

	Sys_WriteStr ("\n=== Quake died on signal ");
	switch (sig)
	{
	case SIGSEGV:	Sys_WriteStr ("SIGSEGV (bad address)");		break;
	case SIGBUS:	Sys_WriteStr ("SIGBUS (bad alignment)");	break;
	case SIGFPE:	Sys_WriteStr ("SIGFPE (arithmetic)");		break;
	case SIGILL:	Sys_WriteStr ("SIGILL (illegal instruction)"); break;
	case SIGABRT:	Sys_WriteStr ("SIGABRT (aborted)");			break;
	default:		Sys_WriteStr ("an unexpected signal");		break;
	}

	if (info && (sig == SIGSEGV || sig == SIGBUS))
	{
		Sys_WriteStr (" at ");
		Sys_WriteHex ((unsigned long)(uintptr_t)info->si_addr);
	}
	Sys_WriteStr (" ===\n");

// Everything the engine printed on its way here, which line buffering has
// already sent -- but a partial line may still be held, and it is often the
// most informative one.
	fflush (stdout);

	n = backtrace (frames, (int)(sizeof(frames) / sizeof(frames[0])));
	backtrace_symbols_fd (frames, n, 2);

	Sys_WriteStr ("=== please include the lines above in a bug report ===\n");

// Back to the default disposition, then let it happen again, so the exit
// status is the signal rather than whatever this handler returned.
	signal (sig, SIG_DFL);
	raise (sig);
}

static void Sys_InitCrashHandler (void)
{
	struct sigaction	sa;
	int					i;
	static const int	sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

	memset (&sa, 0, sizeof(sa));
	sa.sa_sigaction = Sys_CrashHandler;
	sigemptyset (&sa.sa_mask);
// SA_ONSTACK so a stack overflow -- which is one way to get here -- still has
// somewhere to run the handler.
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;

	for (i = 0; i < (int)(sizeof(sigs) / sizeof(sigs[0])); i++)
		sigaction (sigs[i], &sa, 0);
}


int main (int c, char **v)
{

	double		time, oldtime, newtime;
	quakeparms_t parms;
	extern int vcrFile;
	extern int recording;
	int j;

//	static char cwd[1024];

//	signal(SIGFPE, floating_point_exception_handler);
//
// Line buffering, before anything is printed.
//
// stdout is a pipe under docker, so the library would block-buffer it and a
// crash would take the entire log with it -- see Sys_CrashHandler above. A
// line at a time costs one write per Con_Printf, which against the cost of
// drawing a frame is nothing.
//
	setvbuf (stdout, NULL, _IOLBF, 0);

	Sys_InitCrashHandler ();

	signal(SIGFPE, SIG_IGN);

	memset(&parms, 0, sizeof(parms));

	COM_InitArgv(c, v);
	parms.argc = com_argc;
	parms.argv = com_argv;

// 8 MB was the 1996 default and it is not enough here. Every pointer in the
// model, edict and surface caches is twice the width it was, and the hunk is
// where all of them live. The high end of the same hunk holds the z-buffer
// and the surface cache, which at 1920x1200 are 12 MB between them where at
// 320x200 they were under a megabyte. 64 MB is still small enough to be
// uninteresting; -mem <megabytes> overrides it either way.
	parms.memsize = 64*1024*1024;

	j = COM_CheckParm("-mem");
	if (j && j < com_argc-1)
		parms.memsize = (int) (Q_atof(com_argv[j+1]) * 1024 * 1024);
	if (parms.memsize < MINIMUM_MEMORY)
		parms.memsize = MINIMUM_MEMORY;
	parms.membase = malloc (parms.memsize);
	if (!parms.membase)
		Sys_Error ("Could not allocate %d bytes for the heap", parms.memsize);

	parms.basedir = basedir;
// caching is disabled by default, use -cachedir to enable
//	parms.cachedir = cachedir;

	fcntl(0, F_SETFL, fcntl (0, F_GETFL, 0) | FNDELAY);

    Host_Init(&parms);

	Sys_Init();

	if (COM_CheckParm("-nostdout"))
		nostdout = 1;
	else {
		fcntl(0, F_SETFL, fcntl (0, F_GETFL, 0) | FNDELAY);
		printf ("Linux Quake -- Version %0.3f\n", LINUX_VERSION);
	}

    oldtime = Sys_FloatTime () - 0.1;
    while (1)
    {
// find time spent rendering last frame
        newtime = Sys_FloatTime ();
        time = newtime - oldtime;

        if (cls.state == ca_dedicated)
        {   // play vcrfiles at max speed
            if (time < sys_ticrate.value && (vcrFile == -1 || recording) )
            {
				usleep(1);
                continue;       // not time to run a server only tic yet
            }
            time = sys_ticrate.value;
        }

        if (time > sys_ticrate.value*2)
            oldtime = newtime;
        else
            oldtime += time;

        Host_Frame (time);

// A signal asked us to stop. The handler only raised the flag; the actual
// shutdown -- writing config.cfg, flushing the sound, closing the display --
// happens here, where calling into Xlib and stdio is allowed.
        if (sys_signalquit)
        {
            printf ("\nReceived signal %d, shutting down\n",
                    (int)sys_signalquit);
            Sys_Quit ();
        }

// Host_Frame returns without doing anything until a frame's worth of time has
// passed, so without this the loop simply spins, and the original had nothing
// here. On a desktop in 1996 that was the whole machine anyway. In a
// container it is a core pinned at 100% whether or not anything is happening,
// which on a shared host is somebody else's problem as well as yours.
        if (!sys_nosleep.value)
        {
            double  spare = (1.0 / 72.0) - (Sys_FloatTime () - newtime);

            if (spare > 0.0005)
                usleep ((useconds_t)((spare - 0.0005) * 1000000.0));
        }

// graphic debugging aids
        if (sys_linerefresh.value)
            Sys_LineRefresh ();
    }

}


/*
================
Sys_MakeCodeWriteable
================
*/
void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{

	int r;
	unsigned long addr;
	int psize = getpagesize();

	addr = (startaddr & ~(psize-1)) - psize;

//	fprintf(stderr, "writable code %lx(%lx)-%lx, length=%lx\n", startaddr,
//			addr, startaddr+length, length);

	r = mprotect((char*)addr, length + startaddr - addr + psize, 7);

	if (r < 0)
    		Sys_Error("Protection change failed\n");

}


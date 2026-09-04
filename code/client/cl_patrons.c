/*
===========================================================================
cl_patrons.c -- Gold Patron credits, shown briefly when the game exits.

A thank-you screen for the people funding the VR port, matching what the other
Team Beef ports do.  The names live in a plain text file (baseEF/patrons.txt)
rather than in code, so the list can be updated between releases without a
rebuild -- the file format follows OpenJKDF2's resource/patrons.txt.

Drawn by the engine, not the UI DLL: "quit" can come from the menu, from the
console, or from a VM, and the UI module is torn down during shutdown.  Doing
it here also means the VR path is already handled -- SCR_UpdateScreen owns the
OpenXR frame, and VR_UseScreenLayer() reports the screen layer while this is up,
so the credits appear on the flat virtual screen in the headset.
===========================================================================
*/

#include "client.h"

#ifdef BUILD_VR
#include "../vr/VrBase.h"
#endif

// Pumps the OS window/input queue (sdl_input.c; a no-op on Android, where the
// activity glue owns the event pump).  Declared in sys/sys_local.h, which the
// client does not otherwise include.
extern void IN_Frame( void );

#define PATRONS_FILE        "patrons.txt"
#define PATRONS_MAX_GOLD    256
#define PATRONS_MAX_OTHER   8
#define PATRONS_LINE_MAX    64

// Virtual 640x480 layout (SCR_AdjustFrom640 stretches it to the real target,
// so this fills the desktop window and the VR virtual screen alike).
#define PATRONS_SCREEN_W    640
#define PATRONS_SCREEN_H    480
#define PATRONS_MARGIN_TOP  22
#define PATRONS_MARGIN_BOT  14
#define PATRONS_MARGIN_SIDE 16

#define PATRONS_TITLE_SIZE  26
#define PATRONS_FOOTER_SIZE 12
// Name cell size bounds, in 640-space units (a char is size x size).
#define PATRONS_NAME_MAX    20
#define PATRONS_NAME_MIN     7
#define PATRONS_MAX_COLS     4

// How long before a key/controller press can dismiss it.  Without a grace
// period the button still held down from picking "Quit" would skip it instantly.
#define PATRONS_GRACE_MS    1200

static char     patrons_title[PATRONS_LINE_MAX];
static char     patrons_gold[PATRONS_MAX_GOLD][PATRONS_LINE_MAX];
static char     patrons_other[PATRONS_MAX_OTHER][PATRONS_LINE_MAX];
static int      patrons_numGold;
static int      patrons_numOther;
static qboolean patrons_loaded;
static qboolean patrons_active;

cvar_t          *cl_patronsTime;

qboolean CL_Patrons_Active( void ) {
	return patrons_active;
}

static void CL_Patrons_Trim( char *s ) {
	int len, i, j;

	for ( len = 0; s[len]; len++ ) {
		;
	}
	while ( len > 0 && ( s[len-1] == '\r' || s[len-1] == '\n' ||
						 s[len-1] == ' '  || s[len-1] == '\t' ) ) {
		s[--len] = '\0';
	}
	for ( i = 0; s[i] == ' ' || s[i] == '\t'; i++ ) {
		;
	}
	if ( i > 0 ) {
		for ( j = 0; s[i]; ) {
			s[j++] = s[i++];
		}
		s[j] = '\0';
	}
}

/*
===============
CL_Patrons_Init

Parses baseEF/patrons.txt.  Sections are [title], [gold] and [other]; '#'
comments and blank lines are ignored.  A missing file is not an error -- the
screen is simply skipped.
===============
*/
void CL_Patrons_Init( void ) {
	union { char *c; void *v; } f;
	int   len;
	char *p, *line;
	int   section = 0;   /* 0 none, 1 title, 2 gold, 3 other */

	cl_patronsTime = Cvar_Get( "cl_patronsTime", "7", CVAR_ARCHIVE );

	patrons_numGold  = 0;
	patrons_numOther = 0;
	patrons_title[0] = '\0';
	patrons_loaded   = qfalse;

	len = FS_ReadFile( PATRONS_FILE, &f.v );
	if ( !f.c || len <= 0 ) {
		return;
	}

	p = f.c;
	while ( p && *p ) {
		char buf[PATRONS_LINE_MAX];
		char *nl = strchr( p, '\n' );

		line = p;
		if ( nl ) {
			*nl = '\0';
			p = nl + 1;
		} else {
			p = NULL;
		}

		Q_strncpyz( buf, line, sizeof( buf ) );
		CL_Patrons_Trim( buf );

		// The console charset is ASCII only -- a stray UTF-8 byte would draw as
		// two junk glyphs, so show one '?' instead.  Names in patrons.txt should
		// be transliterated (o for o-umlaut, and so on).
		{
			int c;
			for ( c = 0; buf[c]; c++ ) {
				if ( (unsigned char)buf[c] > 126 ) {
					buf[c] = '?';
				}
			}
		}

		if ( !buf[0] || buf[0] == '#' ) {
			continue;
		}

		if ( !Q_stricmp( buf, "[title]" ) ) { section = 1; continue; }
		if ( !Q_stricmp( buf, "[gold]"  ) ) { section = 2; continue; }
		if ( !Q_stricmp( buf, "[other]" ) ) { section = 3; continue; }

		switch ( section ) {
		case 1:
			Q_strncpyz( patrons_title, buf, sizeof( patrons_title ) );
			break;
		case 2:
			if ( patrons_numGold < PATRONS_MAX_GOLD ) {
				Q_strncpyz( patrons_gold[patrons_numGold++], buf, PATRONS_LINE_MAX );
			}
			break;
		case 3:
			if ( patrons_numOther < PATRONS_MAX_OTHER ) {
				Q_strncpyz( patrons_other[patrons_numOther++], buf, PATRONS_LINE_MAX );
			}
			break;
		default:
			break;
		}
	}

	FS_FreeFile( f.v );

	patrons_loaded = (qboolean)( patrons_numGold > 0 || patrons_numOther > 0 );
	if ( patrons_loaded ) {
		Com_Printf( "Loaded %d gold patrons from %s\n", patrons_numGold, PATRONS_FILE );
	}
}

static void CL_Patrons_DrawCentred( int y, const char *str, int size, float *color ) {
	int w = (int)strlen( str ) * size;
	int x = ( PATRONS_SCREEN_W - w ) / 2;

	if ( x < 0 ) {
		x = 0;
	}
	SCR_DrawStringExt( x, y, (float)size, str, color, qtrue, qtrue );
}

// Longest name in the list, in characters -- drives how wide a column has to be.
static int CL_Patrons_LongestGold( void ) {
	int i, longest = 1;

	for ( i = 0; i < patrons_numGold; i++ ) {
		int l = (int)strlen( patrons_gold[i] );
		if ( l > longest ) {
			longest = l;
		}
	}
	return longest;
}

/*
===============
CL_Patrons_Draw

Called from SCR_DrawScreenField while the credits are up.

Everything here is in 640x480 virtual space and goes through SCR_DrawStringExt,
which routes to SCR_DrawChar -> SCR_AdjustFrom640.  Do NOT use
SCR_DrawSmallStringExt: small chars are drawn at NATIVE resolution (see the
comment on SCR_DrawSmallChar), which on a headset eye buffer leaves them a tiny
unreadable clump in the corner while the adjusted text stretches over the top.

The column count and text size are picked to fill the space, so the list stays
readable as patrons are added rather than silently shrinking off the screen.
===============
*/
void CL_Patrons_Draw( void ) {
	static vec4_t gold   = { 1.00f, 0.82f, 0.30f, 1.0f };
	static vec4_t white  = { 1.00f, 1.00f, 1.00f, 1.0f };
	static vec4_t footer = { 0.80f, 0.80f, 0.80f, 1.0f };
	int titleSize = PATRONS_TITLE_SIZE;
	int longest, avail, top, footTop;
	int cols, bestCols, bestSize, size, rows, lineH, colW, i;

	SCR_FillRect( 0, 0, PATRONS_SCREEN_W, PATRONS_SCREEN_H, colorBlack );

	top = PATRONS_MARGIN_TOP;
	if ( patrons_title[0] ) {
		int titleLen = (int)strlen( patrons_title );
		// Shrink an over-long title rather than letting it run off the sides.
		if ( titleLen * titleSize > PATRONS_SCREEN_W - 2 * PATRONS_MARGIN_SIDE ) {
			titleSize = ( PATRONS_SCREEN_W - 2 * PATRONS_MARGIN_SIDE ) / titleLen;
		}
		CL_Patrons_DrawCentred( top, patrons_title, titleSize, white );
		top += titleSize + 18;
	}

	footTop = PATRONS_SCREEN_H - PATRONS_MARGIN_BOT
			  - patrons_numOther * ( PATRONS_FOOTER_SIZE + 2 );

	if ( patrons_numGold > 0 ) {
		avail   = footTop - top - 8;
		longest = CL_Patrons_LongestGold();

		// Pick the column count that lets the names be biggest: more columns give
		// shorter columns (taller rows) but narrower cells, so the two limits
		// trade off and the best answer is not always the same.
		bestCols = 1;
		bestSize = 0;
		for ( cols = 1; cols <= PATRONS_MAX_COLS; cols++ ) {
			int byH, byW;

			rows = ( patrons_numGold + cols - 1 ) / cols;
			byH  = ( avail / rows ) - 2;
			colW = ( PATRONS_SCREEN_W - 2 * PATRONS_MARGIN_SIDE ) / cols;
			byW  = colW / ( longest + 1 );

			size = ( byH < byW ) ? byH : byW;
			if ( size > bestSize ) {
				bestSize = size;
				bestCols = cols;
			}
		}

		size = bestSize;
		if ( size > PATRONS_NAME_MAX ) {
			size = PATRONS_NAME_MAX;
		}
		if ( size < PATRONS_NAME_MIN ) {
			size = PATRONS_NAME_MIN;
		}

		cols  = bestCols;
		rows  = ( patrons_numGold + cols - 1 ) / cols;
		lineH = size + 2;
		colW  = ( PATRONS_SCREEN_W - 2 * PATRONS_MARGIN_SIDE ) / cols;

		// Centre the whole block vertically in whatever space is left.
		top += ( avail - rows * lineH ) / 2;
		if ( top < PATRONS_MARGIN_TOP ) {
			top = PATRONS_MARGIN_TOP;
		}

		for ( i = 0; i < patrons_numGold; i++ ) {
			const char *name = patrons_gold[i];
			int col = i / rows;
			int row = i % rows;
			int w   = (int)strlen( name ) * size;
			int x   = PATRONS_MARGIN_SIDE + col * colW + ( colW - w ) / 2;

			if ( x < 0 ) {
				x = 0;
			}
			SCR_DrawStringExt( x, top + row * lineH, (float)size, name, gold, qtrue, qtrue );
		}
	}

	for ( i = 0; i < patrons_numOther; i++ ) {
		CL_Patrons_DrawCentred( footTop + i * ( PATRONS_FOOTER_SIZE + 2 ),
								patrons_other[i], PATRONS_FOOTER_SIZE, footer );
	}
}

/*
===============
CL_Patrons_ShowAndWait

Runs its own frame loop -- this is called from Com_Quit_f, after the game has
stopped driving Com_Frame.  Returns once the display time is up, or as soon as
the player presses something after the grace period.
===============
*/
void CL_Patrons_ShowAndWait( void ) {
	int start, elapsed, duration;

	if ( !patrons_loaded || com_dedicated->integer ) {
		return;
	}
	// No renderer means nothing to draw on (dedicated, or an early-startup quit).
	if ( !cls.rendererStarted || !cls.uiStarted ) {
		return;
	}
	if ( !cl_patronsTime ) {
		return;
	}
	duration = cl_patronsTime->integer * 1000;
	if ( duration <= 0 ) {
		return;
	}

	S_StopAllSounds();
	Key_ClearStates();

	patrons_active = qtrue;
	start = Sys_Milliseconds();

	for ( ;; ) {
		elapsed = Sys_Milliseconds() - start;
		if ( elapsed >= duration ) {
			break;
		}

		// Pump the window/input queue only -- deliberately NOT Com_EventLoop,
		// which would execute queued console commands while we are already on
		// our way out of Com_Quit_f.
		IN_Frame();

		SCR_UpdateScreen();

		if ( elapsed > PATRONS_GRACE_MS ) {
			if ( anykeydown ) {
				break;
			}
#ifdef BUILD_VR
			if ( VR_IsActive() && VR_GetControllerButtons() != 0 ) {
				break;
			}
#endif
		}
	}

	patrons_active = qfalse;
}

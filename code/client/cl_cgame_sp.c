/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
===========================================================================
cl_cgame_sp.c -- syscall dispatcher for the EF1 singleplayer cgame module

OVERVIEW
--------
This file implements the syscall dispatcher that bridges between the ioEF
engine and the precompiled EF1 singleplayer cgame DLL (efcgamex86.dll).

When the SP cgame module calls trap_* functions, those compile down to a
syscall with a numeric ID.  The DLL's vmMain calls into the engine with
an integer identifying which engine service it wants, and this dispatcher
maps that integer to the appropriate engine function.

WHY A SEPARATE DISPATCHER?
---------------------------
The SP cgame uses a completely different syscall numbering scheme from
ioEF's multiplayer cgame.  The numbers diverge because the SP module
includes several subsystems that don't exist in multiplayer:

  1. Force Feedback (FF) -- The original Ritual engine supported
     DirectInput force-feedback joysticks.  The SP cgame calls
     FF_StartFX, FF_EnsureFX, FF_StopFX, FF_StopAllFX.  These four
     syscalls (34-37) are inserted between the sound and renderer
     blocks, pushing all renderer syscall numbers up by 4 compared
     to ioEF MP.

  2. Extra Renderer Calls -- The SP cgame uses R_GetLighting (45),
     R_DrawScreenShot (51), R_DrawRotatePic (54), and R_Scissor (55).
     These shift later syscall numbers further.

  3. Ambient Sound System -- The SP campaign uses a sophisticated
     ambient sound system (S_UpdateAmbientSet, S_AddLocalSet,
     AS_ParseSets, AS_AddEntry, AS_GetBModelSound, S_GetSampleLength)
     at syscalls 65-70, appended after the main block.

The net effect is that almost every syscall number >= 34 differs between
SP and MP.  Rather than trying to remap numbers at the edges, we use a
clean separate dispatch table.

SNAPSHOT ARCHITECTURE NOTE
--------------------------
The most complex syscall here is SPCG_GETSNAPSHOT.  See its extensive
inline comments for why we bypass ioEF's delta-compression snapshot
system and instead build SP-format snapshots directly from the game
module's live data.
===========================================================================
*/

#ifdef ELITEFORCE

#include "client.h"
#include "snd_local.h"
#include "../qcommon/sp_types.h"

// Q_irand: random integer in [min, max] inclusive
static int Q_irand( int min, int max ) {
	if ( min >= max ) return min;
	return min + ( rand() % ( max - min + 1 ) );
}

/*
 * These functions are defined in sv_game_sp.c (the server-side SP bridge).
 * They are safe to call from the client because SP is always a local game --
 * the server and client run in the same process, sharing the same address
 * space.  There is never a remote SP server.
 *
 * SV_SP_GetRawPlayerState() returns a pointer directly into the SP game
 * module's gclient_t struct (which begins with sp_playerState_t).  We
 * memcpy from this pointer into the SP snapshot's ps field.
 *
 * SV_SP_GetRawEntityState() returns a pointer to the sp_entityState_t at
 * the start of a specific gentity_t slot in the game module's entity array.
 */
extern void *SV_SP_GetRawPlayerState( void );
extern sp_entityState_t *SV_SP_GetRawEntityState( int entNum );
extern sfx_t s_knownSfx[];
extern int s_numSfx;

#define CL_SP_CGAME_SCREENSHOT_SIZE   256
#define CL_SP_CGAME_SCREENSHOT_BYTES  ( CL_SP_CGAME_SCREENSHOT_SIZE * CL_SP_CGAME_SCREENSHOT_SIZE * 4 )

static byte		cl_sp_cgameScreenshot[CL_SP_CGAME_SCREENSHOT_BYTES];
static qboolean	cl_sp_cgameScreenshotValid;
static byte		*cl_sp_cgameCaptureBuffer;
static byte		*cl_sp_cgameEncodeBuffer;
static int		cl_sp_cgameCaptureWidth;
static int		cl_sp_cgameCaptureHeight;

static void CL_SP_FreeCgameCaptureBuffers( void ) {
	if ( cl_sp_cgameCaptureBuffer ) {
		Z_Free( cl_sp_cgameCaptureBuffer );
		cl_sp_cgameCaptureBuffer = NULL;
	}

	if ( cl_sp_cgameEncodeBuffer ) {
		Z_Free( cl_sp_cgameEncodeBuffer );
		cl_sp_cgameEncodeBuffer = NULL;
	}

	cl_sp_cgameCaptureWidth = 0;
	cl_sp_cgameCaptureHeight = 0;
}

static qboolean CL_SP_EnsureCgameCaptureBuffers( void ) {
	int paddedRowBytes;
	int bufferSize;

	if ( cls.glconfig.vidWidth <= 0 || cls.glconfig.vidHeight <= 0 ) {
		return qfalse;
	}

	if ( cl_sp_cgameCaptureBuffer &&
		cl_sp_cgameEncodeBuffer &&
		cl_sp_cgameCaptureWidth == cls.glconfig.vidWidth &&
		cl_sp_cgameCaptureHeight == cls.glconfig.vidHeight ) {
		return qtrue;
	}

	CL_SP_FreeCgameCaptureBuffers();

	cl_sp_cgameCaptureWidth = cls.glconfig.vidWidth;
	cl_sp_cgameCaptureHeight = cls.glconfig.vidHeight;
	paddedRowBytes = PAD( cl_sp_cgameCaptureWidth * 3, 4 );
	bufferSize = paddedRowBytes * cl_sp_cgameCaptureHeight + 4;

	cl_sp_cgameCaptureBuffer = Z_Malloc( bufferSize );
	cl_sp_cgameEncodeBuffer = Z_Malloc( bufferSize );
	if ( !cl_sp_cgameCaptureBuffer || !cl_sp_cgameEncodeBuffer ) {
		CL_SP_FreeCgameCaptureBuffers();
		return qfalse;
	}

	return qtrue;
}

static qboolean CL_SP_CaptureCgameScreenshot( void ) {
	const byte *sourcePixels;
	const byte *srcRow;
	const byte *src;
	int paddedRowBytes;
	int srcX;
	int srcY;
	int x;
	int y;
	byte *dst;
	byte *dstRow;

	if ( !CL_SP_EnsureCgameCaptureBuffers() ) {
		cl_sp_cgameScreenshotValid = qfalse;
		return qfalse;
	}

	re.TakeVideoFrame( cl_sp_cgameCaptureWidth, cl_sp_cgameCaptureHeight,
		cl_sp_cgameCaptureBuffer, cl_sp_cgameEncodeBuffer, qfalse );

	paddedRowBytes = PAD( cl_sp_cgameCaptureWidth * 3, 4 );
	sourcePixels = PADP( cl_sp_cgameCaptureBuffer, 4 );
	for ( y = 0; y < CL_SP_CGAME_SCREENSHOT_SIZE; y++ ) {
		srcY = cl_sp_cgameCaptureHeight - 1 - ( y * cl_sp_cgameCaptureHeight / CL_SP_CGAME_SCREENSHOT_SIZE );
		dstRow = cl_sp_cgameScreenshot + y * CL_SP_CGAME_SCREENSHOT_SIZE * 4;

		if ( srcY < 0 ) {
			srcY = 0;
		} else if ( srcY >= cl_sp_cgameCaptureHeight ) {
			srcY = cl_sp_cgameCaptureHeight - 1;
		}

		srcRow = sourcePixels + srcY * paddedRowBytes;
		for ( x = 0; x < CL_SP_CGAME_SCREENSHOT_SIZE; x++ ) {
			srcX = x * cl_sp_cgameCaptureWidth / CL_SP_CGAME_SCREENSHOT_SIZE;
			src = srcRow + srcX * 3;
			dst = dstRow + x * 4;

			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = 255;
		}
	}

	cl_sp_cgameScreenshotValid = qtrue;
	return qtrue;
}

static void CL_SP_SetFallbackLighting( vec_t *ambientLight, vec_t *directedLight, vec_t *lightDir ) {
	VectorSet( ambientLight, 255.0f, 255.0f, 255.0f );
	VectorSet( directedLight, 255.0f, 255.0f, 255.0f );
	VectorSet( lightDir, 0.0f, 0.0f, 1.0f );
}

static intptr_t CL_SP_GetSampleLengthMilliseconds( sfxHandle_t sfxHandle ) {
	sfx_t *sfx;

	if ( sfxHandle < 0 || sfxHandle >= s_numSfx || dma.speed <= 0 ) {
		return 0;
	}

	sfx = &s_knownSfx[sfxHandle];
	if ( sfx->inMemory == qfalse ) {
		S_memoryLoad( sfx );
	}

	if ( sfx->soundLength <= 0 ) {
		return 0;
	}

	return ( (intptr_t)sfx->soundLength * 1000 ) / dma.speed;
}

#define CL_SP_MAX_AMBIENT_SETS       512
#define CL_SP_MAX_AMBIENT_SUBWAVES   16
#define CL_SP_MAX_AMBIENT_PRECACHE   256
#define CL_SP_AMBIENT_LOOP_ENTITY    ENTITYNUM_WORLD

typedef enum {
	CL_SP_AMBIENT_GENERAL,
	CL_SP_AMBIENT_LOCAL,
	CL_SP_AMBIENT_BMODEL
} cl_sp_ambientSetType_t;

typedef struct {
	char					name[MAX_QPATH];
	cl_sp_ambientSetType_t	type;
	int						waveMinSeconds;
	int						waveMaxSeconds;
	float					radius;
	char					loopSoundPath[MAX_QPATH];
	sfxHandle_t				loopSound;
	qboolean				loopSoundRegistered;
	char					subWavePaths[CL_SP_MAX_AMBIENT_SUBWAVES][MAX_QPATH];
	sfxHandle_t				subWaves[CL_SP_MAX_AMBIENT_SUBWAVES];
	qboolean				subWavesRegistered[CL_SP_MAX_AMBIENT_SUBWAVES];
	int						numSubWaves;
	qboolean				preloaded;
} cl_sp_ambientSet_t;

static cl_sp_ambientSet_t	cl_sp_ambientSets[CL_SP_MAX_AMBIENT_SETS];
static int					cl_sp_numAmbientSets;
static char					cl_sp_ambientPrecache[CL_SP_MAX_AMBIENT_PRECACHE][MAX_QPATH];
static int					cl_sp_numAmbientPrecache;
static qboolean				cl_sp_ambientParsed;
static int					cl_sp_activeGeneralSet = -1;
static int					cl_sp_nextGeneralWaveTime;

static void CL_SP_AmbientEnsureSetRegistered( cl_sp_ambientSet_t *set );

static int CL_SP_AmbientTokenizeLine( char *line, char *tokens[], int maxTokens ) {
	int count;
	char *cursor;

	count = 0;
	cursor = line;
	while ( *cursor ) {
		while ( *cursor && *cursor <= ' ' ) {
			cursor++;
		}

		if ( !*cursor ) {
			break;
		}

		if ( count >= maxTokens ) {
			break;
		}

		tokens[count++] = cursor;
		while ( *cursor && *cursor > ' ' ) {
			cursor++;
		}

		if ( *cursor ) {
			*cursor++ = '\0';
		}
	}

	return count;
}

static qboolean CL_SP_AmbientNextLine( char **cursor, char *line, int lineSize ) {
	char *src;
	int len;

	if ( !cursor || !*cursor || !**cursor ) {
		return qfalse;
	}

	src = *cursor;
	while ( *src == '\r' || *src == '\n' ) {
		src++;
	}

	if ( !*src ) {
		*cursor = src;
		return qfalse;
	}

	len = 0;
	while ( *src && *src != '\r' && *src != '\n' && len < lineSize - 1 ) {
		line[len++] = *src++;
	}
	line[len] = '\0';

	while ( *src == '\r' || *src == '\n' ) {
		src++;
	}

	*cursor = src;
	return qtrue;
}

static void CL_SP_AmbientBuildSoundPath( const char *base, const char *suffix, char *out, int outSize ) {
	if ( !base || !base[0] ) {
		if ( outSize > 0 ) {
			out[0] = '\0';
		}
		return;
	}

	if ( suffix && suffix[0] ) {
		if ( !Q_strncmp( base, "sound/", 6 ) || !Q_strncmp( base, "music/", 6 ) ) {
			Com_sprintf( out, outSize, "%s/%s", base, suffix );
		} else {
			Com_sprintf( out, outSize, "sound/%s/%s", base, suffix );
		}
	} else if ( !Q_strncmp( base, "sound/", 6 ) || !Q_strncmp( base, "music/", 6 ) ) {
		Q_strncpyz( out, base, outSize );
	} else {
		Com_sprintf( out, outSize, "sound/%s", base );
	}
}

static void CL_SP_ClearAmbientSetData( void ) {
	Com_Memset( cl_sp_ambientSets, 0, sizeof( cl_sp_ambientSets ) );
	cl_sp_numAmbientSets = 0;
	cl_sp_ambientParsed = qfalse;
	cl_sp_activeGeneralSet = -1;
	cl_sp_nextGeneralWaveTime = 0;
	S_StopLoopingSound( CL_SP_AMBIENT_LOOP_ENTITY );
}

static void CL_SP_ClearAmbientPrecacheEntries( void ) {
	Com_Memset( cl_sp_ambientPrecache, 0, sizeof( cl_sp_ambientPrecache ) );
	cl_sp_numAmbientPrecache = 0;
}

static qboolean CL_SP_AmbientHasPrecacheEntry( const char *name ) {
	int i;

	if ( !name || !name[0] ) {
		return qfalse;
	}

	for ( i = 0; i < cl_sp_numAmbientPrecache; i++ ) {
		if ( !Q_stricmp( cl_sp_ambientPrecache[i], name ) ) {
			return qtrue;
		}
	}

	return qfalse;
}

static void CL_SP_AmbientAddPrecacheEntry( const char *name ) {
	cl_sp_ambientSet_t *set;

	if ( !name || !name[0] || CL_SP_AmbientHasPrecacheEntry( name ) ) {
		return;
	}

	if ( cl_sp_numAmbientPrecache < CL_SP_MAX_AMBIENT_PRECACHE ) {
		Q_strncpyz( cl_sp_ambientPrecache[cl_sp_numAmbientPrecache++], name, MAX_QPATH );
	}

	if ( cl_sp_ambientParsed ) {
		for ( set = cl_sp_ambientSets; set < cl_sp_ambientSets + cl_sp_numAmbientSets; set++ ) {
			if ( !Q_stricmp( set->name, name ) ) {
				set->preloaded = qtrue;
				CL_SP_AmbientEnsureSetRegistered( set );
				break;
			}
		}
	}
}

static cl_sp_ambientSet_t *CL_SP_FindAmbientSet( const char *name, int expectedType ) {
	int i;
	cl_sp_ambientSet_t *set;

	if ( !name || !name[0] ) {
		return NULL;
	}

	for ( i = 0; i < cl_sp_numAmbientSets; i++ ) {
		set = &cl_sp_ambientSets[i];
		if ( ( expectedType < 0 || set->type == expectedType ) && !Q_stricmp( set->name, name ) ) {
			return set;
		}
	}

	return NULL;
}

static void CL_SP_AmbientEnsureSetRegistered( cl_sp_ambientSet_t *set ) {
	int i;

	if ( !set ) {
		return;
	}

	if ( set->loopSoundPath[0] && !set->loopSoundRegistered ) {
		set->loopSound = S_RegisterSound( set->loopSoundPath, qfalse );
		set->loopSoundRegistered = qtrue;
	}

	for ( i = 0; i < set->numSubWaves; i++ ) {
		if ( set->subWavePaths[i][0] && !set->subWavesRegistered[i] ) {
			set->subWaves[i] = S_RegisterSound( set->subWavePaths[i], qfalse );
			set->subWavesRegistered[i] = qtrue;
		}
	}
}

static int CL_SP_AmbientRandomDelayMilliseconds( const cl_sp_ambientSet_t *set ) {
	int minSeconds;
	int maxSeconds;
	int swap;

	if ( !set ) {
		return 0;
	}

	minSeconds = set->waveMinSeconds;
	maxSeconds = set->waveMaxSeconds;
	if ( minSeconds > maxSeconds ) {
		swap = minSeconds;
		minSeconds = maxSeconds;
		maxSeconds = swap;
	}

	if ( maxSeconds <= 0 ) {
		return 0;
	}

	if ( minSeconds < 0 ) {
		minSeconds = 0;
	}

	return Q_irand( minSeconds, maxSeconds ) * 1000;
}

static sfxHandle_t CL_SP_AmbientPickSubWave( const cl_sp_ambientSet_t *set ) {
	int firstValid;
	int i;
	int index;

	if ( !set || set->numSubWaves <= 0 ) {
		return 0;
	}

	firstValid = -1;
	for ( i = 0; i < set->numSubWaves; i++ ) {
		if ( set->subWavesRegistered[i] && set->subWaves[i] ) {
			firstValid = i;
			break;
		}
	}

	if ( firstValid < 0 ) {
		return 0;
	}

	for ( i = 0; i < set->numSubWaves; i++ ) {
		index = Q_irand( 0, set->numSubWaves - 1 );
		if ( set->subWavesRegistered[index] && set->subWaves[index] ) {
			return set->subWaves[index];
		}
	}

	return set->subWaves[firstValid];
}

static void CL_SP_ParseAmbientSetFile( void ) {
	void *fileBuffer;
	int fileLen;
	char *cursor;
	char line[1024];
	cl_sp_ambientSet_t *currentSet;

	CL_SP_ClearAmbientSetData();
	cl_sp_ambientParsed = qtrue;

	fileBuffer = NULL;
	fileLen = FS_ReadFile( "sound/sound.txt", &fileBuffer );
	if ( fileLen <= 0 || !fileBuffer ) {
		return;
	}

	cursor = (char *)fileBuffer;
	currentSet = NULL;

	while ( CL_SP_AmbientNextLine( &cursor, line, sizeof( line ) ) ) {
		char *tokens[32];
		char *comment;
		int numTokens;

		comment = strchr( line, ';' );
		if ( comment ) {
			*comment = '\0';
		}

		numTokens = CL_SP_AmbientTokenizeLine( line, tokens, ARRAY_LEN( tokens ) );
		if ( numTokens <= 0 ) {
			continue;
		}

		if ( !Q_stricmp( tokens[0], "generalSet" ) ||
			!Q_stricmp( tokens[0], "localSet" ) ||
			!Q_stricmp( tokens[0], "bmodelSet" ) ) {
			if ( numTokens < 2 || cl_sp_numAmbientSets >= CL_SP_MAX_AMBIENT_SETS ) {
				currentSet = NULL;
				continue;
			}

			currentSet = &cl_sp_ambientSets[cl_sp_numAmbientSets++];
			Com_Memset( currentSet, 0, sizeof( *currentSet ) );
			Q_strncpyz( currentSet->name, tokens[1], sizeof( currentSet->name ) );
			currentSet->preloaded = CL_SP_AmbientHasPrecacheEntry( currentSet->name );

			if ( !Q_stricmp( tokens[0], "generalSet" ) ) {
				currentSet->type = CL_SP_AMBIENT_GENERAL;
			} else if ( !Q_stricmp( tokens[0], "localSet" ) ) {
				currentSet->type = CL_SP_AMBIENT_LOCAL;
			} else {
				currentSet->type = CL_SP_AMBIENT_BMODEL;
			}
			continue;
		}

		if ( !currentSet ) {
			continue;
		}

		if ( !Q_stricmp( tokens[0], "timeBetweenWaves" ) && numTokens >= 3 ) {
			currentSet->waveMinSeconds = atoi( tokens[1] );
			currentSet->waveMaxSeconds = atoi( tokens[2] );
		} else if ( !Q_stricmp( tokens[0], "loopedWave" ) && numTokens >= 2 ) {
			CL_SP_AmbientBuildSoundPath( tokens[1], NULL, currentSet->loopSoundPath, sizeof( currentSet->loopSoundPath ) );
		} else if ( !Q_stricmp( tokens[0], "subWaves" ) && numTokens >= 3 ) {
			int i;

			currentSet->numSubWaves = 0;
			for ( i = 2; i < numTokens && currentSet->numSubWaves < CL_SP_MAX_AMBIENT_SUBWAVES; i++ ) {
				CL_SP_AmbientBuildSoundPath(
					tokens[1],
					tokens[i],
					currentSet->subWavePaths[currentSet->numSubWaves],
					sizeof( currentSet->subWavePaths[currentSet->numSubWaves] ) );
				currentSet->numSubWaves++;
			}
		} else if ( !Q_stricmp( tokens[0], "radius" ) && numTokens >= 2 ) {
			currentSet->radius = (float)atof( tokens[1] );
		}
	}

	FS_FreeFile( fileBuffer );

	{
		int i;
		for ( i = 0; i < cl_sp_numAmbientSets; i++ ) {
			if ( cl_sp_ambientSets[i].preloaded ) {
				CL_SP_AmbientEnsureSetRegistered( &cl_sp_ambientSets[i] );
			}
		}
	}

	CL_SP_ClearAmbientPrecacheEntries();
}

static void CL_SP_AmbientEnsureParsed( void ) {
	if ( !cl_sp_ambientParsed ) {
		CL_SP_ParseAmbientSetFile();
	}
}

/*
 * EF1 SP cgame syscall numbers.
 *
 * These are the integer IDs the precompiled SP cgame DLL passes as args[0]
 * when it calls back into the engine.  They are NOT the same as ioEF MP's
 * CG_* enum values -- the two numbering schemes diverge starting at 34
 * (where SP inserts the force-feedback block) and never converge again.
 *
 * The groupings below mirror the original Ritual SDK's ordering:
 *   0-16   : core engine services (print, cvars, commands, filesystem)
 *   17-25  : collision model (CM) queries
 *   26-33  : sound system
 *   34-37  : force feedback (SP-only, shifts all subsequent numbers)
 *   38-55  : renderer (includes SP-only calls at 45, 51, 54, 55)
 *   56-64  : client state queries (glconfig, gamestate, snapshots, usercmd)
 *   65-70  : ambient sound system (SP-only, appended at end)
 */

/* Core engine services -- identical numbering to ioEF MP for 0-16 */
#define SPCG_PRINT                        0
#define SPCG_ERROR                        1
#define SPCG_MILLISECONDS                 2
#define SPCG_CVAR_REGISTER                3
#define SPCG_CVAR_UPDATE                  4
#define SPCG_CVAR_SET                     5
#define SPCG_ARGC                         6
#define SPCG_ARGV                         7
#define SPCG_ARGS                         8
#define SPCG_FS_FOPENFILE                 9
#define SPCG_FS_READ                      10
#define SPCG_FS_WRITE                     11
#define SPCG_FS_FCLOSEFILE                12
#define SPCG_SENDCONSOLECOMMAND           13
#define SPCG_ADDCOMMAND                   14
#define SPCG_SENDCLIENTCOMMAND            15
#define SPCG_UPDATESCREEN                 16

/* Collision model -- still matching ioEF MP numbering at this point */
#define SPCG_CM_LOADMAP                   17
#define SPCG_CM_NUMINLINEMODELS           18
#define SPCG_CM_INLINEMODEL               19
#define SPCG_CM_TEMPBOXMODEL              20
#define SPCG_CM_POINTCONTENTS             21
#define SPCG_CM_TRANSFORMEDPOINTCONTENTS  22
#define SPCG_CM_BOXTRACE                  23
#define SPCG_CM_TRANSFORMEDBOXTRACE       24
#define SPCG_CM_MARKFRAGMENTS             25

/* Sound -- still matching ioEF MP numbering through 33 */
#define SPCG_S_STARTSOUND                 26
#define SPCG_S_STARTLOCALSOUND            27
#define SPCG_S_CLEARLOOPINGSOUNDS         28
#define SPCG_S_ADDLOOPINGSOUND            29
#define SPCG_S_UPDATEENTITYPOSITION       30
#define SPCG_S_RESPATIALIZE               31
#define SPCG_S_REGISTERSOUND              32
#define SPCG_S_STARTBACKGROUNDTRACK       33

/* Force Feedback -- SP-ONLY.  These four calls are where SP and MP syscall
   numbering permanently diverge.  The original Ritual engine supported
   DirectInput Force Feedback devices (rumble joysticks, force-feedback
   steering wheels).  The cgame would trigger haptic effects for weapon
   fire, explosions, and environmental hazards.  ioEF has no FF support,
   so these are stubbed out, but their presence shifts every subsequent
   syscall number up by 4 relative to ioEF MP. */
#define SPCG_FF_STARTFX                   34
#define SPCG_FF_ENSUREFX                  35
#define SPCG_FF_STOPFX                    36
#define SPCG_FF_STOPALLFX                 37

/* Renderer -- numbers 38+ are offset from ioEF MP due to the FF block above.
   Additionally, SP inserts its own renderer calls (GetLighting, DrawScreenShot,
   DrawRotatePic, Scissor) that push numbers further apart. */
#define SPCG_R_LOADWORLDMAP               38
#define SPCG_R_REGISTERMODEL              39
#define SPCG_R_REGISTERSKIN               40
#define SPCG_R_REGISTERSHADER             41
#define SPCG_R_REGISTERSHADERNOMIP        42
#define SPCG_R_CLEARSCENE                 43
#define SPCG_R_ADDREFENTITYTOSCENE        44
#define SPCG_R_GETLIGHTING                45  /* SP-only: query lighting at a world point */
#define SPCG_R_ADDPOLYTOSCENE             46
#define SPCG_R_ADDLIGHTTOSCENE            47
#define SPCG_R_RENDERSCENE                48
#define SPCG_R_SETCOLOR                   49
#define SPCG_R_DRAWSTRETCHPIC             50
#define SPCG_R_DRAWSCREENSHOT             51  /* SP-only: draw a captured screenshot as texture */
#define SPCG_R_MODELBOUNDS                52
#define SPCG_R_LERPTAG                    53
#define SPCG_R_DRAWROTATEPIC              54  /* SP-only: draw a 2D pic with rotation */
#define SPCG_R_SCISSOR                    55  /* SP-only: set a scissor rectangle for 2D rendering */

/* Client state queries */
#define SPCG_GETGLCONFIG                  56
#define SPCG_GETGAMESTATE                 57
#define SPCG_GETCURRENTSNAPSHOTNUMBER     58
#define SPCG_GETSNAPSHOT                  59
#define SPCG_GETSERVERCOMMAND             60
#define SPCG_GETCURRENTCMDNUMBER          61
#define SPCG_GETUSERCMD                   62
#define SPCG_SETUSERCMDVALUE              63
#define SPCG_MEMORY_REMAINING             64

/* Ambient Sound System -- SP-ONLY.  The original Ritual engine had a
   sophisticated positional ambient sound system that could attach sound
   environments to BSP brush models and world regions.  ioEF now bridges
   the basic data flow by parsing `sound/sound.txt`, precaching set names,
   driving looping sounds, and selecting one-shot subwaves, but parity with
   the original blending/mixing behavior is still incomplete. */
#define SPCG_S_UPDATEAMBIENTSET           65  /* update the active ambient sound set */
#define SPCG_S_ADDLOCALSET                66  /* add a local ambient sound set at a position */
#define SPCG_AS_PARSESETS                 67  /* parse ambient set definitions from a file */
#define SPCG_AS_ADDENTRY                  68  /* add an entry to an ambient set */
#define SPCG_AS_GETBMODELSOUND            69  /* get the ambient sound for a brush model */
#define SPCG_S_GETSAMPLELENGTH            70  /* query the duration of a sound sample */

/*
 * VM argument access macros.  We #undef first because these are also defined
 * in the MP cgame dispatcher (cl_cgame.c), and this file may be compiled in
 * the same translation unit or share a common header.
 *
 * VMA(x) -- converts the integer argument at position x to a pointer.
 *           The SP DLL passes pointers as integer offsets; VM_ArgPtr
 *           resolves them to real pointers in the engine's address space.
 * VMF(x) -- reinterprets the integer argument at position x as a float.
 *           Q3 engine syscalls pass floats as bit-identical integers.
 */
#undef VMA
#undef VMF
#define	VMA(x) VM_ArgPtr(args[x])
#define	VMF(x) ((float *)args)[x]

/*
 * FloatAsInt: type-pun a float to an int for returning float values through
 * the intptr_t syscall return channel.  Uses a union to avoid strict-aliasing
 * violations (the floatint_t union is defined in q_shared.h).
 */
static int FloatAsInt( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

/*
 * The EF1 SP cgame was compiled against a usercmd layout with a 4-byte
 * buttons field. ioEF's internal ELITEFORCE usercmd_t keeps buttons as a
 * byte to preserve the engine's message packing. Translate here so the SP
 * cgame reads weapon, angles, and movement fields from the offsets it expects.
 */
static qboolean CL_SP_GetUserCmd( int cmdNumber, sp_usercmd_t *spCmd ) {
	usercmd_t cmd;

	if ( !CL_GetUserCmd( cmdNumber, &cmd ) ) {
		return qfalse;
	}

	spCmd->serverTime = cmd.serverTime;
	spCmd->buttons = cmd.buttons;
	spCmd->weapon = cmd.weapon;
	spCmd->angles[0] = cmd.angles[0];
	spCmd->angles[1] = cmd.angles[1];
	spCmd->angles[2] = cmd.angles[2];
	spCmd->forwardmove = cmd.forwardmove;
	spCmd->rightmove = cmd.rightmove;
	spCmd->upmove = cmd.upmove;

	return qtrue;
}

/*
====================
CL_CM_LoadMap (local forward declaration)
====================
*/
extern void CL_CM_LoadMap( const char *mapname );

/*
====================
CL_SPCgameSystemCalls

Syscall dispatcher for the EF1 singleplayer cgame module.
The SP cgame uses a different syscall numbering from ioEF's
multiplayer cgame, so we translate here.
====================
*/
/*
 * SP syscall trace helper.  Calls into sv_debug_server.c's ring buffer.
 * Only the SP-specific and gameplay-relevant syscalls are traced to avoid
 * flooding; high-frequency boilerplate (print, argc, argv, etc.) is skipped.
 */
#define SPTRACE_STR_MAX 128

extern void DebugServer_TraceSPCall(int id, const char *name, qboolean isImport,
	intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5,
	const char *strArg, intptr_t retVal);

#define SP_TRACE(name, str, ret) \
	DebugServer_TraceSPCall(args[0], name, qfalse, \
		args[1], args[2], args[3], args[4], args[5], \
		(intptr_t)(args[0] < 7 ? 0 : args[6]), str, (intptr_t)(ret))

#define SP_TRACE_NORET(name, str) SP_TRACE(name, str, 0)

intptr_t CL_SPCgameSystemCalls( intptr_t *args ) {
	switch( args[0] ) {

	// --- core ---
	case SPCG_PRINT:
		Com_Printf( "%s", (const char*)VMA(1) );
		return 0;
	case SPCG_ERROR:
		Com_Error( ERR_DROP, "%s", (const char*)VMA(1) );
		return 0;
	case SPCG_MILLISECONDS:
		return Sys_Milliseconds();

	// --- cvars ---
	case SPCG_CVAR_REGISTER:
		Cvar_Register( VMA(1), VMA(2), VMA(3), args[4] );
		return 0;
	case SPCG_CVAR_UPDATE:
		Cvar_Update( VMA(1) );
		return 0;
	case SPCG_CVAR_SET: {
		char cvarBuf[SPTRACE_STR_MAX];
		Com_sprintf(cvarBuf, sizeof(cvarBuf), "%s=%s",
			(const char *)VMA(1), (const char *)VMA(2));
		SP_TRACE("Cvar_Set", cvarBuf, 0);
		Cvar_SetSafe( VMA(1), VMA(2) );
		return 0;
	}

	// --- command args ---
	case SPCG_ARGC:
		return Cmd_Argc();
	case SPCG_ARGV:
		Cmd_ArgvBuffer( args[1], VMA(2), args[3] );
		return 0;
	case SPCG_ARGS:
		Cmd_ArgsBuffer( VMA(1), args[2] );
		return 0;

	// --- filesystem ---
	case SPCG_FS_FOPENFILE:
		return FS_FOpenFileByMode( VMA(1), VMA(2), args[3] );
	case SPCG_FS_READ:
		FS_Read2( VMA(1), args[2], args[3] );
		return 0;
	case SPCG_FS_WRITE:
		FS_Write( VMA(1), args[2], args[3] );
		return 0;
	case SPCG_FS_FCLOSEFILE:
		FS_FCloseFile( args[1] );
		return 0;

	// --- commands ---
	case SPCG_SENDCONSOLECOMMAND:
		SP_TRACE("SendConsoleCommand", VMA(1), 0);
		Cbuf_AddText( VMA(1) );
		return 0;
	case SPCG_ADDCOMMAND:
		CL_AddCgameCommand( VMA(1) );
		return 0;
	case SPCG_SENDCLIENTCOMMAND:
		CL_AddReliableCommand( VMA(1), qfalse );
		return 0;

	// --- screen ---
	case SPCG_UPDATESCREEN:
		// During SP loading, the cgame calls UpdateScreen to show loading
		// progress.  SCR_UpdateScreen triggers a full render + buffer swap,
		// which pollutes one of the OpenGL double-buffers with a stale frame
		// (often the cutscene camera view).  After loading completes and
		// CA_ACTIVE is reached, the normal render loop only draws to the
		// OTHER buffer, causing visible flickering as the display alternates
		// between the stale init frame and the live game frame.
		// Fix: no-op.  Loading progress won't be visible, but the game
		// will load correctly and both double-buffers will be clean.
		return 0;

	// --- collision ---
	case SPCG_CM_LOADMAP:
		CL_CM_LoadMap( VMA(1) );
		return 0;
	case SPCG_CM_NUMINLINEMODELS:
		return CM_NumInlineModels();
	case SPCG_CM_INLINEMODEL:
		return CM_InlineModel( args[1] );
	case SPCG_CM_TEMPBOXMODEL:
		return CM_TempBoxModel( VMA(1), VMA(2), /*capsule*/ qfalse );
	case SPCG_CM_POINTCONTENTS:
		return CM_PointContents( VMA(1), args[2] );
	case SPCG_CM_TRANSFORMEDPOINTCONTENTS:
		return CM_TransformedPointContents( VMA(1), args[2], VMA(3), VMA(4) );
	case SPCG_CM_BOXTRACE:
		CM_BoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], /*capsule*/ qfalse );
		return 0;
	case SPCG_CM_TRANSFORMEDBOXTRACE:
		CM_TransformedBoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], VMA(8), VMA(9), /*capsule*/ qfalse );
		return 0;
	case SPCG_CM_MARKFRAGMENTS:
		return re.MarkFragments( args[1], VMA(2), VMA(3), args[4], VMA(5), args[6], VMA(7) );

	// --- sound ---
	case SPCG_S_STARTSOUND:
		SP_TRACE("S_StartSound", NULL, 0);
		S_StartSound( VMA(1), args[2], args[3], args[4] );
		return 0;
	case SPCG_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;
	case SPCG_S_CLEARLOOPINGSOUNDS:
		S_ClearLoopingSounds( args[1] );
		return 0;
	case SPCG_S_ADDLOOPINGSOUND:
		SP_TRACE("S_AddLoopingSound", NULL, 0);
		S_AddLoopingSound( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case SPCG_S_UPDATEENTITYPOSITION:
		S_UpdateEntityPosition( args[1], VMA(2) );
		return 0;
	case SPCG_S_RESPATIALIZE:
		S_Respatialize( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case SPCG_S_REGISTERSOUND: {
		sfxHandle_t h = S_RegisterSound( VMA(1), qfalse );
		SP_TRACE("S_RegisterSound", VMA(1), h);
		return h;
	}
	case SPCG_S_STARTBACKGROUNDTRACK:
		/* The SP cgame passes a NULL or empty string to stop the music track
		   (e.g., during transitions between cinematic and gameplay).
		   A non-empty string starts a new background music track; arg 2 is
		   the loop portion filename (or NULL to loop the whole track). */
		if ( !VMA(1) || !*((char *) VMA(1)) )
			S_StopBackgroundTrack();
		else
			S_StartBackgroundTrack( VMA(1), VMA(2) );
		return 0;

	// --- force feedback (stubs) ---
	// The original Ritual engine exposed DirectInput Force Feedback to the
	// cgame for haptic effects: weapon recoil, explosion shockwaves, and
	// environmental rumble (e.g., the ship shaking during combat sequences).
	// ioEF has no force-feedback hardware support, so these are safe no-ops.
	// The SP cgame handles the absence gracefully -- it simply gets no haptic
	// response, which doesn't affect gameplay or visuals.
	case SPCG_FF_STARTFX:
		SP_TRACE("FF_StartFX", NULL, 0);
		return 0;
	case SPCG_FF_ENSUREFX:
		SP_TRACE("FF_EnsureFX", NULL, 0);
		return 0;
	case SPCG_FF_STOPFX:
		SP_TRACE("FF_StopFX", NULL, 0);
		return 0;
	case SPCG_FF_STOPALLFX:
		SP_TRACE("FF_StopAllFX", NULL, 0);
		return 0;

	// --- renderer ---
	case SPCG_R_LOADWORLDMAP:
		re.LoadWorld( VMA(1) );
		return 0;
	case SPCG_R_REGISTERMODEL:
		return re.RegisterModel( VMA(1) );
	case SPCG_R_REGISTERSKIN:
		return re.RegisterSkin( VMA(1) );
	case SPCG_R_REGISTERSHADER:
		return re.RegisterShader( VMA(1) );
	case SPCG_R_REGISTERSHADERNOMIP:
		return re.RegisterShaderNoMip( VMA(1) );
	case SPCG_R_CLEARSCENE:
		re.ClearScene();
		return 0;
	case SPCG_R_ADDREFENTITYTOSCENE:
		re.AddRefEntityToScene( VMA(1) );
		return 0;
	case SPCG_R_GETLIGHTING: {
		/*
		 * The SP cgame reads the output vectors unconditionally and uses the
		 * directed light intensity for gameplay (stealth visibility). Provide
		 * deterministic output even when the renderer has no light-grid data.
		 */
		int litOk = re.LightForPoint( VMA(1), VMA(2), VMA(3), VMA(4) );
		if ( !litOk ) {
			CL_SP_SetFallbackLighting( VMA(2), VMA(3), VMA(4) );
		}
		SP_TRACE("R_GetLighting", NULL, litOk);
		return 0;
	}
	case SPCG_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), 1 );
		return 0;
	case SPCG_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case SPCG_R_RENDERSCENE: {
		cl_sp_cgameScreenshotValid = qfalse;
		// When sp_forcecamera is set, override the cutscene camera with
		// the player's actual position and view angles so we can see
		// the 3D world instead of the cutscene's dark space view.
		if ( Cvar_VariableIntegerValue("sp_forcecamera") ) {
			refdef_t *fd = (refdef_t *)VMA(1);
			void *rawPS = SV_SP_GetRawPlayerState();
			static int fcDbg = 0;
			fcDbg++;
			if ( rawPS ) {
				sp_playerState_t *ps = (sp_playerState_t *)rawPS;
				if ( fcDbg <= 3 ) {
					Com_Printf("FORCECAM: org=(%.0f,%.0f,%.0f) ang=(%.0f,%.0f,%.0f) rect=%dx%d+%d+%d fov=%.0fx%.0f rdflags=%d\n",
						ps->origin[0], ps->origin[1], ps->origin[2],
						ps->viewangles[0], ps->viewangles[1], ps->viewangles[2],
						fd->width, fd->height, fd->x, fd->y,
						fd->fov_x, fd->fov_y, fd->rdflags );
				}
				VectorCopy( ps->origin, fd->vieworg );
				fd->vieworg[2] += ps->viewheight;
				AnglesToAxis( ps->viewangles, fd->viewaxis );
				Com_Memset( fd->areamask, 0x00, sizeof( fd->areamask ) );
				// Fix zero viewport from cutscene camera init
				if ( fd->width == 0 || fd->height == 0 ) {
					fd->x = 0;
					fd->y = 0;
					fd->width = cls.glconfig.vidWidth;
					fd->height = cls.glconfig.vidHeight;
					fd->fov_x = 90;
					fd->fov_y = 73.74f; // atan(tan(90/2) * 600/800) * 2
				}
			} else if ( fcDbg <= 3 ) {
				Com_Printf("FORCECAM: rawPS is NULL!\n");
			}
		}
		re.RenderScene( VMA(1) );
		return 0;
	}
	case SPCG_R_SETCOLOR:
		re.SetColor( VMA(1) );
		return 0;
	case SPCG_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;
	case SPCG_R_DRAWSCREENSHOT: {
		// SP-only renderer call: R_DrawScreenShot.
		int captured = cl_sp_cgameScreenshotValid;
		char shotBuf[SPTRACE_STR_MAX];
		if ( !cl_sp_cgameScreenshotValid && !CL_SP_CaptureCgameScreenshot() ) {
			Com_sprintf( shotBuf, sizeof( shotBuf ), "FAILED_CAPTURE x=%d y=%d w=%d h=%d",
				(int)VMF(1), (int)VMF(2), (int)VMF(3), (int)VMF(4) );
			SP_TRACE("R_DrawScreenShot", shotBuf, 0);
			return 0;
		}
		re.DrawStretchRaw( (int)VMF(1), (int)VMF(2), (int)VMF(3), (int)VMF(4),
			CL_SP_CGAME_SCREENSHOT_SIZE, CL_SP_CGAME_SCREENSHOT_SIZE,
			cl_sp_cgameScreenshot, 0, qtrue );
		Com_sprintf( shotBuf, sizeof( shotBuf ), "%s x=%d y=%d w=%d h=%d",
			captured ? "REUSED" : "FRESH_CAPTURE",
			(int)VMF(1), (int)VMF(2), (int)VMF(3), (int)VMF(4) );
		SP_TRACE("R_DrawScreenShot", shotBuf, 1);
		return 0;
	}
	case SPCG_R_MODELBOUNDS:
		re.ModelBounds( args[1], VMA(2), VMA(3) );
		return 0;
	case SPCG_R_LERPTAG:
		return re.LerpTag( VMA(1), args[2], args[3], args[4], VMF(5), VMA(6) );
	case SPCG_R_DRAWROTATEPIC:
		SP_TRACE("R_DrawRotatePic", NULL, 0);
		if ( re.DrawRotatePic ) {
			re.DrawRotatePic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), VMF(9), args[10] );
		} else {
			re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[10] );
		}
		return 0;
	case SPCG_R_SCISSOR: {
		char rectBuf[64];
		Com_sprintf(rectBuf, sizeof(rectBuf), "x=%.0f y=%.0f w=%.0f h=%.0f",
			VMF(1), VMF(2), VMF(3), VMF(4));
		SP_TRACE("R_Scissor", rectBuf, 0);
		if ( re.SetScissor ) {
			re.SetScissor( VMF(1), VMF(2), VMF(3), VMF(4) );
		}
		return 0;
	}

	// --- client state ---
	case SPCG_GETGLCONFIG:
		CL_GetGlconfig( VMA(1) );
		return 0;
	case SPCG_GETGAMESTATE:
		CL_GetGameState( VMA(1) );
		return 0;
	case SPCG_GETCURRENTSNAPSHOTNUMBER:
		CL_GetCurrentSnapshotNumber( VMA(1), VMA(2) );
		return 0;
	case SPCG_GETSNAPSHOT: {
		/*
		 * =====================================================================
		 * SP SNAPSHOT BYPASS ARCHITECTURE
		 * =====================================================================
		 *
		 * This is the most critical syscall in the SP bridge.  It supplies
		 * the SP cgame with a complete world snapshot each frame.
		 *
		 * WHY WE BYPASS DELTA COMPRESSION:
		 *
		 * In normal ioEF multiplayer, the snapshot pipeline works like this:
		 *   1. Server builds a snapshot with ioEF entityState_t / playerState_t
		 *   2. Server delta-compresses it against a previous snapshot
		 *   3. Compressed data is sent over the network
		 *   4. Client decompresses into ioEF snapshot_t
		 *   5. Cgame reads the ioEF snapshot_t
		 *
		 * This pipeline assumes both sides agree on the struct layouts.
		 * For SP, the cgame DLL expects sp_entityState_t and sp_playerState_t,
		 * which have different fields and different sizes.  If we let the
		 * normal pipeline run, the server would compress ioEF-layout data,
		 * the client would decompress it into ioEF-layout structs, and then
		 * the SP cgame would interpret those bytes as SP-layout structs --
		 * reading the wrong fields at the wrong offsets.
		 *
		 * We COULD try to translate ioEF structs to SP structs after
		 * decompression, but that would lose the SP-specific fields
		 * (modelindex3, legsAnimTimer, torsoAnimTimer, scale, pushVec,
		 * leanofs, borgAdaptHits, etc.) because they were never encoded
		 * into the ioEF snapshot in the first place.
		 *
		 * HOW THE BYPASS WORKS:
		 *
		 * Instead, we take a hybrid approach:
		 *
		 *   Step 1: Call CL_GetSnapshot() to get an ioEF-format snapshot.
		 *           We use this ONLY for metadata (snapFlags, ping,
		 *           serverTime, areamask, serverCommandSequence) and the
		 *           ENTITY LIST (which entities are visible this frame).
		 *           The ioEF snapshot system's PVS culling and rate
		 *           management correctly determine which entities should
		 *           be in the snapshot -- we don't want to reimplement that.
		 *
		 *   Step 2: Build an sp_snapshot_t in the buffer the SP cgame
		 *           provided.  Copy metadata from the ioEF snapshot.
		 *
		 *   Step 3: For the playerState, reach directly into the SP game
		 *           module's memory via SV_SP_GetRawPlayerState() and
		 *           memcpy the raw sp_playerState_t.  This preserves all
		 *           SP-specific fields exactly as the game module set them.
		 *
		 *   Step 4: For each entity in the ioEF snapshot's entity list,
		 *           use the entity NUMBER to look up the raw SP entity
		 *           data via SV_SP_GetRawEntityState(), and memcpy the
		 *           raw sp_entityState_t.  Again, this preserves all
		 *           SP-specific fields.
		 *
		 * This works because SP is always a local server -- the game module
		 * lives in the same process, so we can read its memory directly.
		 * There is no network serialization or delta compression involved.
		 *
		 * IMPORTANT: The entity list (which entities appear) comes from the
		 * ioEF snapshot, but the actual entity DATA comes from the SP game
		 * module.  The ioEF snapshot's entity data (in ioEF layout) is
		 * discarded -- we only use the .number field to know which entities
		 * to fetch from the SP side.
		 * =====================================================================
		 */

		/* Use the ioEF snapshot system to get metadata and the visible entity list.
		   tempSnap is static to avoid putting ~300KB on the stack each frame. */
		static snapshot_t tempSnap;
		clSnapshot_t *clSnap;
		qboolean result = CL_GetSnapshot( args[1], &tempSnap );
		if ( !result ) return qfalse;
		clSnap = &cl.snapshots[args[1] & PACKET_MASK];

		/* Build the SP-format snapshot in the buffer the cgame provided (arg 2) */
		sp_snapshot_t *spSnap = (sp_snapshot_t *)VMA(2);
		int i;

		/* Copy metadata from the ioEF snapshot -- these fields have the same
		   meaning and format in both layouts.
		   Strip SNAPFLAG_NOT_ACTIVE for SP: the server-side client state
		   transitions slightly after the first snapshot is generated, so
		   early snapshots carry this flag even though the game is fully
		   initialized.  If the cgame sees NOT_ACTIVE it will refuse to
		   set cg.snap, keeping the loading screen permanently visible. */
		spSnap->snapFlags = tempSnap.snapFlags & ~SNAPFLAG_NOT_ACTIVE;
		spSnap->ping = tempSnap.ping;
		spSnap->serverTime = tempSnap.serverTime;
		Com_Memcpy( spSnap->areamask, tempSnap.areamask, sizeof( spSnap->areamask ) );

		/* SP-specific fields: cmdNum and configstring tracking.
		   ioEF tracks cmdNum in the internal clSnapshot_t, but CL_GetSnapshot()
		   drops it when copying into the MP-facing snapshot_t. Preserve the
		   original value here so the SP cgame can run prediction against the
		   same command stream it was built for. */
		spSnap->cmdNum = clSnap->cmdNum;
		spSnap->serverCommandSequence = tempSnap.serverCommandSequence;
		spSnap->numServerCommands = 0;
		spSnap->numConfigstringChanges = 0;
		spSnap->configstringNum = 0;

		/* PlayerState: copy raw SP data directly from the game module's memory.
		   SV_SP_GetRawPlayerState() returns a pointer to the beginning of the
		   SP gclient_t, which starts with sp_playerState_t.  This gives us
		   all SP-specific fields (leanofs, borgAdaptHits, pushVec, etc.)
		   that would be lost if we tried to translate from ioEF playerState_t. */
		{
			void *rawPS = SV_SP_GetRawPlayerState();
			if ( rawPS ) {
				Com_Memcpy( &spSnap->ps, rawPS, sizeof( sp_playerState_t ) );
			} else {
				Com_Memset( &spSnap->ps, 0, sizeof( sp_playerState_t ) );
			}
		}

		/* Entities: for each entity the ioEF snapshot says is visible,
		   fetch the raw SP entity data from the game module.
		   We use tempSnap.entities[i].number (the entity index) to look up
		   the SP game module's entity array.  The actual ioEF entityState_t
		   field values in tempSnap.entities[] are ignored -- only .number
		   matters.  The real data comes from SV_SP_GetRawEntityState(). */
		spSnap->numEntities = tempSnap.numEntities;
		for ( i = 0; i < tempSnap.numEntities && i < SP_MAX_ENTITIES_IN_SNAPSHOT; i++ ) {
			int entNum = tempSnap.entities[i].number;
			sp_entityState_t *rawEnt = SV_SP_GetRawEntityState( entNum );
			if ( rawEnt ) {
				Com_Memcpy( &spSnap->entities[i], rawEnt, sizeof( sp_entityState_t ) );
			} else {
				/* Entity not found in the SP game module -- this shouldn't
				   normally happen, but can occur if the entity was removed
				   between the server frame and this snapshot fetch.  Zero
				   the entry and preserve the entity number so the cgame
				   can handle it gracefully. */
				Com_Memset( &spSnap->entities[i], 0, sizeof( sp_entityState_t ) );
				spSnap->entities[i].number = entNum;
			}
		}

		return qtrue;
	}
	case SPCG_GETSERVERCOMMAND:
		return CL_GetServerCommand( args[1] );
	case SPCG_GETCURRENTCMDNUMBER:
		return CL_GetCurrentCmdNumber();
	case SPCG_GETUSERCMD:
		return CL_SP_GetUserCmd( args[1], VMA(2) );
	case SPCG_SETUSERCMDVALUE:
		CL_SetUserCmdValue( args[1], VMF(2) );
		return 0;
	case SPCG_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();

	// --- EF1 SP ambient sound bridge ---
	case SPCG_S_UPDATEAMBIENTSET: {	/* refresh which ambient set is active for the player's position */
		cl_sp_ambientSet_t *set;
		const char *ambientName;
		int setIndex;
		int nextDelay;
		sfxHandle_t subWave;
		char infoBuf[SPTRACE_STR_MAX];

		CL_SP_AmbientEnsureParsed();
		ambientName = VMA(1) ? (const char *)VMA(1) : "(null)";
		set = CL_SP_FindAmbientSet( VMA(1), CL_SP_AMBIENT_GENERAL );
		setIndex = set ? (int)( set - cl_sp_ambientSets ) : -1;

		if ( set && !set->preloaded ) {
			set->preloaded = qtrue;
		}
		CL_SP_AmbientEnsureSetRegistered( set );

		if ( setIndex != cl_sp_activeGeneralSet ) {
			cl_sp_activeGeneralSet = setIndex;
			nextDelay = CL_SP_AmbientRandomDelayMilliseconds( set );
			cl_sp_nextGeneralWaveTime = cls.realtime + nextDelay;
		}

		if ( set && set->loopSoundRegistered && set->loopSound ) {
			S_AddRealLoopingSound( CL_SP_AMBIENT_LOOP_ENTITY, VMA(2), vec3_origin, set->loopSound );
		}

		if ( set && set->numSubWaves > 0 && cls.realtime >= cl_sp_nextGeneralWaveTime ) {
			subWave = CL_SP_AmbientPickSubWave( set );
			nextDelay = CL_SP_AmbientRandomDelayMilliseconds( set );
			if ( subWave ) {
				S_StartSound( VMA(2), CL_SP_AMBIENT_LOOP_ENTITY, CHAN_AUTO, subWave );
				cl_sp_nextGeneralWaveTime = cls.realtime + nextDelay +
					(int)CL_SP_GetSampleLengthMilliseconds( subWave );
			} else {
				if ( nextDelay <= 0 ) {
					nextDelay = 1000;
				}
				cl_sp_nextGeneralWaveTime = cls.realtime + nextDelay;
			}
		}

		Com_sprintf( infoBuf, sizeof( infoBuf ), "name=%s set=%d next=%d",
			set ? set->name : ambientName, setIndex, cl_sp_nextGeneralWaveTime );
		SP_TRACE("S_UpdateAmbientSet", infoBuf, setIndex);
		return 0;
	}
	case SPCG_S_ADDLOCALSET: {		/* register a local ambient set at a specific world position */
		cl_sp_ambientSet_t *set;
		const char *ambientName;
		float radiusSquared;
		float distanceSquared;
		int incomingWaveTime;
		int nextWaveTime;
		int nextDelay;
		sfxHandle_t subWave;
		char infoBuf[SPTRACE_STR_MAX];

		ambientName = VMA(1) ? (const char *)VMA(1) : "(null)";
		incomingWaveTime = (int)args[5];
		nextWaveTime = incomingWaveTime;
		CL_SP_AmbientEnsureParsed();
		set = CL_SP_FindAmbientSet( VMA(1), CL_SP_AMBIENT_LOCAL );
		if ( !set ) {
			if ( (int)args[4] >= 0 && (int)args[4] < MAX_GENTITIES ) {
				S_StopLoopingSound( args[4] );
			}
			Com_sprintf( infoBuf, sizeof( infoBuf ), "name=%s ent=%d in=%d missing",
				ambientName, (int)args[4], incomingWaveTime );
			SP_TRACE("S_AddLocalSet", infoBuf, 0);
			return 0;
		}

		if ( !set->preloaded ) {
			set->preloaded = qtrue;
		}
		CL_SP_AmbientEnsureSetRegistered( set );

		if ( set->radius > 0.0f ) {
			radiusSquared = set->radius * set->radius;
			distanceSquared = DistanceSquared( (const vec_t *)VMA(2), (const vec_t *)VMA(3) );
			if ( distanceSquared > radiusSquared ) {
				if ( (int)args[4] >= 0 && (int)args[4] < MAX_GENTITIES ) {
					S_StopLoopingSound( args[4] );
				}
				Com_sprintf( infoBuf, sizeof( infoBuf ), "name=%s ent=%d in=%d out_of_range",
					set->name, (int)args[4], incomingWaveTime );
				SP_TRACE("S_AddLocalSet", infoBuf, 0);
				return 0;
			}
		}

		if ( set->loopSoundRegistered && set->loopSound && (int)args[4] >= 0 && (int)args[4] < MAX_GENTITIES ) {
			S_AddRealLoopingSound( args[4], VMA(3), vec3_origin, set->loopSound );
		}

		if ( set->numSubWaves > 0 && cls.realtime >= nextWaveTime ) {
			subWave = CL_SP_AmbientPickSubWave( set );
			nextDelay = CL_SP_AmbientRandomDelayMilliseconds( set );
			if ( subWave ) {
				S_StartSound( VMA(3), args[4], CHAN_AUTO, subWave );
				nextWaveTime = cls.realtime + nextDelay +
					(int)CL_SP_GetSampleLengthMilliseconds( subWave );
			} else {
				if ( nextDelay <= 0 ) {
					nextDelay = 1000;
				}
				nextWaveTime = cls.realtime + nextDelay;
			}
		}

		Com_sprintf( infoBuf, sizeof( infoBuf ), "name=%s ent=%d in=%d out=%d",
			set->name, (int)args[4], incomingWaveTime, nextWaveTime );
		SP_TRACE("S_AddLocalSet", infoBuf, nextWaveTime);
		return nextWaveTime;
	}
	case SPCG_AS_PARSESETS: {		/* parse ambient set definitions from sound/sound.txt */
		char infoBuf[SPTRACE_STR_MAX];
		CL_SP_ParseAmbientSetFile();
		Com_sprintf( infoBuf, sizeof( infoBuf ), "sets=%d", cl_sp_numAmbientSets );
		SP_TRACE("AS_ParseSets", infoBuf, cl_sp_numAmbientSets);
		return 0;
	}
	case SPCG_AS_ADDENTRY: {		/* add a map-local precache hint for an ambient set */
		const char *ambientName;
		char infoBuf[SPTRACE_STR_MAX];
		ambientName = VMA(1) ? (const char *)VMA(1) : "(null)";
		CL_SP_AmbientAddPrecacheEntry( VMA(1) );
		Com_sprintf( infoBuf, sizeof( infoBuf ), "name=%s parsed=%d cached=%d",
			ambientName, cl_sp_ambientParsed, cl_sp_numAmbientPrecache );
		SP_TRACE("AS_AddEntry", infoBuf, cl_sp_numAmbientPrecache);
		return 0;
	}
	case SPCG_AS_GETBMODELSOUND: {	/* query which ambient sound a brush model should emit */
		cl_sp_ambientSet_t *set;
		sfxHandle_t handle;
		char bmodelBuf[SPTRACE_STR_MAX];

		CL_SP_AmbientEnsureParsed();
		set = CL_SP_FindAmbientSet( VMA(1), CL_SP_AMBIENT_BMODEL );
		if ( !set ) {
			Com_sprintf(bmodelBuf, sizeof(bmodelBuf), "name=%s stage=%d missing",
				(const char *)VMA(1), (int)args[2]);
			SP_TRACE("AS_GetBModelSound", bmodelBuf, -1);
			return -1;
		}

		CL_SP_AmbientEnsureSetRegistered( set );
		if ( args[2] < 0 || args[2] >= set->numSubWaves ) {
			Com_sprintf(bmodelBuf, sizeof(bmodelBuf), "name=%s stage=%d invalid",
				set->name, (int)args[2]);
			SP_TRACE("AS_GetBModelSound", bmodelBuf, -1);
			return -1;
		}

		handle = set->subWaves[args[2]];
		Com_sprintf(bmodelBuf, sizeof(bmodelBuf), "name=%s stage=%d",
			set->name, (int)args[2]);
		SP_TRACE("AS_GetBModelSound", bmodelBuf, handle ? handle : -1);
		return handle ? handle : -1;
	}
	case SPCG_S_GETSAMPLELENGTH: {	/* query duration of a sound sample in milliseconds */
		int len = CL_SP_GetSampleLengthMilliseconds( args[1] );
		char infoBuf[32];
		Com_sprintf( infoBuf, sizeof( infoBuf ), "sfx=%d", (int)args[1] );
		SP_TRACE("S_GetSampleLength", infoBuf, len);
		return len;
	}

	default:
		Com_Error( ERR_DROP, "Bad SP cgame system trap: %ld", (long int) args[0] );
	}
	return 0;
}

#endif /* ELITEFORCE */

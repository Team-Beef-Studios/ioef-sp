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
// sv_game_sp.c -- bridge between ioEF engine and EF1 singleplayer game module
//
// The EF1 SP game module uses a Q2-style GetGameAPI interface with function
// pointer structs, rather than the Q3-style VM (dllEntry/vmMain) interface.
// This file provides the translation layer.

#ifdef ELITEFORCE

#include "server.h"
#include "../sys/sys_loadlib.h"
#include "../qcommon/sp_types.h"

// ============================================================================
// SP game module type definitions
//
// Struct types for playerState_t, entityState_t, gentity_t, and snapshot_t
// are defined in sp_types.h (shared with cl_cgame_sp.c).
//
// The remaining types below are server-side only (game import/export tables).
// ============================================================================

#define SP_GAME_API_VERSION	6

typedef enum {
	eNO = 0,
	eFULL,
	eAUTO,
} SavedGameJustLoaded_e;

//
// functions provided by the engine to the SP game module
//
typedef struct {
	void	(*Printf)( const char *fmt, ... );
	void	(*WriteCam)( const char *text );
	void	(*Error)( int, const char *fmt, ... );
	int	(*Milliseconds)( void );
	cvar_t	*(*cvar)( const char *var_name, const char *value, int flags );
	void	(*cvar_set)( const char *var_name, const char *value );
	int	(*Cvar_VariableIntegerValue)( const char *var_name );
	void	(*Cvar_VariableStringBuffer)( const char *var_name, char *buffer, int bufsize );
	int	(*argc)( void );
	char	*(*argv)( int n );
	int	(*FS_FOpenFile)( const char *qpath, fileHandle_t *file, fsMode_t mode );
	int	(*FS_Read)( void *buffer, int len, fileHandle_t f );
	int	(*FS_Write)( const void *buffer, int len, fileHandle_t f );
	void	(*FS_FCloseFile)( fileHandle_t f );
	int	(*FS_ReadFile)( const char *name, void **buf );
	void	(*FS_FreeFile)( void *buf );
	int	(*FS_GetFileList)( const char *path, const char *extension, char *listbuf, int bufsize );
	qboolean	(*AppendToSaveGame)(unsigned long chid, void *data, int length);
	int		(*ReadFromSaveGame)(unsigned long chid, void *pvAddress, int iLength, void **ppvAddressPtr);
	int		(*ReadFromSaveGameOptional)(unsigned long chid, void *pvAddress, int iLength, void **ppvAddressPtr);
	void	(*SendConsoleCommand)( const char *text );
	void	(*DropClient)( int clientNum, const char *reason );
	void	(*SendServerCommand)( int clientNum, const char *fmt, ... );
	void	(*SetConfigstring)( int num, const char *string );
	void	(*GetConfigstring)( int num, char *buffer, int bufferSize );
	void	(*GetUserinfo)( int num, char *buffer, int bufferSize );
	void	(*SetUserinfo)( int num, const char *buffer );
	void	(*GetServerinfo)( char *buffer, int bufferSize );
	void	(*SetBrushModel)( sp_gentity_t *ent, const char *name );
	void	(*trace)( trace_t *results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int passEntityNum, int contentmask );
	int	(*pointcontents)( const vec3_t point, int passEntityNum );
	qboolean	(*inPVS)( const vec3_t p1, const vec3_t p2 );
	qboolean	(*inPVSIgnorePortals)( const vec3_t p1, const vec3_t p2 );
	void		(*AdjustAreaPortalState)( sp_gentity_t *ent, qboolean open );
	qboolean	(*AreasConnected)( int area1, int area2 );
	void	(*linkentity)( sp_gentity_t *ent );
	void	(*unlinkentity)( sp_gentity_t *ent );
	int	(*EntitiesInBox)( const vec3_t mins, const vec3_t maxs, sp_gentity_t **list, int maxcount );
	qboolean	(*EntityContact)( const vec3_t mins, const vec3_t maxs, const sp_gentity_t *ent );
	int	*S_Override;
	void	*(*Malloc)( int bytes );
	void	(*Free)( void *buf );
} sp_game_import_t;

//
// functions exported by the SP game module
//
typedef struct {
	int		apiversion;
	void		(*Init)( const char *mapname, const char *spawntarget, int checkSum, const char *entstring, int levelTime, int randomSeed, int globalTime, SavedGameJustLoaded_e eSavedGameJustLoaded, qboolean qbLoadTransition );
	void		(*Shutdown) (void);
	void		(*WriteLevel) (qboolean qbAutosave);
	void		(*ReadLevel)  (qboolean qbAutosave, qboolean qbLoadTransition);
	qboolean	(*GameAllowedToSaveHere)(void);
	char		*(*ClientConnect)( int clientNum, qboolean firstTime, SavedGameJustLoaded_e eSavedGameJustLoaded );
	void		(*ClientBegin)( int clientNum, usercmd_t *cmd, SavedGameJustLoaded_e eSavedGameJustLoaded);
	void		(*ClientUserinfoChanged)( int clientNum );
	void		(*ClientDisconnect)( int clientNum );
	void		(*ClientCommand)( int clientNum );
	void		(*ClientThink)( int clientNum, usercmd_t *cmd );
	void		(*RunFrame)( int levelTime );
	qboolean	(*ConsoleCommand)( void );
	struct sp_gentity_s	*gentities;
	int		gentitySize;
	int		num_entities;
} sp_game_export_t;

// sp_playerState_t and sp_entityState_t are defined in sp_types.h

// Shadow playerState_t in ioEF layout for snapshot building
static playerState_t sv_sp_playerState;

// Sync SP playerState to ioEF layout
static void SV_SP_SyncPlayerState( void *sp_client ) {
	sp_playerState_t *sp_ps = (sp_playerState_t *)sp_client;
	playerState_t *ps = &sv_sp_playerState;

	memset( ps, 0, sizeof( *ps ) );

	ps->commandTime     = sp_ps->commandTime;
	ps->pm_type         = sp_ps->pm_type;
	ps->bobCycle        = sp_ps->bobCycle;
	ps->pm_flags        = sp_ps->pm_flags;
	ps->pm_time         = sp_ps->pm_time;
	VectorCopy( sp_ps->origin, ps->origin );
	VectorCopy( sp_ps->velocity, ps->velocity );
	ps->weaponTime      = sp_ps->weaponTime;
	ps->gravity         = sp_ps->gravity;
	ps->speed           = sp_ps->speed;
	ps->delta_angles[0] = sp_ps->delta_angles[0];
	ps->delta_angles[1] = sp_ps->delta_angles[1];
	ps->delta_angles[2] = sp_ps->delta_angles[2];
	ps->groundEntityNum = sp_ps->groundEntityNum;
	ps->legsTimer       = sp_ps->legsAnimTimer;
	ps->legsAnim        = sp_ps->legsAnim;
	ps->torsoTimer      = sp_ps->torsoAnimTimer;
	ps->torsoAnim       = sp_ps->torsoAnim;
	ps->movementDir     = sp_ps->movementDir;
	ps->eFlags          = sp_ps->eFlags;
	ps->eventSequence   = sp_ps->eventSequence;
	ps->events[0]       = sp_ps->events[0];
	ps->events[1]       = sp_ps->events[1];
	ps->eventParms[0]   = sp_ps->eventParms[0];
	ps->eventParms[1]   = sp_ps->eventParms[1];
	ps->externalEvent      = sp_ps->externalEvent;
	ps->externalEventParm  = sp_ps->externalEventParm;
	ps->externalEventTime  = sp_ps->externalEventTime;
	ps->clientNum       = sp_ps->clientNum;
	ps->weapon          = sp_ps->weapon;
	ps->weaponstate     = sp_ps->weaponstate;
	VectorCopy( sp_ps->viewangles, ps->viewangles );
	ps->viewheight      = sp_ps->viewheight;
	ps->damageEvent     = sp_ps->damageEvent;
	ps->damageYaw       = sp_ps->damageYaw;
	ps->damagePitch     = sp_ps->damagePitch;
	ps->damageCount     = sp_ps->damageCount;
	memcpy( ps->stats, sp_ps->stats, sizeof( ps->stats ) );
	memcpy( ps->persistant, sp_ps->persistant, sizeof( ps->persistant ) );
	memcpy( ps->powerups, sp_ps->powerups, sizeof( ps->powerups ) );
	// SP ammo is [4], ioEF ammo is [MAX_WEAPONS=16]; only copy the SP portion
	memset( ps->ammo, 0, sizeof( ps->ammo ) );
	memcpy( ps->ammo, sp_ps->ammo, sizeof( sp_ps->ammo ) );
	ps->ping            = sp_ps->ping;
}

// ============================================================================
// Module state
// ============================================================================

#define SP_SAVE_CHUNK_MAGIC          0x1234ABCDu
#define SP_SAVE_CHUNK_COMM           0x434F4D4Du
#define SP_SAVE_CHUNK_SHOT           0x53484F54u
#define SP_SAVE_CHUNK_MPCM           0x4D50434Du
#define SP_SAVE_CHUNK_GAME           0x47414D45u
#define SP_SAVE_CHUNK_CVCN           0x4356434Eu
#define SP_SAVE_CHUNK_CVAR           0x43564152u
#define SP_SAVE_CHUNK_VALU           0x56414C55u
#define SP_SAVE_CHUNK_TIME           0x54494D45u
#define SP_SAVE_CHUNK_TIMR           0x54494D52u
#define SP_SAVE_CHUNK_CSCN           0x4353434Eu
#define SP_SAVE_CHUNK_CSIN           0x4353494Eu
#define SP_SAVE_CHUNK_CSDA           0x43534441u
#define SP_SAVE_CHUNK_CVSV           0x43565356u
#define SP_SAVE_CHUNK_AMMO           0x414D4D4Fu
#define SP_SAVE_CHUNK_ADPT           0x41445054u
#define SP_SAVE_COMMENT_SIZE         128
#define SP_SAVE_SORTINFO_OFFSET      64
#define SP_SAVE_SHOT_SIZE            ( 256 * 256 * 4 )
#define SP_SAVE_MAP_SIZE             1024
#define SP_SAVE_CVAR_SIZE            1024

typedef struct {
	fileHandle_t	file;
	qboolean		active;
	qboolean		failed;
	char			qpath[MAX_QPATH];
} sv_sp_save_stream_t;

// game_export_t pointer returned by the SP game module
static sp_game_export_t	*ge;

// DLL handle for the SP game module
static void		*gameLibrary;

// Track whether entity data has been located in the engine
static qboolean entityDataLocated;

// Shadow entity array: the engine reads sharedEntity_t layout, but the SP
// game's gentity_t has a different layout.  We sync fields between the two
// before and after every engine call that touches entities.
#define SP_MAX_GENTITIES 1024
static sharedEntity_t sv_sp_entities[SP_MAX_GENTITIES];
static sv_sp_save_stream_t sv_sp_saveWrite;
static sv_sp_save_stream_t sv_sp_saveRead;
static SavedGameJustLoaded_e sv_sp_savedGameJustLoaded;
static SavedGameJustLoaded_e sv_sp_pendingLoadType;
static char sv_sp_pendingLoadQPath[MAX_QPATH];
static char sv_sp_pendingLoadMap[MAX_QPATH];

extern const byte *CL_SP_GetStoredSaveComment( void );
extern qboolean CL_SP_CopySaveScreenshot( byte *outRGBA, int outSize );
extern cvar_t *cvar_vars;
qboolean SV_SP_IsActive( void );

static void SV_SP_SyncToShared( sp_gentity_t *sp_ent ) {
	int num = sp_ent->s.number;
	sharedEntity_t *se;
	sp_entityState_t *src;
	entityState_t *dst;

	if ( num < 0 || num >= SP_MAX_GENTITIES ) return;
	se = &sv_sp_entities[num];
	src = &sp_ent->s;
	dst = &se->s;

	// Translate sp_entityState_t -> ioEF entityState_t field by field.
	// Fields before modelindex3 are at identical offsets and can be bulk-copied.
	dst->number          = src->number;
	dst->eType           = src->eType;
	dst->eFlags          = src->eFlags;
	dst->pos             = src->pos;
	dst->apos            = src->apos;
	dst->time            = src->time;
	dst->time2           = src->time2;
	VectorCopy( src->origin, dst->origin );
	VectorCopy( src->origin2, dst->origin2 );
	VectorCopy( src->angles, dst->angles );
	VectorCopy( src->angles2, dst->angles2 );
	dst->otherEntityNum  = src->otherEntityNum;
	dst->otherEntityNum2 = src->otherEntityNum2;
	dst->groundEntityNum = src->groundEntityNum;
	dst->constantLight   = src->constantLight;
	dst->loopSound       = src->loopSound;
	dst->modelindex      = src->modelindex;
	dst->modelindex2     = src->modelindex2;
	// Skip modelindex3 (SP-only, no ioEF equivalent)
	dst->clientNum       = src->clientNum;
	dst->frame           = src->frame;
	dst->solid           = src->solid;
	dst->event           = src->event;
	dst->eventParm       = src->eventParm;
	dst->powerups        = src->powerups;
	dst->weapon          = src->weapon;
	dst->legsAnim        = src->legsAnim;
	// Skip legsAnimTimer (SP-only)
	dst->torsoAnim       = src->torsoAnim;
	// Skip torsoAnimTimer, scale, pushVec (SP-only)

	// Entity shared state
	se->r.linked       = sp_ent->linked;
	se->r.linkcount    = 0;
	se->r.svFlags      = sp_ent->svFlags;
	se->r.singleClient = 0;
	se->r.bmodel       = sp_ent->bmodel;
	VectorCopy( sp_ent->mins, se->r.mins );
	VectorCopy( sp_ent->maxs, se->r.maxs );
	se->r.contents     = sp_ent->contents;
	VectorCopy( sp_ent->absmin, se->r.absmin );
	VectorCopy( sp_ent->absmax, se->r.absmax );
	VectorCopy( sp_ent->currentOrigin, se->r.currentOrigin );
	VectorCopy( sp_ent->currentAngles, se->r.currentAngles );
	se->r.ownerNum     = sp_ent->owner ? sp_ent->owner->s.number : ENTITYNUM_NONE;
}

static void SV_SP_SyncFromShared( sp_gentity_t *sp_ent ) {
	int num = sp_ent->s.number;
	sharedEntity_t *se;
	entityState_t *src;
	sp_entityState_t *dst;

	if ( num < 0 || num >= SP_MAX_GENTITIES ) return;
	se = &sv_sp_entities[num];
	src = &se->s;
	dst = &sp_ent->s;

	// Copy back fields the engine may have modified (e.g. after SV_LinkEntity).
	// Only copy common fields; preserve SP-specific fields untouched.
	dst->number          = src->number;
	dst->eType           = src->eType;
	dst->eFlags          = src->eFlags;
	dst->pos             = src->pos;
	dst->apos            = src->apos;
	dst->time            = src->time;
	dst->time2           = src->time2;
	VectorCopy( src->origin, dst->origin );
	VectorCopy( src->origin2, dst->origin2 );
	VectorCopy( src->angles, dst->angles );
	VectorCopy( src->angles2, dst->angles2 );
	dst->otherEntityNum  = src->otherEntityNum;
	dst->otherEntityNum2 = src->otherEntityNum2;
	dst->groundEntityNum = src->groundEntityNum;
	dst->constantLight   = src->constantLight;
	dst->loopSound       = src->loopSound;
	dst->modelindex      = src->modelindex;
	dst->modelindex2     = src->modelindex2;
	// Do NOT overwrite modelindex3 (SP-only)
	dst->clientNum       = src->clientNum;
	dst->frame           = src->frame;
	dst->solid           = src->solid;
	dst->event           = src->event;
	dst->eventParm       = src->eventParm;
	dst->powerups        = src->powerups;
	dst->weapon          = src->weapon;
	dst->legsAnim        = src->legsAnim;
	// Do NOT overwrite legsAnimTimer (SP-only)
	dst->torsoAnim       = src->torsoAnim;
	// Do NOT overwrite torsoAnimTimer, scale, pushVec (SP-only)

	sp_ent->linked = se->r.linked;
	VectorCopy( se->r.absmin, sp_ent->absmin );
	VectorCopy( se->r.absmax, sp_ent->absmax );
}

// Ensure entity data is registered with the engine.
// Called lazily because ge->gentities isn't populated until partway
// through ge->Init() (the game allocates its entity array during init).
static void SV_SP_EnsureEntityData( void ) {
	if ( !entityDataLocated && ge && ge->gentities ) {
		sv.gentities = sv_sp_entities;
		sv.gentitySize = sizeof( sharedEntity_t );
		sv.num_entities = ge->num_entities;

		// Translate SP playerState to ioEF layout for the engine.
		// The SP playerState has a different field layout.
		{
			sp_gentity_t *playerEnt = (sp_gentity_t *)( (byte *)ge->gentities );
			if ( playerEnt->client ) {
				SV_SP_SyncPlayerState( playerEnt->client );
				sv.gameClients = &sv_sp_playerState;
				sv.gameClientSize = 0; // SP only has 1 client
			} else {
				// Client not connected yet - use a static dummy to prevent NULL crashes
				sv.gameClients = &sv_sp_playerState;
				sv.gameClientSize = 0;
			}
		}

		entityDataLocated = qtrue;
		Com_Printf( "SP entity data located: %d entities, gentitySize=%d (shadow=%d)\n",
					sv.num_entities, ge->gentitySize, (int)sizeof( sharedEntity_t ) );
	}
}

// Set up gameClients pointer from the SP game's client struct.
// Called after ClientConnect/ClientBegin when the client pointer is valid.
static void SV_SP_SetupGameClient( int clientNum ) {
	sp_gentity_t *ent = (sp_gentity_t *)( (byte *)ge->gentities + ge->gentitySize * clientNum );
	if ( ent->client ) {
		// Translate SP playerState to ioEF layout for the engine
		SV_SP_SyncPlayerState( ent->client );
		sv.gameClients = &sv_sp_playerState;
		sv.gameClientSize = 0; // Only 1 client in SP, stride doesn't matter
		Com_Printf( "SP game client %d set up at %p\n", clientNum, (void *)ent->client );
	}
}

// Sync ALL active entities from the SP game array to the shadow array.
// Called before the engine builds snapshots so entity states are current.
static void SV_SP_SyncAllEntities( void ) {
	int i;
	int num_ents;

	if ( !ge || !ge->gentities ) return;

	num_ents = ge->num_entities;
	sv.num_entities = num_ents;

	for ( i = 0; i < num_ents && i < SP_MAX_GENTITIES; i++ ) {
		sp_gentity_t *sp_ent = (sp_gentity_t *)( (byte *)ge->gentities + ge->gentitySize * i );
		if ( sp_ent->inuse ) {
			SV_SP_SyncToShared( sp_ent );
		}
	}

	// Translate SP playerState to ioEF layout so the engine's snapshot
	// builder can read fields (clientNum, origin, viewheight) at the
	// correct offsets.  The cgame gets raw SP data via SV_SP_GetRawPlayerState.
	{
		sp_gentity_t *playerEnt = (sp_gentity_t *)ge->gentities;
		if ( playerEnt->client ) {
			SV_SP_SyncPlayerState( playerEnt->client );
			sv.gameClients = &sv_sp_playerState;
			sv.gameClientSize = 0;
		}
	}
}

static void SV_SP_ClearPendingLoad( void ) {
	sv_sp_pendingLoadType = eNO;
	sv_sp_pendingLoadQPath[0] = '\0';
	sv_sp_pendingLoadMap[0] = '\0';
}

static void SV_SP_CloseSaveStream( sv_sp_save_stream_t *stream ) {
	if ( stream->file ) {
		FS_FCloseFile( stream->file );
	}

	stream->file = 0;
	stream->active = qfalse;
	stream->failed = qfalse;
	stream->qpath[0] = '\0';
}

static qboolean SV_SP_NormalizeSaveSlotName( const char *slotName, char *outBaseName, int outSize ) {
	size_t length;

	if ( !slotName || !slotName[0] ) {
		return qfalse;
	}

	Q_strncpyz( outBaseName, slotName, outSize );
	length = strlen( outBaseName );
	if ( length > 4 && !Q_stricmp( outBaseName + length - 4, ".sav" ) ) {
		outBaseName[length - 4] = '\0';
	}

	if ( !outBaseName[0] ) {
		return qfalse;
	}

	if ( FS_CheckDirTraversal( outBaseName ) ||
		strchr( outBaseName, '/' ) ||
		strchr( outBaseName, '\\' ) ||
		strchr( outBaseName, ':' ) ) {
		return qfalse;
	}

	if ( !Q_stricmp( outBaseName, "current" ) || !Q_stricmp( outBaseName, "virtual" ) ) {
		return qfalse;
	}

	return qtrue;
}

static void SV_SP_BuildSaveQPath( const char *baseName, char *outQPath, int outSize ) {
	Com_sprintf( outQPath, outSize, "saves/%s.sav", baseName );
}

static void SV_SP_GetMapName( char *outMapName, int outSize ) {
	Q_strncpyz( outMapName, Cvar_VariableString( "mapname" ), outSize );
}

static void SV_SP_GetMapLabel( char *outMapLabel, int outSize ) {
	SV_GetConfigstring( CS_MESSAGE, outMapLabel, outSize );
	if ( !outMapLabel[0] ) {
		SV_SP_GetMapName( outMapLabel, outSize );
	}
	if ( !outMapLabel[0] ) {
		Q_strncpyz( outMapLabel, "unknown", outSize );
	}
}

static void SV_SP_BuildSaveComment( qboolean autosave, byte outComment[SP_SAVE_COMMENT_SIZE] ) {
	char display[SP_SAVE_SORTINFO_OFFSET];
	char sortInfo[SP_SAVE_SORTINFO_OFFSET];
	char mapLabel[MAX_QPATH];
	qtime_t now;
	const byte *storedComment;

	memset( outComment, 0, SP_SAVE_COMMENT_SIZE );
	memset( display, 0, sizeof( display ) );
	memset( sortInfo, 0, sizeof( sortInfo ) );

	SV_SP_GetMapLabel( mapLabel, sizeof( mapLabel ) );
	Com_RealTime( &now );
	storedComment = CL_SP_GetStoredSaveComment();

	if ( autosave ) {
		Com_sprintf( display, sizeof( display ), "-------> %s", mapLabel );
	} else if ( storedComment && storedComment[0] ) {
		Q_strncpyz( display, (const char *)storedComment, sizeof( display ) );
	} else {
		Com_sprintf( display, sizeof( display ), "%02d:%02d %02d/%02d/%02d  %s",
			now.tm_hour, now.tm_min,
			now.tm_mon + 1, now.tm_mday, ( now.tm_year + 1900 ) % 100,
			mapLabel );
	}

	Com_sprintf( sortInfo, sizeof( sortInfo ), "%02d:%02d %02d/%02d/%02d %s",
		now.tm_hour, now.tm_min,
		now.tm_mon + 1, now.tm_mday, ( now.tm_year + 1900 ) % 100,
		mapLabel );

	Q_strncpyz( (char *)outComment, display, SP_SAVE_SORTINFO_OFFSET );
	Q_strncpyz( (char *)outComment + SP_SAVE_SORTINFO_OFFSET, sortInfo, SP_SAVE_SORTINFO_OFFSET );
}

static qboolean SV_SP_WriteSaveChunk( sv_sp_save_stream_t *stream, unsigned long chunkId, const void *data, int length ) {
	unsigned int fileChunkId;
	unsigned int fileLength;
	unsigned int fileChecksum;
	unsigned int fileMagic;
	unsigned int checksum;

	if ( !stream || !stream->active || !stream->file ) {
		return qfalse;
	}

	checksum = length > 0 ? Com_BlockChecksum( data, length ) : 0;
	fileChunkId = LittleLong( chunkId );
	fileLength = LittleLong( length );
	fileChecksum = LittleLong( checksum );
	fileMagic = LittleLong( SP_SAVE_CHUNK_MAGIC );

	if ( FS_Write( &fileChunkId, sizeof( fileChunkId ), stream->file ) != sizeof( fileChunkId ) ||
		FS_Write( &fileLength, sizeof( fileLength ), stream->file ) != sizeof( fileLength ) ||
		FS_Write( &fileChecksum, sizeof( fileChecksum ), stream->file ) != sizeof( fileChecksum ) ||
		( length > 0 && FS_Write( data, length, stream->file ) != length ) ||
		FS_Write( &fileMagic, sizeof( fileMagic ), stream->file ) != sizeof( fileMagic ) ) {
		stream->failed = qtrue;
		Com_Printf( "^1Failed to write save chunk 0x%08lx to %s\n", chunkId, stream->qpath );
		return qfalse;
	}

	return qtrue;
}

static qboolean SV_SP_ReadSaveChunkHeader( fileHandle_t file, unsigned int *chunkId, unsigned int *length, unsigned int *checksum ) {
	unsigned int fileChunkId;
	unsigned int fileLength;
	unsigned int fileChecksum;

	if ( FS_Read( &fileChunkId, sizeof( fileChunkId ), file ) != sizeof( fileChunkId ) ||
		FS_Read( &fileLength, sizeof( fileLength ), file ) != sizeof( fileLength ) ||
		FS_Read( &fileChecksum, sizeof( fileChecksum ), file ) != sizeof( fileChecksum ) ) {
		return qfalse;
	}

	if ( chunkId ) {
		*chunkId = LittleLong( fileChunkId );
	}
	if ( length ) {
		*length = LittleLong( fileLength );
	}
	if ( checksum ) {
		*checksum = LittleLong( fileChecksum );
	}

	return qtrue;
}

static int SV_SP_SkipSaveChunk( fileHandle_t file, unsigned long expectedChunkId, int expectedLength, qboolean quiet ) {
	unsigned int chunkId;
	unsigned int length;
	unsigned int checksum;
	unsigned int fileMagic;

	if ( !SV_SP_ReadSaveChunkHeader( file, &chunkId, &length, &checksum ) ) {
		if ( !quiet ) {
			Com_Printf( "^1Failed to read save header while looking for chunk 0x%08lx\n", expectedChunkId );
		}
		return 0;
	}

	if ( chunkId != expectedChunkId ) {
		if ( !quiet ) {
			Com_Printf( "^1Unexpected save chunk 0x%08x, expected 0x%08lx\n", chunkId, expectedChunkId );
		}
		return 0;
	}

	if ( expectedLength > 0 && (int)length != expectedLength ) {
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx has invalid length %u (expected %d)\n",
				expectedChunkId, length, expectedLength );
		}
		return 0;
	}

	if ( FS_Seek( file, length, FS_SEEK_CUR ) != 0 ||
		FS_Read( &fileMagic, sizeof( fileMagic ), file ) != sizeof( fileMagic ) ) {
		if ( !quiet ) {
			Com_Printf( "^1Failed to skip save chunk 0x%08lx\n", expectedChunkId );
		}
		return 0;
	}

	if ( LittleLong( fileMagic ) != SP_SAVE_CHUNK_MAGIC ) {
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx has bad magic\n", expectedChunkId );
		}
		return 0;
	}

	return length;
}

static int SV_SP_ReadSaveChunk( sv_sp_save_stream_t *stream, unsigned long expectedChunkId,
		void *buffer, int expectedLength, void **outAllocatedBuffer, qboolean quiet ) {
	unsigned int chunkId;
	unsigned int length;
	unsigned int checksum;
	unsigned int fileMagic;
	byte *chunkData;
	qboolean allocated;

	if ( outAllocatedBuffer ) {
		*outAllocatedBuffer = NULL;
	}

	if ( !stream || !stream->active || !stream->file ) {
		return 0;
	}

	if ( !SV_SP_ReadSaveChunkHeader( stream->file, &chunkId, &length, &checksum ) ) {
		stream->failed = qtrue;
		if ( !quiet ) {
			Com_Printf( "^1Failed to read save header from %s\n", stream->qpath );
		}
		return 0;
	}

	if ( chunkId != expectedChunkId ) {
		if ( !quiet ) {
			Com_Printf( "^1Unexpected save chunk 0x%08x in %s, expected 0x%08lx\n",
				chunkId, stream->qpath, expectedChunkId );
		}
		return 0;
	}

	if ( expectedLength > 0 && (int)length != expectedLength ) {
		stream->failed = qtrue;
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx has invalid length %u in %s\n",
				expectedChunkId, length, stream->qpath );
		}
		return 0;
	}

	chunkData = buffer;
	allocated = qfalse;
	if ( !chunkData ) {
		if ( !outAllocatedBuffer || length == 0 ) {
			stream->failed = qtrue;
			if ( !quiet ) {
				Com_Printf( "^1No destination provided for save chunk 0x%08lx\n", expectedChunkId );
			}
			return 0;
		}

		chunkData = Z_Malloc( length );
		if ( !chunkData ) {
			stream->failed = qtrue;
			if ( !quiet ) {
				Com_Printf( "^1Out of memory reading save chunk 0x%08lx\n", expectedChunkId );
			}
			return 0;
		}

		allocated = qtrue;
		*outAllocatedBuffer = chunkData;
	}

	if ( length > 0 && FS_Read( chunkData, length, stream->file ) != (int)length ) {
		stream->failed = qtrue;
		if ( allocated ) {
			Z_Free( chunkData );
			if ( outAllocatedBuffer ) {
				*outAllocatedBuffer = NULL;
			}
		}
		if ( !quiet ) {
			Com_Printf( "^1Failed to read save chunk 0x%08lx from %s\n", expectedChunkId, stream->qpath );
		}
		return 0;
	}

	if ( FS_Read( &fileMagic, sizeof( fileMagic ), stream->file ) != sizeof( fileMagic ) ||
		LittleLong( fileMagic ) != SP_SAVE_CHUNK_MAGIC ) {
		stream->failed = qtrue;
		if ( allocated ) {
			Z_Free( chunkData );
			if ( outAllocatedBuffer ) {
				*outAllocatedBuffer = NULL;
			}
		}
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx has bad magic in %s\n", expectedChunkId, stream->qpath );
		}
		return 0;
	}

	if ( length > 0 && Com_BlockChecksum( chunkData, length ) != checksum ) {
		stream->failed = qtrue;
		if ( allocated ) {
			Z_Free( chunkData );
			if ( outAllocatedBuffer ) {
				*outAllocatedBuffer = NULL;
			}
		}
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx failed checksum in %s\n", expectedChunkId, stream->qpath );
		}
		return 0;
	}

	return length;
}

static qboolean SV_SP_WriteArchivedCvars( void ) {
	cvar_t *var;
	int count;
	int fileCount;

	count = 0;
	for ( var = cvar_vars; var; var = var->next ) {
		if ( var->flags & CVAR_ARCHIVE ) {
			count++;
		}
	}

	fileCount = LittleLong( count );
	if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_CVCN, &fileCount, sizeof( fileCount ) ) ) {
		return qfalse;
	}

	for ( var = cvar_vars; var; var = var->next ) {
		if ( !( var->flags & CVAR_ARCHIVE ) ) {
			continue;
		}

		if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_CVAR, var->name, strlen( var->name ) + 1 ) ||
			!SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_VALU, var->string, strlen( var->string ) + 1 ) ) {
			return qfalse;
		}
	}

	return qtrue;
}

static qboolean SV_SP_SkipArchivedCvars( sv_sp_save_stream_t *stream ) {
	void *nameBuffer;
	void *valueBuffer;
	int fileCount;
	int i;

	if ( !SV_SP_ReadSaveChunk( stream, SP_SAVE_CHUNK_CVCN, &fileCount, sizeof( fileCount ), NULL, qfalse ) ) {
		return qfalse;
	}

	fileCount = LittleLong( fileCount );
	for ( i = 0; i < fileCount; i++ ) {
		nameBuffer = NULL;
		valueBuffer = NULL;
		if ( !SV_SP_ReadSaveChunk( stream, SP_SAVE_CHUNK_CVAR, NULL, 0, &nameBuffer, qfalse ) ||
			!SV_SP_ReadSaveChunk( stream, SP_SAVE_CHUNK_VALU, NULL, 0, &valueBuffer, qfalse ) ) {
			if ( nameBuffer ) {
				Z_Free( nameBuffer );
			}
			if ( valueBuffer ) {
				Z_Free( valueBuffer );
			}
			return qfalse;
		}

		Z_Free( nameBuffer );
		Z_Free( valueBuffer );
	}

	return qtrue;
}

static qboolean SV_SP_RestoreArchivedCvars( void ) {
	void *nameBuffer;
	void *valueBuffer;
	int fileCount;
	int i;

	if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_CVCN, &fileCount, sizeof( fileCount ), NULL, qfalse ) ) {
		return qfalse;
	}

	fileCount = LittleLong( fileCount );
	for ( i = 0; i < fileCount; i++ ) {
		nameBuffer = NULL;
		valueBuffer = NULL;
		if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_CVAR, NULL, 0, &nameBuffer, qfalse ) ||
			!SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_VALU, NULL, 0, &valueBuffer, qfalse ) ) {
			if ( nameBuffer ) {
				Z_Free( nameBuffer );
			}
			if ( valueBuffer ) {
				Z_Free( valueBuffer );
			}
			return qfalse;
		}

		Cvar_Set( (const char *)nameBuffer, (const char *)valueBuffer );
		Z_Free( nameBuffer );
		Z_Free( valueBuffer );
	}

	return qtrue;
}

static qboolean SV_SP_WriteSpecialSaveCvars( qboolean autosave ) {
	char cvarName[32];
	char value[SP_SAVE_CVAR_SIZE];
	int i;

	if ( !autosave ) {
		return qtrue;
	}

	memset( value, 0, sizeof( value ) );
	Cvar_VariableStringBuffer( "playersave", value, sizeof( value ) );
	if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_CVSV, value, sizeof( value ) ) ) {
		return qfalse;
	}

	for ( i = 0; i < 4; i++ ) {
		Com_sprintf( cvarName, sizeof( cvarName ), "playerammo%d", i );
		memset( value, 0, sizeof( value ) );
		Cvar_VariableStringBuffer( cvarName, value, sizeof( value ) );
		if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_AMMO, value, sizeof( value ) ) ) {
			return qfalse;
		}
	}

	for ( i = 0; i < 32; i++ ) {
		Com_sprintf( cvarName, sizeof( cvarName ), "borgadapt%d", i );
		memset( value, 0, sizeof( value ) );
		Cvar_VariableStringBuffer( cvarName, value, sizeof( value ) );
		if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_ADPT, value, sizeof( value ) ) ) {
			return qfalse;
		}
	}

	return qtrue;
}

static qboolean SV_SP_RestoreSpecialSaveCvars( qboolean autosave ) {
	char cvarName[32];
	char value[SP_SAVE_CVAR_SIZE];
	int i;

	if ( !autosave ) {
		return qtrue;
	}

	if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_CVSV, value, sizeof( value ), NULL, qfalse ) ) {
		return qfalse;
	}
	value[sizeof( value ) - 1] = '\0';
	Cvar_Set( "playersave", value );

	for ( i = 0; i < 4; i++ ) {
		if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_AMMO, value, sizeof( value ), NULL, qfalse ) ) {
			return qfalse;
		}
		value[sizeof( value ) - 1] = '\0';
		Com_sprintf( cvarName, sizeof( cvarName ), "playerammo%d", i );
		Cvar_Set( cvarName, value );
	}

	for ( i = 0; i < 32; i++ ) {
		if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_ADPT, value, sizeof( value ), NULL, qfalse ) ) {
			return qfalse;
		}
		value[sizeof( value ) - 1] = '\0';
		Com_sprintf( cvarName, sizeof( cvarName ), "borgadapt%d", i );
		Cvar_Set( cvarName, value );
	}

	return qtrue;
}

static qboolean SV_SP_WriteServerTimes( void ) {
	int fileTime;
	int fileServerTime;

	fileTime = LittleLong( sv.time );
	fileServerTime = LittleLong( svs.time );

	return SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_TIME, &fileTime, sizeof( fileTime ) ) &&
		SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_TIMR, &fileServerTime, sizeof( fileServerTime ) );
}

static qboolean SV_SP_RestoreServerTimes( void ) {
	int fileTime;
	int fileServerTime;

	if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_TIME, &fileTime, sizeof( fileTime ), NULL, qfalse ) ||
		!SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_TIMR, &fileServerTime, sizeof( fileServerTime ), NULL, qfalse ) ) {
		return qfalse;
	}

	sv.time = LittleLong( fileTime );
	svs.time = LittleLong( fileServerTime );
	return qtrue;
}

static qboolean SV_SP_WriteConfigstrings( void ) {
	int configCount;
	int fileCount;
	int index;

	configCount = 0;
	for ( index = 0; index < MAX_CONFIGSTRINGS; index++ ) {
		if ( index == CS_SERVERINFO ) {
			continue;
		}
		if ( sv.configstrings[index] && sv.configstrings[index][0] ) {
			configCount++;
		}
	}

	fileCount = LittleLong( configCount );
	if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_CSCN, &fileCount, sizeof( fileCount ) ) ) {
		return qfalse;
	}

	for ( index = 0; index < MAX_CONFIGSTRINGS; index++ ) {
		int fileIndex;

		if ( index == CS_SERVERINFO ) {
			continue;
		}
		if ( !sv.configstrings[index] || !sv.configstrings[index][0] ) {
			continue;
		}

		fileIndex = LittleLong( index );
		if ( !SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_CSIN, &fileIndex, sizeof( fileIndex ) ) ||
			!SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_CSDA, sv.configstrings[index], strlen( sv.configstrings[index] ) + 1 ) ) {
			return qfalse;
		}
	}

	return qtrue;
}

static qboolean SV_SP_RestoreConfigstrings( void ) {
	void *stringData;
	int fileCount;
	int fileIndex;
	int i;

	if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_CSCN, &fileCount, sizeof( fileCount ), NULL, qfalse ) ) {
		return qfalse;
	}

	fileCount = LittleLong( fileCount );
	for ( i = 0; i < fileCount; i++ ) {
		stringData = NULL;
		if ( !SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_CSIN, &fileIndex, sizeof( fileIndex ), NULL, qfalse ) ||
			!SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_CSDA, NULL, 0, &stringData, qfalse ) ) {
			if ( stringData ) {
				Z_Free( stringData );
			}
			return qfalse;
		}

		fileIndex = LittleLong( fileIndex );
		if ( fileIndex >= 0 && fileIndex < MAX_CONFIGSTRINGS && fileIndex != CS_SERVERINFO ) {
			SV_SetConfigstring( fileIndex, (const char *)stringData );
		}
		Z_Free( stringData );
	}

	return qtrue;
}

static SavedGameJustLoaded_e SV_SP_GameChunkToLoadType( int gameChunkValue ) {
	return gameChunkValue ? eAUTO : eFULL;
}

static qboolean SV_SP_LoadPendingSave( void ) {
	int gameChunkValue;

	if ( sv_sp_pendingLoadType == eNO || !sv_sp_pendingLoadQPath[0] ) {
		return qtrue;
	}

	SV_SP_CloseSaveStream( &sv_sp_saveRead );
	if ( FS_FOpenFileByMode( sv_sp_pendingLoadQPath, &sv_sp_saveRead.file, FS_READ ) < 0 || !sv_sp_saveRead.file ) {
		Com_Printf( "^1Failed to open savegame %s for loading\n", sv_sp_pendingLoadQPath );
		SV_SP_ClearPendingLoad();
		return qfalse;
	}

	sv_sp_saveRead.active = qtrue;
	sv_sp_saveRead.failed = qfalse;
	Q_strncpyz( sv_sp_saveRead.qpath, sv_sp_pendingLoadQPath, sizeof( sv_sp_saveRead.qpath ) );

	if ( !SV_SP_SkipSaveChunk( sv_sp_saveRead.file, SP_SAVE_CHUNK_COMM, SP_SAVE_COMMENT_SIZE, qfalse ) ||
		!SV_SP_SkipSaveChunk( sv_sp_saveRead.file, SP_SAVE_CHUNK_SHOT, SP_SAVE_SHOT_SIZE, qfalse ) ||
		!SV_SP_SkipSaveChunk( sv_sp_saveRead.file, SP_SAVE_CHUNK_MPCM, SP_SAVE_MAP_SIZE, qfalse ) ||
		!SV_SP_RestoreArchivedCvars() ||
		!SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_GAME, &gameChunkValue, sizeof( gameChunkValue ), NULL, qfalse ) ) {
		SV_SP_CloseSaveStream( &sv_sp_saveRead );
		SV_SP_ClearPendingLoad();
		return qfalse;
	}

	gameChunkValue = LittleLong( gameChunkValue );
	if ( !SV_SP_RestoreSpecialSaveCvars( gameChunkValue != 0 ) ||
		!SV_SP_RestoreServerTimes() ||
		!SV_SP_RestoreConfigstrings() ) {
		SV_SP_CloseSaveStream( &sv_sp_saveRead );
		SV_SP_ClearPendingLoad();
		return qfalse;
	}

	ge->ReadLevel( gameChunkValue != 0, qfalse );

	if ( sv_sp_saveRead.failed ) {
		SV_SP_CloseSaveStream( &sv_sp_saveRead );
		SV_SP_ClearPendingLoad();
		return qfalse;
	}

	SV_SP_CloseSaveStream( &sv_sp_saveRead );
	sv_sp_savedGameJustLoaded = sv_sp_pendingLoadType;
	SV_SP_ClearPendingLoad();
	return qtrue;
}

// game_import_t struct we fill and pass to GetGameAPI
static sp_game_import_t	gi;

// Dummy array for S_Override field
static int		s_override_dummy[256];

// ============================================================================
// Wrapper functions for sp_game_import_t
// ============================================================================

static void SV_SP_Printf( const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Printf( "%s", text );
}

static void SV_SP_WriteCam( const char *text ) {
	char mapName[MAX_QPATH];
	char qpath[MAX_QPATH];
	fileHandle_t file;

	if ( !text || !text[0] ) {
		return;
	}

	SV_SP_GetMapName( mapName, sizeof( mapName ) );
	if ( !mapName[0] ) {
		Q_strncpyz( mapName, "unknown", sizeof( mapName ) );
	}

	Com_sprintf( qpath, sizeof( qpath ), "maps/%s.camera", mapName );
	file = FS_FOpenFileAppend( qpath );
	if ( !file ) {
		Com_Printf( "^1Failed to open camera export %s\n", qpath );
		return;
	}

	FS_Write( text, strlen( text ), file );
	FS_FCloseFile( file );
	Com_Printf( "Camera entity appended to %s\n", qpath );
}

static void SV_SP_Error( int errLevel, const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Error( errLevel, "%s", text );
}

static int SV_SP_Milliseconds( void ) {
	return Sys_Milliseconds();
}

static cvar_t *SV_SP_Cvar( const char *var_name, const char *value, int flags ) {
	return Cvar_Get( var_name, value, flags );
}

static void SV_SP_CvarSet( const char *var_name, const char *value ) {
	Cvar_Set( var_name, value );
}

static int SV_SP_CvarVariableIntegerValue( const char *var_name ) {
	return Cvar_VariableIntegerValue( var_name );
}

static void SV_SP_CvarVariableStringBuffer( const char *var_name, char *buffer, int bufsize ) {
	Cvar_VariableStringBuffer( var_name, buffer, bufsize );
}

static int SV_SP_Argc( void ) {
	return Cmd_Argc();
}

static char *SV_SP_Argv( int n ) {
	return Cmd_Argv( n );
}

static int SV_SP_FS_FOpenFile( const char *qpath, fileHandle_t *file, fsMode_t mode ) {
	return FS_FOpenFileByMode( qpath, file, mode );
}

static int SV_SP_FS_Read( void *buffer, int len, fileHandle_t f ) {
	return FS_Read( buffer, len, f );
}

static int SV_SP_FS_Write( const void *buffer, int len, fileHandle_t f ) {
	return FS_Write( buffer, len, f );
}

static void SV_SP_FS_FCloseFile( fileHandle_t f ) {
	FS_FCloseFile( f );
}

static int SV_SP_FS_ReadFile( const char *name, void **buf ) {
	return FS_ReadFile( name, buf );
}

static void SV_SP_FS_FreeFile( void *buf ) {
	FS_FreeFile( buf );
}

static int SV_SP_FS_GetFileList( const char *path, const char *extension, char *listbuf, int bufsize ) {
	return FS_GetFileList( path, extension, listbuf, bufsize );
}

static qboolean SV_SP_AppendToSaveGame( unsigned long chid, void *data, int length ) {
	return SV_SP_WriteSaveChunk( &sv_sp_saveWrite, chid, data, length );
}

static int SV_SP_ReadFromSaveGame( unsigned long chid, void *pvAddress, int iLength, void **ppvAddressPtr ) {
	return SV_SP_ReadSaveChunk( &sv_sp_saveRead, chid, pvAddress, iLength, ppvAddressPtr, qfalse );
}

static int SV_SP_ReadFromSaveGameOptional( unsigned long chid, void *pvAddress, int iLength, void **ppvAddressPtr ) {
	unsigned int nextChunkId;
	unsigned int nextChunkLength;
	unsigned int nextChunkChecksum;
	int offset;

	if ( !sv_sp_saveRead.active || !sv_sp_saveRead.file ) {
		return 0;
	}

	offset = FS_FTell( sv_sp_saveRead.file );
	if ( !SV_SP_ReadSaveChunkHeader( sv_sp_saveRead.file, &nextChunkId, &nextChunkLength, &nextChunkChecksum ) ) {
		sv_sp_saveRead.failed = qtrue;
		return 0;
	}

	if ( nextChunkId != chid ) {
		FS_Seek( sv_sp_saveRead.file, offset, FS_SEEK_SET );
		return 0;
	}

	FS_Seek( sv_sp_saveRead.file, offset, FS_SEEK_SET );
	return SV_SP_ReadSaveChunk( &sv_sp_saveRead, chid, pvAddress, iLength, ppvAddressPtr, qfalse );
}

qboolean SV_SP_SaveGame( const char *slotName ) {
	char baseName[MAX_QPATH];
	char finalQPath[MAX_QPATH];
	char mapName[SP_SAVE_MAP_SIZE];
	byte comment[SP_SAVE_COMMENT_SIZE];
	byte screenshot[SP_SAVE_SHOT_SIZE];
	fileHandle_t file;
	int gameChunkValue;
	qboolean autosave;
	qboolean wroteAllChunks;

	if ( !SV_SP_IsActive() || !ge ) {
		Com_Printf( "save: SP game is not active\n" );
		return qfalse;
	}

	if ( !SV_SP_NormalizeSaveSlotName( slotName, baseName, sizeof( baseName ) ) ) {
		Com_Printf( "save: invalid savegame name '%s'\n", slotName ? slotName : "" );
		return qfalse;
	}

	autosave = !Q_stricmp( baseName, "auto" ) || !Q_stricmp( baseName, "exitholodeck" );
	if ( ge->GameAllowedToSaveHere && !autosave && !ge->GameAllowedToSaveHere() ) {
		Com_Printf( "save: saving is not currently allowed\n" );
		return qfalse;
	}

	SV_SP_BuildSaveQPath( baseName, finalQPath, sizeof( finalQPath ) );

	memset( mapName, 0, sizeof( mapName ) );
	SV_SP_GetMapName( mapName, sizeof( mapName ) );
	SV_SP_BuildSaveComment( autosave, comment );
	if ( !CL_SP_CopySaveScreenshot( screenshot, sizeof( screenshot ) ) ) {
		memset( screenshot, 0, sizeof( screenshot ) );
	}

	gameChunkValue = LittleLong( autosave ? 1 : 0 );

	SV_SP_CloseSaveStream( &sv_sp_saveWrite );
	file = FS_FOpenFileWrite( "saves/current.sav" );
	if ( !file ) {
		Com_Printf( "^1Failed to open temp savegame %s\n", "saves/current.sav" );
		return qfalse;
	}

	sv_sp_saveWrite.file = file;
	sv_sp_saveWrite.active = qtrue;
	sv_sp_saveWrite.failed = qfalse;
	Q_strncpyz( sv_sp_saveWrite.qpath, "saves/current.sav", sizeof( sv_sp_saveWrite.qpath ) );

	wroteAllChunks = SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_COMM, comment, sizeof( comment ) ) &&
		SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_SHOT, screenshot, sizeof( screenshot ) ) &&
		SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_MPCM, mapName, sizeof( mapName ) ) &&
		SV_SP_WriteArchivedCvars() &&
		SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_GAME, &gameChunkValue, sizeof( gameChunkValue ) ) &&
		SV_SP_WriteSpecialSaveCvars( autosave ) &&
		SV_SP_WriteServerTimes() &&
		SV_SP_WriteConfigstrings();

	if ( wroteAllChunks ) {
		ge->WriteLevel( autosave );
		wroteAllChunks = !sv_sp_saveWrite.failed;
	}

	SV_SP_CloseSaveStream( &sv_sp_saveWrite );

	if ( !wroteAllChunks ) {
		FS_HomeRemove( "saves/current.sav" );
		Com_Printf( "^1Failed to save %s\n", baseName );
		return qfalse;
	}

	FS_HomeRemove( finalQPath );
	FS_Rename( "saves/current.sav", finalQPath );
	Com_Printf( "Saved game \"%s\"\n", baseName );
	return qtrue;
}

qboolean SV_SP_LoadGame( const char *slotName ) {
	char baseName[MAX_QPATH];
	char qpath[MAX_QPATH];
	char mapName[SP_SAVE_MAP_SIZE];
	fileHandle_t file;
	int gameChunkValue;

	if ( !SV_SP_IsActive() || !ge ) {
		Com_Printf( "load: SP game is not active\n" );
		return qfalse;
	}

	if ( !SV_SP_NormalizeSaveSlotName( slotName, baseName, sizeof( baseName ) ) ) {
		Com_Printf( "load: invalid savegame name '%s'\n", slotName ? slotName : "" );
		return qfalse;
	}

	SV_SP_BuildSaveQPath( baseName, qpath, sizeof( qpath ) );
	memset( mapName, 0, sizeof( mapName ) );
	gameChunkValue = 0;

	SV_SP_CloseSaveStream( &sv_sp_saveRead );
	if ( FS_FOpenFileByMode( qpath, &file, FS_READ ) < 0 || !file ) {
		Com_Printf( "^1Failed to open savegame %s\n", qpath );
		return qfalse;
	}

	sv_sp_saveRead.file = file;
	sv_sp_saveRead.active = qtrue;
	sv_sp_saveRead.failed = qfalse;
	Q_strncpyz( sv_sp_saveRead.qpath, qpath, sizeof( sv_sp_saveRead.qpath ) );

	if ( !SV_SP_SkipSaveChunk( sv_sp_saveRead.file, SP_SAVE_CHUNK_COMM, SP_SAVE_COMMENT_SIZE, qfalse ) ||
		!SV_SP_SkipSaveChunk( sv_sp_saveRead.file, SP_SAVE_CHUNK_SHOT, SP_SAVE_SHOT_SIZE, qfalse ) ||
		!SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_MPCM, mapName, sizeof( mapName ), NULL, qfalse ) ||
		!SV_SP_SkipArchivedCvars( &sv_sp_saveRead ) ||
		!SV_SP_ReadSaveChunk( &sv_sp_saveRead, SP_SAVE_CHUNK_GAME, &gameChunkValue, sizeof( gameChunkValue ), NULL, qfalse ) ) {
		SV_SP_CloseSaveStream( &sv_sp_saveRead );
		return qfalse;
	}

	SV_SP_CloseSaveStream( &sv_sp_saveRead );

	mapName[sizeof( mapName ) - 1] = '\0';
	gameChunkValue = LittleLong( gameChunkValue );
	sv_sp_pendingLoadType = SV_SP_GameChunkToLoadType( gameChunkValue );
	Q_strncpyz( sv_sp_pendingLoadQPath, qpath, sizeof( sv_sp_pendingLoadQPath ) );
	Q_strncpyz( sv_sp_pendingLoadMap, mapName, sizeof( sv_sp_pendingLoadMap ) );

	Cbuf_ExecuteText( EXEC_APPEND, va( "spmap %s\n", sv_sp_pendingLoadMap ) );
	Com_Printf( "Loading game \"%s\"...\n", baseName );
	return qtrue;
}

qboolean SV_SP_WipeSaveGame( const char *slotName ) {
	char baseName[MAX_QPATH];
	char qpath[MAX_QPATH];

	if ( !SV_SP_NormalizeSaveSlotName( slotName, baseName, sizeof( baseName ) ) ) {
		Com_Printf( "wipe: invalid savegame name '%s'\n", slotName ? slotName : "" );
		return qfalse;
	}

	SV_SP_BuildSaveQPath( baseName, qpath, sizeof( qpath ) );
	FS_HomeRemove( qpath );
	Com_Printf( "Deleted savegame \"%s\"\n", baseName );
	return qtrue;
}

static void SV_SP_SendConsoleCommand( const char *text ) {
	Cbuf_ExecuteText( EXEC_APPEND, text );
}

static void SV_SP_DropClient( int clientNum, const char *reason ) {
	if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
		return;
	}
	SV_DropClient( svs.clients + clientNum, reason );
}

static void SV_SP_SendServerCommand( int clientNum, const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	if ( clientNum == -1 ) {
		SV_SendServerCommand( NULL, "%s", text );
	} else {
		if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
			return;
		}
		SV_SendServerCommand( svs.clients + clientNum, "%s", text );
	}
}

static void SV_SP_SetConfigstring( int num, const char *string ) {
	SV_SetConfigstring( num, string );
}

static void SV_SP_GetConfigstring( int num, char *buffer, int bufferSize ) {
	SV_GetConfigstring( num, buffer, bufferSize );
}

static void SV_SP_GetUserinfo( int num, char *buffer, int bufferSize ) {
	SV_GetUserinfo( num, buffer, bufferSize );
}

static void SV_SP_SetUserinfo( int num, const char *buffer ) {
	SV_SetUserinfo( num, buffer );
}

static void SV_SP_GetServerinfo( char *buffer, int bufferSize ) {
	SV_GetServerinfo( buffer, bufferSize );
}

static void SV_SP_SetBrushModel( sp_gentity_t *ent, const char *name ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( ent );
	SV_SetBrushModel( &sv_sp_entities[ent->s.number], name );
	SV_SP_SyncFromShared( ent );
}

static void SV_SP_Trace( trace_t *results, const vec3_t start, const vec3_t mins,
						 const vec3_t maxs, const vec3_t end, int passEntityNum,
						 int contentmask ) {
	SV_Trace( results, start, (float *)mins, (float *)maxs, end, passEntityNum,
			  contentmask, /*capsule*/ qfalse );
}

static int SV_SP_PointContents( const vec3_t point, int passEntityNum ) {
	return SV_PointContents( point, passEntityNum );
}

static qboolean SV_SP_InPVS( const vec3_t p1, const vec3_t p2 ) {
	return SV_inPVS( p1, p2 );
}

static qboolean SV_SP_InPVSIgnorePortals( const vec3_t p1, const vec3_t p2 ) {
	return SV_inPVSIgnorePortals( p1, p2 );
}

static void SV_SP_AdjustAreaPortalState( sp_gentity_t *ent, qboolean open ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( ent );
	SV_AdjustAreaPortalState( &sv_sp_entities[ent->s.number], open );
}

static qboolean SV_SP_AreasConnected( int area1, int area2 ) {
	return CM_AreasConnected( area1, area2 );
}

static void SV_SP_LinkEntity( sp_gentity_t *ent ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( ent );
	SV_LinkEntity( &sv_sp_entities[ent->s.number] );
	SV_SP_SyncFromShared( ent );
}

static void SV_SP_UnlinkEntity( sp_gentity_t *ent ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( ent );
	SV_UnlinkEntity( &sv_sp_entities[ent->s.number] );
	SV_SP_SyncFromShared( ent );
}

static int SV_SP_EntitiesInBox( const vec3_t mins, const vec3_t maxs, sp_gentity_t **list, int maxcount ) {
	int	entityList[1024];
	int	count;
	int	i;
	int	clampedMax;

	SV_SP_EnsureEntityData();
	clampedMax = maxcount > 1024 ? 1024 : maxcount;
	count = SV_AreaEntities( mins, maxs, entityList, clampedMax );

	for ( i = 0; i < count; i++ ) {
		// Map entity number back to the SP game's entity pointer
		list[i] = (sp_gentity_t *)( (byte *)ge->gentities + ge->gentitySize * entityList[i] );
	}

	return count;
}

static qboolean SV_SP_EntityContact( const vec3_t mins, const vec3_t maxs, const sp_gentity_t *ent ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( (sp_gentity_t *)ent );
	return SV_EntityContact( (float *)mins, (float *)maxs, &sv_sp_entities[ent->s.number],
							/*capsule*/ qfalse );
}

static void *SV_SP_Malloc( int bytes ) {
	return Z_Malloc( bytes );
}

static void SV_SP_Free( void *buf ) {
	Z_Free( buf );
}

// ============================================================================
// Initialization functions
// ============================================================================

/*
===============
SV_SP_InitGameImport

Populates the sp_game_import_t struct with wrapper functions that bridge
from the SP game module's function pointer interface to the engine.
===============
*/
static void SV_SP_InitGameImport( void ) {
	Com_Memset( &gi, 0, sizeof( gi ) );
	Com_Memset( s_override_dummy, 0, sizeof( s_override_dummy ) );

	gi.Printf					= SV_SP_Printf;
	gi.WriteCam					= SV_SP_WriteCam;
	gi.Error					= SV_SP_Error;
	gi.Milliseconds				= SV_SP_Milliseconds;

	gi.cvar						= SV_SP_Cvar;
	gi.cvar_set					= SV_SP_CvarSet;
	gi.Cvar_VariableIntegerValue = SV_SP_CvarVariableIntegerValue;
	gi.Cvar_VariableStringBuffer = SV_SP_CvarVariableStringBuffer;

	gi.argc						= SV_SP_Argc;
	gi.argv						= SV_SP_Argv;

	gi.FS_FOpenFile				= SV_SP_FS_FOpenFile;
	gi.FS_Read					= SV_SP_FS_Read;
	gi.FS_Write					= SV_SP_FS_Write;
	gi.FS_FCloseFile			= SV_SP_FS_FCloseFile;
	gi.FS_ReadFile				= SV_SP_FS_ReadFile;
	gi.FS_FreeFile				= SV_SP_FS_FreeFile;
	gi.FS_GetFileList			= SV_SP_FS_GetFileList;

	gi.AppendToSaveGame			= SV_SP_AppendToSaveGame;
	gi.ReadFromSaveGame			= SV_SP_ReadFromSaveGame;
	gi.ReadFromSaveGameOptional	= SV_SP_ReadFromSaveGameOptional;

	gi.SendConsoleCommand		= SV_SP_SendConsoleCommand;

	gi.DropClient				= SV_SP_DropClient;
	gi.SendServerCommand		= SV_SP_SendServerCommand;

	gi.SetConfigstring			= SV_SP_SetConfigstring;
	gi.GetConfigstring			= SV_SP_GetConfigstring;

	gi.GetUserinfo				= SV_SP_GetUserinfo;
	gi.SetUserinfo				= SV_SP_SetUserinfo;

	gi.GetServerinfo			= SV_SP_GetServerinfo;

	gi.SetBrushModel			= SV_SP_SetBrushModel;
	gi.trace					= SV_SP_Trace;
	gi.pointcontents			= SV_SP_PointContents;

	gi.inPVS					= SV_SP_InPVS;
	gi.inPVSIgnorePortals		= SV_SP_InPVSIgnorePortals;
	gi.AdjustAreaPortalState	= SV_SP_AdjustAreaPortalState;
	gi.AreasConnected			= SV_SP_AreasConnected;

	gi.linkentity				= SV_SP_LinkEntity;
	gi.unlinkentity				= SV_SP_UnlinkEntity;
	gi.EntitiesInBox			= SV_SP_EntitiesInBox;
	gi.EntityContact			= SV_SP_EntityContact;

	gi.S_Override				= s_override_dummy;

	gi.Malloc					= SV_SP_Malloc;
	gi.Free						= SV_SP_Free;
}

/*
===============
SV_SP_InitGameProgs

Loads the EF1 singleplayer game DLL (efgamex86.dll), calls GetGameAPI
to exchange function pointer tables, and validates the API version.
===============
*/

// Stub syscall handler for the cgame side of the SP DLL.
// The SP DLL contains both game (GetGameAPI) and cgame (dllEntry/vmMain).
// The cgame's syscall pointer defaults to -1. If we don't initialize it,
// any cgame code called during game RunFrame will crash.
static intptr_t QDECL SV_SP_CgameSyscallStub( intptr_t arg, ... ) {
	// Most cgame syscalls are rendering/sound calls that don't apply server-side.
	// Return 0 (success/no-op) for everything.
	return 0;
}

void SV_SP_InitGameProgs( void ) {
	sp_game_export_t *(*GetGameAPI)( sp_game_import_t *import );
	char dllPath[MAX_OSPATH];
	const char *basepath;
	const char *gamedir;

	// Set up the import table
	SV_SP_InitGameImport();

	// Load the SP game DLL - search in basepath/gamedir
	Com_Printf( "Loading SP game module: efgamex86.dll\n" );

	basepath = Cvar_VariableString( "fs_basepath" );
	gamedir = Cvar_VariableString( "fs_game" );

	if ( !gamedir[0] ) {
		gamedir = Cvar_VariableString( "com_basegame" );
	}

	// Try basepath/gamedir/efgamex86.dll first
	Com_sprintf( dllPath, sizeof(dllPath), "%s/%s/efgamex86.dll", basepath, gamedir );
	Com_Printf( "Try loading dll file %s\n", dllPath );
	gameLibrary = Sys_LoadLibrary( dllPath );

	if ( !gameLibrary ) {
		// Try just efgamex86.dll in current directory
		gameLibrary = Sys_LoadLibrary( "efgamex86.dll" );
	}

	if ( !gameLibrary ) {
		Com_Error( ERR_FATAL, "SV_SP_InitGameProgs: failed to load efgamex86.dll\n"
				   "Searched: %s", dllPath );
	}

	// Get the GetGameAPI entry point
	GetGameAPI = (sp_game_export_t *(*)(sp_game_import_t *))Sys_LoadFunction( gameLibrary, "GetGameAPI" );

	if ( !GetGameAPI ) {
		Sys_UnloadLibrary( gameLibrary );
		gameLibrary = NULL;
		Com_Error( ERR_FATAL, "SV_SP_InitGameProgs: efgamex86.dll missing GetGameAPI export" );
	}

	// The SP game DLL also contains cgame code (dllEntry/vmMain for client-side).
	// The cgame's syscall function pointer is initialized to -1 (invalid).
	// If we don't call dllEntry, any cgame function called during RunFrame
	// will crash by calling through the -1 pointer.
	// Initialize it with a stub syscall handler that returns 0 for everything.
	{
		void (*cgameDllEntry)( intptr_t (*syscallptr)( intptr_t, ... ) );
		cgameDllEntry = (void (*)(intptr_t (*)(intptr_t, ...)))Sys_LoadFunction( gameLibrary, "dllEntry" );
		if ( cgameDllEntry ) {
			cgameDllEntry( SV_SP_CgameSyscallStub );
			Com_Printf( "SP cgame dllEntry initialized with stub syscall handler\n" );
		}
	}

	// Exchange function pointer tables
	ge = GetGameAPI( &gi );

	if ( !ge ) {
		Sys_UnloadLibrary( gameLibrary );
		gameLibrary = NULL;
		Com_Error( ERR_FATAL, "SV_SP_InitGameProgs: GetGameAPI returned NULL" );
	}

	if ( ge->apiversion != SP_GAME_API_VERSION ) {
		int version = ge->apiversion;
		Sys_UnloadLibrary( gameLibrary );
		gameLibrary = NULL;
		ge = NULL;
		Com_Error( ERR_FATAL, "SV_SP_InitGameProgs: game API version mismatch: expected %d, got %d",
				   SP_GAME_API_VERSION, version );
	}

	Com_Printf( "SP game module loaded successfully (API version %d)\n", ge->apiversion );
}

/*
===============
SV_SP_ShutdownGameProgs

Calls the game module's Shutdown function and unloads the DLL.
===============
*/
void SV_SP_ShutdownGameProgs( void ) {
	SV_SP_CloseSaveStream( &sv_sp_saveWrite );
	SV_SP_CloseSaveStream( &sv_sp_saveRead );

	if ( ge ) {
		ge->Shutdown();
		ge = NULL;
	}

	if ( gameLibrary ) {
		Sys_UnloadLibrary( gameLibrary );
		gameLibrary = NULL;
	}

	entityDataLocated = qfalse;
	sv.gentities = NULL;
	sv.gameClients = NULL;
	sv.num_entities = 0;
	sv_sp_savedGameJustLoaded = eNO;

	Com_Printf( "SP game module unloaded\n" );
}

/*
===============
SV_SP_InitGameVM

Initializes the SP game module by calling ge->Init with the proper
arguments derived from the current server state, then sets up entity
data pointers in the server_t struct.
===============
*/
void SV_SP_InitGameVM( void ) {
	const char	*mapname;
	const char	*entstring;
	char		serverinfo[MAX_INFO_STRING];
	char		loadQPath[MAX_QPATH];
	SavedGameJustLoaded_e loadType;
	qboolean loadTransition;

	if ( !ge ) {
		Com_Error( ERR_FATAL, "SV_SP_InitGameVM: game module not loaded" );
	}

	// Get the map name from the server info configstring
	SV_GetConfigstring( CS_SERVERINFO, serverinfo, sizeof( serverinfo ) );
	mapname = Info_ValueForKey( serverinfo, "mapname" );

	// Get the entity string from the loaded BSP
	entstring = CM_EntityString();

	if ( loadType != eNO && Q_stricmp( sv_sp_pendingLoadMap, mapname ) ) {
		Com_Error( ERR_DROP, "SV_SP_InitGameVM: savegame map '%s' does not match loaded map '%s'",
			sv_sp_pendingLoadMap, mapname );
	}

	// Set up shadow entity array for engine use and reset entity data tracking
	// - it will be set lazily during Init when the game first links an entity
	memset( sv_sp_entities, 0, sizeof( sv_sp_entities ) );
	entityDataLocated = qfalse;
	sv_sp_savedGameJustLoaded = eNO;
	loadType = ( sv_sp_pendingLoadType != eNO ) ? sv_sp_pendingLoadType : eNO;
	loadTransition = qfalse;
	Q_strncpyz( loadQPath, sv_sp_pendingLoadQPath, sizeof( loadQPath ) );

	// Initialize the game module
	ge->Init( mapname,				// mapname
			  "",					// spawntarget
			  sv.checksumFeed,		// checkSum
			  entstring,			// entstring
			  sv.time,				// levelTime
			  rand(),				// randomSeed
			  Com_Milliseconds(),	// globalTime
			  loadType,				// eSavedGameJustLoaded
			  loadTransition );		// qbLoadTransition

	if ( loadType != eNO && !SV_SP_LoadPendingSave() ) {
		Com_Error( ERR_DROP, "SV_SP_InitGameVM: failed to load savegame %s", loadQPath );
	}

	// Ensure entity data is located (it should already be from linkentity calls during Init)
	SV_SP_EnsureEntityData();
}

/*
===============
SV_SP_GameVmMain

A fake vmMain entry point that dispatches VM_Call commands to the
SP game_export_t function pointers. This allows all existing
VM_Call(gvm, GAME_*, ...) sites in the engine to work unchanged.
===============
*/
intptr_t QDECL SV_SP_GameVmMain( int command, ... ) {
	va_list ap;
	int arg0, arg1, arg2;

	va_start( ap, command );
	arg0 = va_arg( ap, int );
	arg1 = va_arg( ap, int );
	arg2 = va_arg( ap, int );
	va_end( ap );
	{
	if ( !ge ) {
		Com_Error( ERR_FATAL, "SV_SP_GameVmMain: game module not loaded" );
		return 0;
	}

	switch ( command ) {
	case GAME_INIT:
		// GAME_INIT is handled by SV_SP_InitGameVM, which is called
		// from SV_InitGameVM. We just need to make sure entity data
		// is wired up.
		SV_SP_InitGameVM();
		return 0;

	case GAME_SHUTDOWN:
		ge->Shutdown();
		return 0;

	case GAME_CLIENT_CONNECT: {
		// Q3 passes: clientNum, firstTime, isBot
		// SP expects: clientNum, firstTime, SavedGameJustLoaded_e
		char *result = ge->ClientConnect( arg0, (qboolean)arg1, sv_sp_savedGameJustLoaded );
		if ( result ) {
			Com_Printf( "SP ClientConnect(%d) REJECTED: %s\n", arg0, result );
			return (intptr_t)result;
		}
		Com_Printf( "SP ClientConnect(%d) accepted\n", arg0 );
		// Set up the game client pointer now that client is connected
		SV_SP_SetupGameClient( arg0 );
		return 0;
	}

	case GAME_CLIENT_BEGIN:
		// Q3 passes: clientNum
		// SP expects: clientNum, usercmd_t*, SavedGameJustLoaded_e
		ge->ClientBegin( arg0, &svs.clients[arg0].lastUsercmd, sv_sp_savedGameJustLoaded );
		sv_sp_savedGameJustLoaded = eNO;
		// Update client pointer in case it changed
		SV_SP_SetupGameClient( arg0 );
		return 0;

	case GAME_CLIENT_USERINFO_CHANGED:
		ge->ClientUserinfoChanged( arg0 );
		return 0;

	case GAME_CLIENT_DISCONNECT:
		ge->ClientDisconnect( arg0 );
		return 0;

	case GAME_CLIENT_COMMAND:
		ge->ClientCommand( arg0 );
		return 0;

	case GAME_CLIENT_THINK:
		// Q3 passes: clientNum
		// SP expects: clientNum, usercmd_t*
		ge->ClientThink( arg0, &svs.clients[arg0].lastUsercmd );
		// Update entity count in case the game changed it
		sv.num_entities = ge->num_entities;
		return 0;

	case GAME_RUN_FRAME:
		// Sync entities before the frame so traces during RunFrame see current data
		SV_SP_SyncAllEntities();
		ge->RunFrame( arg0 );
		// Sync again after the frame for snapshot building
		SV_SP_SyncAllEntities();
		return 0;

	case GAME_CONSOLE_COMMAND:
		return ge->ConsoleCommand();

	default:
		Com_Error( ERR_DROP, "SV_SP_GameVmMain: unknown command %d", command );
		return 0;
	}
	} // end of variadic block
}

/*
===============
SV_SP_GetGameLibrary

Returns the handle to the loaded SP game DLL so the client can
load the cgame from the same DLL.
===============
*/
void *SV_SP_GetGameLibrary( void ) {
	return gameLibrary;
}

sp_game_export_t *SV_SP_GetGameExport( void ) {
	return ge;
}

void *SV_SP_GetRawPlayerState( void ) {
	if ( ge && ge->gentities ) {
		sp_gentity_t *playerEnt = (sp_gentity_t *)ge->gentities;
		if ( playerEnt->client ) {
			return (void *)playerEnt->client;
		}
	}
	return NULL;
}

/*
===============
SV_SP_GetRawEntityState

Returns a pointer to the sp_entityState_t for a given entity number,
read directly from the SP game module's entity array.  Returns NULL
if the entity is out of range or the game module isn't loaded.
===============
*/
sp_entityState_t *SV_SP_GetRawEntityState( int entNum ) {
	sp_gentity_t *ent;
	if ( !ge || !ge->gentities || entNum < 0 || entNum >= ge->num_entities )
		return NULL;
	ent = (sp_gentity_t *)( (byte *)ge->gentities + ge->gentitySize * entNum );
	return &ent->s;
}

/*
===============
SV_SP_IsActive

Returns qtrue if SP game mode is currently active.
===============
*/
qboolean SV_SP_IsActive( void ) {
	return ge != NULL;
}

#endif /* ELITEFORCE */

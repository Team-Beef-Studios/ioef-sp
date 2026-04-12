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
// ============================================================================
// Architecture overview
// ============================================================================
//
// Quake 3 and its derivatives use a VM-based game module interface: the engine
// calls vmMain(command, ...) to dispatch into the module, and the module calls
// back through a syscall function pointer set via dllEntry().  This is the
// "Q3-style" interface.
//
// The EF1 singleplayer game module (efgamex86.dll) predates this convention
// and instead uses the older "Q2-style" GetGameAPI pattern.  The engine fills
// in a struct of function pointers (sp_game_import_t) representing services
// it provides, passes a pointer to the DLL, and receives back a matching
// struct (sp_game_export_t) of function pointers the DLL exports.  All
// subsequent communication happens through these two tables -- there is no
// vmMain, no syscall numbering, and no shared-memory VM sandbox.
//
// A key complication: the EF1 SP DLL is a *combined* game+cgame module.
// It exports both GetGameAPI (server-side game logic) AND the Q3-style
// dllEntry/vmMain pair (client-side cgame rendering).  This means a single
// loaded DLL image serves double duty.  On the server side we call
// GetGameAPI; on the client side, cl_cgame_sp.c calls dllEntry/vmMain.
// Because both sides share global state within the DLL, we must call
// dllEntry with a stub syscall handler during server init (see
// SV_SP_CgameSyscallStub) -- otherwise the cgame's syscall pointer is left
// at its default uninitialized value (-1 / 0xFFFFFFFF), and any cgame code
// that happens to execute during a server-side RunFrame will dereference
// that pointer and crash.
//
// This file provides the full translation layer:
//
//   1. Shadow entity array -- The SP game's gentity_t uses sp_entityState_t
//      (with extra fields like modelindex3, legsAnimTimer, scale, pushVec),
//      but the engine's snapshot builder and collision code expect the ioEF
//      sharedEntity_t layout.  We maintain a parallel array of sharedEntity_t
//      and sync field-by-field before/after engine calls.
//
//   2. PlayerState translation -- The SP playerState_t has a different field
//      layout (different array sizes, extra fields, missing fields).  A
//      translated copy is maintained for the engine's snapshot builder.
//
//   3. GetGameAPI wrapper functions -- Each function pointer in the
//      sp_game_import_t is backed by a thin wrapper that bridges to the
//      engine, handling entity pointer translation where needed.
//
//   4. Save/load system -- Implements a chunk-based binary save format with
//      integrity checking, replacing the original EF1 save code that relied
//      on engine internals we don't have.
//
//   5. GAME_* command dispatch -- SV_SP_GameVmMain translates VM_Call
//      commands (GAME_INIT, GAME_RUN_FRAME, etc.) into the corresponding
//      sp_game_export_t function pointer calls, handling argument
//      differences between the Q3 and SP calling conventions.
// ============================================================================

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

// API version that the engine expects from the SP game DLL.  If the DLL
// returns a different version from ge->apiversion, loading is aborted.
// EF1 SP shipped at version 6; there is no known version 5 or 7.
#define SP_GAME_API_VERSION	6

// Enum used by the save/load system to indicate how a game was loaded.
// eNO = fresh map start (no save loaded), eFULL = full save/load,
// eAUTO = autosave restore (level transition).  Passed to ge->Init,
// ge->ClientConnect, and ge->ClientBegin so the game module knows
// whether to restore entity state or start fresh.
typedef enum {
	eNO = 0,
	eFULL,
	eAUTO,
} SavedGameJustLoaded_e;

//
// functions provided by the engine to the SP game module
//
// This is the Q2-style "game import" table.  The engine fills in every
// function pointer before calling GetGameAPI.  The SP game module stores
// a copy of this struct and calls through these pointers for all engine
// services (printing, filesystem, collision, entity linking, etc.).
//
// Each pointer is backed by a thin SV_SP_* wrapper function defined later
// in this file.  Wrappers that deal with entities (SetBrushModel, linkentity,
// EntitiesInBox, etc.) must translate between sp_gentity_t pointers and
// the engine's sharedEntity_t shadow array.
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
	// Save/load callbacks: the game module writes and reads its own state
	// through these chunk-based I/O functions.  Each chunk is tagged with
	// a 4-byte ID (chid) so the loader can identify and validate blocks.
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
	// S_Override is a pointer to an int array the SP game uses to override
	// sound indices.  In the original EF1 engine this pointed into the sound
	// system's internal tables.  We provide a dummy array since ioEF's sound
	// system doesn't support this mechanism; the game reads/writes it but the
	// values have no effect.
	int	*S_Override;
	void	*(*Malloc)( int bytes );
	void	(*Free)( void *buf );
} sp_game_import_t;

//
// functions exported by the SP game module
//
// Returned by GetGameAPI.  This is the Q2-style "game export" table.
// The engine calls these function pointers to drive the game logic.
//
// Note the signature differences from the Q3 VM interface:
//   - Init takes the map name, entity string, timers, and save-load state
//     directly as parameters (Q3 passes these through syscalls).
//   - ClientConnect/ClientBegin take a SavedGameJustLoaded_e instead of
//     the Q3 isBot flag -- SP has no bots, but needs to know if a save
//     was just loaded so it can skip spawn logic.
//   - ClientThink takes a usercmd_t* directly (Q3 copies it via syscall).
//   - WriteLevel/ReadLevel are the game's save/load entry points.
//   - gentities/gentitySize/num_entities are direct data pointers rather
//     than being communicated via GAME_INIT return values.
//
typedef struct {
	int		apiversion;
	void		(*Init)( const char *mapname, const char *spawntarget, int checkSum, const char *entstring, int levelTime, int randomSeed, int globalTime, SavedGameJustLoaded_e eSavedGameJustLoaded, qboolean qbLoadTransition );
	void		(*Shutdown) (void);
	void		(*WriteLevel) (qboolean qbAutosave);
	void		(*ReadLevel)  (qboolean qbAutosave, qboolean qbLoadTransition);
	qboolean	(*GameAllowedToSaveHere)(void);
	char		*(*ClientConnect)( int clientNum, qboolean firstTime, SavedGameJustLoaded_e eSavedGameJustLoaded );
	void		(*ClientBegin)( int clientNum, sp_usercmd_t *cmd, SavedGameJustLoaded_e eSavedGameJustLoaded);
	void		(*ClientUserinfoChanged)( int clientNum );
	void		(*ClientDisconnect)( int clientNum );
	void		(*ClientCommand)( int clientNum );
	void		(*ClientThink)( int clientNum, sp_usercmd_t *cmd );
	void		(*RunFrame)( int levelTime );
	qboolean	(*ConsoleCommand)( void );
	// The game module sets these after allocating its entity array during Init.
	// gentities points into the game's own heap; gentitySize is the stride
	// (sizeof the game's full gentity_t, which is much larger than sp_gentity_t
	// since it includes private game fields after the engine-visible portion).
	struct sp_gentity_s	*gentities;
	int		gentitySize;
	int		num_entities;
} sp_game_export_t;

// sp_playerState_t and sp_entityState_t are defined in sp_types.h

// ============================================================================
// PlayerState translation
//
// The engine's snapshot builder (SV_BuildClientSnapshot) reads playerState_t
// fields at compile-time offsets.  The SP game module's playerState
// (sp_playerState_t) has a different layout: different field order after
// gravity, different array sizes (ammo[4] vs ammo[16]), extra fields
// (leanofs, friction, borgAdaptHits, pushVec, etc.), and missing fields
// (introTime, damageShieldCount, entityEventSequence).
//
// If we pointed sv.gameClients directly at the SP game's raw playerState
// data, the engine would read garbage -- every field after 'gravity' is at
// a different offset, so clientNum, weapon, origin, viewheight, etc. would
// all be wrong.  The snapshot builder would produce corrupt snapshots, the
// client would render the player at the wrong position, and PVS culling
// (which reads ps->origin and ps->clientNum) would malfunction.
//
// Solution: maintain a single static playerState_t in ioEF layout and
// translate field-by-field from the SP data before each snapshot.  We
// cannot use memcpy because the structs differ in size and field offsets.
// ============================================================================
static playerState_t sv_sp_playerState;

/*
===============
SV_SP_SyncPlayerState

Translates the SP game module's playerState (sp_playerState_t) into the
engine's playerState_t layout.  Called before every snapshot build so the
engine reads correct values for origin, angles, clientNum, weapon, etc.

The SP playerState has several layout differences:
  - legsAnimTimer/torsoAnimTimer map to legsTimer/torsoTimer
  - ammo[4] (SP MAX_AMMO) must be copied into ammo[16] (ioEF MAX_WEAPONS)
    with the remaining slots zeroed
  - Fields like leanofs, friction, scale, borgAdaptHits, pushVec have no
    ioEF equivalent and are silently dropped
  - Fields like introTime, damageShieldCount, entityEventSequence have no
    SP equivalent and are left at zero
===============
*/
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
	// SP ammo array is [4] (MAX_AMMO), ioEF ammo is [16] (MAX_WEAPONS).
	// Zero the full destination first, then copy the 4 SP slots.  The
	// remaining 12 slots stay zero -- they correspond to weapon types
	// that don't exist in SP.
	memset( ps->ammo, 0, sizeof( ps->ammo ) );
	memcpy( ps->ammo, sp_ps->ammo, sizeof( sp_ps->ammo ) );
	ps->ping            = sp_ps->ping;
}

// ============================================================================
// Module state
// ============================================================================

// ============================================================================
// Save file format
//
// Save files use a simple chunk-based binary format.  Each chunk is:
//
//   [4 bytes]  chunk ID   (one of the SP_SAVE_CHUNK_* constants)
//   [4 bytes]  data length in bytes
//   [4 bytes]  checksum   (Com_BlockChecksum of the data, or 0 if length==0)
//   [N bytes]  chunk data (N == length)
//   [4 bytes]  magic sentinel (SP_SAVE_CHUNK_MAGIC = 0x1234ABCD)
//
// The trailing magic value acts as a frame marker -- if it's missing or
// wrong, the chunk is considered corrupt.  All multi-byte values are
// stored in little-endian format.
//
// A complete save file is written in this order:
//   COMM  - human-readable comment (display text + sort key, 128 bytes)
//   SHOT  - screenshot thumbnail (256x256 RGBA, 262144 bytes)
//   MPCM  - map name string (1024 bytes, null-padded)
//   CVCN  - count of archived cvars (int)
//     CVAR/VALU pairs (one pair per archived cvar)
//   GAME  - autosave flag (int: 0 = full save, 1 = autosave)
//   [if autosave:]
//     CVSV  - "playersave" cvar value
//     AMMO  - "playerammoN" cvar values (4 chunks, one per ammo slot)
//     ADPT  - "borgadaptN" cvar values (32 chunks, one per weapon slot)
//   TIME  - sv.time
//   TIMR  - svs.time
//   CSCN  - count of configstrings
//     CSIN/CSDA pairs (index + data, one pair per configstring)
//   [game module level data, written by ge->WriteLevel]
//
// The chunk IDs are ASCII FourCC codes (e.g., 0x434F4D4D = "COMM").
// ============================================================================

#define SP_SAVE_CHUNK_MAGIC          0x1234ABCDu  // Sentinel written after every chunk's data
#define SP_SAVE_CHUNK_COMM           0x434F4D4Du  // "COMM" - save comment / display text
#define SP_SAVE_CHUNK_SHOT           0x53484F54u  // "SHOT" - screenshot thumbnail (256x256 RGBA)
#define SP_SAVE_CHUNK_MPCM           0x4D50434Du  // "MPCM" - map name (null-padded to 1024)
#define SP_SAVE_CHUNK_GAME           0x47414D45u  // "GAME" - autosave flag (0=full, 1=auto)
#define SP_SAVE_CHUNK_CVCN           0x4356434Eu  // "CVCN" - archived cvar count
#define SP_SAVE_CHUNK_CVAR           0x43564152u  // "CVAR" - cvar name (null-terminated string)
#define SP_SAVE_CHUNK_VALU           0x56414C55u  // "VALU" - cvar value (null-terminated string)
#define SP_SAVE_CHUNK_TIME           0x54494D45u  // "TIME" - sv.time (level time in ms)
#define SP_SAVE_CHUNK_TIMR           0x54494D52u  // "TIMR" - svs.time (server real time in ms)
#define SP_SAVE_CHUNK_CSCN           0x4353434Eu  // "CSCN" - configstring count
#define SP_SAVE_CHUNK_CSIN           0x4353494Eu  // "CSIN" - configstring index
#define SP_SAVE_CHUNK_CSDA           0x43534441u  // "CSDA" - configstring data (string)
#define SP_SAVE_CHUNK_CVSV           0x43565356u  // "CVSV" - "playersave" cvar (autosave only)
#define SP_SAVE_CHUNK_AMMO           0x414D4D4Fu  // "AMMO" - "playerammoN" cvar (autosave only)
#define SP_SAVE_CHUNK_ADPT           0x41445054u  // "ADPT" - "borgadaptN" cvar (autosave only)
#define SP_SAVE_COMMENT_SIZE         128          // Total size of comment block in bytes
// The 128-byte comment block is split into two 64-byte halves:
// bytes [0..63]  = display text (shown in the save/load UI)
// bytes [64..127] = sort key (date + map, for chronological ordering)
#define SP_SAVE_SORTINFO_OFFSET      64
#define SP_SAVE_SHOT_SIZE            ( 256 * 256 * 4 )  // 256x256 pixels, 4 bytes/pixel (RGBA)
#define SP_SAVE_MAP_SIZE             1024                // Fixed-size map name field
#define SP_SAVE_CVAR_SIZE            1024                // Max cvar value length in save files

// State for an open save file stream (read or write).
// We maintain separate read and write streams so that a save-during-load
// (e.g., autosave on level transition) doesn't clobber the read state.
// The 'failed' flag is sticky -- once set, all subsequent operations on
// the stream are no-ops, allowing callers to chain writes without
// checking every return value (they check 'failed' at the end).
typedef struct {
	fileHandle_t	file;
	qboolean		active;
	qboolean		failed;
	char			qpath[MAX_QPATH];
} sv_sp_save_stream_t;

// Pointer to the sp_game_export_t returned by GetGameAPI.  NULL when no SP
// game module is loaded.  Also serves as the primary "SP mode active" flag
// (see SV_SP_IsActive).
static sp_game_export_t	*ge;

// OS-level handle to the loaded efgamex86.dll.  Kept open for the duration
// of the game so that the client side (cl_cgame_sp.c) can load cgame
// symbols from the same DLL image without reopening it.
static void		*gameLibrary;

// Set to qtrue once we've wired up sv.gentities / sv.gameClients.
// Cleared on shutdown and re-set lazily during the first frame, because
// ge->gentities isn't populated until partway through ge->Init (the game
// module allocates its entity array during initialization).
static qboolean entityDataLocated;

// ============================================================================
// Shadow entity array
//
// The ioEF engine's core systems (SV_LinkEntity, SV_BuildClientSnapshot,
// SV_Trace, SV_AreaEntities) all expect entities in sharedEntity_t layout.
// The SP game module uses sp_gentity_t, which embeds sp_entityState_t -- a
// struct with extra fields (modelindex3, legsAnimTimer, torsoAnimTimer,
// scale, pushVec) that shift all subsequent field offsets.
//
// We cannot simply cast sp_gentity_t* to sharedEntity_t* because the
// field offsets diverge after modelindex2.  A memcpy is also wrong because
// the structs are different sizes and have fields at different positions.
//
// Instead, we maintain a parallel "shadow" array of sharedEntity_t and
// copy fields one-by-one between the two representations:
//   SV_SP_SyncToShared  -- sp_gentity_t -> sharedEntity_t (before engine calls)
//   SV_SP_SyncFromShared -- sharedEntity_t -> sp_gentity_t (after engine calls)
//
// The engine reads/writes only the shadow array.  The SP game module
// reads/writes only its own array.  The sync functions bridge the gap.
// ============================================================================
#define SP_MAX_GENTITIES 1024
static sharedEntity_t sv_sp_entities[SP_MAX_GENTITIES];

static sv_sp_save_stream_t sv_sp_saveWrite;       // Active save-write stream
static sv_sp_save_stream_t sv_sp_saveRead;        // Active save-read stream
static SavedGameJustLoaded_e sv_sp_savedGameJustLoaded;  // Current load type for ClientConnect/Begin
static SavedGameJustLoaded_e sv_sp_pendingLoadType;      // Load type for a pending save restore
static char sv_sp_pendingLoadQPath[MAX_QPATH];    // Qpath of save file to load after map restart
static char sv_sp_pendingLoadMap[MAX_QPATH];      // Map name from pending save file

extern const byte *CL_SP_GetStoredSaveComment( void );
extern qboolean CL_SP_CopySaveScreenshot( byte *outRGBA, int outSize );
extern cvar_t *cvar_vars;
qboolean SV_SP_IsActive( void );

/*
===============
SV_SP_SyncToShared

Copies entity state from the SP game module's sp_gentity_t into the
engine's shadow sharedEntity_t array.  Called before any engine operation
that reads entity data (traces, linking, snapshot building, etc.).

Field-by-field copy is required because sp_entityState_t and entityState_t
diverge after modelindex2.  The SP struct inserts modelindex3 (used for
the third-person weapon model in SP), which shifts clientNum, frame, and
everything after.  Later in the struct, legsAnimTimer, torsoAnimTimer,
scale, and pushVec are SP-specific fields with no ioEF equivalent.

The entityShared_t portion (linked, svFlags, mins/maxs, etc.) also needs
manual translation because sp_gentity_t stores these as flat fields
rather than in a nested 'r' sub-struct.

The owner pointer is translated to an entity number (ownerNum) since the
engine works with entity numbers, not game-side pointers.
===============
*/
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
	// Fields from 'number' through 'modelindex2' happen to be at the same
	// offsets in both structs, but we copy them individually anyway for
	// clarity and safety (the compiler will optimize sequential stores).
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
	// modelindex3: SP-only field used for third-person weapon models.
	// ioEF entityState_t has no modelindex3, so this value is dropped.
	dst->clientNum       = src->clientNum;
	dst->frame           = src->frame;
	dst->solid           = src->solid;
	dst->event           = src->event;
	dst->eventParm       = src->eventParm;
	dst->powerups        = src->powerups;
	dst->weapon          = src->weapon;
	dst->legsAnim        = src->legsAnim;
	// legsAnimTimer: SP-only field for blending leg animations on the
	// server side.  ioEF handles anim timers entirely client-side.
	dst->torsoAnim       = src->torsoAnim;
	// torsoAnimTimer: Same as legsAnimTimer, but for torso.  Dropped.
	// scale: SP-only entity scale factor (ioEF doesn't support per-entity scale).
	// pushVec: SP-only push/knockback vector (ioEF uses a different mechanism).

	// Entity shared state (the "r" sub-struct in sharedEntity_t)
	// These fields control collision, PVS visibility, and area portal state.
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

/*
===============
SV_SP_SyncFromShared

Reverse of SV_SP_SyncToShared: copies data from the engine's shadow
sharedEntity_t back into the SP game module's sp_gentity_t.  Called
after engine operations that modify entity state (SV_LinkEntity,
SV_SetBrushModel, etc.).

Only common fields are copied back.  SP-specific fields (modelindex3,
legsAnimTimer, torsoAnimTimer, scale, pushVec) are deliberately NOT
overwritten -- the engine never touches them, and clobbering them would
destroy game state the SP module relies on.

Key fields that flow back from the engine:
  - linked, absmin, absmax: updated by SV_LinkEntity based on mins/maxs
    and the entity's position in the world BSP.
  - pos, apos (trajectory_t): the engine may modify these during movement.
  - solid: may be recalculated by SV_SetBrushModel.
===============
*/
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
	// Preserve modelindex3 -- SP-only, engine never modifies it
	dst->clientNum       = src->clientNum;
	dst->frame           = src->frame;
	dst->solid           = src->solid;
	dst->event           = src->event;
	dst->eventParm       = src->eventParm;
	dst->powerups        = src->powerups;
	dst->weapon          = src->weapon;
	dst->legsAnim        = src->legsAnim;
	// Preserve legsAnimTimer -- SP-only, engine never modifies it
	dst->torsoAnim       = src->torsoAnim;
	// Preserve torsoAnimTimer, scale, pushVec -- all SP-only

	sp_ent->linked = se->r.linked;
	VectorCopy( se->r.absmin, sp_ent->absmin );
	VectorCopy( se->r.absmax, sp_ent->absmax );
}

/*
===============
SV_SP_EnsureEntityData

Wires up sv.gentities, sv.gameClients, and sv.num_entities so the engine's
core systems (snapshot builder, collision) can find entity data.

This is called lazily rather than at init time because ge->gentities is
NULL when GetGameAPI returns -- the SP game module doesn't allocate its
entity array until partway through ge->Init().  The first call to any
engine function that touches entities (linkentity, trace, etc.) triggers
this setup.

sv.gentities is pointed at our shadow array (sv_sp_entities), NOT at the
SP game's own entity array, because the engine expects sharedEntity_t
layout.  sv.gentitySize is sizeof(sharedEntity_t) for the same reason.

sv.gameClients is pointed at our translated playerState_t (sv_sp_playerState).
sv.gameClientSize is 0 because SP only ever has one client (the player),
so stride between client structs is irrelevant.
===============
*/
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

/*
===============
SV_SP_SetupGameClient

Refreshes the translated playerState after ClientConnect or ClientBegin.
The SP game module may reallocate or reinitialize the client struct during
these calls, so we re-translate and update sv.gameClients to ensure the
engine's snapshot builder sees current data.

This is also the point where we confirm the client pointer is non-NULL.
Before ClientConnect, ent->client may be NULL (the game hasn't spawned the
player yet), which would cause crashes in SV_SP_SyncPlayerState.
===============
*/
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

/*
===============
SV_SP_SyncAllEntities

Bulk-syncs every active (inuse) entity from the SP game module's entity
array into the engine's shadow sharedEntity_t array, and also refreshes
the translated playerState.

Called twice per frame in GAME_RUN_FRAME:
  1. Before ge->RunFrame -- so that engine-side traces during the frame
     (called through gi.trace) see up-to-date entity positions.
  2. After ge->RunFrame -- so that the snapshot builder sees the final
     post-frame entity states when it runs later in the server frame.

Also updates sv.num_entities in case the game spawned or removed entities
during the frame.
===============
*/
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

// Reset the pending-load state.  Called after a load completes or fails.
static void SV_SP_ClearPendingLoad( void ) {
	sv_sp_pendingLoadType = eNO;
	sv_sp_pendingLoadQPath[0] = '\0';
	sv_sp_pendingLoadMap[0] = '\0';
}

// Close an open save stream (read or write) and reset all its fields.
static void SV_SP_CloseSaveStream( sv_sp_save_stream_t *stream ) {
	if ( stream->file ) {
		FS_FCloseFile( stream->file );
	}

	stream->file = 0;
	stream->active = qfalse;
	stream->failed = qfalse;
	stream->qpath[0] = '\0';
}

/*
===============
SV_SP_NormalizeSaveSlotName

Validates and normalizes a user-provided save slot name.  Strips the
".sav" extension if present, rejects empty names, directory traversal
attempts, path separators, and the reserved names "current" (used as
a temp file during writes) and "virtual" (reserved by the original EF1
save system).  Returns qtrue if the name is valid.
===============
*/
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

// Build the filesystem qpath for a save slot (e.g., "saves/quick.sav").
static void SV_SP_BuildSaveQPath( const char *baseName, char *outQPath, int outSize ) {
	Com_sprintf( outQPath, outSize, "saves/%s.sav", baseName );
}

// Read the current map name from the "mapname" cvar.
static void SV_SP_GetMapName( char *outMapName, int outSize ) {
	Q_strncpyz( outMapName, Cvar_VariableString( "mapname" ), outSize );
}

// Get a human-readable label for the current map.  Tries CS_MESSAGE first
// (the level's display name), falls back to the raw map name.
static void SV_SP_GetMapLabel( char *outMapLabel, int outSize ) {
	SV_GetConfigstring( CS_MESSAGE, outMapLabel, outSize );
	if ( !outMapLabel[0] ) {
		SV_SP_GetMapName( outMapLabel, outSize );
	}
	if ( !outMapLabel[0] ) {
		Q_strncpyz( outMapLabel, "unknown", outSize );
	}
}

/*
===============
SV_SP_BuildSaveComment

Builds the 128-byte comment block for a save file.  The comment has two
halves (each 64 bytes, see SP_SAVE_SORTINFO_OFFSET):
  - Display half: shown in the save/load menu.  For autosaves this is
    "-------> MapLabel"; for manual saves it's either a stored comment
    from the UI or a timestamp + map label.
  - Sort half: always "HH:MM MM/DD/YY MapLabel", used for chronological
    sorting in the save menu.
===============
*/
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

/*
===============
SV_SP_WriteSaveChunk

Writes a single chunk to the save stream.  Each chunk on disk is:
  [4] chunk ID    (little-endian)
  [4] data length (little-endian)
  [4] checksum    (little-endian, Com_BlockChecksum of data)
  [N] data bytes
  [4] magic sentinel (SP_SAVE_CHUNK_MAGIC, little-endian)

The trailing magic sentinel allows the reader to detect truncated or
corrupt files.  If any write fails, stream->failed is set and all
subsequent writes become no-ops (callers chain writes with && and
check at the end).

Returns qtrue on success, qfalse on failure.
===============
*/
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

// Read a chunk header (ID, length, checksum) from the save file without
// reading or validating the data or trailing magic.  Used by both
// SV_SP_ReadSaveChunk and SV_SP_ReadFromSaveGameOptional (which peeks
// at the next chunk ID to decide whether to consume it).
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

/*
===============
SV_SP_SkipSaveChunk

Reads a chunk header, verifies the chunk ID and (optionally) length match
expectations, then seeks past the data and validates the trailing magic
sentinel -- all without actually reading the chunk data into memory.

Used during load to skip over chunks the engine doesn't need to process
(e.g., the COMM comment and SHOT screenshot during a full load, since
those are only needed by the save/load UI).

Returns the data length on success, 0 on failure.
===============
*/
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

/*
===============
SV_SP_ReadSaveChunk

Reads a complete chunk from the save stream, validates it, and returns
the data.  The caller can provide data storage in two ways:

  1. Fixed buffer: pass 'buffer' (non-NULL) and 'expectedLength' > 0.
     The chunk's length must match expectedLength exactly.

  2. Dynamic allocation: pass buffer=NULL and outAllocatedBuffer (non-NULL).
     The function allocates a buffer of the chunk's actual length via
     Z_Malloc and returns it through *outAllocatedBuffer.  The caller
     is responsible for freeing it with Z_Free.

Validates chunk ID, length (if expectedLength > 0), trailing magic
sentinel, and data checksum.  Sets stream->failed on any error.

Returns the data length on success, 0 on failure.
===============
*/
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

/*
===============
SV_SP_WriteArchivedCvars

Saves all cvars with the CVAR_ARCHIVE flag to the save stream.  Written
as a CVCN chunk (count of cvars) followed by alternating CVAR/VALU chunk
pairs (name string, value string).  This preserves engine settings like
graphics quality, audio volume, key bindings, etc. across save/load.
===============
*/
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

// Skip past the archived cvar section in a save file without applying
// the values.  Used during the initial scan of a save file (SV_SP_LoadGame)
// to reach the GAME chunk and extract the map name, without actually
// restoring settings yet -- that happens later in SV_SP_LoadPendingSave.
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

// Read the archived cvar section and apply each name/value pair via
// Cvar_Set, restoring engine settings to their saved state.
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

/*
===============
SV_SP_WriteSpecialSaveCvars

Writes autosave-specific cvars to the save stream.  These are only
relevant for autosaves (level transitions), not for manual saves:

  CVSV chunk  - "playersave" cvar: serialized player inventory/state
                carried across level transitions.
  AMMO chunks - "playerammo0" through "playerammo3": ammo counts for
                each of the 4 ammo types, carried across levels.
  ADPT chunks - "borgadapt0" through "borgadapt31": Borg adaptation
                tracking for each weapon type (EF1-specific gameplay
                mechanic where Borg enemies become resistant to weapons
                used too frequently).

For non-autosaves, this function is a no-op (returns qtrue immediately).
===============
*/
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

// Restore autosave-specific cvars (playersave, playerammoN, borgadaptN)
// from the save stream.  Counterpart to SV_SP_WriteSpecialSaveCvars.
// No-op for non-autosave loads.
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

// Write sv.time (level time) and svs.time (server real time) as TIME
// and TIMR chunks.  Both are needed to restore the server's timing state
// so that entity animations, event timers, and level triggers resume
// from the correct point.
static qboolean SV_SP_WriteServerTimes( void ) {
	int fileTime;
	int fileServerTime;

	fileTime = LittleLong( sv.time );
	fileServerTime = LittleLong( svs.time );

	return SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_TIME, &fileTime, sizeof( fileTime ) ) &&
		SV_SP_WriteSaveChunk( &sv_sp_saveWrite, SP_SAVE_CHUNK_TIMR, &fileServerTime, sizeof( fileServerTime ) );
}

// Restore sv.time and svs.time from TIME/TIMR chunks in the save stream.
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

/*
===============
SV_SP_WriteConfigstrings

Writes all non-empty configstrings (except CS_SERVERINFO, which is
reconstructed at load time) to the save stream.  Written as:
  CSCN chunk - count of non-empty configstrings
  For each:
    CSIN chunk - configstring index (int)
    CSDA chunk - configstring data (null-terminated string)

Configstrings carry model names, sound names, player info, and other
per-level state that must be restored exactly for the client and game
module to function correctly after a load.
===============
*/
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

// Restore configstrings from CSCN/CSIN/CSDA chunks in the save stream.
// CS_SERVERINFO is excluded (it's rebuilt from current server state).
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

// Convert the GAME chunk's autosave flag to a SavedGameJustLoaded_e enum.
// The GAME chunk stores 0 for a full (manual) save, 1 for an autosave.
// We map these to eAUTO and eFULL respectively -- note the inversion:
// a "full save" triggers eFULL load semantics, an autosave triggers eAUTO.
static SavedGameJustLoaded_e SV_SP_GameChunkToLoadType( int gameChunkValue ) {
	return gameChunkValue ? eAUTO : eFULL;
}

/*
===============
SV_SP_LoadPendingSave

Performs the actual save file restoration after the map has been loaded.
This is the second phase of a two-phase load process:

Phase 1 (SV_SP_LoadGame): Opens the save file, reads just enough to
  extract the map name and save type, then issues an "spmap" command to
  load the correct map.  The save file is closed; the map name and qpath
  are stored in sv_sp_pendingLoad* variables.

Phase 2 (this function): Called from SV_SP_InitGameVM after ge->Init
  has run on the freshly loaded map.  Reopens the save file, skips the
  header chunks (COMM, SHOT, MPCM), restores archived cvars, special
  autosave cvars, server times, and configstrings, then calls
  ge->ReadLevel to restore the game module's internal state.

The two-phase approach is necessary because the game module must be
initialized on the correct map before its ReadLevel function can run --
it needs the BSP data, entity string, and initialized entity array.
===============
*/
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

// The game_import_t we populate and pass to GetGameAPI.  The SP game
// module stores a copy of this struct and calls through these function
// pointers for all engine services.
static sp_game_import_t	gi;

// Dummy array for the S_Override pointer in sp_game_import_t.
// The original EF1 engine exposed a sound override table here; ioEF
// doesn't support this feature, but the SP game module reads/writes
// the array during gameplay.  Providing a valid buffer prevents crashes.
static int		s_override_dummy[256];

// ============================================================================
// Wrapper functions for sp_game_import_t
// ============================================================================

// Printf wrapper: the SP game module calls gi.Printf, which routes to
// the engine's Com_Printf.  We use a local buffer to handle the varargs
// since the SP DLL's calling convention may differ from the engine's.
static void SV_SP_Printf( const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Printf( "%s", text );
}

// WriteCam: the SP game module can export camera path data to a file
// for cinematic editing tools.  Appends text to maps/<mapname>.camera.
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

// Error wrapper: the SP game module passes an error level (ERR_DROP,
// ERR_FATAL, etc.) directly; we forward to Com_Error.
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

// gi.AppendToSaveGame wrapper: delegates to our chunk writer.  The game
// module calls this during ge->WriteLevel to serialize its internal state
// (entity data, AI state, script variables, etc.) into the save stream.
// The chunk ID is chosen by the game module -- we just pass it through.
static qboolean SV_SP_AppendToSaveGame( unsigned long chid, void *data, int length ) {
	return SV_SP_WriteSaveChunk( &sv_sp_saveWrite, chid, data, length );
}

// gi.ReadFromSaveGame wrapper: delegates to our chunk reader.  The game
// module calls this during ge->ReadLevel to deserialize its saved state.
// Returns the data length on success, 0 on failure.
static int SV_SP_ReadFromSaveGame( unsigned long chid, void *pvAddress, int iLength, void **ppvAddressPtr ) {
	return SV_SP_ReadSaveChunk( &sv_sp_saveRead, chid, pvAddress, iLength, ppvAddressPtr, qfalse );
}

/*
===============
SV_SP_ReadFromSaveGameOptional

Like ReadFromSaveGame, but non-destructive if the next chunk doesn't
match the requested ID.  Peeks at the next chunk header; if the ID
matches, reads the chunk normally.  If it doesn't match, seeks back
to the original file position and returns 0 without setting the
failed flag.

This allows the game module to probe for optional chunks that may
or may not be present in older save files (forward compatibility).
===============
*/
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

/*
===============
SV_SP_SaveGame

Writes a complete save file for the given slot name.  The process:

1. Validate the slot name (reject empty, traversal, reserved names).
2. Determine if this is an autosave ("auto" or "exitholodeck" slot).
   Non-autosaves are gated by ge->GameAllowedToSaveHere.
3. Gather metadata: map name, comment text, screenshot thumbnail.
4. Write all engine-side chunks to a temp file ("saves/current.sav"):
   COMM, SHOT, MPCM, archived cvars, GAME flag, special autosave cvars,
   server times, configstrings.
5. Call ge->WriteLevel to let the game module append its own chunks
   (entity data, AI state, scripting state, etc.) to the same stream.
6. Close the stream.  If everything succeeded, rename current.sav to
   the final path (saves/<slotName>.sav).  If anything failed, delete
   the temp file.

The temp-file-then-rename approach prevents a failed save from
corrupting an existing save file for the same slot.
===============
*/
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

/*
===============
SV_SP_LoadGame

Initiates loading a save file.  This is phase 1 of the two-phase load
process (see SV_SP_LoadPendingSave for phase 2).

Phase 1:
1. Validate the slot name and open the save file.
2. Skip past the header chunks (COMM, SHOT) to reach MPCM (map name).
3. Skip past archived cvars to reach the GAME chunk (autosave flag).
4. Close the save file (it will be reopened in phase 2).
5. Store the map name, qpath, and load type in sv_sp_pending* variables.
6. Issue an "spmap <mapname>" command to load the correct map.

The engine will process the spmap command, which calls SV_SpawnServer,
which calls SV_SP_InitGameVM, which calls SV_SP_LoadPendingSave (phase 2)
to actually restore the game state after the map is loaded.

Returns qtrue if the save file was valid and the load was initiated.
===============
*/
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

// Delete a save file by slot name.  Used by the UI for "delete save" actions.
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

// SendServerCommand: translates the SP game module's clientNum convention
// (where -1 means "all clients") into the engine's client_t pointer
// convention (where NULL means "all clients").
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

// SetBrushModel: assigns an inline BSP model ("*N") to an entity, setting
// its bounds and solid type.  Must sync to/from the shadow array because
// SV_SetBrushModel reads and modifies sharedEntity_t fields.
static void SV_SP_SetBrushModel( sp_gentity_t *ent, const char *name ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( ent );
	SV_SetBrushModel( &sv_sp_entities[ent->s.number], name );
	SV_SP_SyncFromShared( ent );
}

// Trace wrapper: forwards to the engine's SV_Trace with capsule=qfalse.
// SP game module doesn't use capsule collision.
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

// AdjustAreaPortalState: must sync the entity to the shadow array first
// because the engine looks up the entity's BSP brush model from the
// sharedEntity_t to find which area portal to open/close.
static void SV_SP_AdjustAreaPortalState( sp_gentity_t *ent, qboolean open ) {
	SV_SP_EnsureEntityData();
	SV_SP_SyncToShared( ent );
	SV_AdjustAreaPortalState( &sv_sp_entities[ent->s.number], open );
}

static qboolean SV_SP_AreasConnected( int area1, int area2 ) {
	return CM_AreasConnected( area1, area2 );
}

// LinkEntity: registers the entity in the engine's spatial partition (the
// area/PVS system) so it can be found by traces and EntitiesInBox queries.
// Must sync before (so the engine sees current position/bounds) and after
// (so the game gets back the engine-computed absmin/absmax and linked flag).
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

/*
===============
SV_SP_EntitiesInBox

Spatial query: returns all entities whose bounding boxes intersect the
given AABB.  The engine's SV_AreaEntities returns entity numbers (indices
into the shadow array).  We must map these back to sp_gentity_t pointers
in the SP game module's entity array, since the game module expects to
receive its own entity pointers, not shadow array pointers.

The mapping uses: (byte*)ge->gentities + ge->gentitySize * entityNum
because the game module's entity array has a stride (gentitySize) that
includes private game data beyond the engine-visible sp_gentity_t header.
===============
*/
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

// EntityContact: tests if a bounding box touches an entity's collision
// model.  Must sync the entity to the shadow array so the engine can
// read its solid type and bounds.  Uses box collision (not capsule).
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

/*
===============
SV_SP_CgameSyscallStub

Stub syscall handler installed via dllEntry for the cgame side of the
SP DLL.

Background: The EF1 SP DLL is a combined game+cgame module.  It exports
both GetGameAPI (server-side) and dllEntry/vmMain (client-side).  The
cgame side stores a global function pointer (typically called "syscall")
that dllEntry initializes.  Before dllEntry is called, this pointer is
set to -1 (0xFFFFFFFF) by the DLL's static initialization.

The problem: Some functions in the DLL are shared between the game and
cgame sides (utility functions, entity lookup helpers, etc.).  If any
such function happens to call a cgame syscall during a server-side
ge->RunFrame, the call goes through the -1 pointer and crashes instantly
(SIGSEGV / access violation).

The fix: During server-side init, we call dllEntry with this stub handler.
It returns 0 for all syscall numbers, which is safe because:
  - Render calls (trap_R_*) return 0 = no-op
  - Sound calls (trap_S_*) return 0 = no-op
  - CG_* queries return 0 = harmless defaults
The server never actually needs cgame functionality; we just need the
syscall pointer to not be -1 so that accidental calls don't crash.
===============
*/
static intptr_t QDECL SV_SP_CgameSyscallStub( intptr_t arg, ... ) {
	return 0;
}

/*
===============
SV_SP_BindCgameStub / SV_SP_RestoreCgameSyscall

The SP DLL has a single dllEntry export shared between game and cgame.
During server-side execution (ClientThink, RunFrame), any cgame code
that runs inside the DLL must NOT use the live cgame syscall dispatcher
(VM_DllSyscall with currentVM=gvm), because that routes cgame trap
numbers through the game syscall handler, causing strncpy crashes on
bad pointers.

These functions swap the DLL's syscall pointer to/from the safe stub
around every game-side VM call.
===============
*/
static void (*sp_cgameDllEntry)( intptr_t (*syscallptr)( intptr_t, ... ) );
static intptr_t (*sp_savedCgameSyscall)( intptr_t, ... );

static void SV_SP_BindCgameStub( void ) {
	if ( sp_cgameDllEntry ) {
		sp_cgameDllEntry( SV_SP_CgameSyscallStub );
	}
}

static void SV_SP_RestoreCgameSyscall( void ) {
	if ( sp_cgameDllEntry && sp_savedCgameSyscall ) {
		sp_cgameDllEntry( sp_savedCgameSyscall );
	}
}

void SV_SP_SaveCgameSyscall( intptr_t (*syscall)( intptr_t, ... ) ) {
	sp_savedCgameSyscall = syscall;
}

void SV_SP_InitGameProgs( void ) {
	sp_game_export_t *(*GetGameAPI)( sp_game_import_t *import );
	char dllPath[MAX_OSPATH];
	const char *basepath;
	const char *gamedir;

	// Set up the import table
	SV_SP_InitGameImport();

	/*
	 * On map transitions, the DLL stays loaded (see ShutdownGameProgs).
	 * Only load it if this is the first time or if the engine fully shut down.
	 */
	if ( !gameLibrary ) {
		Com_Printf( "Loading SP game module: efgamex86.dll\n" );

		basepath = Cvar_VariableString( "fs_basepath" );
		gamedir = Cvar_VariableString( "fs_game" );

		if ( !gamedir[0] ) {
			gamedir = Cvar_VariableString( "com_basegame" );
		}

		Com_sprintf( dllPath, sizeof(dllPath), "%s/%s/efgamex86.dll", basepath, gamedir );
		Com_Printf( "Try loading dll file %s\n", dllPath );
		gameLibrary = Sys_LoadLibrary( dllPath );

		if ( !gameLibrary ) {
			gameLibrary = Sys_LoadLibrary( "efgamex86.dll" );
		}

		if ( !gameLibrary ) {
			Com_Error( ERR_FATAL, "SV_SP_InitGameProgs: failed to load efgamex86.dll\n"
					   "Searched: %s", dllPath );
		}
	} else {
		Com_Printf( "Reusing already-loaded SP game module\n" );
	}

	// Get the GetGameAPI entry point
	GetGameAPI = (sp_game_export_t *(*)(sp_game_import_t *))Sys_LoadFunction( gameLibrary, "GetGameAPI" );

	if ( !GetGameAPI ) {
		Sys_UnloadLibrary( gameLibrary );
		gameLibrary = NULL;
		Com_Error( ERR_FATAL, "SV_SP_InitGameProgs: efgamex86.dll missing GetGameAPI export" );
	}

	// CRITICAL: Initialize the cgame's syscall pointer before calling
	// GetGameAPI.  See SV_SP_CgameSyscallStub for detailed explanation.
	// Without this, the server will crash during the first RunFrame that
	// triggers any shared game/cgame code path.
	{
		sp_cgameDllEntry = (void (*)(intptr_t (*)(intptr_t, ...)))Sys_LoadFunction( gameLibrary, "dllEntry" );
		if ( sp_cgameDllEntry ) {
			sp_cgameDllEntry( SV_SP_CgameSyscallStub );
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

	/*
	 * Do NOT unload the DLL here.  efgamex86.dll contains both the
	 * server game (GetGameAPI) and the client cgame (dllEntry/vmMain).
	 * During a map transition the server shuts down first while the
	 * client's cgame may still be executing code from this DLL.
	 * Unloading it would crash the cgame with a SIGSEGV.
	 *
	 * The DLL stays loaded and is reused for the next map.
	 * SV_SP_InitGameProgs will re-call GetGameAPI on the same handle.
	 */

	entityDataLocated = qfalse;
	sv.gentities = NULL;
	sv.gameClients = NULL;
	sv.num_entities = 0;
	sv_sp_savedGameJustLoaded = eNO;

	Com_Printf( "SP game module shut down\n" );
}

/*
===============
SV_SP_UnloadDLL

Actually unloads the SP game DLL.  Called from SV_SpawnServer AFTER
both the cgame and server game have been shut down, so no code in the
DLL can be executing.  The DLL will be reloaded fresh by
SV_SP_InitGameProgs, which resets its global variables.
===============
*/
void SV_SP_UnloadDLL( void ) {
	if ( gameLibrary ) {
		Com_Printf( "SP: unloading game DLL for clean map transition\n" );
		Sys_UnloadLibrary( gameLibrary );
		gameLibrary = NULL;
	}
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

Translation layer between the Q3-style VM_Call interface and the SP
game module's function pointer interface.

The ioEF engine dispatches game commands via VM_Call(gvm, GAME_*, ...),
which expects a vmMain(command, ...) entry point.  The SP game module
has no vmMain -- it uses sp_game_export_t function pointers instead.
This function acts as a "virtual vmMain" that receives GAME_* commands
and routes them to the correct ge->* function pointer.

Key argument translations per command:

  GAME_INIT:
    Handled entirely by SV_SP_InitGameVM (which calls ge->Init with the
    correct SP-specific parameters).

  GAME_CLIENT_CONNECT:
    Q3 passes (clientNum, firstTime, isBot).  SP has no bots; the third
    argument is replaced with sv_sp_savedGameJustLoaded so the game knows
    whether to restore client state from a save.

  GAME_CLIENT_BEGIN:
    Q3 passes (clientNum) only.  SP additionally needs a usercmd_t* and
    SavedGameJustLoaded_e.  We supply the client's lastUsercmd and the
    current load type.  After ClientBegin, savedGameJustLoaded is reset
    to eNO so subsequent frames don't re-trigger load logic.

  GAME_CLIENT_THINK:
    Q3 copies the usercmd via syscall; SP takes a usercmd_t* directly.
    We pass a pointer to the client's lastUsercmd.

  GAME_RUN_FRAME:
    Syncs all entities before and after the frame.  The pre-sync ensures
    engine traces during RunFrame see current data.  The post-sync
    ensures the snapshot builder sees final post-frame state.

  GAME_CLIENT_USERINFO_CHANGED, GAME_CLIENT_DISCONNECT, GAME_CLIENT_COMMAND,
  GAME_CONSOLE_COMMAND:
    Straight pass-through with no argument translation needed.

  GAME_SHUTDOWN:
    Calls ge->Shutdown.  Note: full cleanup (DLL unload, pointer reset)
    is done by SV_SP_ShutdownGameProgs, not here.
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
		// Delegated to SV_SP_InitGameVM which calls ge->Init with the
		// full set of SP-specific parameters (map name, entity string,
		// timers, save-load state) that Q3's GAME_INIT doesn't provide.
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

	case GAME_CLIENT_BEGIN: {
		// Q3 passes: clientNum
		// SP expects: clientNum, sp_usercmd_t*, SavedGameJustLoaded_e
		// Translate usercmd: SP has int buttons, ioEF has byte buttons
		sp_usercmd_t spCmd;
		usercmd_t *cmd = &svs.clients[arg0].lastUsercmd;
		spCmd.serverTime = cmd->serverTime;
		spCmd.buttons = cmd->buttons;
		spCmd.weapon = cmd->weapon;
		spCmd.angles[0] = cmd->angles[0];
		spCmd.angles[1] = cmd->angles[1];
		spCmd.angles[2] = cmd->angles[2];
		spCmd.forwardmove = cmd->forwardmove;
		spCmd.rightmove = cmd->rightmove;
		spCmd.upmove = cmd->upmove;
		ge->ClientBegin( arg0, &spCmd, sv_sp_savedGameJustLoaded );
		sv_sp_savedGameJustLoaded = eNO;
		// Update client pointer in case it changed
		SV_SP_SetupGameClient( arg0 );
		return 0;
	}

	case GAME_CLIENT_USERINFO_CHANGED:
		ge->ClientUserinfoChanged( arg0 );
		return 0;

	case GAME_CLIENT_DISCONNECT:
		ge->ClientDisconnect( arg0 );
		return 0;

	case GAME_CLIENT_COMMAND:
		ge->ClientCommand( arg0 );
		return 0;

	case GAME_CLIENT_THINK: {
		// Q3 passes: clientNum
		// SP expects: clientNum, sp_usercmd_t*
		// Translate usercmd: SP has int buttons, ioEF has byte buttons
		sp_usercmd_t spCmd;
		usercmd_t *cmd = &svs.clients[arg0].lastUsercmd;
		spCmd.serverTime = cmd->serverTime;
		spCmd.buttons = cmd->buttons;
		spCmd.weapon = cmd->weapon;
		spCmd.angles[0] = cmd->angles[0];
		spCmd.angles[1] = cmd->angles[1];
		spCmd.angles[2] = cmd->angles[2];
		spCmd.forwardmove = cmd->forwardmove;
		spCmd.rightmove = cmd->rightmove;
		spCmd.upmove = cmd->upmove;
		SV_SP_BindCgameStub();
		ge->ClientThink( arg0, &spCmd );
		SV_SP_RestoreCgameSyscall();
		// Update entity count in case the game changed it
		sv.num_entities = ge->num_entities;
		return 0;
	}

	case GAME_RUN_FRAME:
		// Sync entities before the frame so traces during RunFrame see current data
		SV_SP_SyncAllEntities();
		SV_SP_BindCgameStub();
		ge->RunFrame( arg0 );
		SV_SP_RestoreCgameSyscall();
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

// Returns the game export table.  Used by other subsystems that need to
// query SP-specific game module state (e.g., gentitySize for entity
// iteration).
sp_game_export_t *SV_SP_GetGameExport( void ) {
	return ge;
}

/*
===============
SV_SP_GetRawPlayerState

Returns a pointer to the SP game module's raw sp_playerState_t (NOT the
translated ioEF playerState_t).  Used by cl_cgame_sp.c when building
SP-format snapshots for the cgame -- the cgame expects sp_playerState_t
layout, not the ioEF layout.  This bypasses the translation layer.
===============
*/
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

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

#ifdef BUILD_VR
#include "../vr/VrBase.h"
#endif

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

static void CL_SP_SetFallbackLighting( vec_t *ambientLight, vec_t *directedLight, vec_t *lightDir ) {
	VectorSet( ambientLight, 255.0f, 255.0f, 255.0f );
	VectorSet( directedLight, 255.0f, 255.0f, 255.0f );
	VectorSet( lightDir, 0.0f, 0.0f, 1.0f );
}

static intptr_t CL_SP_GetSampleLengthMilliseconds( sfxHandle_t sfxHandle ) {
	// Use the active sound backend's duration query.  The old code read the
	// dma backend's s_knownSfx[] directly, which is empty when OpenAL is the
	// active backend (the default) -- so this returned 0 and caption/voice
	// timing was wrong under OpenAL.  S_SoundDuration dispatches to whichever
	// backend is running.
	return (intptr_t)S_SoundDuration( sfxHandle );
}

/*
 * Voice-sound completion tracking for ICARUS (gi.S_Override).
 *
 * The SP game's G_CheckTasksCompleted holds a TID_CHAN_VOICE task until
 * gi.S_Override[ent] reads false, i.e. until the entity's scripted voice clip
 * finishes -- that is how the borg1 intro waits for each Janeway captain's-log
 * line before advancing (billboards, fade, the cut to the 3D scene).  The
 * engine's S_Override array was a never-updated dummy (always 0), so every
 * voice line "completed" on the next frame and ICARUS raced through the whole
 * VO-gated sequence (the ~62s intro collapsed to ~36s).
 *
 * Record each voice clip's wall-clock end time here when it starts; the server
 * bridge (SV_SP_UpdateVoiceOverride) reflects "still playing" into
 * gi.S_Override each game frame.
 */
static int s_spVoiceEndTime[ MAX_GENTITIES ];

void CL_SP_ClearVoiceTracking( void ) {
	Com_Memset( s_spVoiceEndTime, 0, sizeof( s_spVoiceEndTime ) );
}

qboolean CL_SP_IsVoicePlaying( int entnum ) {
	if ( entnum < 0 || entnum >= MAX_GENTITIES ) {
		return qfalse;
	}
	return ( Sys_Milliseconds() < s_spVoiceEndTime[ entnum ] ) ? qtrue : qfalse;
}

// Note that a voice clip has begun on an entity so ICARUS can wait for it.
// CHAN_VOICE (3) and CHAN_VOICE_ATTEN (4) are the SP voice channels.
static void CL_SP_NoteVoiceSound( int entnum, int channel, sfxHandle_t sfx ) {
	int ms;
	if ( entnum < 0 || entnum >= MAX_GENTITIES ) {
		return;
	}
	if ( channel != 3 && channel != 4 ) {
		return;
	}
	ms = S_SoundDuration( sfx );
	if ( ms > 0 ) {
		s_spVoiceEndTime[ entnum ] = Sys_Milliseconds() + ms;
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
   environments to BSP brush models and world regions.  It was used for
   environmental audio in the SP campaign (humming warp cores, creaking
   hull sounds, ambient alien atmospheres).  ioEF does not implement this
   subsystem, so all six calls are stubbed out. */
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
 *           NOTE: read the float from the low bytes of the x-th pointer-width
 *           (intptr_t) arg slot.  The old `((float *)args)[x]` form indexed at a
 *           4-byte stride, which only matches the 8-byte arg stride on 32-bit; on
 *           64-bit it read every float from the wrong (half-shifted) offset,
 *           scrambling all float syscall args (e.g. text/2D draw coords).
 */
#undef VMA
#undef VMF
#define	VMA(x) VM_ArgPtr(args[x])
#define	VMF(x) ( *(float *)&args[x] )

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
	case SPCG_CVAR_SET:
		Cvar_SetSafe( VMA(1), VMA(2) );
		return 0;

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
		SCR_UpdateScreen();
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
		S_StartSound( VMA(1), args[2], args[3], args[4] );
		// Track voice clips so ICARUS can wait for them to finish (gi.S_Override).
		CL_SP_NoteVoiceSound( args[2], args[3], args[4] );
		return 0;
	case SPCG_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;
	case SPCG_S_CLEARLOOPINGSOUNDS:
		S_ClearLoopingSounds( args[1] );
		return 0;
	case SPCG_S_ADDLOOPINGSOUND:
		S_AddLoopingSound( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case SPCG_S_UPDATEENTITYPOSITION:
		S_UpdateEntityPosition( args[1], VMA(2) );
		return 0;
	case SPCG_S_RESPATIALIZE:
		S_Respatialize( args[1], VMA(2), VMA(3), args[4] );
		return 0;
	case SPCG_S_REGISTERSOUND:
		return S_RegisterSound( VMA(1), qfalse );
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
		return 0;
	case SPCG_FF_ENSUREFX:
		return 0;
	case SPCG_FF_STOPFX:
		return 0;
	case SPCG_FF_STOPALLFX:
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
	case SPCG_R_GETLIGHTING:
		/*
		 * The SP cgame reads the output vectors unconditionally and uses the
		 * directed light intensity for gameplay (stealth visibility). Provide
		 * deterministic output even when the renderer has no light-grid data.
		 */
		if ( !re.LightForPoint( VMA(1), VMA(2), VMA(3), VMA(4) ) ) {
			CL_SP_SetFallbackLighting( VMA(2), VMA(3), VMA(4) );
		}
		return 0;
	case SPCG_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), 1 );
		return 0;
	case SPCG_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case SPCG_R_RENDERSCENE: {
#ifdef BUILD_VR
		// Stereo: shift the view origin laterally for the eye currently being
		// rendered (vr.eye) so each eye sees from its own pupil -> IPD parallax.
		// Skipped for the flat virtual-screen layer.  viewaxis[1] is the view
		// LEFT axis, so this is correct under head roll too.
		if ( VR_IsActive() && !VR_UseScreenLayer() ) {
			refdef_t *fd = (refdef_t *)VMA(1);
			float sep = VR_GetEyeStereoSeparation( vr.eye );
			VectorMA( fd->vieworg, sep, fd->viewaxis[1], fd->vieworg );
		}
#endif
		re.RenderScene( VMA(1) );
		return 0;
	}
	case SPCG_R_SETCOLOR:
		re.SetColor( VMA(1) );
		return 0;
	case SPCG_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;
	case SPCG_R_DRAWSCREENSHOT:
		// SP-only renderer call: R_DrawScreenShot.  The original engine could
		// capture a screenshot into a texture and then draw it as a 2D element.
		// The SP cgame uses this to preserve the current backbuffer during
		// transitions. ioEF has no equivalent hook, so we intentionally leave
		// the existing framebuffer contents untouched.
		return 0;
	case SPCG_R_MODELBOUNDS:
		re.ModelBounds( args[1], VMA(2), VMA(3) );
		return 0;
	case SPCG_R_LERPTAG:
		return re.LerpTag( VMA(1), args[2], args[3], args[4], VMF(5), VMA(6) );
	case SPCG_R_DRAWROTATEPIC:
		/* Preserve the art even when we cannot honor the rotation angle. */
		re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[10] );
		return 0;
	case SPCG_R_SCISSOR:
		// SP-only renderer call: R_Scissor.  Sets a rectangular scissor/clip
		// region for subsequent 2D drawing.  The SP cgame used this to clip
		// HUD elements to specific screen regions (e.g., the text crawl in
		// mission briefings, scrolling objective lists). ioEF's renderer has
		// no exported 2D scissor hook, so this remains a no-op.
		return 0;

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
		qboolean result = CL_GetSnapshot( args[1], &tempSnap );
		if ( !result ) return qfalse;

		/* Build the SP-format snapshot in the buffer the cgame provided (arg 2) */
		sp_snapshot_t *spSnap = (sp_snapshot_t *)VMA(2);
		int i;

		/* Copy metadata from the ioEF snapshot -- these fields have the same
		   meaning and format in both layouts */
		spSnap->snapFlags = tempSnap.snapFlags;
		spSnap->ping = tempSnap.ping;
		spSnap->serverTime = tempSnap.serverTime;
		Com_Memcpy( spSnap->areamask, tempSnap.areamask, sizeof( spSnap->areamask ) );

		/* SP-specific fields: cmdNum and configstring tracking.
		   We set these to 0 because the ioEF engine doesn't track them.
		   The SP cgame appears to tolerate zero values here -- it falls back
		   to other mechanisms for command prediction and configstring updates. */
		spSnap->cmdNum = 0;
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
	case SPCG_GETUSERCMD: {
		/*
		 * Translate the engine usercmd_t to the SP cgame's sp_usercmd_t.
		 *
		 * CL_GetUserCmd() copies the ENGINE usercmd_t (byte buttons), but the
		 * SP cgame reads its buffer as sp_usercmd_t (int buttons).  The
		 * differing buttons field size shifts every field after it by 3-4
		 * bytes, so the cgame's client-side movement prediction reads angles[]
		 * one int off -- yaw lands in pitch, pitch lands in roll, roll picks up
		 * forwardmove.  Result: mouse left/right tilts the view up/down, mouse
		 * up/down does nothing, and moving disturbs the camera, even though the
		 * server (which translates correctly in GAME_CLIENT_THINK) moves the
		 * player fine.  Translate field-by-field, mirroring the server side.
		 */
		usercmd_t engineCmd;
		sp_usercmd_t *spCmd = (sp_usercmd_t *)VMA(2);

		if ( !CL_GetUserCmd( args[1], &engineCmd ) ) {
			return qfalse;
		}

		spCmd->serverTime  = engineCmd.serverTime;
		spCmd->buttons     = engineCmd.buttons;
		spCmd->weapon      = engineCmd.weapon;
		spCmd->angles[0]   = engineCmd.angles[0];
		spCmd->angles[1]   = engineCmd.angles[1];
		spCmd->angles[2]   = engineCmd.angles[2];
		spCmd->forwardmove = engineCmd.forwardmove;
		spCmd->rightmove   = engineCmd.rightmove;
		spCmd->upmove      = engineCmd.upmove;
		return qtrue;
	}
	case SPCG_SETUSERCMDVALUE:
		CL_SetUserCmdValue( args[1], VMF(2) );
		return 0;
	case SPCG_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();

	// --- EF1 SP ambient sound stubs ---
	// The Ritual engine's ambient sound system was a layer above the normal
	// Quake 3 sound system.  It managed environmental audio "sets" -- groups
	// of ambient sounds associated with BSP regions or brush models.  As the
	// player moved through the level, the system would cross-fade between
	// ambient sets to create seamless environmental audio transitions (e.g.,
	// moving from a quiet corridor into a humming engine room).
	//
	// ioEF does not implement this subsystem.  The SP campaign still plays
	// its triggered and looping sounds normally via the standard S_StartSound
	// and S_AddLoopingSound calls; only the positional ambient layer is
	// missing, which results in slightly less atmospheric audio but no
	// gameplay impact.
	case SPCG_S_UPDATEAMBIENTSET:	/* refresh which ambient set is active for the player's position */
		return 0;
	case SPCG_S_ADDLOCALSET:		/* register a local ambient set at a specific world position */
		return args[5];
	case SPCG_AS_PARSESETS:			/* parse ambient set definitions from a text file/buffer */
		return 0;
	case SPCG_AS_ADDENTRY:			/* add a sound entry to an ambient set */
		return 0;
	case SPCG_AS_GETBMODELSOUND:	/* query which ambient sound a brush model should emit */
		return -1;
	case SPCG_S_GETSAMPLELENGTH:	/* query duration of a sound sample in milliseconds */
		return CL_SP_GetSampleLengthMilliseconds( args[1] );

	default:
		Com_Error( ERR_DROP, "Bad SP cgame system trap: %ld", (long int) args[0] );
	}
	return 0;
}

#endif /* ELITEFORCE */

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
// cl_cgame_sp.c -- syscall dispatcher for the EF1 singleplayer cgame module

#ifdef ELITEFORCE

#include "client.h"
#include "../qcommon/sp_types.h"

// From sv_game_sp.c — available because SP is always a local server
extern void *SV_SP_GetRawPlayerState( void );
extern sp_entityState_t *SV_SP_GetRawEntityState( int entNum );

/*
 * EF1 SP cgame syscall numbers.
 * These differ from the ioEF (multiplayer) cgame numbering because the SP
 * module includes force-feedback, ambient-sound, and extra renderer calls
 * that shift many of the later values.
 */
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
#define SPCG_CM_LOADMAP                   17
#define SPCG_CM_NUMINLINEMODELS           18
#define SPCG_CM_INLINEMODEL               19
#define SPCG_CM_TEMPBOXMODEL              20
#define SPCG_CM_POINTCONTENTS             21
#define SPCG_CM_TRANSFORMEDPOINTCONTENTS  22
#define SPCG_CM_BOXTRACE                  23
#define SPCG_CM_TRANSFORMEDBOXTRACE       24
#define SPCG_CM_MARKFRAGMENTS             25
#define SPCG_S_STARTSOUND                 26
#define SPCG_S_STARTLOCALSOUND            27
#define SPCG_S_CLEARLOOPINGSOUNDS         28
#define SPCG_S_ADDLOOPINGSOUND            29
#define SPCG_S_UPDATEENTITYPOSITION       30
#define SPCG_S_RESPATIALIZE               31
#define SPCG_S_REGISTERSOUND              32
#define SPCG_S_STARTBACKGROUNDTRACK       33
#define SPCG_FF_STARTFX                   34
#define SPCG_FF_ENSUREFX                  35
#define SPCG_FF_STOPFX                    36
#define SPCG_FF_STOPALLFX                 37
#define SPCG_R_LOADWORLDMAP               38
#define SPCG_R_REGISTERMODEL              39
#define SPCG_R_REGISTERSKIN               40
#define SPCG_R_REGISTERSHADER             41
#define SPCG_R_REGISTERSHADERNOMIP        42
#define SPCG_R_CLEARSCENE                 43
#define SPCG_R_ADDREFENTITYTOSCENE        44
#define SPCG_R_GETLIGHTING                45
#define SPCG_R_ADDPOLYTOSCENE             46
#define SPCG_R_ADDLIGHTTOSCENE            47
#define SPCG_R_RENDERSCENE                48
#define SPCG_R_SETCOLOR                   49
#define SPCG_R_DRAWSTRETCHPIC             50
#define SPCG_R_DRAWSCREENSHOT             51
#define SPCG_R_MODELBOUNDS                52
#define SPCG_R_LERPTAG                    53
#define SPCG_R_DRAWROTATEPIC              54
#define SPCG_R_SCISSOR                    55
#define SPCG_GETGLCONFIG                  56
#define SPCG_GETGAMESTATE                 57
#define SPCG_GETCURRENTSNAPSHOTNUMBER     58
#define SPCG_GETSNAPSHOT                  59
#define SPCG_GETSERVERCOMMAND             60
#define SPCG_GETCURRENTCMDNUMBER          61
#define SPCG_GETUSERCMD                   62
#define SPCG_SETUSERCMDVALUE              63
#define SPCG_MEMORY_REMAINING             64
#define SPCG_S_UPDATEAMBIENTSET           65
#define SPCG_S_ADDLOCALSET                66
#define SPCG_AS_PARSESETS                 67
#define SPCG_AS_ADDENTRY                  68
#define SPCG_AS_GETBMODELSOUND            69
#define SPCG_S_GETSAMPLELENGTH            70

#undef VMA
#undef VMF
#define	VMA(x) VM_ArgPtr(args[x])
#define	VMF(x) ((float *)args)[x]

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
		return S_RegisterSound( VMA(1), args[2] );
	case SPCG_S_STARTBACKGROUNDTRACK:
		if ( !VMA(1) || !*((char *) VMA(1)) )
			S_StopBackgroundTrack();
		else
			S_StartBackgroundTrack( VMA(1), VMA(2) );
		return 0;

	// --- force feedback (stubs) ---
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
		// EF1 SP-only call; not supported in ioEF
		return 0;
	case SPCG_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), 1 );
		return 0;
	case SPCG_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case SPCG_R_RENDERSCENE: {
		static int rsCount = 0;
		if ( rsCount < 3 ) {
			refdef_t *rd = (refdef_t *)VMA(1);
			Com_Printf( "RenderScene: viewport(%d %d %d %d) fov(%.1f %.1f) org(%.1f %.1f %.1f) flags=%d\n",
				rd->x, rd->y, rd->width, rd->height, rd->fov_x, rd->fov_y,
				rd->vieworg[0], rd->vieworg[1], rd->vieworg[2], rd->rdflags );
			rsCount++;
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
	case SPCG_R_DRAWSCREENSHOT:
		// EF1 SP-only call; no-op stub
		return 0;
	case SPCG_R_MODELBOUNDS:
		re.ModelBounds( args[1], VMA(2), VMA(3) );
		return 0;
	case SPCG_R_LERPTAG:
		return re.LerpTag( VMA(1), args[2], args[3], args[4], VMF(5), VMA(6) );
	case SPCG_R_DRAWROTATEPIC:
		// EF1 SP-only call; no-op stub
		return 0;
	case SPCG_R_SCISSOR:
		// EF1 SP-only call; no-op stub
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
		// The SP cgame's snapshot_t has a different layout from ioEF's:
		//   - Extra cmdNum field before ps
		//   - sp_playerState_t (different size/layout from ioEF playerState_t)
		//   - sp_entityState_t (different size/layout from ioEF entityState_t)
		//
		// Since this is a local server, we bypass the engine's snapshot
		// delta compression entirely and build the SP snapshot directly
		// from the game module's live data.

		// Step 1: Get the ioEF snapshot into a temp buffer for metadata + entity list
		static snapshot_t tempSnap;
		qboolean result = CL_GetSnapshot( args[1], &tempSnap );
		if ( !result ) return qfalse;

		// Step 2: Fill the SP snapshot at the cgame's buffer
		sp_snapshot_t *spSnap = (sp_snapshot_t *)VMA(2);
		int i;

		spSnap->snapFlags = tempSnap.snapFlags;
		spSnap->ping = tempSnap.ping;
		spSnap->serverTime = tempSnap.serverTime;
		Com_Memcpy( spSnap->areamask, tempSnap.areamask, sizeof( spSnap->areamask ) );
		spSnap->cmdNum = 0;
		spSnap->serverCommandSequence = tempSnap.serverCommandSequence;
		spSnap->numServerCommands = 0;
		spSnap->numConfigstringChanges = 0;
		spSnap->configstringNum = 0;

		// PlayerState: copy raw SP data directly from the game module
		{
			static int dbgCount = 0;
			void *rawPS = SV_SP_GetRawPlayerState();
			if ( rawPS ) {
				Com_Memcpy( &spSnap->ps, rawPS, sizeof( sp_playerState_t ) );
				if ( dbgCount < 3 ) {
					Com_Printf( "SP snap: origin(%.1f %.1f %.1f) angles(%.1f %.1f %.1f) vh=%d ents=%d sTime=%d\n",
						spSnap->ps.origin[0], spSnap->ps.origin[1], spSnap->ps.origin[2],
						spSnap->ps.viewangles[0], spSnap->ps.viewangles[1], spSnap->ps.viewangles[2],
						spSnap->ps.viewheight, tempSnap.numEntities, spSnap->serverTime );
					dbgCount++;
				}
			} else {
				Com_Memset( &spSnap->ps, 0, sizeof( sp_playerState_t ) );
			}
		}

		// Entities: get raw SP entity data for each entity in the snapshot
		spSnap->numEntities = tempSnap.numEntities;
		for ( i = 0; i < tempSnap.numEntities && i < SP_MAX_ENTITIES_IN_SNAPSHOT; i++ ) {
			int entNum = tempSnap.entities[i].number;
			sp_entityState_t *rawEnt = SV_SP_GetRawEntityState( entNum );
			if ( rawEnt ) {
				Com_Memcpy( &spSnap->entities[i], rawEnt, sizeof( sp_entityState_t ) );
			} else {
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
		return CL_GetUserCmd( args[1], VMA(2) );
	case SPCG_SETUSERCMDVALUE:
		CL_SetUserCmdValue( args[1], VMF(2) );
		return 0;
	case SPCG_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();

	// --- EF1 SP ambient sound stubs ---
	case SPCG_S_UPDATEAMBIENTSET:
		return 0;
	case SPCG_S_ADDLOCALSET:
		return 0;
	case SPCG_AS_PARSESETS:
		return 0;
	case SPCG_AS_ADDENTRY:
		return 0;
	case SPCG_AS_GETBMODELSOUND:
		return 0;
	case SPCG_S_GETSAMPLELENGTH:
		return 0;

	default:
		Com_Error( ERR_DROP, "Bad SP cgame system trap: %ld", (long int) args[0] );
	}
	return 0;
}

#endif /* ELITEFORCE */

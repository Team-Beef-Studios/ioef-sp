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
 * cl_ui_sp.c -- Singleplayer UI bridge for Elite Force 1
 *
 * OVERVIEW
 * --------
 * The ioEF engine communicates with UI modules through a Quake 3-style
 * virtual machine (VM) interface: the engine calls vmMain(command, ...)
 * and the module calls back via numbered syscalls.  The EF1 singleplayer
 * UI module (efuix86.dll), however, uses a Quake 2-style "GetUIAPI"
 * pattern -- it exports a single C function that returns a struct of
 * function pointers, and the engine passes a matching struct of engine
 * function pointers in the other direction.
 *
 * This file bridges between those two worlds:
 *
 *   1. It defines the two function pointer tables:
 *        - sp_uiimport_t  (engine -> UI module, "imports")
 *        - sp_uiexport_t  (UI module -> engine, "exports")
 *
 *   2. It provides thin wrapper functions for every import slot, adapting
 *      ioEF engine APIs to the signatures the SP UI module expects.
 *
 *   3. It implements CL_SP_UIVmMain(), a fake vmMain dispatcher that the
 *      engine's existing VM_Call(uivm, UI_*, ...) call sites invoke.
 *      CL_SP_UIVmMain translates each UI_* command enum into the
 *      corresponding sp_uiexport_t function pointer call.
 *
 *   4. It implements the SG_* (SaveGame) family of callbacks that the SP
 *      UI module expects the engine to provide for load/save screen
 *      thumbnails and save-slot metadata.
 *
 * LOADING SEQUENCE
 * ----------------
 *   CL_SP_InitUI()
 *     -> Sys_LoadLibrary("efuix86.dll")
 *     -> Sys_LoadFunction("GetUIAPI")   -- locate the DLL's entry point
 *     -> GetUIAPI()                     -- receive sp_uiexport_t* back
 *     (UI_Init is NOT called here; it happens later when the engine
 *      issues VM_Call(uivm, UI_INIT), which routes through
 *      CL_SP_UIVmMain -> ue->UI_Init.)
 *
 * RELATIONSHIP TO OTHER SP BRIDGE FILES
 * --------------------------------------
 *   sv_game_sp.c   -- Server-side bridge, loads efgamex86.dll via GetGameAPI
 *   cl_cgame_sp.c  -- Client cgame bridge, shares the same DLL as sv_game_sp
 *   cl_ui_sp.c     -- This file, loads a SEPARATE DLL (efuix86.dll) for menus
 *   sp_types.h     -- Shared SP struct definitions used by all three bridges
 */

#ifdef ELITEFORCE

#include "client.h"
#include "../sys/sys_loadlib.h"

// ============================================================================
// SP UI module type definitions (from EF1 SP ui_public.h)
// ============================================================================

// Protocol version expected by the SP UI module.  If the DLL was compiled
// against a different version, UI_Init will fail or misbehave.  The original
// Ritual release used version 2; we match that here.
#define SP_UI_API_VERSION	2

/*
 * sp_uiimport_t -- "engine imports" table passed TO the SP UI module.
 *
 * The engine fills every slot with a wrapper function that adapts the ioEF
 * internal API to the calling convention the SP UI DLL expects.  This struct
 * is handed to the DLL inside UI_Init(apiVersion, uiimport).  After that
 * call, the DLL caches the pointer and calls back into the engine through
 * these function pointers for the rest of its lifetime.
 *
 * Layout must match the original Ritual ui_public.h exactly -- the DLL was
 * compiled against that header, and function pointers are resolved purely
 * by their position in the struct (there is no name-based lookup).
 *
 * The functions fall into several groups:
 *   - Console / Cvar access        (Printf through Cvar_InfoStringBuffer)
 *   - Command buffer               (Argc, Argv, Cmd_ExecuteText, ...)
 *   - Filesystem                   (FS_FOpenFile through FS_FreeFile)
 *   - Renderer                     (R_RegisterModel through R_ScissorPic)
 *   - Screen / screenshot helpers  (UpdateScreen, PrecacheScreenshot)
 *   - Audio                        (S_StartLocalSound, S_RegisterSound, ...)
 *   - Raw video drawing            (DrawStretchRaw -- for cinematic playback)
 *   - Save-game support (SG_*)     (thumbnail, comment, and validation helpers)
 *   - Key / input                  (Key_KeynumToStringBuf through Key_SetCatcher)
 *   - Misc client state            (GetClipboardData, GetGlconfig, etc.)
 */
typedef struct {
	// --- Console / Cvar ---
	void		(*Printf)( const char *fmt, ... );
	void		(*Error)( int level, const char *fmt, ... );
	void		(*Cvar_Set)( const char *name, const char *value );
	float		(*Cvar_VariableValue)( const char *var_name );
	void		(*Cvar_VariableStringBuffer)( const char *var_name, char *buffer, int bufsize );
	void		(*Cvar_SetValue)( const char *var_name, float value );
	void		(*Cvar_Reset)( const char *name );
	void		(*Cvar_Create)( const char *var_name, const char *var_value, int flags );
	void		(*Cvar_InfoStringBuffer)( int bit, char *buffer, int bufsize );

	// --- Command buffer ---
	int			(*Argc)( void );
	void		(*Argv)( int n, char *buffer, int bufferLength );
	void		(*Cmd_ExecuteText)( int exec_when, const char *text );
	void		(*Cmd_TokenizeString)( const char *text );

	// --- Filesystem ---
	int			(*FS_FOpenFile)( const char *qpath, fileHandle_t *file, fsMode_t mode );
	int			(*FS_Read)( void *buffer, int len, fileHandle_t f );
	int			(*FS_Write)( const void *buffer, int len, fileHandle_t f );
	void		(*FS_FCloseFile)( fileHandle_t f );
	int			(*FS_GetFileList)( const char *path, const char *extension, char *listbuf, int bufsize );
	int			(*FS_ReadFile)( const char *name, void **buf );
	void		(*FS_FreeFile)( void *buf );

	// --- Renderer ---
	qhandle_t	(*R_RegisterModel)( const char *name );
	qhandle_t	(*R_RegisterSkin)( const char *name );
	qhandle_t	(*R_RegisterShader)( const char *name );
	qhandle_t	(*R_RegisterShaderNoMip)( const char *name );
	void		(*R_ClearScene)( void );
	void		(*R_AddRefEntityToScene)( const refEntity_t *re );
	void		(*R_AddPolyToScene)( qhandle_t hShader, int numVerts, const polyVert_t *verts );
	void		(*R_AddLightToScene)( const vec3_t org, float intensity, float r, float g, float b );
	void		(*R_RenderScene)( const refdef_t *fd );
	void		(*R_SetColor)( const float *rgba );
	void		(*R_DrawStretchPic)( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );
	void		(*R_ScissorPic)( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );

	// --- Screen helpers ---
	void		(*UpdateScreen)( void );
	void		(*PrecacheScreenshot)( void );
	void		(*R_LerpTag)( orientation_t *tag, clipHandle_t mod, int startFrame, int endFrame, float frac, const char *tagName );

	// --- Audio ---
	void		(*S_StartLocalSound)( sfxHandle_t sfxHandle, int channelNum );
	sfxHandle_t	(*S_RegisterSound)( const char *name );
	void		(*S_StartLocalLoopingSound)( sfxHandle_t sfxHandle );

	// --- Raw video / cinematic ---
	void		(*DrawStretchRaw)( int x, int y, int w, int h, int cols, int rows, const byte *data, float fLightValue );

	// --- Save-game support (SG_*) ---
	// These callbacks let the SP UI's load/save screen retrieve thumbnails,
	// comment strings, and validity flags for each save slot.  The original
	// EF1 engine implemented a full save-game system (chunked binary files
	// with COMM and SHOT blocks); our implementations here read those chunks
	// from the save files stored in saves/*.sav within baseEF/.
	qboolean	(*SG_GetSaveImage)( const char *psPathlessBaseName, void *pvAddress );
	void *		(*SG_GetSaveGameComment)( const char *psPathlessBaseName );
	qboolean	(*SG_ValidateForLoadSaveScreen)( const char *psPathlessBaseName );
	qboolean	(*SG_GameAllowedToSaveHere)( qboolean inCamera );
	void		(*SG_StoreSaveGameComment)( const char *sComment );
	byte *		(*SCR_GetScreenshot)( qboolean * );

	// --- Key / input ---
	void		(*Key_KeynumToStringBuf)( int keynum, char *buf, int buflen );
	void		(*Key_GetBindingBuf)( int keynum, char *buf, int buflen );
	void		(*Key_SetBinding)( int keynum, const char *binding );
	qboolean	(*Key_IsDown)( int keynum );
	qboolean	(*Key_GetOverstrikeMode)( void );
	void		(*Key_SetOverstrikeMode)( qboolean state );
	void		(*Key_ClearStates)( void );
	int			(*Key_GetCatcher)( void );
	void		(*Key_SetCatcher)( int catcher );

	// --- Misc client state ---
	void		(*GetClipboardData)( char *buf, int bufsize );
	void		(*GetGlconfig)( glconfig_t *config );
	connstate_t	(*GetClientState)( void );
	void		(*GetConfigString)( int index, char *buff, int buffsize );
	int			(*Milliseconds)( void );
} sp_uiimport_t;

/*
 * sp_uiexport_t -- "UI module exports" table returned BY the SP UI module.
 *
 * The DLL's GetUIAPI() entry point returns a pointer to a static instance
 * of this struct.  Each slot is a function implemented inside the DLL that
 * the engine can call.  CL_SP_UIVmMain dispatches to these pointers when
 * the engine issues VM_Call(uivm, UI_*, ...).
 *
 * Note that the SP UI uses string-based menu names (UI_SetActiveMenu takes
 * a "menuname" string like "mainMenu" or "ingameMenu") whereas the ioEF
 * engine uses an integer enum (uiMenuCommand_t).  CL_SP_UIVmMain handles
 * this translation in the UI_SET_ACTIVE_MENU case.
 */
typedef struct {
	void		(*UI_Init)( int apiVersion, sp_uiimport_t *uiimport );
	void		(*UI_Shutdown)( void );
	void		(*UI_KeyEvent)( int key );
	void		(*UI_MouseEvent)( int dx, int dy );
	void		(*UI_Refresh)( int time );
	void		(*UI_GetActiveMenu)( char **menuname, qboolean *fullscreen );
	void		(*UI_SetActiveMenu)( const char *menuname, const char *menuID );
	qboolean	(*UI_ConsoleCommand)( void );
	void		(*UI_DrawConnect)( const char *servername, const char *updateInfoString );
	void		(*UI_DrawConnectText)( const char *servername, const char *updateInfoString );
	void		(*UI_UpdateConnectionString)( char *string );
	void		(*UI_UpdateConnectionMessageString)( char *string );
} sp_uiexport_t;

// ============================================================================
// Module state
// ============================================================================

static sp_uiexport_t	*ue;			// Export table returned by the DLL's GetUIAPI()
static void				*uiLibrary;		// Sys_LoadLibrary handle for efuix86.dll
static sp_uiimport_t	uii;			// Import table we fill in and pass to UI_Init

// ============================================================================
// Save-game file format constants
//
// The original EF1 save format uses a simple chunked binary layout:
//
//   [chunk-header] [payload] [magic-trailer]
//
// Each chunk header is 12 bytes:
//   4 bytes  chunk ID   (e.g. SP_SAVE_CHUNK_COMM, SP_SAVE_CHUNK_SHOT)
//   4 bytes  payload length
//   4 bytes  CRC-32 checksum of the payload
//
// After the payload, a 4-byte magic value (SP_SAVE_CHUNK_MAGIC) acts as
// a sentinel to detect truncated or corrupt files.
//
// The two chunks we care about for the UI load/save screen:
//   COMM  -- 128-byte comment string (map name, timestamp, etc.)
//            Bytes 0..62 are the display string; bytes 64..127 hold
//            a secondary sort key / metadata string.
//   SHOT  -- 256x256 RGBA screenshot thumbnail (256 * 256 * 4 = 262144 bytes)
// ============================================================================

#define SP_SAVE_CHUNK_MAGIC          0x1234ABCDu   // End-of-chunk sentinel
#define SP_SAVE_CHUNK_COMM           0x434F4D4Du   // "COMD" in little-endian
#define SP_SAVE_CHUNK_SHOT           0x53484F54u   // "SHOT" in little-endian
#define SP_SAVE_COMMENT_SIZE         128
#define SP_SAVE_SORTINFO_OFFSET      64            // Secondary string starts at byte 64
#define SP_SAVE_SHOT_SIZE            ( 256 * 256 * 4 )

// Persistent buffers for save-game metadata and thumbnails.
// These are static because the SP UI module expects the returned pointers
// to remain valid across multiple calls (it does NOT copy the data).
static byte		cl_sp_storedSaveComment[SP_SAVE_COMMENT_SIZE];  // Comment being written to a new save
static byte		cl_sp_loadedSaveComment[SP_SAVE_COMMENT_SIZE];  // Comment read back from an existing save
static byte		cl_sp_saveScratch[SP_SAVE_SHOT_SIZE];           // Scratch buffer for validation reads
static byte		cl_sp_cachedScreenshot[SP_SAVE_SHOT_SIZE];      // 256x256 RGBA of the current frame
static qboolean	cl_sp_cachedScreenshotValid;                    // Whether cachedScreenshot is populated

// Dynamic capture buffers -- allocated once at the current resolution,
// reused until the resolution changes or the module is shut down.
static byte		*cl_sp_captureBuffer;   // Raw RGB framebuffer capture
static byte		*cl_sp_encodeBuffer;    // Scratch for TakeVideoFrame encoding
static int		cl_sp_captureWidth;
static int		cl_sp_captureHeight;

// Forward declarations
static qboolean CL_SP_CaptureSaveScreenshot( void );
const byte *CL_SP_GetStoredSaveComment( void );
qboolean CL_SP_CopySaveScreenshot( byte *outRGBA, int outSize );

// ============================================================================
// Wrapper functions for sp_uiimport_t
//
// Each function below corresponds to one slot in the sp_uiimport_t table.
// Most are trivial pass-throughs, but a few adapt between the SP UI module's
// calling convention and the ioEF engine's internal APIs:
//
//   - Printf / Error: variadic wrappers (the SP DLL calls through a function
//     pointer so we need to resolve the va_list on our side)
//   - R_AddPolyToScene: SP version takes 3 args, engine takes 4 (num polys)
//   - S_RegisterSound: SP version takes 1 arg, engine takes 2 (compressed flag)
//   - DrawStretchRaw: SP has a fLightValue float, engine has client+dirty params
//   - R_ScissorPic: not available in ioEF renderer, mapped to DrawStretchPic
//   - S_StartLocalLoopingSound: no equivalent in ioEF client, stubbed out
//   - Key_SetCatcher: preserves KEYCATCH_CONSOLE to prevent UI from closing it
//   - GetClientState: SP returns connstate_t directly, not via a struct
// ============================================================================

static void CL_SP_UI_Printf( const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Printf( "%s", text );
}

static void CL_SP_UI_Error( int errLevel, const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Error( errLevel, "%s", text );
}

static void CL_SP_UI_CvarSet( const char *name, const char *value ) {
	Cvar_Set( name, value );
}

static float CL_SP_UI_CvarVariableValue( const char *var_name ) {
	return Cvar_VariableValue( var_name );
}

static void CL_SP_UI_CvarVariableStringBuffer( const char *var_name, char *buffer, int bufsize ) {
	Cvar_VariableStringBuffer( var_name, buffer, bufsize );
}

static void CL_SP_UI_CvarSetValue( const char *var_name, float value ) {
	Cvar_SetValue( var_name, value );
}

static void CL_SP_UI_CvarReset( const char *name ) {
	Cvar_Reset( name );
}

static void CL_SP_UI_CvarCreate( const char *var_name, const char *var_value, int flags ) {
	Cvar_Get( var_name, var_value, flags );
}

static void CL_SP_UI_CvarInfoStringBuffer( int bit, char *buffer, int bufsize ) {
	Cvar_InfoStringBuffer( bit, buffer, bufsize );
}

static int CL_SP_UI_Argc( void ) {
	return Cmd_Argc();
}

static void CL_SP_UI_Argv( int n, char *buffer, int bufferLength ) {
	Cmd_ArgvBuffer( n, buffer, bufferLength );
}

static void CL_SP_UI_CmdExecuteText( int exec_when, const char *text ) {
	Cbuf_ExecuteText( exec_when, text );
}

static void CL_SP_UI_CmdTokenizeString( const char *text ) {
	Cmd_TokenizeString( text );
}

static int CL_SP_UI_FS_FOpenFile( const char *qpath, fileHandle_t *file, fsMode_t mode ) {
	return FS_FOpenFileByMode( qpath, file, mode );
}

static int CL_SP_UI_FS_Read( void *buffer, int len, fileHandle_t f ) {
	return FS_Read( buffer, len, f );
}

static int CL_SP_UI_FS_Write( const void *buffer, int len, fileHandle_t f ) {
	return FS_Write( buffer, len, f );
}

static void CL_SP_UI_FS_FCloseFile( fileHandle_t f ) {
	FS_FCloseFile( f );
}

static int CL_SP_UI_FS_GetFileList( const char *path, const char *extension, char *listbuf, int bufsize ) {
	return FS_GetFileList( path, extension, listbuf, bufsize );
}

static int CL_SP_UI_FS_ReadFile( const char *name, void **buf ) {
	return FS_ReadFile( name, buf );
}

static void CL_SP_UI_FS_FreeFile( void *buf ) {
	FS_FreeFile( buf );
}

static qhandle_t CL_SP_UI_R_RegisterModel( const char *name ) {
	return re.RegisterModel( name );
}

static qhandle_t CL_SP_UI_R_RegisterSkin( const char *name ) {
	return re.RegisterSkin( name );
}

static qhandle_t CL_SP_UI_R_RegisterShader( const char *name ) {
	return re.RegisterShader( name );
}

static qhandle_t CL_SP_UI_R_RegisterShaderNoMip( const char *name ) {
	return re.RegisterShaderNoMip( name );
}

static void CL_SP_UI_R_ClearScene( void ) {
	re.ClearScene();
}

static void CL_SP_UI_R_AddRefEntityToScene( const refEntity_t *ent ) {
	re.AddRefEntityToScene( ent );
}

// The SP UI's R_AddPolyToScene takes 3 args; the engine's takes 4 (extra 'num' param).
static void CL_SP_UI_R_AddPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts ) {
	re.AddPolyToScene( hShader, numVerts, verts, 1 );
}

static void CL_SP_UI_R_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b ) {
	re.AddLightToScene( org, intensity, r, g, b );
}

static void CL_SP_UI_R_RenderScene( const refdef_t *fd ) {
	re.RenderScene( fd );
}

static void CL_SP_UI_R_SetColor( const float *rgba ) {
	re.SetColor( rgba );
}

static void CL_SP_UI_R_DrawStretchPic( float x, float y, float w, float h,
										float s1, float t1, float s2, float t2,
										qhandle_t hShader ) {
	re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
}

// R_ScissorPic - map to DrawStretchPic (no scissor variant in ioEF renderer)
static void CL_SP_UI_R_ScissorPic( float x, float y, float w, float h,
									float s1, float t1, float s2, float t2,
									qhandle_t hShader ) {
	re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
}

static void CL_SP_UI_UpdateScreen( void ) {
	SCR_UpdateScreen();
}

static void CL_SP_UI_PrecacheScreenshot( void ) {
	CL_SP_CaptureSaveScreenshot();
}

static void CL_SP_UI_R_LerpTag( orientation_t *tag, clipHandle_t mod,
								 int startFrame, int endFrame, float frac,
								 const char *tagName ) {
	re.LerpTag( tag, mod, startFrame, endFrame, frac, tagName );
}

static void CL_SP_UI_S_StartLocalSound( sfxHandle_t sfxHandle, int channelNum ) {
	S_StartLocalSound( sfxHandle, channelNum );
}

// The SP UI's S_RegisterSound takes 1 arg; the engine's takes 2 (extra 'compressed' param).
static sfxHandle_t CL_SP_UI_S_RegisterSound( const char *name ) {
	return S_RegisterSound( name, qfalse );
}

static void CL_SP_UI_S_StartLocalLoopingSound( sfxHandle_t sfxHandle ) {
	// Stub - no looping local sound API in ioEF client
}

// The SP UI's DrawStretchRaw has a float fLightValue param; the engine's has int client + qboolean dirty.
static void CL_SP_UI_DrawStretchRaw( int x, int y, int w, int h,
									  int cols, int rows, const byte *data,
									  float fLightValue ) {
	re.DrawStretchRaw( x, y, w, h, cols, rows, data, 0, qtrue );
}

// ============================================================================
// Savegame bridge helpers
//
// The original EF1 engine had a full save/load subsystem. The SP UI module
// calls into the engine's SG_* functions to:
//
//   SG_GetSaveImage          -- Read the 256x256 RGBA thumbnail from a .sav file
//   SG_GetSaveGameComment    -- Read the comment/description string from a .sav
//   SG_ValidateForLoadSaveScreen -- Check that a .sav file is readable/valid
//   SG_GameAllowedToSaveHere -- Ask the game module if saving is permitted now
//   SG_StoreSaveGameComment  -- Store a comment string for the NEXT save operation
//   SCR_GetScreenshot        -- Get the current frame as a 256x256 RGBA thumbnail
//
// Our implementations read the chunked save format (COMM + SHOT chunks)
// from saves/*.sav files for the load screen, and capture live framebuffer
// data for the save screen thumbnail.  The actual save-game WRITING is
// handled elsewhere (in the server-side SP bridge); this file only provides
// the read-back and screenshot capture that the UI needs.
// ============================================================================

static void CL_SP_FreeCaptureBuffers( void ) {
	if ( cl_sp_captureBuffer ) {
		Z_Free( cl_sp_captureBuffer );
		cl_sp_captureBuffer = NULL;
	}
	if ( cl_sp_encodeBuffer ) {
		Z_Free( cl_sp_encodeBuffer );
		cl_sp_encodeBuffer = NULL;
	}
	cl_sp_captureWidth = 0;
	cl_sp_captureHeight = 0;
}

static qboolean CL_SP_EnsureCaptureBuffers( void ) {
	int paddedRowBytes;
	int bufferSize;

	if ( cls.glconfig.vidWidth <= 0 || cls.glconfig.vidHeight <= 0 ) {
		return qfalse;
	}

	if ( cl_sp_captureBuffer &&
		cl_sp_encodeBuffer &&
		cl_sp_captureWidth == cls.glconfig.vidWidth &&
		cl_sp_captureHeight == cls.glconfig.vidHeight ) {
		return qtrue;
	}

	CL_SP_FreeCaptureBuffers();

	cl_sp_captureWidth = cls.glconfig.vidWidth;
	cl_sp_captureHeight = cls.glconfig.vidHeight;
	paddedRowBytes = PAD( cl_sp_captureWidth * 3, 4 );
	bufferSize = paddedRowBytes * cl_sp_captureHeight + 4;

	cl_sp_captureBuffer = Z_Malloc( bufferSize );
	cl_sp_encodeBuffer = Z_Malloc( bufferSize );
	if ( !cl_sp_captureBuffer || !cl_sp_encodeBuffer ) {
		CL_SP_FreeCaptureBuffers();
		return qfalse;
	}

	return qtrue;
}

static qboolean CL_SP_CaptureSaveScreenshot( void ) {
	const byte *sourcePixels;
	int paddedRowBytes;
	int x;
	int y;

	if ( !CL_SP_EnsureCaptureBuffers() ) {
		cl_sp_cachedScreenshotValid = qfalse;
		return qfalse;
	}

	re.TakeVideoFrame( cl_sp_captureWidth, cl_sp_captureHeight,
		cl_sp_captureBuffer, cl_sp_encodeBuffer, qfalse );

	paddedRowBytes = PAD( cl_sp_captureWidth * 3, 4 );
	sourcePixels = PADP( cl_sp_captureBuffer, 4 );
	for ( y = 0; y < 256; y++ ) {
		int srcY = cl_sp_captureHeight - 1 - ( y * cl_sp_captureHeight / 256 );
		byte *dstRow = cl_sp_cachedScreenshot + y * 256 * 4;
		const byte *srcRow;

		if ( srcY < 0 ) {
			srcY = 0;
		} else if ( srcY >= cl_sp_captureHeight ) {
			srcY = cl_sp_captureHeight - 1;
		}

		srcRow = sourcePixels + srcY * paddedRowBytes;
		for ( x = 0; x < 256; x++ ) {
			int srcX = x * cl_sp_captureWidth / 256;
			const byte *src = srcRow + srcX * 3;
			byte *dst = dstRow + x * 4;

			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = 255;
		}
	}

	cl_sp_cachedScreenshotValid = qtrue;
	return qtrue;
}

static qboolean CL_SP_NormalizeSaveSlotName( const char *slotName, char *outBaseName, int outSize ) {
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

	return qtrue;
}

static void CL_SP_BuildSaveQPath( const char *baseName, char *outQPath, int outSize ) {
	Com_sprintf( outQPath, outSize, "saves/%s.sav", baseName );
}

static qboolean CL_SP_ReadSaveChunkHeader( fileHandle_t file, unsigned int *chunkId, unsigned int *length, unsigned int *checksum ) {
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

static qboolean CL_SP_ReadSaveChunk( fileHandle_t file, unsigned long expectedChunkId,
		void *buffer, int expectedLength, qboolean quiet ) {
	unsigned int chunkId;
	unsigned int length;
	unsigned int checksum;
	unsigned int fileMagic;

	if ( !CL_SP_ReadSaveChunkHeader( file, &chunkId, &length, &checksum ) ) {
		return qfalse;
	}

	if ( chunkId != expectedChunkId ) {
		if ( !quiet ) {
			Com_Printf( "^1Unexpected save chunk 0x%08x, expected 0x%08lx\n", chunkId, expectedChunkId );
		}
		return qfalse;
	}

	if ( expectedLength > 0 && (int)length != expectedLength ) {
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx has invalid length %u\n", expectedChunkId, length );
		}
		return qfalse;
	}

	if ( buffer ) {
		if ( FS_Read( buffer, length, file ) != (int)length ) {
			return qfalse;
		}
	} else if ( FS_Seek( file, length, FS_SEEK_CUR ) != 0 ) {
		return qfalse;
	}

	if ( FS_Read( &fileMagic, sizeof( fileMagic ), file ) != sizeof( fileMagic ) ||
		LittleLong( fileMagic ) != SP_SAVE_CHUNK_MAGIC ) {
		return qfalse;
	}

	if ( buffer && length > 0 && Com_BlockChecksum( buffer, length ) != checksum ) {
		if ( !quiet ) {
			Com_Printf( "^1Save chunk 0x%08lx failed checksum\n", expectedChunkId );
		}
		return qfalse;
	}

	return qtrue;
}

const byte *CL_SP_GetStoredSaveComment( void ) {
	return cl_sp_storedSaveComment;
}

qboolean CL_SP_CopySaveScreenshot( byte *outRGBA, int outSize ) {
	if ( outSize < SP_SAVE_SHOT_SIZE || !outRGBA ) {
		return qfalse;
	}

	if ( !cl_sp_cachedScreenshotValid && !CL_SP_CaptureSaveScreenshot() ) {
		return qfalse;
	}

	Com_Memcpy( outRGBA, cl_sp_cachedScreenshot, SP_SAVE_SHOT_SIZE );
	return qtrue;
}

/*
 * CL_SP_UI_SG_GetSaveImage -- Read the 256x256 RGBA thumbnail from a save file.
 *
 * The UI's load-game screen calls this to display a preview image for each
 * save slot.  We open saves/<name>.sav, skip past the COMM (comment) chunk,
 * and read the SHOT (screenshot) chunk directly into pvAddress.
 *
 * Returns qtrue if the thumbnail was successfully read.
 */
static qboolean CL_SP_UI_SG_GetSaveImage( const char *psPathlessBaseName, void *pvAddress ) {
	char baseName[MAX_QPATH];
	char qpath[MAX_QPATH];
	fileHandle_t file;
	qboolean success;

	if ( !pvAddress || !CL_SP_NormalizeSaveSlotName( psPathlessBaseName, baseName, sizeof( baseName ) ) ) {
		return qfalse;
	}

	CL_SP_BuildSaveQPath( baseName, qpath, sizeof( qpath ) );
	if ( FS_FOpenFileByMode( qpath, &file, FS_READ ) < 0 || !file ) {
		return qfalse;
	}

	success = CL_SP_ReadSaveChunk( file, SP_SAVE_CHUNK_COMM, cl_sp_loadedSaveComment, sizeof( cl_sp_loadedSaveComment ), qtrue ) &&
		CL_SP_ReadSaveChunk( file, SP_SAVE_CHUNK_SHOT, pvAddress, SP_SAVE_SHOT_SIZE, qtrue );
	cl_sp_loadedSaveComment[SP_SAVE_SORTINFO_OFFSET - 1] = '\0';
	cl_sp_loadedSaveComment[SP_SAVE_COMMENT_SIZE - 1] = '\0';
	FS_FCloseFile( file );
	return success;
}

/*
 * CL_SP_UI_SG_GetSaveGameComment -- Read the comment string from a save file.
 *
 * Returns a pointer to a static buffer containing the null-terminated comment
 * string (map name, date, etc.).  The pointer remains valid until the next
 * call to this function or SG_GetSaveImage.  Returns NULL on failure.
 */
static void *CL_SP_UI_SG_GetSaveGameComment( const char *psPathlessBaseName ) {
	char baseName[MAX_QPATH];
	char qpath[MAX_QPATH];
	fileHandle_t file;

	if ( !CL_SP_NormalizeSaveSlotName( psPathlessBaseName, baseName, sizeof( baseName ) ) ) {
		return NULL;
	}

	CL_SP_BuildSaveQPath( baseName, qpath, sizeof( qpath ) );
	if ( FS_FOpenFileByMode( qpath, &file, FS_READ ) < 0 || !file ) {
		return NULL;
	}

	if ( !CL_SP_ReadSaveChunk( file, SP_SAVE_CHUNK_COMM, cl_sp_loadedSaveComment, sizeof( cl_sp_loadedSaveComment ), qtrue ) ) {
		FS_FCloseFile( file );
		return NULL;
	}

	cl_sp_loadedSaveComment[SP_SAVE_SORTINFO_OFFSET - 1] = '\0';
	cl_sp_loadedSaveComment[SP_SAVE_COMMENT_SIZE - 1] = '\0';
	FS_FCloseFile( file );
	return cl_sp_loadedSaveComment;
}

/*
 * CL_SP_UI_SG_ValidateForLoadSaveScreen -- Check if a save file is displayable.
 *
 * The UI calls this to determine whether a save slot should appear on the
 * load/save screen.  We attempt to read both the COMM and SHOT chunks; if
 * either is missing or corrupt, the slot is considered invalid.
 */
static qboolean CL_SP_UI_SG_ValidateForLoadSaveScreen( const char *psPathlessBaseName ) {
	char baseName[MAX_QPATH];
	char qpath[MAX_QPATH];
	fileHandle_t file;
	qboolean valid;

	if ( !CL_SP_NormalizeSaveSlotName( psPathlessBaseName, baseName, sizeof( baseName ) ) ) {
		return qfalse;
	}

	CL_SP_BuildSaveQPath( baseName, qpath, sizeof( qpath ) );
	if ( FS_FOpenFileByMode( qpath, &file, FS_READ ) < 0 || !file ) {
		return qfalse;
	}

	valid = CL_SP_ReadSaveChunk( file, SP_SAVE_CHUNK_COMM, cl_sp_loadedSaveComment, sizeof( cl_sp_loadedSaveComment ), qtrue ) &&
		CL_SP_ReadSaveChunk( file, SP_SAVE_CHUNK_SHOT, cl_sp_saveScratch, SP_SAVE_SHOT_SIZE, qtrue );
	cl_sp_loadedSaveComment[SP_SAVE_SORTINFO_OFFSET - 1] = '\0';
	cl_sp_loadedSaveComment[SP_SAVE_COMMENT_SIZE - 1] = '\0';
	FS_FCloseFile( file );
	return valid;
}

/*
 * CL_SP_UI_SG_GameAllowedToSaveHere -- Ask the game module if saving is allowed.
 *
 * The original EF1 engine forwarded this call to the game DLL, which would
 * return qfalse during cutscenes, scripted sequences, or other moments where
 * a save would cause problems.  We reach across to the server-side SP bridge
 * (SV_SP_GetGameExport) to call the game module's GameAllowedToSaveHere export.
 */
static qboolean CL_SP_UI_SG_GameAllowedToSaveHere( qboolean inCamera ) {
	// Wire through to the SP game module's export if available
	extern void *SV_SP_GetGameExport( void );
	typedef struct { int apiversion; void *Init; void *Shutdown; void *WriteLevel; void *ReadLevel; qboolean (*GameAllowedToSaveHere)(void); } sp_ge_save_t;
	sp_ge_save_t *ge = (sp_ge_save_t *)SV_SP_GetGameExport();
	if ( ge && ge->GameAllowedToSaveHere ) {
		return ge->GameAllowedToSaveHere();
	}
	// No game module loaded -- e.g. at the main menu with no map running.
	// The SP UI's UI_SetActiveMenu() uses this as a gate before bringing up
	// ANY menu, so returning qfalse here prevents the main menu from ever
	// appearing (the engine then draws the full-screen console instead,
	// because clc.state==CA_DISCONNECTED and KEYCATCH_UI is never set).
	// With no in-progress game there is nothing to block, so allow it.
	return qtrue;
}

/*
 * CL_SP_UI_SG_StoreSaveGameComment -- Store a comment for the next save operation.
 *
 * Called by the UI when the player initiates a save.  The comment string
 * (typically the map name and timestamp) is stashed in cl_sp_storedSaveComment
 * and later written into the COMM chunk of the .sav file by the server-side
 * save code.
 */
static void CL_SP_UI_SG_StoreSaveGameComment( const char *sComment ) {
	memset( cl_sp_storedSaveComment, 0, sizeof( cl_sp_storedSaveComment ) );
	if ( sComment ) {
		Q_strncpyz( (char *)cl_sp_storedSaveComment, sComment, SP_SAVE_SORTINFO_OFFSET );
	}
}

/*
 * CL_SP_UI_SCR_GetScreenshot -- Get the current frame as a 256x256 RGBA image.
 *
 * Used by the save-game screen to show a live thumbnail of the current game.
 * Captures from the framebuffer on first call (or if the cache is stale) and
 * returns a pointer to the static 256x256 RGBA buffer.  The qboolean out-param
 * is set to qtrue if the screenshot is valid.
 */
static byte *CL_SP_UI_SCR_GetScreenshot( qboolean *out ) {
	if ( !cl_sp_cachedScreenshotValid ) {
		CL_SP_CaptureSaveScreenshot();
	}

	if ( out ) {
		*out = cl_sp_cachedScreenshotValid;
	}

	return cl_sp_cachedScreenshotValid ? cl_sp_cachedScreenshot : NULL;
}

// ============================================================================
// Key function wrappers
// ============================================================================

static void CL_SP_UI_Key_KeynumToStringBuf( int keynum, char *buf, int buflen ) {
	Q_strncpyz( buf, Key_KeynumToString( keynum ), buflen );
}

static void CL_SP_UI_Key_GetBindingBuf( int keynum, char *buf, int buflen ) {
	char *value;

	value = Key_GetBinding( keynum );
	if ( value ) {
		Q_strncpyz( buf, value, buflen );
	} else {
		*buf = 0;
	}
}

static void CL_SP_UI_Key_SetBinding( int keynum, const char *binding ) {
	Key_SetBinding( keynum, binding );
}

static qboolean CL_SP_UI_Key_IsDown( int keynum ) {
	return Key_IsDown( keynum );
}

static qboolean CL_SP_UI_Key_GetOverstrikeMode( void ) {
	return Key_GetOverstrikeMode();
}

static void CL_SP_UI_Key_SetOverstrikeMode( qboolean state ) {
	Key_SetOverstrikeMode( state );
}

static void CL_SP_UI_Key_ClearStates( void ) {
	Key_ClearStates();
}

static int CL_SP_UI_Key_GetCatcher( void ) {
	return Key_GetCatcher();
}

static void CL_SP_UI_Key_SetCatcher( int catcher ) {
	// Don't allow the UI module to close the console
	Key_SetCatcher( catcher | ( Key_GetCatcher() & KEYCATCH_CONSOLE ) );
}

// ============================================================================
// Client state / config wrappers
// ============================================================================

static void CL_SP_UI_GetClipboardData( char *buf, int bufsize ) {
	char *cbd;

	cbd = Sys_GetClipboardData();
	if ( !cbd ) {
		*buf = 0;
		return;
	}

	Q_strncpyz( buf, cbd, bufsize );
	Z_Free( cbd );
}

static void CL_SP_UI_GetGlconfig( glconfig_t *config ) {
	*config = cls.glconfig;
}

// The SP UI's GetClientState returns connstate_t directly, not via a struct.
static connstate_t CL_SP_UI_GetClientState( void ) {
	return clc.state;
}

static void CL_SP_UI_GetConfigString( int index, char *buff, int buffsize ) {
	int offset;

	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		if ( buffsize ) {
			buff[0] = 0;
		}
		return;
	}

	offset = cl.gameState.stringOffsets[index];
	if ( !offset ) {
		if ( buffsize ) {
			buff[0] = 0;
		}
		return;
	}

	Q_strncpyz( buff, cl.gameState.stringData + offset, buffsize );
}

static int CL_SP_UI_Milliseconds( void ) {
	return Sys_Milliseconds();
}

// ============================================================================
// Import table initialization
// ============================================================================

/*
===============
CL_SP_InitUIImport

Populates the sp_uiimport_t struct with wrapper functions that bridge
from the SP UI module's function pointer interface to the engine.
===============
*/
static void CL_SP_InitUIImport( void ) {
	Com_Memset( &uii, 0, sizeof( uii ) );

	uii.Printf						= CL_SP_UI_Printf;
	uii.Error						= CL_SP_UI_Error;

	uii.Cvar_Set					= CL_SP_UI_CvarSet;
	uii.Cvar_VariableValue			= CL_SP_UI_CvarVariableValue;
	uii.Cvar_VariableStringBuffer	= CL_SP_UI_CvarVariableStringBuffer;
	uii.Cvar_SetValue				= CL_SP_UI_CvarSetValue;
	uii.Cvar_Reset					= CL_SP_UI_CvarReset;
	uii.Cvar_Create					= CL_SP_UI_CvarCreate;
	uii.Cvar_InfoStringBuffer		= CL_SP_UI_CvarInfoStringBuffer;

	uii.Argc						= CL_SP_UI_Argc;
	uii.Argv						= CL_SP_UI_Argv;
	uii.Cmd_ExecuteText				= CL_SP_UI_CmdExecuteText;
	uii.Cmd_TokenizeString			= CL_SP_UI_CmdTokenizeString;

	uii.FS_FOpenFile				= CL_SP_UI_FS_FOpenFile;
	uii.FS_Read						= CL_SP_UI_FS_Read;
	uii.FS_Write					= CL_SP_UI_FS_Write;
	uii.FS_FCloseFile				= CL_SP_UI_FS_FCloseFile;
	uii.FS_GetFileList				= CL_SP_UI_FS_GetFileList;
	uii.FS_ReadFile					= CL_SP_UI_FS_ReadFile;
	uii.FS_FreeFile					= CL_SP_UI_FS_FreeFile;

	uii.R_RegisterModel				= CL_SP_UI_R_RegisterModel;
	uii.R_RegisterSkin				= CL_SP_UI_R_RegisterSkin;
	uii.R_RegisterShader			= CL_SP_UI_R_RegisterShader;
	uii.R_RegisterShaderNoMip		= CL_SP_UI_R_RegisterShaderNoMip;
	uii.R_ClearScene				= CL_SP_UI_R_ClearScene;
	uii.R_AddRefEntityToScene		= CL_SP_UI_R_AddRefEntityToScene;
	uii.R_AddPolyToScene			= CL_SP_UI_R_AddPolyToScene;
	uii.R_AddLightToScene			= CL_SP_UI_R_AddLightToScene;
	uii.R_RenderScene				= CL_SP_UI_R_RenderScene;
	uii.R_SetColor					= CL_SP_UI_R_SetColor;
	uii.R_DrawStretchPic			= CL_SP_UI_R_DrawStretchPic;
	uii.R_ScissorPic				= CL_SP_UI_R_ScissorPic;

	uii.UpdateScreen				= CL_SP_UI_UpdateScreen;
	uii.PrecacheScreenshot			= CL_SP_UI_PrecacheScreenshot;

	uii.R_LerpTag					= CL_SP_UI_R_LerpTag;

	uii.S_StartLocalSound			= CL_SP_UI_S_StartLocalSound;
	uii.S_RegisterSound				= CL_SP_UI_S_RegisterSound;
	uii.S_StartLocalLoopingSound	= CL_SP_UI_S_StartLocalLoopingSound;

	uii.DrawStretchRaw				= CL_SP_UI_DrawStretchRaw;
	uii.SG_GetSaveImage				= CL_SP_UI_SG_GetSaveImage;
	uii.SG_GetSaveGameComment		= CL_SP_UI_SG_GetSaveGameComment;
	uii.SG_ValidateForLoadSaveScreen = CL_SP_UI_SG_ValidateForLoadSaveScreen;
	uii.SG_GameAllowedToSaveHere	= CL_SP_UI_SG_GameAllowedToSaveHere;
	uii.SG_StoreSaveGameComment		= CL_SP_UI_SG_StoreSaveGameComment;
	uii.SCR_GetScreenshot			= CL_SP_UI_SCR_GetScreenshot;

	uii.Key_KeynumToStringBuf		= CL_SP_UI_Key_KeynumToStringBuf;
	uii.Key_GetBindingBuf			= CL_SP_UI_Key_GetBindingBuf;
	uii.Key_SetBinding				= CL_SP_UI_Key_SetBinding;
	uii.Key_IsDown					= CL_SP_UI_Key_IsDown;
	uii.Key_GetOverstrikeMode		= CL_SP_UI_Key_GetOverstrikeMode;
	uii.Key_SetOverstrikeMode		= CL_SP_UI_Key_SetOverstrikeMode;
	uii.Key_ClearStates				= CL_SP_UI_Key_ClearStates;
	uii.Key_GetCatcher				= CL_SP_UI_Key_GetCatcher;
	uii.Key_SetCatcher				= CL_SP_UI_Key_SetCatcher;

	uii.GetClipboardData			= CL_SP_UI_GetClipboardData;
	uii.GetGlconfig					= CL_SP_UI_GetGlconfig;
	uii.GetClientState				= CL_SP_UI_GetClientState;
	uii.GetConfigString				= CL_SP_UI_GetConfigString;
	uii.Milliseconds				= CL_SP_UI_Milliseconds;
}

// ============================================================================
// Initialization / shutdown
// ============================================================================

/*
===============
CL_SP_InitUI

Loads the EF1 singleplayer UI DLL (efuix86.dll), calls GetUIAPI to get the
export table, validates the API version, and calls UI_Init.
===============
*/
/*
 * SP UI DLL filename is arch-derived (ARCH_STRING is "x86" / "x86_64" / ...), so
 * one engine source builds the loader for any target arch.  Must match the UI
 * project's TargetName ("efui" + ARCH_STRING).  See sv_game_sp.c for the game one.
 */
#define SP_UI_DLL_NAME	"efui" ARCH_STRING DLL_EXT

void CL_SP_InitUI( void ) {
	sp_uiexport_t *(*GetUIAPI)( void );
	char dllPath[MAX_OSPATH];
	const char *basepath;
	const char *gamedir;

	// Set up the import table
	CL_SP_InitUIImport();

	// Load the SP UI DLL
	Com_Printf( "Loading SP UI module: " SP_UI_DLL_NAME "\n" );

	basepath = Cvar_VariableString( "fs_basepath" );
	gamedir = Cvar_VariableString( "fs_game" );

	if ( !gamedir[0] ) {
		gamedir = Cvar_VariableString( "com_basegame" );
	}

	// Try basepath/gamedir/<ui dll> first
	Com_sprintf( dllPath, sizeof(dllPath), "%s/%s/" SP_UI_DLL_NAME, basepath, gamedir );
	Com_Printf( "Try loading dll file %s\n", dllPath );
	uiLibrary = Sys_LoadLibrary( dllPath );

	if ( !uiLibrary ) {
		// Try just the UI dll in current directory
		uiLibrary = Sys_LoadLibrary( SP_UI_DLL_NAME );
	}

	if ( !uiLibrary ) {
		Com_Error( ERR_FATAL, "CL_SP_InitUI: failed to load " SP_UI_DLL_NAME "\n"
				   "Searched: %s", dllPath );
	}

	// Get the GetUIAPI entry point
	GetUIAPI = (sp_uiexport_t *(*)( void ))Sys_LoadFunction( uiLibrary, "GetUIAPI" );

	if ( !GetUIAPI ) {
		Sys_UnloadLibrary( uiLibrary );
		uiLibrary = NULL;
		Com_Error( ERR_FATAL, "CL_SP_InitUI: " SP_UI_DLL_NAME " missing GetUIAPI export" );
	}

	// Exchange function pointer tables
	ue = GetUIAPI();

	if ( !ue ) {
		Sys_UnloadLibrary( uiLibrary );
		uiLibrary = NULL;
		Com_Error( ERR_FATAL, "CL_SP_InitUI: GetUIAPI returned NULL" );
	}

	// Don't call UI_Init here; CL_InitUI() will call it via VM_Call(UI_INIT)
	// which dispatches through CL_SP_UIVmMain.
	Com_Printf( "SP UI module loaded successfully\n" );
}

/*
===============
CL_SP_ShutdownUI

Calls the UI module's Shutdown function and unloads the DLL.
===============
*/
void CL_SP_ShutdownUI( void ) {
	if ( ue ) {
		ue->UI_Shutdown();
		ue = NULL;
	}

	if ( uiLibrary ) {
		Sys_UnloadLibrary( uiLibrary );
		uiLibrary = NULL;
	}

	CL_SP_FreeCaptureBuffers();
	cl_sp_cachedScreenshotValid = qfalse;

	Com_Printf( "SP UI module unloaded\n" );
}

qboolean CL_SP_IsUIActive( void ) {
	return uiLibrary != NULL || ue != NULL;
}

void CL_SP_UIUpdateConnectionString( const char *string ) {
	if ( ue && ue->UI_UpdateConnectionString ) {
		ue->UI_UpdateConnectionString( (char *)string );
	}
}

void CL_SP_UIUpdateConnectionMessageString( const char *string ) {
	if ( ue && ue->UI_UpdateConnectionMessageString ) {
		ue->UI_UpdateConnectionMessageString( (char *)string );
	}
}

/*
===============
CL_SP_UIVmMain

A fake vmMain entry point that dispatches VM_Call commands to the
SP uiexport_t function pointers.  This allows all existing
VM_Call(uivm, UI_*, ...) sites in the engine to work unchanged.

The ioEF engine creates a "fake" vm_t for the SP UI (with vm->isFake
set to true in vm.c).  When VM_Call is invoked on a fake VM, it calls
the vmMain function pointer instead of interpreting QVM bytecode.
That function pointer is set to CL_SP_UIVmMain during CL_InitUI().

Each case in the switch below translates a UI_* command enum (defined
in ui_public.h, shared by both MP and SP code paths) into the
corresponding sp_uiexport_t function pointer call.  The arguments are
extracted from the variadic parameter list in the same order that
VM_Call passes them.
===============
*/
intptr_t QDECL CL_SP_UIVmMain( int command, ... ) {
	va_list ap;
	int arg0, arg1;

	va_start( ap, command );
	arg0 = va_arg( ap, int );
	arg1 = va_arg( ap, int );
	va_end( ap );

	if ( !ue ) {
		Com_Error( ERR_FATAL, "CL_SP_UIVmMain: UI module not loaded" );
		return 0;
	}

	switch ( command ) {
	case UI_GETAPIVERSION:
		return UI_API_VERSION;

	// UI_INIT: Called once during CL_InitUI() to initialize the SP UI module.
	// We pass SP_UI_API_VERSION (2) and our filled-in import table so the DLL
	// can cache the engine function pointers and set up its internal state
	// (loading menu assets, registering cvars, etc.).
	case UI_INIT:
		ue->UI_Init( SP_UI_API_VERSION, &uii );
		return 0;

	// UI_SHUTDOWN: Called during CL_ShutdownUI() when the client is quitting
	// or restarting.  The DLL should free all resources it allocated during
	// UI_Init.  Note: the DLL itself is unloaded separately in CL_SP_ShutdownUI
	// after this call returns.
	case UI_SHUTDOWN:
		ue->UI_Shutdown();
		return 0;

	// UI_KEY_EVENT: Dispatched when the engine receives a key press/release
	// while the UI has key focus (KEYCATCH_UI is set).  arg0 is the Q3
	// keynum (K_* constants from keycodes.h).  The SP UI module handles
	// menu navigation, text input, and binding capture through this path.
	case UI_KEY_EVENT:
		ue->UI_KeyEvent( arg0 );
		return 0;

	case UI_MOUSE_EVENT:
		ue->UI_MouseEvent( arg0, arg1 );
		return 0;

	case UI_REFRESH:
		ue->UI_Refresh( arg0 );
		return 0;

	case UI_IS_FULLSCREEN: {
		// The DLL's UI_GetActiveMenu() does strcpy(*menuname, "unknown") when a
		// menu is active, so *menuname must point to a writable buffer -- passing
		// a NULL pointer segfaults inside the DLL. We only care about the
		// fullscreen flag here, but we must supply a real buffer for the name.
		char menubuf[64];
		char *menuname = menubuf;
		qboolean fullscreen = qfalse;
		menubuf[0] = '\0';
		ue->UI_GetActiveMenu( &menuname, &fullscreen );
		return fullscreen;
	}

	case UI_SET_ACTIVE_MENU: {
		// The ioEF engine passes a uiMenuCommand_t enum value.
		// Map it to the string-based SP UI interface.
		// The DLL's UI_SetActiveMenu (ui_atoms.cpp) compares against short
		// lowercase names: "main", "ingame", "needcd", "newgame", etc.
		// Passing NULL triggers UI_ForceMenuOff() to dismiss menus.
		const char *menuname;
		const char *menuID = NULL;

		switch ( (uiMenuCommand_t)arg0 ) {
		case UIMENU_NONE:
			menuname = NULL;
			break;
		case UIMENU_MAIN:
			menuname = "main";
			break;
		case UIMENU_INGAME:
			menuname = "ingame";
			break;
		case UIMENU_NEED_CD:
			menuname = "needcd";
			break;
		case UIMENU_BAD_CD_KEY:
		case UIMENU_TEAM:
		case UIMENU_POSTGAME:
			// These enum values have no SP equivalent; fall through to main.
			menuname = "main";
			break;
		default:
			menuname = "main";
			break;
		}

		// NULL menuname = close menu (UIMENU_NONE). The DLL's UI_SetActiveMenu
		// calls UI_ForceMenuOff() for NULL, but some builds may not handle NULL
		// in their string comparison. Use an empty string as a safe sentinel.
		if ( menuname ) {
			ue->UI_SetActiveMenu( menuname, menuID );
		} else {
			ue->UI_SetActiveMenu( "", menuID );
		}
		return 0;
	}

	case UI_CONSOLE_COMMAND:
		return ue->UI_ConsoleCommand();

	case UI_DRAW_CONNECT_SCREEN:
		ue->UI_DrawConnect( clc.servername, cls.updateInfoString );
		return 0;

	default:
		Com_Error( ERR_DROP, "CL_SP_UIVmMain: unknown command %d", command );
		return 0;
	}
}

#endif /* ELITEFORCE */

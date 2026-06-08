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
#include "tr_local.h"

#ifdef BUILD_VR
typedef union {
	void	*align;
	byte	cmds[MAX_RENDER_COMMANDS + sizeof( int ) + sizeof( void * )];
} vrStereoReplayBuffer_t;

static vrStereoReplayBuffer_t	vrStereoReplayCommands;
static vrStereoReplayBuffer_t	vrStereoReplayScratch;
static int	vrStereoReplayCommandBytes;
static qboolean vrStereoReplayActive;
static qboolean vrStereoReplayLoggedInvalid;

typedef struct {
	int drawSurfs;
	int stretchPics;
	int swaps;
} vrStereoReplayStats_t;

static qboolean R_VR_InspectStereoReplayCommands( const byte *cmds, vrStereoReplayStats_t *stats ) {
	const void *curCmd = cmds;

	Com_Memset( stats, 0, sizeof( *stats ) );

	while ( 1 ) {
		curCmd = PADP( curCmd, sizeof( void * ) );

		switch ( *(const int *)curCmd ) {
		case RC_SET_COLOR:
			curCmd = (const void *)( (const setColorCommand_t *)curCmd + 1 );
			break;
		case RC_STRETCH_PIC:
			stats->stretchPics++;
			curCmd = (const void *)( (const stretchPicCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_SURFS:
			stats->drawSurfs++;
			curCmd = (const void *)( (const drawSurfsCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_BUFFER:
			curCmd = (const void *)( (const drawBufferCommand_t *)curCmd + 1 );
			break;
		case RC_SWAP_BUFFERS:
			stats->swaps++;
			curCmd = (const void *)( (const swapBuffersCommand_t *)curCmd + 1 );
			break;
		case RC_SCREENSHOT:
			curCmd = (const void *)( (const screenshotCommand_t *)curCmd + 1 );
			break;
		case RC_VIDEOFRAME:
			curCmd = (const void *)( (const videoFrameCommand_t *)curCmd + 1 );
			break;
		case RC_COLORMASK:
			curCmd = (const void *)( (const colorMaskCommand_t *)curCmd + 1 );
			break;
		case RC_CLEARDEPTH:
			curCmd = (const void *)( (const clearDepthCommand_t *)curCmd + 1 );
			break;
		case RC_END_OF_LIST:
			return qtrue;
		default:
			return qfalse;
		}
	}
}

static void R_VR_GetHudReplayOffset( stereoFrame_t stereoFrame, float *x, float *y ) {
	int eye;
	float tanLeft, tanRight, tanUp, tanDown;
	float angleLeft, angleRight, angleUp, angleDown;
	float offX640, offY480;
	float unionLeft, unionRight, unionUp, unionDown;
	float fovXRad;
	float depth;
	float parallax;
	cvar_t *hudDepth;

	*x = 0.0f;
	*y = 0.0f;

	if ( stereoFrame != STEREO_LEFT && stereoFrame != STEREO_RIGHT ) {
		return;
	}
	if ( !ri.VR_GetFovTangentsForEye ) {
		return;
	}

	eye = ( stereoFrame == STEREO_LEFT ) ? 0 : 1;
	if ( !ri.VR_GetFovTangentsForEye( eye, &tanLeft, &tanRight, &tanUp, &tanDown ) ) {
		return;
	}

	angleLeft = atan( tanLeft );
	angleRight = atan( tanRight );
	angleUp = atan( tanUp );
	angleDown = atan( tanDown );

	offX640 = -0.5f * ( angleLeft + angleRight ) * 640.0f;
	offY480 = -0.5f * ( angleUp + angleDown ) * 480.0f;

	if ( ri.VR_GetFovTangentsForEye( -1, &unionLeft, &unionRight, &unionUp, &unionDown ) ) {
		fovXRad = fabs( atan( unionLeft ) ) + fabs( atan( unionRight ) );
	} else {
		fovXRad = fabs( angleLeft ) + fabs( angleRight );
	}
	if ( fovXRad < 1.0f * (float)M_PI / 180.0f ) {
		fovXRad = 90.0f * (float)M_PI / 180.0f;
	}

	hudDepth = ri.Cvar_Get( "cg_hudDepth", "2.0", 0 );
	depth = hudDepth ? hudDepth->value : 2.0f;
	if ( depth < 0.5f ) {
		depth = 0.5f;
	}
	parallax = ( atan2( 0.032f, depth ) / ( fovXRad * 0.5f ) ) * 320.0f;

	// The captured command stream is generated with vr.eye == 0, so HUD-scaled
	// cgame draw commands already include the left-eye convergence parallax.
	// Add the eye-specific asymmetric-FOV recentering, and for the right eye
	// move from captured-left parallax to right-eye parallax.
	if ( eye == 1 ) {
		offX640 -= 2.0f * parallax;
	}

	*x = offX640 * ( (float)glConfig.vidWidth / 640.0f );
	*y = -offY480 * ( (float)glConfig.vidHeight / 480.0f );
}

static void R_VR_PatchStretchPicCommand( stretchPicCommand_t *cmd, stereoFrame_t stereoFrame ) {
	float x, y;

	if ( cmd->x <= 1.0f && cmd->y <= 1.0f &&
	     cmd->w >= glConfig.vidWidth - 2.0f &&
	     cmd->h >= glConfig.vidHeight - 2.0f ) {
		return;
	}

	R_VR_GetHudReplayOffset( stereoFrame, &x, &y );
	cmd->x += x;
	cmd->y += y;
}

static void R_VR_PatchDrawSurfsCommand( drawSurfsCommand_t *cmd, stereoFrame_t stereoFrame ) {
	int eye;
	trRefdef_t savedRefdef;

	if ( stereoFrame != STEREO_LEFT && stereoFrame != STEREO_RIGHT ) {
		return;
	}

	eye = ( stereoFrame == STEREO_LEFT ) ? 0 : 1;
	cmd->refdef.stereoFrame = stereoFrame;
	cmd->viewParms.stereoFrame = stereoFrame;

	if ( !( cmd->refdef.rdflags & RDF_NOWORLDMODEL ) && ri.VR_GetEyeStereoSeparation ) {
		float sep = ri.VR_GetEyeStereoSeparation( eye );
		VectorMA( cmd->refdef.vieworg, sep, cmd->refdef.viewaxis[1], cmd->refdef.vieworg );
		VectorCopy( cmd->refdef.vieworg, cmd->viewParms.or.origin );
		VectorCopy( cmd->refdef.vieworg, cmd->viewParms.pvsOrigin );
	}

	R_RebuildViewParmsWorld( &cmd->viewParms );

	savedRefdef = tr.refdef;
	tr.refdef = cmd->refdef;
	R_SetupProjection( &cmd->viewParms, r_zproj->value, qtrue );
	R_SetupProjectionZ( &cmd->viewParms );
	tr.refdef = savedRefdef;
}

static void R_VR_PatchStereoReplayCommands( byte *cmds, stereoFrame_t stereoFrame ) {
	void *curCmd = cmds;
	qboolean seenDrawSurfs = qfalse;

	while ( 1 ) {
		curCmd = PADP( curCmd, sizeof( void * ) );

		switch ( *(int *)curCmd ) {
		case RC_SET_COLOR:
			curCmd = (void *)( (setColorCommand_t *)curCmd + 1 );
			break;
		case RC_STRETCH_PIC:
			if ( seenDrawSurfs ) {
				R_VR_PatchStretchPicCommand( (stretchPicCommand_t *)curCmd, stereoFrame );
			}
			curCmd = (void *)( (stretchPicCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_SURFS:
			R_VR_PatchDrawSurfsCommand( (drawSurfsCommand_t *)curCmd, stereoFrame );
			seenDrawSurfs = qtrue;
			curCmd = (void *)( (drawSurfsCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_BUFFER:
			curCmd = (void *)( (drawBufferCommand_t *)curCmd + 1 );
			break;
		case RC_SWAP_BUFFERS:
			curCmd = (void *)( (swapBuffersCommand_t *)curCmd + 1 );
			break;
		case RC_SCREENSHOT:
			curCmd = (void *)( (screenshotCommand_t *)curCmd + 1 );
			break;
		case RC_VIDEOFRAME:
			curCmd = (void *)( (videoFrameCommand_t *)curCmd + 1 );
			break;
		case RC_COLORMASK:
			curCmd = (void *)( (colorMaskCommand_t *)curCmd + 1 );
			break;
		case RC_CLEARDEPTH:
			curCmd = (void *)( (clearDepthCommand_t *)curCmd + 1 );
			break;
		case RC_END_OF_LIST:
		default:
			return;
		}
	}
}
#endif

/*
=====================
R_PerformanceCounters
=====================
*/
void R_PerformanceCounters( void ) {
	if ( !r_speeds->integer ) {
		// clear the counters even if we aren't printing
		Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
		Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
		return;
	}

	if (r_speeds->integer == 1) {
		ri.Printf (PRINT_ALL, "%i/%i shaders/surfs %i leafs %i verts %i/%i tris %.2f mtex %.2f dc\n",
			backEnd.pc.c_shaders, backEnd.pc.c_surfaces, tr.pc.c_leafs, backEnd.pc.c_vertexes, 
			backEnd.pc.c_indexes/3, backEnd.pc.c_totalIndexes/3, 
			R_SumOfUsedImages()/(1000000.0f), backEnd.pc.c_overDraw / (float)(glConfig.vidWidth * glConfig.vidHeight) ); 
	} else if (r_speeds->integer == 2) {
		ri.Printf (PRINT_ALL, "(patch) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_patch_in, tr.pc.c_sphere_cull_patch_clip, tr.pc.c_sphere_cull_patch_out, 
			tr.pc.c_box_cull_patch_in, tr.pc.c_box_cull_patch_clip, tr.pc.c_box_cull_patch_out );
		ri.Printf (PRINT_ALL, "(md3) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_md3_in, tr.pc.c_sphere_cull_md3_clip, tr.pc.c_sphere_cull_md3_out, 
			tr.pc.c_box_cull_md3_in, tr.pc.c_box_cull_md3_clip, tr.pc.c_box_cull_md3_out );
	} else if (r_speeds->integer == 3) {
		ri.Printf (PRINT_ALL, "viewcluster: %i\n", tr.viewCluster );
	} else if (r_speeds->integer == 4) {
		if ( backEnd.pc.c_dlightVertexes ) {
			ri.Printf (PRINT_ALL, "dlight srf:%i  culled:%i  verts:%i  tris:%i\n", 
				tr.pc.c_dlightSurfaces, tr.pc.c_dlightSurfacesCulled,
				backEnd.pc.c_dlightVertexes, backEnd.pc.c_dlightIndexes / 3 );
		}
	} 
	else if (r_speeds->integer == 5 )
	{
		ri.Printf( PRINT_ALL, "zFar: %.0f\n", tr.viewParms.zFar );
	}
	else if (r_speeds->integer == 6 )
	{
		ri.Printf( PRINT_ALL, "flare adds:%i tests:%i renders:%i\n", 
			backEnd.pc.c_flareAdds, backEnd.pc.c_flareTests, backEnd.pc.c_flareRenders );
	}

	Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
	Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
}


/*
====================
R_IssueRenderCommands
====================
*/
void R_IssueRenderCommands( qboolean runPerformanceCounters ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;
	assert(cmdList);
	// add an end-of-list command
	*(int *)(cmdList->cmds + cmdList->used) = RC_END_OF_LIST;

	// clear it out, in case this is a sync and not a buffer flip
	cmdList->used = 0;

	if ( runPerformanceCounters ) {
		R_PerformanceCounters();
	}

	// actually start the commands going
	if ( !r_skipBackEnd->integer ) {
		// let it start on the new batch
		RB_ExecuteRenderCommands( cmdList->cmds );
	}
}


/*
====================
R_IssuePendingRenderCommands

Issue any pending commands and wait for them to complete.
====================
*/
void R_IssuePendingRenderCommands( void ) {
	if ( !tr.registered ) {
		return;
	}
	R_IssueRenderCommands( qfalse );
}

/*
============
R_GetCommandBufferReserved

make sure there is enough command space
============
*/
void *R_GetCommandBufferReserved( int bytes, int reservedBytes ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;
	bytes = PAD(bytes, sizeof(void *));

	// always leave room for the end of list command
	if ( cmdList->used + bytes + sizeof( int ) + reservedBytes > MAX_RENDER_COMMANDS ) {
		if ( bytes > MAX_RENDER_COMMANDS - sizeof( int ) ) {
			ri.Error( ERR_FATAL, "R_GetCommandBuffer: bad size %i", bytes );
		}
		// if we run out of room, just start dropping commands
		return NULL;
	}

	cmdList->used += bytes;

	return cmdList->cmds + cmdList->used - bytes;
}

/*
=============
R_GetCommandBuffer

returns NULL if there is not enough space for important commands
=============
*/
void *R_GetCommandBuffer( int bytes ) {
	return R_GetCommandBufferReserved( bytes, PAD( sizeof( swapBuffersCommand_t ), sizeof(void *) ) );
}


/*
=============
R_AddDrawSurfCmd

=============
*/
void	R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	drawSurfsCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_SURFS;

	cmd->drawSurfs = drawSurfs;
	cmd->numDrawSurfs = numDrawSurfs;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
}


/*
=============
RE_SetColor

Passing NULL will set the color to white
=============
*/
void	RE_SetColor( const float *rgba ) {
	setColorCommand_t	*cmd;

  if ( !tr.registered ) {
    return;
  }
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SET_COLOR;
	if ( !rgba ) {
		static float colorWhite[4] = { 1, 1, 1, 1 };

		rgba = colorWhite;
	}

	cmd->color[0] = rgba[0];
	cmd->color[1] = rgba[1];
	cmd->color[2] = rgba[2];
	cmd->color[3] = rgba[3];
}


/*
=============
RE_StretchPic
=============
*/
void RE_StretchPic ( float x, float y, float w, float h, 
					  float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	stretchPicCommand_t	*cmd;

  if (!tr.registered) {
    return;
  }
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}

#define MODE_RED_CYAN	1
#define MODE_RED_BLUE	2
#define MODE_RED_GREEN	3
#define MODE_GREEN_MAGENTA 4
#define MODE_MAX	MODE_GREEN_MAGENTA

void R_SetColorMode(GLboolean *rgba, stereoFrame_t stereoFrame, int colormode)
{
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = GL_TRUE;
	
	if(colormode > MODE_MAX)
	{
		if(stereoFrame == STEREO_LEFT)
			stereoFrame = STEREO_RIGHT;
		else if(stereoFrame == STEREO_RIGHT)
			stereoFrame = STEREO_LEFT;
		
		colormode -= MODE_MAX;
	}
	
	if(colormode == MODE_GREEN_MAGENTA)
	{
		if(stereoFrame == STEREO_LEFT)
			rgba[0] = rgba[2] = GL_FALSE;
		else if(stereoFrame == STEREO_RIGHT)
			rgba[1] = GL_FALSE;
	}
	else
	{
		if(stereoFrame == STEREO_LEFT)
			rgba[1] = rgba[2] = GL_FALSE;
		else if(stereoFrame == STEREO_RIGHT)
		{
			rgba[0] = GL_FALSE;
		
			if(colormode == MODE_RED_BLUE)
				rgba[1] = GL_FALSE;
			else if(colormode == MODE_RED_GREEN)
				rgba[2] = GL_FALSE;
		}
	}
}


/*
====================
RE_BeginFrame

If running in stereo, RE_BeginFrame will be called twice
for each RE_EndFrame
====================
*/
void RE_BeginFrame( stereoFrame_t stereoFrame ) {
	drawBufferCommand_t	*cmd = NULL;
	colorMaskCommand_t *colcmd = NULL;

	if ( !tr.registered ) {
		return;
	}
	glState.finishCalled = qfalse;

	tr.frameCount++;
	tr.frameSceneNum = 0;

	//
	// do overdraw measurement
	//
	if ( r_measureOverdraw->integer )
	{
		if ( glConfig.stencilBits < 4 )
		{
			ri.Printf( PRINT_ALL, "Warning: not enough stencil bits to measure overdraw: %d\n", glConfig.stencilBits );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else if ( r_shadows->integer == 2 )
		{
			ri.Printf( PRINT_ALL, "Warning: stencil shadows and overdraw measurement are mutually exclusive\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else
		{
			R_IssuePendingRenderCommands();
			qglEnable( GL_STENCIL_TEST );
			qglStencilMask( ~0U );
			qglClearStencil( 0U );
			qglStencilFunc( GL_ALWAYS, 0U, ~0U );
			qglStencilOp( GL_KEEP, GL_INCR, GL_INCR );
		}
		r_measureOverdraw->modified = qfalse;
	}
	else
	{
		// this is only reached if it was on and is now off
		if ( r_measureOverdraw->modified ) {
			R_IssuePendingRenderCommands();
			qglDisable( GL_STENCIL_TEST );
		}
		r_measureOverdraw->modified = qfalse;
	}

	//
	// texturemode stuff
	//
	if ( r_textureMode->modified ) {
		R_IssuePendingRenderCommands();
		GL_TextureMode( r_textureMode->string );
		r_textureMode->modified = qfalse;
	}

	//
	// gamma stuff
	//
	if ( r_gamma->modified ) {
		r_gamma->modified = qfalse;

		R_IssuePendingRenderCommands();
		R_SetColorMappings();
	}

	// check for errors
	if ( !r_ignoreGLErrors->integer )
	{
		int	err;

		R_IssuePendingRenderCommands();
		if ((err = qglGetError()) != GL_NO_ERROR)
			ri.Error(ERR_FATAL, "RE_BeginFrame() - glGetError() failed (0x%x)!", err);
	}

	if (glConfig.stereoEnabled) {
		if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
			return;
			
		cmd->commandId = RC_DRAW_BUFFER;
		
		if ( stereoFrame == STEREO_LEFT ) {
			cmd->buffer = (int)GL_BACK_LEFT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			cmd->buffer = (int)GL_BACK_RIGHT;
		} else {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	}
	else
	{
		if(r_anaglyphMode->integer)
		{
			if(r_anaglyphMode->modified)
			{
				// clear both, front and backbuffer.
				qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				qglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
				
				qglDrawBuffer(GL_FRONT);
				qglClear(GL_COLOR_BUFFER_BIT);
				qglDrawBuffer(GL_BACK);
				qglClear(GL_COLOR_BUFFER_BIT);
				
				r_anaglyphMode->modified = qfalse;
			}
			
			if(stereoFrame == STEREO_LEFT)
			{
				if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
					return;
				
				if( !(colcmd = R_GetCommandBuffer(sizeof(*colcmd))) )
					return;
			}
			else if(stereoFrame == STEREO_RIGHT)
			{
				clearDepthCommand_t *cldcmd;
				
				if( !(cldcmd = R_GetCommandBuffer(sizeof(*cldcmd))) )
					return;

				cldcmd->commandId = RC_CLEARDEPTH;

				if( !(colcmd = R_GetCommandBuffer(sizeof(*colcmd))) )
					return;
			}
			else
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );

			R_SetColorMode(colcmd->rgba, stereoFrame, r_anaglyphMode->integer);
			colcmd->commandId = RC_COLORMASK;
		}
		else
		{
			// In VR, quad-buffer stereo is off but the engine still passes
			// STEREO_LEFT/RIGHT to tag which eye is being rendered (each eye goes
			// to its own OpenXR swapchain FBO, not GL_BACK_LEFT/RIGHT).  Treat it
			// as a normal single-buffer frame; don't error.
#ifdef BUILD_VR
			if(stereoFrame != STEREO_CENTER && !(ri.VR_IsActive && ri.VR_IsActive()))
#else
			if(stereoFrame != STEREO_CENTER)
#endif
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is disabled, but stereoFrame was %i", stereoFrame );

			if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
				return;
		}

		if(cmd)
		{
			cmd->commandId = RC_DRAW_BUFFER;

			if(r_anaglyphMode->modified)
			{
				qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				r_anaglyphMode->modified = qfalse;
			}

			if (!Q_stricmp(r_drawBuffer->string, "GL_FRONT"))
				cmd->buffer = (int)GL_FRONT;
			else
				cmd->buffer = (int)GL_BACK;
		}
	}
	
	tr.refdef.stereoFrame = stereoFrame;
}


/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_EndFrame( int *frontEndMsec, int *backEndMsec ) {
	swapBuffersCommand_t	*cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = R_GetCommandBufferReserved( sizeof( *cmd ), 0 );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SWAP_BUFFERS;

	R_IssueRenderCommands( qtrue );

	R_InitNextFrame();

	if ( frontEndMsec ) {
		*frontEndMsec = tr.frontEndMsec;
	}
	tr.frontEndMsec = 0;
	if ( backEndMsec ) {
		*backEndMsec = backEnd.pc.msec;
	}
	backEnd.pc.msec = 0;
}

#ifdef BUILD_VR
qboolean RE_VR_BeginStereoReplayCapture( void ) {
	if ( !tr.registered ) {
		return qfalse;
	}
	if ( r_debugSurface->integer ) {
		return qfalse;
	}

	tr.vrStereoReplayCapture = qtrue;
	vrStereoReplayActive = qfalse;
	vrStereoReplayCommandBytes = 0;
	return qtrue;
}

void RE_VR_CancelStereoReplayCapture( void ) {
	tr.vrStereoReplayCapture = qfalse;
	vrStereoReplayActive = qfalse;
	vrStereoReplayCommandBytes = 0;
	R_InitNextFrame();
}

qboolean RE_VR_ReplayStereoFrame( stereoFrame_t stereoFrame, qboolean finalReplay ) {
	renderCommandList_t *cmdList;
	vrStereoReplayStats_t stats;
	static qboolean loggedReplayStats = qfalse;

	if ( !tr.registered ) {
		return qfalse;
	}

	cmdList = &backEndData->commands;

	if ( !vrStereoReplayActive ) {
		swapBuffersCommand_t *cmd = R_GetCommandBufferReserved( sizeof( *cmd ), 0 );
		if ( !cmd ) {
			RE_VR_CancelStereoReplayCapture();
			return qfalse;
		}
		cmd->commandId = RC_SWAP_BUFFERS;

		*(int *)( cmdList->cmds + cmdList->used ) = RC_END_OF_LIST;
		vrStereoReplayCommandBytes = cmdList->used + sizeof( int );
		if ( vrStereoReplayCommandBytes > (int)sizeof( vrStereoReplayCommands.cmds ) ) {
			RE_VR_CancelStereoReplayCapture();
			return qfalse;
		}

		Com_Memcpy( vrStereoReplayCommands.cmds, cmdList->cmds, vrStereoReplayCommandBytes );
		if ( !R_VR_InspectStereoReplayCommands( vrStereoReplayCommands.cmds, &stats ) ||
		     ( stats.drawSurfs == 0 && stats.stretchPics == 0 ) ) {
			if ( !vrStereoReplayLoggedInvalid ) {
				ri.Printf( PRINT_WARNING,
					"VR stereo replay: invalid captured command stream (%d bytes, drawSurfs=%d, stretchPics=%d, swaps=%d).\n",
					vrStereoReplayCommandBytes, stats.drawSurfs, stats.stretchPics, stats.swaps );
				vrStereoReplayLoggedInvalid = qtrue;
			}
			RE_VR_CancelStereoReplayCapture();
			return qfalse;
		}
		if ( !loggedReplayStats ) {
			ri.Printf( PRINT_ALL,
				"VR stereo replay: captured %d bytes (%d drawSurfs, %d stretchPics, %d swaps).\n",
				vrStereoReplayCommandBytes, stats.drawSurfs, stats.stretchPics, stats.swaps );
			loggedReplayStats = qtrue;
		}
		vrStereoReplayActive = qtrue;
		tr.vrStereoReplayCapture = qfalse;
	}

	Com_Memcpy( vrStereoReplayScratch.cmds, vrStereoReplayCommands.cmds, vrStereoReplayCommandBytes );
	R_VR_PatchStereoReplayCommands( vrStereoReplayScratch.cmds, stereoFrame );

	if ( !r_skipBackEnd->integer ) {
		RB_ExecuteRenderCommands( vrStereoReplayScratch.cmds );
	}

	if ( finalReplay ) {
		R_PerformanceCounters();
		R_InitNextFrame();
		vrStereoReplayActive = qfalse;
		vrStereoReplayCommandBytes = 0;
		tr.frontEndMsec = 0;
		backEnd.pc.msec = 0;
	}

	return qtrue;
}
#endif

/*
=============
RE_TakeVideoFrame
=============
*/
void RE_TakeVideoFrame( int width, int height,
		byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg )
{
	videoFrameCommand_t	*cmd;

	if( !tr.registered ) {
		return;
	}

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if( !cmd ) {
		return;
	}

	cmd->commandId = RC_VIDEOFRAME;

	cmd->width = width;
	cmd->height = height;
	cmd->captureBuffer = captureBuffer;
	cmd->encodeBuffer = encodeBuffer;
	cmd->motionJpeg = motionJpeg;
}

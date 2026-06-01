/*
===========================================================================
VrBase.h -- lightweight engine-facing VR entry points for the ioEF OpenXR
VR port.  Unlike VrCommon.h / TBXR_Common.h this header pulls in NO OpenXR
or GL headers, so plain engine translation units (cl_main.c, cl_scrn.c,
sdl_glimp.c, renderergl1) can call the VR layer without taking an OpenXR
dependency.  Only available when the engine is built with BUILD_VR.
===========================================================================
*/
#ifndef vrbase_h
#define vrbase_h

#include "VrClientInfo.h"   /* the shared vr_client_info_t contract */

/* ---- lifecycle (called from the client startup / shutdown) ---- */
qboolean VR_PreRendererInit( void ); /* BEFORE GL window: XR instance + force per-eye render res */
void     VR_InitOnce( void );        /* AFTER GL window: session/swapchains; idempotent */
qboolean VR_Init( void );            /* phase 2 worker; qfalse => run flat */
void     VR_Shutdown( void );

/* true once the OpenXR session is up and VR is enabled (else the engine runs flat) */
qboolean VR_IsActive( void );

/* ---- per-frame, game-specific (cvar refresh etc.) ---- */
void     VR_FrameSetup( void );

/* ---- virtual-screen decision (true => render 2D to the flat quad layer) ---- */
qboolean VR_UseScreenLayer( void );

/* Per-eye asymmetric view frustum tangents for the eye currently being
   rendered (vr.eye), as set by TBXR_prepareEyeBuffer.  Returns qfalse when
   the VR projection should NOT be used (session inactive, or a non-immersive
   cinematic) so the caller falls back to the flat projection.  The tangents
   are tan(angle): left/down are negative, right/up positive. */
qboolean VR_GetFovTangents( float *tanLeft, float *tanRight, float *tanUp, float *tanDown );

/* Signed lateral view-origin offset (in Quake units, along the view LEFT axis)
   for the given eye, giving stereo (IPD) parallax.  Derived from the actual
   OpenXR eye poses * vr_worldscale.  Apply along refdef viewaxis[1]. */
float    VR_GetEyeStereoSeparation( int eye );

/* 6DoF positional head tracking.  HORIZONTAL: physical lean/step -> player
   movement (RealRTCWXR-style) -- forward/side outputs scaled by 127 into the
   usercmd.  (Vertical duck/stand and the seated-height raise are handled on the
   cgame side via the shared vr.height_offset / vr.worldscale; see cg_view.) */
void     VR_GetPositionalMove( float *forward, float *side );

/* ---- motion-controller input (injected by cl_input.c CL_FinishMove) ----
   Movement: thumbstick forward/side (-1..1-ish), engine scales by 127.
   Buttons:  OR of the EF usercmd button bits (BUTTON_ATTACK / BUTTON_USE).
   UpMove:   +127 jump / -127 crouch / 0 none, written to cmd->upmove.
   TurnDelta: yaw degrees to add to cl.viewangles[YAW] (snap/smooth turn). */
void     VR_GetControllerMove( float *forward, float *side );
int      VR_GetControllerButtons( void );
int      VR_GetControllerUpMove( void );
float    VR_GetTurnDelta( void );

/* ---- OpenXR frame driver (reusable TBXR layer) ---- */
void     TBXR_FrameSetup( void );          /* xrWaitFrame + xrBeginFrame + pose update */
void     TBXR_prepareEyeBuffer( int eye );
void     TBXR_finishEyeBuffer( int eye );
void     TBXR_submitFrame( void );

#endif /* vrbase_h */

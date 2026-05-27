/*
===========================================================================
EFXR_SurfaceView.c -- game-specific OpenXR glue for the ioEF VR port.

Ported/adapted from RealRTCWXR (code/RealRTCWXR/RealRTCWXR/windows/
RTCWXR_SurfaceView.c) for the ioEF Elite Force VR port (milestone M1).

Holds the engine-touching glue that TBXR_Common.c deliberately avoids:
the virtual-screen-layer decision (reads clc/Key_GetCatcher), HMD pose
plumbing into the shared `vr` struct, the per-eye VR projection, per-frame
setup, the screen-resolution / swap helpers, and (stubbed for M1) the
controller-input and haptics entry points.

RTCW-specific gameplay (saber, vehicle, binoculars, emplaced gun, remote
turret, zoom-mode) and any vr_client_info_t fields RealRTCW had but the
ioEF VrClientInfo.h contract omits are dropped -- see report for the list.
None of this is wired into the engine yet (M1 is dead code).
===========================================================================
*/

#include "../VrCommon.h"
#include "../VrCvars.h"
#include "../VrBase.h"

#include "../../client/client.h"


/*
================================================================================

Virtual screen layer / cinematics decision

================================================================================
*/

qboolean VR_UseScreenLayer()
{
	static int frame = 0;
	vr.using_screen_layer =
			(frame++ < 100) || //use screen for first 100 frames - stops splash screen giving a headache
			(bool)((vr.cin_camera && !vr.immersive_cinematics) ||
			vr.misc_camera ||
			clc.demoplaying ||
			(clc.state == CA_DISCONNECTED) ||
			(clc.state == CA_CHALLENGING) ||
			(clc.state == CA_CONNECTING) ||
			(clc.state == CA_CINEMATIC) ||
			(clc.state == CA_LOADING) ||
			(clc.state == CA_PRIMED) ||
			( Key_GetCatcher( ) & KEYCATCH_UI ) ||
			( Key_GetCatcher( ) & KEYCATCH_CONSOLE ));

	return vr.using_screen_layer;
}

float VR_GetScreenLayerDistance()
{
	return (2.0f + vr_screen_dist->value);
}


/*
================================================================================

HMD pose plumbing

================================================================================
*/

void VR_SetHMDOrientation(float pitch, float yaw, float roll)
{
	//Orientation
	VectorSet(vr.hmdorientation, pitch, yaw, roll);
	VectorSubtract(vr.hmdorientation_last, vr.hmdorientation, vr.hmdorientation_delta);

	//Keep this for our records
	VectorCopy(vr.hmdorientation, vr.hmdorientation_last);

	if (!vr.third_person)
	{
		VectorCopy(vr.hmdorientation, vr.hmdorientation_first);
	}

	VectorCopy(vr.weaponangles[ANGLES_ADJUSTED], vr.weaponangles_first[ANGLES_ADJUSTED]);

	// View yaw delta
	float clientview_yaw = vr.clientviewangles[YAW] - vr.hmdorientation[YAW];
	vr.clientview_yaw_delta = vr.clientview_yaw_last - clientview_yaw;
	vr.clientview_yaw_last = clientview_yaw;

	// Max-height is set only once on start, or after re-calibration
	// (ignore too low value which is sometimes provided on start)
	if (!vr.maxHeight || vr.maxHeight < 1.0) {
		vr.maxHeight = vr.hmdposition[1];
	}

	vr.curHeight = vr.hmdposition[1];
}

void VR_SetHMDPosition(float x, float y, float z )
{
	static bool s_useScreen = qfalse;
	static int frame = 0;

	VectorSet(vr.hmdposition, x, y, z);

	//Can be set elsewhere
	vr.take_snap |= (s_useScreen != VR_UseScreenLayer());
	if (vr.take_snap || (frame++ < 100))
	{
		s_useScreen = VR_UseScreenLayer();

		//Record player position on transition
		VectorSet(vr.hmdposition_snap, x, y, z);
		VectorCopy(vr.hmdorientation, vr.hmdorientation_snap);
		if (vr.cin_camera)
		{
			//Reset snap turn too if in a cinematic
			vr.snapTurn = 0;
		}
		vr.take_snap = false;
	}

	VectorSubtract(vr.hmdposition, vr.hmdposition_snap, vr.hmdposition_offset);

	//Position
	VectorSubtract(vr.hmdposition_last, vr.hmdposition, vr.hmdposition_delta);

	//Keep this for our records
	VectorCopy(vr.hmdposition, vr.hmdposition_last);
}


/*
================================================================================

VR projection / per-frame setup

================================================================================
*/

// Per-eye asymmetric view frustum tangents for the eye currently being
// rendered (vr.eye).  The engine (renderergl1 R_SetupProjection) feeds these
// into its existing projection/frustum/Z math, so we only need the tangents
// of the OpenXR-provided asymmetric FOV -- not a full matrix.  Returns qfalse
// when the VR projection should not be used (session not yet active, or a
// non-immersive cinematic) so the engine falls back to the flat projection.
qboolean VR_GetFovTangents(float *tanLeft, float *tanRight, float *tanUp, float *tanDown)
{
	XrFovf fov;

	if (!gAppState.SessionActive || gAppState.Views == NULL)
	{
		return qfalse;
	}

	//Don't use our projection if playing a cinematic and we are not immersive
	if (vr.cin_camera && !vr.immersive_cinematics)
	{
		return qfalse;
	}

	fov = gAppState.Views[vr.eye].fov;

	*tanLeft  = tanf(fov.angleLeft);   // negative
	*tanRight = tanf(fov.angleRight);  // positive
	*tanUp    = tanf(fov.angleUp);     // positive
	*tanDown  = tanf(fov.angleDown);   // negative

	return qtrue;
}

// Signed lateral eye offset (Quake units, + = along the view LEFT axis) for
// stereo parallax.  Half the inter-pupillary distance taken from the actual
// OpenXR eye poses, scaled to world units by vr_worldscale.  Eye 0 (left) shifts
// left (+), eye 1 (right) shifts right (-).  The engine applies this along the
// rendered refdef viewaxis[1] in SPCG_R_RENDERSCENE, so head roll is handled
// automatically and per-eye display canting is covered by the asymmetric
// projection.
float VR_GetEyeStereoSeparation(int eye)
{
	XrVector3f *l, *r;
	float dx, dy, dz, ipd, half;

	if (!gAppState.SessionActive || gAppState.Views == NULL)
	{
		return 0.0f;
	}

	l = &gAppState.Views[0].pose.position;
	r = &gAppState.Views[1].pose.position;
	dx = r->x - l->x;
	dy = r->y - l->y;
	dz = r->z - l->z;
	ipd = sqrtf(dx * dx + dy * dy + dz * dz);          // metres

	half = 0.5f * ipd * vr_worldscale->value;          // -> Quake units
	return (eye == 0) ? half : -half;
}

int VR_SetRefreshRate(int refreshRate)
{
	return 0;
}

//All the stuff we want to do each frame specifically for this game
void VR_FrameSetup()
{
	static float refresh = 0;
	if (refresh != vr_refresh->value)
	{
		refresh = vr_refresh->value;
		VR_SetRefreshRate(vr_refresh->value);
	}

	//get any cvar values required here
	vr.immersive_cinematics = (vr_immersive_cinematics->value != 0.0f);
}


/*
================================================================================

Engine glue used by TBXR_Common.c (keeps that file engine-agnostic)

================================================================================
*/

void EFXR_GetScreenResolution(int *width, int *height)
{
	*width = cls.glconfig.vidWidth;
	*height = cls.glconfig.vidHeight;
}

void EFXR_SwapWindow()
{
	// Wired into the GL backend in a later milestone; no-op for M1.
}


/*
================================================================================

VR lifecycle

================================================================================
*/

static qboolean vrInitialised = qfalse;
static qboolean vrPreInited = qfalse;

qboolean VR_IsActive()
{
	return vrInitialised;
}

// Phase 1 -- called BEFORE the GL window/context is created (from CL_InitRenderer
// just before re.BeginRegistration).  The XR instance/system/resolution queries
// need no GL context, so we do them here and force the engine's render
// resolution to the headset's per-eye recommended size via r_customwidth/height
// + r_mode -1.  Otherwise the engine would render at the desktop window size into
// the (larger) per-eye swapchain textures, and the image would appear squashed
// into the bottom-left corner of each eye.  Honours vr_enable.
qboolean VR_PreRendererInit()
{
	cvar_t *vr_enable;

	if (vrPreInited)
	{
		return (qboolean)(gAppState.Instance != XR_NULL_HANDLE);
	}

	vr_enable = Cvar_Get("vr_enable", "1", CVAR_ARCHIVE | CVAR_LATCH);
	if (!vr_enable->integer)
	{
		Com_Printf("VR: vr_enable is 0 -- running flat-screen.\n");
		return qfalse;
	}

	vrPreInited = qtrue;

	// Instance + system + per-eye resolution (no GL needed yet).
	TBXR_InitialiseOpenXR();
	if (gAppState.Instance == XR_NULL_HANDLE)
	{
		Com_Printf("VR: OpenXR unavailable -- running flat-screen.\n");
		return qfalse;
	}

	// Apply a render scale to the OpenXR-recommended per-eye size.  ioEF uses the
	// fixed-function OpenGL 1.x renderer (not a modern GPU renderer like the
	// RealRTCWXR rend2 reference), so rendering two full recommended-size eyes
	// (e.g. 2528x2704) per frame is far too heavy -- it drops to a few fps and
	// the (window-coupled) mirror becomes larger than the desktop.  vr_supersample
	// scales BOTH the eye swapchain and the engine render resolution together, so
	// the viewport still exactly fills each eye texture (no squashing).  Raise it
	// toward 1.0+ for higher fidelity (e.g. release builds / faster GPUs).
	{
		cvar_t *vr_supersample = Cvar_Get("vr_supersample", "0.75", CVAR_ARCHIVE | CVAR_LATCH);
		float ss = vr_supersample->value;
		if (ss < 0.25f) ss = 0.25f;
		if (ss > 2.0f)  ss = 2.0f;
		gAppState.Width  = (float)((int)(gAppState.Width  * ss));
		gAppState.Height = (float)((int)(gAppState.Height * ss));
	}

	// NB: gAppState.Width/Height are float; cast for the %d cvar/print.
	Cvar_Set("r_customwidth", va("%d", (int)gAppState.Width));
	Cvar_Set("r_customheight", va("%d", (int)gAppState.Height));
	Cvar_Set("r_mode", "-1");
	Com_Printf("VR: rendering at per-eye resolution %dx%d (vr_supersample applied)\n",
			   (int)gAppState.Width, (int)gAppState.Height);
	return qtrue;
}

// Phase 2 -- idempotent, called once the GL context exists and is current (from
// CL_InitRenderer, after re.BeginRegistration).  Creates the session/swapchains.
void VR_InitOnce()
{
	if (vrInitialised)
	{
		return;
	}

	if (!vrPreInited || gAppState.Instance == XR_NULL_HANDLE)
	{
		// VR_PreRendererInit didn't run or OpenXR is unavailable -> stay flat.
		return;
	}

	if (VR_Init())
	{
		vrInitialised = qtrue;
		Com_Printf("VR: OpenXR session active.\n");
	}
	else
	{
		vrInitialised = qfalse;
		Com_Printf("VR: OpenXR initialisation failed -- running flat-screen.\n");
	}
}

void VR_Shutdown()
{
	if (!vrInitialised)
	{
		return;
	}
	TBXR_LeaveVR();
	vrInitialised = qfalse;
}

// Phase 2 of VR bring-up (the OpenXR instance/system/resolution were already
// created in VR_PreRendererInit before the GL window existed).  Now that the GL
// context is current we can load the GL FBO extensions and create the session +
// per-eye swapchains.
qboolean VR_Init()
{
	if (gAppState.Instance == XR_NULL_HANDLE)
	{
		return qfalse;
	}

	GlInitExtensions();

	TBXR_EnterVR();
	TBXR_InitRenderer();
	TBXR_InitActions();
	TBXR_WaitForSessionActive();

	//Initialise all our variables
	vr.snapTurn = 0.0f;
	vr.immersive_cinematics = qtrue;
	vr.move_speed = 1; // Default to full speed now

	//init randomiser
	srand(time(NULL));

	//Create Cvars
	VR_InitCvars();

	vr.menu_right_handed = vr_control_scheme->integer == 0;

	return qtrue;
}


/*
================================================================================

Haptics / controller input -- stubbed for M1 (full input layer is a later
milestone).  Keeping the symbols here so TBXR_Common.c links.

================================================================================
*/

void VR_HapticEvent(const char* event, int position, int flags, int intensity, float angle, float yHeight )
{
}

void VR_HapticUpdateEvent(const char* event, int intensity, float angle )
{
}

void VR_HapticEndFrame()
{
}

void VR_HapticStopEvent(const char* event)
{
}

void VR_HapticEnable()
{
}

void VR_HapticDisable()
{
}

void VR_HandleControllerInput()
{
	// M1: controller input layer not yet ported.
}

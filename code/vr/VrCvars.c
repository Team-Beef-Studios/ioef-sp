/*
===========================================================================
VrCvars.c -- VR cvar definitions for the ioEF OpenXR VR port.

Ported/adapted from RealRTCWXR for the ioEF Elite Force VR port
(milestone M1).  Defines the M1 cvar subset and VR_InitCvars(), which
Cvar_Get()s them.  Not called from engine startup yet (M1 is dead code).
===========================================================================
*/

#include "VrCvars.h"

cvar_t	*vr_worldscale;
cvar_t	*vr_heightAdjust;
cvar_t	*vr_screen_dist;
cvar_t	*vr_immersive_cinematics;
cvar_t	*vr_control_scheme;
cvar_t	*vr_haptic_intensity;
cvar_t	*vr_refresh;

void VR_InitCvars(void)
{
	vr_worldscale            = Cvar_Get( "vr_worldscale", "32.0", CVAR_ARCHIVE );
	vr_heightAdjust          = Cvar_Get( "vr_heightAdjust", "0.0", CVAR_ARCHIVE );
	vr_screen_dist           = Cvar_Get( "vr_screen_dist", "3.5", CVAR_ARCHIVE );
	// 0 => scripted/ROQ cutscenes play on the flat virtual screen (Team Beef
	// default; comfortable).  1 => cutscenes render immersively in 3D.
	vr_immersive_cinematics  = Cvar_Get( "vr_immersive_cinematics", "0", CVAR_ARCHIVE );
	vr_control_scheme        = Cvar_Get( "vr_control_scheme", "0", CVAR_ARCHIVE );
	vr_haptic_intensity      = Cvar_Get( "vr_haptic_intensity", "1.0", CVAR_ARCHIVE );
	vr_refresh               = Cvar_Get( "vr_refresh", "72", CVAR_ARCHIVE );
}

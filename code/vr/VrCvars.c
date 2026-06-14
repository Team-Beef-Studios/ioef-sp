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
cvar_t	*vr_height_offset;
cvar_t	*vr_positional_factor;
cvar_t	*vr_screen_dist;
cvar_t	*vr_immersive_cinematics;
cvar_t	*vr_control_scheme;
cvar_t	*vr_haptic_intensity;
cvar_t	*vr_refresh;
cvar_t	*vr_turn_mode;
cvar_t	*vr_turn_angle;
cvar_t	*vr_switch_sticks;
cvar_t	*vr_weapon_pitchadjust;

void VR_InitCvars(void)
{
	vr_worldscale            = Cvar_Get( "vr_worldscale", "32.0", CVAR_ARCHIVE );
	// Extra eye height in METRES added to the real HMD height -- raise the view
	// for seated play (e.g. 0.6-0.9 to sit at standing height).  In-game eye
	// height = (real HMD height from the floor + vr_height_offset) * vr_worldscale.
	vr_height_offset         = Cvar_Get( "vr_height_offset", "0.0", CVAR_ARCHIVE );
	// 6DoF horizontal positional movement strength (lean/step -> walk).  Matches
	// RealRTCWXR (vr_positional_factor 12).
	vr_positional_factor     = Cvar_Get( "vr_positional_factor", "12.0", CVAR_ARCHIVE );
	vr_screen_dist           = Cvar_Get( "vr_screen_dist", "3.5", CVAR_ARCHIVE );
	// 0 => scripted/ROQ cutscenes play on the flat virtual screen (Team Beef
	// default; comfortable).  1 => cutscenes render immersively in 3D.
	vr_immersive_cinematics  = Cvar_Get( "vr_immersive_cinematics", "0", CVAR_ARCHIVE );
	// 0 = right-handed (weapon hand = right), 10 = left-handed.  Mirrors the
	// RealRTCWXR control_scheme enum so a future left-handed UI toggle is just
	// this cvar.
	vr_control_scheme        = Cvar_Get( "vr_control_scheme", "0", CVAR_ARCHIVE );
	vr_haptic_intensity      = Cvar_Get( "vr_haptic_intensity", "1.0", CVAR_ARCHIVE );
	vr_refresh               = Cvar_Get( "vr_refresh", "72", CVAR_ARCHIVE );
	// Turning: 0 = snap turn (discrete), 1 = smooth turn.
	vr_turn_mode             = Cvar_Get( "vr_turn_mode", "0", CVAR_ARCHIVE );
	// Snap-turn step (degrees); also the per-tick smooth-turn rate basis.
	vr_turn_angle            = Cvar_Get( "vr_turn_angle", "45", CVAR_ARCHIVE );
	// Swap which thumbstick moves vs turns (0 = move on dominant hand's off
	// stick, turn on dominant; 1 = swapped).  Matches RealRTCWXR.
	vr_switch_sticks         = Cvar_Get( "vr_switch_sticks", "0", CVAR_ARCHIVE );
	// Global controller aim pitch bias, matching JKXR's default.  The raw
	// controller pose remains in ANGLES_DEFAULT; weapons use ANGLES_ADJUSTED.
	vr_weapon_pitchadjust    = Cvar_Get( "vr_weapon_pitchadjust", "-20.0", CVAR_ARCHIVE );
}

/*
===========================================================================
VrCvars.h -- VR cvar declarations for the ioEF OpenXR VR port.

Ported/adapted from RealRTCWXR (code/RealRTCWXR/RealRTCWXR/VrCvars.h) for
the ioEF Elite Force VR port (milestone M1).  Trimmed to the M1 subset:
the comfort/world-scale cvars plus the handful actually referenced by
TBXR_Common.c / EFXR_SurfaceView.c.  RTCW-only cvars (turn, virtual-stock,
vehicle, gesture, weapon-align, trigger thresholds, ...) are dropped and can
be reinstated alongside the input layer in a later milestone.
===========================================================================
*/
#if !defined(vrcvars_h)
#define vrcvars_h

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

extern cvar_t	*vr_worldscale;
extern cvar_t	*vr_height_offset;
extern cvar_t	*vr_positional_factor;
extern cvar_t	*vr_screen_dist;
extern cvar_t	*vr_immersive_cinematics;
extern cvar_t	*vr_control_scheme;
extern cvar_t	*vr_haptic_intensity;
extern cvar_t	*vr_refresh;
extern cvar_t	*vr_turn_mode;
extern cvar_t	*vr_turn_angle;
extern cvar_t	*vr_switch_sticks;
extern cvar_t	*vr_weapon_pitchadjust;

void VR_InitCvars(void);

#endif //vrcvars_h

/************************************************************************************

Content		:	Handles common VR helper functionality and owns the global VR state.
Ported/adapted from RealRTCWXR (code/RealRTCWXR/RealRTCWXR/VrInputCommon.c)
for the ioEF Elite Force VR port (milestone M1).

This translation unit DEFINES the engine-side global `vr_client_info_t vr;`
(under -DEFXR_CLIENT, which the Makefile applies to client-binary objects only).
Math helpers are kept; the RealRTCW controller-button / touchscreen / movement
input helpers are dropped for M1 (the full input layer is a later milestone).

Created		:	September 2019
Authors		:	Simon Brown

*************************************************************************************/

#include "VrCommon.h"
#include "VrCvars.h"

#include "../qcommon/qcommon.h"
#include "../qcommon/q_shared.h"
#include "../client/client.h"   // CL_MouseEvent/CL_KeyEvent/Key_GetCatcher/K_MOUSE1 (menu pointer)

long long global_time;
int ducked;

vr_client_info_t vr;

extern ovrApp gAppState;

/* ----------------------------------------------------------------------------
   Controller state globals (filled by OpenXrInput.c TBXR_UpdateControllers,
   consumed by EFXR_SurfaceView.c VR_HandleControllerInput).  Ported from
   RealRTCWXR VrInputCommon.c.
   ---------------------------------------------------------------------------- */
ovrInputStateTrackedRemote leftTrackedRemoteState_old;
ovrInputStateTrackedRemote leftTrackedRemoteState_new;
ovrTrackedController        leftRemoteTracking_new;
ovrInputStateTrackedRemote rightTrackedRemoteState_old;
ovrInputStateTrackedRemote rightTrackedRemoteState_new;
ovrTrackedController        rightRemoteTracking_new;

/* Per-frame movement/turn outputs that the engine (cl_input.c) reads via the
   VR_GetControllerMove / VR_GetTurnDelta getters in EFXR_SurfaceView.c. */
float remote_movementSideways;
float remote_movementForward;


void rotateAboutOrigin(float x, float y, float rotation, vec2_t out)
{
    out[0] = cosf(DEG2RAD(-rotation)) * x  +  sinf(DEG2RAD(-rotation)) * y;
    out[1] = cosf(DEG2RAD(-rotation)) * y  -  sinf(DEG2RAD(-rotation)) * x;
}

float length(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

#define NLF_DEADZONE 0.1
#define NLF_POWER 2.2

float nonLinearFilter(float in)
{
    float val = 0.0f;
    if (in > NLF_DEADZONE)
    {
        val = in > 1.0f ? 1.0f : in;
        val -= NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = powf(val, NLF_POWER);
    }
    else if (in < -NLF_DEADZONE)
    {
        val = in < -1.0f ? -1.0f : in;
        val += NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = -powf(fabsf(val), NLF_POWER);
    }

    return val;
}

bool between(float min, float val, float max)
{
    return (min < val) && (val < max);
}


/* ============================================================================
   SHARED VR CONTROLLER INPUT (moved here from the per-platform EFXR_SurfaceView.c
   so a single copy drives BOTH the Android and Windows/PCVR builds).  Platform
   files keep only platform-specific glue (EGL/WGL swap, screen res, VR_Init).
   ============================================================================ */
/*
================================================================================

Controller input mapping  (movement / turn / shoot / jump / use / crouch)

Modeled on RealRTCWXR VrInputDefault.c HandleInput_Default, trimmed to the
focused ioEF scope (no saber/akimbo/gesture/weapon-align/binoculars/vehicle).
Handedness is driven entirely by vr_control_scheme (0 = right-handed, 10 =
left-handed) and vr_switch_sticks, exactly like RealRTCWXR, so a future full
left-handed mode is just the cvar.

The actual button/move state is exposed to the engine (cl_input.c CL_FinishMove)
via VR_GetControllerMove / VR_GetControllerButtons / VR_GetControllerUpMove /
VR_GetTurnDelta -- the engine ORs the buttons into the usercmd and adds the
turn delta to cl.viewangles[YAW] (same incremental model as the HMD yaw).

================================================================================
*/

/* EF game usercmd button bits (from Elite-Force-VR/game/q_shared.h -- the game
   DLL consumes these verbatim from usercmd_t.buttons).  The engine's own
   q_shared.h does not define BUTTON_USE / BUTTON_ALT_ATTACK, so we use the
   numeric values the game expects. */
#define EF_BUTTON_ATTACK        1
#define EF_BUTTON_USE_HOLDABLE  4    /* skips scripted cinematics (ClientCinematicThink) */
#define EF_BUTTON_USE           32
#define EF_BUTTON_ALT_ATTACK    128

/* EF SP weapon ids.  cl.snap.ps.weapon carries the SP value in sp_game mode;
   do not use ioquake3's stock Q3 weapon enum names here. */
#define EF_WP_PHASER              1
#define EF_WP_COMPRESSION_RIFLE   2
#define EF_WP_IMOD                3

/* Per-frame controller output, read by the engine via the getters below. */
static int   vr_controllerButtons = 0;      /* OR of EF_BUTTON_* this frame   */
static int   vr_controllerUpMove  = 0;      /* +127 jump / -127 crouch / 0    */
static float vr_turnDelta         = 0.0f;   /* yaw delta to apply this frame  */

/* ------------------------------------------------------------------------
   VR menu laser-pointer (ported from RealRTCWXR VrInputCommon.c).
   When a 2D menu/console is up it is drawn on a quad facing
   vr.hmdorientation_snap[YAW]; we map the pointing controller's yaw/pitch
   (relative to that snapped facing) to a normalised cursor and feed it to the
   UI as relative mouse deltas, with the trigger / face button as left-click.
   ------------------------------------------------------------------------ */

// Normalised (0..1) cursor -> engine virtual 640x480 UI space, fed as a RELATIVE
// mouse delta (the UI accumulates deltas; we track the previous absolute pos).
static void VR_MenuMouseAbs(float x, float y)
{
	static int ox = 0, oy = 0;
	int absx = (int)(x * 640.0f);
	int absy = (int)(y * 480.0f);
	CL_MouseEvent(absx - ox, absy - oy, 0);
	ox = absx;
	oy = absy;
}

// Map the pointing controller's aim angles (relative to the menu's facing yaw)
// to a cursor position.  -sin(dyaw) sweeps left/right; pitch/90 sweeps up/down.
static void VR_MenuPoint(float menuYaw, const vec3_t controllerAngles)
{
	float cursorX = -sinf(DEG2RAD(controllerAngles[YAW] - menuYaw)) + 0.5f;
	float cursorY = (controllerAngles[PITCH] / 90.0f) + 0.5f;
	VR_MenuMouseAbs(cursorX, cursorY);
}

// Edge-detected controller button -> engine key event (e.g. trigger -> K_MOUSE1).
static void VR_MenuButtonKey(const ovrInputStateTrackedRemote *cur,
                             const ovrInputStateTrackedRemote *prev,
                             uint32_t button, int key)
{
	if ((cur->Buttons & button) != (prev->Buttons & button))
	{
		CL_KeyEvent(key, (cur->Buttons & button) != 0, Sys_Milliseconds());
	}
}

static void VR_CacheControllerPoses(const ovrTrackedController *pDomTrack,
                                    const ovrTrackedController *pOffTrack)
{
	vec3_t zero = {0.0f, 0.0f, 0.0f};

	if (pDomTrack->Active)
	{
		VectorCopy(vr.weaponangles[ANGLES_ADJUSTED], vr.weaponangles_last[ANGLES_ADJUSTED]);
		QuatToYawPitchRoll(pDomTrack->Pose.orientation, zero, vr.weaponangles[ANGLES_DEFAULT]);
		zero[PITCH] = vr_weapon_pitchadjust->value;
		QuatToYawPitchRoll(pDomTrack->Pose.orientation, zero, vr.weaponangles[ANGLES_ADJUSTED]);
		zero[PITCH] = 0.0f;
		VectorSubtract(vr.weaponangles[ANGLES_ADJUSTED], vr.weaponangles_last[ANGLES_ADJUSTED],
			vr.weaponangles_delta[ANGLES_ADJUSTED]);

		VectorSet(vr.weaponposition,
			pDomTrack->Pose.position.x,
			pDomTrack->Pose.position.y,
			pDomTrack->Pose.position.z);
		VectorSubtract(vr.weaponposition, vr.hmdposition, vr.weaponoffset);

		for (int i = NUM_WEAPON_SAMPLES - 1; i > 0; --i)
		{
			VectorCopy(vr.weaponoffset_history[i - 1], vr.weaponoffset_history[i]);
			vr.weaponoffset_history_timestamp[i] = vr.weaponoffset_history_timestamp[i - 1];
		}
		VectorCopy(vr.weaponoffset, vr.weaponoffset_history[0]);
		vr.weaponoffset_timestamp = Sys_Milliseconds();
		vr.weaponoffset_history_timestamp[0] = vr.weaponoffset_timestamp;
	}

	if (pOffTrack->Active)
	{
		VectorCopy(vr.offhandangles[ANGLES_ADJUSTED], vr.offhandangles_last[ANGLES_ADJUSTED]);
		QuatToYawPitchRoll(pOffTrack->Pose.orientation, zero, vr.offhandangles[ANGLES_DEFAULT]);
		zero[PITCH] = vr_weapon_pitchadjust->value;
		QuatToYawPitchRoll(pOffTrack->Pose.orientation, zero, vr.offhandangles[ANGLES_ADJUSTED]);
		zero[PITCH] = 0.0f;
		VectorSubtract(vr.offhandangles[ANGLES_ADJUSTED], vr.offhandangles_last[ANGLES_ADJUSTED],
			vr.offhandangles_delta[ANGLES_ADJUSTED]);

		for (int i = 4; i > 0; --i)
		{
			VectorCopy(vr.offhandposition[i - 1], vr.offhandposition[i]);
		}
		VectorSet(vr.offhandposition[0],
			pOffTrack->Pose.position.x,
			pOffTrack->Pose.position.y,
			pOffTrack->Pose.position.z);
		VectorSubtract(vr.offhandposition[0], vr.hmdposition, vr.offhandoffset);
	}
}

static qboolean VR_WeaponAlignSupportedWeapon(int weapon)
{
	return weapon == EF_WP_PHASER ||
		weapon == EF_WP_COMPRESSION_RIFLE ||
		weapon == EF_WP_IMOD;
}

static const char *VR_WeaponAlignName(int weapon)
{
	switch (weapon)
	{
	case EF_WP_PHASER:
		return "Phaser";
	case EF_WP_COMPRESSION_RIFLE:
		return "Compression Rifle";
	case EF_WP_IMOD:
		return "IMOD";
	default:
		return "Unsupported";
	}
}

typedef struct {
	int weapon;
	vr_weapon_adjustment_t adjustment;
} vrWeaponAlignState_t;

static void VR_WeaponAlignCvarName(int weapon, char *buffer, int bufferSize)
{
	Com_sprintf(buffer, bufferSize, "vr_weapon_adjustment_%i", weapon);
}

static void VR_WeaponAlignLoad(vrWeaponAlignState_t *state, int weapon)
{
	char cvarName[64];
	cvar_t *cvar;

	memset(&state->adjustment, 0, sizeof(state->adjustment));
	state->adjustment.scale = 1.0f;
	state->weapon = weapon;

	if (!VR_WeaponAlignSupportedWeapon(weapon))
	{
		state->adjustment.loaded = qfalse;
		return;
	}

	VR_WeaponAlignCvarName(weapon, cvarName, sizeof(cvarName));
	cvar = Cvar_Get(cvarName, "1.0,0,0,0,0,0,0", CVAR_ARCHIVE);
	sscanf(cvar->string, "%f,%f,%f,%f,%f,%f,%f",
		&state->adjustment.scale,
		&state->adjustment.offset[0],
		&state->adjustment.offset[1],
		&state->adjustment.offset[2],
		&state->adjustment.angles[PITCH],
		&state->adjustment.angles[YAW],
		&state->adjustment.angles[ROLL]);
	state->adjustment.loaded = qtrue;
}

static void VR_WeaponAlignWrite(const vrWeaponAlignState_t *state)
{
	char cvarName[64];
	char buffer[256];

	if (!state->adjustment.loaded || !VR_WeaponAlignSupportedWeapon(state->weapon))
	{
		return;
	}

	VR_WeaponAlignCvarName(state->weapon, cvarName, sizeof(cvarName));
	Com_sprintf(buffer, sizeof(buffer), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
		state->adjustment.scale,
		state->adjustment.offset[0],
		state->adjustment.offset[1],
		state->adjustment.offset[2],
		state->adjustment.angles[PITCH],
		state->adjustment.angles[YAW],
		state->adjustment.angles[ROLL]);
	Cvar_Set(cvarName, buffer);
}

static void VR_WeaponAlignStatus(const vrWeaponAlignState_t *state, const char *itemName, float itemValue)
{
	char cvarName[64];

	if (!VR_WeaponAlignSupportedWeapon(state->weapon))
	{
		Com_sprintf(vr.weaponadjustment_info, sizeof(vr.weaponadjustment_info),
			"Weapon alignment: unsupported weapon %i", state->weapon);
		return;
	}

	VR_WeaponAlignCvarName(state->weapon, cvarName, sizeof(cvarName));
	Com_sprintf(vr.weaponadjustment_info, sizeof(vr.weaponadjustment_info),
		"%s (%s)  %s: %.3f",
		VR_WeaponAlignName(state->weapon),
		cvarName,
		itemName,
		itemValue);
}

static int vr_previousControlScheme = RIGHT_HANDED_DEFAULT;

static void VR_ExitWeaponAlignMode(void)
{
	if (vr_previousControlScheme != LEFT_HANDED_DEFAULT)
	{
		vr_previousControlScheme = RIGHT_HANDED_DEFAULT;
	}
	Cvar_Set("vr_control_scheme", va("%i", vr_previousControlScheme));
	vr.weaponadjustment_info[0] = '\0';
}

static void VR_HandleWeaponAlignInput(void)
{
	static vrWeaponAlignState_t state = { -1 };
	static int itemIndex = 0;
	static qboolean itemSwitched = qfalse;
	static float joyx[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	ovrInputStateTrackedRemote *pDom = &rightTrackedRemoteState_new;
	ovrInputStateTrackedRemote *pDomOld = &rightTrackedRemoteState_old;
	ovrInputStateTrackedRemote *pOff = &leftTrackedRemoteState_new;
	ovrInputStateTrackedRemote *pOffOld = &leftTrackedRemoteState_old;
	ovrTrackedController *pDomTrack = &rightRemoteTracking_new;
	ovrTrackedController *pOffTrack = &leftRemoteTracking_new;
	int weapon = cl.snap.ps.weapon;
	float *items[7];
	const char *itemNames[7] = { "scale", "right", "up", "forward", "pitch", "yaw", "roll" };
	const float itemIncrements[7] = { 0.005f, 0.02f, 0.02f, 0.02f, 0.1f, 0.1f, 0.1f };
	float primaryJoystickX = 0.0f;
	qboolean changed = qfalse;

	vr.right_handed = qtrue;
	VR_CacheControllerPoses(pDomTrack, pOffTrack);

	if (weapon != state.weapon)
	{
		VR_WeaponAlignLoad(&state, weapon);
	}

	items[0] = &state.adjustment.scale;
	items[1] = &state.adjustment.offset[0];
	items[2] = &state.adjustment.offset[1];
	items[3] = &state.adjustment.offset[2];
	items[4] = &state.adjustment.angles[PITCH];
	items[5] = &state.adjustment.angles[YAW];
	items[6] = &state.adjustment.angles[ROLL];

	if ((pOff->Buttons & xrButton_X) && !(pOffOld->Buttons & xrButton_X))
	{
		VR_ExitWeaponAlignMode();
		goto saveState;
	}

	if (!VR_WeaponAlignSupportedWeapon(weapon))
	{
		VR_WeaponAlignStatus(&state, itemNames[itemIndex], 0.0f);
		goto saveState;
	}

	for (int i = 3; i > 0; --i)
	{
		joyx[i] = joyx[i - 1];
	}
	joyx[0] = pDom->Joystick.x;
	for (int i = 0; i < 4; ++i)
	{
		primaryJoystickX += joyx[i];
	}
	primaryJoystickX *= 0.25f;

	if (between(-0.2f, pDom->Joystick.y, 0.2f) &&
		(between(0.8f, primaryJoystickX, 1.0f) ||
		 between(-1.0f, primaryJoystickX, -0.8f)))
	{
		if (!itemSwitched)
		{
			itemIndex += primaryJoystickX > 0.0f ? 1 : -1;
			if (itemIndex >= 7) itemIndex = 0;
			if (itemIndex < 0) itemIndex = 6;
			itemSwitched = qtrue;
		}
	}
	else
	{
		itemSwitched = qfalse;
	}

	if (((pDom->Buttons & xrButton_Joystick) != (pDomOld->Buttons & xrButton_Joystick)) &&
		(pDomOld->Buttons & xrButton_Joystick))
	{
		*items[itemIndex] = 0.0f;
		changed = qtrue;
	}

	if (between(-0.2f, primaryJoystickX, 0.2f))
	{
		if (pDom->Joystick.y > 0.6f)
		{
			*items[itemIndex] += itemIncrements[itemIndex];
			changed = qtrue;
		}
		else if (pDom->Joystick.y < -0.6f)
		{
			*items[itemIndex] -= itemIncrements[itemIndex];
			changed = qtrue;
		}
	}

	if (((pDom->Buttons & xrButton_A) != (pDomOld->Buttons & xrButton_A)) &&
		(pDomOld->Buttons & xrButton_A))
	{
		Cbuf_AddText("weapnext\n");
	}

	if (((pDom->Buttons & xrButton_B) != (pDomOld->Buttons & xrButton_B)) &&
		(pDomOld->Buttons & xrButton_B))
	{
		Cbuf_AddText("weapprev\n");
	}

	if (changed)
	{
		VR_WeaponAlignWrite(&state);
	}

	VR_WeaponAlignStatus(&state, itemNames[itemIndex], *items[itemIndex]);

saveState:
	rightTrackedRemoteState_old = rightTrackedRemoteState_new;
	leftTrackedRemoteState_old  = leftTrackedRemoteState_new;
}

void VR_HandleControllerInput()
{
	TBXR_UpdateControllers();

	// Reset per-frame outputs.
	vr_controllerButtons = 0;
	vr_controllerUpMove  = 0;
	vr_turnDelta         = 0.0f;

	if (vr_control_scheme->integer == WEAPON_ALIGN)
	{
		VR_HandleWeaponAlignInput();
		return;
	}

	// Ensure handedness flags are set (mirrors RealRTCWXR).  <10 = right-handed.
	vr.right_handed = vr_control_scheme->integer < 10;

	// Pick dominant (weapon) vs off hand from the control scheme, RealRTCWXR-style.
	ovrInputStateTrackedRemote *pDom, *pOff, *pDomOld, *pOffOld;
	ovrTrackedController       *pDomTrack;   // dominant-hand aim pose (menu pointer)
	ovrTrackedController       *pOffTrack;   // off-hand aim pose
	int domFace1, domFace2;   // dominant-hand face buttons (jump, use)
	int offFace1;             // off-hand face button 1 (hold = mission objectives)
	int offFace2;             // off-hand face button 2 (toggle in-game menu)
	if (vr_control_scheme->integer == LEFT_HANDED_DEFAULT)
	{
		pDom = &leftTrackedRemoteState_new;
		pDomOld = &leftTrackedRemoteState_old;
		pOff = &rightTrackedRemoteState_new;
		pOffOld = &rightTrackedRemoteState_old;
		pDomTrack = &leftRemoteTracking_new;
		pOffTrack = &rightRemoteTracking_new;
		domFace1 = xrButton_X;   // jump
		domFace2 = xrButton_Y;   // use
		offFace1 = xrButton_A;   // off-hand (right) primary -> mission info
		offFace2 = xrButton_B;   // off-hand (right) secondary -> menu
	}
	else
	{
		pDom = &rightTrackedRemoteState_new;
		pDomOld = &rightTrackedRemoteState_old;
		pOff = &leftTrackedRemoteState_new;
		pOffOld = &leftTrackedRemoteState_old;
		pDomTrack = &rightRemoteTracking_new;
		pOffTrack = &leftRemoteTracking_new;
		domFace1 = xrButton_A;   // jump
		domFace2 = xrButton_B;   // use
		offFace1 = xrButton_X;   // off-hand (left) primary -> mission info
		offFace2 = xrButton_Y;   // off-hand (left) secondary -> menu
	}

	// Cache the current dominant/off-hand aim poses for the SP modules.  The
	// positions remain in OpenXR metres; cgame/game convert them into EF world
	// units with vr.worldscale when placing weapons and muzzle traces.
	VR_CacheControllerPoses(pDomTrack, pOffTrack);

	if (vr_align_weapons->integer)
	{
		qboolean alignNow = (pOff->Buttons & offFace1) != 0;
		qboolean alignWas = (pOffOld->Buttons & offFace1) != 0;
		if (alignNow && !alignWas)
		{
			vr_previousControlScheme = vr_control_scheme->integer == LEFT_HANDED_DEFAULT ?
				LEFT_HANDED_DEFAULT : RIGHT_HANDED_DEFAULT;
			Cvar_Set("vr_control_scheme", va("%i", WEAPON_ALIGN));
			Com_sprintf(vr.weaponadjustment_info, sizeof(vr.weaponadjustment_info),
				"Weapon alignment mode");
			rightTrackedRemoteState_old = rightTrackedRemoteState_new;
			leftTrackedRemoteState_old  = leftTrackedRemoteState_new;
			return;
		}
	}

	// vr_switch_sticks swaps which stick moves vs turns (the move stick is
	// normally the OFF hand, the turn stick the DOMINANT hand -- RealRTCWXR).
	XrVector2f *pMoveStick;
	XrVector2f *pTurnStick;
	if (vr_switch_sticks->integer)
	{
		pMoveStick = &pDom->Joystick;
		pTurnStick = &pOff->Joystick;
	}
	else
	{
		pMoveStick = &pOff->Joystick;
		pTurnStick = &pDom->Joystick;
	}

	// When a 2D menu or the console is up, the screen-layer quad is shown; drive
	// its cursor with the pointing controller (laser-pointer) instead of running
	// gameplay input.  (Console/UI catcher -- NOT cinematics, which still want the
	// gameplay branch so the A-button cutscene-skip works.)
	qboolean menuActive = (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CONSOLE)) != 0;
	if (menuActive)
	{
		// Aim the dominant controller at the menu quad (which faces
		// hmdorientation_snap[YAW]) and map yaw/pitch to the UI cursor.
		vec3_t zero = {0.0f, 0.0f, 0.0f};
		vec3_t aimAngles;
		QuatToYawPitchRoll(pDomTrack->Pose.orientation, zero, aimAngles);
		VR_MenuPoint(vr.hmdorientation_snap[YAW], aimAngles);

		// Click: dominant trigger or face button 1 -> left mouse button.
		VR_MenuButtonKey(pDom, pDomOld, xrButton_Trigger, K_MOUSE1);
		VR_MenuButtonKey(pDom, pDomOld, domFace1,         K_MOUSE1);
		// Menu/back button (either hand) -> Escape (back out / close the menu).
		VR_MenuButtonKey(&leftTrackedRemoteState_new,  &leftTrackedRemoteState_old,  xrButton_Enter, K_ESCAPE);
		VR_MenuButtonKey(&rightTrackedRemoteState_new, &rightTrackedRemoteState_old, xrButton_Enter, K_ESCAPE);
		// Off-hand face button 2 (Y right-handed / B left-handed) also closes the
		// menu -- SteamVR hijacks the system menu button, so this is the reliable
		// way to toggle our in-game menu.
		VR_MenuButtonKey(pOff, pOffOld, offFace2, K_ESCAPE);

		// Save state for edge detection next frame, then we're done.
		rightTrackedRemoteState_old = rightTrackedRemoteState_new;
		leftTrackedRemoteState_old  = leftTrackedRemoteState_new;
		return;
	}

	// ---- Movement: move-hand thumbstick -> forward/side (deadzone + filter) ----
	{
		float dist = length(pMoveStick->x, pMoveStick->y);
		float nlf  = nonLinearFilter(dist);
		float d    = (dist > 1.0f) ? dist : 1.0f;
		float x    = nlf * (pMoveStick->x / d);
		float y    = nlf * (pMoveStick->y / d);

		vr.player_moving = (fabs(x) + fabs(y)) > 0.05f;

		float speed = (vr.move_speed == 0 ? 0.75f : (vr.move_speed == 1 ? 1.0f : 0.5f));
		remote_movementSideways = x * speed;
		remote_movementForward  = y * speed;
	}

	// ---- Turn: turn-hand thumbstick X -> yaw (snap or smooth) ----
	{
		float turnX = pTurnStick->x;
		bool usingSnapTurn = (vr_turn_mode->integer == 0);

		static qboolean snapReady = qtrue;  // re-arm when stick returns to centre
		if (usingSnapTurn)
		{
			if (turnX > 0.7f)
			{
				if (snapReady)
				{
					// stick right -> turn right (yaw decreases in Quake)
					vr_turnDelta -= vr_turn_angle->value;
					snapReady = qfalse;
				}
			}
			else if (turnX < -0.7f)
			{
				if (snapReady)
				{
					vr_turnDelta += vr_turn_angle->value;
					snapReady = qfalse;
				}
			}
			else if (turnX > -0.3f && turnX < 0.3f)
			{
				snapReady = qtrue;
			}
		}
		else if (fabs(turnX) > 0.1f) // smooth turn
		{
			vr_turnDelta -= (vr_turn_angle->value / 10.0f) * turnX;
		}

		// Keep snapTurn in the shared struct in sync (cgame may read it).
		vr.snapTurn += vr_turnDelta;
		while (vr.snapTurn >  180.0f) vr.snapTurn -= 360.0f;
		while (vr.snapTurn < -180.0f) vr.snapTurn += 360.0f;
	}

	// ---- Weapon switch: turn-hand (primary) thumbstick Y -> next/prev weapon ----
	// Temporary mapping until a weapon wheel.  Turning only uses the stick's X,
	// so its Y is free: push UP = weapnext, DOWN = weapprev.  Edge-detected (one
	// switch per flick; re-arms when the stick returns toward centre), same model
	// as snap-turn.  Routed through the cgame's existing weapnext/weapprev cmds.
	{
		float wy = pTurnStick->y;
		static qboolean weapReady = qtrue;
		if (wy > 0.7f)
		{
			if (weapReady) { Cbuf_AddText("weapnext\n"); weapReady = qfalse; }
		}
		else if (wy < -0.7f)
		{
			if (weapReady) { Cbuf_AddText("weapprev\n"); weapReady = qfalse; }
		}
		else if (wy > -0.3f && wy < 0.3f)
		{
			weapReady = qtrue;
		}
	}

	// ---- Buttons ----
	// Shoot: dominant-hand trigger.
	if (pDom->Buttons & xrButton_Trigger)
	{
		vr_controllerButtons |= EF_BUTTON_ATTACK;
	}

	// Secondary / alt fire: off-hand trigger.  (Dominant trigger = primary fire,
	// off-hand trigger = alt fire -- the off-hand trigger is otherwise unused.)
	if (pOff->Buttons & xrButton_Trigger)
	{
		vr_controllerButtons |= EF_BUTTON_ALT_ATTACK;
	}

	// Dominant face button 1 (A right / X left):
	//  - during a scripted cinematic -> BUTTON_USE_HOLDABLE, which the game's
	//    ClientCinematicThink treats as "skip the cutscene" (a fresh press
	//    toggles the skip/fast-forward);
	//  - otherwise -> jump (upmove +127).
	if (pDom->Buttons & domFace1)
	{
		if (vr.cin_camera)
			vr_controllerButtons |= EF_BUTTON_USE_HOLDABLE;
		else
			vr_controllerUpMove = 127;
	}

	// Use: dominant face button 2 (B right / Y left) -> BUTTON_USE.
	if (pDom->Buttons & domFace2)
	{
		vr_controllerButtons |= EF_BUTTON_USE;
	}

	// Crouch: off-hand grip (squeeze) -> upmove -127.  Takes priority over jump
	// only if jump isn't pressed (jump already set upmove to +127 above).
	if ((pOff->Buttons & xrButton_GripTrigger) && vr_controllerUpMove == 0)
	{
		vr_controllerUpMove = -127;
	}

	// Mission objectives: HOLD the off-hand face button 1 to show the mission-info
	// screen -- the cgame '+info'/'-info' commands the desktop binds to a key
	// (shows while held).  Edge-detected so we issue each command once.
	{
		qboolean infoNow = (pOff->Buttons & offFace1) != 0;
		qboolean infoWas = (pOffOld->Buttons & offFace1) != 0;
		if (infoNow && !infoWas)      Cbuf_AddText("+info\n");
		else if (!infoNow && infoWas) Cbuf_AddText("-info\n");
	}

	// Menu button (the '|||' / menu button -- left controller on Touch) toggles
	// the in-game menu so the player can Save / Load / Quit.  We inject an Escape
	// key event on the press edge (a momentary tap: the down toggles the menu, the
	// up is a no-op for Escape).  Checked on BOTH hands so it works whichever
	// controller carries the menu button across vendors.
	{
		// The dedicated menu button (xrButton_Enter, either hand) OR the off-hand
		// face button 2 (Y right-handed / B left-handed) opens the menu.  The Y
		// alias exists because SteamVR hijacks the physical menu button for its
		// own dashboard, leaving our in-game menu otherwise unreachable there.
		qboolean menuNow = (((leftTrackedRemoteState_new.Buttons | rightTrackedRemoteState_new.Buttons) & xrButton_Enter) != 0)
			|| ((pOff->Buttons & offFace2) != 0);
		qboolean menuWas = (((leftTrackedRemoteState_old.Buttons | rightTrackedRemoteState_old.Buttons) & xrButton_Enter) != 0)
			|| ((pOffOld->Buttons & offFace2) != 0);
		if (menuNow && !menuWas)
		{
			Com_QueueEvent(0, SE_KEY, K_ESCAPE, qtrue,  0, NULL);
			Com_QueueEvent(0, SE_KEY, K_ESCAPE, qfalse, 0, NULL);
		}
	}

	// HUD + gun toggle: click the LEFT thumbstick to hide both the 2D HUD and
	// the weapon viewmodel for an unobstructed view; click again to restore.
	// Always the physical left controller (independent of handedness, since the
	// click is separate from the stick's analog X/Y and never disturbs
	// movement/turn).  Edge-detected on the press; routed through the cgame's
	// cg_draw2D / cg_drawGun cvars (both CVAR_ARCHIVE, default 1).
	{
		qboolean hudNow = (leftTrackedRemoteState_new.Buttons & xrButton_LThumb) != 0;
		qboolean hudWas = (leftTrackedRemoteState_old.Buttons & xrButton_LThumb) != 0;
		if (hudNow && !hudWas)
		{
			static qboolean hudHidden = qfalse;
			hudHidden = !hudHidden;
			if (hudHidden)
				Cbuf_AddText("cg_draw2D 0; cg_drawGun 0\n");
			else
				Cbuf_AddText("cg_draw2D 1; cg_drawGun 1\n");
		}
	}

	// Save state for edge detection next frame (RealRTCWXR pattern).
	rightTrackedRemoteState_old = rightTrackedRemoteState_new;
	leftTrackedRemoteState_old  = leftTrackedRemoteState_new;
}

/* ---- engine-facing getters (called from cl_input.c CL_FinishMove) ---- */

// Thumbstick movement for this frame (-1..1-ish); engine scales by 127.
void VR_GetControllerMove(float *forward, float *side)
{
	*forward = remote_movementForward;
	*side    = remote_movementSideways;
}

// OR of EF_BUTTON_* the engine should set on the usercmd this frame.
int VR_GetControllerButtons(void)
{
	return vr_controllerButtons;
}

// +127 jump / -127 crouch / 0 none -- engine writes to cmd->upmove.
int VR_GetControllerUpMove(void)
{
	return vr_controllerUpMove;
}

// Yaw delta (degrees) to add to cl.viewangles[YAW] this frame (snap/smooth turn).
float VR_GetTurnDelta(void)
{
	return vr_turnDelta;
}

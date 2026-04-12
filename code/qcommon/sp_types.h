/*
===========================================================================
sp_types.h -- struct definitions for EF1 singleplayer game module ABI

The SP game DLLs (efgamex86.dll, efuix86.dll) were compiled with a different
playerState_t, entityState_t, and snapshot_t layout than the ioEF engine.
These types define the SP side of the ABI so the engine can correctly
translate data across the boundary.

These MUST match the layout the DLLs were compiled with (Elite-Reinforce
game/q_shared.h and cgame/cg_public.h).
===========================================================================
*/

#ifndef SP_TYPES_H
#define SP_TYPES_H

#include "q_shared.h"

// ============================================================================
// SP entityState_t
//
// The SP version has extra fields vs ioEF:
//   modelindex3, legsAnimTimer, torsoAnimTimer, scale, pushVec
// Fields up to modelindex2 are at identical offsets. After that, the SP
// layout inserts modelindex3 before clientNum, shifting everything.
// ============================================================================

typedef struct {
	int		number;
	int		eType;
	int		eFlags;

	trajectory_t	pos;
	trajectory_t	apos;

	int		time;
	int		time2;

	vec3_t	origin;
	vec3_t	origin2;

	vec3_t	angles;
	vec3_t	angles2;

	int		otherEntityNum;
	int		otherEntityNum2;

	int		groundEntityNum;

	int		constantLight;
	int		loopSound;

	int		modelindex;
	int		modelindex2;
	int		modelindex3;		// SP-specific
	int		clientNum;
	int		frame;

	int		solid;

	int		event;
	int		eventParm;

	int		powerups;
	int		weapon;
	int		legsAnim;
	int		legsAnimTimer;		// SP-specific
	int		torsoAnim;
	int		torsoAnimTimer;		// SP-specific

	int		scale;				// SP-specific

	vec3_t	pushVec;			// SP-specific
} sp_entityState_t;

// ============================================================================
// SP playerState_t
//
// Differences from ioEF playerState_t:
//   - No introTime field
//   - Has leanofs (signed char) and friction (short) after gravity
//   - Has legsAnimTimer/torsoAnimTimer/scale in the anim section
//   - events[2] and eventParms[2] (not [4])
//   - Has externalEventTime
//   - No damageShieldCount
//   - ammo[4] (MAX_AMMO=4) instead of ammo[16] (MAX_WEAPONS=16)
//   - Has borgAdaptHits[32] (MAX_WEAPONS=32)
//   - Has pushVec, leanStopDebounceTime
//   - No entityEventSequence
// ============================================================================

typedef struct {
	int			commandTime;
	int			pm_type;
	int			bobCycle;
	int			pm_flags;
	int			pm_time;

	vec3_t		origin;
	vec3_t		velocity;
	int			weaponTime;
	int			rechargeTime;
	short		useTime;
	// 2 bytes padding (alignment to 4 for gravity)
	int			gravity;
	signed char	leanofs;
	// 1 byte padding (alignment to 2 for friction)
	short		friction;
	// 0 bytes padding (friction ends at 4-byte boundary)
	int			speed;
	int			delta_angles[3];

	int			groundEntityNum;
	int			legsAnim;
	int			legsAnimTimer;
	int			torsoAnim;
	int			torsoAnimTimer;
	int			scale;
	int			movementDir;

	int			eFlags;

	int			eventSequence;
	int			events[2];			// MAX_PS_EVENTS = 2
	int			eventParms[2];

	int			externalEvent;
	int			externalEventParm;
	int			externalEventTime;

	int			clientNum;
	int			weapon;
	int			weaponstate;

	vec3_t		viewangles;
	int			viewheight;

	int			damageEvent;
	int			damageYaw;
	int			damagePitch;
	int			damageCount;

	int			stats[16];			// MAX_STATS
	int			persistant[16];		// MAX_PERSISTANT
	int			powerups[16];		// MAX_POWERUPS
	int			ammo[4];			// MAX_AMMO = 4
	int			borgAdaptHits[32];	// MAX_WEAPONS = 32

	vec3_t		pushVec;

	// not communicated over the net at all
	int			ping;
	unsigned char	leanStopDebounceTime;
} sp_playerState_t;

// ============================================================================
// SP snapshot_t  (cgame API)
//
// Differs from ioEF snapshot_t:
//   - Has cmdNum field before ps
//   - Uses sp_playerState_t and sp_entityState_t (different sizes)
//   - Has numConfigstringChanges / configstringNum after entities
// ============================================================================

#define SP_MAX_ENTITIES_IN_SNAPSHOT	256

typedef struct {
	int				snapFlags;
	int				ping;

	int				serverTime;

	byte			areamask[MAX_MAP_AREA_BYTES];

	int				cmdNum;

	sp_playerState_t	ps;

	int				numEntities;
	sp_entityState_t	entities[SP_MAX_ENTITIES_IN_SNAPSHOT];

	int				numConfigstringChanges;
	int				configstringNum;

	int				numServerCommands;
	int				serverCommandSequence;
} sp_snapshot_t;

// ============================================================================
// SP gentity_t  (server-visible portion)
//
// The SP game module's gentity_t begins with sp_entityState_t, NOT the
// engine's entityState_t.  This struct must use sp_entityState_t so that
// field accesses after 's' (client, inuse, linked, etc.) land at the
// correct offsets.
// ============================================================================

typedef struct sp_gentity_s sp_gentity_t;
struct sp_gentity_s {
	sp_entityState_t	s;
	struct sp_gclient_s	*client;
	qboolean		inuse;
	qboolean		linked;
	int			svFlags;
	qboolean		bmodel;
	vec3_t			mins, maxs;
	int			contents;
	vec3_t			absmin, absmax;
	vec3_t			currentOrigin;
	vec3_t			currentAngles;
	sp_gentity_t		*owner;
	// Game-private data follows but engine doesn't need it
};

#endif /* SP_TYPES_H */

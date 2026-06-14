/*
===========================================================================
VrClientInfo.h -- the shared VR state contract for the ioEF OpenXR VR port.

This single struct (vr_client_info_t) is the bridge between the engine's
OpenXR layer (which owns the global instance) and the separate SP modules
(cgame/game/ui in efgamex86_64.dll / efuix86_64.dll, and the renderer DLL),
which receive a POINTER to it.  Modeled on the Team Beef RealRTCWXR pattern.

IMPORTANT: this file must be kept BYTE-IDENTICAL between this repo (ioef-sp)
and Elite-Force-VR, exactly like code/qcommon/sp_types.h.  Both the MinGW64
engine and the MSVC-x64 DLLs compile it; Win64 is a single C ABI so the
layout matches (re-validate sizes on arm64 for the standalone build).

It depends only on q_shared.h types (vec2_t/vec3_t/qboolean) -- NO OpenXR
headers -- so the DLLs never need the OpenXR SDK.

  -DEFXR_CLIENT  : engine client binary -> owns the global  `vr_client_info_t vr;`
  (not defined)  : module DLLs           -> receive a pointer `vr_client_info_t *vr;`
===========================================================================
*/
#if !defined(vr_client_info_h)
#define vr_client_info_h

#define VR_CLIENT_INFO_VERSION  1

/* Passed by a VR engine in CG_INIT arg2 so a VR-aware cgame only trusts the
   vr pointer (arg1) when paired with a VR engine.  "VRM1". */
#define VR_CGINIT_SENTINEL      0x56524d31

#define NUM_WEAPON_SAMPLES      10

#define ANGLES_DEFAULT          0
#define ANGLES_ADJUSTED         1
#define ANGLES_COUNT            2

#define ACTIVE_OFF_HAND         1
#define ACTIVE_WEAPON_HAND      2

typedef struct {
    qboolean    loaded;
    float       scale;
    vec3_t      angles;
    vec3_t      offset;
} vr_weapon_adjustment_t;

typedef struct {
    /* ---- camera / virtual-screen state (drives the flat quad layer) ---- */
    qboolean    cin_camera;             /* a scripted/ICARUS cinematic camera has taken over */
    qboolean    misc_camera;            /* looking through a misc camera-view entity */
    qboolean    using_screen_layer;     /* this frame is being shown on the flat virtual screen */
    int         eye;                    /* the eye currently being rendered (0 = left, 1 = right) */
    qboolean    immersive_cinematics;   /* user opted to view cutscenes immersively, not on the screen */

    qboolean    third_person;
    float       fov_x;
    float       fov_y;
    float       off_center_fov_x[2];    /* per-eye asymmetric FOV centre offset */
    float       off_center_fov_y[2];

    /* ---- 6DoF scale config (engine caches these from cvars each frame) ---- */
    float       worldscale;             /* Quake units per real-world metre (vr_worldscale) */
    float       height_offset;          /* extra eye height in metres, for seated play (vr_height_offset) */

    /* ---- comfort / control configuration (cached from cvars) ---- */
    qboolean    right_handed;
    qboolean    menu_right_handed;
    int         move_speed;             /* 0 = comfortable, 1 = full, 2 = walk */
    qboolean    player_moving;
    qboolean    crouched;
    qboolean    weapon_stabilised;      /* two-handed (virtual stock) hold */
    qboolean    scopeactive;            /* aiming through a scope/zoom */

    qboolean    take_snap;

    /* ---- HMD pose (world/room space, from xrLocateViews / view space) ---- */
    vec3_t      hmdposition;
    vec3_t      hmdposition_last;        /* internal: for delta calc only */
    vec3_t      hmdposition_delta;
    vec3_t      hmdposition_snap;        /* HMD position last time the menu came up */
    vec3_t      hmdposition_offset;

    vec3_t      hmdorientation;          /* pitch/yaw/roll, degrees */
    vec3_t      hmdorientation_last;     /* internal: for delta calc only */
    vec3_t      hmdorientation_delta;
    vec3_t      hmdorientation_snap;
    vec3_t      hmdorientation_first;    /* only updated while in first person */

    /* ---- gameplay view yaw (the cgame uses this as the base heading) ---- */
    vec3_t      clientviewangles;
    float       snapTurn;                /* yaw applied by stick turn */
    float       clientview_yaw_last;     /* internal: for delta calc only */
    float       clientview_yaw_delta;

    /* ---- weapon-hand controller pose (M2) ---- */
    vec3_t      weaponangles[ANGLES_COUNT];
    vec3_t      weaponangles_last[ANGLES_COUNT];
    vec3_t      weaponangles_delta[ANGLES_COUNT];
    vec3_t      weaponangles_first[ANGLES_COUNT];

    vec3_t      weaponposition;
    vec3_t      weaponoffset;
    float       weaponoffset_timestamp;
    vec3_t      weaponoffset_history[NUM_WEAPON_SAMPLES];
    float       weaponoffset_history_timestamp[NUM_WEAPON_SAMPLES];

    vec3_t      muzzlebounce;

    /* ---- off-hand controller pose (M2) ---- */
    vec3_t      offhandangles[ANGLES_COUNT];
    vec3_t      offhandangles_last[ANGLES_COUNT];
    vec3_t      offhandangles_delta[ANGLES_COUNT];

    vec3_t      offhandposition[5];      /* last 5 positions */
    vec3_t      offhandoffset;

    /* ---- gesture / item interaction (M2/M3) ---- */
    int         item_selector;
    qboolean    use_item;
    int         useGestureState;

    /* ---- velocity-triggered melee (M2) ---- */
    qboolean    velocitytriggered;
    qboolean    velocitytriggeractive;
    float       primaryswingvelocity;
    qboolean    primaryVelocityTriggeredAttack;

    float       maxHeight;
    float       curHeight;

    /* ---- weapon-alignment test mode (M2/M3) ---- */
    char        weaponadjustment_info[256];
    char        test_name[256];
    float       test_scale;
    vec3_t      test_angles;
    vec3_t      test_offset;
} vr_client_info_t;

#ifndef EFXR_CLIENT
extern vr_client_info_t *vr;     /* module DLLs (cgame/game/ui/renderer): pointer */
#else
extern vr_client_info_t  vr;     /* engine client binary: the global instance */
#endif

#endif /* vr_client_info_h */

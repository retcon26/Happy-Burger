#include "game.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "achieve.h"
#include "audio.h"
#include "builds.h"
#include "camera.h"
#include "pak.h"
#include "workshop.h"

#include "ents.h"
#include "gfx.h"
#include "mem.h"
#include "menu.h"
#include "nav.h"
#include "net.h"
#include "npc.h"
#include "vmodel.h"
#include "vehicle.h"
#include "voice.h"
#include "physics.h"
#include "proplib.h"
#include "propx.h"
#include "script.h"
#include "settings.h"
#include "sky.h"
#include "ui.h"

#define DEG2RAD (3.14159265f / 180.0f)
#define PI 3.14159265f

#ifdef PS2
// Frame timings for the debug overlay: map, props, npcs (game_render) and update (main.c), in ms
float g_ps2_prof[4];
void gu_profile_text(char *buf, int len);
#endif
static int g_flashlight; // Square toggles it on the PS2, holding D-pad Right on the PSP
// A dedicated server (retcons-mod-server): it hosts, but nobody plays on it. Its own player stays out of
// the game (noclip, not sent, not a target) and notes go to its log.
static int g_dedicated;
static void (*g_log)(const char *msg);

// The PSP plays over ad-hoc wifi, the PS2 over the network adapter
#ifdef PS2
#define NET_HOSTING_HINT "Hosting - others can join over the LAN"
#define NET_LOST_MSG "Lost the network connection"
#define NET_STARTING_MSG "Starting the network..."
#define NET_HOSTING_TAG "HOSTING (LAN)"
#define RAGDOLL_LOCAL_MSG "Ragdoll (only on this console)"
#else
#define NET_HOSTING_HINT \
    (net_mode() == NET_MODE_INET ? "Hosting - others can join over Wi-Fi" : "Hosting - others can join over ad-hoc")
#define NET_LOST_MSG (net_mode() == NET_MODE_INET ? "Lost the Wi-Fi connection" : "Lost the ad-hoc connection")
#define NET_STARTING_MSG (net_mode() == NET_MODE_INET ? "Connecting to Wi-Fi..." : "Starting ad-hoc...")
#define NET_HOSTING_TAG (net_mode() == NET_MODE_INET ? "HOSTING (WI-FI)" : "HOSTING (AD-HOC)")
#define RAGDOLL_LOCAL_MSG "Ragdoll (only on this PSP)"
#endif
#ifndef PS2
static float g_flash_hold; // how long D-pad Right has been held
static int g_flash_fired;
#endif

#define LOOK_YAW_SPEED 150.0f  // deg/s at full speed
#define LOOK_PITCH_SPEED 110.0f
#define LOOK_RAMP_TIME 0.30f   // seconds to reach full look speed (fine aiming on a tap)
#define LOOK_MIN_FRACTION 0.30f

#define USE_RANGE 4096.0f
#define SPAWN_RANGE 400.0f
#define MAX_SPAWNABLE 4096 // spawn menu entries per spawnlist
// Spawn menu grid entries past the prop model numbers: NPCs and ragdolls by NPC type
#define GRID_NPC 100000
#define GRID_RAG 200000
#define GRID_WEAPON 300000 // spawn menu grid entries: GRID_WEAPON + Weapon
#define RECENT_MAX 12   // the spawn menu's Recent list
#define RECENT_CAT (-2) // G.cat for it
#define PHYSGUN_MIN_DIST 32.0f
#define PHYSGUN_MAX_DIST 2000.0f
#define PHYSGUN_PULL 14.0f
#define PHYSGUN_TURN 10.0f
#define BALLOON_FORCE 30000.0f // mass * acceleration: lifts about 50kg against 600 gravity
#define MAX_BALLOONS 4
#define UNDO_MAX 64
#define UNDO_GROUP_BASE 0x100000 // an undo entry for the UNDO_GROUP_BASE + n entries below it (a loaded build)
#define PLAYER_PROP_LIMIT 40
#define STEP_STRIDE 72.0f
#define IMPACT_MIN_SPEED 140.0f
#define PROP_DRAW_DIST_HIGH 2400.0f
#define PROP_DRAW_DIST_LOW 1500.0f

typedef enum {
    WEAPON_HANDS,
    WEAPON_PHYSGUN,
    WEAPON_GRAVGUN,
    WEAPON_TOOLGUN,
    WEAPON_CROWBAR, // the guns from here on (is_gun)
    WEAPON_PISTOL,
    WEAPON_357,
    WEAPON_SMG,
    WEAPON_SHOTGUN,
    WEAPON_AR2,
    WEAPON_CROSSBOW, // from here on they fire things that fly (is_launcher): the host's projectiles
    WEAPON_RPG,
    WEAPON_GRENADE,
    WEAPON_BUGBAIT,
    WEAPON_COUNT
} Weapon;
typedef enum {
    TOOL_REMOVER,
    TOOL_COLOUR,
    TOOL_BALLOON,
    TOOL_DUPLICATOR,
    TOOL_WELD,
    TOOL_ROPE,
    TOOL_BALLSOCKET,
    TOOL_ELASTIC,
    TOOL_NOCOLLIDE,
    TOOL_WEIGHT,
    TOOL_THRUSTER,
    TOOL_HOVERBALL,
    TOOL_EXPLOSIVE,
    TOOL_SLIDER,
    TOOL_INFLATOR,
    TOOL_DEFLATOR,
    TOOL_CAMERA,
    TOOL_WHEEL,
    TOOL_WINCH,
    TOOL_HYDRAULIC,
    TOOL_LAMP,
    TOOL_LIGHT,
    TOOL_BUTTON,
    TOOL_EMITTER,
    TOOL_FACEPOSER,
    TOOL_FINGERPOSER,
    TOOL_COUNT
} Tool;

static const char *k_weapon_names[WEAPON_COUNT] = {"Hands", "Physics Gun", "Gravity Gun", "Tool Gun", "Crowbar",
                                                   "Pistol", ".357 Magnum", "SMG", "Shotgun", "AR2",
                                                   "Crossbow", "RPG", "Grenade", "Bug Bait"};
// World models (the old way of showing a weapon), then first-person models with arms (c_ models)
static const char *k_weapon_models[WEAPON_COUNT] = {NULL, "_physgun", "_physgun", "_toolgun", "_crowbar",
                                                    "_pistol", "_357", "_smg1", "_shotgun", "_ar2",
                                                    "_crossbow", "_rpg", "_grenade", "_bugbait"};
static const char *k_weapon_vms[WEAPON_COUNT] = {NULL, "_vm_physgun", "_vm_gravgun", "_vm_toolgun", "_vm_crowbar",
                                                 "_vm_pistol", "_vm_357", "_vm_smg1", "_vm_shotgun", "_vm_ar2",
                                                 "_vm_crossbow", "_vm_rpg", "_vm_grenade", "_vm_bugbait"};
// Source draws first-person models with their own field of view (viewmodel_fov 54, horizontal at 4:3)
#define VM_FOV_Y 41.4f

// Gravity gun (HL2's): L pulls in and holds a prop, R punts. Hands: R picks up something light, L throws it.
#define GRAVGUN_MAX_MASS 250.0f
#define GRAVGUN_PULL_RANGE 1000.0f
#define GRAVGUN_PUNT_RANGE 250.0f
#define GRAVGUN_PUNT_SPEED 1400.0f // for a 50 kg prop; lighter ones fly faster
#define HANDS_MAX_MASS 35.0f
#define HANDS_RANGE 96.0f
#define HANDS_THROW_SPEED 500.0f

// HL2's weapons, roughly as they handle in GMod
typedef struct {
    int clip, ammo;       // magazine size and starting spare ammo (0, 0: melee)
    float delay;          // seconds between shots
    int automatic;        // keeps firing while R is held
    int pellets;          // bullets per shot
    float damage, spread; // per bullet; spread is the cone's tangent
    float range;
    float force;          // how hard a bullet shoves what it hits (u/s for a 50 kg prop)
    float reload;         // seconds
    float kick;           // view punch, degrees
    const char *fire_snd;
} WeaponStats;
static const WeaponStats k_ws[WEAPON_COUNT] = {
    [WEAPON_CROWBAR] = {0, 0, 0.45f, 1, 1, 25.0f, 0.0f, 72.0f, 300.0f, 0.0f, 1.5f, "crowbar_swing"},
    [WEAPON_PISTOL] = {18, 150, 0.14f, 0, 1, 12.0f, 0.012f, 4096.0f, 220.0f, 1.4f, 1.2f, "pistol_fire"},
    [WEAPON_357] = {6, 36, 0.75f, 0, 1, 75.0f, 0.004f, 4096.0f, 900.0f, 3.0f, 5.0f, "357_fire"},
    [WEAPON_SMG] = {45, 270, 0.075f, 1, 1, 8.0f, 0.035f, 4096.0f, 160.0f, 1.6f, 0.7f, "smg1_fire"},
    [WEAPON_SHOTGUN] = {6, 34, 0.85f, 0, 7, 9.0f, 0.07f, 2048.0f, 180.0f, 2.6f, 4.0f, "shotgun_fire"},
    [WEAPON_AR2] = {30, 90, 0.1f, 1, 1, 11.0f, 0.025f, 4096.0f, 200.0f, 1.6f, 1.0f, "ar2_fire"},
    // The rest fire projectiles: their damage is the projectile's own (see Projectiles)
    [WEAPON_CROSSBOW] = {1, 10, 0.4f, 0, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.8f, 2.5f, "crossbow_fire"},
    [WEAPON_RPG] = {1, 2, 1.0f, 0, 1, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 4.0f, "rpg_fire"},
    [WEAPON_GRENADE] = {1, 4, 0.6f, 0, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f, 0.0f, "grenade_throw"},
    [WEAPON_BUGBAIT] = {0, 0, 0.6f, 0, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, "grenade_throw"}, // never runs out
};
static int is_gun(int w) {
    return w >= WEAPON_CROWBAR && w < WEAPON_COUNT;
}
static int is_launcher(int w) {
    return w >= WEAPON_CROSSBOW && w < WEAPON_COUNT;
}
static const char *k_tool_names[TOOL_COUNT] = {"Remover",     "Colour",  "Balloon",  "Duplicator", "Weld",
                                               "Rope",        "Ball Socket", "Elastic", "No-Collide", "Weight",
                                               "Thruster",    "Hoverball", "Explosive", "Slider",  "Inflator",   "Deflator", "Camera",
                                               "Wheel",       "Winch",     "Hydraulic", "Lamp",    "Light",      "Button",   "Emitter",
                                               "Face Poser",  "Finger Poser"};
// GMod-style tool descriptions: what it does, then what the trigger does
static const char *k_tool_desc[TOOL_COUNT] = {
    "Removes props", "Changes the colour of props", "Ties balloons to props", "Copies and pastes props",
    "Welds two props together", "Ties two points together with a rope",
    "Joins two props at a point they can swivel round", "Ties two points together with a stretchy band",
    "Makes a prop pass through other props", "Changes how heavy a prop is",
    "Puts a thruster on a prop (Use switches it)", "Holds a prop at its height (Use switches it)",
    "Blows a prop up", "Lets a prop slide along the line between two points",
    "Makes a part of a ragdoll bigger", "Makes a part of a ragdoll smaller",
    "Places cameras you can look through",
    "Puts a motorised wheel on a prop (Use switches it)", "Ties two points with a rope you can reel in (Use)",
    "Joins two points with a ram that pushes out (Use)", "Places a lamp that lights where it points (Use)",
    "Places a light in the colour you picked (Use)", "Places a button that switches a contraption",
    "Places an emitter of sparks, smoke or fire (Use)",
    "Changes a ragdoll's expression", "Poses a ragdoll's fingers",
};
static const char *k_tool_help[TOOL_COUNT] = {
    "R: Remove an object", "R: Apply the selected colour", "R: Add a balloon", "R: Copy a prop / paste it",
    "R: Pick a prop, then a second prop", "R: Pick a point, then a second point",
    "R: Pick a prop, then a second prop", "R: Pick a point, then a second point",
    "R: Switch a prop's collisions", "R: Next weight (1 to 1000 kg)",
    "R: Add a thruster that pushes into the surface", "R: Add a hoverball",
    "R: Explode a prop", "R: Pick a point, then a second point",
    "R: Inflate the ragdoll part you aim at", "R: Deflate the ragdoll part you aim at",
    "R: Place a camera    L: Switch view",
    "R: Add a wheel (its motor drives the way you face)", "R: Pick a point, then a second point",
    "R: Pick a point, then a second point", "R: Place a lamp (in the colour you picked)",
    "R: Place a light (in the colour you picked)", "R: Pick the prop it switches, then the spot",
    "R: Place an emitter    L: Change effect",
    "R: Pose the face you aim at    L: Next face", "R: Pose the hand you aim at    L: Next pose",
};
// The tools panel, grouped like GMod's
static const struct {
    const char *heading;
    Tool tools[8];
    int count;
} k_tool_groups[] = {
    {"Construction", {TOOL_BALLOON, TOOL_DUPLICATOR, TOOL_NOCOLLIDE, TOOL_REMOVER, TOOL_WEIGHT}, 5},
    {"Constraints", {TOOL_BALLSOCKET, TOOL_ELASTIC, TOOL_HYDRAULIC, TOOL_ROPE, TOOL_SLIDER, TOOL_WELD, TOOL_WINCH}, 7},
    {"Mechanics", {TOOL_BUTTON, TOOL_EMITTER, TOOL_EXPLOSIVE, TOOL_HOVERBALL, TOOL_THRUSTER, TOOL_WHEEL}, 6},
    {"Poser", {TOOL_FACEPOSER, TOOL_FINGERPOSER, TOOL_INFLATOR, TOOL_DEFLATOR}, 4},
    {"Render", {TOOL_CAMERA, TOOL_COLOUR, TOOL_LAMP, TOOL_LIGHT}, 4},
};
#define NUM_TOOL_GROUPS (int)(sizeof(k_tool_groups) / sizeof(k_tool_groups[0]))
// The Face Poser's expression and the Finger Poser's hand pose (npc_face_name / npc_hand_name; L changes them)
static int g_face_sel = 1, g_hand_sel = 1;
static const char *k_pause_items[] = {"Resume Game", "Options", "Saves", "Screenshot", "Toggle Noclip",
                                      "Freeze All Props", "Clean Up Props", "Disconnect"};
// Pause > Screenshot (game_set_screenshot): the next frame is drawn without the pause menu, then saved
static int (*g_shot)(const char *name, char *out, int out_len);
static int g_shot_pending;
// Quick chat (see Quick chat)
#define QC_SLICES 8
#define QC_HOLD 0.3f     // seconds on Down before the wheel opens (shorter is a tap: crouch)
#define QC_COOLDOWN 1.0f // between messages, so nobody floods the chat
#define QC_RADIUS 74.0f
static const char *const k_quick_msgs[QC_SLICES] = {"Hello!", "Thanks!", "Yes", "No",
                                                    "Follow me!", "Wait here", "Help!", "Look at this!"};
static struct {
    float hold, cooldown;
    int open, page, sel; // page 0 messages, 1 gestures; sel -1: nothing picked
    int tap;             // this frame Down was tapped (crouch)
    int test;            // opened by "qcwheel": stays up without Down held
    int gesture, gesture_seq; // this player's, for the others (NetPlayer)
} g_qc;

static void quick_chat_send(int page, int sel);

void game_set_screenshot(int (*save)(const char *name, char *out, int out_len)) {
    g_shot = save;
}
#define SELECT_HOLD_TIME 0.4f // holding Select this long toggles noclip instead of opening the spawn menu
#define NUM_PAUSE_ITEMS (int)(sizeof(k_pause_items) / sizeof(k_pause_items[0]))
enum { NOTE_GENERIC, NOTE_UNDO, NOTE_HINT, NOTE_ERROR, NOTE_CLEANUP, NOTE_CHAT, NOTE_ACHIEVEMENT };
#define MAX_NOTES 4
static const unsigned int k_palette[] = {
    0xFFFFFFFF, 0xFF4040FF, 0xFF40C040, 0xFFFF6040, 0xFF40E0FF, 0xFFFF60FF, 0xFF40A0FF, 0xFF606060,
};
#define PALETTE_SIZE (int)(sizeof(k_palette) / sizeof(k_palette[0]))
static const char *k_impact_sounds[] = {"impact_concrete", "impact_wood", "impact_metal", "impact_plastic",
                                        "impact_concrete", "impact_glass", "impact_cardboard", "impact_flesh",
                                        "impact_rubber"};
#define NUM_SURF (int)(sizeof(k_impact_sounds) / sizeof(k_impact_sounds[0]))

// How a world model is held as a view model. GMod's guns point their barrel along -Y with the grip
// at the origin; HL2's w_ models point along +X, so they're turned -90 degrees first.
typedef struct {
    float fwd, right, down; // grip position relative to the eye
    float length;           // grip to muzzle
    float turn;             // degrees about the model's up axis before holding it
    int centre;             // hold it by the middle of its bounds (HL2's models have their origin elsewhere)
    float pitch, roll;      // degrees: tip it up, and twist it about its length (the crowbar is held upright)
} ViewModel;
static const ViewModel k_vm[WEAPON_COUNT] = {
    [WEAPON_HANDS] = {20.0f, 0.0f, 10.0f, 10.0f, 0.0f, 0, 0.0f, 0.0f},
    [WEAPON_PHYSGUN] = {6.0f, 7.5f, 9.0f, 26.0f, 0.0f, 0, 0.0f, 0.0f},
    [WEAPON_GRAVGUN] = {6.0f, 7.5f, 9.0f, 26.0f, 0.0f, 0, 0.0f, 0.0f},
    [WEAPON_TOOLGUN] = {7.0f, 6.0f, 7.5f, 14.0f, 0.0f, 0, 0.0f, 0.0f},
    [WEAPON_CROWBAR] = {20.0f, 10.0f, 13.0f, 17.0f, 180.0f, 1, 60.0f, 90.0f},
    [WEAPON_PISTOL] = {15.0f, 4.5f, 6.5f, 6.0f, 180.0f, 1, 0.0f, 0.0f},
    [WEAPON_357] = {15.0f, 4.5f, 6.5f, 7.0f, 0.0f, 1, 0.0f, 0.0f},
    [WEAPON_SMG] = {16.0f, 5.0f, 7.0f, 9.0f, 180.0f, 1, 0.0f, 0.0f},
    [WEAPON_SHOTGUN] = {19.0f, 5.0f, 7.0f, 15.0f, 180.0f, 1, 0.0f, 0.0f},
    [WEAPON_AR2] = {17.0f, 5.0f, 7.0f, 12.0f, 0.0f, 1, 0.0f, 0.0f},
    [WEAPON_CROSSBOW] = {18.0f, 5.0f, 7.0f, 14.0f, 0.0f, 1, 0.0f, 0.0f},
    [WEAPON_RPG] = {14.0f, 6.0f, 6.0f, 20.0f, 0.0f, 1, 0.0f, 0.0f},
    [WEAPON_GRENADE] = {16.0f, 7.0f, 8.0f, 4.0f, 0.0f, 1, 0.0f, 0.0f},
    [WEAPON_BUGBAIT] = {16.0f, 7.0f, 8.0f, 4.0f, 0.0f, 1, 0.0f, 0.0f},
};

// Weapons that carry a prop the way the physics gun does
static int carries_props(int w) {
    return w == WEAPON_PHYSGUN || w == WEAPON_GRAVGUN || w == WEAPON_HANDS;
}

// --- Multiplayer types ---------------------------------------------------------------------------

enum { ROLE_OFFLINE, ROLE_HOST, ROLE_CLIENT };
enum { PF_NOCLIP = 1, PF_DUCKED = 2, PF_DEAD = 4, PF_GOD = 8, PF_VEHICLE = 16 }; // PF_GOD: the host has God mode on
// Actions clients ask the host to do
enum {
    CMD_SPAWN = 1,  // i0 model, f: pos[3] normal[3] rot[4], u tint
    CMD_SPAWN_NPC,  // i0 type, f: pos[3] yaw
    CMD_REMOVE,     // i0 body
    CMD_REMOVE_NPC, // i0 npc
    CMD_COLOUR,     // i0 body, u tint
    CMD_BALLOON,    // i0 body
    CMD_WELD,       // i0 a, i1 b, f: point[3]
    CMD_ROPE,       // i0 a, i1 b, f: pa[3] pb[3] length
    CMD_FREEZE,     // i0 body
    CMD_UNDO,
    CMD_USE,        // f: eye[3] dir[3]
    CMD_RESPAWN,
    CMD_DAMAGE,     // f0 amount (fall damage)
    CMD_SHOOT,      // i0 weapon, i1 spread seed, f: eye[3] dir[3]
    CMD_PUNT,       // i0 body, f: vel[3] (gravity gun punt, or a throw)
    CMD_BALLSOCKET, // i0 a, i1 b, f: point[3]
    CMD_ELASTIC,    // i0 a, i1 b, f: pa[3] pb[3] rest length
    CMD_NOCOLLIDE,  // i0 body
    CMD_WEIGHT,     // i0 body, f0 mass
    CMD_THRUSTER,   // i0 body, f: point[3] dir[3]
    CMD_HOVER,      // i0 body, f: point[3]
    CMD_EXPLODE,    // i0 body
    CMD_TOGGLE,     // i0 body: switch its thrusters and hoverballs
    CMD_SLIDER,     // i0 a, i1 b, f: pa[3] pb[3]
    CMD_RAG_SPAWN,  // i0 NPC type, f: pos[3] yaw
    CMD_RAG_REMOVE, // i0 ragdoll
    CMD_RAG_FREEZE, // i0 ragdoll, i1 frozen
    CMD_RAG_INFLATE, // i0 ragdoll, i1 bone, f0 factor
    CMD_SEAT_TOGGLE, // i0 seat: switch the thrusters and hoverballs of everything joined to it
    CMD_TOOL,        // i0 body aimed at, i1 Tool (wheel...emitter); u: first pick's body + 1 | colour << 16 |
                     // emitter effect << 24; f: point, surface normal, then view direction or the first pick
    CMD_LAUNCH,      // i0 weapon (crossbow, RPG, grenade, bug bait), f: eye[3] dir[3]
    CMD_SQUEEZE,     // bug bait squeezed: antlions follow this player
    CMD_PICKUP,      // i0 body: a health kit, vial, battery or ammo box this player walked over
    CMD_HEAL,        // f0 health from a charger
    CMD_RAG_POSE,    // i0 ragdoll, i1 0 its face / 1 left hand / 2 right hand, u the expression or hand pose
    CMD_QUICKCHAT,   // i0 quick chat message: the host puts "name: message" in everyone's chat
    CMD_GESTURE,     // i0 quick chat gesture (npc_gesture_name): everyone sees this player's character do it
};
#define MAX_OUTQ 8

typedef struct {
    uint16_t id;
    uint8_t kind, pad;
    int32_t i0, i1;
    uint32_t u;
    float f[10];
} NetCmd;

typedef struct {
    int active;
    uint8_t mac[6];
    char name[16];
    vec3 pos; // feet
    float yaw, pitch;
    int weapon, flags;
    int held, held_prev; // physics gun target body, or -1
    int held_rag, held_bone; // or a ragdoll, by one of its bones
    vec3 hold_pos;
    quat hold_rot;
    float health;
    int model;       // NPC type of their player model, or -1
    int colour, pgun; // Profile colours (profile_colour indices): player and physics gun
    int gesture, gesture_seq; // quick chat: the last gesture (index + 1) and how many so far (a new one restarts it)
    int gesture_known;        // a client has seen this player's gesture_seq (a gesture from before isn't replayed)
    float prop_hurt; // host: until a flying prop can hurt them again
    int veh;         // the vehicle or seat (body) they're in, or -1
    float veh_throttle, veh_steer;
    int veh_brake;
    float heard; // seconds since the last packet
    uint32_t nonce;
    uint16_t last_cmd;
    int undo[UNDO_MAX];
    int undo_count;
} Peer;

static struct {
    Map map;
    char map_name[32];
    PropLib props;   // spawnable props + weapon models
    int props_loaded;
    PropLib maplib;  // this map's static and physics prop models
    int maplib_loaded;
    unsigned int *sprop_ambient;
    unsigned char *sprop_sunlit;
    int spawnable[MAX_SPAWNABLE];
    int num_spawnable;
    int vm_model[WEAPON_COUNT];
    int vm_c[WEAPON_COUNT];      // first-person model with arms, or -1
    int vm_anim[WEAPON_COUNT];   // its animated version (vmodel.c), or -1
    int vm_last_weapon;          // the weapon whose draw animation last played
    int gibs[8][8], num_gibs[8]; // broken-piece models per break group (props named _gib<group>)
    vec3 vm_muzzle[WEAPON_COUNT]; // its muzzle in view space (x forward, y left, z up)
    PhysWorld phys;
    Player player;
    float yaw, pitch;
    float look_hold[4];
    Camera cam;
    MapDrawStats ds;
    int sprops_drawn;
    int bodies_drawn;

    Weapon weapon;
    Tool tool;
    // Guns
    int clip[WEAPON_COUNT], ammo[WEAPON_COUNT];
    float fire_cd;      // until the next shot
    float reload_left;  // > 0 while reloading
    float muzzle_time;  // muzzle flash
    struct {
        vec3 a, b;
        float time;
    } tracers[8];
    int next_tracer;
    // Physics gun (held is a body; held_rag a ragdoll, by one of its bones)
    int held_rag, held_bone;
    int held;
    float hold_dist;
    vec3 grab_local;
    quat hold_rel;
    int hold_sound;
    // Tool gun
    float beam_time;
    int use_carry; // G.held is being carried with the use key, not a weapon
    // Driving (vehicle.c): the vehicle or seat we're in, or -1
    int veh;
    float veh_yaw;            // its heading last frame (the view turns with it)
    float veh_throttle, veh_steer;
    int veh_brake;
    int veh_snd;              // engine loop, or -1
    // Voice chat (voice.c): hold D-pad Up to talk in multiplayer (a quick tap still undoes)
    float talk_hold;
    int up_undo;              // this frame's Up means undo
    uint8_t voice_seq;
    float voice_heard[NET_MAX_PLAYERS]; // seconds since each player's last voice frame
    vec3 blast_pos; // last explosion, for its flash
    float blast_time;
    int tool_scroll; // first row shown in the spawn menu's tools list
    vec3 beam_end;
    int dupe_model;
    quat dupe_rot;
    unsigned int dupe_tint;
    int stage;        // weld/rope: 1 once the first point is picked
    int stage_body;
    vec3 stage_point;
    // View model animation
    float bob_phase;
    float switch_time;
    float kick;

    int undo[UNDO_MAX]; // body index, or NPC_ID_BASE + NPC index
    int undo_count;
    float impact_cooldown[MAX_BODIES];

    // Health
    int npcs_loaded;
    float health;
    float suit; // HEV suit power (Options > HEV suit): soaks up 80% of damage, as in HL2
    int cam_view; // Camera tool: the camera prop (body) the view is from, -1 = your own eyes
    float hurt_flash;
    float dead_time; // > 0 while dead

    // Footsteps and landing
    float step_dist;
    int was_on_ground;
    float fall_speed;
    int last_water;

    // Spawn menu (GMod's Q menu)
    int qmenu;
    int focus;       // 0 categories, 1 prop grid, 2 tools
    int cat;         // spawnlist shown in the grid (row_name), or RECENT_CAT
    int cat_scroll;  // first row shown
    int qrow;        // cursor in the spawnlists panel (panel_rows: a list or a game's header)
    int group_open;  // the game whose spawnlists show under its header (k_games index), or -1
    int recent[RECENT_MAX], recent_n; // last things spawned from the menu, newest first
    int grid_sel;
    int grid_scroll;
    int tool_row;    // row in the tools panel
    int colour_sel;  // Colour tool's chosen colour
    // Pause menu
    int paused;
    int pause_sel;
    int pause_options;
    int pause_saves; // Pause > Saves is open
    float select_time; // how long Select has been held
    int select_fired;  // the hold already toggled noclip
    // Notifications, newest first
    struct {
        char text[48];
        int kind;
        float time;
    } notes[MAX_NOTES];
    float wsel_time; // weapon selector display time

    struct {
        int punt, pickup_grav, hold_grav, explode, break_glass, break_flesh, break_hard;
        int pickup, drop, hold, dryfire, freeze, tool, spawn, undo, menu_move, menu_select, step_concrete, step_water,
            land, splash, remove, weld, impact[NUM_SURF];
        int fire[WEAPON_COUNT], crowbar_hit, reload, pump, empty, bullet_impact, bullet_flesh;
        int jeep_engine, airboat_engine, vehicle_enter, vehicle_exit;
        int bolt_hit, bolt_flesh, gren_tick, bait_squeeze, bait_splat;
        int medkit, medvial, battery, charge_health, charge_suit, charge_no_health, charge_no_suit, charge_done_suit;
        int ammo, shutter;
    } snd;

    // Multiplayer (see the Multiplayer section). Slot 0 is the host.
    int role;
    int my_slot;
    int acting; // host: the slot whose command is running (0 = this PSP's player)
    uint8_t host_mac[6];
    Peer peers[NET_MAX_PLAYERS];
    int body_owner[MAX_BODIES];
    unsigned char body_dirty[MAX_BODIES];
    vec3 net_tpos[MAX_BODIES];
    quat net_trot[MAX_BODIES];
    float snap_timer, ents_timer, ropes_timer, beacon_timer, send_timer;
    int body_rr;
    uint32_t tick;
    uint32_t last_body_tick;
    float host_heard;
    NetCmd outq[MAX_OUTQ];
    int outq_n;
    uint16_t next_cmd;
    uint16_t acked;
    uint16_t respawn_cmd;
    int respawn_pending;
    vec3 net_hold_pos;
    quat net_hold_rot;
    int net_was_ready;
    int server_god; // client: the host has God mode on
    float prop_hurt; // until a flying prop can hurt this player again
    float left_hold; // PSP: how long D-pad Left has been held (tap: next weapon, hold: reload)
    int left_fired;
    // Bug bait: R went down (bait_down) and how long it's been held; it was squeezed, not thrown
    float bait_hold;
    int bait_down, bait_squeezed;
    // Items: the charger charging us (-1: none) and its sound; a client's health still to ask the host for;
    // a kit we just took, until the host has removed it
    int charging, charge_snd;
    float heal_pending, heal_timer;
    int taken_body;
    float taken_time;
    char exit_msg[64];
} G;

// --- Gamemodes: what the rules decide about each player (see Gamemodes) ---
#define GM_NO_DISGUISE 0xFFFF // GmPlayer.disguise: none
#define GM_EVENTS 8
#define GM_NW_MAX 1024      // the gamemode's networked variables, packed (script_nw_pack)
#define GM_USE_RANGE 140.0f // how far Use reaches for a gamemode (Prop Hunt: the prop to become)
#define ALL_WEAPONS ((1u << WEAPON_COUNT) - 1u)
enum { GEV_CHAT = 1, GEV_CENTER, GEV_SOUND, GEV_AMMO };
typedef struct {
    int team, flags;      // GPF_*
    int model;            // forced player model (NPC type), -1: their own
    float speed;          // walking speed, 1 = usual
    int disguise;         // the prop they look like: model | lib << 15, or GM_NO_DISGUISE
    unsigned int weapons; // what they may carry (a bit per Weapon)
    int spawn_seq;        // counts (re)spawns and moves (1-255, 0: none yet): a new one sends them to spawn_pos...
    int spawn_move;       // ...moved there as they are, rather than respawned
    vec3 spawn_pos;
    float spawn_yaw;
    int select_seq, select_weapon; // the gamemode put this weapon in their hand
    float max_health;
    // Where the rules run
    int serial;     // counts who's had the slot (scripts' Player objects)
    int known;      // the gamemode's been told they're here (PlayerInitialSpawn)...
    uint32_t nonce; // ...as this join (a rejoin is someone new)
    int alive;      // last frame
    int attacker;   // who hurt them last (a slot, -1 the world)
    float dead_for;
} GmPlayer;
// Something that happens once, for one player (target) or everyone (255)
typedef struct {
    uint16_t seq; // 0: an empty slot
    uint8_t kind, target;
    int16_t arg, arg2; // GEV_CENTER: arg seconds; GEV_SOUND: arg2 1 at pos; GEV_AMMO: the weapon (255 all), how much
    float pos[3];
    char text[44];
} GmEvent;
static struct {
    int on; // a gamemode runs (not plain sandbox)
    char mode[NET_MODE_LEN];
    int flags; // GMF_*
    float respawn_time;
    GmPlayer p[NET_MAX_PLAYERS];
    int scripts_pending; // they start on the first frame, once it's known whether this PSP hosts
    // Where the rules run: sending it all
    int dirty;
    float state_timer, ev_timer;
    GmEvent ev[GM_EVENTS];
    uint16_t ev_seq;
    int ev_sends;
    // A client: the events it's seen
    uint16_t ev_seen;
    int ev_synced;
    // This PSP's player
    int seen_spawn, seen_select;
    unsigned int had_weapons;
    float spawn_grace;   // a client just respawned: dead in an older snapshot isn't dead (until an alive one)
    float respawn_asked; // a client asked to respawn this long ago
    int spectate;        // dead: whose view to watch (-1: where you died)
    char center[48];     // text across the middle of the screen
    float center_time;
} g_gm;
static char g_next_mode[NET_MODE_LEN]; // game_set_gamemode
static int g_npc_attacker = -1;        // the player whose shot or blast is hurting NPCs right now
static unsigned int g_buttons;         // this frame's held buttons (game_button_down, for HUD scripts)
// The sandbox's weapons: everything, or with Options > Spawn with weapons off just the toolbox until the
// spawn menu's Weapons hands you more (a gamemode's game has its own: g_gm)
#define TOOLBOX ((1u << WEAPON_HANDS) | (1u << WEAPON_PHYSGUN) | (1u << WEAPON_GRAVGUN) | (1u << WEAPON_TOOLGUN))
static unsigned int g_sandbox_weapons = ALL_WEAPONS;
static void sandbox_loadout(void);
static void gm_switch_weapon(int w);
static void physgun_release(int quiet);

// The sandbox's tools (spawn menu, tool gun, noclip, God mode...): no gamemode, or one that keeps them
static int gm_sandbox(void) {
    return !g_gm.on || (g_gm.flags & GMF_SANDBOX);
}

// Options > God mode: when hosting it covers everyone (never in a gamemode's game)
static int host_god(void) {
    return gm_sandbox() && g_settings.god_mode;
}

// Options > Unlimited ammo (not in a gamemode's game)
static int unlimited_ammo(void) {
    return gm_sandbox() && g_settings.unlimited_ammo;
}

static int gm_disguised(int s);
static void player_box(int s, vec3 feet, int ducked, vec3 *lo, vec3 *hi);
static void gm_respawn_request(int s);
static void gm_puppet(int s);
static int gm_next_weapon(int w);
static void gm_reset(void);
static void gm_start_scripts(void);
static void gm_local_update(float dt);
static void gm_server_update(float dt);
static void gm_host_send(float dt);
static void gm_client_state(const void *data, int len);
static void gm_client_events(const void *data, int len);
static void gm_move(MoveCmd *cmd);
static void gm_use(int body);
static void gm_dead_update(const InputState *in);
static void gm_spectate_camera(void);
static int gm_tag_visible(int s);
static void gm_npc_died(int npc, int type, vec3 pos);
static unsigned int gm_carried(void);

// Options > Player model: which NPC types are offered (settings.c shows their names)
#define MAX_PLAYER_MODELS 64
static int g_pm_types[MAX_PLAYER_MODELS];
static int g_pm_count;

static const char *player_model_name(int i) {
    return i >= 0 && i < g_pm_count ? npc_type_name(g_pm_types[i]) : "";
}

// The NPC type this player looks like, or -1 for the default
static int player_model(void) {
    if (g_gm.on && G.my_slot >= 0 && G.my_slot < NET_MAX_PLAYERS && g_gm.p[G.my_slot].model >= 0) return g_gm.p[G.my_slot].model;
    return g_pm_count > 0 ? g_pm_types[g_settings.player_model % g_pm_count] : -1;
}
static int third_person(void);

static int is_client(void) {
    return G.role == ROLE_CLIENT;
}

// God mode: the option covers single player and, when hosting, everyone in the game. A client goes
// by the host's option, not its own.
static int god_mode(void) {
    return gm_sandbox() && (is_client() ? G.server_god : g_settings.god_mode);
}
static void send_cmd(int kind, int i0, int i1, uint32_t u, const float *f, int nf);
static int prop_named(const char *name);
static void vehicle_enter(int body);
static void recent_add(int item);
static void qmenu_test_state(void);
static void qmenu_test_list(const char *name);
static int grid_items(int *out);
static void qmenu_test_pick(int k);
static int shoot(int weapon, vec3 eye, vec3 f, uint32_t seed, int world, int effects);
static vec3 view_right(void);
// autoload.txt test commands: the last prop "spawnat" made, and "autodrive"'s held stick
static int g_test_body = -1, g_test_drive;
static float g_test_throttle, g_test_steer, g_test_walk;
// autoload.txt "physrot": buttons held down and the stick, as if pressed (R and X: the physics gun turning)
static unsigned int g_test_hold, g_test_hold_prev;
static char g_rejoin[48]; // the server to join again after it changed map (game_rejoin_address)
static float g_test_ax, g_test_ay;
static void net_leave(void);
static void vm_event(int kind);
static void vm_tick(float dt);
static void spawn_ragdoll(int type, vec3 pos, float yaw);
static void damage_player(float amount);
static vec3 view_forward(void);
static void blood_clear(void);
static void blood_spray(vec3 hit, vec3 dir, int amount);
static void bullet_hole(vec3 eye, vec3 dir, vec3 hit, int body);

// GMod-style notification in the bottom right
static void notify(const char *msg, int kind) {
    if (G.acting) return; // a remote player's action: not for this screen
    if (g_log) g_log(msg);
    memmove(&G.notes[1], &G.notes[0], sizeof(G.notes[0]) * (MAX_NOTES - 1));
    snprintf(G.notes[0].text, sizeof(G.notes[0].text), "%s", msg);
    G.notes[0].kind = kind;
    G.notes[0].time = 0.0f;
}

static void toast(const char *msg) {
    notify(msg, NOTE_GENERIC);
}

static void play(int id, float vol) {
    if (g_settings.sound && !G.acting) snd_play(id, vol, 0.0f);
}

static void play_at(int id, vec3 pos, float vol) {
    if (g_settings.sound) snd_play_at(id, pos, vol);
}

// Spawnable model numbers: the core pack's models, then the extra props (propx.c) after them.
// An extra prop's record is filled into *tmp; NULL for a number that's neither.
static const PpakModel *model_info(int model, PpakModel *tmp) {
    if (model < 0) return NULL;
    if (model < G.props.num_models) return &G.props.models[model];
    if (model - G.props.num_models >= propx_count()) return NULL;
    propx_info(model - G.props.num_models, tmp);
    return tmp;
}

static const char *model_name(int model) {
    return model < G.props.num_models ? G.props.models[model].name : propx_name(model - G.props.num_models);
}

// Its spawn menu category: the core pack's, then the extras' own after them
static int model_cat(int model) {
    if (model < G.props.num_models) return (int)((G.props.models[model].flags >> 16) & 0xFF);
    return G.props.num_categories + propx_category_of(model - G.props.num_models);
}

// The pack a spawnable model is drawn from, and its index in that pack. An extra prop is read
// from disc the first time; NULL when it can't be (no memory).
static const PropLib *model_lib(int model, int *local) {
    *local = 0;
    if (model < 0) return NULL;
    if (model < G.props.num_models) {
        *local = model;
        return &G.props;
    }
    return propx_lib(model - G.props.num_models);
}

static const PropLib *body_lib(const Body *b, int *local) {
    if (b->lib == 1) {
        *local = b->model;
        return &G.maplib;
    }
    return model_lib(b->model, local);
}

// Frees the extra props no body uses any more, making room for others
static void release_unused_extras(void) {
    for (int x = 0; x < propx_count(); x++) {
        if (!propx_loaded(x)) continue;
        int used = 0;
        for (int i = 0; i < MAX_BODIES && !used; i++) {
            const Body *b = &G.phys.bodies[i];
            used = b->active && b->lib == 0 && b->model == G.props.num_models + x;
        }
        if (!used) propx_unload(x);
    }
    propx_retry_all();
}


// Props spawned by the player who is acting (clients don't know owners: they count everything)
static int player_prop_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_BODIES; i++) {
        n += G.phys.bodies[i].active && G.phys.bodies[i].lib == 0 && G.phys.bodies[i].life <= 0.0f &&
             (is_client() || G.body_owner[i] == G.acting); // broken pieces don't count
    }
    return n;
}

// --- Lifecycle ---------------------------------------------------------------------------------

static void npc_shot(const char *weapon, vec3 from, vec3 to);
static void minge_act(int npc, int action, vec3 at, vec3 target);
static void devices_clear(void);
static void toolgun_fire(void);
static void test_use_aim(void);
static void test_use_body(int body);
static void launch(int w, vec3 eye, vec3 f);
static void projectiles_clear(void);
// Items (see Items): what a prop is, for pickups and chargers. Ammo boxes come after the chargers, one
// kind per gun (k_ammo_items).
enum {
    ITEM_NONE, ITEM_HEALTHKIT, ITEM_HEALTHVIAL, ITEM_BATTERY, ITEM_HEALTH_CHARGER, ITEM_SUIT_CHARGER,
    ITEM_AMMO_PISTOL, ITEM_AMMO_357, ITEM_AMMO_SMG, ITEM_AMMO_SHOTGUN, ITEM_AMMO_AR2, ITEM_AMMO_CROSSBOW,
    ITEM_AMMO_RPG, ITEM_AMMO_GRENADE, ITEM_COUNT
};
static int is_charger(int kind) {
    return kind == ITEM_HEALTH_CHARGER || kind == ITEM_SUIT_CHARGER;
}
static int is_pickup(int kind) { // taken by walking over it
    return (kind >= ITEM_HEALTHKIT && kind <= ITEM_BATTERY) || kind >= ITEM_AMMO_PISTOL;
}
static int item_kind(int body);
static void items_clear(void);
static void charger_use(int body);
static void test_use_item(void);
static void stop_charging(int sound);
static void save_build(const char *name);
static Build g_build;
static int build_capture(Build *bd);
static int load_build(const char *name, int in_place);
static void cleanup_props(void);

static void load_shared(void) {
    if (G.props_loaded) return;
    char perr[96];
    if (proplib_load(&G.props, "data/props", perr, sizeof(perr)) == 0) {
        G.props_loaded = 1;
        G.num_spawnable = 0;
        for (int w = 0; w < WEAPON_COUNT; w++) G.vm_model[w] = G.vm_c[w] = G.vm_anim[w] = -1;
        // Animated first-person weapons when the pack has them
        if (vmodels_load() == 0) {
            for (int w = 0; w < WEAPON_COUNT; w++) G.vm_anim[w] = vmodel_find(k_weapon_vms[w]);
        }
        G.vm_last_weapon = -1;
        for (int i = 0; i < G.props.num_models; i++) {
            const PpakModel *md = &G.props.models[i];
            for (int w = 0; w < WEAPON_COUNT; w++) {
                if (k_weapon_models[w] && strcmp(md->name, k_weapon_models[w]) == 0) G.vm_model[w] = i;
                if (k_weapon_vms[w] && strcmp(md->name, k_weapon_vms[w]) == 0) G.vm_c[w] = i;
            }
            if (!(md->flags & PPAK_HIDDEN) && G.num_spawnable < MAX_SPAWNABLE) G.spawnable[G.num_spawnable++] = i;
        }
        // The extra props, on disc until spawned; their categories come after the core pack's
        if (propx_init() == 0) {
            for (int i = 0; i < propx_count() && G.num_spawnable < MAX_SPAWNABLE; i++) {
                G.spawnable[G.num_spawnable++] = G.props.num_models + i;
            }
        }
        // Broken-piece models, by break group
        memset(G.num_gibs, 0, sizeof(G.num_gibs));
        for (int i = 0; i < G.props.num_models; i++) {
            int g;
            if (sscanf(G.props.models[i].name, "_gib%d", &g) == 1 && g > 0 && g < 8 && G.num_gibs[g] < 8) {
                G.gibs[g][G.num_gibs[g]++] = i;
            }
        }
        // Muzzle of each first-person model: its furthest-forward point, roughly level with the barrel
        for (int w = 0; w < WEAPON_COUNT; w++) {
            if (G.vm_c[w] < 0) continue;
            const PpakModel *md = &G.props.models[G.vm_c[w]];
            vec3 best = v3(md->maxs[0], 0, 0);
            float bx = -1e9f;
            for (uint32_t k = 0; k < md->num_meshes; k++) {
                const PpakMesh *ms = &G.props.meshes[md->first_mesh + k];
                for (uint32_t j = 0; j < ms->num_vertices; j++) {
                    const PpakVertex *v = &G.props.verts[ms->first_vertex + j];
                    if (v->x > bx) {
                        bx = v->x;
                        best = v3(v->x, v->y, v->z);
                    }
                }
            }
            G.vm_muzzle[w] = best;
        }
    }
    G.snd.pickup = snd_find("physgun_pickup");
    G.snd.drop = snd_find("physgun_drop");
    G.snd.punt = snd_find("gravgun_punt");
    G.snd.explode = snd_find("explode");
    G.snd.break_glass = snd_find("break_glass");
    G.snd.break_flesh = snd_find("break_flesh");
    G.snd.break_hard = snd_find("break_hard");
    G.snd.pickup_grav = snd_find("gravgun_pickup");
    G.snd.hold_grav = snd_find("gravgun_hold");
    if (G.snd.pickup_grav < 0) G.snd.pickup_grav = G.snd.pickup;
    if (G.snd.hold_grav < 0) G.snd.hold_grav = G.snd.hold;
    if (G.snd.punt < 0) G.snd.punt = G.snd.drop;
    G.snd.hold = snd_find("physgun_hold");
    G.snd.dryfire = snd_find("physgun_dryfire");
    G.snd.freeze = snd_find("physgun_freeze");
    G.snd.tool = snd_find("toolgun_shoot");
    G.snd.spawn = snd_find("spawn");
    G.snd.undo = snd_find("undo");
    G.snd.menu_move = snd_find("menu_move");
    G.snd.menu_select = snd_find("menu_select");
    G.snd.step_concrete = snd_find("step_concrete");
    G.snd.step_water = snd_find("step_water");
    G.snd.land = snd_find("jump_land");
    G.snd.splash = snd_find("splash");
    G.snd.remove = snd_find("remove");
    G.snd.weld = snd_find("weld");
    for (int i = 0; i < NUM_SURF; i++) G.snd.impact[i] = snd_find(k_impact_sounds[i]);
    for (int w = 0; w < WEAPON_COUNT; w++) G.snd.fire[w] = k_ws[w].fire_snd ? snd_find(k_ws[w].fire_snd) : -1;
    G.snd.crowbar_hit = snd_find("crowbar_hit");
    G.snd.reload = snd_find("reload");
    G.snd.pump = snd_find("shotgun_pump");
    G.snd.empty = snd_find("empty");
    G.snd.bullet_impact = snd_find("bullet_impact");
    G.snd.bullet_flesh = snd_find("bullet_flesh");
    G.snd.jeep_engine = snd_find("jeep_engine");
    G.snd.airboat_engine = snd_find("airboat_engine");
    G.snd.vehicle_enter = snd_find("vehicle_enter");
    G.snd.vehicle_exit = snd_find("vehicle_exit");
    G.snd.bolt_hit = snd_find("crossbow_hit");
    G.snd.bolt_flesh = snd_find("crossbow_hitbod");
    G.snd.gren_tick = snd_find("grenade_tick");
    G.snd.bait_squeeze = snd_find("bugbait_squeeze");
    G.snd.bait_splat = snd_find("bugbait_splat");
    G.snd.medkit = snd_find("medkit");
    G.snd.medvial = snd_find("medvial");
    G.snd.battery = snd_find("battery");
    G.snd.ammo = snd_find("ammo_pickup");
    G.snd.shutter = snd_find("screenshot");
    G.snd.charge_health = snd_find("charge_health");
    G.snd.charge_suit = snd_find("charge_suit");
    G.snd.charge_no_health = snd_find("charge_no_health");
    G.snd.charge_no_suit = snd_find("charge_no_suit");
    G.snd.charge_done_suit = snd_find("charge_done_suit");
    G.hold_sound = -1;
    G.group_open = -1; // the spawn menu's game groups start closed
    G.veh = -1;
    G.veh_snd = -1;
    // Drivable vehicles and seats (vehicle.c): which spawnable props they are
    {
        static const char *seat_names[] = {"Jeep Seat", "Airboat Seat", "Office Seat"};
        static const float seat_eyes[] = {37.0f, 33.0f, 52.0f};
        int seats[3];
        for (int k = 0; k < 3; k++) seats[k] = G.props_loaded ? prop_named(seat_names[k]) : -1;
        vehicles_set_models(G.props_loaded ? prop_named("Jeep") : -1, G.props_loaded ? prop_named("Airboat") : -1, seats,
                            seat_eyes, 3);
        vehicles_init();
    }
    G.npcs_loaded = npcs_load() == 0 && npc_type_count() > 0;
    npc_set_shot_handler(npc_shot);
    npc_set_minge_handler(minge_act);
    game_refresh_player_models();
}

// Options > Player model: the NPC pack's characters marked as player models (Workshop ones only while
// they're switched on in Main menu > Workshop)
void game_refresh_player_models(void) {
    g_pm_count = 0;
    for (int t = 0; t < npc_type_count() && g_pm_count < MAX_PLAYER_MODELS; t++) {
        if (npc_type_player_model(t) && (!npc_type_workshop(t) || workshop_enabled(WS_CUSTOM, npc_type_name(t)))) {
            g_pm_types[g_pm_count++] = t;
        }
    }
    settings_set_player_models(g_pm_count, player_model_name);
}

static void refill_ammo(void) {
    for (int w = 0; w < WEAPON_COUNT; w++) {
        G.clip[w] = k_ws[w].clip;
        G.ammo[w] = k_ws[w].ammo;
    }
    G.reload_left = 0.0f;
    G.fire_cd = 0.0f;
}

// A static prop's axes and origin in the world: props carried by a door (its model) follow the door.
static void sprop_frame(const PmapStaticProp *sp, vec3 *ax, vec3 *ay, vec3 *az, vec3 *o) {
    *ax = v3(sp->axis_x[0], sp->axis_x[1], sp->axis_x[2]);
    *ay = v3(sp->axis_y[0], sp->axis_y[1], sp->axis_y[2]);
    *az = v3(sp->axis_z[0], sp->axis_z[1], sp->axis_z[2]);
    *o = v3(sp->origin[0], sp->origin[1], sp->origin[2]);
    int p = (int)sp->parent - 1;
    if (p < 0 || p >= G.map.num_ents || !G.map.ent_state) return;
    const PmapEnt *e = &G.map.ents[p];
    const EntState *s = &G.map.ent_state[p];
    vec3 rel = v3_sub(*o, v3(e->origin[0], e->origin[1], e->origin[2]));
    if (s->rotated) {
        *ax = q_rotate(s->rot, *ax);
        *ay = q_rotate(s->rot, *ay);
        *az = q_rotate(s->rot, *az);
        rel = q_rotate(s->rot, rel);
    }
    *o = v3_add(s->pos, rel);
}

static void light_static_props(void) {

    int n = G.map.num_sprops;
    G.sprop_ambient = calloc(n > 0 ? n : 1, sizeof(unsigned int));
    G.sprop_sunlit = calloc(n > 0 ? n : 1, 1);
    const float *sd = G.map.info->sun_dir;
    for (int i = 0; i < n; i++) {
        const PmapStaticProp *sp = &G.map.sprops[i];
        vec3 o = v3(sp->origin[0], sp->origin[1], sp->origin[2] + 16.0f);
        int leaf = map_point_leaf(&G.map, &o);
        G.sprop_ambient[i] = (leaf >= 0 && leaf < G.map.num_leafs) ? G.map.leafs[leaf].ambient_abgr : G.map.info->ambient_abgr;
        Trace tr;
        cm_trace_line(&G.map, o, v3_mad(o, v3(sd[0], sd[1], sd[2]), 3000.0f), MASK_PROPSOLID, NULL, 0, &tr);
        G.sprop_sunlit[i] = tr.fraction >= 1.0f;
    }
}

// Gives a body its .phy convex hull when the model has one.
static void attach_hull(Body *b, const PropLib *lib, int model) {
    if (b->shape != SHAPE_HULL) return;
    if (!lib->hulls || !lib->hulls[model].num_pieces) {
        b->shape = SHAPE_BOX; // pack without hull data: fall back to the bounding box
        return;
    }
    const PpakHull *h = &lib->hulls[model];
    b->hull.pieces = lib->hull_pieces + h->first_piece;
    b->hull.num_pieces = (int)h->num_pieces;
    b->hull.planes = lib->hull_planes;
    b->hull.points = lib->hull_points + h->first_point;
    b->hull.num_points = (int)h->num_points;
}

static void spawn_map_physics(void) {
    for (int i = 0; i < G.map.num_phys_props && G.maplib_loaded; i++) {
        const PmapPhysProp *pp = &G.map.phys_props[i];
        if ((int)pp->model >= G.maplib.num_models) continue;
        const PpakModel *md = &G.maplib.models[pp->model];
        vec3 mins = v3(md->mins[0], md->mins[1], md->mins[2]), maxs = v3(md->maxs[0], md->maxs[1], md->maxs[2]);
        quat q = q_norm((quat){pp->rot[0], pp->rot[1], pp->rot[2], pp->rot[3]});
        // The map gives the model origin; physics wants the shape centre
        vec3 centre_local = v3_scale(v3_add(mins, maxs), 0.5f);
        vec3 pos = v3_add(v3(pp->origin[0], pp->origin[1], pp->origin[2]), q_rotate(q, centre_local));
        int b = phys_spawn(&G.phys, (int)pp->model, (int)(md->flags & PPAK_SHAPE_MASK), mins, maxs, md->mass, pos, q);
        if (b < 0) break;
        attach_hull(&G.phys.bodies[b], &G.maplib, (int)pp->model);
        G.phys.bodies[b].lib = 1;
        G.phys.bodies[b].surface = (int)md->surface;
        G.phys.bodies[b].asleep = 1; // map props wait until something disturbs them
    }
}

// --- gm_construct_13 ---------------------------------------------------------------------------
// A darkened copy of gm_construct where a black figure turns up in the distance now and then,
// usually at the edge of your view. Look straight at it, or walk up to it, and it's gone.

#define HAUNT_MAP "gm_construct_13"

static struct {
    int on, visible;
    vec3 pos;    // feet
    float yaw;   // degrees, faces where the player was when it appeared
    float timer; // until it next appears, or how long it stays
    float stare; // seconds the player has looked right at it
} g_haunt;

static unsigned int dim_abgr(unsigned int c, float k) {
    unsigned int out = c & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) out |= (unsigned int)((float)((c >> s) & 0xFF) * k) << s;
    return out;
}

// Night-time version of the map: dim the baked lighting, dark sky, thick dark fog.
static void haunt_setup(void) {
    PmapInfo *info = (PmapInfo *)G.map.info;
    info->fog_abgr = 0xFF0E0D0C;
    info->sky_abgr = 0xFF0A0909;
    info->sky_top_abgr = 0xFF060505;
    info->sky_bottom_abgr = 0xFF181615;
    info->fog_start = 150.0f;
    info->fog_end = 3200.0f;
    info->sun_abgr = dim_abgr(info->sun_abgr, 0.25f);
    info->ambient_abgr = dim_abgr(info->ambient_abgr, 0.45f);
    G.map.sky_first_tex = -1; // painted (dark) dome instead of the daytime skybox
    const PmapSection *s = pak_find(G.map.file, PMAP_SECTION_ID('V', 'E', 'R', 'T'));
    if (s) {
        PmapVertex *v = (PmapVertex *)((char *)G.map.file + s->offset);
        int n = (int)(s->size / sizeof(PmapVertex));
        for (int i = 0; i < n; i++) v[i].color = dim_abgr(v[i].color, 0.42f);
    }
    PmapLeaf *leafs = (PmapLeaf *)G.map.leafs;
    for (int i = 0; i < G.map.num_leafs; i++) leafs[i].ambient_abgr = dim_abgr(leafs[i].ambient_abgr, 0.45f);
    PmapLightStyle *ls = (PmapLightStyle *)G.map.lstyles;
    for (int i = 0; i < G.map.num_lstyles; i++) ls[i].base = dim_abgr(ls[i].base, 0.42f);
    memset(&g_haunt, 0, sizeof(g_haunt));
    g_haunt.on = 1;
    g_haunt.timer = 15.0f + (float)(rand() % 20);
    sceKernelDcacheWritebackAll(); // the GE reads the dimmed vertex colours straight from RAM
}

// Somewhere on the ground 1100-2700 units away, off to one side of the view, in line of sight.
static int haunt_place(void) {
    vec3 eye = player_eye(&G.player);
    for (int tries = 0; tries < 16; tries++) {
        float side = (rand() & 1) ? 1.0f : -1.0f;
        float ang = (G.yaw + side * (22.0f + (float)(rand() % 38))) * DEG2RAD;
        float dist = 1100.0f + (float)(rand() % 1600);
        vec3 top = v3(eye.x + cosf(ang) * dist, eye.y + sinf(ang) * dist, eye.z + 300.0f);
        Trace tr;
        cm_trace_line(&G.map, top, v3(top.x, top.y, top.z - 3000.0f), MASK_PLAYERSOLID, NULL, 0, &tr);
        if (tr.startsolid || tr.fraction >= 1.0f || tr.normal.z < 0.7f) continue;
        vec3 feet = tr.endpos;
        cm_trace_line(&G.map, eye, v3(feet.x, feet.y, feet.z + 70.0f), MASK_PROPSOLID, NULL, 0, &tr);
        if (tr.fraction < 0.98f) continue;
        g_haunt.pos = feet;
        g_haunt.yaw = atan2f(eye.y - feet.y, eye.x - feet.x) / DEG2RAD;
        return 1;
    }
    return 0;
}

static void haunt_update(float dt) {
    if (G.dead_time > 0.0f || G.player.noclip) {
        g_haunt.visible = 0;
        return;
    }
    if (!g_haunt.visible) {
        g_haunt.timer -= dt;
        if (g_haunt.timer <= 0.0f) {
            if (haunt_place()) {
                g_haunt.visible = 1;
                g_haunt.timer = 6.0f + (float)(rand() % 10);
                g_haunt.stare = 0.0f;
            } else {
                g_haunt.timer = 3.0f; // nowhere good from here; try again soon
            }
        }
        return;
    }
    vec3 d = v3_sub(v3(g_haunt.pos.x, g_haunt.pos.y, g_haunt.pos.z + 60.0f), player_eye(&G.player));
    float dist = v3_len(d);
    if (v3_dot(d, view_forward()) / (dist + 0.001f) > 0.985f) g_haunt.stare += dt;
    g_haunt.timer -= dt;
    if (dist < 700.0f || g_haunt.stare > 0.6f || g_haunt.timer <= 0.0f) {
        g_haunt.visible = 0;
        g_haunt.timer = 20.0f + (float)(rand() % 40);
    }
}

void game_preload(void) {
    load_shared();
}

int game_start(const char *map_name, char *err, int err_len) {
    load_shared();
    if (map_load(&G.map, map_name, err, err_len) != 0) {
        return -1;
    }
    snprintf(G.map_name, sizeof(G.map_name), "%s", map_name);
    achieve_map(map_name);
    g_haunt.on = 0;
    if (strncmp(map_name, HAUNT_MAP, strlen(HAUNT_MAP)) == 0) haunt_setup();
    char base[64], perr[96];
    snprintf(base, sizeof(base), "data/maps/%s", map_file_name(map_name));
    char *dot = strstr(base, ".low");
    if (dot) *dot = 0;
    G.maplib_loaded = (G.map.num_sprops || G.map.num_phys_props) && proplib_load(&G.maplib, base, perr, sizeof(perr)) == 0;
    nav_load(base); // the NPCs' node graph, if the map has one

    ents_init(&G.map);
    phys_init(&G.phys);
    devices_clear();
    projectiles_clear();
    items_clear();
    memset(G.impact_cooldown, 0, sizeof(G.impact_cooldown));

    blood_clear();
    const PmapSpawn *sp = &G.map.spawns[0];
    player_spawn(&G.player, v3(sp->x, sp->y, sp->z));
    G.yaw = sp->yaw;
    G.pitch = 0.0f;
    camera_init(&G.cam);
    G.cam.fov_y = 62.0f;
    G.cam.zfar = G.map.info->fog_end + 512.0f;
    sky_build(&G.map);
    light_static_props();
    spawn_map_physics();
    G.weapon = WEAPON_PHYSGUN;
    G.held = -1;
    G.undo_count = 0;
    G.qmenu = 0;
    G.paused = 0;
    memset(&g_qc, 0, sizeof(g_qc)); // no quick chat wheel left open from the last game
    G.dupe_model = -1;
    G.stage = 0;
    memset(G.notes, 0, sizeof(G.notes));
    G.was_on_ground = 1;
    G.switch_time = 0.0f;
    G.health = 100.0f;
    G.suit = 100.0f;
    G.dead_time = 0.0f;
    G.hurt_flash = 0.0f;
    G.held_rag = -1;
    G.cam_view = -1;
    if (G.veh_snd >= 0) snd_stop(G.veh_snd);
    G.veh = G.veh_snd = -1;
    refill_ammo();
    sandbox_loadout(); // Options > Spawn with weapons
    npcs_clear();
    // Single player until game_host / game_join say otherwise
    G.role = ROLE_OFFLINE;
    G.my_slot = 0;
    G.acting = 0;
    G.exit_msg[0] = 0;
    memset(G.peers, 0, sizeof(G.peers));
    memset(G.body_owner, 0, sizeof(G.body_owner));
    memset(G.body_dirty, 0, sizeof(G.body_dirty));
    G.outq_n = 0;
    G.snap_timer = G.ents_timer = G.ropes_timer = G.beacon_timer = G.send_timer = 0.0f;
    if (!G.props_loaded) toast("No props: run the converter's props step");
    npc_set_death_handler(gm_npc_died);
    gm_reset(); // the gamemode game_set_gamemode picked: its scripts start on the first frame
    return 0;
}

void game_set_view(float x, float y, float z, float yaw, float pitch) {
    G.player.origin = v3(x, y, z);
    G.yaw = yaw;
    G.pitch = pitch;
}

void game_end(void) {
    achieve_save(); // the playtime since the last save
    stop_charging(0);
    net_leave();
    script_stop();
    if (G.hold_sound >= 0) snd_stop(G.hold_sound);
    G.hold_sound = -1;
    if (G.maplib_loaded) proplib_free(&G.maplib);
    G.maplib_loaded = 0;
    propx_unload_all();
    free(G.sprop_ambient);
    free(G.sprop_sunlit);
    G.sprop_ambient = NULL;
    G.sprop_sunlit = NULL;
    npcs_clear();
    nav_free();
    map_free(&G.map);
}

static int spawn_prop(int model, vec3 at, vec3 normal, quat rot, unsigned int tint);
static quat yaw_quat(float yaw_deg);
static quat spawn_rot(int model);

// autoload.txt "after SECONDS COMMAND": a command run that far into the game (timed tests: rockets, fuses)
#define MAX_LATER 64
static struct {
    float t;
    char line[96];
} g_later[MAX_LATER];

static void later_update(float dt) {
    for (int k = 0; k < MAX_LATER; k++) {
        if (g_later[k].t <= 0.0f) continue;
        g_later[k].t -= dt;
        if (g_later[k].t > 0.0f) continue;
        char line[96];
        snprintf(line, sizeof(line), "%s", g_later[k].line);
        game_command(line);
    }
}

void game_command(const char *line) {
    int i, j;
    float x, y, z, yaw;
    if (sscanf(line, "after %f %n", &x, &i) == 1 && i > 0) {
        for (int k = 0; k < MAX_LATER; k++) {
            if (g_later[k].t > 0.0f) continue;
            g_later[k].t = x > 0.001f ? x : 0.001f;
            snprintf(g_later[k].line, sizeof(g_later[k].line), "%s", line + i);
            g_later[k].line[strcspn(g_later[k].line, "\r\n")] = 0;
            break;
        }
    } else if (sscanf(line, "prop %d %f %f %f %f", &i, &x, &y, &z, &yaw) == 5) {
        spawn_prop(i, v3(x, y, z), v3(0, 0, 1), yaw_quat(yaw), 0xFFFFFFFF);
    } else if (sscanf(line, "npc %d %f %f %f %f", &i, &x, &y, &z, &yaw) == 5) {
        npc_spawn(i, v3(x, y, z), yaw);
    } else if (sscanf(line, "npc %d %f", &i, &x) == 2) { // npc TYPE DISTANCE: in front of the player
        vec3 f = view_forward();
        f.z = 0.0f;
        f = v3_norm(f);
        npc_spawn(i, v3_add(G.player.origin, v3_scale(f, x)), G.yaw + 180.0f);
    } else if (sscanf(line, "balloon %d", &i) == 1 && i >= 0 && i < MAX_BODIES && G.phys.bodies[i].active) {
        Body *b = &G.phys.bodies[i];
        b->balloons++;
        b->lift = b->balloons * BALLOON_FORCE / b->mass;
    } else if (sscanf(line, "menu %d", &i) == 1) {
        G.qmenu = 1;
        G.focus = i % 3;
    } else if (sscanf(line, "quickchat %d %d", &i, &j) == 2) { // quickchat PAGE SLICE: as if picked on the wheel
        quick_chat_send(i, j);
    } else if (sscanf(line, "qcwheel %d %d", &i, &j) == 2) { // the wheel open on PAGE with SLICE picked
        g_qc.open = 1;
        g_qc.page = i;
        g_qc.sel = j;
        g_qc.test = 1;
    } else if (strncmp(line, "screenshot", 10) == 0) { // Pause > Screenshot
        if (g_shot) {
            G.paused = 1;
            g_shot_pending = 1;
        }
    } else if (strncmp(line, "pause saves", 11) == 0) { // Pause > Saves
        G.paused = 1;
        G.pause_saves = 1;
        saves_open(G.map_name);
    } else if (strncmp(line, "pause", 5) == 0) {
        G.paused = 1;
    } else if (strncmp(line, "autosave ", 9) == 0) { // autosave NAME: savebuild, but quiet with nothing to save
        char nm[BUILD_NAME_LEN];
        snprintf(nm, sizeof(nm), "%s", line + 9);
        nm[strcspn(nm, "\r\n")] = 0;
        if (build_capture(&g_build) > 0 && build_write(nm, &g_build) == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Autosaved %d props as %s", g_build.num_props, nm);
            if (g_log) g_log(msg);
        }
    } else if (strncmp(line, "savebuild ", 10) == 0) { // savebuild NAME
        char nm[BUILD_NAME_LEN];
        snprintf(nm, sizeof(nm), "%s", line + 10);
        nm[strcspn(nm, "\r\n")] = 0;
        save_build(nm);
    } else if (strncmp(line, "loadbuild ", 10) == 0 || strncmp(line, "loadhere ", 9) == 0) { // where you look / built
        int here = line[4] == 'h';
        char nm[BUILD_NAME_LEN];
        snprintf(nm, sizeof(nm), "%s", line + (here ? 9 : 10));
        nm[strcspn(nm, "\r\n")] = 0;
        load_build(nm, here);
    } else if (strncmp(line, "cleanup", 7) == 0) {
        cleanup_props();
    } else if (sscanf(line, "tool %d", &i) == 1) {
        G.tool = (Tool)(i % TOOL_COUNT);
        G.weapon = WEAPON_TOOLGUN;
    } else if (sscanf(line, "face %d", &i) == 1) { // the Face Poser's expression
        g_face_sel = i % npc_face_count();
    } else if (sscanf(line, "hand %d", &i) == 1) { // the Finger Poser's hand pose
        g_hand_sel = i % npc_hand_count();
    } else if (sscanf(line, "npcpos %d", &i) == 1) { // where NPC i is, and whether it's following a route
        char msg[64];
        npc_nav_debug(i, msg, sizeof(msg));
        notify(msg, NOTE_HINT);
    } else if (strncmp(line, "navinfo", 7) == 0) { // how big the map's NPC node graph is
        char msg[48];
        snprintf(msg, sizeof(msg), "Nav: %d nodes", nav_node_count());
        notify(msg, NOTE_HINT);
    } else if (strncmp(line, "notify", 6) == 0) {
        notify(line + 7, NOTE_UNDO);
    } else if (strncmp(line, "debug", 5) == 0) {
        g_settings.debug = 1;
    } else if (strncmp(line, "flashlight", 10) == 0) {
        g_flashlight = 1;
    } else if (strncmp(line, "noclip", 6) == 0) {
        G.player.noclip = 1;
    } else if (strncmp(line, "noai", 4) == 0) { // NPCs stand still (Options > NPC thinking off), for looking at them
        g_settings.npc_think = 0;
    } else if (sscanf(line, "spawnat %f %n", &x, &i) == 1) { // spawnat DISTANCE PROP NAME: in front of the player
        const char *name = line + i;
        char nm[32];
        snprintf(nm, sizeof(nm), "%s", name);
        nm[strcspn(nm, "\r\n")] = 0;
        int model = G.props_loaded ? prop_named(nm) : -1;
        vec3 f = view_forward();
        f.z = 0.0f;
        f = v3_norm(f);
        if (model >= 0) g_test_body = spawn_prop(model, v3_mad(G.player.origin, f, x), v3(0, 0, 1), spawn_rot(model), 0xFFFFFFFF);
    } else if (sscanf(line, "shots %d", &i) == 1) { // fires the weapon in hand i times, fanned across the view
        vec3 eye = player_eye(&G.player), f = view_forward(), r = view_right();
        for (int k = 0; k < i; k++) {
            vec3 d = v3_norm(v3_add(f, v3_add(v3_scale(r, 0.25f * ((float)(k % 5) / 2.0f - 1.0f)),
                                               v3(0, 0, 0.12f * ((float)(k / 5) - 1.0f)))));
            shoot(G.weapon, eye, d, (uint32_t)k, 1, 1);
        }
    } else if (strncmp(line, "qtest", 5) == 0) { // spawn menu: a couple of Recent entries, TF2 open, on Vehicles
        qmenu_test_state();
    } else if (strncmp(line, "qcat ", 5) == 0) { // qcat NAME: the spawn menu open on that list (qcat Weapons)
        char nm[32];
        snprintf(nm, sizeof(nm), "%s", line + 5);
        nm[strcspn(nm, "\r\n")] = 0;
        qmenu_test_list(nm);
    } else if (sscanf(line, "qpick %d", &i) == 1) { // qpick N: the open list's Nth tile, as if X was pressed on it
        qmenu_test_pick(i);
    } else if (strncmp(line, "wsel", 4) == 0) { // the weapon selector up for 5 seconds: what you carry
        G.wsel_time = 5.0f;
    } else if (strncmp(line, "thirdperson", 11) == 0) {
        g_settings.third_person = 1;
    } else if (strncmp(line, "drive", 5) == 0) { // get into what spawnat made
        if (g_test_body >= 0) vehicle_enter(g_test_body);
    } else if (sscanf(line, "autodrive %f %f", &x, &y) == 2) { // hold the stick: throttle, steer
        g_test_drive = 1;
        g_test_throttle = x;
        g_test_steer = y;
    } else if (sscanf(line, "autowalk %f", &x) == 1) { // hold the stick forward (or back), for walking into doors
        g_test_walk = x;
    } else if (sscanf(line, "look %f %f", &x, &y) == 2) { // look YAW PITCH
        G.yaw = x;
        G.pitch = y;
    } else if (sscanf(line, "turn %f %f", &x, &y) == 2) { // turn DYAW PITCH: yaw from where it faces now
        G.yaw += x;
        G.pitch = y;

    } else if (sscanf(line, "physrot %f %f", &x, &y) == 2) { // physics gun: grab what's in front, hold X, push the stick
        G.weapon = WEAPON_PHYSGUN;
        g_test_hold = PSP_CTRL_RTRIGGER | PSP_CTRL_CROSS;
        g_test_ax = x, g_test_ay = y;
    } else if (strncmp(line, "physrot off", 11) == 0) {
        g_test_hold = 0;
    } else if (sscanf(line, "hold %x", &i) == 1) { // hold buttons: a PSP_CTRL_ mask in hex (1 is Select), 0 lets go
        g_test_hold = (unsigned int)i;
        g_test_ax = g_test_ay = 0.0f;
    } else if (sscanf(line, "targetfps %d", &i) == 1) { // Options > Target FPS, by index (not saved)
        g_settings.target_fps = i;
    } else if (sscanf(line, "lod %d", &i) == 1) { // Options > LOD distance, 1 to 5 (not saved)
        g_settings.lod = i;

    } else if (strncmp(line, "toolfire", 8) == 0) { // the tool gun's R, at what's in front
        toolgun_fire();
    } else if (strncmp(line, "useaim", 6) == 0) { // Use on the prop in front (thrusters, wheels, lamps...)
        test_use_aim();
    } else if (strncmp(line, "usetest", 7) == 0) { // Use on the prop "spawnat" made
        if (g_test_body >= 0) test_use_body(g_test_body);
    } else if (strncmp(line, "use", 3) == 0) { // presses Use on what's in front (doors, buttons)
        ents_use(&G.map, player_eye(&G.player), view_forward());


    } else if (strncmp(line, "mute", 4) == 0) {

        g_settings.sound = 0;
    } else if (strncmp(line, "host", 4) == 0) {
        game_host();
    } else if (strncmp(line, "hurt", 4) == 0) {
        damage_player(30.0f);
    } else if (sscanf(line, "rag %d %f", &i, &x) == 2) { // rag TYPE DISTANCE: a ragdoll in front of the player
        vec3 f = view_forward();
        f.z = 0.0f;
        spawn_ragdoll(i, v3_add(G.player.origin, v3_scale(v3_norm(f), x)), G.yaw + 180.0f);
    } else if (strncmp(line, "duck", 4) == 0) { // crouch on or off
        G.player.ducked = !G.player.ducked;
    } else if (strncmp(line, "ragfreeze", 9) == 0) { // every ragdoll stays as it is (standing, if just spawned)
        for (int r = 0; r < MAX_RAGDOLLS; r++) npc_ragdoll_freeze(r, 1);
    } else if (sscanf(line, "weapon %d", &i) == 1 && i >= 0 && i < WEAPON_COUNT) {
        G.weapon = (Weapon)i;
    } else if (sscanf(line, "vmloop %d", &i) == 1) { // loops one first-person animation (VM_*), for tests
        G.vm_last_weapon = (int)G.weapon;
        if (G.vm_anim[G.weapon] >= 0) vmodel_play(G.vm_anim[G.weapon], i, 1);
    } else if (strncmp(line, "launch", 6) == 0) { // fires the weapon in hand's projectile (crossbow, RPG...)
        if (is_launcher(G.weapon)) launch(G.weapon, player_eye(&G.player), view_forward());
    } else if (strncmp(line, "squeeze", 7) == 0) { // squeezes bug bait: antlions follow
        npc_bugbait_squeeze(G.my_slot);
    } else if (sscanf(line, "unlimited %d", &i) == 1) { // Options > Unlimited ammo (not saved)
        g_settings.unlimited_ammo = i != 0;
    } else if (sscanf(line, "ammo %d", &i) == 1) { // every gun's spare ammo, for testing ammo boxes
        for (int w = 0; w < WEAPON_COUNT; w++) G.ammo[w] = i;
    } else if (sscanf(line, "health %f", &x) == 1) { // sets health / suit power, for testing items
        G.health = clampf(x, 1.0f, 100.0f);
    } else if (sscanf(line, "suit %f", &x) == 1) {
        G.suit = clampf(x, 0.0f, 100.0f);
    } else if (strncmp(line, "charge", 6) == 0) { // Use on the charger in front
        test_use_item();
    } else if (strncmp(line, "killall", 7) == 0) { // every NPC dies, knocked away from the player
        for (int n = 0; n < MAX_NPCS; n++) npc_damage_dir(n, 10000.0f, v3_scale(view_forward(), 400.0f));
    }
}

// --- Helpers ---------------------------------------------------------------------------------

static vec3 view_forward(void) {
    float cp = cosf(G.pitch * DEG2RAD), sp = sinf(G.pitch * DEG2RAD);
    float cy = cosf(G.yaw * DEG2RAD), sy = sinf(G.yaw * DEG2RAD);
    return v3(cp * cy, cp * sy, sp);
}

static vec3 view_right(void) {
    float cy = cosf(G.yaw * DEG2RAD), sy = sinf(G.yaw * DEG2RAD);
    return v3(sy, -cy, 0.0f);
}

// Grip position of the view model, with walking bob, switch-lowering and recoil applied.
static vec3 vm_grip(void) {
    const ViewModel *vm = &k_vm[G.weapon];
    vec3 f = view_forward(), r = view_right();
    vec3 u = v3_cross(r, f);
    vec3 eye = player_eye(&G.player);
    float bob_x = sinf(G.bob_phase) * 0.5f, bob_y = fabsf(cosf(G.bob_phase)) * 0.5f;
    float lower = G.switch_time > 0.0f ? G.switch_time * 40.0f : 0.0f;
    vec3 p = v3_mad(eye, f, vm->fwd - G.kick);
    p = v3_mad(p, r, vm->right + bob_x);
    p = v3_mad(p, u, -(vm->down + bob_y + lower));
    return p;
}

// Where the first-person model's origin sits: the eye, with walking bob, switch-lowering and recoil
static vec3 vm_origin(void) {
    vec3 f = view_forward(), r = view_right();
    vec3 u = v3_cross(r, f);
    float bob_x = sinf(G.bob_phase) * 0.5f, bob_y = fabsf(cosf(G.bob_phase)) * 0.5f;
    float lower = G.switch_time > 0.0f ? G.switch_time * 40.0f : 0.0f;
    vec3 p = v3_mad(player_eye(&G.player), f, -G.kick);
    p = v3_mad(p, r, bob_x);
    return v3_mad(p, u, -(bob_y + lower));
}

// The first-person model is drawn with its own narrower field of view; this is how much bigger
// sideways offsets look than in the world's projection.
static float vm_fov_scale(void) {
    return tanf(G.cam.fov_y * 0.5f * DEG2RAD) / tanf(VM_FOV_Y * 0.5f * DEG2RAD);
}

static vec3 gun_muzzle(void) {
    if (G.vm_c[G.weapon] >= 0) {
        vec3 f = view_forward(), r = view_right();
        vec3 u = v3_cross(r, f), m = G.vm_muzzle[G.weapon];
        float k = vm_fov_scale();
        vec3 p = v3_mad(vm_origin(), f, m.x);
        p = v3_mad(p, r, -m.y * k);
        return v3_mad(p, u, m.z * k);
    }
    return v3_mad(vm_grip(), view_forward(), k_vm[G.weapon].length);
}

static int build_prop_boxes(CmBox *boxes, int skip) {
    int n = 0;
    vec3 eye = player_eye(&G.player);
    for (int i = 0; i < MAX_BODIES; i++) {
        const Body *b = &G.phys.bodies[i];
        if (!b->active || i == skip || b->life > 0.0f) continue; // broken pieces are too small to block anyone
        // Only nearby props matter for the player's movement
        if (fabsf(b->pos.x - eye.x) > 400.0f || fabsf(b->pos.y - eye.y) > 400.0f || fabsf(b->pos.z - eye.z) > 400.0f) continue;
        phys_body_cmbox(b, &boxes[n], i);
        n++;
    }
    return n + npc_boxes(boxes + n, MAX_NPCS + NET_MAX_PLAYERS);
}

typedef struct {
    int rag, bone; // ragdoll (and which bone) hit first, or -1
    int npc;     // NPC hit first, or -1
    int body;    // -1 if the world was hit first (or nothing)
    float dist;
    vec3 point;
    vec3 normal; // world normal (when body < 0)
    int hit_world;
} Aim;

static Aim aim_trace(float range) {
    Aim a;
    vec3 eye = player_eye(&G.player), f = view_forward();
    Trace tr;
    cm_trace_line(&G.map, eye, v3_mad(eye, f, range), MASK_PROPSOLID, NULL, 0, &tr);
    float world_t = tr.fraction * range;
    float t;
    a.body = phys_raycast(&G.phys, eye, f, world_t, &t);
    if (a.body >= 0) {
        a.dist = t;
        a.hit_world = 0;
        a.normal = v3_scale(f, -1.0f);
    } else {
        a.dist = world_t;
        a.hit_world = tr.fraction < 1.0f;
        a.normal = tr.normal;
    }
    a.npc = npc_raycast(eye, f, a.dist, &t);
    if (a.npc >= 0) {
        a.body = -1;
        a.dist = t;
        a.hit_world = 0;
        a.normal = v3_scale(f, -1.0f);
    }
    a.bone = 0;
    a.rag = npc_ragdoll_raycast(eye, f, a.dist, &t, &a.bone);
    if (a.rag >= 0) {
        a.body = a.npc = -1;
        a.dist = t;
        a.hit_world = 0;
        a.normal = v3_scale(f, -1.0f);
    }
    a.point = v3_mad(eye, f, a.dist);
    return a;
}

// Each player has their own undo list; the host keeps remote players' lists in their Peer
static void undo_list(int slot, int **list, int **count) {
    if (slot == 0) {
        *list = G.undo;
        *count = &G.undo_count;
    } else {
        *list = G.peers[slot].undo;
        *count = &G.peers[slot].undo_count;
    }
}

static void undo_push(int body) {
    int *list, *count;
    undo_list(G.acting, &list, &count);
    if (*count == UNDO_MAX) {
        memmove(list, list + 1, sizeof(int) * (UNDO_MAX - 1));
        (*count)--;
    }
    list[(*count)++] = body;
}

static void remove_body(int i) {
    if (G.held == i) {
        G.held = -1;
        if (G.hold_sound >= 0) snd_stop(G.hold_sound);
        G.hold_sound = -1;
    }
    if (G.stage && G.stage_body == i) G.stage = 0;
    phys_remove(&G.phys, i);
}

// --- Tool actions (single player and host; clients send a command for these) ------------------------

#define THRUSTER_FORCE 40000.0f // kg u/s^2: lifts about 65 kg against gravity
#define EXPLOSION_RADIUS 320.0f
#define EXPLOSION_SPEED 1100.0f // what a 50 kg prop right next to it flies off at
#define EXPLOSION_DAMAGE 120.0f
static const float k_weights[] = {1.0f, 10.0f, 50.0f, 250.0f, 1000.0f};
#define NUM_WEIGHTS (int)(sizeof(k_weights) / sizeof(k_weights[0]))

// The next weight up from a prop's mass (wrapping round to the lightest)
static float next_weight(float mass) {
    for (int k = 0; k < NUM_WEIGHTS; k++) {
        if (k_weights[k] > mass + 0.5f) return k_weights[k];
    }
    return k_weights[0];
}

static void show_explosion(vec3 c) {
    G.blast_pos = c;
    G.blast_time = 0.35f;
    play_at(G.snd.explode, c, 1.0f);
}

static int break_group(const Body *b);
static void break_body(int i);

static void fx_explosion(vec3 c);
static void pvp_damage(int victim, int attacker, float amount);

// An explosion at c (the Explosive tool, rockets, grenades): breakables near it shatter, everything's thrown,
// and NPCs and players near it are hurt. attacker: the player slot behind it, for kill notices.
static void explode_at(vec3 c, int attacker) {
    // Breakables close by shatter, then everything gets thrown
    for (int k = 0; k < MAX_BODIES; k++) {
        const Body *o = &G.phys.bodies[k];
        if (o->active && break_group(o) && v3_len(v3_sub(o->pos, c)) < EXPLOSION_RADIUS * 0.8f) break_body(k);
    }
    phys_blast(&G.phys, c, EXPLOSION_RADIUS, EXPLOSION_SPEED);
    show_explosion(c);
    fx_explosion(c); // and multiplayer clients see it too
    g_npc_attacker = attacker;
    for (int n = 0; n < MAX_NPCS; n++) {
        if (!npc_active(n)) continue;
        NpcNet nn;
        npc_net_get(n, &nn);
        vec3 d = v3_sub(v3(nn.pos[0], nn.pos[1], nn.pos[2] + 36.0f), c);
        float dist = v3_len(d);
        if (dist < EXPLOSION_RADIUS) {
            float k = 1.0f - dist / EXPLOSION_RADIUS;
            npc_damage_dir(n, EXPLOSION_DAMAGE * k, v3_scale(v3_norm(v3(d.x, d.y, d.z + 40.0f)), 600.0f * k));
            blood_spray(v3(nn.pos[0], nn.pos[1], nn.pos[2] + 36.0f), v3_norm(d), 10);
        }
    }
    g_npc_attacker = -1;
    vec3 d = v3_sub(v3_add(G.player.origin, v3(0, 0, 36)), c);
    float dist = v3_len(d);
    if (dist < EXPLOSION_RADIUS && !G.player.noclip && (!g_gm.on || script_should_damage(G.my_slot, attacker))) {
        float k = 1.0f - dist / EXPLOSION_RADIUS;
        g_gm.p[G.my_slot].attacker = attacker;
        damage_player(EXPLOSION_DAMAGE * 0.6f * k);
        G.player.velocity = v3_mad(G.player.velocity, v3_norm(v3(d.x, d.y, d.z + 60.0f)), 500.0f * k);
        G.player.on_ground = 0;
    }
    // The other players, when hosting (their PSPs move them, so they're hurt but not thrown)
    for (int s = 1; s < NET_MAX_PLAYERS && G.role == ROLE_HOST; s++) {
        const Peer *p = &G.peers[s];
        if (!p->active || p->health <= 0.0f || (p->flags & PF_NOCLIP)) continue;
        float pd = v3_len(v3_sub(v3_add(p->pos, v3(0, 0, 36)), c));
        if (pd < EXPLOSION_RADIUS) pvp_damage(s, attacker, EXPLOSION_DAMAGE * 0.6f * (1.0f - pd / EXPLOSION_RADIUS));
    }
}

static void do_explode(int i) {
    vec3 c = G.phys.bodies[i].pos;
    remove_body(i);
    explode_at(c, G.acting ? G.acting : G.my_slot);
}

static int body_valid(int i);

// --- Devices: lamps, lights, buttons and emitters ---------------------------------------------------
// Each sits on a prop (its own model, welded to what it was put on, or frozen in place on the world).
// The host owns them and sends them to clients (PK_DEVICES); everyone draws their light and particles.
enum { DEV_LAMP = 1, DEV_LIGHT, DEV_BUTTON, DEV_EMITTER };
enum { EMIT_SPARKS, EMIT_SMOKE, EMIT_FIRE, EMIT_COUNT };
static const char *const k_emit_names[EMIT_COUNT] = {"Sparks", "Smoke", "Fire"};
#define MAX_DEVICES 24
#define LAMP_RANGE 900.0f
#define LIGHT_RANGE 320.0f
#define WHEEL_SPEED 15.0f // rad/s: about 250 u/s on the Wheel tool's tyre
typedef struct {
    int type; // DEV_*, 0 = free
    int body;
    int on;
    vec3 lpos, ldir; // in the body's frame
    int colour;      // palette index (lamps, lights)
    int effect;      // EMIT_*
    int target;      // button: the prop whose contraption it switches
    float emit_acc, press; // emitter: particles owed; button: pressed-in time left
} Device;
static Device g_dev[MAX_DEVICES];
static int g_emit_effect; // the Emitter tool's effect (L changes it)

#define MAX_PARTICLES 160
typedef struct {
    vec3 pos, vel;
    float life, max_life, size;
    int effect;
} Particle;
static Particle g_parts[MAX_PARTICLES];

static void devices_clear(void) {
    memset(g_dev, 0, sizeof(g_dev));
    memset(g_parts, 0, sizeof(g_parts));
}

static int device_add(int type, int body, vec3 world_pos, vec3 world_dir, int colour, int effect, int target) {
    for (int d = 0; d < MAX_DEVICES; d++) {
        Device *v = &g_dev[d];
        if (v->type) continue;
        const Body *b = &G.phys.bodies[body];
        memset(v, 0, sizeof(*v));
        v->type = type;
        v->body = body;
        v->on = type != DEV_BUTTON;
        v->lpos = q_rotate_inv(b->rot, v3_sub(world_pos, b->pos));
        v->ldir = q_rotate_inv(b->rot, world_dir);
        v->colour = colour;
        v->effect = effect;
        v->target = target;
        return d;
    }
    return -1;
}

static void device_frame(const Device *v, vec3 *pos, vec3 *dir) {
    const Body *b = &G.phys.bodies[v->body];
    *pos = v3_add(b->pos, q_rotate(b->rot, v->lpos));
    *dir = q_rotate(b->rot, v->ldir);
}

static int body_has_device(int body) {
    for (int d = 0; d < MAX_DEVICES; d++) {
        if (g_dev[d].type && g_dev[d].body == body) return 1;
    }
    return 0;
}

// The rotation turning +Z onto n (Wheel, Light, Button and Emitter models stand on +Z)
static quat quat_z_to(vec3 n) {
    vec3 z = v3(0, 0, 1);
    float c = v3_dot(z, n);
    if (c < -0.999f) return q_axis_angle(v3(1, 0, 0), PI);
    vec3 axis = v3_cross(z, n);
    float s = v3_len(axis);
    if (s < 1e-5f) return q_identity();
    return q_axis_angle(v3_scale(axis, 1.0f / s), atan2f(s, c));
}

// A device's own model on a surface: welded to the prop it's on, or frozen on the world
static int place_part(const char *model_name, quat rot, int body, vec3 p, vec3 n) {
    int m = prop_named(model_name);
    if (m < 0) {
        toast("No model for that tool in the prop pack");
        return -1;
    }
    int i = spawn_prop(m, p, n, rot, 0xFFFFFFFF);
    if (i < 0) return -1;
    if (body >= 0 && body_valid(body)) {
        phys_weld(&G.phys, i, body, p);
    } else {
        G.phys.bodies[i].frozen = 1;
    }
    return i;
}

// Host (and single player): the Wheel, Winch, Hydraulic, Lamp, Light, Button and Emitter tools.
// body/p/n: what was aimed at; sbody/v: the first pick of the two-click tools, or the view direction.
static void tool_apply(int tool, int body, vec3 p, vec3 n, int sbody, vec3 v, int colour, int effect) {
    if (body >= 0 && !body_valid(body)) body = -1;
    if (sbody >= 0 && !body_valid(sbody)) sbody = -1;
    colour = colour >= 0 && colour < PALETTE_SIZE ? colour : 0;
    switch (tool) {
    case TOOL_WHEEL: {
        if (body < 0) {
            toast("Put wheels on a prop");
            return;
        }
        int w = place_part("_wheel", quat_z_to(n), -1, p, n);
        if (w < 0) return;
        G.phys.bodies[w].frozen = 0;
        // Its motor turns it so it rolls the way you were facing (spin = up x forward)
        vec3 f = v3_norm(v3(v.x, v.y, 0.0f));
        float sign = v3_dot(v3_cross(v3(0, 0, 1), f), n) >= 0.0f ? 1.0f : -1.0f;
        if (phys_axis(&G.phys, w, body, G.phys.bodies[w].pos, n, sign * WHEEL_SPEED) < 0) {
            remove_body(w);
            toast("Too many constraints");
            return;
        }
        play(G.snd.weld, 0.6f);
        toast("Wheel added (Use switches its motor)");
        return;
    }
    case TOOL_WINCH:
    case TOOL_HYDRAULIC: {
        if (sbody < 0 && body < 0) {
            toast("It needs a prop on at least one end");
            return;
        }
        if (sbody == body || v3_len(v3_sub(p, v)) < 8.0f) {
            toast("Pick two different points");
            return;
        }
        int ok = tool == TOOL_WINCH ? phys_winch(&G.phys, sbody, v, body, p, 120.0f) >= 0
                                    : phys_hydraulic(&G.phys, sbody, v, body, p, 2.0f, 80.0f) >= 0;
        if (!ok) {
            toast("Too many constraints");
            return;
        }
        play(G.snd.weld, 0.6f);
        toast(tool == TOOL_WINCH ? "Winch added (Use: reel in / out)" : "Hydraulic added (Use: push / pull)");
        return;
    }
    case TOOL_LAMP:
    case TOOL_LIGHT:
    case TOOL_EMITTER: {
        // The lamp's model shines along its Y axis; the light's and the emitter's stand on Z
        quat rot = tool == TOOL_LAMP ? q_mul(quat_z_to(n), q_axis_angle(v3(1, 0, 0), PI * 0.5f)) : quat_z_to(n);
        int i = place_part(tool == TOOL_LAMP ? "_lamp" : tool == TOOL_LIGHT ? "_light" : "_emitter", rot, body, p, n);
        if (i < 0) return;
        const Body *b = &G.phys.bodies[i];
        int type = tool == TOOL_LAMP ? DEV_LAMP : tool == TOOL_LIGHT ? DEV_LIGHT : DEV_EMITTER;
        vec3 at = tool == TOOL_LIGHT ? b->pos : v3_mad(b->pos, n, 6.0f);
        if (device_add(type, i, at, n, colour, effect % EMIT_COUNT, -1) < 0) {
            remove_body(i);
            toast("Too many lamps, lights, buttons and emitters");
            return;
        }
        play(G.snd.weld, 0.6f);
        toast(tool == TOOL_LAMP ? "Lamp placed (Use switches it)" : tool == TOOL_LIGHT ? "Light placed (Use switches it)"
                                                                                      : "Emitter placed (Use switches it)");
        return;
    }
    case TOOL_BUTTON: {
        if (sbody < 0) {
            toast("Pick the prop it switches first");
            return;
        }
        int i = place_part("_button", quat_z_to(n), body, p, n);
        if (i < 0) return;
        if (device_add(DEV_BUTTON, i, G.phys.bodies[i].pos, n, colour, 0, sbody) < 0) {
            remove_body(i);
            toast("Too many lamps, lights, buttons and emitters");
            return;
        }
        play(G.snd.weld, 0.6f);
        toast("Button placed (Use it to switch)");
        return;
    }
    default:
        return;
    }
}

// What Use switches on a prop: its thrusters and hoverballs, the wheel motors, winches and hydraulics on
// it, its lamps, lights and emitters; a button switches the contraption it was linked to.
static int toggle_contraption(int seat);
static int toggle_forces(int i) {
    int n = phys_toggle_forces(&G.phys, i) + phys_toggle_joints(&G.phys, i);
    for (int d = 0; d < MAX_DEVICES; d++) {
        Device *v = &g_dev[d];
        if (!v->type || v->body != i) continue;
        n++;
        if (v->type == DEV_BUTTON) {
            static int snd = -2;
            if (snd == -2) snd = snd_find("button");
            play_at(snd, G.phys.bodies[i].pos, 0.8f);
            v->press = 0.3f;
            if (body_valid(v->target)) toggle_contraption(v->target);
        } else {
            v->on = !v->on;
        }
    }
    return n;
}

static void test_use_aim(void) {
    Aim a = aim_trace(1500.0f);
    if (a.body >= 0) toggle_forces(a.body);
}

static void test_use_body(int body) {
    char msg[48];
    int n = toggle_forces(body);
    int joints = 0;
    for (int j = 0; j < MAX_JOINTS; j++) joints += G.phys.joints[j].active;
    snprintf(msg, sizeof(msg), "usetest body %d: %d switched, %d joints", body, n, joints);
    toast(msg);
}



// From a seat or a button: everything that switches on whatever is welded, roped or hinged to it (GMod

// wires them to keys; here one press switches them all). Returns how many there are.
static int toggle_contraption(int seat) {
    unsigned char seen[MAX_BODIES];
    int queue[MAX_BODIES], head = 0, tail = 0, n = 0;
    memset(seen, 0, sizeof(seen));
    seen[seat] = 1;
    queue[tail++] = seat;
    while (head < tail) {
        int i = queue[head++];
        n += phys_toggle_forces(&G.phys, i);
        for (int d = 0; d < MAX_DEVICES; d++) {
            Device *v = &g_dev[d];
            if (v->type && v->type != DEV_BUTTON && v->body == i) {
                v->on = !v->on;
                n++;
            }
        }
        for (int j = 0; j < MAX_JOINTS; j++) {
            const Joint *jt = &G.phys.joints[j];
            if (!jt->active) continue;
            int other = jt->a == i ? jt->b : jt->b == i ? jt->a : -1;
            if (other >= 0 && !seen[other] && G.phys.bodies[other].active) {
                seen[other] = 1;
                queue[tail++] = other;
            }
        }
    }
    // Each wheel motor, winch and hydraulic once, though both its props are in the contraption
    for (int j = 0; j < MAX_JOINTS; j++) {
        const Joint *jt = &G.phys.joints[j];
        if (jt->active && (seen[jt->a] || (jt->b >= 0 && seen[jt->b]))) n += phys_toggle_joint(&G.phys, j);
    }
    return n;
}


static quat yaw_quat(float yaw_deg) {
    return q_axis_angle(v3(0, 0, 1), yaw_deg * DEG2RAD);
}

// How a new prop is turned: facing you, except vehicles and seats, which point the way you look so
// you can get straight in (the Jeep and Airboat face -Y in their models, seats +X)
static quat spawn_rot(int model) {
    Body probe;
    memset(&probe, 0, sizeof(probe));
    probe.active = 1;
    probe.model = model;
    int kind = vehicle_kind(&probe);
    if (kind == VEH_JEEP || kind == VEH_AIRBOAT) return yaw_quat(G.yaw + 90.0f);
    if (kind == VEH_SEAT) return yaw_quat(G.yaw);
    return yaw_quat(G.yaw + 180.0f);
}


static int prop_named(const char *name);

// The Camera tool's prop, -1 in packs made before it
static int camera_model(void) {
    return G.props_loaded ? prop_named("_camera") : -1;
}

static int is_camera(int body) {
    if (body < 0 || body >= MAX_BODIES) return 0;
    const Body *b = &G.phys.bodies[body];
    return b->active && b->lib == 0 && b->model >= 0 && b->model == camera_model();
}

// A spawnable prop's body: at pos (its shape's centre), or with sit_on (a surface normal) sat on the
// surface at pos. Loads an extra prop's model first (making room if it must). Returns the body, or
// -1 when the world is full and -2 when there's no memory for the model.
static int new_body(int model, vec3 pos, quat rot, unsigned int tint, const vec3 *sit_on) {
    PpakModel tmp;
    const PpakModel *md = G.props_loaded ? model_info(model, &tmp) : NULL;
    if (!md) return -1;
    int local;
    const PropLib *lib = model_lib(model, &local);
    if (!lib) {
        release_unused_extras();
        lib = model_lib(model, &local);
    }
    if (!lib) return -2;
    md = &lib->models[local]; // exact bounds, now it's loaded
    vec3 mins = v3(md->mins[0], md->mins[1], md->mins[2]);
    vec3 maxs = v3(md->maxs[0], md->maxs[1], md->maxs[2]);
    if (sit_on) {
        // Sit the shape on the surface we hit: lift it along the normal by its extent in that direction.
        vec3 half = v3_scale(v3_sub(maxs, mins), 0.5f);
        vec3 ln = q_rotate_inv(rot, *sit_on);
        float lift = fabsf(ln.x) * half.x + fabsf(ln.y) * half.y + fabsf(ln.z) * half.z + 1.0f;
        pos = v3_mad(pos, *sit_on, lift);
    }
    int i = phys_spawn(&G.phys, model, (int)(md->flags & PPAK_SHAPE_MASK), mins, maxs, md->mass, pos, rot);
    if (i < 0) return -1;
    attach_hull(&G.phys.bodies[i], lib, local);
    G.phys.bodies[i].tint_abgr = tint;
    G.phys.bodies[i].surface = (int)md->surface;
    if (model == camera_model()) G.phys.bodies[i].frozen = 1; // cameras hang where they're put, as in GMod
    if (is_charger(item_kind(i))) G.phys.bodies[i].frozen = 1; // and chargers, as on a wall
    G.body_owner[i] = G.acting;
    return i;
}

static int spawn_prop(int model, vec3 at, vec3 normal, quat rot, unsigned int tint) {
    PpakModel tmp;
    const PpakModel *md = G.props_loaded ? model_info(model, &tmp) : NULL;
    if (!md) return -1;
    if (is_client()) {
        float f[10] = {at.x, at.y, at.z, normal.x, normal.y, normal.z, rot.x, rot.y, rot.z, rot.w};
        send_cmd(CMD_SPAWN, model, 0, tint, f, 10);
        play(G.snd.spawn, 0.6f);
        achieve_prop_spawned();
        return -1;
    }
    if (player_prop_count() >= PLAYER_PROP_LIMIT) {
        toast("Prop limit reached - remove some first");
        return -1;
    }
    int i = new_body(model, at, rot, tint, &normal);
    if (i == -2) {
        toast("Not enough memory - remove some props first");
        return -1;
    }
    if (i < 0) {
        toast("Too many physics objects");
        return -1;
    }
    undo_push(i);
    play(G.snd.spawn, 0.6f);
    if (G.acting == G.my_slot) achieve_prop_spawned(); // ours, not a client's we spawned for them
    return i;
}

// --- Breakables -----------------------------------------------------------------------------------

#define BREAK_SPEED 250.0f // impact speed (u/s) that shatters a breakable: about a 1.5 m drop
#define PIECE_LIFE 8.0f    // seconds broken pieces stay

static float rand_unit(void) {
    return (float)(rand() % 2001 - 1000) * 0.001f;
}

// Which break group a body shatters into (0: it doesn't break)
static int break_group(const Body *b) {
    if (b->lib != 0 || b->life > 0.0f || b->model < 0 || b->model >= G.props.num_models) return 0;
    return (int)((G.props.models[b->model].flags & PPAK_BREAK_MASK) >> PPAK_BREAK_SHIFT);
}

// Host: swaps a breakable prop for 2-4 of its group's pieces, flying apart, which go after a while
static void break_body(int i) {
    Body *b = &G.phys.bodies[i];
    int g = break_group(b);
    if (!g) return;
    vec3 pos = b->pos, vel = b->vel, h = b->half;
    quat rot = b->rot;
    int surf = b->surface;
    unsigned int tint = b->tint_abgr;
    remove_body(i);
    play_at(surf == SURF_GLASS ? G.snd.break_glass : surf == SURF_FLESH ? G.snd.break_flesh : G.snd.break_hard, pos, 0.9f);
    int n = G.num_gibs[g];
    int pieces = n ? 2 + rand() % 3 : 0;
    for (int k = 0; k < pieces; k++) {
        int model = G.gibs[g][rand() % n];
        const PpakModel *md = &G.props.models[model];
        vec3 off = v3(rand_unit() * h.x * 0.6f, rand_unit() * h.y * 0.6f, rand_unit() * h.z * 0.6f);
        quat r = q_mul(rot, q_axis_angle(v3_norm(v3(rand_unit(), rand_unit(), 0.5f)), rand_unit() * 3.0f));
        int j = phys_spawn(&G.phys, model, (int)(md->flags & PPAK_SHAPE_MASK), v3(md->mins[0], md->mins[1], md->mins[2]),
                           v3(md->maxs[0], md->maxs[1], md->maxs[2]), md->mass, v3_add(pos, q_rotate(rot, off)), r);
        if (j < 0) break;
        attach_hull(&G.phys.bodies[j], &G.props, model);
        Body *p = &G.phys.bodies[j];
        p->surface = (int)md->surface;
        p->tint_abgr = tint;
        p->vel = v3_add(v3_scale(vel, 0.4f), v3(rand_unit() * 120.0f, rand_unit() * 120.0f, 60.0f + (rand() % 120)));
        p->angvel = v3(rand_unit() * 10.0f, rand_unit() * 10.0f, rand_unit() * 10.0f);
        p->life = PIECE_LIFE + (float)(rand() % 100) * 0.02f;
        p->nocollide = 1; // pieces only hit the world: cheaper, and they don't jam between props
        G.body_owner[j] = G.acting;
    }
}

// Host: broken pieces disappear when their time is up
static void expire_pieces(float dt) {
    if (is_client()) return;
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &G.phys.bodies[i];
        if (!b->active || b->life <= 0.0f) continue;
        b->life -= dt;
        if (b->life <= 0.0f) remove_body(i);
    }
}

// A ragdoll at pos facing yaw: on the host (or alone) here, with undo; a client asks the host
static void spawn_ragdoll(int type, vec3 pos, float yaw) {
    if (is_client()) {
        float f[4] = {pos.x, pos.y, pos.z, yaw};
        send_cmd(CMD_RAG_SPAWN, type, 0, 0, f, 4);
        play(G.snd.spawn, 0.6f);
        return;
    }
    int r = npc_ragdoll_spawn(type, pos, yaw);
    if (r >= 0) {
        undo_push(RAGDOLL_ID_BASE + r);
        play(G.snd.spawn, 0.6f);
    }
}

// The spawn menu's Weapons: into your hands, with a full load of ammo if you didn't have it
static void give_weapon(int w) {
    unsigned int bit = 1u << w;
    if (!(g_sandbox_weapons & bit)) {
        g_sandbox_weapons |= bit;
        G.clip[w] = k_ws[w].clip;
        G.ammo[w] = k_ws[w].ammo;
    }
    physgun_release(1);
    gm_switch_weapon(w);
    play(G.snd.ammo, 0.7f);
}

static void spawn_from_menu(int model) {
    recent_add(model);
    if (model >= GRID_WEAPON) {
        if (!g_gm.on) give_weapon(model - GRID_WEAPON); // (from Recent, in a gamemode's game: its weapons only)
        return;
    }
    Aim a = aim_trace(SPAWN_RANGE);
    if (model >= GRID_NPC) {
        // NPCs stand on the floor below where you aim, facing you
        Trace tr;
        vec3 above = v3_mad(a.point, a.normal, 8.0f);
        cm_trace_line(&G.map, above, v3_add(above, v3(0, 0, -512)), MASK_PLAYERSOLID, NULL, 0, &tr);
        if (model >= GRID_RAG) {
            spawn_ragdoll(model - GRID_RAG, v3_add(tr.endpos, v3(0, 0, 4)), G.yaw + 180.0f);
            return;
        }
        if (is_client()) {
            float f[4] = {tr.endpos.x, tr.endpos.y, tr.endpos.z + 2.0f, G.yaw + 180.0f};
            send_cmd(CMD_SPAWN_NPC, model - GRID_NPC, 0, 0, f, 4);
            play(G.snd.spawn, 0.6f);
            return;
        }
        int i = npc_spawn(model - GRID_NPC, v3_add(tr.endpos, v3(0, 0, 2)), G.yaw + 180.0f);
        if (i < 0) {
            toast("Too many NPCs");
            return;
        }
        undo_push(NPC_ID_BASE + i);
        play(G.snd.spawn, 0.6f);
        return;
    }
    vec3 n = a.body < 0 && a.hit_world ? a.normal : v3(0, 0, 1);
    // Face the player, like GMod
    spawn_prop(model, a.point, n, spawn_rot(model), 0xFFFFFFFF);
}

static void update_balloon_lift(Body *b) {
    b->lift = b->balloons * BALLOON_FORCE / b->mass;
}

// --- Weapons ----------------------------------------------------------------------------------

static void physgun_release(int quiet) {
    if (G.held >= 0) {
        G.phys.bodies[G.held].held = 0;
        phys_wake(&G.phys, G.held);
        if (!quiet) play(G.snd.drop, 0.7f);
    }
    if (G.held_rag >= 0 && !quiet) play(G.snd.drop, 0.7f);
    if (G.hold_sound >= 0) snd_stop(G.hold_sound);
    G.hold_sound = -1;
    G.held = -1;
    G.held_rag = -1;
    G.use_carry = 0;
}

static void physgun_drive(int i, vec3 want_pos, quat want_rot);

// Physics gun: holding X while holding a prop turns it with the circle pad (GMod's E and mouse), and
// the player stands still and stops looking down meanwhile
#define PHYSGUN_ROTATE_SPEED 150.0f // degrees a second, stick pushed all the way
static int physgun_rotating(const InputState *in) {
    return G.weapon == WEAPON_PHYSGUN && G.held >= 0 && !G.use_carry && (in->held & PSP_CTRL_RTRIGGER) &&
           (in->held & PSP_CTRL_CROSS);
}

// The physics gun holding a ragdoll by one bone: the rest of it dangles
static void physgun_ragdoll(const InputState *in, float dt) {
    if (!npc_ragdoll_active(G.held_rag)) {
        physgun_release(1);
        return;
    }
    if (!(in->held & PSP_CTRL_RTRIGGER)) {
        physgun_release(0);
        return;
    }
    if (in->pressed & PSP_CTRL_LTRIGGER) {
        npc_ragdoll_freeze(G.held_rag, 1);
        if (is_client()) send_cmd(CMD_RAG_FREEZE, G.held_rag, 1, 0, NULL, 0);
        physgun_release(1);
        play(G.snd.freeze, 0.7f);
        toast("Frozen");
        return;
    }
    if (in->held & PSP_CTRL_UP) G.hold_dist += 300.0f * dt;
    if (in->held & PSP_CTRL_DOWN) G.hold_dist -= 300.0f * dt;
    G.hold_dist = clampf(G.hold_dist, PHYSGUN_MIN_DIST, PHYSGUN_MAX_DIST);
    vec3 target = v3_mad(player_eye(&G.player), view_forward(), G.hold_dist);
    G.net_hold_pos = target; // a client's host drags it (the state it sends says where)
    npc_ragdoll_drag(G.held_rag, G.held_bone, target, dt);
}

static void physgun_update(const InputState *in, float dt) {
    if (G.held_rag >= 0) {
        physgun_ragdoll(in, dt);
        return;
    }
    if ((in->pressed & PSP_CTRL_RTRIGGER) && G.held < 0) {
        Aim a = aim_trace(USE_RANGE);
        if (a.rag >= 0) {
            G.held_rag = a.rag;
            G.held_bone = a.bone;
            G.hold_dist = clampf(a.dist, PHYSGUN_MIN_DIST, PHYSGUN_MAX_DIST);
            npc_ragdoll_freeze(a.rag, 0);
            play(G.snd.pickup, 0.7f);
            if (g_settings.sound) G.hold_sound = snd_loop(G.snd.hold, 0.35f);
            return;
        }
        if (a.body >= 0) {
            Body *b = &G.phys.bodies[a.body];
            G.held = a.body;
            G.hold_dist = clampf(a.dist, PHYSGUN_MIN_DIST, PHYSGUN_MAX_DIST);
            G.grab_local = q_rotate_inv(b->rot, v3_sub(a.point, b->pos));
            G.hold_rel = q_mul(q_conj(yaw_quat(G.yaw)), b->rot);
            b->frozen = 0;
            phys_wake(&G.phys, a.body);
            play(G.snd.pickup, 0.7f);
            if (g_settings.sound) G.hold_sound = snd_loop(G.snd.hold, 0.35f);
        } else {
            play(G.snd.dryfire, 0.5f);
        }
    }
    if (G.held < 0) return;
    Body *b = &G.phys.bodies[G.held];
    if (!b->active) {
        physgun_release(1);
        return;
    }
    if (!(in->held & PSP_CTRL_RTRIGGER)) {
        physgun_release(0);
        return;
    }
    if (in->pressed & PSP_CTRL_LTRIGGER) { // freeze in place, GMod's right click
        if (is_client()) send_cmd(CMD_FREEZE, G.held, 0, 0, NULL, 0);
        b->frozen = 1;
        b->held = 0;
        b->vel = v3(0, 0, 0);
        b->angvel = v3(0, 0, 0);
        physgun_release(1);
        play(G.snd.freeze, 0.7f);
        toast("Frozen");
        return;
    }
    if (in->held & PSP_CTRL_UP) G.hold_dist += 300.0f * dt;
    if (in->held & PSP_CTRL_DOWN) G.hold_dist -= 300.0f * dt;
    G.hold_dist = clampf(G.hold_dist, PHYSGUN_MIN_DIST, PHYSGUN_MAX_DIST);
    float spin = 0.0f;
    if (in->held & PSP_CTRL_LEFT) spin += 120.0f * dt;
    if (in->held & PSP_CTRL_RIGHT) spin -= 120.0f * dt;
    if (spin != 0.0f) G.hold_rel = q_norm(q_mul(yaw_quat(spin), G.hold_rel));
    if (physgun_rotating(in)) {
        // Every axis: left and right turn it round the up axis, up and down tip it away from you or
        // towards you (round the axis across your view)
        float turn = -in->ax * PHYSGUN_ROTATE_SPEED * dt, tip = -in->ay * PHYSGUN_ROTATE_SPEED * dt;
        quat r = q_mul(yaw_quat(turn), q_axis_angle(v3(0, 1, 0), tip * DEG2RAD));
        G.hold_rel = q_norm(q_mul(r, G.hold_rel));
    }

    vec3 eye = player_eye(&G.player);
    vec3 target = v3_mad(eye, view_forward(), G.hold_dist);
    quat want_rot = q_mul(yaw_quat(G.yaw), G.hold_rel);
    vec3 want_pos = v3_sub(target, q_rotate(want_rot, G.grab_local));
    if (is_client()) {
        // The host moves it; show it where we want it meanwhile
        G.net_hold_pos = want_pos;
        G.net_hold_rot = want_rot;
        float k = clampf(dt * PHYSGUN_PULL, 0.0f, 1.0f);
        b->pos = v3_lerp(b->pos, want_pos, k);
        b->rot = q_norm((quat){b->rot.x + (want_rot.x - b->rot.x) * k, b->rot.y + (want_rot.y - b->rot.y) * k,
                               b->rot.z + (want_rot.z - b->rot.z) * k, b->rot.w + (want_rot.w - b->rot.w) * k});
        return;
    }
    physgun_drive(G.held, want_pos, want_rot);
}

static float body_radius(const Body *b) {
    float r = b->half.x > b->half.y ? b->half.x : b->half.y;
    return r > b->half.z ? r : b->half.z;
}

// Shoves a body along the view (gravity gun punt, or throwing what the hands carry)
static void launch_body(int i, float speed_50kg) {
    Body *b = &G.phys.bodies[i];
    float k = clampf(50.0f / b->mass, 0.25f, 2.5f); // light things fly further
    vec3 vel = v3_add(v3_scale(view_forward(), speed_50kg * k), G.player.velocity);
    if (is_client()) {
        float v[3] = {vel.x, vel.y, vel.z};
        send_cmd(CMD_PUNT, i, 0, 0, v, 3);
    }
    b->frozen = 0;
    b->held = 0;
    phys_wake(&G.phys, i);
    b->vel = vel;
    vm_event(VM_FIRE); // the gravity gun's punt
}

// Holds the carried prop (G.held) by its centre, gap units beyond its own size in front of the eye,
// keeping the turn it had when picked up
static void carry_in_front(float gap, float dt) {
    Body *b = &G.phys.bodies[G.held];
    G.hold_dist = body_radius(b) + gap;
    vec3 eye = player_eye(&G.player);
    quat want_rot = q_mul(yaw_quat(G.yaw), G.hold_rel);
    vec3 want_pos = v3_mad(eye, view_forward(), G.hold_dist);
    if (is_client()) {
        G.net_hold_pos = want_pos;
        G.net_hold_rot = want_rot;
        float k = clampf(dt * PHYSGUN_PULL, 0.0f, 1.0f);
        b->pos = v3_lerp(b->pos, want_pos, k);
        b->rot = want_rot;
        return;
    }
    physgun_drive(G.held, want_pos, want_rot);
}

// --- Use key (GMod's E) ---------------------------------------------------------------------------
// Tap it on a door or button to use it. Hold it on something light to carry it in front of you,
// whatever weapon you have: let go to drop it, or press R to throw it.

#define USE_CARRY_MAX_MASS 35.0f
#define USE_CARRY_RANGE 100.0f

static unsigned int use_buttons(void) {
#ifdef PS2
    return PSP_CTRL_RIGHT | PSP_CTRL_TRIANGLE; // Triangle is free on the DualShock (the right stick looks)
#else
    return PSP_CTRL_RIGHT;
#endif
}

// Starts carrying the prop the player is looking at, if it's close and light enough
static int use_pickup(void) {
    Aim a = aim_trace(USE_CARRY_RANGE);
    if (a.body < 0 || a.dist > USE_CARRY_RANGE) return 0;
    Body *b = &G.phys.bodies[a.body];
    if (b->frozen || b->mass > USE_CARRY_MAX_MASS) return 0;
    G.held = a.body;
    G.use_carry = 1;
    G.grab_local = v3(0, 0, 0);
    G.hold_rel = q_mul(q_conj(yaw_quat(G.yaw)), b->rot);
    phys_wake(&G.phys, a.body);
    play(G.snd.drop, 0.5f);
    return 1;
}

static void use_carry_update(const InputState *in, float dt) {
    if (G.held < 0 || !G.phys.bodies[G.held].active) {
        physgun_release(1);
        return;
    }
    if (!(in->held & use_buttons())) { // let go of the key: drop it
        physgun_release(0);
        return;
    }
    if (in->pressed & PSP_CTRL_RTRIGGER) {
        int i = G.held;
        physgun_release(1);
        launch_body(i, HANDS_THROW_SPEED);
        play(G.snd.drop, 0.8f);
        return;
    }
    carry_in_front(24.0f, dt);
}

// Gravity gun and hands: carry one prop in front of you, then drop or launch it.
// The gravity gun pulls from far away, lifts up to 250 kg and punts; hands only reach what's close and light.
static void carry_update(const InputState *in, float dt) {
    int gravgun = G.weapon == WEAPON_GRAVGUN;
#ifdef PS2
    unsigned int pick = gravgun ? PSP_CTRL_LTRIGGER : PSP_CTRL_RTRIGGER;
    unsigned int launch = gravgun ? PSP_CTRL_RTRIGGER : PSP_CTRL_LTRIGGER;
#else
    // L is jump on the PSP, so R picks up and drops, and L (jump is off while carrying) launches
    unsigned int pick = PSP_CTRL_RTRIGGER, launch = PSP_CTRL_LTRIGGER;
#endif
    float max_mass = gravgun ? GRAVGUN_MAX_MASS : HANDS_MAX_MASS;
    (void)dt;

    if (G.held >= 0) {
        Body *b = &G.phys.bodies[G.held];
        if (!b->active) {
            physgun_release(1);
            return;
        }
        if (in->pressed & launch) {
            int i = G.held;
            physgun_release(1);
            launch_body(i, gravgun ? GRAVGUN_PUNT_SPEED : HANDS_THROW_SPEED);
            play(gravgun ? G.snd.punt : G.snd.drop, 0.8f);
            G.kick = gravgun ? 4.0f : 1.0f;
            return;
        }
        if (in->pressed & pick) {
            physgun_release(0);
            return;
        }
        carry_in_front(gravgun ? 40.0f : 24.0f, dt);
        return;
    }

    if (in->pressed & pick) {
        Aim a = aim_trace(gravgun ? GRAVGUN_PULL_RANGE : HANDS_RANGE);
        Body *b = a.body >= 0 ? &G.phys.bodies[a.body] : NULL;
        if (b && !b->frozen && b->mass <= max_mass) {
            G.held = a.body;
            G.grab_local = v3(0, 0, 0);
            G.hold_rel = q_mul(q_conj(yaw_quat(G.yaw)), b->rot);
            phys_wake(&G.phys, a.body);
            play(gravgun ? G.snd.pickup_grav : G.snd.drop, 0.7f);
            if (gravgun && g_settings.sound) G.hold_sound = snd_loop(G.snd.hold_grav, 0.3f);
        } else if (gravgun && b && !b->frozen && a.dist <= GRAVGUN_PUNT_RANGE) {
            launch_body(a.body, GRAVGUN_PUNT_SPEED); // too heavy to lift: shove it instead
            play(G.snd.punt, 0.8f);
            G.kick = 4.0f;
        } else {
            play(G.snd.dryfire, 0.5f);
        }
        return;
    }
#ifdef PS2
    if (gravgun && (in->pressed & launch)) {
        Aim a = aim_trace(GRAVGUN_PUNT_RANGE);
        if (a.body >= 0 && G.phys.bodies[a.body].mass <= GRAVGUN_MAX_MASS * 4.0f) {
            launch_body(a.body, GRAVGUN_PUNT_SPEED);
            play(G.snd.punt, 0.8f);
        } else {
            play(G.snd.dryfire, 0.5f);
        }
        G.kick = 4.0f;
    }
#endif
}

// Pulls a body toward where a physics gun wants it (any player's)
static void physgun_drive(int i, vec3 want_pos, quat want_rot) {
    Body *b = &G.phys.bodies[i];
    b->held = 1;
    b->frozen = 0;
    phys_wake(&G.phys, i);
    b->vel = v3_scale(v3_sub(want_pos, b->pos), PHYSGUN_PULL);
    quat err = q_mul(want_rot, q_conj(b->rot));
    if (err.w < 0.0f) err = (quat){-err.x, -err.y, -err.z, -err.w};
    float s = sqrtf(err.x * err.x + err.y * err.y + err.z * err.z);
    if (s > 1e-4f) {
        float angle = 2.0f * atan2f(s, err.w);
        b->angvel = v3_scale(v3(err.x / s, err.y / s, err.z / s), angle * PHYSGUN_TURN);
    } else {
        b->angvel = v3(0, 0, 0);
    }
}

// Camera tool's L: on to the next camera, and after the last one back to your own eyes
static void camera_switch(void) {
    int total = 0, next = -1, nth = 0;
    for (int i = 0; i < MAX_BODIES; i++) {
        if (!is_camera(i)) continue;
        total++;
        if (next < 0 && i > G.cam_view) {
            next = i;
            nth = total;
        }
    }
    G.cam_view = next;
    play(G.snd.menu_move, 0.6f);
    if (total == 0) {
        toast("No cameras yet - R places one");
    } else if (next < 0) {
        toast("Your view");
    } else {
        char msg[32];
        snprintf(msg, sizeof(msg), "Camera %d of %d", nth, total);
        toast(msg);
    }
}

static int camera_tool_out(void) {
    return G.weapon == WEAPON_TOOLGUN && G.tool == TOOL_CAMERA;
}

static void toolgun_fire(void) {
    Aim a = aim_trace(USE_RANGE);
    G.beam_end = a.point;
    G.beam_time = 0.15f;
    G.kick = 2.5f;
    play(G.snd.tool, 0.6f);
    vm_event(VM_FIRE);
    Body *b = a.body >= 0 ? &G.phys.bodies[a.body] : NULL;
    switch (G.tool) {
    case TOOL_REMOVER:
        if (a.rag >= 0) {
            play(G.snd.remove, 0.6f);
            if (is_client()) {
                send_cmd(CMD_RAG_REMOVE, a.rag, 0, 0, NULL, 0);
            } else {
                npc_ragdoll_remove(a.rag);
            }
        } else if (a.npc >= 0) {
            play(G.snd.remove, 0.6f);
            if (is_client()) {
                send_cmd(CMD_REMOVE_NPC, a.npc, 0, 0, NULL, 0);
            } else {
                npc_remove(a.npc);
            }
        } else if (b) {
            play(G.snd.remove, 0.6f);
            if (is_client()) {
                send_cmd(CMD_REMOVE, a.body, 0, 0, NULL, 0);
            } else {
                remove_body(a.body);
            }
        }
        break;
    case TOOL_INFLATOR:
    case TOOL_DEFLATOR:
        if (a.rag >= 0) {
            float factor = G.tool == TOOL_INFLATOR ? 1.25f : 0.8f;
            float s = npc_ragdoll_inflate(a.rag, a.bone, factor); // a client shows it now; the host confirms
            if (is_client()) send_cmd(CMD_RAG_INFLATE, a.rag, a.bone, 0, &factor, 1);
            char msg[32];
            snprintf(msg, sizeof(msg), "Size %.0f%%", s * 100.0f);
            notify(msg, NOTE_HINT);
        } else {
            notify("Aim at a ragdoll", NOTE_ERROR);
        }
        break;
    case TOOL_FACEPOSER:
        if (a.rag < 0) {
            notify("Aim at a ragdoll", NOTE_ERROR);
        } else if (!npc_ragdoll_can_face(a.rag, g_face_sel)) {
            notify(npc_ragdoll_can_face(a.rag, 0) ? "That face can't make that expression" : "That ragdoll has no face to pose",
                   NOTE_ERROR);
        } else {
            npc_ragdoll_set_face(a.rag, g_face_sel); // a client shows it now; the host's ragdoll info confirms it
            if (is_client()) send_cmd(CMD_RAG_POSE, a.rag, 0, (uint32_t)g_face_sel, NULL, 0);
            char msg[40];
            snprintf(msg, sizeof(msg), "Face: %s", npc_face_name(g_face_sel));
            notify(msg, NOTE_HINT);
        }
        break;
    case TOOL_FINGERPOSER: {
        float d = 0.0f;
        int h = a.rag >= 0 ? npc_ragdoll_nearest_hand(a.rag, a.point, &d) : -1;
        if (a.rag < 0) {
            notify("Aim at a ragdoll", NOTE_ERROR);
        } else if (h < 0) {
            notify("That ragdoll has no fingers to pose", NOTE_ERROR);
        } else {
            // Aim at a hand to pose it, anywhere else on the body to pose both
            int both = d > 24.0f;
            for (int k = 0; k < 2; k++) {
                if (!both && k != h) continue;
                if (npc_ragdoll_set_hand(a.rag, k, g_hand_sel) && is_client()) {
                    send_cmd(CMD_RAG_POSE, a.rag, 1 + k, (uint32_t)g_hand_sel, NULL, 0);
                }
            }
            char msg[48];
            snprintf(msg, sizeof(msg), "%s: %s", both ? "Hands" : h ? "Right hand" : "Left hand", npc_hand_name(g_hand_sel));
            notify(msg, NOTE_HINT);
        }
        break;
    }
    case TOOL_COLOUR:
        if (b) b->tint_abgr = k_palette[G.colour_sel];
        if (b && is_client()) send_cmd(CMD_COLOUR, a.body, 0, k_palette[G.colour_sel], NULL, 0);
        if (b) G.body_dirty[a.body] = 1;
        break;
    case TOOL_BALLOON:
        if (b && is_client()) {
            send_cmd(CMD_BALLOON, a.body, 0, 0, NULL, 0);
        } else if (b) {
            if (b->balloons < MAX_BALLOONS) {
                G.body_dirty[a.body] = 1;
                b->balloons++;
                update_balloon_lift(b);
                b->frozen = 0;
                phys_wake(&G.phys, a.body);
            } else {
                toast("That prop has enough balloons");
            }
        }
        break;
    case TOOL_DUPLICATOR:
        if (b && b->lib == 0) {
            G.dupe_model = b->model;
            G.dupe_rot = q_mul(q_conj(yaw_quat(G.yaw)), b->rot);
            G.dupe_tint = b->tint_abgr;
            toast("Copied");
        } else if (b) {
            toast("Map props can't be copied");
        } else if (G.dupe_model >= 0 && a.hit_world) {
            spawn_prop(G.dupe_model, a.point, a.normal, q_mul(yaw_quat(G.yaw), G.dupe_rot), G.dupe_tint);
        }
        break;
    case TOOL_WELD:
    case TOOL_BALLSOCKET:
    case TOOL_ROPE:
    case TOOL_ELASTIC:
    case TOOL_SLIDER: {
        // Weld and ball socket join a prop to a prop (or the world) at the second point;
        // rope and elastic tie two points together, and a slider runs along the line between them
        int rigid = G.tool == TOOL_WELD || G.tool == TOOL_BALLSOCKET;
        if (!G.stage) {
            if (!b && (rigid || !a.hit_world)) break;
            G.stage = 1;
            G.stage_body = a.body;
            G.stage_point = a.point;
            toast(rigid ? "Now pick what to attach it to" : "Now pick the other end");
        } else {
            if (!b && !a.hit_world) break;
            int ok;
            if (rigid) {
                if (G.stage_body < 0 || a.body == G.stage_body) break;
                if (is_client()) {
                    float f[3] = {a.point.x, a.point.y, a.point.z};
                    send_cmd(G.tool == TOOL_WELD ? CMD_WELD : CMD_BALLSOCKET, G.stage_body, a.body, 0, f, 3);
                    ok = 1;
                } else if (G.tool == TOOL_WELD) {
                    ok = phys_weld(&G.phys, G.stage_body, a.body, a.point) >= 0;
                } else {
                    ok = phys_ballsocket(&G.phys, G.stage_body, a.body, a.point) >= 0;
                }
            } else {
                if (G.stage_body < 0 && a.body < 0) {
                    toast("It needs a prop on at least one end");
                    G.stage = 0;
                    break;
                }
                if (G.tool == TOOL_SLIDER && a.body == G.stage_body) {
                    toast("Pick a different prop, or the world");
                    G.stage = 0;
                    break;
                }
                float len = v3_len(v3_sub(a.point, G.stage_point));
                if (G.tool == TOOL_ELASTIC) len *= 0.5f; // rests at half the length, so it pulls straight away
                if (is_client()) {
                    float f[7] = {G.stage_point.x, G.stage_point.y, G.stage_point.z, a.point.x, a.point.y, a.point.z, len};
                    int cmd = G.tool == TOOL_ROPE ? CMD_ROPE : G.tool == TOOL_ELASTIC ? CMD_ELASTIC : CMD_SLIDER;
                    send_cmd(cmd, G.stage_body, a.body, 0, f, 7);
                    ok = 1;
                } else if (G.tool == TOOL_ROPE) {
                    ok = phys_rope(&G.phys, G.stage_body, G.stage_point, a.body, a.point, len) >= 0;
                } else if (G.tool == TOOL_ELASTIC) {
                    ok = phys_elastic(&G.phys, G.stage_body, G.stage_point, a.body, a.point, len) >= 0;
                } else {
                    ok = phys_slider(&G.phys, G.stage_body, G.stage_point, a.body, a.point) >= 0;
                }
            }
            G.stage = 0;
            if (ok) {
                play(G.snd.weld, 0.6f);
                toast(G.tool == TOOL_WELD         ? "Welded"
                      : G.tool == TOOL_BALLSOCKET ? "Ball socket added"
                      : G.tool == TOOL_ROPE       ? "Roped"
                      : G.tool == TOOL_ELASTIC    ? "Elastic added"
                                                  : "Slider added");
            } else {
                toast("Too many constraints");
            }
        }
        break;
    }
    case TOOL_NOCOLLIDE:
        if (!b) break;
        b->nocollide = !b->nocollide; // a client flips its copy too, for the message
        if (is_client()) send_cmd(CMD_NOCOLLIDE, a.body, 0, 0, NULL, 0);
        phys_wake(&G.phys, a.body);
        toast(b->nocollide ? "Passes through props now" : "Collides with props again");
        break;
    case TOOL_WEIGHT: {
        if (!b) break;
        float m = next_weight(b->mass);
        if (is_client()) {
            float f[1] = {m};
            send_cmd(CMD_WEIGHT, a.body, 0, 0, f, 1);
        }
        phys_set_mass(&G.phys, a.body, m);
        char msg[32];
        snprintf(msg, sizeof(msg), "Weight: %.0f kg", m);
        toast(msg);
        break;
    }
    case TOOL_THRUSTER:
    case TOOL_HOVERBALL: {
        if (!b) break;
        vec3 dir = view_forward(); // a thruster pushes the way you shot it in
        int ok;
        if (is_client()) {
            float f[6] = {a.point.x, a.point.y, a.point.z, dir.x, dir.y, dir.z};
            send_cmd(G.tool == TOOL_THRUSTER ? CMD_THRUSTER : CMD_HOVER, a.body, 0, 0, f, 6);
            ok = 1;
        } else if (G.tool == TOOL_THRUSTER) {
            ok = phys_add_force(&G.phys, FORCE_THRUSTER, a.body, a.point, dir, THRUSTER_FORCE) >= 0;
        } else {
            ok = phys_add_force(&G.phys, FORCE_HOVER, a.body, a.point, v3(0, 0, 1), a.point.z) >= 0;
        }
        if (ok) {
            play(G.snd.weld, 0.6f);
            toast(G.tool == TOOL_THRUSTER ? "Thruster added (Use switches it)" : "Hoverball added (Use switches it)");
        } else {
            toast("Too many thrusters and hoverballs");
        }
        break;
    }
    case TOOL_CAMERA: {
        // A camera where you aim, pointing the way you look (frozen in the air if you aim at nothing)
        int cm = camera_model();
        if (cm < 0) {
            toast("No camera model in this build's prop pack");
            break;
        }
        vec3 fwd = view_forward();
        vec3 at = a.point, n = a.normal;
        if (!b && !a.hit_world) {
            at = v3_mad(player_eye(&G.player), fwd, 80.0f);
            n = v3_scale(fwd, -1.0f);
        }
        quat rot = q_mul(yaw_quat(G.yaw), q_axis_angle(v3(0, 1, 0), -G.pitch * DEG2RAD));
        if (spawn_prop(cm, at, n, rot, 0xFFFFFFFF) >= 0 || is_client()) toast("Camera placed (L: switch view)");
        break;
    }
    case TOOL_EXPLOSIVE:
        if (!b) break;
        if (is_client()) {
            send_cmd(CMD_EXPLODE, a.body, 0, 0, NULL, 0);
            show_explosion(b->pos);
        } else {
            do_explode(a.body);
        }
        break;
    case TOOL_WHEEL:
    case TOOL_LAMP:
    case TOOL_LIGHT:
    case TOOL_EMITTER:
    case TOOL_WINCH:
    case TOOL_HYDRAULIC:
    case TOOL_BUTTON: {
        int two = G.tool == TOOL_WINCH || G.tool == TOOL_HYDRAULIC || G.tool == TOOL_BUTTON;
        if (two && !G.stage) {
            // The first pick: one end, or (Button) the prop it will switch
            if (G.tool == TOOL_BUTTON ? !b : (!b && !a.hit_world)) {
                if (G.tool == TOOL_BUTTON) toast("Pick the prop it switches");
                break;
            }
            G.stage = 1;
            G.stage_body = a.body;
            G.stage_point = a.point;
            toast(G.tool == TOOL_BUTTON ? "Now pick where the button goes" : "Now pick the other end");
            break;
        }
        G.stage = 0;
        if (!b && !a.hit_world) break;
        vec3 v = two ? G.stage_point : view_forward();
        int sbody = two ? G.stage_body : -1;
        if (is_client()) {
            float f[9] = {a.point.x, a.point.y, a.point.z, a.normal.x, a.normal.y, a.normal.z, v.x, v.y, v.z};
            uint32_t u = (uint32_t)((sbody + 1) & 0xFFFF) | ((uint32_t)G.colour_sel << 16) | ((uint32_t)g_emit_effect << 24);
            send_cmd(CMD_TOOL, a.body, G.tool, u, f, 9);
            play(G.snd.weld, 0.6f);
        } else {
            tool_apply(G.tool, a.body, a.point, a.normal, sbody, v, G.colour_sel, g_emit_effect);
        }
        break;
    }
    default:
        break;
    }
}

// --- Guns ---------------------------------------------------------------------------------------

// Spread comes from a seed so the host can repeat a client's shotgun blast pellet for pellet
static float spread_rand(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)((*s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static void add_tracer(vec3 a, vec3 b);

// An armed NPC fired: the gun's sound where it is, and a tracer to where the shot went
static void npc_shot(const char *weapon, vec3 from, vec3 to) {
    if (strncmp(weapon, "_tf_", 4) == 0) {
        // TF2's guns: their own sounds (named like the gun: "_tf_rocket" -> "tf_rocket")
        static const char *names[9];
        static int snd[9], count;
        int k = 0;
        while (k < count && strcmp(names[k], weapon) != 0) k++;
        if (k == count && count < 9) {
            names[count] = weapon;
            snd[count++] = snd_find(weapon + 1);
        }
        int flame = strcmp(weapon, "_tf_flame") == 0;
        static float flame_next;
        float now = (float)(sceKernelGetSystemTimeWide() / 1000) * 0.001f;
        if (k < count && snd[k] >= 0 && g_settings.sound && (!flame || now >= flame_next)) {
            snd_play_at(snd[k], from, 0.7f);
            if (flame) flame_next = now + 0.5f; // one roar per half second, not one per puff
        }
        if (strcmp(weapon, "_tf_rocket") == 0 || strcmp(weapon, "_tf_grenade") == 0) {
            // Rockets and pipes: a bang where they land that throws props about (the mercenaries' own
            // damage comes from npc.c, so nobody else gets hurt by it)
            show_explosion(to);
            phys_blast(&G.phys, to, 200.0f, 500.0f);
        } else if (!flame) {
            add_tracer(from, to);
            vec3 d = v3_sub(to, from);
            float len = v3_len(d);
            if (len > 1.0f) bullet_hole(from, v3_scale(d, 1.0f / len), to, -1);
        }
        return;
    }
    int w = strcmp(weapon, "_pistol") == 0    ? WEAPON_PISTOL

            : strcmp(weapon, "_shotgun") == 0 ? WEAPON_SHOTGUN
            : strcmp(weapon, "_357") == 0     ? WEAPON_357
            : strcmp(weapon, "_ar2") == 0     ? WEAPON_AR2
                                              : WEAPON_SMG;
    if (g_settings.sound && G.snd.fire[w] >= 0) snd_play_at(G.snd.fire[w], from, 0.7f);
    add_tracer(from, to);
    vec3 d = v3_sub(to, from);
    float len = v3_len(d);
    if (len > 1.0f) bullet_hole(from, v3_scale(d, 1.0f / len), to, -1); // a hole where it hit a wall (none if it hit someone)

}

// --- Mingebags ------------------------------------------------------------------------------------
// The NPCs decide when to make mischief (npc.c); the props and chat are the game's. Their props aren't
// anyone's (no undo, no prop limit) but only the newest MINGE_PROPS of them stay.
#define MINGE_OWNER 100 // body_owner of mingebag props: not a player slot
#define MINGE_PROPS 16

static const char *const k_minge_names[] = {"xX_Sn1per_Xx", "[FUN] Bob", "garry", "propkill3r", "Mingebag",
                                            "darkrp_pro", "kevin", "~*Turbo*~"};
static const char *const k_minge_lines[] = {"lol", "xD", "get rekt", "gg ez", "how do i noclip", "admin??",
                                            "can i have admin", "lmao", "NICE", "mic spam", "who stole my crowbar",
                                            "wat", "brb", "1v1 me", "prop surf time", "hahaha", "u mad?",
                                            "this server sucks", "i am the admin", "spawning bathtubs"};
static const char *const k_minge_hurt[] = {"ow", "wtf", "rude", "hey!!", "ur banned", "reported", "lag",
                                           "i wasnt ready"};
// What a mingebag spawns: the classics
static const char *const k_minge_props[] = {"Bathtub", "Fridge", "Couch", "Vending Machine", "Oil Drum",
                                            "Wood Crate", "Watermelon", "Traffic Cone", "Dumpster", "Toilet",
                                            "Hula Doll", "Breen Bust", "Lockers", "Mattress", "Washing Machine",
                                            "Sawblade"};
#define NUM_MINGE_PROPS (int)(sizeof(k_minge_props) / sizeof(k_minge_props[0]))
static int g_minge_models[NUM_MINGE_PROPS], g_minge_models_n = -1;
static int g_minge_bodies[MINGE_PROPS], g_minge_next;

static NetHeader net_header(int type);

static void minge_say(int npc, const char *line) {
    PkChat c;
    memset(&c, 0, sizeof(c));
    snprintf(c.text, sizeof(c.text), "%s: %s", k_minge_names[npc % (int)(sizeof(k_minge_names) / sizeof(k_minge_names[0]))], line);
    notify(c.text, NOTE_CHAT);
    if (G.role == ROLE_HOST) { // everyone reads it
        c.h = net_header(PK_CHAT);
        net_send(NULL, &c, sizeof(c));
    }
}

static int minge_model(void) {
    if (g_minge_models_n < 0) { // looked up once: prop_named only remembers a few names
        g_minge_models_n = 0;
        for (int k = 0; k < NUM_MINGE_PROPS; k++) {
            for (int i = 0; i < G.props.num_models; i++) {
                if (strncmp(G.props.models[i].name, k_minge_props[k], 32) == 0) {
                    g_minge_models[g_minge_models_n++] = i;
                    break;
                }
            }
        }
    }
    return g_minge_models_n ? g_minge_models[rand() % g_minge_models_n] : -1;
}

static void minge_act(int npc, int action, vec3 at, vec3 target) {
    if (action == MINGE_CHAT) {
        minge_say(npc, k_minge_lines[rand() % (int)(sizeof(k_minge_lines) / sizeof(k_minge_lines[0]))]);
    } else if (action == MINGE_DIED) {
        achieve_minge_killed();
    } else if (action == MINGE_HURT) {

        if (rand() % 3 == 0) minge_say(npc, k_minge_hurt[rand() % (int)(sizeof(k_minge_hurt) / sizeof(k_minge_hurt[0]))]);
    } else if (action == MINGE_SPAWN && G.props_loaded) {
        int model = minge_model();
        if (model < 0 || model >= G.props.num_models) return;
        const PpakModel *md = &G.props.models[model];
        vec3 mins = v3(md->mins[0], md->mins[1], md->mins[2]), maxs = v3(md->maxs[0], md->maxs[1], md->maxs[2]);
        // Not inside a wall: back off toward the mingebag if there's something in the way
        Trace tr;
        cm_trace_line(&G.map, v3(at.x, at.y, at.z + 20.0f), v3(at.x, at.y, at.z + 20.0f + (maxs.z - mins.z)), MASK_PROPSOLID, NULL, 0, &tr);
        if (tr.fraction < 1.0f) return;

        // The oldest one goes to make room
        int old = g_minge_bodies[g_minge_next];
        if (old > 0 && G.phys.bodies[old - 1].active && G.body_owner[old - 1] == MINGE_OWNER) remove_body(old - 1);
        g_minge_bodies[g_minge_next] = 0;
        quat rot = q_axis_angle(v3(0, 0, 1), (float)(rand() % 360) * DEG2RAD);
        vec3 pos = v3(at.x, at.y, at.z + 20.0f - mins.z);
        int i = phys_spawn(&G.phys, model, (int)(md->flags & PPAK_SHAPE_MASK), mins, maxs, md->mass, pos, rot);
        if (i < 0) return;
        attach_hull(&G.phys.bodies[i], &G.props, model);
        G.phys.bodies[i].tint_abgr = 0xFFFFFFFF;
        G.phys.bodies[i].surface = (int)md->surface;
        G.body_owner[i] = MINGE_OWNER;
        g_minge_bodies[g_minge_next] = i + 1;
        g_minge_next = (g_minge_next + 1) % MINGE_PROPS;
        play_at(G.snd.spawn, pos, 0.6f);
    } else if (action == MINGE_THROW) {
        // The nearest loose prop goes flying at the player, physics-gun style
        int best = -1;
        float best_d = 300.0f;
        for (int i = 0; i < MAX_BODIES; i++) {
            const Body *b = &G.phys.bodies[i];
            if (!b->active || b->frozen || b->mass > 400.0f || i == G.held || vehicle_kind(b) != VEH_NONE) continue;
            float d = v3_len(v3_sub(b->pos, at));
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        if (best < 0) {
            minge_act(npc, MINGE_SPAWN, at, target); // nothing to throw: make something
            return;
        }
        Body *b = &G.phys.bodies[best];
        vec3 aim = v3_sub(v3(target.x, target.y, target.z + 40.0f), b->pos);
        float dist = v3_len(aim);
        if (dist < 1.0f) return;
        b->vel = v3_add(v3_scale(aim, 560.0f / dist), v3(0, 0, 120.0f + dist * 0.2f));

        phys_wake(&G.phys, best);
        play_at(G.snd.spawn, b->pos, 0.4f);
    }
}


// Prop pack index of a model by name (NPC weapons), remembered after the first look
static int prop_named(const char *name) {
    static char names[16][16];
    static int index[16], count;
    for (int k = 0; k < count; k++) {
        if (strncmp(names[k], name, 16) == 0) return index[k];
    }
    int found = -1;
    for (int i = 0; i < G.props.num_models && found < 0; i++) {
        if (strncmp(G.props.models[i].name, name, 16) == 0) found = i;
    }
    if (count < 16) {
        strncpy(names[count], name, 16);
        index[count++] = found;
    }
    return found;
}

static void make_light(PropLight *l, unsigned int ambient, int sunlit, unsigned int tint);

// The guns NPCs hold, in the hands npcs_draw just posed
static void draw_npc_weapons(void) {
    if (!G.props_loaded) return;
    proplib_begin();
    for (int i = 0; i < MAX_NPCS; i++) {
        const char *name;
        ScePspFMatrix4 m;
        if (!npc_weapon(i, &name, &m)) continue;
        int model = prop_named(name);
        if (model < 0) continue;
        vec3 at = v3(m.w.x, m.w.y, m.w.z);
        int leaf = map_point_leaf(&G.map, &at);
        unsigned int amb = (leaf >= 0 && leaf < G.map.num_leafs) ? G.map.leafs[leaf].ambient_abgr : G.map.info->ambient_abgr;
        PropLight l;
        make_light(&l, amb, 1, 0xFFFFFFFF);
        proplib_draw(&G.props, model, &m, &l);
    }
    proplib_end();
}

// --- Blood and decals -----------------------------------------------------------------------------
// A shot into flesh throws a spray of drops and leaves a splat on whatever is behind it, plus a
// smaller one on the floor below. Bullets leave holes in walls and props (on props they move with
// it). Decals use HL2's textures (ui.pak "decal_*"), drawn the way HL2's DecalModulate draws them:
// the surface's own colour times the texture times two, so they're lit just like the surface.

#define MAX_BLOOD_DROPS 64
#define MAX_DECALS 80
#define BLOOD_DROP_LIFE 0.6f
#define BLOOD_DECAL_LIFE 90.0f // seconds before a splat is gone (bullet holes stay until pushed out)

enum { DECAL_BLOOD, DECAL_CONCRETE, DECAL_METAL, DECAL_WOOD, DECAL_GLASS, DECAL_KINDS };
// Texture names per kind, in ui.pak
static const char *const k_decal_tex[DECAL_KINDS][4] = {
    {"decal_blood1", "decal_blood2", "decal_blood3", "decal_blood4"},
    {"decal_concrete1", "decal_concrete2", NULL, NULL},
    {"decal_metal1", "decal_metal2", NULL, NULL},
    {"decal_wood1", "decal_wood2", NULL, NULL},
    {"decal_glass1", NULL, NULL, NULL},
};
#define DECAL_TEXTURES 16

typedef struct {
    vec3 p, v;
    float life;
} BloodDrop;
typedef struct {
    vec3 c, s, t, n;  // centre on the surface, the surface's in-plane axes and normal (in the body's
                      // frame when stuck to a prop)
    float r, age;
    int body;         // the prop it's stuck to, or -1 for the world
    int body_model;   // ...which must still be the same prop
    uint8_t tex, kind, active, pad;
} Decal;
static BloodDrop g_drops[MAX_BLOOD_DROPS];
static Decal g_decals[MAX_DECALS];
static int g_next_drop, g_next_decal;
static uint32_t g_blood_rand = 12345;
static int g_decal_img[DECAL_TEXTURES]; // ui image per texture slot, -1 if missing
static int g_decal_first[DECAL_KINDS], g_decal_count[DECAL_KINDS];
static int g_decal_tex_ready;

static float blood_rand(void) { // -1..1
    g_blood_rand = g_blood_rand * 1664525u + 1013904223u;
    return (float)((g_blood_rand >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

// npc.c: a prop hit an NPC hard
void game_blood(vec3 at, vec3 dir) {
    blood_spray(at, dir, 6);
}

static void blood_clear(void) {
    memset(g_drops, 0, sizeof(g_drops));
    memset(g_decals, 0, sizeof(g_decals));
}

static void decal_textures(void) {
    if (g_decal_tex_ready) return;
    g_decal_tex_ready = 1;
    int n = 0;
    for (int k = 0; k < DECAL_KINDS; k++) {
        g_decal_first[k] = n;
        g_decal_count[k] = 0;
        for (int j = 0; j < 4 && k_decal_tex[k][j] && n < DECAL_TEXTURES; j++) {
            int id = ui_image_find(k_decal_tex[k][j]);
            if (id < 0) continue;
            g_decal_img[n++] = id;
            g_decal_count[k]++;
        }
    }
}

// Does a square of half-size r around at lie flat on the surface (no corner hanging over an edge)?
static int decal_fits(vec3 at, vec3 n, vec3 s, vec3 t, float r) {
    for (int k = 0; k < 4; k++) {
        vec3 corner = v3_add(at, v3_add(v3_scale(s, (k & 1) ? r : -r), v3_scale(t, (k & 2) ? r : -r)));
        Trace tr;
        cm_trace_line(&G.map, v3_mad(corner, n, 2.0f), v3_mad(corner, n, -3.0f), MASK_PROPSOLID, NULL, 0, &tr);
        if (tr.fraction >= 1.0f || tr.startsolid || v3_dot(tr.normal, n) < 0.9f) return 0;
    }
    return 1;
}

// A decal at a world point on a surface with normal n; body >= 0 sticks it to that prop
static void decal_add(vec3 at, vec3 n, float r, int kind, int body) {
    decal_textures();
    if (!g_decal_count[kind]) return;
    vec3 s = v3_norm(fabsf(n.z) < 0.9f ? v3_cross(n, v3(0, 0, 1)) : v3_cross(n, v3(1, 0, 0)));
    vec3 t = v3_cross(n, s);
    // Turned at random, so neighbouring holes and splats don't look stamped
    float a = blood_rand() * PI, ca = cosf(a), sa = sinf(a);
    vec3 s2 = v3_add(v3_scale(s, ca), v3_scale(t, sa)), t2 = v3_cross(n, s2);
    if (body < 0) {
        // On the world: shrink it until it fits on its surface; a splat that can't is dropped
        int tries = 0;
        while (!decal_fits(at, n, s2, t2, r * 0.8f) && tries < 3) {
            r *= 0.6f;
            tries++;
        }
        if (tries == 3 && kind == DECAL_BLOOD) return;
    }
    Decal *d = &g_decals[g_next_decal];
    g_next_decal = (g_next_decal + 1) % MAX_DECALS;
    memset(d, 0, sizeof(*d));
    d->active = 1;
    d->kind = (uint8_t)kind;
    d->tex = (uint8_t)(g_decal_first[kind] + (int)((blood_rand() * 0.5f + 0.5f) * (float)g_decal_count[kind] * 0.999f));
    d->r = r;
    d->body = body;
    if (body >= 0) {
        const Body *b = &G.phys.bodies[body];
        d->body_model = b->model;
        d->c = q_rotate_inv(b->rot, v3_sub(at, b->pos));
        d->n = q_rotate_inv(b->rot, n);
        d->s = q_rotate_inv(b->rot, s2);
        d->t = q_rotate_inv(b->rot, t2);
    } else {
        d->c = at;
        d->n = n;
        d->s = s2;
        d->t = t2;
    }
}

static int surface_decal(int surface) {
    switch (surface) {
    case SURF_METAL:
        return DECAL_METAL;
    case SURF_WOOD:
    case SURF_CARDBOARD:
        return DECAL_WOOD;
    case SURF_GLASS:
        return DECAL_GLASS;
    default:
        return DECAL_CONCRETE;
    }
}

// A bullet hole where a shot hit the world (body < 0) or a prop
static void bullet_hole(vec3 eye, vec3 dir, vec3 hit, int body) {
    Trace tr;
    if (body < 0) {
        cm_trace_line(&G.map, v3_mad(hit, dir, -4.0f), v3_mad(hit, dir, 4.0f), MASK_PROPSOLID, NULL, 0, &tr);
        if (tr.fraction >= 1.0f || tr.startsolid) return;
        decal_add(tr.endpos, tr.normal, 2.6f + 0.6f * blood_rand(), DECAL_CONCRETE, -1);
        return;
    }
    // The prop's real shape gives the face it was hit on
    const Body *b = &G.phys.bodies[body];
    CmBox box;
    phys_body_cmbox(b, &box, 0);
    cm_trace(&G.map, v3_mad(hit, dir, -8.0f), v3_mad(hit, dir, 8.0f), v3(0, 0, 0), v3(0, 0, 0), 0, &box, 1, &tr);
    vec3 n = tr.fraction < 1.0f && !tr.startsolid ? tr.normal : v3_scale(dir, -1.0f);
    (void)eye;
    decal_add(tr.fraction < 1.0f ? tr.endpos : hit, n, 2.4f + 0.5f * blood_rand(), surface_decal(b->surface), body);
}

static void blood_spray(vec3 hit, vec3 dir, int amount) {
    for (int i = 0; i < amount; i++) {
        BloodDrop *b = &g_drops[g_next_drop];
        g_next_drop = (g_next_drop + 1) % MAX_BLOOD_DROPS;
        b->p = hit;
        // Mostly out the far side, some back towards the shooter, all thrown up a little
        float out = (i & 3) ? 70.0f : -50.0f;
        b->v = v3(dir.x * out + blood_rand() * 60.0f, dir.y * out + blood_rand() * 60.0f, dir.z * out + 40.0f + blood_rand() * 50.0f);
        b->life = BLOOD_DROP_LIFE * (0.6f + 0.4f * blood_rand() * blood_rand());
    }
    Trace tr;
    cm_trace_line(&G.map, hit, v3_mad(hit, dir, 110.0f), MASK_PROPSOLID, NULL, 0, &tr);
    if (tr.fraction < 1.0f && !tr.startsolid) decal_add(tr.endpos, tr.normal, 16.0f + 6.0f * blood_rand(), DECAL_BLOOD, -1);
    cm_trace_line(&G.map, hit, v3(hit.x, hit.y, hit.z - 160.0f), MASK_PROPSOLID, NULL, 0, &tr);
    if (tr.fraction < 1.0f && !tr.startsolid && blood_rand() > -0.3f)
        decal_add(tr.endpos, tr.normal, 11.0f + 4.0f * blood_rand(), DECAL_BLOOD, -1);
}

static void blood_update(float dt) {
    for (int i = 0; i < MAX_BLOOD_DROPS; i++) {
        BloodDrop *b = &g_drops[i];
        if (b->life <= 0.0f) continue;
        b->life -= dt;
        b->v.z -= 500.0f * dt;
        b->p = v3_mad(b->p, b->v, dt);
    }
    for (int i = 0; i < MAX_DECALS; i++) {
        Decal *d = &g_decals[i];
        if (!d->active) continue;
        d->age += dt;
        if (d->kind == DECAL_BLOOD && d->age > BLOOD_DECAL_LIFE) d->active = 0;
        // Gone with its prop (removed, broken, or the slot reused)
        if (d->body >= 0 && (!G.phys.bodies[d->body].active || G.phys.bodies[d->body].model != d->body_model)) d->active = 0;
    }
}

static void add_tracer(vec3 a, vec3 b) {
    G.tracers[G.next_tracer].a = a;
    G.tracers[G.next_tracer].b = b;
    G.tracers[G.next_tracer].time = 0.06f;
    G.next_tracer = (G.next_tracer + 1) % 8;
}

// --- PvP -----------------------------------------------------------------------------------------
// In multiplayer, bullets and the crowbar hurt the other players, as in GMod. The host keeps everyone's
// health, so only it applies the damage; on a client a hit just shows blood.

static int ray_box(vec3 o, vec3 d, vec3 lo, vec3 hi, float max_t, float *t_out) {
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    float tmin = 0.0f, tmax = max_t;
    for (int k = 0; k < 3; k++) {
        if (fabsf(dd[k]) < 1e-6f) {
            if (oo[k] < l[k] || oo[k] > h[k]) return 0;
            continue;
        }
        float t1 = (l[k] - oo[k]) / dd[k], t2 = (h[k] - oo[k]) / dd[k];
        if (t1 > t2) {
            float tt = t1;
            t1 = t2;
            t2 = tt;
        }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }
    *t_out = tmin;
    return 1;
}

// The first player (by slot) along the ray within max_t, other than exclude (the shooter), or -1
static int player_raycast(vec3 o, vec3 d, float max_t, int exclude, float *t_out) {
    if (G.role == ROLE_OFFLINE) return -1;
    int best = -1;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        if (s == exclude) continue;
        vec3 feet;
        int ducked;
        if (s == G.my_slot) {
            if (G.dead_time > 0.0f || G.player.noclip) continue;
            feet = G.player.origin;
            ducked = G.player.ducked;
        } else {
            const Peer *p = &G.peers[s];
            if (!p->active || p->health <= 0.0f || (p->flags & (PF_NOCLIP | PF_DEAD))) continue;
            feet = p->pos;
            ducked = (p->flags & PF_DUCKED) != 0;
        }
        float t;
        vec3 lo, hi;
        player_box(s, feet, ducked, &lo, &hi);
        if (ray_box(o, d, lo, hi, max_t, &t)) {
            max_t = t;
            best = s;
        }
    }
    if (best >= 0) *t_out = max_t;
    return best;
}

static const char *player_name(int s) {
    return s == G.my_slot ? net_nickname() : G.peers[s].name;
}

// Host: a player shot another
static void pvp_damage(int victim, int attacker, float amount) {
    if (god_mode()) return;
    if (g_gm.on && !script_should_damage(victim, attacker)) return; // the gamemode's rules (no friendly fire...)
    g_gm.p[victim].attacker = attacker;
    int was_alive, dead;
    if (victim == G.my_slot) {
        was_alive = G.dead_time <= 0.0f;
        damage_player(amount);
        dead = G.dead_time > 0.0f;
    } else {
        Peer *p = &G.peers[victim];
        was_alive = p->health > 0.0f;
        p->health = fmaxf(0.0f, p->health - amount);
        dead = p->health <= 0.0f;
    }
    if (was_alive && dead && !g_gm.on) { // a gamemode says who killed whom itself
        char msg[40]; // two 15-character names fit; notifications hold 48
        snprintf(msg, sizeof(msg), "%.15s killed %.15s", player_name(attacker), player_name(victim));
        int acting = G.acting;
        G.acting = 0; // notify() leaves out remote players' actions; this one is for the host to see
        notify(msg, NOTE_HINT);
        G.acting = acting;
    }
}

// One bullet (or crowbar swing) along dir. With world set it also hits props and NPCs (single
// player / host); a multiplayer client only shoves its own ragdolls and leaves the rest to the host.
// Returns what it hit: 0 nothing, 1 the world, 2 a prop, 3 an NPC, 4 a ragdoll, 5 a player.
static int fire_bullet(vec3 eye, vec3 dir, const WeaponStats *ws, int world, int sound, vec3 *end) {
    Trace tr;
    cm_trace_line(&G.map, eye, v3_mad(eye, dir, ws->range), MASK_PROPSOLID, NULL, 0, &tr);
    float best = tr.fraction * ws->range, t;
    int kind = tr.fraction < 1.0f, idx = -1, bone = 0;
    int b = phys_raycast(&G.phys, eye, dir, best, &t);
    if (b >= 0) {
        best = t;
        kind = 2;
        idx = b;
    }
    int n = npc_raycast(eye, dir, best, &t);
    if (n >= 0) {
        best = t;
        kind = 3;
        idx = n;
    }
    int rb;
    int r = npc_ragdoll_raycast(eye, dir, best, &t, &rb);
    if (r >= 0) {
        best = t;
        kind = 4;
        idx = r;
        bone = rb;
    }
    // Players: the shooter is the player whose command this is (host), else this PSP's own
    int shooter = G.acting ? G.acting : G.my_slot;
    int pl = player_raycast(eye, dir, best, shooter, &t);
    if (pl >= 0) {
        best = t;
        kind = 5;
        idx = pl;
    }
    vec3 hit = v3_mad(eye, dir, best);
    *end = hit;
    int melee = ws->clip == 0;
    if (kind >= 3) blood_spray(hit, dir, melee ? 5 : 8);
    if (kind == 2 && world && break_group(&G.phys.bodies[idx])) {
        break_body(idx); // a shot or a crowbar hit breaks it outright
    } else if (kind == 2 && world) {
        Body *bd = &G.phys.bodies[idx];
        if (!bd->frozen) {
            phys_wake(&G.phys, idx);
            float k = fminf(ws->force * 50.0f / bd->mass, 1500.0f);
            bd->vel = v3_mad(bd->vel, dir, k);
            // Off-centre hits spin it
            bd->angvel = v3_add(bd->angvel, v3_scale(v3_cross(v3_sub(hit, bd->pos), dir), k * 0.002f));
        }
    } else if (kind == 3 && world) {
        g_npc_attacker = shooter;
        npc_damage_dir(idx, ws->damage, v3_scale(dir, ws->force));
        g_npc_attacker = -1;
    } else if (kind == 5 && world) {
        pvp_damage(idx, shooter, ws->damage);
    } else if (kind == 4) {
        npc_ragdoll_push(idx, bone, v3_scale(dir, ws->force * 1.5f));
    }
    if (!melee && kind == 1) {
        bullet_hole(eye, dir, hit, -1);
    } else if (!melee && kind == 2 && G.phys.bodies[idx].active) {
        bullet_hole(eye, dir, hit, idx);
    }
    if (sound && kind) {
        int s = kind >= 3 ? G.snd.bullet_flesh : melee ? G.snd.crowbar_hit : G.snd.bullet_impact;
        if (g_settings.sound) snd_play_at(s, hit, 0.6f);
    }
    if (kind == 2 && world && g_gm.on && body_valid(idx)) script_entity_damage(idx, shooter, ws->damage);
    return kind;
}

// Plays an animation on the first-person weapon in hand (when it has animated models)
static void vm_event(int kind) {
    if (G.vm_anim[G.weapon] >= 0) vmodel_play(G.vm_anim[G.weapon], kind, 0);
}

// Each frame: the draw animation when the weapon changes, and the physics guns' hold loop
static void vm_tick(float dt) {
    int vm = G.vm_anim[G.weapon];
    if ((int)G.weapon != G.vm_last_weapon) {
        G.vm_last_weapon = (int)G.weapon;
        if (vm >= 0) vmodel_play(vm, VM_DRAW, 0);
    }
    if (vm >= 0 && (G.weapon == WEAPON_PHYSGUN || G.weapon == WEAPON_GRAVGUN)) {
        int holding = !G.use_carry && (G.held >= 0 || G.held_rag >= 0);
        int k = vmodel_playing();
        if (holding && k != VM_RELOAD && k != VM_DRAW) vmodel_play(vm, VM_RELOAD, 1); // c_physcannon's hold
        if (!holding && k == VM_RELOAD) vmodel_play(vm, VM_IDLE, 0);
    }
    vmodel_update(dt);
}

// A whole shot: every pellet, from eye along f. effects: tracers (only the shooter sees them).
// Returns what the first pellet hit (fire_bullet's kinds).
static int shoot(int weapon, vec3 eye, vec3 f, uint32_t seed, int world, int effects) {
    const WeaponStats *ws = &k_ws[weapon];
    vec3 r = v3_cross(f, v3(0, 0, 1));
    r = v3_len(r) > 1e-3f ? v3_norm(r) : v3(1, 0, 0);
    vec3 u = v3_cross(r, f);
    int first = 0;
    for (int p = 0; p < ws->pellets; p++) {
        float sx = spread_rand(&seed) * ws->spread, sy = spread_rand(&seed) * ws->spread;
        vec3 d = v3_norm(v3_add(f, v3_add(v3_scale(r, sx), v3_scale(u, sy))));
        vec3 end;
        int kind = fire_bullet(eye, d, ws, world, p == 0, &end);
        if (p == 0) first = kind;
        if (effects && ws->clip) add_tracer(gun_muzzle(), end);
    }
    return first;
}

static float rand01(void);

// --- Projectiles: rockets, crossbow bolts and bug bait, and grenades ------------------------------
// The host (or single player) flies them, tracing ahead each frame; multiplayer clients get them in
// PK_PROJ to draw (moving them on between packets), with the latest explosions. Grenades are props that
// bounce about (synced as bodies) until their fuse burns down.

#define MAX_PROJECTILES 16
#define MAX_GRENADES 8
enum { PROJ_ROCKET = 1, PROJ_BOLT, PROJ_BAIT };
#define ROCKET_SPEED 1300.0f
#define ROCKET_TURN 2.5f   // rad/s: how fast a rocket swings round to the RPG's laser
#define ROCKET_LIFE 8.0f
#define BOLT_SPEED 3500.0f
#define BOLT_DROP 120.0f   // u/s^2
#define BOLT_DAMAGE 100.0f
#define BOLT_STUCK_TIME 10.0f
#define BAIT_SPEED 900.0f
#define THROWN_GRAVITY 600.0f
#define GRENADE_SPEED 850.0f
#define GRENADE_FUSE 2.5f

typedef struct {
    int type;    // PROJ_*, 0 = free
    int owner;   // the player slot that fired it
    vec3 pos, vel;
    float life;  // flying: until it gives out; a stuck bolt: until it goes
    int stuck;   // a bolt in a wall
    float trail; // a rocket: until its next puff of smoke
} Projectile;
static Projectile g_proj[MAX_PROJECTILES];
static struct {
    int body, owner;
    float fuse, tick; // fuse <= 0: a free slot
} g_gren[MAX_GRENADES];
// The latest explosions, for clients: g_fx_seq counts them and g_fx_pos[k] is number g_fx_seq - k
static uint8_t g_fx_seq, g_fx_seen;
static int g_fx_synced;        // client: g_fx_seen is set
static vec3 g_fx_pos[4];
static float g_fx_age = 99.0f; // host: seconds since the last one (sent for a second after)

static void fx_explosion(vec3 c) {
    memmove(&g_fx_pos[1], &g_fx_pos[0], sizeof(vec3) * 3);
    g_fx_pos[0] = c;
    g_fx_seq++;
    g_fx_age = 0.0f;
}

static void projectiles_clear(void) {
    memset(g_proj, 0, sizeof(g_proj));
    memset(g_gren, 0, sizeof(g_gren));
    g_fx_synced = 0;
    g_fx_age = 99.0f;
}

// A puff of an emitter's particles (rocket smoke, bug bait splats)
static void puff(int effect, vec3 pos, vec3 vel, float life, float size) {
    for (int k = 0; k < MAX_PARTICLES; k++) {
        Particle *p = &g_parts[k];
        if (p->life > 0.0f) continue;
        p->effect = effect;
        p->pos = pos;
        p->vel = vel;
        p->max_life = p->life = life;
        p->size = size;
        return;
    }
}

// Where a player is looking: this PSP's view, or a remote player's from their last state. 0 if they're
// not in the game (or dead); weapon gets what they hold.
static int player_view(int slot, vec3 *eye, vec3 *dir, int *weapon) {
    if (slot == G.my_slot) {
        if (G.dead_time > 0.0f || (g_dedicated && slot == 0)) return 0;
        *eye = player_eye(&G.player);
        *dir = view_forward();
        *weapon = G.weapon;
        return 1;
    }
    if (slot <= 0 || slot >= NET_MAX_PLAYERS) return 0;
    const Peer *p = &G.peers[slot];
    if (!p->active || p->health <= 0.0f) return 0;
    float cp = cosf(p->pitch * DEG2RAD), sp = sinf(p->pitch * DEG2RAD);
    float cy = cosf(p->yaw * DEG2RAD), sy = sinf(p->yaw * DEG2RAD);
    *eye = v3_add(p->pos, v3(0, 0, (p->flags & PF_DUCKED) ? 28.0f : 64.0f));
    *dir = v3(cp * cy, cp * sy, sp);
    *weapon = p->weapon;
    return 1;
}

static void grenade_throw(int owner, vec3 eye, vec3 dir) {
    int m = prop_named("_grenade");
    if (m < 0) {
        toast("No grenade model in the prop pack");
        return;
    }
    int k;
    for (k = 0; k < MAX_GRENADES && g_gren[k].fuse > 0.0f; k++) {
    }
    if (k == MAX_GRENADES) return;
    int i = new_body(m, v3_mad(eye, dir, 20.0f), q_axis_angle(v3(0, 0, 1), rand01() * 6.28f), 0xFFFFFFFF, NULL);
    if (i < 0) {
        toast("Too many physics objects");
        return;
    }
    Body *b = &G.phys.bodies[i];
    b->vel = v3_add(v3_scale(dir, GRENADE_SPEED), v3(0, 0, 120.0f));
    b->angvel = v3(rand01() * 8.0f - 4.0f, rand01() * 8.0f - 4.0f, rand01() * 4.0f - 2.0f);
    b->life = GRENADE_FUSE + 1.0f; // a live grenade isn't a spawned prop (limit, saves), and goes if it's lost
    G.body_owner[i] = owner;
    phys_wake(&G.phys, i);
    g_gren[k].body = i;
    g_gren[k].owner = owner;
    g_gren[k].fuse = GRENADE_FUSE;
    g_gren[k].tick = 0.0f;
}

// Fires (or throws) weapon w's projectile for player `owner`, from eye along dir. Host and single player.
static void proj_launch(int owner, int w, vec3 eye, vec3 dir) {
    dir = v3_norm(dir);
    if (w == WEAPON_GRENADE) {
        grenade_throw(owner, eye, dir);
        return;
    }
    Projectile *p = NULL;
    for (int k = 0; k < MAX_PROJECTILES && !p; k++) {
        if (!g_proj[k].type) p = &g_proj[k];
    }
    for (int k = 0; k < MAX_PROJECTILES && !p; k++) {
        if (g_proj[k].stuck) p = &g_proj[k]; // full: an old bolt in a wall makes way
    }
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->owner = owner;
    p->pos = v3_mad(eye, dir, 16.0f);
    if (w == WEAPON_RPG) {
        p->type = PROJ_ROCKET;
        p->vel = v3_scale(dir, ROCKET_SPEED);
        p->life = ROCKET_LIFE;
    } else if (w == WEAPON_CROSSBOW) {
        p->type = PROJ_BOLT;
        p->vel = v3_scale(dir, BOLT_SPEED);
        p->life = 4.0f;
    } else {
        p->type = PROJ_BAIT;
        p->vel = v3_add(v3_scale(dir, BAIT_SPEED), v3(0, 0, 120.0f));
        p->life = 6.0f;
    }
}

// The nearest thing on the way from a to b: 0 nothing, 1 the world, 2 a prop, 3 an NPC, 4 a ragdoll,
// 5 a player (never `skip`). Fills how far along (0..1), what was hit (idx, bone) and the world's normal.
static int proj_trace(vec3 a, vec3 b, int skip, float *frac, int *idx, int *bone, vec3 *normal) {
    vec3 d = v3_sub(b, a);
    float len = v3_len(d), t;
    if (len < 1e-3f) return 0;
    d = v3_scale(d, 1.0f / len);
    Trace tr;
    cm_trace_line(&G.map, a, b, MASK_PROPSOLID, NULL, 0, &tr);
    float best = tr.fraction * len;
    int kind = tr.fraction < 1.0f;
    *normal = tr.normal;
    int i = phys_raycast(&G.phys, a, d, best, &t);
    if (i >= 0) {
        best = t;
        kind = 2;
        *idx = i;
    }
    i = npc_raycast(a, d, best, &t);
    if (i >= 0) {
        best = t;
        kind = 3;
        *idx = i;
    }
    int rb;
    i = npc_ragdoll_raycast(a, d, best, &t, &rb);
    if (i >= 0) {
        best = t;
        kind = 4;
        *idx = i;
        *bone = rb;
    }
    i = player_raycast(a, d, best, skip, &t);
    if (i >= 0) {
        best = t;
        kind = 5;
        *idx = i;
    }
    *frac = best / len;
    return kind;
}

// A crossbow bolt arriving: hurts what it hits, sticks in walls
static void bolt_hit(Projectile *p, int kind, int idx, int bone, vec3 hit, vec3 dir) {
    p->type = 0;
    if (kind == 3) {
        g_npc_attacker = p->owner;
        npc_damage_dir(idx, BOLT_DAMAGE, v3_scale(dir, 600.0f));
        g_npc_attacker = -1;
        blood_spray(hit, dir, 8);
    } else if (kind == 5) {
        pvp_damage(idx, p->owner, BOLT_DAMAGE);
        blood_spray(hit, dir, 8);
    } else if (kind == 4) {
        npc_ragdoll_push(idx, bone, v3_scale(dir, 900.0f));
    } else if (kind == 2 && break_group(&G.phys.bodies[idx])) {
        break_body(idx);
    } else if (kind == 2) {
        Body *b = &G.phys.bodies[idx];
        if (!b->frozen) {
            phys_wake(&G.phys, idx);
            b->vel = v3_mad(b->vel, dir, fminf(600.0f * 50.0f / b->mass, 1500.0f));
        }
    } else {
        // The world: it stays in the wall a while, sticking out
        p->type = PROJ_BOLT;
        p->stuck = 1;
        p->pos = v3_mad(hit, dir, -6.0f);
        p->life = BOLT_STUCK_TIME;
    }
    if (g_settings.sound) snd_play_at(kind >= 3 ? G.snd.bolt_flesh : G.snd.bolt_hit, hit, 0.8f);
}

// Host and single player, each frame: grenades' fuses, then everything in flight
static void projectiles_update(float dt) {
    g_fx_age += dt;
    for (int k = 0; k < MAX_GRENADES; k++) {
        if (g_gren[k].fuse <= 0.0f) continue;
        int i = g_gren[k].body;
        if (!body_valid(i)) { // removed (cleaned up, undone)
            g_gren[k].fuse = 0.0f;
            continue;
        }
        g_gren[k].fuse -= dt;
        g_gren[k].tick -= dt;
        if (g_gren[k].tick <= 0.0f && g_gren[k].fuse > 0.0f) {
            g_gren[k].tick = g_gren[k].fuse < 1.0f ? 0.25f : 0.5f; // ticks faster at the end, like HL2's
            play_at(G.snd.gren_tick, G.phys.bodies[i].pos, 0.6f);
        }
        if (g_gren[k].fuse <= 0.0f) {
            vec3 c = G.phys.bodies[i].pos;
            remove_body(i);
            explode_at(c, g_gren[k].owner);
        }
    }
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        Projectile *p = &g_proj[k];
        if (!p->type) continue;
        p->life -= dt;
        if (p->life <= 0.0f) {
            if (p->type == PROJ_ROCKET && !p->stuck) explode_at(p->pos, p->owner); // out of fuel
            p->type = 0;
            continue;
        }
        if (p->stuck) continue;
        if (p->type == PROJ_ROCKET) {
            // Laser guided: it turns towards wherever its owner aims, while they hold the RPG
            vec3 eye, dir;
            int w;
            if (player_view(p->owner, &eye, &dir, &w) && w == WEAPON_RPG) {
                Trace tr;
                vec3 far = v3_mad(eye, dir, 8192.0f);
                cm_trace_line(&G.map, eye, far, MASK_PROPSOLID, NULL, 0, &tr);
                vec3 aim = tr.endpos, cur = v3_norm(p->vel), want = v3_sub(aim, p->pos);
                if (v3_len(want) > 32.0f) {
                    want = v3_norm(want);
                    float k2 = clampf(ROCKET_TURN * dt, 0.0f, 1.0f);
                    p->vel = v3_scale(v3_norm(v3_add(cur, v3_scale(v3_sub(want, cur), k2))), ROCKET_SPEED);
                }
            }
        } else {
            p->vel.z -= (p->type == PROJ_BOLT ? BOLT_DROP : THROWN_GRAVITY) * dt;
        }
        vec3 next = v3_mad(p->pos, p->vel, dt), n;
        float frac;
        int idx = -1, bone = 0;
        int kind = proj_trace(p->pos, next, p->owner, &frac, &idx, &bone, &n);
        if (!kind) {
            p->pos = next;
            continue;
        }
        vec3 hit = v3_add(p->pos, v3_scale(v3_sub(next, p->pos), frac)), dir = v3_norm(p->vel);
        if (p->type == PROJ_ROCKET) {
            p->type = 0;
            explode_at(v3_mad(hit, dir, -8.0f), p->owner);
        } else if (p->type == PROJ_BOLT) {
            bolt_hit(p, kind, idx, bone, hit, dir);
        } else {
            // Bug bait bursts: antlions make for the spot
            p->type = 0;
            vec3 at = v3_mad(hit, kind == 1 ? n : v3_scale(dir, -1.0f), 8.0f);
            npc_bugbait_throw(at);
            play_at(G.snd.bait_splat, hit, 0.9f);
            for (int s = 0; s < 6; s++) {
                puff(EMIT_SPARKS, at, v3((rand01() - 0.5f) * 160.0f, (rand01() - 0.5f) * 160.0f, rand01() * 120.0f), 0.5f, 1.6f);
            }
        }
    }
}

// Smoke behind rockets, on everyone's screen
static void projectiles_trails(float dt) {
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        Projectile *p = &g_proj[k];
        if (p->type != PROJ_ROCKET || p->stuck) continue;
        p->trail -= dt;
        if (p->trail > 0.0f) continue;
        p->trail = 0.03f;
        puff(EMIT_SMOKE, v3_mad(p->pos, v3_norm(p->vel), -14.0f), v3(0, 0, 15.0f), 1.2f, 5.0f);
    }
}

// Multiplayer client: moves them on between the host's packets (it does the hitting)
static void projectiles_client_update(float dt) {
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        Projectile *p = &g_proj[k];
        if (!p->type || p->stuck) continue;
        if (p->type != PROJ_ROCKET) p->vel.z -= (p->type == PROJ_BOLT ? BOLT_DROP : THROWN_GRAVITY) * dt;
        p->pos = v3_mad(p->pos, p->vel, dt);
    }
}

static void draw_projectiles(void) {
    static const char *const models[] = {NULL, "_missile", "_bolt", "_bugbait"};
    if (!G.props_loaded) return;
    proplib_begin();
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        const Projectile *p = &g_proj[k];
        if (!p->type) continue;
        int model = prop_named(models[p->type]);
        if (model < 0) continue;
        // The model's +X along its flight
        vec3 x = v3_len(p->vel) > 1e-3f ? v3_norm(p->vel) : v3(1, 0, 0), up = fabsf(x.z) > 0.95f ? v3(1, 0, 0) : v3(0, 0, 1);
        vec3 y = v3_norm(v3_cross(up, x)), z = v3_cross(x, y);
        ScePspFMatrix4 m = {{x.x, x.y, x.z, 0}, {y.x, y.y, y.z, 0}, {z.x, z.y, z.z, 0}, {p->pos.x, p->pos.y, p->pos.z, 1}};
        int leaf = map_point_leaf(&G.map, &p->pos);
        unsigned int amb = (leaf >= 0 && leaf < G.map.num_leafs) ? G.map.leafs[leaf].ambient_abgr : G.map.info->ambient_abgr;
        PropLight l;
        make_light(&l, amb, 1, 0xFFFFFFFF);
        proplib_draw(&G.props, model, &m, &l);
    }
    proplib_end();
}

// What the weapon in hand fires: the host's projectile, or a command asking it for one
static void launch(int w, vec3 eye, vec3 f) {
    if (is_client()) {
        float v[6] = {eye.x, eye.y, eye.z, f.x, f.y, f.z};
        send_cmd(CMD_LAUNCH, w, 0, 0, v, 6);
        return;
    }
    proj_launch(G.my_slot, w, eye, f);
}

// Bug bait: tap R to throw it (antlions swarm the spot), hold R to squeeze it (they come and follow you)
#define BAIT_SQUEEZE_HOLD 0.45f
static void bugbait_update(const InputState *in, float dt) {
    if (in->pressed & PSP_CTRL_RTRIGGER) {
        G.bait_hold = 0.0f;
        G.bait_squeezed = 0;
        G.bait_down = 1;
    }
    if (!G.bait_down) return;
    if (in->held & PSP_CTRL_RTRIGGER) {
        G.bait_hold += dt;
        if (G.bait_hold >= BAIT_SQUEEZE_HOLD && !G.bait_squeezed) {
            G.bait_squeezed = 1;
            play(G.snd.bait_squeeze, 0.8f);
            vm_event(VM_ALT);
            if (is_client()) {
                send_cmd(CMD_SQUEEZE, 0, 0, 0, NULL, 0);
            } else {
                npc_bugbait_squeeze(G.my_slot);
            }
            notify("Antlions follow you now", NOTE_HINT);
        }
        return;
    }
    G.bait_down = 0;
    if (G.bait_squeezed || G.fire_cd > 0.0f) return;
    G.fire_cd = k_ws[WEAPON_BUGBAIT].delay;
    if (g_settings.sound) snd_play_chan(G.snd.fire[WEAPON_BUGBAIT], 0.6f, SND_CHAN_WEAPON);
    vm_event(VM_FIRE);
    launch(WEAPON_BUGBAIT, player_eye(&G.player), view_forward());
}

// --- Quick chat ------------------------------------------------------------------------------------
// Hold D-pad Down: a wheel of short messages and gestures (the consoles have no keyboard). The circle pad
// picks a slice, L / R turn the page, and letting go of Down sends it. Messages go in everyone's chat
// ("name: Hello!"; a client asks the host, CMD_QUICKCHAT); gestures play on your character for everyone
// (NetPlayer.gesture, CMD_GESTURE), and on your own in the third-person view.

static void quick_chat_send(int page, int sel) {
    if (sel < 0 || sel >= QC_SLICES) return;
    if (page == 0) {
        if (g_qc.cooldown > 0.0f) return;
        g_qc.cooldown = QC_COOLDOWN;
        char text[48];
        snprintf(text, sizeof(text), "%s: %s", net_nickname(), k_quick_msgs[sel]);
        if (is_client()) {
            send_cmd(CMD_QUICKCHAT, sel, 0, 0, NULL, 0); // it comes back in the host's chat
        } else if (!script_quick_chat(G.my_slot, k_quick_msgs[sel])) {
            game_chat(text);
        }
        return;
    }
    if (sel >= npc_gesture_count()) return;
    if (!npc_type_can_gesture(player_model())) {
        notify("Your player model can't do gestures (Options > Player model)", NOTE_ERROR);
        return;
    }
    npc_player_gesture(-1, sel);
    g_qc.gesture = sel + 1;
    g_qc.gesture_seq = (g_qc.gesture_seq + 1) & 255;
    if (is_client()) send_cmd(CMD_GESTURE, sel, 0, 0, NULL, 0);
    if (!third_person()) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%s (the others see it)", npc_gesture_name(sel));
        notify(msg, NOTE_HINT);
    }
}

// Each frame: 1 while the wheel has the controls
static int quick_chat_update(const InputState *in, float dt) {
    g_qc.tap = 0;
    g_qc.cooldown -= dt;
    if (g_qc.test) return 1;
    int allowed = !G.player.noclip && G.held < 0 && G.held_rag < 0 && G.veh < 0 && G.dead_time <= 0.0f;
    if (!(in->held & PSP_CTRL_DOWN) || !allowed) {
        int was_open = g_qc.open;
        if (was_open && allowed) {
            quick_chat_send(g_qc.page, g_qc.sel);
        } else if ((in->released & PSP_CTRL_DOWN) && g_qc.hold < QC_HOLD) {
            g_qc.tap = 1;
        }
        g_qc.open = 0;
        g_qc.hold = 0.0f;
        return was_open;
    }
    g_qc.hold += dt;
    if (!g_qc.open) {
        if (g_qc.hold < QC_HOLD) return 0;
        g_qc.open = 1;
        g_qc.sel = -1;
        play(G.snd.menu_select, 0.5f);
    }
    if (in->pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        g_qc.page ^= 1;
        g_qc.sel = -1;
        play(G.snd.menu_move, 0.4f);
    }
    // The stick's direction picks a slice: 0 at the top, then round clockwise (it stays picked when the
    // stick springs back)
    if (in->ax * in->ax + in->ay * in->ay > 0.25f) {
        float a = atan2f(in->ax, -in->ay);
        int s = ((int)floorf(a / (2.0f * PI) * QC_SLICES + 0.5f) + QC_SLICES) % QC_SLICES;
        if (s != g_qc.sel) play(G.snd.menu_move, 0.3f);
        g_qc.sel = s;
    }
    return 1;
}

static void draw_quick_chat(void) {
    if (!g_qc.open) return;
    const float cx = 240.0f, cy = 132.0f;
    ui_rrect(cx - QC_RADIUS - 46, cy - QC_RADIUS - 18, (QC_RADIUS + 46) * 2, (QC_RADIUS + 18) * 2, 40, 0xA0000000);
    int gestures = g_qc.page == 1;
    for (int s = 0; s < QC_SLICES; s++) {
        float a = (float)s * (2.0f * PI / QC_SLICES);
        float x = cx + sinf(a) * QC_RADIUS, y = cy - cosf(a) * QC_RADIUS;
        const char *label = gestures ? (s < npc_gesture_count() ? npc_gesture_name(s) : "-") : k_quick_msgs[s];
        float w = ui_text_width(FONT_SMALL, label) + 12.0f;
        if (s == g_qc.sel) ui_rrect(x - w * 0.5f, y - 9, w, 18, 4, 0xFF2AA8F0);
        ui_text(FONT_SMALL, x, y - 6, s == g_qc.sel ? 0xFFFFFFFF : 0xFFE0E0E0, ALIGN_CENTER, label);
    }
    ui_text(FONT_BOLD, cx, cy - 16, 0xFFFFFFFF, ALIGN_CENTER, gestures ? "Gestures" : "Messages");
    ui_text(FONT_SMALL, cx, cy + 2, 0xFFA0A0A0, ALIGN_CENTER, "L / R: page");
    // The hint above the wheel: below it are the health and suit boxes
    float hy = cy - QC_RADIUS - 36;
    if (gestures && !npc_gesture_count()) {
        ui_text_shadow(FONT_SMALL, cx, hy, 0xFFE0E0E0, ALIGN_CENTER, "Gestures need the NPC pack rebuilt");
    } else {
        ui_text_shadow(FONT_SMALL, cx, hy, 0xFFE0E0E0, ALIGN_CENTER, "Circle pad: pick    Let go of Down: send");
    }
}

// --- Items: medkits, batteries, ammo and chargers ----------------------------------------------------
// Health kits, vials, suit batteries and ammo boxes are props you take by walking over them (when they'd help);
// health and suit chargers stay where they're put and charge you after a tap of Use, while they've charge
// left (they slowly fill up again). Each PSP looks after its own player: a multiplayer client asks the host
// to remove what it took (CMD_PICKUP) and to add the health (CMD_HEAL); suit power and ammo are each PSP's own.

#define PICKUP_RANGE 48.0f
#define CHARGE_RANGE 120.0f
#define CHARGE_RATE 12.0f     // points a second
#define HEALTH_CHARGER_CAP 50.0f
#define SUIT_CHARGER_CAP 75.0f
#define CHARGER_REFILL 1.0f   // points a second while nobody's using it
#define MAX_HEALTH 100.0f
#define MAX_SUIT 100.0f

static float g_charge[MAX_BODIES];     // each charger's charge left (as this PSP sees it)
static int g_charge_model[MAX_BODIES]; // the model the charge belongs to, + 1 (0: not set)

// By the prop's name, so every entry of it counts (the Combine spawnlist has a Health Charger too)
#define MAX_ITEM_MODELS 1024
static int item_kind(int body) {
    static unsigned char kinds[MAX_ITEM_MODELS];
    static int ready;
    if (!body_valid(body) || !G.props_loaded) return ITEM_NONE;
    if (!ready) {
        static const char *const names[ITEM_COUNT] = {
            NULL,         "Health Kit", "Health Vial", "Suit Battery",   "Health Charger", "Suit Charger",
            "Pistol Ammo", ".357 Ammo", "SMG Ammo",    "Shotgun Shells", "AR2 Ammo",       "Crossbow Bolts",
            "RPG Round",  "Grenade"};
        ready = 1;
        for (int m = 0; m < G.props.num_models && m < MAX_ITEM_MODELS; m++) {
            for (int k = 1; k < ITEM_COUNT; k++) {
                if (strncmp(G.props.models[m].name, names[k], sizeof(G.props.models[m].name)) == 0) kinds[m] = (unsigned char)k;
            }
        }
    }
    const Body *b = &G.phys.bodies[body];
    if (b->lib != 0 || b->model < 0 || b->model >= G.props.num_models || b->model >= MAX_ITEM_MODELS) return ITEM_NONE;
    return kinds[b->model];
}

// Test command "charge": Use on the charger in front
static void test_use_item(void) {
    Aim a = aim_trace(CHARGE_RANGE);
    if (a.body >= 0 && is_charger(item_kind(a.body))) charger_use(a.body);
}

static float item_amount(int kind) {
    return kind == ITEM_HEALTHKIT ? 25.0f : kind == ITEM_HEALTHVIAL ? 10.0f : 15.0f;
}

// Ammo boxes, by kind from ITEM_AMMO_PISTOL: the gun, how much a box holds (HL2's) and the most you can
// carry for it (HL2's, or what you start with when that's more)
static const struct {
    int weapon, amount, max;
    const char *name;
} k_ammo_items[] = {
    {WEAPON_PISTOL, 20, 150, "Pistol ammo"},     {WEAPON_357, 6, 36, ".357 ammo"},    {WEAPON_SMG, 45, 270, "SMG ammo"},
    {WEAPON_SHOTGUN, 20, 34, "Shotgun shells"},  {WEAPON_AR2, 20, 90, "AR2 ammo"},     {WEAPON_CROSSBOW, 6, 10, "Bolts"},
    {WEAPON_RPG, 1, 3, "RPG rounds"},            {WEAPON_GRENADE, 1, 5, "Grenades"},
};

// A charger's charge left: full the first time it's seen
static float *charger_left(int body) {
    int m = G.phys.bodies[body].model + 1;
    if (g_charge_model[body] != m) {
        g_charge_model[body] = m;
        g_charge[body] = item_kind(body) == ITEM_SUIT_CHARGER ? SUIT_CHARGER_CAP : HEALTH_CHARGER_CAP;
    }
    return &g_charge[body];
}

static void items_clear(void) {
    memset(g_charge_model, 0, sizeof(g_charge_model));
    G.charging = -1;
    G.charge_snd = -1;
    G.heal_pending = 0.0f;
    G.taken_body = -1;
}

// Health for this PSP's player. A multiplayer client's is the host's to change: it's asked (in small amounts)
// and the new health comes back in a snapshot (adding it here first would look like damage when it did).
static void heal_self(float amount) {
    if (is_client()) {
        G.heal_pending += amount;
    } else {
        G.health = fminf(MAX_HEALTH, G.health + amount);
    }
}

static void flush_heal(void) {
    if (!is_client() || G.heal_pending <= 0.0f) return;
    float f[1] = {G.heal_pending};
    send_cmd(CMD_HEAL, 0, 0, 0, f, 1);
    G.heal_pending = 0.0f;
}

static void stop_charging(int sound) {
    if (G.charge_snd >= 0) snd_stop(G.charge_snd);
    G.charge_snd = -1;
    if (sound && G.charging >= 0) {
        int suit = item_kind(G.charging) == ITEM_SUIT_CHARGER;
        play(suit ? G.snd.charge_done_suit : G.snd.charge_no_health, 0.7f);
    }
    G.charging = -1;
    flush_heal();
}

// Use on a charger: start charging, or stop if it's the one already charging
static void charger_use(int body) {
    if (G.charging == body) {
        stop_charging(1);
        return;
    }
    stop_charging(0);
    int suit = item_kind(body) == ITEM_SUIT_CHARGER;
    float need = suit ? (g_settings.hev_suit ? MAX_SUIT - G.suit : 0.0f) : MAX_HEALTH - G.health;
    if (*charger_left(body) <= 0.5f || need <= 0.5f) {
        play(suit ? G.snd.charge_no_suit : G.snd.charge_no_health, 0.7f);
        notify(*charger_left(body) <= 0.5f ? "It's empty: it fills up again slowly"
               : suit                     ? "Your suit's charged (or the HEV suit is off in Options)"
                                          : "You're at full health",
               NOTE_HINT);
        return;
    }
    G.charging = body;
    if (g_settings.sound) G.charge_snd = snd_loop(suit ? G.snd.charge_suit : G.snd.charge_health, 0.6f);
}

// Each frame (every PSP, for its own player): chargers refill, kits and batteries are taken, charging goes on
static void items_update(float dt) {
    for (int i = 0; i < MAX_BODIES; i++) {
        if (!g_charge_model[i] || i == G.charging) continue;
        float cap = item_kind(i) == ITEM_SUIT_CHARGER ? SUIT_CHARGER_CAP : HEALTH_CHARGER_CAP;
        g_charge[i] = fminf(cap, g_charge[i] + CHARGER_REFILL * dt);
    }
    if (G.taken_time > 0.0f) G.taken_time -= dt;
    G.heal_timer -= dt;
    if (G.heal_timer <= 0.0f) {
        G.heal_timer = 0.25f;
        flush_heal();
    }
    if (G.dead_time > 0.0f || G.player.noclip || G.veh >= 0 || (g_dedicated && G.my_slot == 0)) {
        if (G.charging >= 0) stop_charging(0);
        return;
    }
    vec3 c = v3_add(G.player.origin, v3(0, 0, 36));
    for (int i = 0; i < MAX_BODIES; i++) {
        int kind = item_kind(i);
        if (!is_pickup(kind) || (i == G.taken_body && G.taken_time > 0.0f)) continue;
        if (v3_len(v3_sub(G.phys.bodies[i].pos, c)) > PICKUP_RANGE) continue;
        char msg[32];
        if (kind >= ITEM_AMMO_PISTOL) {
            // Ammo: into the gun's reserve, up to what you can carry (none needed with unlimited ammo)
            int w = k_ammo_items[kind - ITEM_AMMO_PISTOL].weapon, max = k_ammo_items[kind - ITEM_AMMO_PISTOL].max;
            if (unlimited_ammo() || G.ammo[w] >= max) continue;
            int got = k_ammo_items[kind - ITEM_AMMO_PISTOL].amount;
            if (got > max - G.ammo[w]) got = max - G.ammo[w];
            G.ammo[w] += got;
            snprintf(msg, sizeof(msg), "%s +%d", k_ammo_items[kind - ITEM_AMMO_PISTOL].name, got);
            notify(msg, NOTE_HINT);
            play(G.snd.ammo, 0.8f);
        } else {
            int battery = kind == ITEM_BATTERY;
            if (battery ? !(g_settings.hev_suit && G.suit < MAX_SUIT - 0.5f) : G.health >= MAX_HEALTH - 0.5f) continue;
            float amount = item_amount(kind);
            snprintf(msg, sizeof(msg), battery ? "Suit +%d" : "Health +%d", (int)amount);
            notify(msg, NOTE_HINT);
            play(battery ? G.snd.battery : kind == ITEM_HEALTHKIT ? G.snd.medkit : G.snd.medvial, 0.8f);
            if (battery) {
                G.suit = fminf(MAX_SUIT, G.suit + amount);
            } else if (!is_client()) { // a client's health: the host adds it when it takes the kit away
                G.health = fminf(MAX_HEALTH, G.health + amount);
            }
        }
        if (is_client()) {
            // The host takes it away (and adds the health) when it hears; until then this one's ours
            send_cmd(CMD_PICKUP, i, 0, 0, NULL, 0);
            G.taken_body = i;
            G.taken_time = 2.0f;
        } else {
            remove_body(i);
        }
        break;
    }
    if (G.charging < 0) return;
    int kind = item_kind(G.charging);
    if ((kind != ITEM_HEALTH_CHARGER && kind != ITEM_SUIT_CHARGER) ||
        v3_len(v3_sub(G.phys.bodies[G.charging].pos, player_eye(&G.player))) > CHARGE_RANGE + 40.0f) {
        stop_charging(0); // walked away (or it's gone)
        return;
    }
    int suit = kind == ITEM_SUIT_CHARGER;
    float *left = charger_left(G.charging);
    float need = suit ? (g_settings.hev_suit ? MAX_SUIT - G.suit : 0.0f) : MAX_HEALTH - G.health;
    if (need <= 0.5f || *left <= 0.0f) {
        stop_charging(1);
        return;
    }
    float d = fminf(CHARGE_RATE * dt, fminf(need, *left));
    *left -= d;
    if (suit) {
        G.suit += d;
    } else {
        heal_self(d);
    }
}

static void start_reload(void) {
    const WeaponStats *ws = &k_ws[G.weapon];
    if (!ws->clip || G.clip[G.weapon] >= ws->clip || G.ammo[G.weapon] <= 0 || G.reload_left > 0.0f) return;
    G.reload_left = ws->reload;
    play(G.weapon == WEAPON_SHOTGUN ? G.snd.pump : G.snd.reload, 0.6f);
    // The shotgun loads shell by shell: start, then a loop of shells (gun_update), then the finish
    vm_event(G.weapon == WEAPON_SHOTGUN && vmodel_has(G.vm_anim[G.weapon], VM_RELOAD_START) ? VM_RELOAD_START : VM_RELOAD);
}

static void gun_update(const InputState *in, float dt) {
    int w = G.weapon;
    const WeaponStats *ws = &k_ws[w];
    if (w == WEAPON_BUGBAIT) {
        bugbait_update(in, dt);
        return;
    }
    if (G.reload_left > 0.0f) {
        G.reload_left -= dt;
        int vm = G.vm_anim[w];
        if (w == WEAPON_SHOTGUN && vm >= 0 && vmodel_has(vm, VM_RELOAD_START)) {
            float end_len = vmodel_length(vm, VM_RELOAD_END);
            int k = vmodel_playing();
            if (G.reload_left > end_len && k == VM_IDLE) vmodel_play(vm, VM_RELOAD, 1); // the start is done
            if (G.reload_left <= end_len && k == VM_RELOAD) vmodel_play(vm, VM_RELOAD_END, 0);
        }
        if (G.reload_left <= 0.0f) {
            int take = ws->clip - G.clip[w];
            if (take > G.ammo[w]) take = G.ammo[w];
            G.clip[w] += take;
            G.ammo[w] -= take;
            if (w == WEAPON_SHOTGUN) {
                play(G.snd.pump, 0.6f);
                vm_event(VM_PUMP);
            }
        }
        return;
    }
    if (unlimited_ammo() && ws->clip) G.clip[w] = ws->clip; // turned on with an empty gun
    int want = ws->automatic ? (in->held & PSP_CTRL_RTRIGGER) : (in->pressed & PSP_CTRL_RTRIGGER);
    if (!want || G.fire_cd > 0.0f) return;
    if (ws->clip && G.clip[w] == 0) {
        if (G.ammo[w] > 0) {
            start_reload();
        } else {
            play(G.snd.empty, 0.5f);
            G.fire_cd = 0.3f;
        }
        return;
    }
    G.fire_cd = ws->delay;
    if (ws->clip && !unlimited_ammo()) G.clip[w]--;
#ifdef PS2
    play(G.snd.fire[w], ws->clip ? 0.8f : 0.5f);
#else
    if (g_settings.sound) snd_play_chan(G.snd.fire[w], ws->clip ? 0.8f : 0.5f, SND_CHAN_WEAPON);
#endif
    G.kick = ws->clip ? ws->kick * 0.6f : 4.0f;
    G.pitch = clampf(G.pitch + ws->kick * 0.4f, -89.0f, 89.0f); // view punch
    if (ws->clip && w != WEAPON_CROSSBOW && w != WEAPON_GRENADE) G.muzzle_time = 0.05f;
    vec3 eye = player_eye(&G.player), f = view_forward();
    if (is_launcher(w)) {
        // The crossbow, the RPG and grenades: what they fire flies (see Projectiles)
        launch(w, eye, f);
        vm_event(VM_FIRE);
    } else {
        uint32_t seed = sceKernelGetSystemTimeLow();
        int hit = shoot(w, eye, f, seed, !is_client(), 1);
        vm_event(ws->clip ? VM_FIRE : hit ? VM_ALT : VM_FIRE); // the crowbar: a hit, or a swing through air
        if (is_client()) {
            float v[6] = {eye.x, eye.y, eye.z, f.x, f.y, f.z};
            send_cmd(CMD_SHOOT, w, (int)seed, 0, v, 6);
        }
    }
    if (ws->clip && G.clip[w] == 0) start_reload(); // like HL2, reload as soon as it's empty
}

// --- Spawn menu --------------------------------------------------------------------------------

#define GRID_COLS 4
#define GRID_ROWS 3
#define NUM_TOOL_ROWS (TOOL_COUNT + 1) // every tool, then the colour swatches

// Tools panel rows in display order (grouped), with the colour row last
static Tool tool_at_row(int row) {
    int r = 0;
    for (int g = 0; g < NUM_TOOL_GROUPS; g++) {
        for (int k = 0; k < k_tool_groups[g].count; k++, r++) {
            if (r == row) return k_tool_groups[g].tools[k];
        }
    }
    return TOOL_COUNT;
}

// Spawnlist rows: All Props, the core pack's categories, NPCs, Ragdolls (the same characters
// spawned limp), then the extra props' categories
static int npc_cat(void) {
    return G.npcs_loaded ? G.props.num_categories + 1 : -1;
}

static int rag_cat(void) {
    return G.npcs_loaded && npc_ragdolls_available() ? G.props.num_categories + 1 + (npc_cat() >= 0) : -1;
}

// A Workshop character that's switched on (Main menu > Workshop)
static int workshop_char_on(int t) {
    return npc_type_workshop(t) && workshop_enabled(WS_CUSTOM, npc_type_name(t));
}

// Workshop: characters from your GMod Workshop addons, as ragdolls (and player models in Options)
static int ws_cat(void) {
    int any = 0;
    for (int t = 0; G.npcs_loaded && t < npc_type_count() && !any; t++) any = workshop_char_on(t);
    return any ? G.props.num_categories + 1 + (npc_cat() >= 0) + (rag_cat() >= 0) : -1;
}

// Weapons: the guns and tools, into your hands (Options > Spawn with weapons off starts you with the toolbox).
// Not in a gamemode's game, even one that keeps the spawn menu: players carry what it gives them.
static int wep_cat(void) {
    return g_gm.on ? -1 : G.props.num_categories + 1 + (npc_cat() >= 0) + (rag_cat() >= 0) + (ws_cat() >= 0);
}

static int extra_row0(void) {
    return G.props.num_categories + 1 + (npc_cat() >= 0) + (rag_cat() >= 0) + (ws_cat() >= 0) + (wep_cat() >= 0);
}

static int num_cats(void) {
    return extra_row0() + propx_num_categories();
}

// The prop category (model_cat's numbering) a row lists; -1 for All Props
// The extra props' spawnlists are named "<game> <folder>" (extras.py). The panel groups them under one
// header per game, opened one at a time, so they aren't a list of 160 to scroll through.
static const struct {
    const char *prefix, *title;
} k_games[] = {
    {"Black Mesa", "Black Mesa"},   {"Episode 1", "HL2 Ep. 1"},     {"Episode 2", "HL2 Ep. 2"},
    {"Lost Coast", "Lost Coast"},   {"Portal 2", "Portal 2"},       {"CS:S", "Counter-Strike"},
    {"DoD:S", "Day of Defeat"},     {"L4D2", "Left 4 Dead 2"},      {"L4D", "Left 4 Dead"},
    {"Portal", "Portal"},           {"FoF", "Fistful (FoF)"},       {"HL2", "Half-Life 2"},
    {"TF2", "TF2"},
};
#define NUM_GAMES (int)(sizeof(k_games) / sizeof(k_games[0]))
#define MAX_XCATS 512
#define ROW_HEADER(k) (-100 - (k)) // panel_rows: the header of the k-th game group
static signed char g_xcat_game[MAX_XCATS]; // per extra spawnlist: its k_games entry (NUM_GAMES: other)
static int g_game_order[NUM_GAMES + 1], g_num_groups; // groups in the pack's order
static int g_groups_ready;

static void group_extras(void) {
    if (g_groups_ready) return;
    g_groups_ready = 1;
    g_num_groups = 0;
    int seen[NUM_GAMES + 1];
    memset(seen, 0, sizeof(seen));
    int nx = propx_num_categories();
    if (nx > MAX_XCATS) nx = MAX_XCATS;
    for (int e = 0; e < nx; e++) {
        const char *name = propx_category(e);
        int g = NUM_GAMES;
        for (int k = 0; k < NUM_GAMES; k++) {
            size_t l = strlen(k_games[k].prefix);
            if (strncmp(name, k_games[k].prefix, l) == 0 && name[l] == ' ') {
                g = k;
                break;
            }
        }
        g_xcat_game[e] = (signed char)g;
        if (!seen[g]) {
            seen[g] = 1;
            g_game_order[g_num_groups++] = g;
        }
    }
}

static const char *group_title(int g) {
    return g < NUM_GAMES ? k_games[g].title : "Other Games";
}

static int row_prop_cat(int row) {
    if (row == 0) return -1;
    if (row <= G.props.num_categories) return row - 1;
    return G.props.num_categories + row - extra_row0();
}

static const char *row_name(int row) {
    if (row == RECENT_CAT) return "Recent";
    if (row == 0) return "All Props";
    if (row == npc_cat()) return "NPCs";
    if (row == rag_cat()) return "Ragdolls";
    if (row == ws_cat()) return "Workshop";
    if (row == wep_cat()) return "Weapons";
    if (row <= G.props.num_categories) return G.props.categories[row - 1];
    return propx_category(row - extra_row0());
}
// A spawnlist's name without its game, under the game's header ("HL2 City" -> "City")
static const char *short_row_name(int row) {
    const char *n = row_name(row);
    int e = row - extra_row0();
    if (row >= extra_row0() && e < MAX_XCATS && g_xcat_game[e] < NUM_GAMES) {
        size_t l = strlen(k_games[(int)g_xcat_game[e]].prefix);
        if (strncmp(n, k_games[(int)g_xcat_game[e]].prefix, l) == 0 && n[l] == ' ') return n + l + 1;
    }
    return n;
}

// The spawnlists panel's rows: Recent (once something's been spawned), All Props and the core lists, NPCs,
// Ragdolls, then a header per game with the open game's lists under it. Entries are spawnlists
// (row_name), RECENT_CAT, or ROW_HEADER(k) for the k-th game group.
#define MAX_PANEL_ROWS (MAX_XCATS + 64)
static int panel_rows(int *out) {
    group_extras();
    int n = 0;
    if (G.recent_n) out[n++] = RECENT_CAT;
    for (int c = 0; c < extra_row0(); c++) out[n++] = c;
    int nx = propx_num_categories();
    if (nx > MAX_XCATS) nx = MAX_XCATS;
    for (int k = 0; k < g_num_groups; k++) {
        int g = g_game_order[k];
        out[n++] = ROW_HEADER(k);
        if (G.group_open != g) continue;
        for (int e = 0; e < nx; e++) {
            if (g_xcat_game[e] == g) out[n++] = extra_row0() + e;
        }
    }
    return n;
}

static void qmenu_test_state(void) {
    if (G.props_loaded) {
        recent_add(prop_named("Wood Crate"));
        recent_add(prop_named("Jeep"));
        for (int c = 0; c < G.props.num_categories; c++) {
            if (strcmp(G.props.categories[c], "Vehicles") == 0) G.cat = c + 1;
        }
    }
    for (int k = 0; k < NUM_GAMES; k++) {
        if (strcmp(k_games[k].prefix, "TF2") == 0) G.group_open = k;
    }
    static int rows[MAX_PANEL_ROWS];
    int nr = panel_rows(rows);
    for (int k = 0; k < nr; k++) {
        if (rows[k] <= ROW_HEADER(0) && g_game_order[-100 - rows[k]] == G.group_open) G.qrow = k;
    }
    if (G.qrow + 2 < nr) {
        G.qrow += 2; // a couple of lists into the game
        G.cat = rows[G.qrow];
    }
}

static void qmenu_test_list(const char *name) {
    static int rows[MAX_PANEL_ROWS];
    int nr = panel_rows(rows);
    for (int k = 0; k < nr; k++) {
        if (rows[k] >= 0 && strcmp(row_name(rows[k]), name) == 0) {
            G.qmenu = 1;
            G.focus = 1;
            G.qrow = k;
            G.cat = rows[k];
            G.grid_sel = 0;
        }
    }
}

static void qmenu_test_pick(int k) {
    static int items[MAX_SPAWNABLE];
    int n = grid_items(items);
    if (k < 0 || k >= n) return;
    G.grid_sel = k;
    spawn_from_menu(items[k]);
    G.qmenu = 0;
}

// Spawned from the menu: to the front of Recent

static void recent_add(int item) {
    int k = 0;
    while (k < G.recent_n && G.recent[k] != item) k++;
    if (k == G.recent_n && G.recent_n < RECENT_MAX) G.recent_n++;
    if (k == RECENT_MAX) k = RECENT_MAX - 1;
    memmove(&G.recent[1], &G.recent[0], sizeof(int) * (size_t)k);
    G.recent[0] = item;
}

// Props you can drive or sit in (a badge on their icon)
static int drivable_model(int model) {
    if (model < 0 || model >= G.props.num_models) return VEH_NONE;
    Body probe;
    memset(&probe, 0, sizeof(probe));
    probe.active = 1;
    probe.model = model;
    return vehicle_kind(&probe);
}

// Grid entries are prop model numbers, GRID_NPC + NPC type, or GRID_RAG + NPC type
static int grid_items(int *out) {
    int n = 0;
    if (G.cat == RECENT_CAT) {
        for (int k = 0; k < G.recent_n; k++) out[n++] = G.recent[k];
        return n;
    }

    if (G.cat == wep_cat()) {
        for (int w = 0; w < WEAPON_COUNT; w++) out[n++] = GRID_WEAPON + w;
        return n;
    }
    if (G.cat == npc_cat() || G.cat == rag_cat() || G.cat == ws_cat()) {
        // Every character can be a ragdoll; the ragdoll-only ones (no animations) can't be NPCs. The
        // Workshop's are only under Workshop, as ragdolls.
        int npcs = G.cat == npc_cat(), ws = G.cat == ws_cat();
        for (int t = 0; t < npc_type_count() && n < MAX_SPAWNABLE; t++) {
            if (npc_type_workshop(t) != ws || (ws && !workshop_char_on(t)) || (npcs && npc_type_ragdoll_only(t))) continue;
            out[n++] = (npcs ? GRID_NPC : GRID_RAG) + t;
        }
        return n;
    }

    int want = row_prop_cat(G.cat);
    for (int k = 0; k < G.num_spawnable && n < MAX_SPAWNABLE; k++) {
        int m = G.spawnable[k];
        if (G.cat == 0 || model_cat(m) == want) out[n++] = m;
    }
    return n;
}

static void qmenu_close(void) {
    G.qmenu = 0;
}

static void open_pause(void) {
    G.paused = 1;
    G.pause_sel = 0;
    G.pause_options = 0;
    G.pause_saves = 0;
    play(G.snd.menu_select, 0.5f);
}

static void qmenu_update(const InputState *in) {
    if (in->pressed & (PSP_CTRL_SELECT | PSP_CTRL_CIRCLE)) {
        // Letting go of that Select mustn't open it straight back up (nor hold on into noclip)
        if (in->pressed & PSP_CTRL_SELECT) G.select_fired = 1;
        qmenu_close();
        return;
    }
    if (in->pressed & PSP_CTRL_START) {
        qmenu_close();
        open_pause();
        return;
    }
    int ncats = num_cats();
    static int items[MAX_SPAWNABLE];
    int n = grid_items(items);
    int before = G.focus * 1000 + G.cat * 100 + G.grid_sel + G.tool_row * 10000 + G.colour_sel * 100000 +
                 G.qrow * 7 + G.group_open * 13;

    if (in->pressed & PSP_CTRL_RTRIGGER) G.focus = (G.focus + 1) % 3;
    if (in->pressed & PSP_CTRL_LTRIGGER) G.focus = (G.focus + 2) % 3;

    (void)ncats;
    if (G.focus == 0) {
        static int rows[MAX_PANEL_ROWS];
        int nr = panel_rows(rows);
        if (G.qrow >= nr) G.qrow = nr - 1;
        if (G.qrow < 0) G.qrow = 0;
        // Rows move about (Recent appears, groups open): keep the cursor on the list being shown
        if (rows[G.qrow] > ROW_HEADER(0) && rows[G.qrow] != G.cat) {
            for (int k = 0; k < nr; k++) {
                if (rows[k] == G.cat) G.qrow = k;
            }
        }
        if (in->pressed & PSP_CTRL_UP) G.qrow = (G.qrow + nr - 1) % nr;
        if (in->pressed & PSP_CTRL_DOWN) G.qrow = (G.qrow + 1) % nr;
        int r = rows[G.qrow];
        if (r <= ROW_HEADER(0)) {
            // A game's header: X or Right opens its spawnlists (and closes the open one), again closes it
            int g = g_game_order[-100 - r];
            if (in->pressed & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS)) G.group_open = G.group_open == g ? -1 : g;
            if ((in->pressed & PSP_CTRL_LEFT) && G.group_open == g) G.group_open = -1;
        } else {
            if (r != G.cat) {
                G.cat = r;
                G.grid_sel = 0;
                G.grid_scroll = 0;
            }
            if ((in->pressed & PSP_CTRL_LEFT) && r >= extra_row0() && G.group_open >= 0) {
                // Left inside a game's lists: close it, back on its header
                int g = G.group_open;
                G.group_open = -1;
                nr = panel_rows(rows);
                for (int k = 0; k < nr; k++) {
                    if (rows[k] <= ROW_HEADER(0) && g_game_order[-100 - rows[k]] == g) G.qrow = k;
                }
            } else if (in->pressed & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS)) {
                G.focus = 1;
            }
        }
    } else if (G.focus == 1) {
        int col = G.grid_sel % GRID_COLS;
        if (in->pressed & PSP_CTRL_LEFT) {
            if (col == 0) {
                G.focus = 0;
            } else {
                G.grid_sel--;
            }
        } else if (in->pressed & PSP_CTRL_RIGHT) {
            if (col == GRID_COLS - 1 || G.grid_sel + 1 >= n) {
                G.focus = 2;
            } else {
                G.grid_sel++;
            }
        }
        if ((in->pressed & PSP_CTRL_DOWN) && G.grid_sel + GRID_COLS < n) G.grid_sel += GRID_COLS;
        if ((in->pressed & PSP_CTRL_UP) && G.grid_sel >= GRID_COLS) G.grid_sel -= GRID_COLS;
        int row = G.grid_sel / GRID_COLS;
        if (row < G.grid_scroll) G.grid_scroll = row;
        if (row >= G.grid_scroll + GRID_ROWS) G.grid_scroll = row - GRID_ROWS + 1;
        if ((in->pressed & PSP_CTRL_CROSS) && n > 0) {
            play(G.snd.menu_select, 0.6f);
            spawn_from_menu(items[G.grid_sel < n ? G.grid_sel : 0]);
            qmenu_close();
            return;
        }
    } else {
        if (in->pressed & PSP_CTRL_UP) G.tool_row = (G.tool_row + NUM_TOOL_ROWS - 1) % NUM_TOOL_ROWS;
        if (in->pressed & PSP_CTRL_DOWN) G.tool_row = (G.tool_row + 1) % NUM_TOOL_ROWS;
        if (G.tool_row == TOOL_COUNT) {
            // Colour swatches
            if (in->pressed & PSP_CTRL_RIGHT) G.colour_sel = (G.colour_sel + 1) % PALETTE_SIZE;
            if (in->pressed & PSP_CTRL_LEFT) G.colour_sel = (G.colour_sel + PALETTE_SIZE - 1) % PALETTE_SIZE;
        } else if (in->pressed & PSP_CTRL_LEFT) {
            G.focus = 1;
        }
        if ((in->pressed & PSP_CTRL_CROSS)) {
            Tool t = G.tool_row == TOOL_COUNT ? TOOL_COLOUR : tool_at_row(G.tool_row);
            play(G.snd.menu_select, 0.6f);
            G.tool = t;
            G.stage = 0;
            if (G.weapon != WEAPON_TOOLGUN) G.switch_time = 0.25f;
            G.weapon = WEAPON_TOOLGUN;
            G.wsel_time = 1.5f;
            physgun_release(1);
            qmenu_close();
            return;
        }
    }
    int after = G.focus * 1000 + G.cat * 100 + G.grid_sel + G.tool_row * 10000 + G.colour_sel * 100000 +
                G.qrow * 7 + G.group_open * 13;

    if (after != before) play(G.snd.menu_move, 0.4f);
}

static void freeze_all(void) {
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &G.phys.bodies[i];
        if (b->active && b->lib == 0) {
            b->frozen = 1;
            b->vel = b->angvel = v3(0, 0, 0);
        }
    }
    physgun_release(1);
    notify("Froze all props", NOTE_HINT);
}

static void cleanup_props(void) {
    for (int i = 0; i < MAX_BODIES; i++) {
        if (G.phys.bodies[i].active && G.phys.bodies[i].lib == 0) remove_body(i);
    }
    npcs_clear();
    blood_clear();
    release_unused_extras();
    G.undo_count = 0;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) G.peers[s].undo_count = 0;
    notify("Cleaned up all props", NOTE_CLEANUP);
}

// --- Saves: builds written to the memory stick and loaded back on any map (builds.c) ----------------

static Build g_build;

static const char *model_cat_name(int model) {
    int c = model_cat(model);
    if (c < G.props.num_categories) return c >= 0 ? G.props.categories[c] : "";
    return propx_category(c - G.props.num_categories);
}

// A saved prop's model: by its name in its category, or by its name alone (another pack's categories)
static int model_by_name(const char *name, const char *cat) {
    int any = -1;
    for (int m = 0; m < G.props.num_models; m++) {
        if (strncmp(G.props.models[m].name, name, sizeof(G.props.models[m].name)) != 0) continue;
        if (strcmp(model_cat_name(m), cat) == 0) return m;
        if (any < 0) any = m;
    }
    for (int x = 0; x < propx_count(); x++) {
        if (strcmp(propx_name(x), name) != 0) continue;
        int m = G.props.num_models + x;
        if (strcmp(model_cat_name(m), cat) == 0) return m;
        if (any < 0) any = m;
    }
    return any;
}

// Everything players spawned (not the map's own props or broken pieces), from the middle of its
// floor and turned so the way you face is the build's forward. Returns how many props.
static int build_capture(Build *bd) {
    memset(bd, 0, sizeof(*bd));
    snprintf(bd->map, sizeof(bd->map), "%s", G.map_name);
    int idx[MAX_BODIES];
    vec3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (int i = 0; i < MAX_BODIES; i++) {
        const Body *b = &G.phys.bodies[i];
        idx[i] = -1;
        if (!b->active || b->lib != 0 || b->life > 0.0f || bd->num_props >= BUILD_MAX_PROPS) continue;
        idx[i] = bd->num_props++;
        vec3 mn, mx;
        phys_body_aabb(b, &mn, &mx);
        lo = v3(fminf(lo.x, mn.x), fminf(lo.y, mn.y), fminf(lo.z, mn.z));
        hi = v3(fmaxf(hi.x, mx.x), fmaxf(hi.y, mx.y), fmaxf(hi.z, mx.z));
    }
    if (bd->num_props == 0) return 0;
    bd->origin = v3((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z);
    bd->yaw = G.yaw;
    quat inv = yaw_quat(-G.yaw);
    for (int i = 0; i < MAX_BODIES; i++) {
        if (idx[i] < 0) continue;
        const Body *b = &G.phys.bodies[i];
        BuildProp *p = &bd->props[idx[i]];
        snprintf(p->name, sizeof(p->name), "%s", model_name(b->model));
        snprintf(p->cat, sizeof(p->cat), "%s", model_cat_name(b->model));
        p->pos = q_rotate(inv, v3_sub(b->pos, bd->origin));
        p->rot = q_norm(q_mul(inv, b->rot));
        p->tint = b->tint_abgr;
        p->mass = b->mass;
        p->flags = (b->frozen ? BUILD_FROZEN : 0) | (b->nocollide ? BUILD_NOCOLLIDE : 0);
        p->balloons = b->balloons;
    }
    for (int j = 0; j < MAX_JOINTS && bd->num_joints < BUILD_MAX_JOINTS; j++) {
        const Joint *jt = &G.phys.joints[j];
        if (!jt->active || idx[jt->a] < 0 || (jt->b >= 0 && idx[jt->b] < 0)) continue;
        Joint *o = &bd->joints[bd->num_joints++];
        *o = *jt;
        o->a = idx[jt->a];
        o->b = jt->b >= 0 ? idx[jt->b] : -1;
        if (jt->b < 0) {
            // Held to the world: its anchor, angle and line are world ones, so they move with the build
            o->lb = q_rotate(inv, v3_sub(jt->lb, bd->origin));
            o->rel = q_mul(inv, jt->rel);
            o->axis = q_rotate(inv, jt->axis);
        }
    }
    for (int f = 0; f < MAX_FORCES && bd->num_forces < BUILD_MAX_FORCES; f++) {
        const Force *fc = &G.phys.forces[f];
        if (!fc->active || idx[fc->body] < 0) continue;
        Force *o = &bd->forces[bd->num_forces++];
        *o = *fc;
        o->body = idx[fc->body];
        if (fc->type == FORCE_HOVER) o->strength -= bd->origin.z; // the height it holds, from the floor
    }
    for (int d = 0; d < MAX_DEVICES && bd->num_devices < BUILD_MAX_DEVICES; d++) {
        const Device *v = &g_dev[d];
        if (!v->type || idx[v->body] < 0) continue;
        BuildDevice *o = &bd->devices[bd->num_devices++];
        o->type = v->type;
        o->body = idx[v->body];
        o->on = v->on;
        o->lpos = v->lpos;
        o->ldir = v->ldir;
        o->colour = v->colour;
        o->effect = v->effect;
        o->target = body_valid(v->target) ? idx[v->target] : -1;
    }
    return bd->num_props;
}

// Spawns a build with its floor's middle at origin, its forward turned to yaw. Returns how many props
// it made (0: nothing, with msg saying why).
static int build_place(const Build *bd, vec3 origin, float yaw, char *msg, int msg_len) {
    int free_bodies = 0;
    for (int i = 0; i < MAX_BODIES; i++) free_bodies += !G.phys.bodies[i].active;
    int room = PLAYER_PROP_LIMIT - player_prop_count();
    if (bd->num_props > room || bd->num_props > free_bodies) {
        snprintf(msg, msg_len, "It has %d props: room for %d", bd->num_props, room < free_bodies ? room : free_bodies);
        return 0;
    }
    quat r = yaw_quat(yaw);
    int body[BUILD_MAX_PROPS], made = 0, missing = 0;
    for (int k = 0; k < bd->num_props; k++) {
        const BuildProp *p = &bd->props[k];
        int model = model_by_name(p->name, p->cat);
        body[k] = model >= 0 ? new_body(model, v3_add(origin, q_rotate(r, p->pos)), q_norm(q_mul(r, p->rot)), p->tint, NULL)
                             : -1;
        if (body[k] < 0) {
            missing++;
            continue;
        }
        Body *b = &G.phys.bodies[body[k]];
        if (fabsf(p->mass - b->mass) > 0.5f) phys_set_mass(&G.phys, body[k], p->mass);
        b->frozen = (p->flags & BUILD_FROZEN) != 0;
        b->nocollide = (p->flags & BUILD_NOCOLLIDE) != 0;
        b->balloons = p->balloons < MAX_BALLOONS ? p->balloons : MAX_BALLOONS;
        update_balloon_lift(b);
        undo_push(body[k]);
        made++;
    }
    for (int k = 0; k < bd->num_joints; k++) {
        const Joint *src = &bd->joints[k];
        if (body[src->a] < 0 || (src->b >= 0 && body[src->b] < 0)) continue;
        int j;
        for (j = 0; j < MAX_JOINTS && G.phys.joints[j].active; j++) {
        }
        if (j == MAX_JOINTS) break;
        Joint *jt = &G.phys.joints[j];
        *jt = *src;
        jt->a = body[src->a];
        jt->b = src->b >= 0 ? body[src->b] : -1;
        if (src->b < 0) {
            jt->lb = v3_add(origin, q_rotate(r, src->lb));
            jt->rel = q_mul(r, src->rel);
            jt->axis = q_rotate(r, src->axis);
        }
    }
    for (int k = 0; k < bd->num_forces; k++) {
        const Force *src = &bd->forces[k];
        if (body[src->body] < 0) continue;
        int f;
        for (f = 0; f < MAX_FORCES && G.phys.forces[f].active; f++) {
        }
        if (f == MAX_FORCES) break;
        Force *fc = &G.phys.forces[f];
        *fc = *src;
        fc->body = body[src->body];
        if (fc->type == FORCE_HOVER) fc->strength += origin.z;
    }
    for (int k = 0; k < bd->num_devices; k++) {
        const BuildDevice *src = &bd->devices[k];
        if (body[src->body] < 0) continue;
        int d;
        for (d = 0; d < MAX_DEVICES && g_dev[d].type; d++) {
        }
        if (d == MAX_DEVICES) break;
        Device *v = &g_dev[d];
        memset(v, 0, sizeof(*v));
        v->type = src->type;
        v->body = body[src->body];
        v->on = src->on;
        v->lpos = src->lpos;
        v->ldir = src->ldir;
        v->colour = src->colour >= 0 && src->colour < PALETTE_SIZE ? src->colour : 0;
        v->effect = src->effect % EMIT_COUNT;
        v->target = src->target >= 0 && src->target < bd->num_props ? body[src->target] : -1;
    }
    for (int k = 0; k < bd->num_props; k++) {
        if (body[k] >= 0) phys_wake(&G.phys, body[k]);
    }
    // One Undo takes the whole build away
    if (made > 1) undo_push(UNDO_GROUP_BASE + made);
    if (missing) {
        snprintf(msg, msg_len, "Loaded (%d prop%s not in this prop pack)", missing, missing == 1 ? "" : "s");
    } else {
        snprintf(msg, msg_len, "Loaded %d prop%s", made, made == 1 ? "" : "s");
    }
    return made;
}

// Where a build goes when loaded "where you look": on the floor there, far enough out not to land on you
static vec3 build_drop_point(const Build *bd) {
    float radius = 0.0f;
    for (int k = 0; k < bd->num_props; k++) {
        float d = sqrtf(bd->props[k].pos.x * bd->props[k].pos.x + bd->props[k].pos.y * bd->props[k].pos.y) + 24.0f;
        if (d > radius) radius = d;
    }
    Aim a = aim_trace(radius + 600.0f); // not off at the horizon
    vec3 at = a.point;
    vec3 f = view_forward();
    f.z = 0.0f;
    f = v3_len(f) > 1e-3f ? v3_norm(f) : v3(1, 0, 0);
    vec3 from = G.player.origin;
    float out;
    if (!(a.hit_world && a.normal.z > 0.7f)) at = v3_mad(at, f, -radius); // a wall: come back off it
    out = v3_dot(v3_sub(at, from), f);
    if (out < radius + 40.0f) at = v3_mad(at, f, radius + 40.0f - out);
    // Down onto the floor
    Trace tr;
    vec3 above = v3_add(at, v3(0, 0, 48.0f));
    cm_trace_line(&G.map, above, v3_add(above, v3(0, 0, -2048.0f)), MASK_PLAYERSOLID, NULL, 0, &tr);
    return v3_add(tr.endpos, v3(0, 0, 2.0f));
}

static void save_build(const char *name) {
    char msg[48];
    if (build_capture(&g_build) == 0) {
        notify("Nothing to save: spawn some props first", NOTE_ERROR);
        return;
    }
    if (build_write(name, &g_build) != 0) {
        notify("Couldn't save (is the memory stick full?)", NOTE_ERROR);
        return;
    }
    snprintf(msg, sizeof(msg), "Saved %s (%d props)", name, g_build.num_props);
    notify(msg, NOTE_HINT);
    play(G.snd.weld, 0.6f);
    achieve_unlock(ACH_SAVE_POINT);
}

static int load_build(const char *name, int in_place) {
    char msg[48];
    if (build_read(name, &g_build) != 0) {
        notify("Couldn't read that save", NOTE_ERROR);
        return 0;
    }
    vec3 at = in_place ? g_build.origin : build_drop_point(&g_build);
    float yaw = in_place ? g_build.yaw : G.yaw;
    int made = build_place(&g_build, at, yaw, msg, sizeof(msg));
    notify(msg, made ? NOTE_HINT : NOTE_ERROR);
    if (made) play(G.snd.spawn, 0.6f);
    if (made && strcmp(g_build.map, G.map_name) != 0) achieve_unlock(ACH_DEJA_VU);
    return made;
}

static void saves_page(const InputState *in) {
    char name[BUILD_NAME_LEN];
    int r = saves_update(in, name, sizeof(name));
    if (r == SAVES_BACK) {
        G.pause_saves = 0;
    } else if (r == SAVES_SAVE) {
        save_build(name);
        saves_refresh();
    } else if (r == SAVES_LOAD_AIM || r == SAVES_LOAD_IN_PLACE) {
        if (load_build(name, r == SAVES_LOAD_IN_PLACE)) {
            G.pause_saves = 0;
            G.paused = 0; // back to the game, to see it
        }
    }
}

// The pause menu's items (k_pause_items indices): a gamemode's game leaves out saves, noclip and cleanup
static int pause_items(int *out) {
    int n = 0;
    for (int i = 0; i < NUM_PAUSE_ITEMS; i++) {
        if (gm_sandbox() || i == 0 || i == 1 || i == 3 || i == NUM_PAUSE_ITEMS - 1) out[n++] = i;
    }
    return n;
}

// GMod's escape menu: Resume, Options, cleanup, Disconnect
static GameResult pause_update(const InputState *in) {
    if (g_shot_pending) return GAME_CONTINUE; // the screenshot's frame is on its way
    if (G.pause_options) {
        if (options_update(in)) G.pause_options = 0;
        if (!g_settings.sound && G.hold_sound >= 0) {
            snd_stop(G.hold_sound);
            G.hold_sound = -1;
        }
        return GAME_CONTINUE;
    }
    if (G.pause_saves) {
        saves_page(in);
        return GAME_CONTINUE;
    }
    int items[NUM_PAUSE_ITEMS], n = pause_items(items);
    if (G.pause_sel >= n) G.pause_sel = 0;
    int before = G.pause_sel;
    if (in->pressed & PSP_CTRL_UP) G.pause_sel = (G.pause_sel + n - 1) % n;
    if (in->pressed & PSP_CTRL_DOWN) G.pause_sel = (G.pause_sel + 1) % n;
    if (G.pause_sel != before) play(G.snd.menu_move, 0.4f);
    if (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
        G.paused = 0;
        return GAME_CONTINUE;
    }
    if (!(in->pressed & PSP_CTRL_CROSS)) return GAME_CONTINUE;
    play(G.snd.menu_select, 0.6f);
    switch (items[G.pause_sel]) {
    case 0: G.paused = 0; break;
    case 1: G.pause_options = 1; break;
    case 2:
        if (is_client()) {
            // The host has the whole world (welds, thrusters...): builds are saved and loaded there
            notify("Only the host can save and load builds", NOTE_ERROR);
            G.paused = 0;
        } else {
            G.pause_saves = 1;
            saves_open(G.map_name);
        }
        break;
    case 3:
        // Stays paused (the world holds still) while the next frame is drawn without this menu
        if (g_shot) {
            g_shot_pending = 1;
        } else {
            G.paused = 0;
        }
        break;
    case 4:
        G.player.noclip = !G.player.noclip;
        G.player.velocity = v3(0, 0, 0);
        G.paused = 0;
        notify(G.player.noclip ? "Noclip enabled" : "Noclip disabled", NOTE_HINT);
        break;
    case 5:
    case 6:
        if (is_client()) {
            notify("Only the host can do that", NOTE_ERROR);
        } else if (items[G.pause_sel] == 5) {
            freeze_all();
        } else {
            cleanup_props();
        }
        G.paused = 0;
        break;
    case 7:
        G.paused = 0;
        G.qmenu = 0;
        net_leave();
        return GAME_EXIT_TO_MENU;
    default:
        break;
    }
    if (!G.paused) G.qmenu = 0;
    return GAME_CONTINUE;
}

// --- Per-frame update ----------------------------------------------------------------------------

static float look_rate(int i, int held, float dt) {
    G.look_hold[i] = held ? G.look_hold[i] + dt : 0.0f;
    if (!held) return 0.0f;
    float k = LOOK_MIN_FRACTION + (1.0f - LOOK_MIN_FRACTION) * clampf(G.look_hold[i] / LOOK_RAMP_TIME, 0.0f, 1.0f);
    return k * (0.4f + 0.2f * g_settings.look_speed);
}

static void player_sounds(float dt) {
    Player *p = &G.player;
    float hs = sqrtf(p->velocity.x * p->velocity.x + p->velocity.y * p->velocity.y);
    if (!p->noclip && p->on_ground) {
        G.step_dist += hs * dt;
        if (G.step_dist > STEP_STRIDE) {
            G.step_dist = 0.0f;
            play(p->water_level >= 1 ? G.snd.step_water : G.snd.step_concrete, p->ducked ? 0.15f : 0.35f);
        }
        G.bob_phase += hs * dt * 0.035f;
    }
    if (!p->noclip && p->on_ground && !G.was_on_ground && G.fall_speed > 300.0f) {
        play(G.snd.land, clampf(G.fall_speed / 900.0f, 0.3f, 1.0f));
    }
    if (p->water_level >= 2 && G.last_water < 2 && G.fall_speed > 150.0f) play(G.snd.splash, 0.7f);
    G.fall_speed = p->velocity.z < 0.0f ? -p->velocity.z : 0.0f;
    G.was_on_ground = p->on_ground;
    G.last_water = p->water_level;
}

static void impact_sounds(float dt) {
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &G.phys.bodies[i];
        G.impact_cooldown[i] -= dt;
        if (!b->active) continue;
        // Hard enough to shatter a breakable (bottles, the watermelon, pots)? The host decides.
        if (b->impact > BREAK_SPEED && !is_client() && break_group(b)) {
            break_body(i);
            continue;
        }
        if (b->impact > IMPACT_MIN_SPEED && G.impact_cooldown[i] <= 0.0f) {
            int s = b->surface >= 0 && b->surface < NUM_SURF ? b->surface : 0;
            play_at(G.snd.impact[s], b->pos, clampf((b->impact - IMPACT_MIN_SPEED) / 500.0f + 0.2f, 0.2f, 1.0f));
            G.impact_cooldown[i] = 0.15f;
        }
        b->impact = 0.0f;
    }
}

static void damage_player(float amount) {
    if (G.dead_time > 0.0f || G.player.noclip || god_mode()) return;
    // The HEV suit takes 80% of the hit out of its power while it has some (HL2's armour)
    if (g_settings.hev_suit && G.suit > 0.0f) {
        float soaked = fminf(amount * 0.8f, G.suit);
        G.suit -= soaked;
        amount -= soaked;
    }
    if (is_client()) {
        // The host keeps everyone's health; it comes back in the next snapshot
        float f[1] = {amount};
        send_cmd(CMD_DAMAGE, 0, 0, 0, f, 1);
        return;
    }
    G.health -= amount;
    G.hurt_flash = 0.4f;
    play(G.snd.impact[0], 0.8f);
    if (G.health <= 0.0f) {
        G.health = 0.0f;
        G.dead_time = 0.001f;
        physgun_release(1);
        G.qmenu = 0;
    }
}

static void respawn(void) {
    const PmapSpawn *sp = &G.map.spawns[0];
    player_spawn(&G.player, v3(sp->x, sp->y, sp->z));
    G.yaw = sp->yaw;
    G.pitch = 0.0f;
    G.health = 100.0f;
    G.suit = 100.0f;
    G.dead_time = 0.0f;
    refill_ammo();
    sandbox_loadout();
    if (is_client()) {
        send_cmd(CMD_RESPAWN, 0, 0, 0, NULL, 0);
        G.respawn_cmd = G.next_cmd;
        G.respawn_pending = 1;
    }
}

// Hard landings hurt, like HL2: nothing below about 580 units/s
static void fall_damage(void) {
    if (!G.player.noclip && G.player.on_ground && !G.was_on_ground && G.fall_speed > 580.0f) {
        g_gm.p[G.my_slot].attacker = -1;
        damage_player((G.fall_speed - 580.0f) * 0.25f);
    }
}

// --- Multiplayer ----------------------------------------------------------------------------------
// Host-authoritative over ad-hoc wireless. The host runs physics, NPCs and doors and broadcasts
// snapshots; clients mirror them, send their own position and physics gun target, and ask the
// host to do everything else (spawn, remove, weld...) with numbered commands that are resent
// until a snapshot acknowledges them.

#define SNAP_INTERVAL 0.05f
#define ENTS_INTERVAL 0.1f
#define ROPES_INTERVAL 0.5f
#define BEACON_INTERVAL 0.5f
#define SEND_INTERVAL 0.05f
#define PEER_TIMEOUT 20.0f // a joining PSP is quiet while it loads the map
#define HOST_TIMEOUT 8.0f
#define BODIES_PER_PACKET 36
#define MAX_BODY_PACKETS 3
#define BODY_ROUND_ROBIN 12
#define MAX_NET_ROPES 40
#define ENTS_PER_PACKET 300

#define NET_NO_MODEL 255 // NetPlayer / PkCState .model: the default character
typedef struct {
    uint8_t active, weapon, flags, model; // model: NPC type of the player model
    float pos[3];
    float yaw, pitch, health;
    int16_t held;
    uint16_t ack; // last command of this player's the host has run
    int16_t veh;  // the vehicle or seat they're in, or -1
    uint8_t colour, pgun; // Profile colours: player and physics gun (profile_colour indices)
    uint8_t gesture, gesture_seq; // quick chat: the last gesture (index + 1), counting up for each new one
    char name[16];
} NetPlayer;

typedef struct {
    NetHeader h;
    uint32_t tick;
    NetPlayer players[NET_MAX_PLAYERS];
    NpcNet npcs[MAX_NPCS];
    uint8_t rag_mask;               // the ragdoll slots in use
    uint8_t rag_gen[MAX_RAGDOLLS];  // and which ragdoll each holds (npc_ragdoll_gen)
} PkPlayers;

typedef struct {
    NetHeader h;
    RagInfo i;
} PkRagInfo;

typedef struct {
    NetHeader h;
    RagPose p; // only p.nb bones are sent
} PkRagPose;
_Static_assert(sizeof(PkPlayers) <= 1400 && sizeof(PkRagInfo) <= 1400, "packets must fit one datagram");

// A player model's tint for a Profile colour: halfway to it, so the character still looks like itself
static unsigned int body_tint(int colour) {
    unsigned int c = profile_colour(colour, 0), out = 0xFF000000u;
    for (int k = 0; k < 24; k += 8) out |= ((255u + ((c >> k) & 0xFFu)) / 2u) << k;
    return out;
}

static uint8_t model_byte(int type) {
    return type >= 0 && type < NET_NO_MODEL ? (uint8_t)type : NET_NO_MODEL;
}

typedef struct {
    NetHeader h;
    float pos[3];
    float yaw, pitch;
    uint8_t weapon, flags, ncmds, model;
    int16_t held;
    int8_t held_rag;   // the ragdoll its physics gun holds, or -1...
    uint8_t held_bone; // ...by this bone (hold_pos is where)
    float hold_pos[3];
    float hold_rot[4];
    int16_t veh;           // the vehicle or seat we're in, or -1...
    int8_t throttle, steer; // ...and how we drive it (x 127)
    uint8_t veh_brake, colour, pgun, vpad; // colour, pgun: Profile colours
    NetCmd cmds[MAX_OUTQ];
} PkCState;

typedef struct {
    NetHeader h;
    uint8_t speaker, seq, pad[2]; // speaker: their slot (the host fills it in when relaying)
    VoiceFrame frame;
} PkVoice;

typedef struct {
    uint8_t index, lib;
    uint16_t model;
    float pos[3];
    int16_t rot[4];
    uint32_t tint;
    uint8_t balloons, flags, pad[2];
} NetBody;

typedef struct {
    NetHeader h;
    uint32_t tick;
    uint8_t active[(MAX_BODIES + 7) / 8]; // which bodies exist: the rest get removed
    uint8_t count, pad[3];
    NetBody b[BODIES_PER_PACKET];
} PkBodies;

typedef struct {
    NetHeader h;
    uint16_t first, count;
    uint8_t data[ENTS_PER_PACKET * 4];
} PkEnts;

// Lamps, lights, buttons and emitters, for clients to draw (and know which props they can Use)
typedef struct {
    uint8_t type, body, on, effect, colour, target, pad[2]; // target: NET_WORLD for none
    float lpos[3], ldir[3];
} NetDevice;

typedef struct {
    NetHeader h;
    uint8_t count, pad[3];
    NetDevice d[MAX_DEVICES];
} PkDevices;

// Ropes, elastics and sliders, for clients to draw
#define NET_WORLD 255 // NetRope.b: tied to the world
typedef struct {
    uint8_t a, b, type, pad; // bodies (MAX_BODIES fits a byte), JOINT_*
    float la[3], lb[3];
    float length;
} NetRope;

typedef struct {
    NetHeader h;
    uint8_t count, pad[3];
    NetRope r[MAX_NET_ROPES];
} PkRopes;

static NetHeader net_header(int type) {
    NetHeader h = {NET_MAGIC, (uint8_t)type, (uint8_t)G.my_slot, NET_VERSION};
    return h;
}

static int seq_newer(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

static void map_base(char *out, int len) {
    snprintf(out, len, "%s", G.map_name);
    char *dot = strstr(out, ".low");
    if (dot) *dot = 0;
}

static void send_cmd(int kind, int i0, int i1, uint32_t u, const float *f, int nf) {
    if (!is_client()) return;
    if (G.outq_n >= MAX_OUTQ) {
        notify("Network busy - try again", NOTE_ERROR);
        return;
    }
    NetCmd *c = &G.outq[G.outq_n++];
    memset(c, 0, sizeof(*c));
    c->id = ++G.next_cmd;
    c->kind = (uint8_t)kind;
    c->i0 = i0;
    c->i1 = i1;
    c->u = u;
    for (int k = 0; k < nf && k < 10; k++) c->f[k] = f[k];
}

static int undo_pop(int slot) {
    int *list, *count;
    undo_list(slot, &list, &count);
    while (*count > 0) {
        int i = list[--(*count)];
        if (i >= UNDO_GROUP_BASE) {
            int n = i - UNDO_GROUP_BASE, removed = 0;
            for (; n > 0 && *count > 0; n--) {
                int k = list[--(*count)];
                if (k >= 0 && k < MAX_BODIES && G.phys.bodies[k].active && G.phys.bodies[k].lib == 0 &&
                    G.body_owner[k] == slot) {
                    remove_body(k);
                    removed++;
                }
            }
            if (!removed) continue;
            release_unused_extras();
            return 4;
        }
        if (i >= RAGDOLL_ID_BASE) {
            if (!npc_ragdoll_active(i - RAGDOLL_ID_BASE)) continue;
            npc_ragdoll_remove(i - RAGDOLL_ID_BASE);
            return 3;
        }
        if (i >= NPC_ID_BASE) {
            if (!npc_active(i - NPC_ID_BASE)) continue;
            npc_remove(i - NPC_ID_BASE);
            return 2;
        }
        const Body *b = &G.phys.bodies[i];
        if (b->active && b->lib == 0 && G.body_owner[i] == slot) {
            remove_body(i);
            return 1;
        }
    }
    return 0;
}

static int body_valid(int i) {
    return i >= 0 && i < MAX_BODIES && G.phys.bodies[i].active;
}

// The sandbox's building commands, which a gamemode's game turns away
static int sandbox_cmd(int kind) {
    switch (kind) {
    case CMD_SPAWN: case CMD_SPAWN_NPC: case CMD_REMOVE: case CMD_REMOVE_NPC: case CMD_COLOUR: case CMD_BALLOON:
    case CMD_WELD: case CMD_ROPE: case CMD_FREEZE: case CMD_BALLSOCKET: case CMD_ELASTIC: case CMD_NOCOLLIDE:
    case CMD_WEIGHT: case CMD_THRUSTER: case CMD_HOVER: case CMD_EXPLODE: case CMD_SLIDER: case CMD_RAG_SPAWN:
    case CMD_RAG_REMOVE: case CMD_RAG_FREEZE: case CMD_RAG_INFLATE: case CMD_TOOL: case CMD_RAG_POSE:
        return 1;
    default:
        return 0;
    }
}

// Host: runs a client's action as that player
static void exec_cmd(int slot, const NetCmd *c) {
    Peer *p = &G.peers[slot];
    int i = c->i0;
    Body *b = body_valid(i) ? &G.phys.bodies[i] : NULL;
    if (!gm_sandbox() && sandbox_cmd(c->kind)) return;
    G.acting = slot;
    switch (c->kind) {
    case CMD_SPAWN:
        spawn_prop(i, v3(c->f[0], c->f[1], c->f[2]), v3(c->f[3], c->f[4], c->f[5]),
                   q_norm((quat){c->f[6], c->f[7], c->f[8], c->f[9]}), c->u);
        break;
    case CMD_SPAWN_NPC: {
        int n = npc_spawn(i, v3(c->f[0], c->f[1], c->f[2]), c->f[3]);
        if (n >= 0) undo_push(NPC_ID_BASE + n);
        break;
    }
    case CMD_REMOVE:
        if (b) {
            play_at(G.snd.remove, b->pos, 0.6f);
            remove_body(i);
        }
        break;
    case CMD_REMOVE_NPC:
        npc_remove(i);
        break;
    case CMD_COLOUR:
        if (b) {
            b->tint_abgr = c->u;
            G.body_dirty[i] = 1;
        }
        break;
    case CMD_BALLOON:
        if (b && b->balloons < MAX_BALLOONS) {
            b->balloons++;
            update_balloon_lift(b);
            b->frozen = 0;
            phys_wake(&G.phys, i);
            G.body_dirty[i] = 1;
        }
        break;
    case CMD_WELD:
        if (b && body_valid(c->i1) && c->i1 != i) phys_weld(&G.phys, i, c->i1, v3(c->f[0], c->f[1], c->f[2]));
        break;
    case CMD_ROPE: {
        int a = b ? i : -1, o = body_valid(c->i1) ? c->i1 : -1;
        if (a >= 0 || o >= 0) {
            phys_rope(&G.phys, a, v3(c->f[0], c->f[1], c->f[2]), o, v3(c->f[3], c->f[4], c->f[5]), c->f[6]);
        }
        break;
    }
    case CMD_FREEZE:
        if (b) {
            b->frozen = 1;
            b->held = 0;
            b->vel = b->angvel = v3(0, 0, 0);
            G.body_dirty[i] = 1;
            if (p->held == i) p->held = -1;
            p->held_prev = -1;
        }
        break;
    case CMD_PUNT:
        if (b) {
            if (p->held == i) p->held = -1;
            if (p->held_prev == i) p->held_prev = -1;
            b->frozen = 0;
            b->held = 0;
            phys_wake(&G.phys, i);
            b->vel = v3(c->f[0], c->f[1], c->f[2]);
            G.body_dirty[i] = 1;
        }
        break;
    case CMD_BALLSOCKET:
        if (b && (c->i1 < 0 || body_valid(c->i1)) && c->i1 != i) phys_ballsocket(&G.phys, i, c->i1, v3(c->f[0], c->f[1], c->f[2]));
        break;
    case CMD_ELASTIC: {
        int a2 = body_valid(i) ? i : -1, o = body_valid(c->i1) ? c->i1 : -1;
        if (a2 >= 0 || o >= 0)
            phys_elastic(&G.phys, a2, v3(c->f[0], c->f[1], c->f[2]), o, v3(c->f[3], c->f[4], c->f[5]), c->f[6]);
        break;
    }
    case CMD_SLIDER: {
        int a2 = body_valid(i) ? i : -1, o = body_valid(c->i1) ? c->i1 : -1;
        if ((a2 >= 0 || o >= 0) && a2 != o)
            phys_slider(&G.phys, a2, v3(c->f[0], c->f[1], c->f[2]), o, v3(c->f[3], c->f[4], c->f[5]));
        break;
    }
    case CMD_NOCOLLIDE:
        if (b) {
            b->nocollide = !b->nocollide;
            phys_wake(&G.phys, i);
        }
        break;
    case CMD_WEIGHT:
        if (b) phys_set_mass(&G.phys, i, clampf(c->f[0], 1.0f, 1000.0f));
        break;
    case CMD_THRUSTER:
        if (b) phys_add_force(&G.phys, FORCE_THRUSTER, i, v3(c->f[0], c->f[1], c->f[2]), v3_norm(v3(c->f[3], c->f[4], c->f[5])), THRUSTER_FORCE);
        break;
    case CMD_HOVER:
        if (b) phys_add_force(&G.phys, FORCE_HOVER, i, v3(c->f[0], c->f[1], c->f[2]), v3(0, 0, 1), c->f[2]);
        break;
    case CMD_EXPLODE:
        if (b) do_explode(i);
        break;
    case CMD_TOGGLE:
        if (b) toggle_forces(i);
        break;
    case CMD_TOOL:
        if (c->i1 >= TOOL_WHEEL && c->i1 <= TOOL_EMITTER) {
            tool_apply(c->i1, b ? i : -1, v3(c->f[0], c->f[1], c->f[2]), v3_norm(v3(c->f[3], c->f[4], c->f[5])),
                       (int)(c->u & 0xFFFF) - 1, v3(c->f[6], c->f[7], c->f[8]), (int)((c->u >> 16) & 0xFF),
                       (int)((c->u >> 24) & 0xFF));
        }
        break;
    case CMD_SEAT_TOGGLE:
        if (b) toggle_contraption(i);
        break;
    case CMD_UNDO:
        undo_pop(slot);
        break;
    case CMD_USE: {
        // A gamemode's use comes first (Prop Hunt: becoming the prop they aim at); a sandbox script's after
        // doors and buttons
        vec3 eye = v3(c->f[0], c->f[1], c->f[2]), dir = v3(c->f[3], c->f[4], c->f[5]);
        int body = b && v3_len(v3_sub(b->pos, eye)) < GM_USE_RANGE + 100.0f ? i : -1;
        if (!gm_sandbox()) {
            if (!script_use(slot, body)) ents_use(&G.map, eye, dir);
        } else if (!ents_use(&G.map, eye, dir)) {
            script_use(slot, body);
        }
        break;
    }
    case CMD_RESPAWN:
        if (g_gm.on) {
            gm_respawn_request(slot); // when the gamemode lets them, at one of its spawn points
        } else {
            p->health = 100.0f;
        }
        break;
    case CMD_DAMAGE:
        g_gm.p[slot].attacker = -1; // a fall
        if (p->health > 0.0f && !host_god()) p->health = fmaxf(0.0f, p->health - c->f[0]);
        break;
    case CMD_RAG_SPAWN: {
        int r = npc_ragdoll_spawn(i, v3(c->f[0], c->f[1], c->f[2]), c->f[3]);
        if (r >= 0) undo_push(RAGDOLL_ID_BASE + r);
        break;
    }
    case CMD_RAG_REMOVE:
        npc_ragdoll_remove(i);
        break;
    case CMD_RAG_FREEZE:
        npc_ragdoll_freeze(i, c->i1);
        break;
    case CMD_RAG_INFLATE:
        npc_ragdoll_inflate(i, c->i1, clampf(c->f[0], 0.5f, 2.0f));
        break;
    case CMD_QUICKCHAT:
        if (i >= 0 && i < QC_SLICES && !script_quick_chat(slot, k_quick_msgs[i])) {
            char text[48];
            snprintf(text, sizeof(text), "%s: %s", p->name, k_quick_msgs[i]);
            game_chat(text);
        }
        break;
    case CMD_GESTURE:
        if (i >= 0 && i < npc_gesture_count()) {
            p->gesture = i + 1;
            p->gesture_seq++;
            npc_player_gesture(slot, i);
        }
        break;
    case CMD_RAG_POSE:
        if (c->i1 == 0) {
            npc_ragdoll_set_face(i, (int)c->u);
        } else {
            npc_ragdoll_set_hand(i, c->i1 - 1, (int)c->u);
        }
        break;
    case CMD_LAUNCH:
        if (is_launcher(i)) {
            vec3 eye = v3(c->f[0], c->f[1], c->f[2]);
            if (g_settings.sound && G.snd.fire[i] >= 0) snd_play_at(G.snd.fire[i], eye, 0.8f);
            proj_launch(slot, i, eye, v3(c->f[3], c->f[4], c->f[5]));
        }
        break;
    case CMD_SQUEEZE:
        npc_bugbait_squeeze(slot);
        break;
    case CMD_PICKUP: {
        // A kit, battery or ammo box the player walked over: gone for everyone, and a kit's health theirs
        int kind = item_kind(i);
        if (!is_pickup(kind) || p->health <= 0.0f) break;
        if (v3_len(v3_sub(G.phys.bodies[i].pos, v3_add(p->pos, v3(0, 0, 36)))) > 150.0f) break;
        if (kind == ITEM_HEALTHKIT || kind == ITEM_HEALTHVIAL) p->health = fminf(MAX_HEALTH, p->health + item_amount(kind));
        remove_body(i);
        break;
    }
    case CMD_HEAL:
        if (p->health > 0.0f) p->health = fminf(MAX_HEALTH, p->health + clampf(c->f[0], 0.0f, 25.0f));
        break;
    case CMD_SHOOT:
        if (is_gun(i) && !is_launcher(i)) {
            vec3 eye = v3(c->f[0], c->f[1], c->f[2]);
            if (g_settings.sound && G.snd.fire[i] >= 0) snd_play_at(G.snd.fire[i], eye, 0.8f);
            shoot(i, eye, v3_norm(v3(c->f[3], c->f[4], c->f[5])), (uint32_t)c->i1, 1, 0);
        }
        break;
    default:
        break;
    }
    G.acting = 0;
}

static int peer_by_mac(const uint8_t *mac) {
    for (int s = 1; s < NET_MAX_PLAYERS; s++) {
        if (G.peers[s].active && memcmp(G.peers[s].mac, mac, 6) == 0) return s;
    }
    return -1;
}

static void release_peer_hold(Peer *p) {
    if (body_valid(p->held_prev) && p->held_prev != G.held) {
        G.phys.bodies[p->held_prev].held = 0;
        phys_wake(&G.phys, p->held_prev);
    }
    p->held_prev = -1;
}

static void drop_peer(int s, const char *why) {
    Peer *p = &G.peers[s];
    release_peer_hold(p);
    char msg[48];
    snprintf(msg, sizeof(msg), "%s %s", p->name, why);
    notify(msg, NOTE_HINT);
    p->active = 0;
    npc_puppet_set(s, 0, -1, v3(0, 0, 0), 0.0f, 0.0f);
}

static int num_players(void);

// A dedicated server's rules (game_set_server): how many can play, and the password to join
static int g_server_players = NET_MAX_PLAYERS - 1;
static char g_server_password[16];

void game_set_server(int max_players, const char *password) {
    g_server_players = max_players < 1 ? 1 : max_players > NET_MAX_PLAYERS - 1 ? NET_MAX_PLAYERS - 1 : max_players;
    snprintf(g_server_password, sizeof(g_server_password), "%s", password ? password : "");
}

// The slots this host gives out, its own (slot 0) included: a PSP takes 4 players, a dedicated server
// keeps slot 0 to itself and takes up to 7
static int host_slots(void) {
    return g_dedicated ? g_server_players + 1 : NET_PSP_HOST_SLOTS;
}

// "A game is running here": the host's name, the map and how full it is
static void make_beacon(PkBeacon *bc) {
    memset(bc, 0, sizeof(*bc));
    bc->h = net_header(PK_BEACON);
    map_base(bc->map, sizeof(bc->map));
    snprintf(bc->host, sizeof(bc->host), "%s", net_nickname());
    bc->players = (uint8_t)num_players();
    bc->max = (uint8_t)(g_dedicated ? host_slots() - 1 : host_slots());
    bc->flags = (g_dedicated ? BEACON_DEDICATED : 0) | (g_dedicated && g_server_password[0] ? BEACON_PASSWORD : 0);
    snprintf(bc->mode, sizeof(bc->mode), "%s", g_gm.mode);
}

static void host_receive(void) {
    union {
        NetHeader h;
        PkJoin join;
        PkCState cs;
        uint8_t raw[1500];
    } pk;
    uint8_t from[6];
    int len;
    while ((len = net_recv(from, &pk, sizeof(pk))) > 0) {
        if (pk.h.version != NET_VERSION) continue;
        int s = peer_by_mac(from);
        if (pk.h.type == PK_INFO) {
            // Someone's server list asking about this game (over Wi-Fi or the internet)
            PkBeacon bc;
            make_beacon(&bc);
            net_send(from, &bc, sizeof(bc));
            continue;
        }
        if (pk.h.type == PK_JOIN && len >= (int)sizeof(PkJoin)) {
            int is_new = s < 0;
            if (is_new) {
                NetHeader no = net_header(PK_FULL);
                if (g_dedicated && g_server_password[0] &&
                    strncmp(pk.join.password, g_server_password, sizeof(pk.join.password)) != 0) {
                    no.slot = JOIN_REFUSED_PASSWORD;
                    net_send(from, &no, sizeof(no));
                    continue;
                }
                for (s = 1; s < host_slots() && G.peers[s].active; s++) {
                }
                if (s == host_slots()) {
                    no.slot = JOIN_REFUSED_FULL;
                    net_send(from, &no, sizeof(no));
                    continue;
                }
            }
            // (Re)start this player, unless it's just a resend of the JOIN we already answered
            Peer *p = &G.peers[s];
            if (is_new || p->nonce != pk.join.nonce) {
                release_peer_hold(p);
                memset(p, 0, sizeof(*p));
                p->active = 1;
                p->nonce = pk.join.nonce;
                memcpy(p->mac, from, 6);
                snprintf(p->name, sizeof(p->name), "%.15s", pk.join.name);
                p->health = 100.0f;
                p->model = -1;
                p->held = p->held_prev = -1;
                p->veh = -1;

                p->pos = v3(G.map.spawns[0].x, G.map.spawns[0].y, G.map.spawns[0].z);
            }
            p->heard = 0.0f;
            if (is_new) {
                char msg[48];
                snprintf(msg, sizeof(msg), "%s joined the game", p->name);
                notify(msg, NOTE_HINT);
                if (net_mode() == NET_MODE_INET && !g_dedicated) achieve_unlock(ACH_ON_THE_AIR);
            }
            PkWelcome w;
            memset(&w, 0, sizeof(w));
            w.h = net_header(PK_WELCOME);
            w.h.slot = (uint8_t)s;
            map_base(w.map, sizeof(w.map));
            snprintf(w.mode, sizeof(w.mode), "%s", g_gm.mode);
            net_send(from, &w, sizeof(w));
        } else if (pk.h.type == PK_CSTATE && s > 0 && len >= (int)offsetof(PkCState, cmds)) {
            Peer *p = &G.peers[s];
            const PkCState *cs = &pk.cs;
            p->heard = 0.0f;
            p->pos = v3(cs->pos[0], cs->pos[1], cs->pos[2]);
            p->yaw = cs->yaw;
            p->pitch = cs->pitch;
            p->weapon = cs->weapon;
            p->flags = cs->flags;
            p->model = cs->model == NET_NO_MODEL ? -1 : cs->model;
            p->colour = cs->colour;
            p->pgun = cs->pgun;
            p->held = body_valid(cs->held) ? cs->held : -1;
            p->held_rag = npc_ragdoll_active(cs->held_rag) ? cs->held_rag : -1;
            p->held_bone = cs->held_bone;
            p->hold_pos = v3(cs->hold_pos[0], cs->hold_pos[1], cs->hold_pos[2]);
            p->hold_rot = q_norm((quat){cs->hold_rot[0], cs->hold_rot[1], cs->hold_rot[2], cs->hold_rot[3]});
            p->veh = body_valid(cs->veh) && vehicle_kind(&G.phys.bodies[cs->veh]) != VEH_NONE ? cs->veh : -1;
            p->veh_throttle = cs->throttle / 127.0f;
            p->veh_steer = cs->steer / 127.0f;
            p->veh_brake = cs->veh_brake;
            int n = cs->ncmds;
            int fits = (len - (int)offsetof(PkCState, cmds)) / (int)sizeof(NetCmd);
            if (n > fits) n = fits;
            if (n > MAX_OUTQ) n = MAX_OUTQ;
            for (int k = 0; k < n; k++) {
                if (!seq_newer(cs->cmds[k].id, p->last_cmd)) continue; // already done
                exec_cmd(s, &cs->cmds[k]);
                p->last_cmd = cs->cmds[k].id;
            }
        } else if (pk.h.type == PK_VOICE && s > 0 && len >= (int)sizeof(PkVoice)) {
            // Play it here and pass it on to everyone else
            PkVoice *v = (PkVoice *)pk.raw;
            v->speaker = (uint8_t)s;
            voice_play(s, &v->frame);
            G.voice_heard[s] = 0.0f;
            net_send(NULL, v, sizeof(PkVoice));
        } else if (pk.h.type == PK_LEAVE && s > 0) {
            drop_peer(s, "left the game");
        }
    }
}

static void pack_player(NetPlayer *np, int s) {
    memset(np, 0, sizeof(*np));
    uint8_t god = host_god() ? PF_GOD : 0; // tells clients, so they stop reporting damage
    if (s == 0 && g_dedicated) return; // nobody plays on a dedicated server
    if (s == 0) {
        np->active = 1;
        np->pos[0] = G.player.origin.x;
        np->pos[1] = G.player.origin.y;
        np->pos[2] = G.player.origin.z;
        np->yaw = G.yaw;
        np->pitch = G.pitch;
        np->weapon = (uint8_t)G.weapon;
        np->model = model_byte(player_model());
        np->colour = (uint8_t)g_settings.player_colour;
        np->pgun = (uint8_t)g_settings.physgun_colour;
        np->flags = (uint8_t)((G.player.noclip ? PF_NOCLIP : 0) | (G.player.ducked ? PF_DUCKED : 0) | (G.dead_time > 0.0f ? PF_DEAD : 0) | (G.veh >= 0 ? PF_VEHICLE : 0) | god);
        np->health = G.health;
        np->held = (int16_t)G.held;
        np->veh = (int16_t)G.veh;
        np->gesture = (uint8_t)g_qc.gesture;
        np->gesture_seq = (uint8_t)g_qc.gesture_seq;
        snprintf(np->name, sizeof(np->name), "%s", net_nickname());
        return;
    }
    const Peer *p = &G.peers[s];
    if (!p->active) return;
    np->active = 1;
    np->pos[0] = p->pos.x;
    np->pos[1] = p->pos.y;
    np->pos[2] = p->pos.z;
    np->yaw = p->yaw;
    np->pitch = p->pitch;
    np->weapon = (uint8_t)p->weapon;
    np->model = model_byte(p->model);
    np->colour = (uint8_t)p->colour;
    np->pgun = (uint8_t)p->pgun;
    np->flags = (uint8_t)((p->flags & ~PF_GOD) | god);
    np->health = p->health;
    np->held = (int16_t)p->held;
    np->veh = (int16_t)p->veh;
    np->gesture = (uint8_t)p->gesture;
    np->gesture_seq = (uint8_t)p->gesture_seq;
    np->ack = p->last_cmd;
    snprintf(np->name, sizeof(np->name), "%s", p->name);
}

static void pack_body(NetBody *nb, int i) {
    const Body *b = &G.phys.bodies[i];
    nb->index = (uint8_t)i;
    nb->lib = (uint8_t)b->lib;
    nb->model = (uint16_t)b->model;
    nb->pos[0] = b->pos.x;
    nb->pos[1] = b->pos.y;
    nb->pos[2] = b->pos.z;
    nb->rot[0] = (int16_t)(b->rot.x * 32767.0f);
    nb->rot[1] = (int16_t)(b->rot.y * 32767.0f);
    nb->rot[2] = (int16_t)(b->rot.z * 32767.0f);
    nb->rot[3] = (int16_t)(b->rot.w * 32767.0f);
    nb->tint = b->tint_abgr;
    nb->balloons = (uint8_t)b->balloons;
    nb->flags = (uint8_t)((b->frozen ? 1 : 0) | (b->held ? 2 : 0));
}

static void host_send_bodies(void) {
    static PkBodies pk;
    memset(&pk, 0, offsetof(PkBodies, b));
    pk.h = net_header(PK_BODIES);
    pk.tick = G.tick;
    for (int i = 0; i < MAX_BODIES; i++) {
        if (G.phys.bodies[i].active) pk.active[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    // Moving or changed bodies every time, then a few others in turn so everything gets refreshed
    int list[MAX_BODY_PACKETS * BODIES_PER_PACKET];
    int cap = MAX_BODY_PACKETS * BODIES_PER_PACKET, n = 0;
    unsigned char picked[MAX_BODIES] = {0};
    for (int i = 0; i < MAX_BODIES && n < cap; i++) {
        const Body *b = &G.phys.bodies[i];
        if (!b->active || !((!b->asleep && !b->frozen) || b->held || G.body_dirty[i])) continue;
        list[n++] = i;
        picked[i] = 1;
        G.body_dirty[i] = 0;
    }
    int k, extra = 0;
    for (k = 0; k < MAX_BODIES && n < cap && extra < BODY_ROUND_ROBIN; k++) {
        int i = (G.body_rr + k) % MAX_BODIES;
        if (!G.phys.bodies[i].active || picked[i]) continue;
        list[n++] = i;
        extra++;
    }
    G.body_rr = (G.body_rr + k) % MAX_BODIES;
    int start = 0;
    do {
        int cnt = n - start < BODIES_PER_PACKET ? n - start : BODIES_PER_PACKET;
        pk.count = (uint8_t)cnt;
        for (int j = 0; j < cnt; j++) pack_body(&pk.b[j], list[start + j]);
        net_send(NULL, &pk, (int)offsetof(PkBodies, b) + cnt * (int)sizeof(NetBody));
        start += cnt;
    } while (start < n);
}

static void host_send_players(void) {
    PkPlayers pk;
    memset(&pk, 0, sizeof(pk));
    pk.h = net_header(PK_PLAYERS);
    pk.tick = G.tick;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) pack_player(&pk.players[s], s);
    for (int i = 0; i < MAX_NPCS; i++) npc_net_get(i, &pk.npcs[i]);
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        pk.rag_gen[r] = (uint8_t)npc_ragdoll_gen(r);
        if (pk.rag_gen[r]) pk.rag_mask |= (uint8_t)(1u << r);
    }
    net_send(NULL, &pk, sizeof(pk));
}

// Each ragdoll's building info as soon as it's new (then once a second, in case that was lost), and its
// points every snapshot while it moves (once a second when it's still)
static void host_send_ragdolls(void) {
    static PkRagInfo info;
    static PkRagPose pose;
    static uint8_t sent_gen[MAX_RAGDOLLS];
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        int gen = npc_ragdoll_gen(r);
        if (!gen) {
            sent_gen[r] = 0;
            continue;
        }
        int turn = (int)(G.tick % 20u) == r * 3; // this ragdoll's slot in the once-a-second round
        if (sent_gen[r] != gen || turn) {
            info.h = net_header(PK_RAGINFO);
            npc_ragdoll_net_info(r, &info.i);
            net_send(NULL, &info, sizeof(info));
            sent_gen[r] = (uint8_t)gen;
        }
        if (npc_ragdoll_awake(r) || (int)(G.tick % 20u) == r * 3 + 1) {
            pose.h = net_header(PK_RAGPOSE);
            int n = npc_ragdoll_net_pose(r, &pose.p);
            net_send(NULL, &pose, (int)sizeof(NetHeader) + n);
        }
    }
}

static void host_send_ents(void) {
    static PkEnts pk;
    for (int first = 0; first < G.map.num_ents; first += ENTS_PER_PACKET) {
        int cnt = G.map.num_ents - first < ENTS_PER_PACKET ? G.map.num_ents - first : ENTS_PER_PACKET;
        pk.h = net_header(PK_ENTS);
        pk.first = (uint16_t)first;
        pk.count = (uint16_t)cnt;
        int bytes = ents_net_pack(&G.map, first, cnt, pk.data);
        net_send(NULL, &pk, (int)offsetof(PkEnts, data) + bytes);
    }
}

static void host_send_ropes(void) {
    static PkRopes pk;
    memset(&pk, 0, offsetof(PkRopes, r));
    pk.h = net_header(PK_ROPES);
    for (int j = 0; j < MAX_JOINTS && pk.count < MAX_NET_ROPES; j++) {
        const Joint *jt = &G.phys.joints[j];
        if (!jt->active || (jt->type != JOINT_ROPE && jt->type != JOINT_ELASTIC && jt->type != JOINT_SLIDER &&
                            jt->type != JOINT_ROD))
            continue;
        NetRope *r = &pk.r[pk.count++];
        r->a = (uint8_t)jt->a;
        r->b = jt->b >= 0 ? (uint8_t)jt->b : NET_WORLD;
        r->type = (uint8_t)jt->type;
        r->la[0] = jt->la.x;
        r->la[1] = jt->la.y;
        r->la[2] = jt->la.z;
        r->lb[0] = jt->lb.x;
        r->lb[1] = jt->lb.y;
        r->lb[2] = jt->lb.z;
        r->length = jt->length;
    }
    net_send(NULL, &pk, (int)offsetof(PkRopes, r) + pk.count * (int)sizeof(NetRope));
}

static void host_send_devices(void) {
    static PkDevices pk;
    memset(&pk, 0, offsetof(PkDevices, d));
    pk.h = net_header(PK_DEVICES);
    for (int d = 0; d < MAX_DEVICES; d++) {
        const Device *v = &g_dev[d];
        if (!v->type) continue;
        NetDevice *o = &pk.d[pk.count++];
        o->type = (uint8_t)v->type;
        o->body = (uint8_t)v->body;
        o->on = (uint8_t)v->on;
        o->effect = (uint8_t)v->effect;
        o->colour = (uint8_t)v->colour;
        o->target = v->target >= 0 ? (uint8_t)v->target : NET_WORLD;
        o->lpos[0] = v->lpos.x;
        o->lpos[1] = v->lpos.y;
        o->lpos[2] = v->lpos.z;
        o->ldir[0] = v->ldir.x;
        o->ldir[1] = v->ldir.y;
        o->ldir[2] = v->ldir.z;
    }
    net_send(NULL, &pk, (int)offsetof(PkDevices, d) + pk.count * (int)sizeof(NetDevice));
}

static void client_apply_devices(const PkDevices *pk, int len) {
    int cnt = pk->count;
    int fits = (len - (int)offsetof(PkDevices, d)) / (int)sizeof(NetDevice);
    if (cnt > fits) cnt = fits;
    float acc[MAX_DEVICES];
    for (int d = 0; d < MAX_DEVICES; d++) acc[d] = g_dev[d].emit_acc;
    memset(g_dev, 0, sizeof(g_dev));
    for (int k = 0; k < cnt && k < MAX_DEVICES; k++) {
        const NetDevice *o = &pk->d[k];
        if (!body_valid(o->body) || o->type < DEV_LAMP || o->type > DEV_EMITTER) continue;
        Device *v = &g_dev[k];
        v->type = o->type;
        v->body = o->body;
        v->on = o->on;
        v->effect = o->effect % EMIT_COUNT;
        v->colour = o->colour % PALETTE_SIZE;
        v->target = o->target == NET_WORLD ? -1 : o->target;
        v->lpos = v3(o->lpos[0], o->lpos[1], o->lpos[2]);
        v->ldir = v3(o->ldir[0], o->ldir[1], o->ldir[2]);
        v->emit_acc = acc[k];
    }
}

static int num_players(void) {
    int n = g_dedicated ? 0 : 1;
    for (int s = 1; s < NET_MAX_PLAYERS; s++) n += G.peers[s].active;
    return n;
}

// Host, before the physics step: hear clients, drive their physics guns, move their puppets
static void host_update(float dt) {
    NetState st = net_poll();
    if (st == NET_FAILED) {
        notify(net_status(), NOTE_ERROR);
        net_stop();
        G.role = ROLE_OFFLINE;
        return;
    }
    if (st != NET_READY) return;
    if (!G.net_was_ready) {
        G.net_was_ready = 1;
        if (!g_dedicated) notify(NET_HOSTING_HINT, NOTE_HINT);
    }
    host_receive();
    for (int s = 1; s < NET_MAX_PLAYERS; s++) {
        Peer *p = &G.peers[s];
        if (!p->active) {
            npc_puppet_set(s, 0, -1, v3(0, 0, 0), 0.0f, dt);
            continue;
        }
        p->heard += dt;
        if (p->heard > PEER_TIMEOUT) {
            drop_peer(s, "timed out");
            continue;
        }
        if (p->held != p->held_prev) release_peer_hold(p);
        if (body_valid(p->held) && p->held != G.held) {
            physgun_drive(p->held, p->hold_pos, p->hold_rot);
            p->held_prev = p->held;
        }
        if (p->held_rag >= 0) npc_ragdoll_drag(p->held_rag, p->held_bone, p->hold_pos, dt);
        npc_puppet_set(s, p->health > 0.0f && !(p->flags & PF_VEHICLE), p->model, p->pos, p->yaw, dt);
        npc_puppet_tint(s, body_tint(p->colour));
        gm_puppet(s);
    }
}

static void host_send_proj(void);

// Host, after the physics step: beacons and snapshots
static void host_send(float dt) {
    if (!net_active()) return;
    G.beacon_timer -= dt;
    if (G.beacon_timer <= 0.0f) {
        G.beacon_timer = BEACON_INTERVAL;
        PkBeacon bc;
        make_beacon(&bc);
        net_send(NULL, &bc, sizeof(bc));
    }
    if (num_players() < (g_dedicated ? 1 : 2)) return; // nobody to send to
    gm_host_send(dt);
    G.snap_timer -= dt;
    if (G.snap_timer <= 0.0f) {
        G.snap_timer = SNAP_INTERVAL;
        G.tick++;
        host_send_players();
        host_send_bodies();
        host_send_ragdolls();
        host_send_proj();
    }
    G.ents_timer -= dt;
    if (G.ents_timer <= 0.0f && G.map.num_ents) {
        G.ents_timer = ENTS_INTERVAL;
        host_send_ents();
    }
    G.ropes_timer -= dt;
    if (G.ropes_timer <= 0.0f) {
        G.ropes_timer = ROPES_INTERVAL;
        host_send_ropes();
        host_send_devices();
    }
}

// --- Projectiles over the network: what's flying, and the latest explosions --------------------------

typedef struct {
    uint8_t type, stuck, pad[2];
    float pos[3], vel[3];
} NetProj;

typedef struct {
    NetHeader h;
    uint8_t count, fx_seq, pad[2];
    float fx[4][3]; // explosions fx_seq, fx_seq - 1...
    NetProj p[MAX_PROJECTILES];
} PkProj;

// Host, with each snapshot while anything's flying (or something's just gone off)
static void host_send_proj(void) {
    PkProj pk;
    memset(&pk, 0, sizeof(pk));
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        const Projectile *p = &g_proj[k];
        if (!p->type) continue;
        NetProj *np = &pk.p[pk.count++];
        np->type = (uint8_t)p->type;
        np->stuck = (uint8_t)p->stuck;
        memcpy(np->pos, &p->pos, sizeof(np->pos));
        memcpy(np->vel, &p->vel, sizeof(np->vel));
    }
    if (!pk.count && g_fx_age > 1.0f) return;
    pk.h = net_header(PK_PROJ);
    pk.fx_seq = g_fx_seq;
    for (int k = 0; k < 4; k++) memcpy(pk.fx[k], &g_fx_pos[k], sizeof(pk.fx[k]));
    net_send(NULL, &pk, (int)offsetof(PkProj, p) + pk.count * (int)sizeof(NetProj));
}

static void client_apply_proj(const PkProj *pk, int len) {
    int n = pk->count, fits = (len - (int)offsetof(PkProj, p)) / (int)sizeof(NetProj);
    if (n > fits) n = fits;
    if (n > MAX_PROJECTILES) n = MAX_PROJECTILES;
    // Keep each rocket's smoke timing; the rest is the host's
    for (int k = 0; k < MAX_PROJECTILES; k++) {
        Projectile *p = &g_proj[k];
        if (k >= n) {
            p->type = 0;
            continue;
        }
        p->type = pk->p[k].type;
        p->stuck = pk->p[k].stuck;
        memcpy(&p->pos, pk->p[k].pos, sizeof(p->pos));
        memcpy(&p->vel, pk->p[k].vel, sizeof(p->vel));
    }
    // Explosions we haven't shown yet, oldest first
    if (g_fx_synced) {
        for (int k = 3; k >= 0; k--) {
            uint8_t seq = (uint8_t)(pk->fx_seq - k);
            if ((int8_t)(seq - g_fx_seen) > 0) show_explosion(v3(pk->fx[k][0], pk->fx[k][1], pk->fx[k][2]));
        }
    }
    g_fx_seen = pk->fx_seq;
    g_fx_synced = 1;
}

static void client_apply_players(const PkPlayers *pk) {
    npc_ragdoll_net_keep(pk->rag_mask, pk->rag_gen);
    if (!pk->players[0].active) {
        // Nobody plays on the host: a dedicated server. And the game's maker, on one by name
        achieve_unlock(ACH_ONLINE);
        if (strcasecmp(net_nickname(), "Retcon") == 0) achieve_unlock(ACH_REAL_RETCON);
    }
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        const NetPlayer *np = &pk->players[s];
        if (s == G.my_slot) {
            // Forget the commands the host has run
            int keep = 0;
            for (int k = 0; k < G.outq_n; k++) {
                if (seq_newer(G.outq[k].id, np->ack)) G.outq[keep++] = G.outq[k];
            }
            G.outq_n = keep;
            G.acked = np->ack;
            G.server_god = (np->flags & PF_GOD) != 0;
            if (G.respawn_pending && !seq_newer(G.respawn_cmd, np->ack)) G.respawn_pending = 0;
            if (g_gm.spawn_grace > 0.0f && np->health > 0.0f) g_gm.spawn_grace = 0.0f; // the host has our respawn
            if (!G.respawn_pending && g_gm.spawn_grace <= 0.0f) {
                if (np->health < G.health - 0.5f) {
                    G.hurt_flash = 0.4f;
                    play(G.snd.impact[0], 0.8f);
                }
                G.health = np->health;
                if (G.health <= 0.0f && G.dead_time <= 0.0f) {
                    G.dead_time = 0.001f;
                    physgun_release(1);
                    G.qmenu = 0;
                }
            }
            continue;
        }
        Peer *p = &G.peers[s];
        p->active = np->active;
        p->pos = v3(np->pos[0], np->pos[1], np->pos[2]);
        p->yaw = np->yaw;
        p->pitch = np->pitch;
        p->weapon = np->weapon;
        p->flags = np->flags;
        p->model = np->model == NET_NO_MODEL ? -1 : np->model;
        p->colour = np->colour;
        p->pgun = np->pgun;
        // A new gesture (not one from before this PSP joined, or before they came back)
        if (!np->active) {
            p->gesture_known = 0;
        } else if (!p->gesture_known || np->gesture_seq != (uint8_t)p->gesture_seq) {
            if (p->gesture_known && np->gesture) npc_player_gesture(s, np->gesture - 1);
            p->gesture_seq = np->gesture_seq;
            p->gesture_known = 1;
        }
        p->health = np->health;
        p->veh = np->veh;
        snprintf(p->name, sizeof(p->name), "%.15s", np->name);
    }
    for (int i = 0; i < MAX_NPCS; i++) npc_net_set(i, &pk->npcs[i]);
}

static void client_apply_bodies(const PkBodies *pk, int len) {
    if ((int32_t)(pk->tick - G.last_body_tick) < 0) return; // arrived out of order
    G.last_body_tick = pk->tick;
    for (int i = 0; i < MAX_BODIES; i++) {
        if (!(pk->active[i >> 3] & (1u << (i & 7))) && G.phys.bodies[i].active) remove_body(i);
    }
    int cnt = pk->count;
    int fits = (len - (int)offsetof(PkBodies, b)) / (int)sizeof(NetBody);
    if (cnt > fits) cnt = fits;
    for (int j = 0; j < cnt; j++) {
        const NetBody *nb = &pk->b[j];
        int i = nb->index;
        if (i >= MAX_BODIES || !(pk->active[i >> 3] & (1u << (i & 7)))) continue;
        if ((nb->lib == 1 && (!G.maplib_loaded || nb->model >= G.maplib.num_models)) || (nb->lib == 0 && (!G.props_loaded || nb->model >= G.props.num_models + propx_count()))) continue;
        Body *b = &G.phys.bodies[i];
        vec3 pos = v3(nb->pos[0], nb->pos[1], nb->pos[2]);
        quat rot = q_norm((quat){nb->rot[0] / 32767.0f, nb->rot[1] / 32767.0f, nb->rot[2] / 32767.0f, nb->rot[3] / 32767.0f});
        if (!b->active || b->model != nb->model || b->lib != nb->lib) {
            if (b->active) remove_body(i);
            int local;
            const PropLib *lib = nb->lib == 1 ? &G.maplib : model_lib(nb->model, &local);
            if (nb->lib == 1) local = nb->model;
            if (!lib) continue; // an extra prop we have no room for
            const PpakModel *md = &lib->models[local];
            vec3 mins = v3(md->mins[0], md->mins[1], md->mins[2]), maxs = v3(md->maxs[0], md->maxs[1], md->maxs[2]);
            if (phys_spawn_at(&G.phys, i, nb->model, (int)(md->flags & PPAK_SHAPE_MASK), mins, maxs, md->mass, pos, rot) < 0) continue;
            attach_hull(b, lib, local);
            b->lib = nb->lib;
            b->surface = (int)md->surface;
        }
        G.net_tpos[i] = pos;
        G.net_trot[i] = rot;
        b->tint_abgr = nb->tint;
        if (b->balloons != nb->balloons) {
            b->balloons = nb->balloons;
            update_balloon_lift(b);
        }
        if (i != G.held) b->frozen = nb->flags & 1;
        b->asleep = 1; // never simulated here
    }
}

static void client_apply_ropes(const PkRopes *pk, int len) {
    int cnt = pk->count;
    int fits = (len - (int)offsetof(PkRopes, r)) / (int)sizeof(NetRope);
    if (cnt > fits) cnt = fits;
    memset(G.phys.joints, 0, sizeof(G.phys.joints));
    for (int k = 0; k < cnt && k < MAX_JOINTS; k++) {
        const NetRope *r = &pk->r[k];
        int ob = r->b == NET_WORLD ? -1 : r->b;
        if (!body_valid(r->a) || (ob >= 0 && !body_valid(ob))) continue;
        Joint *jt = &G.phys.joints[k];
        jt->active = 1;
        jt->type = r->type == JOINT_ELASTIC || r->type == JOINT_SLIDER || r->type == JOINT_ROD ? r->type : JOINT_ROPE;

        jt->a = r->a;
        jt->b = ob;
        jt->la = v3(r->la[0], r->la[1], r->la[2]);
        jt->lb = v3(r->lb[0], r->lb[1], r->lb[2]);
        jt->length = r->length;
    }
}

// Client: returns 0 when the game is over (host gone)
static int client_update(float dt) {
    if (net_poll() != NET_READY) {
        snprintf(G.exit_msg, sizeof(G.exit_msg), "%s", NET_LOST_MSG);
        return 0;
    }
    static union {
        NetHeader h;
        PkPlayers pl;
        PkBodies bd;
        PkEnts en;
        PkRopes rp;
        PkRagInfo ri;
        PkRagPose rpo;
        uint8_t raw[1500];
    } pk;
    uint8_t from[6];
    int len;
    while ((len = net_recv(from, &pk, sizeof(pk))) > 0) {
        if (memcmp(from, G.host_mac, 6) != 0 || pk.h.version != NET_VERSION) continue;
        G.host_heard = 0.0f;
        switch (pk.h.type) {
        case PK_PLAYERS:
            if (len >= (int)sizeof(PkPlayers)) client_apply_players(&pk.pl);
            break;
        case PK_BODIES:
            if (len >= (int)offsetof(PkBodies, b)) client_apply_bodies(&pk.bd, len);
            break;
        case PK_ENTS:
            if (len >= (int)offsetof(PkEnts, data) + pk.en.count * 4 && pk.en.first + pk.en.count <= G.map.num_ents) {
                ents_net_unpack(&G.map, pk.en.first, pk.en.count, pk.en.data);
            }
            break;
        case PK_ROPES:
            if (len >= (int)offsetof(PkRopes, r)) client_apply_ropes(&pk.rp, len);
            break;
        case PK_DEVICES:
            if (len >= (int)offsetof(PkDevices, d)) client_apply_devices((const PkDevices *)pk.raw, len);
            break;
        case PK_PROJ:
            if (len >= (int)offsetof(PkProj, p)) client_apply_proj((const PkProj *)pk.raw, len);
            break;
        case PK_RAGINFO:
            if (len >= (int)sizeof(PkRagInfo)) npc_ragdoll_net_set_info(&pk.ri.i);
            break;
        case PK_CHAT:
            if (len >= (int)sizeof(PkChat)) {
                const PkChat *c = (const PkChat *)pk.raw;
                char text[sizeof(c->text)];
                snprintf(text, sizeof(text), "%.*s", (int)sizeof(c->text) - 1, c->text);
                notify(text, NOTE_CHAT);
            }
            break;
        case PK_VOICE:
            if (len >= (int)sizeof(PkVoice)) {

                const PkVoice *v = (const PkVoice *)pk.raw;
                if (v->speaker != G.my_slot && v->speaker < NET_MAX_PLAYERS) {
                    voice_play(v->speaker, &v->frame);
                    G.voice_heard[v->speaker] = 0.0f;
                }
            }
            break;
        case PK_RAGPOSE:
            if (len >= (int)(sizeof(NetHeader) + offsetof(RagPose, pos)) &&
                len >= (int)(sizeof(NetHeader) + offsetof(RagPose, pos) + pk.rpo.p.nb * sizeof(pk.rpo.p.pos[0]))) {
                npc_ragdoll_net_set_pose(&pk.rpo.p);
            }
            break;
        case PK_GMSTATE:
            gm_client_state(pk.raw, len);
            break;
        case PK_GMEVENTS:
            gm_client_events(pk.raw, len);
            break;
        case PK_LEAVE:
            // h.slot says why (GAME_LEAVE_*): servers kick players and change maps. A map change joins
            // the same server again straight away (game_rejoin_address).
            snprintf(G.exit_msg, sizeof(G.exit_msg), "%s",
                     pk.h.slot == GAME_LEAVE_KICKED ? "You were kicked from the server"
                     : pk.h.slot == GAME_LEAVE_MAP  ? "The server is changing map: joining again..."
                                                    : "The host ended the game");
            if (pk.h.slot == GAME_LEAVE_MAP && net_mode() == NET_MODE_INET) {
                net_addr_text(G.host_mac, g_rejoin, sizeof(g_rejoin));
            }
            return 0;
        default:
            break;
        }
    }
    G.host_heard += dt;
    if (G.host_heard > HOST_TIMEOUT) {
        snprintf(G.exit_msg, sizeof(G.exit_msg), "Lost connection to the host");
        return 0;
    }

    G.send_timer -= dt;
    if (G.send_timer <= 0.0f) {
        G.send_timer = SEND_INTERVAL;
        PkCState cs;
        memset(&cs, 0, sizeof(cs));
        cs.h = net_header(PK_CSTATE);
        cs.pos[0] = G.player.origin.x;
        cs.pos[1] = G.player.origin.y;
        cs.pos[2] = G.player.origin.z;
        cs.yaw = G.yaw;
        cs.pitch = G.pitch;
        cs.weapon = (uint8_t)G.weapon;
        cs.model = model_byte(player_model());
        cs.colour = (uint8_t)g_settings.player_colour;
        cs.pgun = (uint8_t)g_settings.physgun_colour;
        cs.flags = (uint8_t)((G.player.noclip ? PF_NOCLIP : 0) | (G.player.ducked ? PF_DUCKED : 0) | (G.dead_time > 0.0f ? PF_DEAD : 0) | (G.veh >= 0 ? PF_VEHICLE : 0));
        cs.held = (int16_t)((carries_props(G.weapon) || G.use_carry) && G.held >= 0 ? G.held : -1);
        cs.held_rag = (int8_t)(G.held_rag >= 0 ? G.held_rag : -1);
        cs.held_bone = (uint8_t)G.held_bone;
        cs.hold_pos[0] = G.net_hold_pos.x;
        cs.hold_pos[1] = G.net_hold_pos.y;
        cs.hold_pos[2] = G.net_hold_pos.z;
        cs.hold_rot[0] = G.net_hold_rot.x;
        cs.hold_rot[1] = G.net_hold_rot.y;
        cs.hold_rot[2] = G.net_hold_rot.z;
        cs.hold_rot[3] = G.net_hold_rot.w;
        cs.veh = (int16_t)G.veh;
        cs.throttle = (int8_t)(G.veh_throttle * 127.0f);
        cs.steer = (int8_t)(G.veh_steer * 127.0f);
        cs.veh_brake = (uint8_t)G.veh_brake;
        cs.ncmds = (uint8_t)G.outq_n;
        memcpy(cs.cmds, G.outq, sizeof(NetCmd) * G.outq_n);
        net_send(G.host_mac, &cs, (int)offsetof(PkCState, cmds) + G.outq_n * (int)sizeof(NetCmd));
    }

    // Glide bodies toward their latest positions (the one we hold is ours to move)
    float k = clampf(dt * 15.0f, 0.0f, 1.0f);
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &G.phys.bodies[i];
        if (!b->active || i == G.held) continue;
        vec3 d = v3_sub(G.net_tpos[i], b->pos);
        b->pos = v3_len(d) > 256.0f ? G.net_tpos[i] : v3_mad(b->pos, d, k);
        quat t = G.net_trot[i];
        if (t.x * b->rot.x + t.y * b->rot.y + t.z * b->rot.z + t.w * b->rot.w < 0.0f) t = (quat){-t.x, -t.y, -t.z, -t.w};
        b->rot = q_norm((quat){b->rot.x + (t.x - b->rot.x) * k, b->rot.y + (t.y - b->rot.y) * k, b->rot.z + (t.z - b->rot.z) * k,
                               b->rot.w + (t.w - b->rot.w) * k});
    }
    npcs_animate(dt);
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        const Peer *p = &G.peers[s];
        npc_puppet_set(s, s != G.my_slot && p->active && p->health > 0.0f && !(p->flags & PF_VEHICLE), p->model, p->pos,
                       p->yaw, dt);
        npc_puppet_tint(s, body_tint(p->colour));
        gm_puppet(s);
    }
    return 1;
}

static void net_leave(void) {
    if (G.role == ROLE_OFFLINE) return;
    if (net_active()) {
        NetHeader bye = net_header(PK_LEAVE);
        for (int k = 0; k < 3; k++) net_send(G.role == ROLE_CLIENT ? G.host_mac : NULL, &bye, sizeof(bye));
    }
    net_stop();
    G.role = ROLE_OFFLINE;
    voice_set_talking(0);
    npcs_clear();
    npc_ragdolls_set_remote(0);
}

// Push to talk (multiplayer, with a mic): hold D-pad Up; a quick tap is still undo. In single
// player Up undoes straight away as before. active: the game's controls are live (not a menu).
#define TALK_HOLD 0.25f
static void voice_update(const InputState *in, float dt, int active) {
    G.up_undo = 0;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) G.voice_heard[s] += dt;
    int mp = G.role != ROLE_OFFLINE && voice_available();
    if (!active) {
        G.talk_hold = 0.0f;
        voice_set_talking(0);
    } else if (!mp) {
        G.up_undo = (in->pressed & PSP_CTRL_UP) != 0;
    } else if (in->held & PSP_CTRL_UP) {
        G.talk_hold += dt;
        if (G.talk_hold >= TALK_HOLD && !voice_talking()) voice_set_talking(1);
    } else {
        if (G.talk_hold > 0.0f && G.talk_hold < TALK_HOLD) G.up_undo = 1;
        G.talk_hold = 0.0f;
        voice_set_talking(0);
    }
    // Send what the mic has picked up: to the host, or (the host) to everyone
    VoiceFrame f;
    while (voice_take_frame(&f)) {
        if (!net_active()) continue;
        PkVoice v;
        v.h = net_header(PK_VOICE);
        v.speaker = (uint8_t)G.my_slot;
        v.seq = G.voice_seq++;
        v.pad[0] = v.pad[1] = 0;
        v.frame = f;
        net_send(G.role == ROLE_CLIENT ? G.host_mac : NULL, &v, sizeof(v));
    }
}

// Who's talking: you (a mic dot) and anyone heard in the last moment
static void draw_voice_hud(void) {
    if (G.role == ROLE_OFFLINE) return;
    float y = 60.0f;
    if (voice_talking()) {
        ui_rrect(8, y, 74, 16, 4, 0xA0000000);
        ui_rrect(12, y + 4, 8, 8, 4, 0xFF3030E0);
        ui_text(FONT_SMALL, 24, y + 2, 0xFFFFFFFF, ALIGN_LEFT, "Talking");
        y += 20.0f;
    }
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        if (s == G.my_slot || G.voice_heard[s] > 0.3f || !G.peers[s].active) continue;
        char line[32];
        snprintf(line, sizeof(line), "%.15s", G.peers[s].name[0] ? G.peers[s].name : "Player");
        float w = ui_text_width(FONT_SMALL, line) + 26.0f;
        ui_rrect(8, y, w, 16, 4, 0xA0000000);
        ui_rrect(12, y + 4, 8, 8, 4, 0xFF40C040);
        ui_text(FONT_SMALL, 24, y + 2, 0xFFFFFFFF, ALIGN_LEFT, line);
        y += 20.0f;
    }
}

// Other players' names over their heads, in their Profile colours (when you can see them)
static void draw_name_tags(void) {
    if (G.role == ROLE_OFFLINE) return;
    float cy = cosf(G.cam.yaw * DEG2RAD), sy = sinf(G.cam.yaw * DEG2RAD);
    float cp = cosf(G.cam.pitch * DEG2RAD), sp = sinf(G.cam.pitch * DEG2RAD);
    vec3 f = v3(cp * cy, cp * sy, sp), r = v3(sy, -cy, 0.0f), u = v3(-cy * sp, -sy * sp, cp);
    vec3 eye = v3(G.cam.pos.x, G.cam.pos.y, G.cam.pos.z);
    float th = tanf(G.cam.fov_y * 0.5f * DEG2RAD), tw = th * 480.0f / 272.0f;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        const Peer *p = &G.peers[s];
        if (s == G.my_slot || !p->active || p->health <= 0.0f || !p->name[0] || !gm_tag_visible(s)) continue;
        vec3 head = v3_add(p->pos, v3(0, 0, (p->flags & PF_DUCKED) ? 56.0f : 82.0f));
        vec3 d = v3_sub(head, eye);
        float z = v3_dot(d, f), dist = v3_len(d);
        if (z < 16.0f || dist > 1500.0f) continue;
        float sx = 240.0f + v3_dot(d, r) / (z * tw) * 240.0f, sy2 = 136.0f - v3_dot(d, u) / (z * th) * 136.0f;
        if (sx < -40.0f || sx > 520.0f || sy2 < 0.0f || sy2 > 272.0f) continue;
        Trace tr;
        cm_trace_line(&G.map, eye, head, MASK_PROPSOLID, NULL, 0, &tr);
        if (tr.fraction < 0.98f) continue; // behind a wall
        unsigned int a = dist > 1100.0f ? (unsigned int)(255.0f * (1500.0f - dist) / 400.0f) : 255u;
        float w = ui_text_width(FONT_SMALL, p->name) + 10.0f;
        ui_rrect(sx - w * 0.5f, sy2 - 9.0f, w, 16.0f, 3, ((a * 0x90u / 255u) << 24) | 0x202020);
        ui_text(FONT_SMALL, sx, sy2 - 7.0f, (a << 24) | (profile_colour(p->colour, 0) & 0xFFFFFFu), ALIGN_CENTER, p->name);
    }
}

void game_host(void) {

    G.role = ROLE_HOST;
    G.my_slot = 0;
    G.net_was_ready = 0;
    if (net_start() != 0) {
        notify(net_status(), NOTE_ERROR);
        G.role = ROLE_OFFLINE;
        return;
    }
    if (!g_dedicated) notify(NET_STARTING_MSG, NOTE_HINT);
    voice_init();
}

void game_join(int slot, const uint8_t host_mac[6]) {
    G.role = ROLE_CLIENT;
    G.my_slot = slot;
    memcpy(G.host_mac, host_mac, 6);
    G.host_heard = 0.0f;
    G.last_body_tick = 0;
    G.outq_n = 0;
    G.next_cmd = 0;
    G.respawn_pending = 0;
    G.server_god = 0;
    npcs_clear();
    npc_ragdolls_set_remote(1);
    for (int i = 0; i < MAX_BODIES; i++) {
        G.net_tpos[i] = G.phys.bodies[i].pos;
        G.net_trot[i] = G.phys.bodies[i].rot;
    }
    notify("Joined the game", NOTE_HINT);
    voice_init();
}

const char *game_exit_message(void) {
    return G.exit_msg;
}

int game_rejoin_address(char *out, int len) {
    if (!g_rejoin[0]) return 0;
    snprintf(out, (size_t)len, "%s", g_rejoin);
    g_rejoin[0] = 0;
    return 1;
}

// --- For the dedicated server's console -------------------------------------------------------------

const char *game_peer_name(int slot) {
    return slot > 0 && slot < NET_MAX_PLAYERS && G.role == ROLE_HOST && G.peers[slot].active ? G.peers[slot].name : NULL;
}

void game_kick(int slot, int reason) {
    if (!game_peer_name(slot)) return;
    NetHeader bye = net_header(PK_LEAVE);
    bye.slot = (uint8_t)reason;
    for (int k = 0; k < 3; k++) net_send(G.peers[slot].mac, &bye, sizeof(bye));
    drop_peer(slot, reason == GAME_LEAVE_KICKED ? "was kicked" : "left");
}

void game_chat(const char *text) {
    PkChat c;
    memset(&c, 0, sizeof(c));
    snprintf(c.text, sizeof(c.text), "%s", text);
    notify(c.text, NOTE_CHAT);
    if (G.role == ROLE_HOST) {
        c.h = net_header(PK_CHAT);
        net_send(NULL, &c, sizeof(c));
    }
}

static void npc_hits(int target, float amount) {
    if (target == 0) {
        g_gm.p[0].attacker = -1;
        damage_player(amount);
        return;
    }
    Peer *p = &G.peers[target];
    g_gm.p[target].attacker = -1;
    if (p->health > 0.0f && !host_god()) p->health = fmaxf(0.0f, p->health - amount);
}

// --- Physics damage ------------------------------------------------------------------------------
// A prop flying into a player hurts, like HL2: by its mass times how fast it closes in, past a speed
// that lets things be dropped, rolled and nudged harmlessly. It knocks the player back and loses some of
// its own speed. The host works it out for everyone, since only it simulates the props.
#define PROP_HURT_SPEED 300.0f      // closing speed (units/s) below which a prop does nothing
#define PROP_HURT_SCALE (1.0f / 800.0f)
#define PROP_HURT_MAX 150.0f
#define PROP_HURT_COOLDOWN 0.4f

// The hardest hit a player standing at feet takes from a prop this step, or 0. Only the prop's own
// speed counts, so running into a prop doesn't hurt.
static float prop_hit(vec3 feet, int ducked, float dt, int *body, vec3 *dir) {
    vec3 lo = v3_add(feet, v3(-18, -18, 0)), hi = v3_add(feet, v3(18, 18, ducked ? 40.0f : 74.0f));
    vec3 centre = v3_add(feet, v3(0, 0, ducked ? 20.0f : 36.0f));
    float worst = 0.0f;
    for (int i = 0; i < MAX_BODIES; i++) {
        const Body *b = &G.phys.bodies[i];
        if (!b->active || b->asleep || b->frozen || b->held || b->mass < 2.0f) continue;
        vec3 rel = b->vel;
        if (v3_dot(rel, rel) < PROP_HURT_SPEED * PROP_HURT_SPEED) continue;
        // Its box over the whole step, so a fast one can't skip past
        vec3 bl, bh, back = v3_scale(b->vel, -dt);
        phys_body_aabb(b, &bl, &bh);
        vec3 sl = v3(fminf(bl.x, bl.x + back.x), fminf(bl.y, bl.y + back.y), fminf(bl.z, bl.z + back.z));
        vec3 sh = v3(fmaxf(bh.x, bh.x + back.x), fmaxf(bh.y, bh.y + back.y), fmaxf(bh.z, bh.z + back.z));
        if (sl.x > hi.x || sh.x < lo.x || sl.y > hi.y || sh.y < lo.y || sl.z > hi.z || sh.z < lo.z) continue;
        if (bh.z <= feet.z + 6.0f) continue; // under their feet: they're riding it
        vec3 to = v3_sub(centre, b->pos);
        float closing = v3_len(to) > 1.0f ? v3_dot(rel, v3_norm(to)) : v3_len(rel);
        if (closing < PROP_HURT_SPEED) continue;
        float dmg = (closing - PROP_HURT_SPEED) * b->mass * PROP_HURT_SCALE;
        if (dmg > worst) {
            worst = dmg;
            *body = i;
            *dir = v3_norm(rel);
        }
    }
    return fminf(worst, PROP_HURT_MAX);
}

static void prop_damage(float dt) {
    G.prop_hurt -= dt;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        int local = s == 0;
        Peer *p = &G.peers[s];
        if (!local) {
            p->prop_hurt -= dt;
            if (G.role != ROLE_HOST || !p->active || p->health <= 0.0f || (p->flags & (PF_NOCLIP | PF_VEHICLE)) || p->prop_hurt > 0.0f) continue;
        } else if (G.dead_time > 0.0f || G.player.noclip || G.prop_hurt > 0.0f || G.veh >= 0) {
            continue;
        }
        int bi = -1;
        vec3 dir = v3(0, 0, 0);
        float dmg = local ? prop_hit(G.player.origin, G.player.ducked, dt, &bi, &dir)
                          : prop_hit(p->pos, (p->flags & PF_DUCKED) != 0, dt, &bi, &dir);
        if (dmg < 1.0f) continue;
        Body *b = &G.phys.bodies[bi];
        b->vel = v3_scale(b->vel, 0.5f); // it gave up some of its speed to the hit
        if (local) {
            G.prop_hurt = PROP_HURT_COOLDOWN;
            g_gm.p[0].attacker = -1;
            damage_player(dmg);
            G.player.velocity = v3_mad(G.player.velocity, dir, fminf(dmg * 8.0f, 450.0f));
            if (dir.z > -0.5f) G.player.on_ground = 0;
        } else {
            p->prop_hurt = PROP_HURT_COOLDOWN;
            g_gm.p[s].attacker = -1;
            if (!host_god()) p->health = fmaxf(0.0f, p->health - dmg);
        }
    }
}

// --- Vehicles and seats (vehicle.c) ----------------------------------------------------------------

// Someone else is in it (the host knows its peers; a client hears it in the players snapshot)
static int vehicle_taken(int body) {
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        if (s == G.my_slot || G.role == ROLE_OFFLINE) continue;
        const Peer *p = &G.peers[s];
        if (p->active && p->veh == body) return 1;
    }
    return 0;
}

static void vehicle_enter(int body) {
    Body *b = &G.phys.bodies[body];
    if (vehicle_taken(body)) {
        notify("Someone's already in it", NOTE_ERROR);
        return;
    }
    if (b->frozen && vehicle_kind(b) != VEH_SEAT) {
        notify("Unfreeze it to drive it", NOTE_ERROR);
        return;
    }
    physgun_release(1);
    G.use_carry = 0;
    G.veh = body;
    vec3 eye;
    vehicle_seat(b, &eye, &G.veh_yaw);
    G.yaw = G.veh_yaw;
    G.pitch = 0.0f;
    G.player.velocity = v3(0, 0, 0);
    G.player.ducked = 0;
    G.player.noclip = 0;
    G.veh_throttle = G.veh_steer = 0.0f;
    G.veh_brake = 0;
    play(G.snd.vehicle_enter, 0.7f);
    int kind = vehicle_kind(b);
    int engine = kind == VEH_JEEP ? G.snd.jeep_engine : kind == VEH_AIRBOAT ? G.snd.airboat_engine : -1;
    G.veh_snd = engine >= 0 ? snd_loop(engine, 0.35f) : -1;
#ifdef PS2
    notify(kind == VEH_SEAT ? "R1: thrusters on/off   Triangle: get out" : "Triangle: get out   X: handbrake", NOTE_HINT);
#else
    notify(kind == VEH_SEAT ? "R: thrusters on/off   Right: get out" : "Right: get out   L: handbrake", NOTE_HINT);
#endif
}

// Gets out beside it (the first exit with room for you). force: even with no room (it's gone, or you died)
static void vehicle_leave(int force) {
    if (G.veh < 0) return;
    Body *b = &G.phys.bodies[G.veh];
    G.player.ducked = 0;
    vec3 mn, mx;
    player_hull(&G.player, &mn, &mx);
    vec3 spot = G.player.origin;
    int found = 0;
    if (b->active) {
        CmBox boxes[MAX_BODIES + MAX_NPCS + NET_MAX_PLAYERS];
        int nb = build_prop_boxes(boxes, -1);
        vec3 exits[6];
        int n = vehicle_exits(b, exits, 6);
        for (int k = 0; k < n && !found; k++) {
            Trace tr;
            cm_trace(&G.map, exits[k], exits[k], mn, mx, MASK_PLAYERSOLID, boxes, nb, &tr);
            if (!tr.startsolid) {
                spot = exits[k];
                found = 1;
            }
        }
    }
    if (!found && !force) {
        notify("No room to get out here", NOTE_ERROR);
        return;
    }
    G.player.origin = spot;
    G.player.velocity = v3(0, 0, 0);
    G.player.on_ground = 0;
    G.was_on_ground = 1; // no fall damage for climbing out
    if (G.veh_snd >= 0) snd_stop(G.veh_snd);
    G.veh_snd = -1;
    G.veh = -1;
    play(G.snd.vehicle_exit, 0.7f);
}

// While in it: the stick drives (or nothing, on a seat), L is the handbrake, R switches a seat's
// contraption, the use key gets out. Looking around still works.
static void vehicle_controls(const InputState *in, float dt) {
    (void)dt;
    Body *b = &G.phys.bodies[G.veh];
    G.veh_throttle = clampf(-in->ay, -1.0f, 1.0f);
    G.veh_steer = clampf(in->ax, -1.0f, 1.0f);
    if (fabsf(G.veh_throttle) < 0.15f) G.veh_throttle = 0.0f; // the nub's dead zone
    if (fabsf(G.veh_steer) < 0.15f) G.veh_steer = 0.0f;
    if (g_test_drive) {
        G.veh_throttle = g_test_throttle;
        G.veh_steer = g_test_steer;
    }
#ifdef PS2
    G.veh_brake = (in->held & PSP_CTRL_CROSS) != 0;
    int toggle = in->pressed & PSP_CTRL_RTRIGGER;
#else
    G.veh_brake = (in->held & PSP_CTRL_LTRIGGER) != 0;
    int toggle = in->pressed & PSP_CTRL_RTRIGGER;
#endif
    if (toggle && vehicle_kind(b) == VEH_SEAT) {
        if (is_client()) {
            send_cmd(CMD_SEAT_TOGGLE, G.veh, 0, 0, NULL, 0);
            play(G.snd.menu_move, 0.5f);
        } else if (toggle_contraption(G.veh)) {
            play(G.snd.menu_move, 0.5f);
        } else {
            notify("No thrusters or hoverballs on it"
, NOTE_HINT);
        }
    }
    if (in->pressed & use_buttons()) vehicle_leave(0);
}

// Keeps you in the seat as it moves, and turns your view with it. Called after the physics step.
static void vehicle_place_player(void) {
    if (G.veh < 0) return;
    Body *b = &G.phys.bodies[G.veh];
    if (!b->active || vehicle_kind(b) == VEH_NONE || G.dead_time > 0.0f) {
        vehicle_leave(1); // it was removed or undone (or you died)
        return;
    }
    vec3 eye;
    float yaw;
    vehicle_seat(b, &eye, &yaw);
    float turn = yaw - G.veh_yaw;
    while (turn > 180.0f) turn -= 360.0f;
    while (turn < -180.0f) turn += 360.0f;
    G.yaw = fmodf(G.yaw + turn, 360.0f);
    G.veh_yaw = yaw;
    G.player.origin = v3_add(G.player.origin, v3_sub(eye, player_eye(&G.player)));
    G.player.velocity = v3(0, 0, 0);
    G.player.on_ground = 1;
    G.was_on_ground = 1;
}

// Host: drives every occupied vehicle this frame (yours and the other players')
static void vehicles_drive(void) {
    if (G.veh >= 0 && !is_client()) vehicle_drive(G.veh, G.veh_throttle, G.veh_steer, G.veh_brake);
    for (int s = 1; s < NET_MAX_PLAYERS && G.role == ROLE_HOST; s++) {
        const Peer *p = &G.peers[s];
        if (p->active && p->veh >= 0 && body_valid(p->veh)) vehicle_drive(p->veh, p->veh_throttle, p->veh_steer, p->veh_brake);
    }
}

// Everything that isn't the local player: physics, NPCs, scripts, the network.
// Returns 0 when a multiplayer game has ended for this PSP.
static int world_step(float dt) {
    projectiles_trails(dt);
    if (is_client()) {
        script_think(dt);
        npc_ragdolls_update(&G.map, dt, &G.phys);
        projectiles_client_update(dt);
        return client_update(dt);
    }
    if (G.role == ROLE_HOST) host_update(dt);
    gm_server_update(dt); // joins, deaths and leavings, for the gamemode
    script_think(dt);
    vehicles_drive();
    phys_step(&G.phys, &G.map, dt);
    vehicles_end_frame();
    projectiles_update(dt);
    prop_damage(dt);
    NpcTarget t[NET_MAX_PLAYERS];
    memset(t, 0, sizeof(t));
    t[0].pos = G.player.origin;
    t[0].valid = G.dead_time <= 0.0f && !G.player.noclip && !(g_gm.on && (g_gm.p[0].flags & GPF_NOTARGET));
    for (int s = 1; s < NET_MAX_PLAYERS && G.role == ROLE_HOST; s++) {
        const Peer *p = &G.peers[s];
        t[s].pos = p->pos;
        t[s].valid = p->active && p->health > 0.0f && !(p->flags & PF_NOCLIP) && !(g_gm.on && (g_gm.p[s].flags & GPF_NOTARGET));
    }
    // Options (the host's, in multiplayer); a gamemode's game always has them thinking
    npc_set_ai(gm_sandbox() && !g_settings.npc_think, gm_sandbox() && g_settings.npc_ignore_players);
    npc_set_blade_model(G.props_loaded ? prop_named("Sawblade") : -1);
    npcs_update(&G.map, dt, t, G.role == ROLE_HOST ? NET_MAX_PLAYERS : 1, &G.phys, npc_hits);
    npc_ragdolls_update(&G.map, dt, &G.phys);
    impact_sounds(dt);
    expire_pieces(dt);
    if (G.role == ROLE_HOST) host_send(dt);
    return 1;
}

// Achievements: playtime and who's in the game; anything newly unlocked pops up
static void achievements_tick(float dt) {
    int players = 1;
    for (int s = 0; s < NET_MAX_PLAYERS && G.role != ROLE_OFFLINE; s++) players += s != G.my_slot && G.peers[s].active;
    achieve_update(dt, players);
    for (int a; (a = achieve_pop()) >= 0;) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Achievement: %s", achieve_name(a));
        notify(msg, NOTE_ACHIEVEMENT);
        play(G.snd.spawn, 0.8f);
    }
}

static float rand01(void) {
    return (float)(rand() & 0xFFFF) / 65535.0f;
}

// Devices each frame (host and clients): gone with their prop, buttons popping back out, emitters
// puffing out particles, and the particles moving on
static void devices_update(float dt) {


    for (int d = 0; d < MAX_DEVICES; d++) {
        Device *v = &g_dev[d];
        if (!v->type) continue;
        if (v->body < 0 || v->body >= MAX_BODIES || !G.phys.bodies[v->body].active) {
            v->type = 0;
            continue;
        }
        if (v->press > 0.0f) v->press -= dt;
        if (v->type != DEV_EMITTER || !v->on) continue;
        static const float rates[EMIT_COUNT] = {40.0f, 10.0f, 30.0f};
        v->emit_acc += rates[v->effect % EMIT_COUNT] * dt;
        vec3 pos, dir;
        device_frame(v, &pos, &dir);
        while (v->emit_acc >= 1.0f) {
            v->emit_acc -= 1.0f;
            Particle *p = NULL;
            for (int k = 0; k < MAX_PARTICLES && !p; k++) {
                if (g_parts[k].life <= 0.0f) p = &g_parts[k];
            }
            if (!p) break;
            vec3 jitter = v3(rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f);
            p->effect = v->effect % EMIT_COUNT;
            p->pos = pos;
            if (p->effect == EMIT_SPARKS) {
                p->vel = v3_add(v3_scale(dir, 80.0f + 160.0f * rand01()), v3_scale(jitter, 240.0f));
                p->max_life = 0.4f + 0.4f * rand01();
                p->size = 1.2f;
            } else if (p->effect == EMIT_SMOKE) {
                p->vel = v3_add(v3_add(v3_scale(dir, 40.0f), v3_scale(jitter, 30.0f)), v3(0, 0, 30.0f));
                p->max_life = 2.0f + rand01();
                p->size = 6.0f;
            } else {
                p->vel = v3_add(v3_add(v3_scale(dir, 60.0f), v3_scale(jitter, 50.0f)), v3(0, 0, 60.0f));
                p->max_life = 0.35f + 0.3f * rand01();
                p->size = 7.0f;
            }
            p->life = p->max_life;
        }
    }
    for (int k = 0; k < MAX_PARTICLES; k++) {
        Particle *p = &g_parts[k];
        if (p->life <= 0.0f) continue;
        p->life -= dt;
        if (p->effect == EMIT_SPARKS) p->vel.z -= 600.0f * dt; // sparks fall
        p->pos = v3_mad(p->pos, p->vel, dt);
    }
}

// Single player and host: doors, buttons and triggers, touched by this PSP's player (its box, or just
// its feet while dead) and, when hosting, everyone else's
static void host_ents_update(float dt, vec3 hmin, vec3 hmax) {
    vec3 mins[NET_MAX_PLAYERS], maxs[NET_MAX_PLAYERS];
    int n = 0;
    if (!g_dedicated) {
        mins[n] = hmin;
        maxs[n++] = hmax;
    }
    for (int s = 1; s < NET_MAX_PLAYERS && G.role == ROLE_HOST; s++) {
        const Peer *p = &G.peers[s];
        if (!p->active || p->health <= 0.0f || (p->flags & PF_NOCLIP)) continue;
        mins[n] = v3_add(p->pos, v3(-16, -16, 0));
        maxs[n++] = v3_add(p->pos, v3(16, 16, (p->flags & PF_DUCKED) ? 36 : 72));
    }
    ents_update(&G.map, dt, mins, maxs, n);
}

// A dedicated server's frame: the world and the network, no player of its own
static GameResult dedicated_update(float dt) {
    phys_set_quality(g_settings.physics);
    devices_update(dt);
    host_ents_update(dt, G.player.origin, G.player.origin);
    world_step(dt);
    return GAME_CONTINUE;
}

void game_host_dedicated(void (*log)(const char *msg)) {
    g_dedicated = 1;
    g_log = log;
    G.player.noclip = 1; // out of everyone's way: not hit, not a target, touches nothing
    G.player.origin = v3(0, 0, -60000.0f);
    game_host();
}

GameResult game_update(const InputState *in, float dt) {
    if (g_gm.scripts_pending) gm_start_scripts();
    if (g_dedicated) return dedicated_update(dt);
    InputState test_in;
    if (g_test_hold || g_test_hold_prev) {
        test_in = *in;
        test_in.held |= g_test_hold;
        test_in.pressed |= g_test_hold & ~g_test_hold_prev;
        test_in.released |= g_test_hold_prev & ~g_test_hold;
        if (g_test_hold) test_in.ax = g_test_ax, test_in.ay = g_test_ay;
        g_test_hold_prev = g_test_hold;
        in = &test_in;
    }
    g_buttons = in->held;
    later_update(dt);
    gm_local_update(dt); // what the gamemode decided: a respawn, weapons, a disguise...
    items_update(dt);
    phys_set_quality(g_settings.physics);
    achievements_tick(dt);
    devices_update(dt);
    for (int i = 0; i < MAX_NOTES; i++) G.notes[i].time += dt;
    if (g_haunt.on && !G.paused) haunt_update(dt);
    G.hurt_flash -= dt;
    if (G.dead_time > 0.0f && !G.paused) {
        // Dead: the world keeps going; X (or a few seconds) respawns
        if (G.veh >= 0) vehicle_leave(1);
        voice_update(in, dt, 1); // the dead can still talk
        G.dead_time += dt;
        if (g_gm.on) {
            gm_dead_update(in); // watching the others; X respawns when the gamemode lets you
        } else if (G.dead_time > 1.0f && (in->pressed & (PSP_CTRL_CROSS | PSP_CTRL_RTRIGGER) || G.dead_time > 6.0f)) {
            respawn();
        }
        if (in->pressed & PSP_CTRL_START) open_pause();
        MoveCmd still = {0};
        still.yaw = G.yaw;
        player_move(&G.player, &G.map, &still, NULL, 0, dt);
        npc_self_set(0, -1, G.player.origin, G.yaw, dt);
        if (is_client()) {
            ents_client_update(&G.map, dt);
        } else {
            host_ents_update(dt, G.player.origin, G.player.origin);
        }
        if (!world_step(dt)) {
            net_leave();
            return GAME_EXIT_TO_MENU;
        }
        return GAME_CONTINUE;
    }
    G.wsel_time -= dt;
    G.beam_time -= dt;
    G.switch_time -= dt;
    G.kick *= 1.0f - clampf(dt * 12.0f, 0.0f, 1.0f);
    vm_tick(dt);
    MoveCmd cmd = {0};

    if (G.paused) {
        // The world is frozen while paused, like single-player GMod; multiplayer keeps going
        voice_update(in, dt, 0);
        GameResult r = pause_update(in);
        if (r != GAME_CONTINUE || G.role == ROLE_OFFLINE) return r;
    } else if (G.qmenu) {
        voice_update(in, dt, 0);
        qmenu_update(in);
    } else {
        voice_update(in, dt, 1);

        // Quick chat: hold D-pad Down for the wheel. While it's up the stick picks and L / R turn the page,
        // so nothing else gets them this frame (a tap of Down still crouches: g_qc.tap)
        InputState qc_in;
        if (quick_chat_update(in, dt)) {
            const unsigned int mine = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_UP | PSP_CTRL_DOWN |
                                      PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS;
            qc_in = *in;
            qc_in.ax = qc_in.ay = 0.0f;
            qc_in.held &= ~mine;
            qc_in.pressed &= ~mine;
            qc_in.released &= ~mine;
            in = &qc_in;
        }

        if (in->pressed & PSP_CTRL_START) {
            physgun_release(1);
            open_pause();
            return GAME_CONTINUE;
        }
        // Select: tap for the spawn menu, hold for noclip (a gamemode's game: held, it shows the scoreboard)
        if (!gm_sandbox()) {
            G.select_time = 0.0f;
            G.select_fired = 0;
        } else if (in->held & PSP_CTRL_SELECT) {
            G.select_time += dt;
            if (G.select_time >= SELECT_HOLD_TIME && !G.select_fired) {
                G.select_fired = 1;
                G.player.noclip = !G.player.noclip;
                G.player.velocity = v3(0, 0, 0);
                notify(G.player.noclip ? "Noclip enabled" : "Noclip disabled", NOTE_HINT);
            }
        } else {
            if ((in->released & PSP_CTRL_SELECT) && !G.select_fired) {
                G.qmenu = 1;
                play(G.snd.menu_select, 0.5f);
            }
            G.select_time = 0.0f;
            G.select_fired = 0;
        }
        float invert = g_settings.invert_y ? -1.0f : 1.0f;
#ifdef PS2
        if (in->pressed & PSP_CTRL_SQUARE) {
            g_flashlight = !g_flashlight;
            play(G.snd.menu_move, 0.6f);
        }
#else
        // D-pad Right: tap to use, hold to switch the flashlight (Square is a look button on the PSP)
        if ((in->held & PSP_CTRL_RIGHT) && G.held < 0 && G.held_rag < 0) {
            g_flash_hold += dt;
            if (g_flash_hold >= SELECT_HOLD_TIME && !g_flash_fired) {
                g_flash_fired = 1;
                g_flashlight = !g_flashlight;
                play(G.snd.menu_move, 0.6f);
                notify(g_flashlight ? "Flashlight on" : "Flashlight off", NOTE_HINT);
            }
        } else {
            g_flash_hold = 0.0f;
            g_flash_fired = 0;
        }
#endif
        int rotating = physgun_rotating(in); // X and the stick turn the held prop instead
#ifdef PS2
        // DualShock 2: look with the right stick (squared for fine aim near the centre)
        {
            float k = 0.6f + 0.2f * g_settings.look_speed;
            float rx = in->rx * fabsf(in->rx), ry = in->ry * fabsf(in->ry);
            G.yaw -= LOOK_YAW_SPEED * 1.2f * k * dt * rx;
            G.pitch -= invert * LOOK_PITCH_SPEED * 1.2f * k * dt * ry;
        }
        (void)look_rate;
#else
        G.yaw += LOOK_YAW_SPEED * dt * (look_rate(0, in->held & PSP_CTRL_SQUARE, dt) - look_rate(1, in->held & PSP_CTRL_CIRCLE, dt));
        G.pitch += invert * LOOK_PITCH_SPEED * dt *
                   (look_rate(2, in->held & PSP_CTRL_TRIANGLE, dt) - look_rate(3, (in->held & PSP_CTRL_CROSS) && !rotating, dt));
#endif
        G.pitch = clampf(G.pitch, -89.0f, 89.0f);
        G.yaw = fmodf(G.yaw, 360.0f);

        if (G.veh >= 0) {
            vehicle_controls(in, dt); // driving: the stick steers, Use gets out
        } else {
            cmd.forward = rotating ? 0.0f : g_test_walk != 0.0f ? g_test_walk : -in->ay;
            cmd.side = rotating ? 0.0f : in->ax;
            cmd.yaw = G.yaw;
            cmd.pitch = G.pitch;
            int holding = G.held >= 0 || G.held_rag >= 0;
    #ifdef PS2
            // X jumps on the DualShock (and L1, except with the gravity gun or the Camera tool, where L1 is theirs), except with the gravity gun or the Camera tool, where L1 is theirs)
            cmd.jump = !holding && (in->held & (G.weapon == WEAPON_GRAVGUN || camera_tool_out() ? PSP_CTRL_CROSS
                                                                                                : PSP_CTRL_LTRIGGER | PSP_CTRL_CROSS));
    #else
            cmd.jump = !holding && !camera_tool_out() && (in->held & PSP_CTRL_LTRIGGER); // the Camera tool's L switches view
    #endif
            if (!camera_tool_out() || !is_camera(G.cam_view)) G.cam_view = -1; // putting the tool away ends the camera view
            if (G.player.noclip) {
                cmd.duck = (in->held & PSP_CTRL_DOWN) && !holding;
            } else if (g_qc.tap && !holding) {
                // Crouch is a toggle (the thumb can't hold it and steer), on a tap: holding opens quick chat
                G.player.ducked = !G.player.ducked;
            }
            if (!G.player.noclip) cmd.duck = G.player.ducked;
            gm_move(&cmd); // a gamemode's game: frozen, slower or faster, disguised (no crouching)

            // Reload (GMod's R): Circle on the DualShock, which the right stick frees up; on the PSP, hold
            // D-pad Left (a tap still switches weapons, when it's let go)
            int next_weapon = 0;
    #ifdef PS2
            next_weapon = !holding && (in->pressed & PSP_CTRL_LEFT);
            if (!holding && (in->pressed & PSP_CTRL_CIRCLE)) start_reload();
    #else
            if (in->held & PSP_CTRL_LEFT) {
                G.left_hold += dt;
                if (G.left_hold >= SELECT_HOLD_TIME && !G.left_fired) {
                    G.left_fired = 1;
                    start_reload();
                }
            } else {
                next_weapon = !holding && (in->released & PSP_CTRL_LEFT) && !G.left_fired;
                G.left_hold = 0.0f;
                G.left_fired = 0;
            }
    #endif
            if (next_weapon) {
                G.weapon = gm_next_weapon(G.weapon); // what a gamemode lets you carry
                G.reload_left = 0.0f; // switching away cancels a reload
                G.switch_time = 0.25f;
                G.wsel_time = 1.5f;
                G.stage = 0;
                play(G.snd.menu_move, 0.4f);
            }
            // Use key: switch a prop's thrusters and hoverballs, else pick up something light, else use
            // whatever's there (doors, buttons). Only the host knows which props have thrusters.
            int use_pressed = !holding && (in->pressed & use_buttons());
            Aim use_aim;
            use_aim.body = -1;
            if (use_pressed) use_aim = aim_trace(1500.0f);
            // A vehicle or seat within reach: get in
            int vehicle = use_aim.body >= 0 && use_aim.dist < 160.0f &&
                          vehicle_kind(&G.phys.bodies[use_aim.body]) != VEH_NONE;
            // A health or suit charger within reach: charge from it (Use again stops)
            if (!vehicle && use_aim.body >= 0 && use_aim.dist < CHARGE_RANGE &&
                is_charger(item_kind(use_aim.body))) {
                charger_use(use_aim.body);
                use_aim.body = -1;
                use_pressed = 0;
            }
            // A gamemode's game: its own use first (Prop Hunt: becoming the prop you look at), then doors and
            // buttons; light props are picked up only where it allows that
            if (!vehicle && use_pressed && !gm_sandbox()) {
                if (!((g_gm.flags & GMF_PICKUP) && use_pickup())) {
                    gm_use(use_aim.body >= 0 && use_aim.dist < GM_USE_RANGE ? use_aim.body : -1);
                }
                use_pressed = 0;
                use_aim.body = -1;
            }
            int has_forces = !vehicle && use_aim.body >= 0 && !is_client() && toggle_forces(use_aim.body);
            // A client knows the lamps, buttons... (PK_DEVICES): those are used, not picked up
            int client_device = !vehicle && use_aim.body >= 0 && is_client() && body_has_device(use_aim.body);
            if (vehicle) {
                vehicle_enter(use_aim.body);
                use_aim.body = -1;
            } else if (has_forces) {
                play(G.snd.menu_move, 0.5f);
            } else if (client_device) {
                send_cmd(CMD_TOGGLE, use_aim.body, 0, 0, NULL, 0);
                play(G.snd.menu_move, 0.5f);
            } else if (use_pressed && use_pickup()) {
                // carrying it now (use_carry_update)
            } else if (use_aim.body >= 0 && is_client()) {
                send_cmd(CMD_TOGGLE, use_aim.body, 0, 0, NULL, 0);
                play(G.snd.menu_move, 0.5f);
            } else if (use_pressed) {
                vec3 eye = player_eye(&G.player), f = view_forward();
                if (is_client()) {
                    float v[6] = {eye.x, eye.y, eye.z, f.x, f.y, f.z};
                    send_cmd(CMD_USE, -1, 0, 0, v, 6);
                } else if (!ents_use(&G.map, eye, f) && !script_use(G.my_slot, use_aim.body)) {
                    play(G.snd.dryfire, 0.3f);
                }
            }
            if (!holding && G.up_undo && is_client()) {
                send_cmd(CMD_UNDO, 0, 0, 0, NULL, 0);
                play(G.snd.undo, 0.6f);
                notify("Undone", NOTE_UNDO);
            } else if (!holding && G.up_undo) {
                int r = undo_pop(0);
                if (r) {
                    play(G.snd.undo, 0.6f);
                    notify(r == 4 ? "Undone Build" : r == 3 ? "Undone Ragdoll" : r == 2 ? "Undone NPC" : "Undone Prop",
                           NOTE_UNDO);
                }
            }
            if (G.switch_time <= 0.0f) {
                if (G.use_carry) {
                    use_carry_update(in, dt); // carrying with the use key takes over from the weapon
                } else if (G.weapon == WEAPON_PHYSGUN) {
                    physgun_update(in, dt);
                } else if (G.weapon == WEAPON_GRAVGUN || G.weapon == WEAPON_HANDS) {
                    carry_update(in, dt);
                } else if (G.weapon == WEAPON_TOOLGUN) {
                    if (in->pressed & PSP_CTRL_RTRIGGER) toolgun_fire();
                    if (G.tool == TOOL_CAMERA && (in->pressed & PSP_CTRL_LTRIGGER)) camera_switch();
                    if (G.tool == TOOL_EMITTER && (in->pressed & PSP_CTRL_LTRIGGER)) {
                        g_emit_effect = (g_emit_effect + 1) % EMIT_COUNT;
                        char msg[32];
                        snprintf(msg, sizeof(msg), "Emitter: %s", k_emit_names[g_emit_effect]);
                        toast(msg);
                        play(G.snd.menu_move, 0.5f);
                    }
                    if ((G.tool == TOOL_FACEPOSER || G.tool == TOOL_FINGERPOSER) && (in->pressed & PSP_CTRL_LTRIGGER)) {
                        char msg[40];
                        if (G.tool == TOOL_FACEPOSER) {
                            g_face_sel = (g_face_sel + 1) % npc_face_count();
                            snprintf(msg, sizeof(msg), "Face: %s", npc_face_name(g_face_sel));
                        } else {
                            g_hand_sel = (g_hand_sel + 1) % npc_hand_count();
                            snprintf(msg, sizeof(msg), "Hand: %s", npc_hand_name(g_hand_sel));
                        }
                        toast(msg);
                        play(G.snd.menu_move, 0.5f);
                    }
                } else {
                    gun_update(in, dt);
                }
            }
        }
    }
    G.fire_cd -= dt;
    G.muzzle_time -= dt;
    G.blast_time -= dt;
    for (int i = 0; i < 8; i++) G.tracers[i].time -= dt;
    blood_update(dt);

    CmBox boxes[MAX_BODIES + MAX_NPCS + NET_MAX_PLAYERS];
    int nb = build_prop_boxes(boxes, G.held);
    if (G.veh < 0) player_move(&G.player, &G.map, &cmd, boxes, nb, dt); // in a vehicle, it carries you
    // Your body, in third person or seen from a camera (not while driving: it would stand up through the roof)
    npc_self_set((third_person() || G.cam_view >= 0) && G.veh < 0, player_model(), G.player.origin, G.yaw, dt);
    npc_puppet_tint(-1, body_tint(g_settings.player_colour));
    gm_puppet(-1);

    vec3 pmin, pmax;
    player_hull(&G.player, &pmin, &pmax);
    pmin = v3_add(G.player.origin, pmin);
    pmax = v3_add(G.player.origin, pmax);
    if (is_client()) {
        ents_client_update(&G.map, dt);
    } else {
        host_ents_update(dt, pmin, pmax);
    }

    // Walking into props shoves them (the host's physics decides that in multiplayer)
    if (!G.player.noclip && !is_client()) {
        vec3 smin = v3_sub(pmin, v3(3, 3, 0)), smax = v3_add(pmax, v3(3, 3, 0));
        float pspeed = sqrtf(G.player.velocity.x * G.player.velocity.x + G.player.velocity.y * G.player.velocity.y);
        for (int i = 0; i < MAX_BODIES && pspeed > 20.0f; i++) {
            Body *b = &G.phys.bodies[i];
            if (!b->active || b->frozen || b->held) continue;
            vec3 bmin, bmax;
            phys_body_aabb(b, &bmin, &bmax);
            if (bmin.x > smax.x || bmax.x < smin.x || bmin.y > smax.y || bmax.y < smin.y || bmin.z > smax.z - 18.0f ||
                bmax.z < smin.z + 2.0f)
                continue;
            vec3 d = v3_sub(b->pos, G.player.origin);
            d.z = 0.0f;
            d = v3_norm(d);
            float push = v3_dot(G.player.velocity, d);
            if (push <= 0.0f) continue;
            float k = clampf(60.0f / b->mass, 0.05f, 1.0f);
            phys_wake(&G.phys, i);
            b->vel = v3_mad(b->vel, d, push * k * 0.5f);
        }
    }

    // Fell out of the map
    if (G.player.origin.z < G.map.info->mins[2] - 512.0f) {
        const PmapSpawn *sp = &G.map.spawns[0];
        player_spawn(&G.player, v3(sp->x, sp->y, sp->z));
    }

    snd_set_listener(player_eye(&G.player), G.yaw);
    fall_damage();
    player_sounds(dt);
    if (!world_step(dt)) {
        net_leave();
        return GAME_EXIT_TO_MENU;
    }
    vehicle_place_player(); // after the physics step, so the view rides along without lagging a frame
    return GAME_CONTINUE;
}

// --- Gamemodes -------------------------------------------------------------------------------------
// A gamemode (script.c: Lua from data/lua/gamemodes, or one of the game's own in psp/lua) turns a map into
// a game with rules: teams, rounds, loadouts and respawns. Its rules run where the world does (single
// player, the host, a dedicated server); every PSP with a player runs its HUD. What the rules decide about
// each player is kept in g_gm and goes to everyone in PK_GMSTATE, with the gamemode's networked variables;
// things that happen once (a message across the middle of the screen, a sound, some ammo) go out as
// PK_GMEVENTS. Each PSP then applies its own player's part: where to (re)spawn, what it may carry, how fast
// it walks, whether it's frozen, blinded or disguised as a prop.

#define GM_STATE_INTERVAL 0.25f
#define GM_EVENT_RESENDS 3  // frames a new event goes out in (then once a second, with the others)
#define GM_SPAWN_GRACE 2.0f // a client waits this long at most for the host's alive snapshot after a respawn

typedef struct {
    uint8_t team, flags, model, speed; // model: NET_NO_MODEL for their own; speed x 1/64
    uint16_t disguise, weapons;
    uint8_t spawn_seq, spawn_move, select_seq, select_weapon;
    uint16_t max_health, pad;
    float spawn_pos[3], spawn_yaw;
} NetGmPlayer;

typedef struct {
    NetHeader h;
    char mode[NET_MODE_LEN];
    uint8_t flags, respawn_time, pad[2]; // respawn_time: seconds before X respawns (255: the gamemode decides)
    NetGmPlayer p[NET_MAX_PLAYERS];
    uint16_t nw_len, pad2;
    char nw[GM_NW_MAX];
} PkGmState;

typedef struct {
    NetHeader h;
    uint8_t count, pad;
    uint16_t latest; // the newest event so far (a client that's just joined starts from it)
    GmEvent ev[GM_EVENTS];
} PkGmEvents;
_Static_assert(sizeof(PkGmState) <= 1400 && sizeof(PkGmEvents) <= 1400, "packets must fit one datagram");

// Scripts name the weapons like this (ply:Give("smg"))
static const char *const k_weapon_ids[WEAPON_COUNT] = {"hands",   "physgun", "gravgun", "toolgun",  "crowbar",
                                                       "pistol",  "357",     "smg",     "shotgun",  "ar2",
                                                       "crossbow", "rpg",    "grenade", "bugbait"};
static int g_gm_spawning; // inside gm_spawn: a PlayerSpawn hook asking for another spawn is ignored

void game_set_gamemode(const char *name) {
    snprintf(g_next_mode, sizeof(g_next_mode), "%s", name && strcmp(name, "sandbox") != 0 ? name : "");
}

const char *game_gamemode(void) {
    return g_gm.mode;
}

int game_is_server(void) {
    return !is_client();
}

int game_is_dedicated(void) {
    return g_dedicated;
}

static int gm_slot_ok(int s) {
    return s >= 0 && s < NET_MAX_PLAYERS;
}

// Someone plays in slot s (this PSP's own player too, except on a dedicated server)
static int pl_active(int s) {
    if (!gm_slot_ok(s)) return 0;
    if (s == G.my_slot) return !g_dedicated;
    return G.role != ROLE_OFFLINE && G.peers[s].active;
}

static float pl_health(int s) {
    return s == G.my_slot ? G.health : G.peers[s].health;
}

static vec3 pl_feet(int s) {
    return s == G.my_slot ? G.player.origin : G.peers[s].pos;
}

static unsigned int gm_default_weapons(void) {
    return gm_sandbox() ? ALL_WEAPONS : 1u << WEAPON_HANDS;
}

static void gm_reset_player(int s) {
    GmPlayer *g = &g_gm.p[s];
    int serial = g->serial;
    memset(g, 0, sizeof(*g));
    g->serial = serial + 1;
    g->model = -1;
    g->speed = 1.0f;
    g->disguise = GM_NO_DISGUISE;
    g->weapons = gm_default_weapons();
    g->max_health = 100.0f;
    g->attacker = -1;
}

static void gm_reset(void) {
    memset(&g_gm, 0, sizeof(g_gm));
    snprintf(g_gm.mode, sizeof(g_gm.mode), "%s", g_next_mode);
    g_gm.on = g_gm.mode[0] != 0;
    g_gm.respawn_time = -1.0f;
    g_gm.spectate = -1;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) gm_reset_player(s);
    g_gm.scripts_pending = 1;
    g_npc_attacker = -1;
}

// The first frame: by now it's known whether this PSP hosts, joined someone or plays alone
static void gm_start_scripts(void) {
    g_gm.scripts_pending = 0;
    if (!script_start(G.map_name, g_gm.mode, !is_client(), !g_dedicated) && g_gm.on && !is_client()) {
        // No such gamemode here: the plain sandbox, then
        char msg[48];
        snprintf(msg, sizeof(msg), "No gamemode \"%.16s\": playing Sandbox", g_gm.mode);
        notify(msg, NOTE_ERROR);
        g_gm.on = 0;
        g_gm.mode[0] = 0;
        script_start(G.map_name, "", 1, !g_dedicated);
    }
}

void game_gm_setup(int flags, float respawn_time) {
    if (!g_gm.on) return;
    g_gm.flags = flags;
    g_gm.respawn_time = respawn_time;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        if (!g_gm.p[s].known) g_gm.p[s].weapons = gm_default_weapons();
    }
    g_gm.dirty = 1;
}

// The prop a disguise names (model | lib << 15), or NULL
static const PpakModel *disguise_model(int code, const PropLib **lib_out, int *local) {
    if (code == GM_NO_DISGUISE) return NULL;
    int model = code & 0x7FFF;
    const PropLib *lib;
    if (code >> 15) {
        if (!G.maplib_loaded || model >= G.maplib.num_models) return NULL;
        lib = &G.maplib;
        *local = model;
    } else {
        lib = G.props_loaded ? model_lib(model, local) : NULL;
        if (!lib) return NULL;
    }
    if (lib_out) *lib_out = lib;
    return &lib->models[*local];
}

static int gm_disguised(int s) {
    int local;
    return g_gm.on && gm_slot_ok(s) && disguise_model(g_gm.p[s].disguise, NULL, &local) != NULL;
}

// A disguised player's box: as wide and as tall as the prop, up to a person's size
static int gm_disguise_box(int s, float *half, float *height) {
    int local;
    const PpakModel *md = g_gm.on && gm_slot_ok(s) ? disguise_model(g_gm.p[s].disguise, NULL, &local) : NULL;
    if (!md) return 0;
    float hx = (md->maxs[0] - md->mins[0]) * 0.5f, hy = (md->maxs[1] - md->mins[1]) * 0.5f;
    *half = clampf(fmaxf(hx, hy), 4.0f, 16.0f);
    *height = clampf(md->maxs[2] - md->mins[2], 8.0f, 72.0f);
    return 1;
}

// What shots hit: a person's box, or a disguise's prop (all of it, turned as they face)
static void player_box(int s, vec3 feet, int ducked, vec3 *lo, vec3 *hi) {
    int local;
    const PpakModel *md = g_gm.on && gm_slot_ok(s) ? disguise_model(g_gm.p[s].disguise, NULL, &local) : NULL;
    if (!md) {
        *lo = v3_add(feet, v3(-16, -16, 0));
        *hi = v3_add(feet, v3(16, 16, ducked ? 36.0f : 72.0f));
        return;
    }
    float yaw = (s == G.my_slot ? G.yaw : G.peers[s].yaw) * DEG2RAD;
    float c = fabsf(cosf(yaw)), sn = fabsf(sinf(yaw));
    float hx = (md->maxs[0] - md->mins[0]) * 0.5f, hy = (md->maxs[1] - md->mins[1]) * 0.5f;
    float ex = c * hx + sn * hy, ey = sn * hx + c * hy;
    *lo = v3_add(feet, v3(-ex, -ey, 0));
    *hi = v3_add(feet, v3(ex, ey, md->maxs[2] - md->mins[2]));
}

// Other players' characters (slot -1: your own body): hidden and prop-sized while disguised
static void gm_puppet(int s) {
    float half = 0.0f, height = 0.0f;
    int on = gm_disguise_box(s < 0 ? G.my_slot : s, &half, &height);
    npc_puppet_disguise(s, on, half, height);
}

// Name tags in a team game show only team mates', and nobody sees a disguised enemy's
static int gm_tag_visible(int s) {
    if (!g_gm.on || g_dedicated) return 1;
    int mine = g_gm.p[G.my_slot].team, theirs = g_gm.p[s].team;
    int team_mate = mine != 0 && mine == theirs;
    if (gm_disguised(s) || (g_gm.flags & GMF_TEAM_TAGS)) return team_mate;
    return 1;
}

// Somewhere near p that's clear of the walls and of other players (maps often have just the one spawn)
static vec3 gm_free_spot(vec3 p, int skip) {
    for (int k = 0; k < 9; k++) {
        vec3 q = p;
        if (k) {
            float a = (float)k * (PI / 4.0f);
            q = v3_add(p, v3(cosf(a) * 56.0f, sinf(a) * 56.0f, 0.0f));
            Trace tr;
            vec3 up = v3_add(q, v3(0, 0, 4));
            cm_trace(&G.map, up, up, v3(-16, -16, 0), v3(16, 16, 72), MASK_PLAYERSOLID, NULL, 0, &tr);
            if (tr.startsolid) continue;
            cm_trace_line(&G.map, v3_add(p, v3(0, 0, 36)), v3_add(q, v3(0, 0, 36)), MASK_PLAYERSOLID, NULL, 0, &tr);
            if (tr.fraction < 1.0f) continue;
        }
        int taken = 0;
        for (int o = 0; o < NET_MAX_PLAYERS && !taken; o++) {
            if (o == skip || !pl_active(o) || pl_health(o) <= 0.0f) continue;
            vec3 d = v3_sub(pl_feet(o), q);
            taken = d.x * d.x + d.y * d.y < 40.0f * 40.0f && fabsf(d.z) < 72.0f;
        }
        if (!taken) return q;
    }
    return p;
}

// Where the rules run: (re)spawns player s at the gamemode's spawn point (or a random one of the map's),
// with full health, and has the gamemode give them their loadout
static void gm_spawn(int s) {
    if (is_client() || !pl_active(s) || g_gm_spawning) return;
    g_gm_spawning = 1;
    GmPlayer *g = &g_gm.p[s];
    vec3 pos;
    float yaw;
    if (!script_select_spawn(s, &pos, &yaw)) {
        int n = G.map.num_spawns > 0 ? G.map.num_spawns : 1;
        const PmapSpawn *sp = &G.map.spawns[rand() % n];
        pos = v3(sp->x, sp->y, sp->z);
        yaw = sp->yaw;
    }
    pos = gm_free_spot(pos, s);
    g->spawn_pos = pos;
    g->spawn_yaw = yaw;
    g->spawn_move = 0;
    g->spawn_seq = g->spawn_seq % 255 + 1;
    g->alive = 1;
    g->dead_for = 0.0f;
    g->attacker = -1;
    if (s == G.my_slot) {
        G.health = g->max_health;
    } else {
        Peer *p = &G.peers[s];
        p->health = g->max_health;
        p->pos = pos;
        p->flags &= ~PF_DEAD;
    }
    g_gm.dirty = 1;
    script_player_spawn(s);
    g_gm_spawning = 0;
}

static void gm_respawn_request(int s) {
    if (!g_gm.on || g_gm.respawn_time < 0.0f || !pl_active(s) || pl_health(s) > 0.0f) return;
    if (g_gm.p[s].dead_for + 0.1f < fmaxf(1.0f, g_gm.respawn_time)) return;
    gm_spawn(s);
}

// Where the rules run, each frame: tells the gamemode who joined (and spawns them), died and left
static void gm_server_update(float dt) {
    if (!g_gm.on) return;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        GmPlayer *g = &g_gm.p[s];
        int act = pl_active(s);
        uint32_t nonce = s == G.my_slot ? 1u : G.peers[s].nonce;
        if (g->known && (!act || g->nonce != nonce)) {
            script_player_leave(s);
            gm_reset_player(s);
            g_gm.dirty = 1;
        }
        if (!act) continue;
        if (!g->known) {
            g->known = 1;
            g->nonce = nonce;
            g->alive = pl_health(s) > 0.0f;
            script_player_join(s);
            if (pl_health(s) > 0.0f) gm_spawn(s); // unless the gamemode has them watch for now (KillSilent)
            continue;
        }
        int alive = pl_health(s) > 0.0f;
        if (g->alive && !alive) {
            g->dead_for = 0.0f;
            script_player_death(s, g->attacker);
        }
        g->alive = alive;
        if (!alive) g->dead_for += dt;
    }
}

static void gm_npc_died(int npc, int type, vec3 pos) {
    if (!is_client()) script_npc_killed(npc, type, g_npc_attacker);
}

// --- This PSP's player: what the gamemode decided for it

static unsigned int gm_carried(void) {
    return g_gm.on && !g_dedicated ? g_gm.p[G.my_slot].weapons | 1u << WEAPON_HANDS
                                   : g_sandbox_weapons | 1u << WEAPON_HANDS;
}

// A (re)spawn in the sandbox: every weapon, or the toolbox (Options > Spawn with weapons)
static void sandbox_loadout(void) {
    g_sandbox_weapons = g_settings.spawn_weapons ? ALL_WEAPONS : TOOLBOX;
    if (!(gm_carried() & (1u << G.weapon))) G.weapon = WEAPON_PHYSGUN;
}

static void gm_switch_weapon(int w) {
    if (w == (int)G.weapon) return;
    if (G.held >= 0 || G.held_rag >= 0) physgun_release(1);
    G.weapon = (Weapon)w;
    G.reload_left = 0.0f;
    G.switch_time = 0.25f;
    G.wsel_time = 1.5f;
    G.stage = 0;
}

// What to hold when the weapon in hand is taken away: the best gun left
static int gm_best_weapon(unsigned int mask) {
    static const int order[] = {WEAPON_SMG,     WEAPON_AR2,      WEAPON_SHOTGUN, WEAPON_PISTOL,  WEAPON_357,
                                WEAPON_CROSSBOW, WEAPON_RPG,     WEAPON_CROWBAR, WEAPON_GRENADE, WEAPON_GRAVGUN,
                                WEAPON_PHYSGUN, WEAPON_TOOLGUN, WEAPON_BUGBAIT};
    for (int k = 0; k < (int)(sizeof(order) / sizeof(order[0])); k++) {
        if (mask & (1u << order[k])) return order[k];
    }
    return WEAPON_HANDS;
}

static int gm_next_weapon(int w) {
    unsigned int mask = gm_carried();
    for (int k = 1; k <= WEAPON_COUNT; k++) {
        int n = (w + k) % WEAPON_COUNT;
        if (mask & (1u << n)) return n;
    }
    return w;
}

static void gm_local_update(float dt) {
    g_gm.center_time -= dt;
    g_gm.respawn_asked -= dt;
    if (!g_gm.on && g_settings.spawn_weapons) g_sandbox_weapons = ALL_WEAPONS; // switched on in Options
    if (!g_gm.on || g_dedicated) return;
    GmPlayer *g = &g_gm.p[G.my_slot];
    if (g_gm.spawn_grace > 0.0f) g_gm.spawn_grace -= dt;
    if (g->spawn_seq && g->spawn_seq != g_gm.seen_spawn) {
        g_gm.seen_spawn = g->spawn_seq;
        if (G.veh >= 0) vehicle_leave(1);
        if (g->spawn_move) {
            G.player.origin = g->spawn_pos;
            G.player.velocity = v3(0, 0, 0);
            G.yaw = g->spawn_yaw;
        } else {
            physgun_release(1);
            stop_charging(0);
            player_spawn(&G.player, g->spawn_pos);
            G.yaw = g->spawn_yaw;
            G.pitch = 0.0f;
            G.health = g->max_health;
            G.suit = 100.0f;
            G.dead_time = 0.0f;
            G.hurt_flash = 0.0f;
            G.qmenu = 0;
            refill_ammo();
            G.respawn_pending = 0;
            if (is_client()) g_gm.spawn_grace = GM_SPAWN_GRACE;
        }
    }
    // Weapons: a new one comes with its ammo; one taken away leaves the best one left in your hands
    unsigned int mask = gm_carried(), added = mask & ~g_gm.had_weapons;
    for (int w = 0; w < WEAPON_COUNT; w++) {
        if (!(added & (1u << w))) continue;
        G.clip[w] = k_ws[w].clip;
        G.ammo[w] = k_ws[w].ammo;
    }
    g_gm.had_weapons = mask;
    if (g->select_seq != g_gm.seen_select) {
        g_gm.seen_select = g->select_seq;
        if (mask & (1u << g->select_weapon)) gm_switch_weapon(g->select_weapon);
    }
    if (!(mask & (1u << G.weapon))) gm_switch_weapon(gm_best_weapon(mask));
    if (!gm_sandbox()) {
        G.player.noclip = 0;
        G.qmenu = 0;
    }
    // Disguised: a box the size of the prop
    float half = 0.0f, height = 0.0f;
    gm_disguise_box(G.my_slot, &half, &height);
    G.player.hull_half = half;
    G.player.hull_height = height;
}

static void gm_move(MoveCmd *cmd) {
    if (!g_gm.on || g_dedicated) return;
    const GmPlayer *g = &g_gm.p[G.my_slot];
    if (g->flags & GPF_FROZEN) {
        cmd->forward = cmd->side = 0.0f;
        cmd->jump = 0;
    }
    cmd->speed = g->speed;
    if (G.player.hull_height > 0.0f) {
        cmd->duck = 0;
        G.player.ducked = 0;
    }
}

static void gm_use(int body) {
    vec3 eye = player_eye(&G.player), f = view_forward();
    if (is_client()) {
        float v[6] = {eye.x, eye.y, eye.z, f.x, f.y, f.z};
        send_cmd(CMD_USE, body, 0, 0, v, 6);
    } else if (!script_use(G.my_slot, body) && !ents_use(&G.map, eye, f)) {
        play(G.snd.dryfire, 0.3f);
    }
}

static int gm_spectate_ok(int s) {
    return s >= 0 && s != G.my_slot && pl_active(s) && pl_health(s) > 0.0f;
}

// Dead in a gamemode's game: L / R pick whose view to watch; X respawns once the gamemode lets you
static void gm_dead_update(const InputState *in) {
    int dir = (in->pressed & PSP_CTRL_RTRIGGER) ? 1 : (in->pressed & PSP_CTRL_LTRIGGER) ? NET_MAX_PLAYERS - 1 : 0;
    if (dir || !gm_spectate_ok(g_gm.spectate)) {
        int from = g_gm.spectate < 0 ? 0 : g_gm.spectate, step = dir ? dir : 1;
        int pick = -1;
        for (int k = dir ? 1 : 0; k <= NET_MAX_PLAYERS && pick < 0; k++) {
            int s = (from + step * k) % NET_MAX_PLAYERS;
            if (gm_spectate_ok(s)) pick = s;
        }
        if (pick != g_gm.spectate && dir) play(G.snd.menu_move, 0.4f);
        g_gm.spectate = pick;
    }
    if (g_gm.respawn_time >= 0.0f && G.dead_time > fmaxf(1.0f, g_gm.respawn_time) && (in->pressed & PSP_CTRL_CROSS)) {
        if (!is_client()) {
            gm_respawn_request(G.my_slot);
        } else if (g_gm.respawn_asked <= 0.0f) {
            send_cmd(CMD_RESPAWN, 0, 0, 0, NULL, 0);
            g_gm.respawn_asked = 1.0f;
        }
    }
}

static void gm_spectate_camera(void) {
    if (!g_gm.on || G.dead_time < 1.5f || !gm_spectate_ok(g_gm.spectate)) return;
    int s = g_gm.spectate;
    vec3 feet;
    float yaw;
    if (!npc_puppet_pos(s, &feet, &yaw)) {
        feet = G.peers[s].pos;
        yaw = G.peers[s].yaw;
    }
    vec3 head = v3_add(feet, v3(0, 0, 56));
    float cy = cosf(yaw * DEG2RAD), sy = sinf(yaw * DEG2RAD);
    vec3 want = v3_add(head, v3(-cy * 110.0f, -sy * 110.0f, 30.0f));
    Trace tr;
    cm_trace(&G.map, head, want, v3(-4, -4, -4), v3(4, 4, 4), MASK_PLAYERSOLID, NULL, 0, &tr);
    G.cam.pos = tr.startsolid ? head : tr.endpos;
    G.cam.yaw = yaw;
    G.cam.pitch = -14.0f;
}

// --- Messages, sounds and ammo: events

static void gm_apply_event(const GmEvent *e) {
    if (e->target != 255 && (g_dedicated || e->target != G.my_slot)) return;
    char text[sizeof(e->text) + 1];
    snprintf(text, sizeof(text), "%.*s", (int)sizeof(e->text), e->text);
    int acting = G.acting;
    G.acting = 0; // notify() and play() leave out remote players' actions; these are for this screen
    switch (e->kind) {
    case GEV_CHAT:
        notify(text, NOTE_CHAT);
        break;
    case GEV_CENTER:
        if (g_dedicated) {
            if (g_log) g_log(text);
            break;
        }
        snprintf(g_gm.center, sizeof(g_gm.center), "%s", text);
        g_gm.center_time = e->arg > 0 ? (float)e->arg : 4.0f;
        break;
    case GEV_SOUND: {
        int id = g_dedicated ? -1 : snd_find(text);
        if (id < 0) break;
        if (e->arg2) {
            play_at(id, v3(e->pos[0], e->pos[1], e->pos[2]), 1.0f);
        } else {
            play(id, 0.9f);
        }
        break;
    }
    case GEV_AMMO:
        if (g_dedicated) break;
        for (int w = 0; w < WEAPON_COUNT; w++) {
            if (!k_ws[w].clip) continue; // melee and bug bait: nothing to give
            if (e->arg == 255 && (gm_carried() & (1u << w))) {
                G.clip[w] = k_ws[w].clip;
                if (G.ammo[w] < k_ws[w].ammo) G.ammo[w] = k_ws[w].ammo;
            } else if (e->arg == w) {
                G.ammo[w] = G.ammo[w] + e->arg2 > 9999 ? 9999 : G.ammo[w] + e->arg2;
            }
        }
        break;
    default:
        break;
    }
    G.acting = acting;
}

// Where the rules run: something for player target's PSP (255: everyone's), this one's own included
static void gm_event(int target, int kind, int arg, int arg2, const vec3 *pos, const char *text) {
    if (is_client()) return;
    uint16_t seq = ++g_gm.ev_seq;
    if (!seq) seq = ++g_gm.ev_seq; // 0 marks an empty slot
    GmEvent *e = &g_gm.ev[seq % GM_EVENTS];
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->kind = (uint8_t)kind;
    e->target = (uint8_t)target;
    e->arg = (int16_t)arg;
    e->arg2 = (int16_t)arg2;
    if (pos) {
        e->pos[0] = pos->x;
        e->pos[1] = pos->y;
        e->pos[2] = pos->z;
    }
    if (text) snprintf(e->text, sizeof(e->text), "%s", text);
    g_gm.ev_sends = GM_EVENT_RESENDS;
    gm_apply_event(e);
}

static void gm_send_state(void) {
    static PkGmState pk;
    memset(&pk, 0, sizeof(pk));
    pk.h = net_header(PK_GMSTATE);
    snprintf(pk.mode, sizeof(pk.mode), "%s", g_gm.mode);
    pk.flags = (uint8_t)g_gm.flags;
    pk.respawn_time = g_gm.respawn_time < 0.0f ? 255 : (uint8_t)fminf(254.0f, ceilf(g_gm.respawn_time));
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        const GmPlayer *g = &g_gm.p[s];
        NetGmPlayer *o = &pk.p[s];
        o->team = (uint8_t)g->team;
        o->flags = (uint8_t)g->flags;
        o->model = model_byte(g->model);
        o->speed = (uint8_t)clampf(g->speed * 64.0f + 0.5f, 1.0f, 255.0f);
        o->disguise = (uint16_t)g->disguise;
        o->weapons = (uint16_t)g->weapons;
        o->spawn_seq = (uint8_t)g->spawn_seq;
        o->spawn_move = (uint8_t)g->spawn_move;
        o->select_seq = (uint8_t)g->select_seq;
        o->select_weapon = (uint8_t)g->select_weapon;
        o->max_health = (uint16_t)g->max_health;
        o->spawn_pos[0] = g->spawn_pos.x;
        o->spawn_pos[1] = g->spawn_pos.y;
        o->spawn_pos[2] = g->spawn_pos.z;
        o->spawn_yaw = g->spawn_yaw;
    }
    int n = script_nw_pack(pk.nw, sizeof(pk.nw));
    pk.nw_len = (uint16_t)n;
    net_send(NULL, &pk, (int)offsetof(PkGmState, nw) + n);
}

static void gm_host_send(float dt) {
    g_gm.state_timer -= dt;
    if (g_gm.state_timer <= 0.0f || g_gm.dirty) {
        g_gm.state_timer = GM_STATE_INTERVAL;
        g_gm.dirty = 0;
        gm_send_state();
    }
    g_gm.ev_timer -= dt;
    if (g_gm.ev_sends > 0 || g_gm.ev_timer <= 0.0f) {
        g_gm.ev_timer = 1.0f;
        if (g_gm.ev_sends > 0) g_gm.ev_sends--;
        PkGmEvents pk;
        memset(&pk, 0, sizeof(pk));
        pk.h = net_header(PK_GMEVENTS);
        pk.latest = g_gm.ev_seq;
        for (int k = 0; k < GM_EVENTS; k++) {
            if (g_gm.ev[k].seq) pk.ev[pk.count++] = g_gm.ev[k];
        }
        net_send(NULL, &pk, (int)offsetof(PkGmEvents, ev) + pk.count * (int)sizeof(GmEvent));
    }
}

static void gm_client_state(const void *data, int len) {
    const PkGmState *pk = data;
    if (len < (int)offsetof(PkGmState, nw)) return;
    if (g_gm.on) {
        g_gm.flags = pk->flags;
        g_gm.respawn_time = pk->respawn_time == 255 ? -1.0f : (float)pk->respawn_time;
        for (int s = 0; s < NET_MAX_PLAYERS; s++) {
            const NetGmPlayer *o = &pk->p[s];
            GmPlayer *g = &g_gm.p[s];
            g->team = o->team;
            g->flags = o->flags;
            g->model = o->model == NET_NO_MODEL ? -1 : o->model;
            g->speed = o->speed / 64.0f;
            g->disguise = o->disguise;
            g->weapons = o->weapons;
            g->spawn_seq = o->spawn_seq;
            g->spawn_move = o->spawn_move;
            g->spawn_pos = v3(o->spawn_pos[0], o->spawn_pos[1], o->spawn_pos[2]);
            g->spawn_yaw = o->spawn_yaw;
            g->select_seq = o->select_seq;
            g->select_weapon = o->select_weapon < WEAPON_COUNT ? o->select_weapon : WEAPON_HANDS;
            g->max_health = o->max_health ? o->max_health : 100.0f;
        }
    }
    int n = pk->nw_len;
    if (n > len - (int)offsetof(PkGmState, nw)) n = len - (int)offsetof(PkGmState, nw);
    if (n > GM_NW_MAX) n = GM_NW_MAX;
    script_nw_unpack(pk->nw, n);
}

static void gm_client_events(const void *data, int len) {
    const PkGmEvents *pk = data;
    if (len < (int)offsetof(PkGmEvents, ev)) return;
    int n = pk->count, fits = (len - (int)offsetof(PkGmEvents, ev)) / (int)sizeof(GmEvent);
    if (n > fits) n = fits;
    if (n > GM_EVENTS) n = GM_EVENTS;
    if (!g_gm.ev_synced) {
        // Just joined: what happened before isn't news
        g_gm.ev_synced = 1;
        g_gm.ev_seen = pk->latest;
        return;
    }
    for (;;) {
        const GmEvent *next = NULL;
        for (int k = 0; k < n; k++) {
            const GmEvent *e = &pk->ev[k];
            if (!e->seq || !seq_newer(e->seq, g_gm.ev_seen)) continue;
            if (!next || seq_newer(next->seq, e->seq)) next = e;
        }
        if (!next) break;
        gm_apply_event(next);
        g_gm.ev_seen = next->seq;
    }
}

// --- What scripts call (script.c's player, team and gamemode functions)

int game_pl_valid(int s) {
    return pl_active(s);
}

int game_pl_local(void) {
    return g_dedicated ? -1 : G.my_slot;
}

int game_pl_serial(int s) {
    return gm_slot_ok(s) ? g_gm.p[s].serial : 0;
}

const char *game_pl_name(int s) {
    return pl_active(s) ? player_name(s) : "";
}

float game_pl_health(int s) {
    return pl_active(s) ? fmaxf(0.0f, pl_health(s)) : 0.0f;
}

void game_pl_kill(int s, int silent) {
    if (is_client() || !pl_active(s) || pl_health(s) <= 0.0f) return;
    GmPlayer *g = &g_gm.p[s];
    if (silent) g->alive = 0; // no PlayerDeath for this one
    g->attacker = -1;
    if (s == G.my_slot) {
        G.health = 0.0f;
        if (G.dead_time <= 0.0f) {
            G.dead_time = 0.001f;
            physgun_release(1);
            G.qmenu = 0;
        }
    } else {
        G.peers[s].health = 0.0f;
    }
}

void game_pl_set_health(int s, float hp) {
    if (is_client() || !pl_active(s) || pl_health(s) <= 0.0f) return; // the dead are spawned, not healed
    if (hp <= 0.0f) {
        game_pl_kill(s, 0);
    } else if (s == G.my_slot) {
        G.health = hp;
    } else {
        G.peers[s].health = hp;
    }
}

float game_pl_max_health(int s) {
    return gm_slot_ok(s) ? g_gm.p[s].max_health : 100.0f;
}

void game_pl_set_max_health(int s, float hp) {
    if (is_client() || !gm_slot_ok(s)) return;
    g_gm.p[s].max_health = clampf(hp, 1.0f, 1000.0f);
    g_gm.dirty = 1;
}

vec3 game_pl_pos(int s) {
    return pl_active(s) ? pl_feet(s) : v3(0, 0, 0);
}

void game_pl_view(int s, vec3 *eye, vec3 *dir, float *yaw, float *pitch) {
    float y = 0.0f, p = 0.0f;
    if (s == G.my_slot && !g_dedicated) {
        *eye = player_eye(&G.player);
        y = G.yaw;
        p = G.pitch;
    } else if (gm_slot_ok(s)) {
        const Peer *pe = &G.peers[s];
        *eye = v3_add(pe->pos, v3(0, 0, (pe->flags & PF_DUCKED) ? 28.0f : 64.0f));
        y = pe->yaw;
        p = pe->pitch;
    } else {
        *eye = v3(0, 0, 0);
    }
    float cp = cosf(p * DEG2RAD);
    *dir = v3(cp * cosf(y * DEG2RAD), cp * sinf(y * DEG2RAD), sinf(p * DEG2RAD));
    *yaw = y;
    *pitch = p;
}

int game_pl_team(int s) {
    return gm_slot_ok(s) ? g_gm.p[s].team : 0;
}

void game_pl_set_team(int s, int team) {
    if (is_client() || !gm_slot_ok(s)) return;
    g_gm.p[s].team = team & 255;
    g_gm.dirty = 1;
}

void game_pl_spawn(int s) {
    gm_spawn(s);
}

void game_pl_set_pos(int s, vec3 pos, float yaw) {
    if (is_client() || !pl_active(s)) return;
    GmPlayer *g = &g_gm.p[s];
    g->spawn_pos = pos;
    g->spawn_yaw = yaw;
    g->spawn_move = 1;
    g->spawn_seq = g->spawn_seq % 255 + 1;
    if (s != G.my_slot) G.peers[s].pos = pos;
    g_gm.dirty = 1;
}

int game_pl_flags(int s) {
    return gm_slot_ok(s) ? g_gm.p[s].flags : 0;
}

void game_pl_set_flag(int s, int flag, int on) {
    if (is_client() || !gm_slot_ok(s)) return;
    if (on) {
        g_gm.p[s].flags |= flag;
    } else {
        g_gm.p[s].flags &= ~flag;
    }
    g_gm.dirty = 1;
}

float game_pl_speed(int s) {
    return gm_slot_ok(s) ? g_gm.p[s].speed : 1.0f;
}

void game_pl_set_speed(int s, float k) {
    if (is_client() || !gm_slot_ok(s)) return;
    g_gm.p[s].speed = clampf(k, 0.1f, 3.0f);
    g_gm.dirty = 1;
}

int game_pl_model(int s) {
    return gm_slot_ok(s) ? g_gm.p[s].model : -1;
}

void game_pl_set_model(int s, int type) {
    if (is_client() || !gm_slot_ok(s)) return;
    g_gm.p[s].model = type >= 0 && type < npc_type_count() && type < NET_NO_MODEL ? type : -1;
    g_gm.dirty = 1;
}

int game_pl_disguise(int s, int body) {
    if (is_client() || !gm_slot_ok(s) || !body_valid(body)) return 0;
    const Body *b = &G.phys.bodies[body];
    g_gm.p[s].disguise = (b->model & 0x7FFF) | ((b->lib & 1) << 15);
    g_gm.dirty = 1;
    return 1;
}

void game_pl_undisguise(int s) {
    if (is_client() || !gm_slot_ok(s)) return;
    g_gm.p[s].disguise = GM_NO_DISGUISE;
    g_gm.dirty = 1;
}

const char *game_pl_disguise_name(int s) {
    int local;
    const PpakModel *md = gm_slot_ok(s) ? disguise_model(g_gm.p[s].disguise, NULL, &local) : NULL;
    if (!md) return NULL;
    return (g_gm.p[s].disguise >> 15) ? md->name : model_name(g_gm.p[s].disguise & 0x7FFF);
}

int game_pl_weapon(int s) {
    return s == G.my_slot ? (int)G.weapon : gm_slot_ok(s) ? G.peers[s].weapon : WEAPON_HANDS;
}

unsigned int game_pl_weapons(int s) {
    return gm_slot_ok(s) ? (g_gm.p[s].weapons | 1u << WEAPON_HANDS) : ALL_WEAPONS;
}

void game_pl_set_weapons(int s, unsigned int mask) {
    if (is_client() || !gm_slot_ok(s)) return;
    g_gm.p[s].weapons = (mask & ALL_WEAPONS) | 1u << WEAPON_HANDS;
    g_gm.dirty = 1;
}

void game_pl_select(int s, int weapon) {
    if (is_client() || !gm_slot_ok(s) || weapon < 0 || weapon >= WEAPON_COUNT) return;
    g_gm.p[s].select_weapon = weapon;
    g_gm.p[s].select_seq = (g_gm.p[s].select_seq + 1) & 255;
    g_gm.dirty = 1;
}

void game_pl_give_ammo(int s, int weapon, int amount) {
    if (!gm_slot_ok(s)) return;
    gm_event(s, GEV_AMMO, weapon < 0 || weapon >= WEAPON_COUNT ? 255 : weapon, amount, NULL, NULL);
}

int game_weapon_count(void) {
    return WEAPON_COUNT;
}

int game_weapon_find(const char *id) {
    // GMod's names work too ("weapon_smg1", "weapon_physcannon")
    static const struct {
        const char *alias;
        int w;
    } k_alias[] = {{"smg1", WEAPON_SMG},     {"physcannon", WEAPON_GRAVGUN}, {"frag", WEAPON_GRENADE},
                   {"stunstick", WEAPON_CROWBAR}, {"fists", WEAPON_HANDS}, {"magnum", WEAPON_357}};
    if (strncasecmp(id, "weapon_", 7) == 0) id += 7;
    for (int w = 0; w < WEAPON_COUNT; w++) {
        if (strcasecmp(id, k_weapon_ids[w]) == 0) return w;
    }
    for (int k = 0; k < (int)(sizeof(k_alias) / sizeof(k_alias[0])); k++) {
        if (strcasecmp(id, k_alias[k].alias) == 0) return k_alias[k].w;
    }
    return -1;
}

const char *game_weapon_id(int w) {
    return w >= 0 && w < WEAPON_COUNT ? k_weapon_ids[w] : "";
}

void game_gm_message(int s, int kind, const char *text) {
    if (is_client()) {
        // A HUD script's own: this screen only
        if (s >= 0 && s != G.my_slot) return;
        if (kind == GM_MSG_CENTER) {
            snprintf(g_gm.center, sizeof(g_gm.center), "%s", text);
            g_gm.center_time = 4.0f;
        } else {
            notify(text, NOTE_CHAT);
        }
        return;
    }
    gm_event(s < 0 ? 255 : s, kind == GM_MSG_CENTER ? GEV_CENTER : GEV_CHAT, 0, 0, NULL, text);
}

void game_gm_sound(int s, const char *name, const vec3 *at) {
    if (is_client()) {
        int id = snd_find(name);
        if (id >= 0 && (s < 0 || s == G.my_slot)) {
            if (at) {
                play_at(id, *at, 1.0f);
            } else {
                play(id, 0.9f);
            }
        }
        return;
    }
    gm_event(s < 0 ? 255 : s, GEV_SOUND, 0, at != NULL, at, name);
}

int game_body_serial(int i) {
    return game_body(i) ? G.phys.bodies[i].serial : 0;
}

const char *game_body_model_name(int i) {
    const Body *b = game_body(i);
    if (!b) return "";
    if (b->lib == 1) return G.maplib_loaded && b->model < G.maplib.num_models ? G.maplib.models[b->model].name : "";
    return model_name(b->model);
}

void game_body_bounds(int i, vec3 *mins, vec3 *maxs) {
    const Body *b = game_body(i);
    vec3 h = b ? b->half : v3(0, 0, 0);
    *mins = v3_scale(h, -1.0f);
    *maxs = h;
}

int game_npc_spawn(const char *type, vec3 pos, float yaw) {
    if (is_client()) return -1;
    int t = npc_type_find(type);
    return t < 0 ? -1 : npc_spawn(t, pos, yaw);
}

void game_npc_damage(int i, float amount, int attacker) {
    if (is_client()) return;
    g_npc_attacker = attacker;
    npc_damage(i, amount);
    g_npc_attacker = -1;
}

int game_spawn_count(void) {
    return G.map.num_spawns > 0 ? G.map.num_spawns : 1;
}

vec3 game_spawn_point(int i, float *yaw) {
    if (i < 0 || i >= game_spawn_count()) i = 0;
    const PmapSpawn *sp = &G.map.spawns[i];
    *yaw = sp->yaw;
    return v3(sp->x, sp->y, sp->z);
}

float game_trace(vec3 a, vec3 b, int skip, vec3 *hit, int *body, int *slot) {
    *body = *slot = -1;
    vec3 d = v3_sub(b, a);
    float len = v3_len(d);
    if (len < 1e-3f) {
        *hit = a;
        return 1.0f;
    }
    d = v3_scale(d, 1.0f / len);
    Trace tr;
    cm_trace_line(&G.map, a, b, MASK_PROPSOLID, NULL, 0, &tr);
    float best = tr.fraction * len, t;
    int bi = phys_raycast(&G.phys, a, d, best, &t);
    if (bi >= 0) {
        best = t;
        *body = bi;
    }
    int pl = player_raycast(a, d, best, skip, &t);
    if (pl >= 0) {
        best = t;
        *body = -1;
        *slot = pl;
    }
    *hit = v3_mad(a, d, best);
    return best / len;
}

void game_cleanup_map(void) {
    if (is_client()) return;
    physgun_release(1);
    for (int i = 0; i < MAX_BODIES; i++) {
        if (G.phys.bodies[i].active) remove_body(i);
    }
    for (int n = 0; n < MAX_NPCS; n++) npc_remove(n);
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        if (npc_ragdoll_active(r)) npc_ragdoll_remove(r);
    }
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        G.peers[s].held = G.peers[s].held_prev = G.peers[s].held_rag = -1;
        G.peers[s].undo_count = 0;
    }
    projectiles_clear();
    devices_clear();
    items_clear();
    blood_clear();
    release_unused_extras();
    G.undo_count = 0;
    spawn_map_physics();
}

int game_button_down(const char *name) {
    static const struct {
        const char *name;
        unsigned int bit;
    } k_buttons[] = {{"select", PSP_CTRL_SELECT}, {"start", PSP_CTRL_START},   {"up", PSP_CTRL_UP},
                     {"down", PSP_CTRL_DOWN},     {"left", PSP_CTRL_LEFT},     {"right", PSP_CTRL_RIGHT},
                     {"l", PSP_CTRL_LTRIGGER},    {"r", PSP_CTRL_RTRIGGER},    {"cross", PSP_CTRL_CROSS},
                     {"circle", PSP_CTRL_CIRCLE}, {"square", PSP_CTRL_SQUARE}, {"triangle", PSP_CTRL_TRIANGLE}};
    for (int k = 0; k < (int)(sizeof(k_buttons) / sizeof(k_buttons[0])); k++) {
        if (strcasecmp(name, k_buttons[k].name) == 0) return (g_buttons & k_buttons[k].bit) != 0;
    }
    return 0;
}

void game_achievement(const char *id) {
    static const struct {
        const char *id;
        int ach;
    } k_ids[] = {{"hide_in_plain_sight", ACH_PROP_HUNT}, {"survivor", ACH_SURVIVOR}, {"top_frag", ACH_TOP_FRAG}};
    for (int k = 0; k < (int)(sizeof(k_ids) / sizeof(k_ids[0])); k++) {
        if (strcmp(id, k_ids[k].id) == 0 && !g_dedicated) achieve_unlock(k_ids[k].ach);
    }
}

// --- Script access ---------------------------------------------------------------------------------

// Map scripts only change the world on the host (or single player): clients get it in snapshots
// A script's props (a gamemode's kitchen and its food) are the game's, not a player's: they don't count
// towards anyone's prop limit or go on their Undo list, and they're put exactly where the script says
#define SCRIPT_OWNER 101 // body_owner of props a script spawned: not a player slot
int game_spawn_prop_by_name(const char *name, float x, float y, float z, float yaw) {
    if (is_client()) return -1;
    for (int k = 0; k < G.num_spawnable; k++) {
        int i = G.spawnable[k];
        if (strcasecmp(model_name(i), name) == 0) {
            int b = new_body(i, v3(x, y, z), yaw_quat(yaw), 0xFFFFFFFF, NULL);
            if (b < 0) return -1;
            G.body_owner[b] = SCRIPT_OWNER;
            return b;
        }
    }
    return -1;
}

Body *game_body(int i) {
    return (i >= 0 && i < MAX_BODIES && G.phys.bodies[i].active) ? &G.phys.bodies[i] : NULL;
}

void game_remove_body(int i) {
    if (game_body(i) && !is_client()) remove_body(i);
}

void game_player_pos(float *x, float *y, float *z) {
    *x = G.player.origin.x;
    *y = G.player.origin.y;
    *z = G.player.origin.z;
}

float game_player_yaw(void) {
    return G.yaw;
}

void game_message(const char *msg) {
    toast(msg);
}

int game_aim_body(void) {
    return aim_trace(USE_RANGE).body;
}

// --- Rendering ---------------------------------------------------------------------------------

typedef struct {
    unsigned int color;
    float x, y, z;
} ColVert;
#define COLVERT_FMT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static unsigned int scale_abgr(unsigned int c, float k) {
    unsigned int out = c & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float v = (float)((c >> s) & 0xFF) * k;
        out |= (v > 255.0f ? 255u : (unsigned int)v) << s;
    }
    return out;
}

static void update_body_light(Body *b) {
    if (b->light_age < 20 && b->asleep) {
        b->light_age++;
        return;
    }
    b->light_age = 0;
    int leaf = map_point_leaf(&G.map, &b->pos);
    b->ambient_abgr = (leaf >= 0 && leaf < G.map.num_leafs) ? G.map.leafs[leaf].ambient_abgr : G.map.info->ambient_abgr;
    // In sunlight unless something solid is between the prop and the sun (within a few thousand units).
    const float *sd = G.map.info->sun_dir;
    Trace tr;
    vec3 from = v3_add(b->pos, v3(0, 0, b->half.z + 2.0f));
    cm_trace_line(&G.map, from, v3_mad(from, v3(sd[0], sd[1], sd[2]), 3000.0f), MASK_PROPSOLID, NULL, 0, &tr);
    b->sunlit = tr.fraction >= 1.0f;
}

// Options > Draw distance: Near, Medium, Far, or the map's own fog distance
static float view_distance(void) {
    static const float k_limits[4] = {1200.0f, 2200.0f, 3500.0f, 1e9f};
    int d = g_settings.draw_dist < 1 ? 1 : g_settings.draw_dist > 4 ? 4 : g_settings.draw_dist;
    return k_limits[d - 1] < G.map.info->fog_end ? k_limits[d - 1] : G.map.info->fog_end;
}

// The map's fog, pulled in to hide where drawing stops when the draw distance is shorter
static void apply_fog(void) {
    const PmapInfo *in = G.map.info;
    float vd = view_distance();
    if (vd >= in->fog_end) {
        gfx_set_fog(in->fog_start, in->fog_end, in->fog_abgr);
        return;
    }
    unsigned int col = in->fog_end > in->fog_start ? in->fog_abgr : in->sky_abgr;
    gfx_set_fog(fminf(in->fog_start, vd * 0.55f), vd, col);
}

static int in_view(vec3 c, float radius, float max_dist) {
    vec3 d = v3_sub(c, G.cam.pos);
    float dist = v3_len(d);
    if (dist - radius > max_dist) return 0;
    if (dist < radius + 8.0f) return 1;
    // Behind the camera, with a generous cone for the wide PSP screen
    return v3_dot(d, view_forward()) > -radius && v3_dot(d, view_forward()) / dist > 0.35f - radius / dist;
}

static void make_light(PropLight *l, unsigned int ambient, int sunlit, unsigned int tint) {
    const float *sd = G.map.info->sun_dir;
    l->sun_dir = v3(sd[0], sd[1], sd[2]);
    l->sun_abgr = sunlit ? scale_abgr(G.map.info->sun_abgr, 0.75f) : 0xFF000000;
    l->ambient_abgr = scale_abgr(ambient, sunlit ? 1.1f : 1.4f);
    l->tint_abgr = tint;
}

// Players disguised as props (Prop Hunt): the prop, standing where they are and turning as they look
static void draw_disguises(void) {
    if (!g_gm.on) return;
    for (int s = 0; s < NET_MAX_PLAYERS; s++) {
        if (!pl_active(s) || pl_health(s) <= 0.0f) continue;
        const PropLib *lib;
        int local;
        const PpakModel *md = disguise_model(g_gm.p[s].disguise, &lib, &local);
        if (!md) continue;
        vec3 feet;
        float yaw;
        if (s == G.my_slot) {
            if (!third_person()) continue;
            feet = G.player.origin;
            yaw = G.yaw;
        } else if (!npc_puppet_pos(s, &feet, &yaw)) {
            continue; // in a vehicle, or not here yet
        }
        vec3 mn = v3(md->mins[0], md->mins[1], md->mins[2]), mx = v3(md->maxs[0], md->maxs[1], md->maxs[2]);
        vec3 size = v3_sub(mx, mn), mid = v3_add(feet, v3(0, 0, size.z * 0.5f));
        if (!in_view(mid, 0.5f * v3_len(size), view_distance())) continue;
        // Its box centred on their feet, its bottom on the ground
        float cy = cosf(yaw * DEG2RAD), sy = sinf(yaw * DEG2RAD);
        vec3 ax = v3(cy, sy, 0.0f), ay = v3(-sy, cy, 0.0f);
        vec3 c = v3_add(v3_scale(ax, (mn.x + mx.x) * 0.5f), v3_scale(ay, (mn.y + mx.y) * 0.5f));
        vec3 o = v3_sub(feet, v3_add(c, v3(0, 0, mn.z)));
        ScePspFMatrix4 m;
        m.x = (ScePspFVector4){ax.x, ax.y, ax.z, 0.0f};
        m.y = (ScePspFVector4){ay.x, ay.y, ay.z, 0.0f};
        m.z = (ScePspFVector4){0.0f, 0.0f, 1.0f, 0.0f};
        m.w = (ScePspFVector4){o.x, o.y, o.z, 1.0f};
        int leaf = map_point_leaf(&G.map, &mid);
        unsigned int amb = leaf >= 0 && leaf < G.map.num_leafs ? G.map.leafs[leaf].ambient_abgr : G.map.info->ambient_abgr;
        const float *sd = G.map.info->sun_dir;
        vec3 top = v3_add(feet, v3(0, 0, size.z + 2.0f));
        Trace tr;
        cm_trace_line(&G.map, top, v3_mad(top, v3(sd[0], sd[1], sd[2]), 3000.0f), MASK_PROPSOLID, NULL, 0, &tr);
        PropLight l;
        make_light(&l, amb, tr.fraction >= 1.0f, 0xFFFFFFFF);
        proplib_draw(lib, local, &m, &l);
    }
}

static void draw_props(void) {
    // Options > LOD distance stretches how far props are drawn (and the map's own fade distances), up to
    // the draw distance
    float lod = settings_lod_scale();
    float max_dist = fminf(view_distance(), lod * (mem_tier() == MEM_TIER_HIGH ? PROP_DRAW_DIST_HIGH : PROP_DRAW_DIST_LOW));
    proplib_begin();
    G.bodies_drawn = 0;
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &G.phys.bodies[i];
        if (!b->active) continue;
        if (b->lib == 1 && !G.maplib_loaded) continue;
        int local;
        const PropLib *lib = body_lib(b, &local);
        if (!lib) continue;
        if (!in_view(b->pos, v3_len(b->half), max_dist)) continue;
        update_body_light(b);
        PropLight l;
        make_light(&l, b->ambient_abgr, b->sunlit, b->tint_abgr);
        ScePspFMatrix4 m;
        phys_body_matrix(b, &m);
        proplib_draw(lib, local, &m, &l);
        G.bodies_drawn++;
    }
    draw_disguises();

    // Static props: PVS row for the camera's cluster, then view cone and distance
    G.sprops_drawn = 0;
    if (G.maplib_loaded && G.map.num_sprops) {
        int leaf = map_point_leaf(&G.map, &G.cam.pos);
        int cluster = (leaf >= 0 && leaf < G.map.num_leafs) ? G.map.leafs[leaf].cluster : -1;
        const uint32_t *row = map_spvs_row(&G.map, cluster);
        for (int i = 0; i < G.map.num_sprops; i++) {
            if (row && !(row[i >> 5] & (1u << (i & 31)))) continue;
            const PmapStaticProp *sp = &G.map.sprops[i];
            if ((int)sp->model >= G.maplib.num_models) continue;
            const PpakModel *md = &G.maplib.models[sp->model];
            vec3 ax, ay, az, o;
            sprop_frame(sp, &ax, &ay, &az, &o);
            vec3 lc = v3((md->mins[0] + md->maxs[0]) * 0.5f, (md->mins[1] + md->maxs[1]) * 0.5f, (md->mins[2] + md->maxs[2]) * 0.5f);
            vec3 c = v3_add(o, v3_add(v3_scale(ax, lc.x), v3_add(v3_scale(ay, lc.y), v3_scale(az, lc.z))));
            float radius = 0.5f * sqrtf((md->maxs[0] - md->mins[0]) * (md->maxs[0] - md->mins[0]) +
                                        (md->maxs[1] - md->mins[1]) * (md->maxs[1] - md->mins[1]) +
                                        (md->maxs[2] - md->mins[2]) * (md->maxs[2] - md->mins[2]));
            float dist = sp->fade_dist > 0.0f ? fminf(sp->fade_dist * lod, max_dist) : max_dist;
            if (!in_view(c, radius, dist + radius)) continue;
            ScePspFMatrix4 m;
            m.x = (ScePspFVector4){ax.x, ax.y, ax.z, 0.0f};
            m.y = (ScePspFVector4){ay.x, ay.y, ay.z, 0.0f};
            m.z = (ScePspFVector4){az.x, az.y, az.z, 0.0f};
            m.w = (ScePspFVector4){o.x, o.y, o.z, 1.0f};
            PropLight l;
            make_light(&l, G.sprop_ambient[i], G.sprop_sunlit[i], 0xFFFFFFFF);
            proplib_draw(&G.maplib, (int)sp->model, &m, &l);
            G.sprops_drawn++;
        }
    }
    proplib_end();
}

static void draw_lines(const ColVert *pts, int n, int strip) {
    ColVert *v = sceGuGetMemory(sizeof(ColVert) * n);
    memcpy(v, pts, sizeof(ColVert) * n);
    sceGumDrawArray(strip ? GU_LINE_STRIP : GU_LINES, COLVERT_FMT, n, 0, v);
}

// A soft round blob facing the camera at c, radius s: a fan of segs triangles, col in the middle fading
// to nothing at the rim. Writes segs * 3 vertices.
static int glow(ColVert *v, vec3 c, vec3 r, vec3 u, float s, unsigned int col, int segs) {
    unsigned int rim = col & 0x00FFFFFFu;
    for (int k = 0; k < segs; k++) {
        float a0 = (float)k * (2.0f * PI / (float)segs), a1 = (float)(k + 1) * (2.0f * PI / (float)segs);
        vec3 p0 = v3_add(c, v3_add(v3_scale(r, cosf(a0) * s), v3_scale(u, sinf(a0) * s)));
        vec3 p1 = v3_add(c, v3_add(v3_scale(r, cosf(a1) * s), v3_scale(u, sinf(a1) * s)));
        v[k * 3 + 0] = (ColVert){col, c.x, c.y, c.z};
        v[k * 3 + 1] = (ColVert){rim, p0.x, p0.y, p0.z};
        v[k * 3 + 2] = (ColVert){rim, p1.x, p1.y, p1.z};
    }
    return segs * 3;
}

// Lamps' and lights' glows and the emitters' particles: glowing ones added on, smoke blended over
static void draw_devices(void) {
    float cy = cosf(G.cam.yaw * DEG2RAD), sy = sinf(G.cam.yaw * DEG2RAD);
    float cp = cosf(G.cam.pitch * DEG2RAD), sp = sinf(G.cam.pitch * DEG2RAD);
    vec3 r = v3(sy, -cy, 0.0f), u = v3(-cy * sp, -sy * sp, cp);
    int n = 0;
    for (int d = 0; d < MAX_DEVICES; d++) n += g_dev[d].type && g_dev[d].on && g_dev[d].type <= DEV_LIGHT;
    for (int k = 0; k < MAX_PARTICLES; k++) n += g_parts[k].life > 0.0f;
    if (!n) return;
    sceGuDepthMask(GU_TRUE);
    sceGuEnable(GU_BLEND);
    for (int pass = 0; pass < 2; pass++) {
        // Pass 0: smoke (alpha blended); pass 1: glows, sparks and fire (added)
        ColVert *v = sceGuGetMemory(sizeof(ColVert) * (18 * MAX_PARTICLES + 24 * MAX_DEVICES));
        int nv = 0;
        if (pass == 0) {
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        } else {
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
            for (int d = 0; d < MAX_DEVICES; d++) {
                const Device *dv = &g_dev[d];
                if (!dv->type || !dv->on || dv->type > DEV_LIGHT) continue;
                vec3 pos, dir;
                device_frame(dv, &pos, &dir);
                unsigned int c = k_palette[dv->colour % PALETTE_SIZE] & 0x00FFFFFFu;
                nv += glow(v + nv, pos, r, u, dv->type == DEV_LAMP ? 10.0f : 14.0f, 0xE0000000u | c, 8);
            }
        }
        for (int k = 0; k < MAX_PARTICLES; k++) {
            const Particle *p = &g_parts[k];
            if (p->life <= 0.0f || (p->effect == EMIT_SMOKE) != (pass == 0)) continue;
            float t = 1.0f - p->life / p->max_life; // 0 new .. 1 gone
            unsigned int a = (unsigned int)(255.0f * (1.0f - t)), col;
            float s = p->size;
            if (p->effect == EMIT_SMOKE) {
                s = p->size * (1.0f + 2.5f * t);
                col = ((a * 0x90u / 255u) << 24) | 0x707070u;
            } else if (p->effect == EMIT_FIRE) {
                s = p->size * (1.0f - 0.6f * t);
                unsigned int g = (unsigned int)(200.0f * (1.0f - t)); // yellow to red
                col = (a << 24) | (0x10u << 16) | (g << 8) | 0xFFu;
            } else {
                col = (a << 24) | 0x60D0FFu;
            }
            nv += glow(v + nv, p->pos, r, u, s * 1.4f, col, 6);

        }
        if (nv) sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, nv, 0, v);
    }
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
}

#ifndef PS2
// The two lamps or lights nearest the camera that are on light the world (GE lights 2 and 3)
static void devices_lights(void) {
    int pick[2] = {-1, -1};
    float best[2] = {1e9f, 1e9f};
    vec3 eye = v3(G.cam.pos.x, G.cam.pos.y, G.cam.pos.z);
    for (int d = 0; d < MAX_DEVICES; d++) {
        const Device *v = &g_dev[d];
        if (!v->type || !v->on || v->type > DEV_LIGHT) continue;
        vec3 pos, dir;
        device_frame(v, &pos, &dir);
        float dist = v3_len(v3_sub(pos, eye));
        if (dist > 2500.0f) continue;
        if (dist < best[0]) {
            best[1] = best[0];
            pick[1] = pick[0];
            best[0] = dist;
            pick[0] = d;
        } else if (dist < best[1]) {
            best[1] = dist;
            pick[1] = d;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (pick[k] < 0) {
            gfx_world_light(k, 0, 0, NULL, NULL, 0, 0.0f);
            continue;
        }
        const Device *v = &g_dev[pick[k]];
        vec3 pos, dir;
        device_frame(v, &pos, &dir);
        float p[3] = {pos.x, pos.y, pos.z}, dd[3] = {dir.x, dir.y, dir.z};
        gfx_world_light(k, 1, v->type == DEV_LAMP, p, dd, k_palette[v->colour % PALETTE_SIZE],
                        v->type == DEV_LAMP ? LAMP_RANGE : LIGHT_RANGE);
    }
}
#endif

static void draw_balloons_and_ropes(void) {
    // A balloon is a little coloured octahedron on a string.
    static const float oct[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const int tri[8][3] = {{0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2}, {1, 4, 2}, {1, 3, 4}, {1, 5, 3}, {1, 2, 5}};
    for (int i = 0; i < MAX_BODIES; i++) {
        const Body *b = &G.phys.bodies[i];
        if (!b->active || b->balloons == 0) continue;
        vec3 top = v3_add(b->pos, q_rotate(b->rot, v3(0, 0, b->half.z)));
        for (int k = 0; k < b->balloons; k++) {
            float ang = (float)k * (2.0f * PI / MAX_BALLOONS);
            vec3 c = v3_add(top, v3(cosf(ang) * 14.0f * (b->balloons > 1), sinf(ang) * 14.0f * (b->balloons > 1), 56.0f));
            unsigned int col = k_palette[1 + (i + k) % (PALETTE_SIZE - 2)];
            ColVert *v = sceGuGetMemory(sizeof(ColVert) * 24);
            for (int t = 0; t < 8; t++) {
                float shade = t < 4 ? 1.0f : 0.6f;
                for (int j = 0; j < 3; j++) {
                    const float *o = oct[tri[t][j]];
                    float sz = o[2] > 0 ? 13.0f : 10.0f;
                    v[t * 3 + j] = (ColVert){scale_abgr(col, shade), c.x + o[0] * 10.0f, c.y + o[1] * 10.0f, c.z + o[2] * sz};
                }
            }
            sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, 24, 0, v);
            ColVert string[2] = {{0xFFE0E0E0, top.x, top.y, top.z}, {0xFFE0E0E0, c.x, c.y, c.z - 10.0f}};
            draw_lines(string, 2, 0);
        }
    }
    for (int j = 0; j < MAX_JOINTS; j++) {
        const Joint *jt = &G.phys.joints[j];
        if (!jt->active || (jt->type != JOINT_ROPE && jt->type != JOINT_ELASTIC && jt->type != JOINT_SLIDER &&
                            jt->type != JOINT_ROD))
            continue;
        vec3 a, b;
        phys_joint_anchors(&G.phys, jt, &a, &b);
        if (jt->type == JOINT_ROD) {
            // A hydraulic: a dark ram, drawn twice to look thicker
            for (int k = 0; k < 2; k++) {
                ColVert ram[2] = {{0xFF404850, a.x, a.y, a.z + k * 0.8f}, {0xFF909898, b.x, b.y, b.z + k * 0.8f}};
                draw_lines(ram, 2, 0);
            }
            continue;
        }
        // Sag the rope a little when it's slack; elastic is a straight red band, a slider a grey rod
        float slack = jt->type == JOINT_ROPE ? clampf(jt->length - v3_len(v3_sub(a, b)), 0.0f, 200.0f) : 0.0f;
        unsigned int col = jt->type == JOINT_ROPE ? 0xFF305070 : jt->type == JOINT_ELASTIC ? 0xFF2020C0 : 0xFFA0A0A0;
        ColVert pts[9];
        for (int i = 0; i < 9; i++) {
            float t = (float)i / 8.0f;
            vec3 p = v3_lerp(a, b, t);
            p.z -= slack * 0.5f * 4.0f * t * (1.0f - t);
            pts[i] = (ColVert){col, p.x, p.y, p.z};
        }
        draw_lines(pts, 9, 1);
    }
    // Thrusters: a small dark cone with a flame while on. Hoverballs: a small glowing ball.
    for (int f = 0; f < MAX_FORCES; f++) {
        const Force *fc = &G.phys.forces[f];
        if (!fc->active || !G.phys.bodies[fc->body].active) continue;
        const Body *b = &G.phys.bodies[fc->body];
        vec3 p = v3_add(b->pos, q_rotate(b->rot, fc->lpos));
        vec3 d = fc->type == FORCE_THRUSTER ? q_rotate(b->rot, fc->ldir) : v3(0, 0, 1);
        vec3 s = fabsf(d.z) < 0.9f ? v3_norm(v3_cross(d, v3(0, 0, 1))) : v3(1, 0, 0);
        vec3 t = v3_cross(d, s);
        float r = fc->type == FORCE_THRUSTER ? 4.0f : 5.0f;
        unsigned int body_col = fc->type == FORCE_THRUSTER ? 0xFF404040 : (fc->on ? 0xFF60E0FF : 0xFF606060);
        ColVert *v = sceGuGetMemory(sizeof(ColVert) * 24);
        // Eight triangles round the attach point: a little octahedron (stretched back for thrusters)
        vec3 tip = v3_mad(p, d, fc->type == FORCE_THRUSTER ? -8.0f : r), back = v3_mad(p, d, -r);
        vec3 ring[4] = {v3_mad(p, s, r), v3_mad(p, t, r), v3_mad(p, s, -r), v3_mad(p, t, -r)};
        for (int k = 0; k < 4; k++) {
            vec3 q0 = ring[k], q1 = ring[(k + 1) & 3];
            v[k * 6 + 0] = (ColVert){body_col, tip.x, tip.y, tip.z};
            v[k * 6 + 1] = (ColVert){body_col, q0.x, q0.y, q0.z};
            v[k * 6 + 2] = (ColVert){body_col, q1.x, q1.y, q1.z};
            v[k * 6 + 3] = (ColVert){scale_abgr(body_col, 0.6f), back.x, back.y, back.z};
            v[k * 6 + 4] = (ColVert){scale_abgr(body_col, 0.6f), q1.x, q1.y, q1.z};
            v[k * 6 + 5] = (ColVert){scale_abgr(body_col, 0.6f), q0.x, q0.y, q0.z};
        }
        sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, 24, 0, v);
        if (fc->type == FORCE_THRUSTER && fc->on) {
            // The flame comes out the back, flickering
            float len = 14.0f + (float)(sceKernelGetSystemTimeLow() % 7);
            vec3 e = v3_mad(p, d, -8.0f - len);
            ColVert flame[4] = {{0xFF40C0FF, tip.x, tip.y, tip.z}, {0xFF2060FF, e.x, e.y, e.z},
                                {0xFF40C0FF, tip.x + s.x * 2, tip.y + s.y * 2, tip.z + s.z * 2},
                                {0xFF2060FF, e.x + s.x * 2, e.y + s.y * 2, e.z + s.z * 2}};
            draw_lines(flame, 4, 0);
        }
    }
    // Explosion: a bright fireball that grows and fades
    if (G.blast_time > 0.0f) {
        float k = G.blast_time / 0.35f;
        float s = 40.0f + 120.0f * (1.0f - k);
        vec3 c = G.blast_pos, f = view_forward(), r = view_right();
        vec3 u = v3_cross(r, f);
        // A disc facing the camera: bright yellow-white middle fading to a transparent orange edge
        unsigned int mid = ((unsigned int)(k * 240.0f) << 24) | 0x0090E0FF, edge = 0x000040FF;
        ColVert *v = sceGuGetMemory(sizeof(ColVert) * 36);
        for (int i = 0; i < 12; i++) {
            float a0 = (float)i * (2.0f * PI / 12.0f), a1 = (float)(i + 1) * (2.0f * PI / 12.0f);
            vec3 p0 = v3_add(c, v3_add(v3_scale(r, cosf(a0) * s), v3_scale(u, sinf(a0) * s)));
            vec3 p1 = v3_add(c, v3_add(v3_scale(r, cosf(a1) * s), v3_scale(u, sinf(a1) * s)));
            v[i * 3] = (ColVert){mid, c.x, c.y, c.z};
            v[i * 3 + 1] = (ColVert){edge, p0.x, p0.y, p0.z};
            v[i * 3 + 2] = (ColVert){edge, p1.x, p1.y, p1.z};
        }
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
        sceGuDisable(GU_CULL_FACE);
        sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, 36, 0, v);
        sceGuDisable(GU_BLEND);
    }
}

// One box of the figure: local x is its right, local y its facing direction, z up.
static int haunt_box(ColVert *v, vec3 c, float yaw, float ox, float hx, float hy, float z0, float z1) {
    float f = yaw * DEG2RAD;
    vec3 fw = v3(cosf(f), sinf(f), 0.0f), rt = v3(sinf(f), -cosf(f), 0.0f);
    vec3 p[8];
    for (int i = 0; i < 8; i++) {
        float lx = ox + ((i & 1) ? hx : -hx), ly = (i & 2) ? hy : -hy, lz = (i & 4) ? z1 : z0;
        p[i] = v3(c.x + rt.x * lx + fw.x * ly, c.y + rt.y * lx + fw.y * ly, c.z + lz);
    }
    static const unsigned char faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    int n = 0;
    for (int fi = 0; fi < 6; fi++) {
        const unsigned char *q = faces[fi];
        const int tri[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
        for (int k = 0; k < 6; k++) v[n++] = (ColVert){0xFF000000, p[tri[k]].x, p[tri[k]].y, p[tri[k]].z};
    }
    return n;
}

static void draw_haunt(void) {
    if (!g_haunt.on || !g_haunt.visible) return;
    ColVert *v = sceGuGetMemory(sizeof(ColVert) * 36 * 6);
    vec3 c = g_haunt.pos;
    float y = g_haunt.yaw;
    int n = 0;
    n += haunt_box(v + n, c, y, -5.0f, 3.0f, 3.0f, 0.0f, 44.0f);   // legs
    n += haunt_box(v + n, c, y, 5.0f, 3.0f, 3.0f, 0.0f, 44.0f);
    n += haunt_box(v + n, c, y, 0.0f, 9.0f, 4.5f, 44.0f, 74.0f);   // body
    n += haunt_box(v + n, c, y, -12.5f, 2.5f, 2.5f, 30.0f, 72.0f); // long arms
    n += haunt_box(v + n, c, y, 12.5f, 2.5f, 2.5f, 30.0f, 72.0f);
    n += haunt_box(v + n, c, y, 0.0f, 4.5f, 5.0f, 75.0f, 88.0f);   // head
    // Pure black: no fog to grey it out, and the flashlight doesn't light it
    gu_flashlight(0, NULL, NULL);
    gfx_set_fog(0, 0, 0);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_CULL_FACE);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, n, 0, v);
    apply_fog();
    vec3 f = view_forward();
    float e[3] = {G.cam.pos.x, G.cam.pos.y, G.cam.pos.z}, fw[3] = {f.x, f.y, f.z};
    gu_flashlight(g_flashlight && G.dead_time <= 0.0f, e, fw);
}

typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} DecalVert;
#define DECALVERT_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static void draw_blood(void) {
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE); // test against the world, don't write
    sceGuDisable(GU_CULL_FACE);
    // Decals: surface x texture x 2 (grey leaves it alone), with no fog (it would tint the multiply)
    decal_textures();
    gfx_set_fog(0, 0, 0);
    sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0);
    for (int tex = 0; tex < DECAL_TEXTURES; tex++) {
        int n = 0;
        for (int i = 0; i < MAX_DECALS; i++) n += g_decals[i].active && g_decals[i].tex == tex;
        float umax, vmax;
        if (!n || !ui_image_bind(g_decal_img[tex], &umax, &vmax)) continue;
        DecalVert *v = sceGuGetMemory(sizeof(DecalVert) * n * 6), *o = v;
        int drawn = 0;
        for (int i = 0; i < MAX_DECALS; i++) {
            const Decal *d = &g_decals[i];
            if (!d->active || d->tex != tex) continue;
            vec3 c = d->c, s = d->s, t = d->t, nn = d->n;
            if (d->body >= 0) {
                const Body *b = &G.phys.bodies[d->body];
                c = v3_add(b->pos, q_rotate(b->rot, c));
                s = q_rotate(b->rot, s);
                t = q_rotate(b->rot, t);
                nn = q_rotate(b->rot, nn);
            }
            float dist = v3_len(v3_sub(c, G.cam.pos));
            if (dist > 1500.0f) continue;
            // Lifted off the surface by more the further it is: the 16-bit depth buffer can't tell
            // a decal from its wall far away, and a fixed gap shows close up
            c = v3_mad(c, nn, 0.15f + dist * 0.0018f);
            vec3 a = v3_scale(s, d->r), bb = v3_scale(t, d->r);
            vec3 q[4] = {v3_sub(v3_sub(c, a), bb), v3_sub(v3_add(c, a), bb), v3_add(v3_add(c, a), bb), v3_add(v3_sub(c, a), bb)};
            const float uv[4][2] = {{0, 0}, {umax, 0}, {umax, vmax}, {0, vmax}};
            const int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) {
                int j = idx[k];
                *o++ = (DecalVert){uv[j][0], uv[j][1], 0xFFFFFFFF, q[j].x, q[j].y, q[j].z};
            }
            drawn++;
        }
        if (drawn) sceGumDrawArray(GU_TRIANGLES, DECALVERT_FMT, drawn * 6, 0, v);
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    apply_fog();
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    // Drops: little camera-facing diamonds
    vec3 r = view_right(), u = v3_cross(r, view_forward());
    int n = 0;
    for (int i = 0; i < MAX_BLOOD_DROPS; i++) n += g_drops[i].life > 0.0f;
    if (n) {
        ColVert *v = sceGuGetMemory(sizeof(ColVert) * n * 6), *o = v;
        for (int i = 0; i < MAX_BLOOD_DROPS; i++) {
            const BloodDrop *b = &g_drops[i];
            if (b->life <= 0.0f) continue;
            float sz = 1.0f + 0.8f * (b->life / BLOOD_DROP_LIFE);
            unsigned int col = ((unsigned int)(clampf(b->life / 0.2f, 0.0f, 1.0f) * 230.0f) << 24) | 0x00000090;
            vec3 a = v3_scale(r, sz), c = v3_scale(u, sz), p = b->p;
            vec3 q[4] = {v3_sub(p, a), v3_sub(p, c), v3_add(p, a), v3_add(p, c)};
            const int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) *o++ = (ColVert){col, q[idx[k]].x, q[idx[k]].y, q[idx[k]].z};
        }
        sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, n * 6, 0, v);
    }
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
}

// The bright middle of a physics gun beam: its colour, most of the way to white
static unsigned int beam_core(unsigned int c) {
    unsigned int out = 0xFF000000u;
    for (int k = 0; k < 24; k += 8) out |= ((((c >> k) & 0xFFu) + 3u * 255u) / 4u) << k;
    return out;
}

static void draw_beams(void) {
    vec3 muzzle = gun_muzzle();
    unsigned int pgun = profile_colour(g_settings.physgun_colour, 1);

    if (G.weapon == WEAPON_PHYSGUN && !G.use_carry && (G.held >= 0 || G.held_rag >= 0)) {
        vec3 end;
        if (G.held_rag >= 0) {
            end = npc_ragdoll_point(G.held_rag, G.held_bone);
        } else {
            const Body *b = &G.phys.bodies[G.held];
            end = v3_add(b->pos, q_rotate(b->rot, G.grab_local));
        }
        vec3 ctrl = v3_mad(muzzle, view_forward(), v3_len(v3_sub(end, muzzle)) * 0.5f);
        // Bendy blue beam, drawn three times slightly offset to look thicker than one pixel.
        for (int pass = 0; pass < 3; pass++) {
            ColVert pts[12];
            vec3 off = v3((float)(pass - 1) * 0.4f, 0.0f, (float)(pass - 1) * 0.4f);
            for (int i = 0; i < 12; i++) {
                float t = (float)i / 11.0f;
                vec3 p = v3_add(v3_add(v3_scale(muzzle, (1 - t) * (1 - t)), v3_scale(ctrl, 2 * t * (1 - t))), v3_scale(end, t * t));
                p = v3_add(p, off);
                pts[i] = (ColVert){pass == 1 ? beam_core(pgun) : pgun, p.x, p.y, p.z};
            }
            draw_lines(pts, 12, 1);
        }
    }
    // Other players' physics gun beams, in their colours: from their gun to what they hold
    for (int s = 0; s < NET_MAX_PLAYERS && G.role != ROLE_OFFLINE; s++) {
        const Peer *p = &G.peers[s];
        if (s == G.my_slot || !p->active || p->weapon != WEAPON_PHYSGUN || !body_valid(p->held) || p->health <= 0.0f) continue;
        float yr = p->yaw * DEG2RAD, pr = p->pitch * DEG2RAD;
        vec3 f = v3(cosf(pr) * cosf(yr), cosf(pr) * sinf(yr), -sinf(pr)), r = v3(sinf(yr), -cosf(yr), 0.0f);
        vec3 from = v3_add(v3_add(p->pos, v3(0, 0, (p->flags & PF_DUCKED) ? 30.0f : 52.0f)), v3_add(v3_scale(f, 22.0f), v3_scale(r, 8.0f)));
        vec3 to = G.phys.bodies[p->held].pos, ctrl = v3_mad(from, f, v3_len(v3_sub(to, from)) * 0.5f);
        unsigned int c = profile_colour(p->pgun, 1);
        ColVert pts[12];
        for (int i = 0; i < 12; i++) {
            float t = (float)i / 11.0f;
            vec3 q = v3_add(v3_add(v3_scale(from, (1 - t) * (1 - t)), v3_scale(ctrl, 2 * t * (1 - t))), v3_scale(to, t * t));
            pts[i] = (ColVert){i & 1 ? beam_core(c) : c, q.x, q.y, q.z};
        }
        draw_lines(pts, 12, 1);
    }
    if (G.weapon == WEAPON_TOOLGUN && G.beam_time > 0.0f) {
        ColVert pts[2] = {{0xFFFFFFFF, muzzle.x, muzzle.y, muzzle.z}, {0xFF80FFFF, G.beam_end.x, G.beam_end.y, G.beam_end.z}};
        draw_lines(pts, 2, 0);
    }
    // Bullet tracers: a short bright streak partway along each shot
    for (int i = 0; i < 8; i++) {
        if (G.tracers[i].time <= 0.0f) continue;
        float k = 1.0f - G.tracers[i].time / 0.06f;
        vec3 a = v3_lerp(G.tracers[i].a, G.tracers[i].b, k * 0.7f), b = v3_lerp(G.tracers[i].a, G.tracers[i].b, k * 0.7f + 0.3f);
        ColVert pts[2] = {{0xFF80E0FF, a.x, a.y, a.z}, {0xFFC0F8FF, b.x, b.y, b.z}};
        draw_lines(pts, 2, 0);
    }
}

static void draw_world_viewmodel(int model, vec3 f, vec3 r, vec3 u, unsigned int amb, int glow);

// The weapon in your hands: its world model held in front of the camera, drawn after
// clearing depth so it never pokes into walls.
static void draw_viewmodel(void) {
    int model = G.vm_model[G.weapon];
    int cmodel = G.vm_c[G.weapon];
    if (!G.props_loaded || (model < 0 && cmodel < 0) || G.player.noclip) return;
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    vec3 f = view_forward(), r = view_right();
    vec3 u = v3_cross(r, f);
    vec3 eye = player_eye(&G.player);
    int leaf = map_point_leaf(&G.map, &eye);
    unsigned int amb = (leaf >= 0 && leaf < G.map.num_leafs) ? G.map.leafs[leaf].ambient_abgr : G.map.info->ambient_abgr;
    int glow = (G.weapon == WEAPON_PHYSGUN || G.weapon == WEAPON_GRAVGUN) && !G.use_carry && (G.held >= 0 || G.held_rag >= 0);
    int flash = is_gun(G.weapon) && G.muzzle_time > 0.0f;
    if (cmodel >= 0) {
        // The c_ model is authored in view space (+X forward, +Y left, +Z up, origin at the eye) and
        // drawn with Source's view model field of view, so the arms sit where they do in GMod.
        vec3 o = vm_origin();
        ScePspFMatrix4 m;
        m.x = (ScePspFVector4){f.x, f.y, f.z, 0.0f};
        m.y = (ScePspFVector4){-r.x, -r.y, -r.z, 0.0f};
        m.z = (ScePspFVector4){u.x, u.y, u.z, 0.0f};
        m.w = (ScePspFVector4){o.x, o.y, o.z, 1.0f};
        Camera vc = G.cam;
        vc.fov_y = VM_FOV_Y;
        vc.znear = 1.0f;
        vc.zfar = 256.0f;
        camera_apply(&vc);
        PropLight l;
        make_light(&l, amb, 1, glow ? 0xFFFFE0C0 : 0xFFFFFFFF);
        l.ambient_abgr = scale_abgr(amb, 1.6f);
        if (G.vm_anim[G.weapon] >= 0) {
            vmodel_draw(G.vm_anim[G.weapon], &m, &l); // animated (draw, fire, reload...)
        } else {
            proplib_begin();
            proplib_draw(&G.props, cmodel, &m, &l);
            proplib_end();
        }
        camera_apply(&G.cam);
    } else {
        draw_world_viewmodel(model, f, r, u, amb, glow);
    }

    // Physics gun glow at the claws while it's holding something, or a gun's muzzle flash
    if (glow || flash) {
        vec3 c = gun_muzzle();
        float s = flash ? 4.5f : 2.0f + 0.5f * sinf(G.bob_phase * 7.0f + (float)(sceKernelGetSystemTimeLow() & 0xFFFF) * 0.0004f);
        unsigned int col = flash ? 0xD050D0FF : G.weapon == WEAPON_GRAVGUN ? 0x8040A0FF : 0x80FFC060;
        vec3 a = v3_scale(r, s), b = v3_scale(u, s);
        ColVert q[6] = {
            {col, c.x - a.x - b.x, c.y - a.y - b.y, c.z - a.z - b.z}, {col, c.x + a.x - b.x, c.y + a.y - b.y, c.z + a.z - b.z},
            {col, c.x + a.x + b.x, c.y + a.y + b.y, c.z + a.z + b.z}, {col, c.x - a.x - b.x, c.y - a.y - b.y, c.z - a.z - b.z},
            {col, c.x + a.x + b.x, c.y + a.y + b.y, c.z + a.z + b.z}, {col, c.x - a.x + b.x, c.y - a.y + b.y, c.z - a.z + b.z},
        };
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
        ColVert *v = sceGuGetMemory(sizeof(q));
        memcpy(v, q, sizeof(q));
        sceGumDrawArray(GU_TRIANGLES, COLVERT_FMT, 6, 0, v);
        sceGuDisable(GU_BLEND);
    }
}

// Fallback for packs without c_ models: the weapon's world model held in front of the camera
static void draw_world_viewmodel(int model, vec3 f, vec3 r, vec3 u, unsigned int amb, int glow) {
    vec3 grip = vm_grip();
    // Model -Y points forward, -X points right, +Z up (after turning it by k_vm's turn)
    float tc = cosf(k_vm[G.weapon].turn * DEG2RAD), ts = sinf(k_vm[G.weapon].turn * DEG2RAD);
    vec3 mx = v3_add(v3_scale(r, -tc), v3_scale(f, -ts)), my = v3_add(v3_scale(r, ts), v3_scale(f, -tc));
    vec3 mz = u;
    if (k_vm[G.weapon].pitch != 0.0f || k_vm[G.weapon].roll != 0.0f) {
        // Twist about the view direction (up turns towards left), then tip forward-pointing parts up
        float cb = cosf(k_vm[G.weapon].roll * DEG2RAD), sb = sinf(k_vm[G.weapon].roll * DEG2RAD);
        float ca = cosf(k_vm[G.weapon].pitch * DEG2RAD), sa = sinf(k_vm[G.weapon].pitch * DEG2RAD);
        vec3 *axes[3] = {&mx, &my, &mz};
        for (int k = 0; k < 3; k++) {
            vec3 v = *axes[k];
            float vr = v3_dot(v, r), vu = v3_dot(v, u), vf = v3_dot(v, f);
            float r1 = vr * cb - vu * sb, u1 = vr * sb + vu * cb; // roll
            float u2 = u1 * ca + vf * sa, f2 = vf * ca - u1 * sa; // pitch
            *axes[k] = v3_add(v3_add(v3_scale(r, r1), v3_scale(u, u2)), v3_scale(f, f2));
        }
    }
    ScePspFMatrix4 m;
    m.x = (ScePspFVector4){mx.x, mx.y, mx.z, 0.0f};
    m.y = (ScePspFVector4){my.x, my.y, my.z, 0.0f};
    m.z = (ScePspFVector4){mz.x, mz.y, mz.z, 0.0f};
    if (k_vm[G.weapon].centre) {
        const PpakModel *md = &G.props.models[model];
        vec3 c = v3((md->mins[0] + md->maxs[0]) * 0.5f, (md->mins[1] + md->maxs[1]) * 0.5f, (md->mins[2] + md->maxs[2]) * 0.5f);
        grip = v3_sub(grip, v3_add(v3_add(v3_scale(mx, c.x), v3_scale(my, c.y)), v3_scale(mz, c.z)));
    }
    m.w = (ScePspFVector4){grip.x, grip.y, grip.z, 1.0f};
    PropLight l;
    make_light(&l, amb, 1, glow ? 0xFFFFE0C0 : 0xFFFFFFFF);
    l.ambient_abgr = scale_abgr(amb, 1.6f);
    proplib_begin();
    proplib_draw(&G.props, model, &m, &l);
    proplib_end();
}

// Derma-ish palette (ABGR)
#define C_FRAME 0xF2D6D6D6
#define C_PANEL 0xFFF4F4F4
#define C_TAB 0xFFFFFFFF
#define C_TEXT 0xFF303030
#define C_TEXT_DIM 0xFF808080
#define C_ACCENT 0xFF2AA8F0
#define C_ACCENT_DIM 0xFFB0D8F0
#define C_HL2 0xFF00DCFF // HL2/GMod HUD yellow

// Spawn menu, laid out like GMod's Q menu: spawnlists on the left, tools on the right
static void draw_qmenu(int props) {
    ui_rect(0, 0, 480, 272, 0x50000000);

    // Spawnlists frame
    float fx = 6, fy = 6, fw = 316, fh = 244;
    ui_rrect(fx, fy, fw, fh, 4, C_FRAME);
    ui_rrect(fx + 6, fy + 4, 76, 20, 3, C_TAB);
    ui_text(FONT_BOLD, fx + 44, fy + 8, C_TEXT, ALIGN_CENTER, "Spawnlists");

    // Spawnlists: Recent, your lists, NPCs, Ragdolls, then one header per game (opened one at a time)
    float cx = fx + 6, cy = fy + 28, cw = 96, ch = fh - 34;
    ui_rect(cx, cy, cw, ch, C_PANEL);
    if (G.focus == 0) ui_outline(cx, cy, cw, ch, C_ACCENT);
    static int rows[MAX_PANEL_ROWS];
    int nr = panel_rows(rows);
    float rh = 15.0f;
    int visible = (int)((ch - 8) / rh);
    if (G.qrow < G.cat_scroll) G.cat_scroll = G.qrow;
    if (G.qrow >= G.cat_scroll + visible) G.cat_scroll = G.qrow - visible + 1;
    if (G.cat_scroll > nr - visible) G.cat_scroll = nr - visible > 0 ? nr - visible : 0;
    for (int k = G.cat_scroll; k < nr && k < G.cat_scroll + visible; k++) {
        float ry = cy + 4 + (k - G.cat_scroll) * rh;
        int r = rows[k];
        int cursor = k == G.qrow;
        int header = r <= ROW_HEADER(0);
        int shown = !header && r == G.cat; // the list in the grid (dimmer when the cursor's elsewhere)
        if (cursor || shown) ui_rect(cx + 2, ry - 1, cw - 4, rh - 1, cursor && G.focus == 0 ? C_ACCENT : C_ACCENT_DIM);
        unsigned int tc = cursor && G.focus == 0 ? 0xFFFFFFFF : C_TEXT;
        if (header) {
            // A game: a +/- box, its name in bold
            int g = g_game_order[-100 - r];
            int open = G.group_open == g;
            ui_rect(cx + 5, ry + 3, 9, 9, cursor && G.focus == 0 ? 0xFFFFFFFF : 0xFF8A8A8A);
            ui_rect(cx + 6, ry + 4, 7, 7, cursor && G.focus == 0 ? C_ACCENT : 0xFFF4F4F4);
            ui_rect(cx + 7, ry + 7, 5, 1, cursor && G.focus == 0 ? 0xFFFFFFFF : C_TEXT);
            if (!open) ui_rect(cx + 9, ry + 5, 1, 5, cursor && G.focus == 0 ? 0xFFFFFFFF : C_TEXT);
            ui_text_clip(FONT_SMALL, cx + 17, ry + 1, cw - 20, tc, group_title(g));

        } else {
            int sub = r >= extra_row0(); // inside an open game: indented
            float ix = cx + 6 + (sub ? 8 : 0);
            // A little folder glyph like GMod's spawnlist tree (Recent's is orange)
            unsigned int fc = cursor && G.focus == 0 ? 0xFFFFFFFF : r == RECENT_CAT ? 0xFF3A9AF0 : 0xFF5AB4E8;
            ui_rect(ix, ry + 4, 9, 7, fc);
            ui_rect(ix, ry + 2, 4, 3, fc);
            ui_text_clip(FONT_SMALL, ix + 13, ry + 1, cw - (ix - cx) - 17, tc, short_row_name(r));
        }
    }
    if (nr > visible) {
        float bar = (ch - 8) * visible / nr;
        ui_rect(cx + cw - 4, cy + 4, 2, ch - 8, 0xFFDDDDDD);
        ui_rect(cx + cw - 4, cy + 4 + G.cat_scroll * (ch - 8 - bar) / (nr - visible), 2, bar, 0xFF9A9A9A);
    }

    // Icon grid: four columns of 45-pixel tiles, with room for the scroll bar
    propx_new_frame();
    static int items[MAX_SPAWNABLE];
    int n = grid_items(items);
    float gx = cx + cw + 6, gy = cy;
    float gw = fw - cw - 18;
    const float pitch = 47.0f, tile = 45.0f;
    ui_rect(gx, gy, gw, 164, C_PANEL);
    if (G.focus == 1) ui_outline(gx, gy, gw, 164, C_ACCENT);
    for (int k = G.grid_scroll * GRID_COLS; k < n && k < (G.grid_scroll + GRID_ROWS) * GRID_COLS; k++) {
        int r = k / GRID_COLS - G.grid_scroll, c = k % GRID_COLS;
        float tx = gx + 5 + c * pitch, ty = gy + 7 + r * 52;
        int sel = k == G.grid_sel;
        ui_rrect(tx - 1, ty - 1, tile + 2, tile + 2, 3, sel ? (G.focus == 1 ? C_ACCENT : C_ACCENT_DIM) : 0xFFC8C8C8);
        ui_rect(tx + 1, ty + 1, tile - 2, tile - 2, 0xFFFFFFFF);
        int wep = items[k] >= GRID_WEAPON ? items[k] - GRID_WEAPON : -1;
        const char *iname = wep >= 0               ? k_weapon_names[wep]
                            : items[k] >= GRID_RAG ? npc_type_name(items[k] - GRID_RAG)
                            : items[k] >= GRID_NPC ? npc_type_name(items[k] - GRID_NPC)
                                                   : model_name(items[k]);
        char key[64];
        snprintf(key, sizeof(key), wep >= 0 ? "icon:weapon:%s" : "icon:%s", wep >= 0 ? k_weapon_ids[wep] : iname);
        int img = ui_image_find(key);
        int extra = items[k] < GRID_NPC && items[k] >= G.props.num_models;
        if (img >= 0) {
            ui_image(img, tx + 1, ty + 1, tile - 2, tile - 2, 0xFFFFFFFF);
        } else if (!extra || !propx_draw_icon(items[k] - G.props.num_models, tx + 1, ty + 1, tile - 2, tile - 2)) {
            ui_text_clip(FONT_SMALL, tx + 3, ty + 17, tile - 6, C_TEXT, iname);
        }
        // Drivable or sittable: a little steering wheel in the corner
        if (items[k] < GRID_NPC && drivable_model(items[k]) != VEH_NONE) {
            float bx = tx + tile - 14, by = ty + 2;
            ui_rrect(bx, by, 12, 12, 6, 0xE0303030);
            ui_rrect(bx + 2, by + 2, 8, 8, 4, 0xFFFFFFFF);
            ui_rrect(bx + 3, by + 3, 6, 6, 3, 0xE0303030);
            ui_rect(bx + 5, by + 5, 2, 2, 0xFFFFFFFF);
        }
    }
    int rows_n = (n + GRID_COLS - 1) / GRID_COLS;
    if (rows_n > GRID_ROWS) {
        float bar = 152.0f * GRID_ROWS / rows_n;
        ui_rect(gx + gw - 5, gy + 6, 3, 152, 0xFFDDDDDD);
        ui_rect(gx + gw - 5, gy + 6 + G.grid_scroll * (152.0f - bar) / (rows_n - GRID_ROWS), 3, bar, 0xFF9A9A9A);
    }
    // Selected prop details
    float iy = gy + 170;
    ui_rect(gx, iy, gw, ch - 170, C_PANEL);
    if (n > 0 && G.grid_sel < n && items[G.grid_sel] >= GRID_WEAPON) {
        int w = items[G.grid_sel] - GRID_WEAPON;
        ui_text(FONT_BOLD, gx + 6, iy + 5, C_TEXT, ALIGN_LEFT, k_weapon_names[w]);
        ui_text(FONT_SMALL, gx + 6, iy + 22, gm_carried() & (1u << w) ? C_TEXT_DIM : C_ACCENT, ALIGN_LEFT,
                gm_carried() & (1u << w) ? "You have it: X takes it out" : "X: pick it up");
    } else if (n > 0 && G.grid_sel < n && items[G.grid_sel] >= GRID_RAG) {
        ui_text(FONT_BOLD, gx + 6, iy + 5, C_TEXT, ALIGN_LEFT, npc_type_name(items[G.grid_sel] - GRID_RAG));
        ui_textf(FONT_SMALL, gx + 6, iy + 22, C_TEXT_DIM, ALIGN_LEFT, RAGDOLL_LOCAL_MSG);
    } else if (n > 0 && G.grid_sel < n && items[G.grid_sel] >= GRID_NPC) {
        ui_text(FONT_BOLD, gx + 6, iy + 5, C_TEXT, ALIGN_LEFT, npc_type_name(items[G.grid_sel] - GRID_NPC));
        ui_textf(FONT_SMALL, gx + 6, iy + 22, C_TEXT_DIM, ALIGN_LEFT, "NPCs %d/%d", npc_alive_count(), MAX_NPCS);
    } else if (n > 0 && G.grid_sel < n) {
        PpakModel tmp;
        const PpakModel *md = model_info(items[G.grid_sel], &tmp);
        ui_text(FONT_BOLD, gx + 6, iy + 5, C_TEXT, ALIGN_LEFT, md->name);
        int kind = drivable_model(items[G.grid_sel]);
        if (kind != VEH_NONE) {
            ui_text(FONT_SMALL, gx + 6, iy + 22, C_ACCENT, ALIGN_LEFT, kind == VEH_SEAT ? "Seat: Use to sit in it" : "Drivable: Use to get in");
        } else {
            ui_textf(FONT_SMALL, gx + 6, iy + 22, C_TEXT_DIM, ALIGN_LEFT, "%.0f kg   %.0f x %.0f x %.0f", md->mass,
                     md->maxs[0] - md->mins[0], md->maxs[1] - md->mins[1], md->maxs[2] - md->mins[2]);
        }
    }
    if (n > 0) ui_textf(FONT_SMALL, gx + gw - 6, iy + 22, C_TEXT_DIM, ALIGN_RIGHT, "%d / %d", G.grid_sel + 1, n);
    ui_textf(FONT_SMALL, gx + gw - 6, iy + 5, props >= PLAYER_PROP_LIMIT ? 0xFF3030D0 : C_TEXT_DIM, ALIGN_RIGHT,
             "Props %d/%d", props, PLAYER_PROP_LIMIT);

    // Tools frame
    float tx = 328, ty = 6, tw = 146, th = 244;
    ui_rrect(tx, ty, tw, th, 4, C_FRAME);
    ui_rrect(tx + 6, ty + 4, 50, 20, 3, C_TAB);
    ui_text(FONT_BOLD, tx + 31, ty + 8, C_TEXT, ALIGN_CENTER, "Tools");
    float lx = tx + 6, ly = ty + 28, lw = tw - 12;
    ui_rect(lx, ly, lw, 150, C_PANEL);
    if (G.focus == 2) ui_outline(lx, ly, lw, 150, C_ACCENT);
    // The list is taller than the panel, so it scrolls to keep the selected tool in view
    const float list_h = 150.0f;
    {
        float vy = 4.0f, sel_y = 0.0f;
        int r = 0;
        for (int g = 0; g < NUM_TOOL_GROUPS; g++) {
            vy += 15.0f;
            for (int k = 0; k < k_tool_groups[g].count; k++, r++, vy += 14.0f) {
                if (r == G.tool_row) sel_y = vy;
            }
            vy += 3.0f;
        }
        if (G.tool_row < TOOL_COUNT) {
            if (sel_y - G.tool_scroll < 18.0f) G.tool_scroll = (int)(sel_y - 18.0f);
            if (sel_y - G.tool_scroll > list_h - 16.0f) G.tool_scroll = (int)(sel_y - list_h + 16.0f);
        }
        if (G.tool_scroll > (int)(vy - list_h)) G.tool_scroll = (int)(vy - list_h);
        if (G.tool_scroll < 0) G.tool_scroll = 0;
    }
    float y = ly + 4 - (float)G.tool_scroll;
    int row = 0;
#define TOOL_ROW_VISIBLE(yy, hh) ((yy) >= ly && (yy) + (hh) <= ly + list_h)
    for (int g = 0; g < NUM_TOOL_GROUPS; g++) {
        if (TOOL_ROW_VISIBLE(y, 13)) ui_text(FONT_BOLD, lx + 5, y, C_TEXT, ALIGN_LEFT, k_tool_groups[g].heading);
        y += 15;
        for (int k = 0; k < k_tool_groups[g].count; k++, row++) {
            Tool t = k_tool_groups[g].tools[k];
            int sel = row == G.tool_row;
            if (TOOL_ROW_VISIBLE(y - 1, 14)) {
                if (sel) ui_rect(lx + 2, y - 1, lw - 4, 14, G.focus == 2 ? C_ACCENT : C_ACCENT_DIM);
                ui_text(FONT_SMALL, lx + 14, y, sel ? 0xFFFFFFFF : C_TEXT, ALIGN_LEFT, k_tool_names[t]);
                if (t == G.tool && G.weapon == WEAPON_TOOLGUN) ui_rect(lx + 6, y + 4, 4, 4, sel ? 0xFFFFFFFF : C_ACCENT);
            }
            y += 14;
        }
        y += 3;
    }
#undef TOOL_ROW_VISIBLE
    // Scroll hint: a thin bar on the right
    {
        float total = 4.0f;
        for (int g = 0; g < NUM_TOOL_GROUPS; g++) total += 18.0f + 14.0f * (float)k_tool_groups[g].count;
        if (total > list_h) {
            float bar = list_h * list_h / total;
            ui_rect(lx + lw - 4, ly + 2, 2, list_h - 4, 0xFFDDDDDD);
            ui_rect(lx + lw - 4, ly + 2 + (float)G.tool_scroll * (list_h - 4 - bar) / (total - list_h), 2, bar, 0xFF9A9A9A);
        }
    }
    // Colour tool swatches
    float sy = ly + 154;
    int csel = G.focus == 2 && G.tool_row == TOOL_COUNT;
    ui_rect(lx, sy, lw, 38, C_PANEL);
    if (csel) ui_outline(lx, sy, lw, 38, C_ACCENT);
    ui_text(FONT_SMALL, lx + 5, sy + 3, C_TEXT, ALIGN_LEFT, "Colour");
    for (int i = 0; i < PALETTE_SIZE; i++) {
        float px = lx + 5 + i * 16.5f;
        if (i == G.colour_sel) ui_rect(px - 2, sy + 18, 17, 17, csel ? C_ACCENT : 0xFF606060);
        ui_rect(px, sy + 20, 13, 13, k_palette[i]);
    }
    // Description of the highlighted tool
    Tool ht = G.tool_row == TOOL_COUNT ? TOOL_COLOUR : tool_at_row(G.tool_row);
    ui_text_clip(FONT_SMALL, lx + 2, sy + 42, lw - 2, C_TEXT_DIM, k_tool_desc[ht]);

    // Footer hints
    ui_rrect(6, 254, 468, 16, 3, 0xC0000000);
    ui_text(FONT_SMALL, 14, 256, 0xFFE0E0E0, ALIGN_LEFT,
            "X Select    L/R Switch panel    O / SELECT Close    START Pause");
}

static void draw_notifications(void) {
    static const unsigned int kind_colour[] = {0xFF2AA8F0, 0xFF50C0FF, 0xFF60C060, 0xFF4040E0, 0xFF40A0FF, 0xFF909090,
                                               0xFF30B8F0};
    static const char *kind_glyph[] = {"i", "<", "!", "x", "*", ">", "+"};
    float y = is_gun(G.weapon) && k_ws[G.weapon].clip ? 190 : 226; // above the ammo box
    for (int i = 0; i < MAX_NOTES; i++) {
        float t = G.notes[i].time;
        if (!G.notes[i].text[0] || t > 4.0f) continue;
        float slide = t < 0.2f ? (1.0f - t / 0.2f) * 120.0f : 0.0f;
        unsigned int a = t > 3.5f ? (unsigned int)(255.0f * (4.0f - t) / 0.5f) : 255u;
        float w = ui_text_width(FONT_SMALL, G.notes[i].text) + 30;
        float x = 474 - w + slide;
        ui_rrect(x, y, w, 20, 3, (a * 0xC0 / 255) << 24 | 0x202020);
        ui_rrect(x + 3, y + 3, 14, 14, 3, (a << 24) | (kind_colour[G.notes[i].kind] & 0xFFFFFF));
        ui_text(FONT_BOLD, x + 10, y + 3, (a << 24) | 0xFFFFFF, ALIGN_CENTER, kind_glyph[G.notes[i].kind]);
        ui_text(FONT_SMALL, x + 22, y + 4, (a << 24) | 0xFFFFFF, ALIGN_LEFT, G.notes[i].text);
        y -= 24;
    }
}

// A gamemode's game, dead: "You died", then whose view you're watching, and when X respawns you
static void draw_gm_dead(void) {
    int watching = G.dead_time >= 1.5f && gm_spectate_ok(g_gm.spectate);
    float y = 150.0f;
    if (watching) {
        char line[48];
        snprintf(line, sizeof(line), "Watching %s", player_name(g_gm.spectate));
        ui_text_shadow(FONT_BOLD, 240, 222, 0xFFFFFFFF, ALIGN_CENTER, line);
        ui_text_shadow(FONT_SMALL, 240, 238, 0xFFC0C0C0, ALIGN_CENTER, "L / R: someone else");
        y = 204.0f;
    } else {
        ui_rect(0, 0, 480, 272, 0x700000A0);
        ui_text_shadow(FONT_TITLE, 240, 110, 0xFFFFFFFF, ALIGN_CENTER, "You died");
    }
    if (g_gm.respawn_time < 0.0f) return; // the gamemode brings you back (its HUD says when)
    float wait = fmaxf(1.0f, g_gm.respawn_time) - G.dead_time;
    char msg[32];
    snprintf(msg, sizeof(msg), wait > 0.0f ? "Respawn in %d" : "Press X to respawn", (int)ceilf(wait));
    ui_text_shadow(FONT_SMALL, 240, y, 0xFFE0E0E0, ALIGN_CENTER, msg);
}

// A gamemode's message across the middle of the screen (PrintMessage(HUD_PRINTCENTER, ...))
static void draw_gm_center(void) {
    if (g_gm.center_time <= 0.0f || !g_gm.center[0]) return;
    unsigned int a = g_gm.center_time < 0.5f ? (unsigned int)(255.0f * g_gm.center_time / 0.5f) : 255u;
    float w = ui_text_width(FONT_LARGE, g_gm.center) + 24.0f;
    ui_rrect(240.0f - w * 0.5f, 40, w, 28, 6, (a * 0xA0u / 255u) << 24);
    ui_text(FONT_LARGE, 240, 44, (a << 24) | 0xFFFFFF, ALIGN_CENTER, g_gm.center);
}

// HL2 / GMod style HUD
static void draw_hud(float fps) {
    if (G.player.water_level >= 3) ui_rect(0, 0, 480, 272, 0x60804010); // underwater tint
    if (g_gm.on && !g_dedicated && (g_gm.p[G.my_slot].flags & GPF_BLIND) && G.dead_time <= 0.0f) {
        ui_rect(0, 0, 480, 272, 0xFF000000); // blindfolded (Prop Hunt's hunters while the props hide)
    }

    if (G.cam_view >= 0) {
        // Looking through a camera: a recording dot instead of the crosshair (it isn't where you aim)
        ui_rrect(12, 12, 8, 8, 4, 0xF02020E0);
        ui_text_shadow(FONT_BOLD, 26, 9, 0xFFFFFFFF, ALIGN_LEFT, "CAMERA");
        ui_text_shadow(FONT_SMALL, 26, 26, 0xFFE0E0E0, ALIGN_LEFT, "L: Next camera / your view");
    } else if (g_settings.crosshair && !G.qmenu) {
        // A small dot with a dark rim so it shows on bright and dark backgrounds
        ui_rect(238, 135, 4, 4, 0x80000000);
        ui_rect(239, 136, 2, 2, 0xF0FFFFFF);
    }

    // Health box, bottom left
    // Label and number laid out from measured widths so they can't overlap
    if (G.hurt_flash > 0.0f) ui_rect(0, 0, 480, 272, ((unsigned int)(G.hurt_flash * 200.0f) << 24) | 0x0000C0);
    if (G.dead_time > 0.0f && g_gm.on) {
        draw_gm_dead();
    } else if (G.dead_time > 0.0f) {
        ui_rect(0, 0, 480, 272, 0x700000A0);
        ui_text_shadow(FONT_TITLE, 240, 110, 0xFFFFFFFF, ALIGN_CENTER, "You died");
        if (G.dead_time > 1.0f) ui_text_shadow(FONT_SMALL, 240, 150, 0xFFE0E0E0, ALIGN_CENTER, "Press X to respawn");
    }
    draw_gm_center();
    // Options > Show HUD off hides the boxes, hints and notes (the crosshair has its own option)
    if (!g_settings.show_hud) goto stats;

    // GM.ShowHealth = false: a game where nobody gets hurt (Happy Burger) has no health or suit boxes
    if (g_gm.on && (g_gm.flags & GMF_NO_HEALTH)) goto no_health;
    char hp[8];
    snprintf(hp, sizeof(hp), "%d", (int)ceilf(G.health));
    unsigned int hp_col = G.health > 25.0f ? C_HL2 : 0xFF2020FF;
    float label_w = ui_text_width(FONT_BOLD, "HEALTH");
    float num_w = ui_text_width(FONT_TITLE, hp);
    float box_w = 10 + label_w + 12 + num_w + 10;
    ui_rrect(10, 230, box_w, 34, 6, 0x60000000);
    ui_text(FONT_BOLD, 20, 248, hp_col, ALIGN_LEFT, "HEALTH");
    ui_text(FONT_TITLE, 10 + box_w - 10, 233, hp_col, ALIGN_RIGHT, hp);
    // HEV suit power, next to it like HL2's SUIT box
    if (g_settings.hev_suit) {
        char sp[8];
        snprintf(sp, sizeof(sp), "%d", (int)ceilf(G.suit));
        float sl = ui_text_width(FONT_BOLD, "SUIT"), sn = ui_text_width(FONT_TITLE, sp);
        float sx = 10 + box_w + 8, sw = 10 + sl + 12 + sn + 10;
        unsigned int sc = G.suit > 0.0f ? C_HL2 : 0xFF2020FF;
        ui_rrect(sx, 230, sw, 34, 6, 0x60000000);
        ui_text(FONT_BOLD, sx + 10, 248, sc, ALIGN_LEFT, "SUIT");
        ui_text(FONT_TITLE, sx + sw - 10, 233, sc, ALIGN_RIGHT, sp);
    }
    if (god_mode()) ui_text_shadow(FONT_SMALL, 12, 214, C_HL2, ALIGN_LEFT, "God mode");
no_health:

    // Tool gun info, top left like GMod's tool HUD (a camera view has its own label there)
    if (G.weapon == WEAPON_TOOLGUN && G.cam_view < 0) {
        const char *help = G.stage ? "R: Pick the second point" : k_tool_help[G.tool];
        // The Posers show what R will do
        const char *desc = k_tool_desc[G.tool];
        char pick[48];
        if (G.tool == TOOL_FACEPOSER || G.tool == TOOL_FINGERPOSER) {
            snprintf(pick, sizeof(pick), G.tool == TOOL_FACEPOSER ? "Expression: %s" : "Hand pose: %s",
                     G.tool == TOOL_FACEPOSER ? npc_face_name(g_face_sel) : npc_hand_name(g_hand_sel));
            desc = pick;
        }
        float w = 24 + fmaxf(fmaxf(ui_text_width(FONT_LARGE, k_tool_names[G.tool]), ui_text_width(FONT_SMALL, desc)),
                             ui_text_width(FONT_SMALL, help));
        ui_rrect(8, 8, w, 58, 6, 0x90000000);
        ui_text_shadow(FONT_LARGE, 18, 12, 0xFFFFFFFF, ALIGN_LEFT, k_tool_names[G.tool]);
        ui_text(FONT_SMALL, 18, 34, 0xFFC8C8C8, ALIGN_LEFT, desc);
        ui_text(FONT_SMALL, 18, 49, C_HL2, ALIGN_LEFT, help);
        if (G.tool == TOOL_COLOUR) ui_rrect(w - 10, 14, 12, 12, 3, k_palette[G.colour_sel]);
    }

    // Weapon selector, top centre, HL2 bucket style
    if (G.wsel_time > 0.0f) {
        unsigned int a = G.wsel_time < 0.3f ? (unsigned int)(255 * G.wsel_time / 0.3f) : 255u;
        // A slot per weapon, the selected one wide enough for its name; they shrink to fit the screen
        // (in a gamemode's game, just the ones you carry)
        unsigned int have = gm_carried();
        int count = 0;
        for (int i = 0; i < WEAPON_COUNT; i++) count += (have >> i) & 1;
        const float wide = 104.0f, gap = 3.0f;
        const float slot = count > 1 ? fminf(34.0f, (468.0f - wide - gap * (count - 1)) / (count - 1)) : 0.0f;
        float x = 240.0f - (slot * (count - 1) + wide + gap * (count - 1)) * 0.5f;
        for (int i = 0; i < WEAPON_COUNT; i++) {
            if (!(have & (1u << i))) continue;
            int sel = i == (int)G.weapon;
            float bw = sel ? wide : slot, by = 70;
            ui_rrect(x, by, bw, sel ? 44 : 30, 5, ((sel ? 0xA0u : 0x60u) * a / 255) << 24);
            ui_textf(FONT_BOLD, x + 5, by + 3, (a << 24) | (C_HL2 & 0xFFFFFF), ALIGN_LEFT, "%d", i + 1);
            if (sel) {
                ui_text(FONT_BOLD, x + bw / 2, by + 22, (a << 24) | (C_HL2 & 0xFFFFFF), ALIGN_CENTER, k_weapon_names[i]);
            }
            x += bw + gap;
        }
    }

    // Ammo, bottom right, like the health box
    if (is_gun(G.weapon) && k_ws[G.weapon].clip) {
        char clip[8], spare[12];
        snprintf(clip, sizeof(clip), "%d", G.clip[G.weapon]);
        snprintf(spare, sizeof(spare), "%d", G.ammo[G.weapon]);
        float lw = ui_text_width(FONT_BOLD, "AMMO"), cw = ui_text_width(FONT_TITLE, clip), sw = ui_text_width(FONT_BOLD, spare);
        float bw = 10 + lw + 12 + cw + 10 + sw + 10;
        unsigned int col = G.clip[G.weapon] == 0 ? 0xFF2020FF : C_HL2;
        ui_rrect(470 - bw, 230, bw, 34, 6, 0x60000000);
        ui_text(FONT_BOLD, 480 - bw, 248, col, ALIGN_LEFT, "AMMO");
        ui_text(FONT_TITLE, 480 - bw + lw + 12 + cw, 233, col, ALIGN_RIGHT, clip);
        ui_text(FONT_BOLD, 460, 248, C_HL2, ALIGN_RIGHT, spare);
        if (G.reload_left > 0.0f) ui_text_shadow(FONT_SMALL, 470 - bw / 2, 214, 0xFFE0E0E0, ALIGN_CENTER, "Reloading...");
    }

    // Context hint while holding something with the physics gun: above the health and suit boxes
    float hint_y = g_settings.show_hud ? 214.0f : 250.0f;
    if (G.use_carry) {
        ui_text_shadow(FONT_SMALL, 240, hint_y, 0xFFE0E0E0, ALIGN_CENTER, "Let go of Use to drop    R: Throw");
    } else if (G.held_rag >= 0) {
        ui_text_shadow(FONT_SMALL, 240, hint_y, 0xFFE0E0E0, ALIGN_CENTER, "L: Freeze    Up/Down: Distance");
    } else if (G.held >= 0 && G.weapon == WEAPON_PHYSGUN) {
        ui_text_shadow(FONT_SMALL, 240, hint_y, 0xFFE0E0E0, ALIGN_CENTER,
                       "L: Freeze   Up/Down: Distance   Left/Right: Rotate   X + pad: Turn");
    }

    draw_notifications();

stats:
    if (g_settings.show_fps) ui_textf(FONT_SMALL, 474, 4, 0xC0FFFFFF, ALIGN_RIGHT, "%.0f fps", fps);
    if (g_settings.debug) {
        ui_textf(FONT_SMALL, 6, 74, 0xFFFFFFFF, ALIGN_LEFT, "%s  tris %d (clip %d)  batches %d/%d  tex %d", G.map_name,
                 G.ds.tris_drawn, G.ds.clipped_tris, G.ds.batches_drawn, G.ds.batches_visible, G.ds.tex_binds);
        ui_textf(FONT_SMALL, 6, 88, 0xFFFFFFFF, ALIGN_LEFT, "cluster %d  heap %u KB  vram tex %d  contacts %d",
                 G.ds.cluster, (unsigned int)(mem_heap_used() / 1024), G.map.vram_textures, G.phys.contacts_last_step);
        ui_textf(FONT_SMALL, 6, 102, 0xFFFFFFFF, ALIGN_LEFT, "pos %.0f %.0f %.0f  %s  props %d+%d", G.player.origin.x,
                 G.player.origin.y, G.player.origin.z, G.player.on_ground ? "ground" : "air", G.sprops_drawn,
                 G.bodies_drawn);
#ifdef PS2
        char prof[128];
        gu_profile_text(prof, sizeof(prof));
        ui_text_shadow(FONT_SMALL, 6, 130, 0xFF80FFFF, ALIGN_LEFT, prof);
        ui_textf(FONT_SMALL, 6, 144, 0xFF80FFFF, ALIGN_LEFT, "ps2: update %.1fms  map %.1fms  props %.1fms  npcs %.1fms",
                 g_ps2_prof[3], g_ps2_prof[0], g_ps2_prof[1], g_ps2_prof[2]);
#endif
        if (npc_ragdoll_active(0)) {
            vec3 rp = npc_ragdoll_point(0, 0);
            int ss, hits, asleep;
            npc_ragdoll_debug(&ss, &hits, &asleep);
            ui_textf(FONT_SMALL, 6, 116, 0xFFFFFFFF, ALIGN_LEFT, "ragdoll 0 at %.0f %.0f %.0f  startsolid %d hits %d %s", rp.x,
                     rp.y, rp.z, ss, hits, asleep ? "asleep" : "awake");
        }
    }
}

static void draw_pause(void) {
    ui_rect(0, 0, 480, 272, 0x80000000);
    menu_draw_backdrop();
    int logo = ui_image_find("logo");
    if (logo >= 0) {
        int w, h;
        ui_image_size(logo, &w, &h);
        float s = fminf(200.0f / (float)w, 50.0f / (float)h);
        ui_image(logo, 20, 18, w * s, h * s, 0xFFFFFFFF);
    }
    if (G.pause_options) {
        options_draw(150, 60, 300, 186);
    } else if (G.pause_saves) {
        saves_draw(130, 60, 340, 186);
    } else {
        ui_text_shadow(FONT_BOLD, 24, 74, 0xFFA0A0A0, ALIGN_LEFT, "PAUSED");
        int items[NUM_PAUSE_ITEMS], n = pause_items(items);
        const char *labels[NUM_PAUSE_ITEMS];
        for (int k = 0; k < n; k++) labels[k] = k_pause_items[items[k]];
        menu_draw_buttons(labels, n, G.pause_sel, 88, 20);
        ui_text_shadow(FONT_SMALL, 24, 256, 0xC0FFFFFF, ALIGN_LEFT, "X Select    O / START Resume");
        ui_textf(FONT_SMALL, 474, 256, 0xA0FFFFFF, ALIGN_RIGHT, "%s  -  %d props", G.map_name, player_prop_count());
        if (G.role != ROLE_OFFLINE) {
            // Who's playing, top right
            float y = 74;
            ui_text_shadow(FONT_BOLD, 456, y, 0xFFA0A0A0, ALIGN_RIGHT, G.role == ROLE_HOST ? NET_HOSTING_TAG : "MULTIPLAYER");
            if (G.role == ROLE_HOST && !net_active()) {
                y += 16;
                ui_text_shadow(FONT_SMALL, 456, y, 0xFFC0C0C0, ALIGN_RIGHT, net_status());
            }
            for (int s = 0; s < NET_MAX_PLAYERS; s++) {
                const char *name = s == G.my_slot ? net_nickname() : G.peers[s].active ? G.peers[s].name : NULL;
                if (!name) continue;
                y += 16;
                ui_textf(FONT_SMALL, 456, y, s == G.my_slot ? 0xFFFFFFFF : 0xFFD0D0D0, ALIGN_RIGHT, "%s%s", name, s == 0 ? "  (host)" : "");
            }
        }
    }
}

// Options > Third-person view: the camera sits behind the player's eye, pulled in where a wall's in the way
#define THIRD_PERSON_BACK 80.0f
#define THIRD_PERSON_UP 12.0f

static int third_person(void) {
    return (g_settings.third_person || gm_disguised(G.my_slot)) && G.dead_time <= 0.0f;
}

static vec3 third_person_camera(vec3 eye) {
    // Further back in a vehicle, so you see all of it
    float back = G.veh >= 0 && vehicle_kind(&G.phys.bodies[G.veh]) != VEH_SEAT ? THIRD_PERSON_BACK * 3.0f : THIRD_PERSON_BACK;
    if (gm_disguised(G.my_slot)) back = THIRD_PERSON_BACK * 1.6f; // a prop: see it and what's round it
    vec3 want = v3_add(v3_mad(eye, view_forward(), -back), v3(0, 0, THIRD_PERSON_UP + (back - THIRD_PERSON_BACK) * 0.3f));
    Trace tr;
    cm_trace(&G.map, eye, want, v3(-4, -4, -4), v3(4, 4, 4), MASK_PLAYERSOLID, NULL, 0, &tr);
    return tr.startsolid ? eye : tr.endpos;
}

void game_render(float fps) {
    gfx_begin_frame(G.map.info->sky_abgr);
    npc_set_draw_scale(settings_lod_scale()); // Options > LOD distance
    G.cam.pos = player_eye(&G.player);
    if (G.dead_time > 0.0f) G.cam.pos.z = G.player.origin.z + 10.0f + 50.0f * fmaxf(0.0f, 1.0f - G.dead_time * 2.0f);
    if (third_person()) G.cam.pos = third_person_camera(G.cam.pos);
    G.cam.yaw = G.yaw;
    G.cam.pitch = G.pitch;
    gm_spectate_camera(); // dead in a gamemode's game: watching someone
    if (is_camera(G.cam_view)) {
        // Looking through a camera prop: from just in front of its lens, along its forward axis
        const Body *cb = &G.phys.bodies[G.cam_view];
        vec3 f = q_rotate(cb->rot, v3(1, 0, 0));
        G.cam.pos = v3_mad(cb->pos, f, 12.0f);
        G.cam.yaw = atan2f(f.y, f.x) / DEG2RAD;
        G.cam.pitch = asinf(clampf(f.z, -1.0f, 1.0f)) / DEG2RAD;
    }
    G.cam.zfar = view_distance() + 512.0f;
    camera_apply(&G.cam);
    sky_draw(&G.cam);
    apply_fog();
    map_set_view_limit(view_distance());
#ifdef PS2
    {
        vec3 f = view_forward();
        float e[3] = {G.cam.pos.x, G.cam.pos.y, G.cam.pos.z}, fw[3] = {f.x, f.y, f.z};
        gu_flashlight(g_flashlight && G.dead_time <= 0.0f, e, fw);
    }
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    map_draw(&G.map, &G.cam, &G.ds);
    SceInt64 t1 = sceKernelGetSystemTimeWide();
    draw_props();
    SceInt64 t2 = sceKernelGetSystemTimeWide();
    npcs_draw(&G.map, &G.cam);
    draw_npc_weapons();
    draw_haunt();
    g_ps2_prof[0] = (float)(t1 - t0) / 1000.0f;
    g_ps2_prof[1] = (float)(t2 - t1) / 1000.0f;
    g_ps2_prof[2] = (float)(sceKernelGetSystemTimeWide() - t2) / 1000.0f;
#else
    {
        vec3 f = view_forward();
        float e[3] = {G.cam.pos.x, G.cam.pos.y, G.cam.pos.z}, fw[3] = {f.x, f.y, f.z};
        gu_flashlight(g_flashlight && G.dead_time <= 0.0f, e, fw);
    }
    devices_lights();
    map_draw(&G.map, &G.cam, &G.ds);
    draw_props();
    npcs_draw(&G.map, &G.cam);
    draw_npc_weapons();
    draw_projectiles();
    draw_haunt();
#endif
    sceGuDisable(GU_TEXTURE_2D);
    draw_balloons_and_ropes();
    draw_devices();
    draw_beams();
    draw_blood();
    if (G.dead_time <= 0.0f && !third_person() && G.cam_view < 0 && G.veh < 0) draw_viewmodel();

    gu_flashlight(0, NULL, NULL); // the menu's background map shouldn't inherit it
#ifndef PS2
    gfx_world_light(0, 0, 0, NULL, NULL, 0, 0.0f);
    gfx_world_light(1, 0, 0, NULL, NULL, 0, 0.0f);
#endif
    map_set_view_limit(1e9f);     // nor the draw distance

    ui_begin();
    if (!G.paused || g_shot_pending) {
        draw_name_tags();
        draw_hud(fps);
        draw_quick_chat();
        draw_voice_hud();
        script_hud();

    }
    if (G.qmenu && !G.paused) draw_qmenu(player_prop_count());
    if (G.paused && !g_shot_pending) draw_pause();
    ui_end();
    gfx_end_frame();
    if (G.paused && G.pause_saves) saves_frame(); // the keyboard for a save's name
    gfx_present(1);
    if (g_shot_pending) {
        // That frame is on the screen now: save it, then back to the game
        char path[160], msg[64];
        if (g_shot(G.map_name, path, sizeof(path)) == 0) {
            const char *file = strrchr(path, '/');
            snprintf(msg, sizeof(msg), "Screenshot saved: %.40s", file ? file + 1 : path);
            notify(msg, NOTE_HINT);
            play(G.snd.shutter, 0.7f);
        } else {
            notify(path, NOTE_ERROR);
        }
        g_shot_pending = 0;
        G.paused = 0;
    }
}

#include "npc.h"

#include <pspgu.h>
#include <pspgum.h>
#include <math.h>
#include <pspkernel.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "gfx.h"
#include "mem.h"
#include "pak.h"
#include "pmap_format.h"

#define DEG2RAD (3.14159265f / 180.0f)
#define MAX_PALETTE 64
#define DRAW_DIST 2200.0f

enum { SEQ_IDLE, SEQ_WALK, SEQ_RUN, SEQ_ATTACK, SEQ_DEATH, SEQ_COUNT };
enum { ST_IDLE, ST_WANDER, ST_CHASE, ST_FLEE, ST_ATTACK, ST_DEAD, ST_SHOOT };

typedef struct {
    char name[32];
    char ai[16];
    float health;
    float mins[3], maxs[3];
    uint32_t first_mesh, num_meshes;
    uint32_t num_bones;
    uint32_t seq[SEQ_COUNT];
    uint32_t first_rag; // into NPCB
} NpcModel;

// Ragdoll skeleton, one per palette bone: nearest skinned ancestor and bind-pose pivot (model space)
typedef struct {
    int32_t parent;
    float pivot[3];
} RagBone;

typedef struct {
    uint32_t first_vertex, num_vertices, first_index, num_indices;
    uint16_t tex, flags;
} NpcMesh;

typedef struct {
    float u, v, nx, ny, nz, x, y, z;
    uint32_t bone;
} NpcVertex;

typedef struct {
    uint32_t num_frames;
    float fps;
    float speed;
    uint32_t loop;
    uint32_t first_matrix;
    uint32_t pad[3];
} NpcSeq;

typedef struct {
    float u, v, nx, ny, nz, x, y, z;
} DrawVert;

// "NPCW": what each NPC type holds: a prop pack model, and the matrix taking that model into the NPC's
// bind pose, to follow one palette bone (the hand)
typedef struct {
    char name[16];
    int32_t bone;
    float m[12];
} NpcWeapon;

// How NPCs use each weapon
typedef struct {
    const char *name;
    float interval;            // seconds between shots in a burst
    float damage, range;       // per pellet
    int burst_min, burst_max;  // shots per burst
    int pellets;
    int melee;                 // swung, not fired
} NpcGun;
static const NpcGun k_guns[] = {
    {"_pistol", 0.4f, 5.0f, 1400.0f, 2, 3, 1, 0},  {"_smg1", 0.1f, 3.0f, 1400.0f, 4, 7, 1, 0},
    {"_ar2", 0.12f, 5.0f, 1800.0f, 3, 5, 1, 0},    {"_shotgun", 0.9f, 3.0f, 700.0f, 1, 2, 6, 0},
    {"_357", 0.8f, 18.0f, 1800.0f, 1, 2, 1, 0},    {"_stunstick", 0.9f, 15.0f, 70.0f, 1, 1, 1, 1},
    {"_crowbar", 0.8f, 12.0f, 70.0f, 1, 1, 1, 1},
    // TF2's mercenaries' guns: a little gentler than TF2's numbers, so a fight lasts a while
    {"_tf_scatter", 0.625f, 5.0f, 900.0f, 1, 2, 6, 0}, {"_tf_rocket", 0.8f, 55.0f, 1600.0f, 1, 2, 1, 0},
    {"_tf_flame", 0.08f, 3.0f, 320.0f, 10, 16, 1, 0},  {"_tf_grenade", 0.6f, 45.0f, 1200.0f, 1, 2, 1, 0},
    {"_tf_minigun", 0.1f, 4.0f, 1300.0f, 10, 18, 2, 0}, {"_tf_shotgun", 0.625f, 5.0f, 800.0f, 1, 2, 6, 0},
    {"_tf_syringe", 0.105f, 5.0f, 1100.0f, 5, 9, 1, 0}, {"_tf_sniper", 1.5f, 50.0f, 2800.0f, 1, 1, 1, 0},
    {"_tf_revolver", 0.5f, 25.0f, 1500.0f, 1, 2, 1, 0},
};
#define NUM_GUNS (int)(sizeof(k_guns) / sizeof(k_guns[0]))

typedef struct {
    int active;
    int type;
    int ai;
    Player mover; // NPCs walk with the player's movement code
    float yaw;
    float health;
    int state;
    float state_time;
    int seq;
    float anim_time;
    float attack_cooldown;
    float hit_done; // this attack swing already dealt damage
    float wander_yaw;
    float stuck_time;
    vec3 last_pos;
    float hurt_cooldown;
    float voice_cooldown; // seconds before it says anything else
    unsigned int ambient;
    int sunlit;
    int light_age;
    // Mirrored from the network (multiplayer clients, other players' puppets)
    vec3 net_pos;
    float net_yaw;
    float speed;   // smoothed, for picking walk/run
    int ragdolled; // died and became a ragdoll: invisible, removed shortly
    // Fighting
    int enemy_kind, enemy_idx; // 0 none, 1 player target, 2 NPC
    int bait;                  // an antlion that bug bait controls (not headcrabs or guards, though they share its AI)
    vec3 enemy_pos;
    float think;               // until the next look round for an enemy
    float fire_timer;
    int burst_left;
    // Mingebags
    unsigned int tint; // players' characters: their Profile colour (0: none)
    float minge_timer; // until its next bit of mischief
    float minge_turn;  // until it switches which way it circles
    int minge_side;    // +1 / -1
    // The held weapon as drawn this frame
    ScePspFMatrix4 weapon_world;
    int weapon_drawn;
} Npc;

#define MAX_PUPPETS 4
static Npc g_puppets[MAX_PUPPETS];
static int g_puppet_type = -1;
static Npc g_self; // the local player in third person

#define RAG_MAX_BONES MAX_PALETTE
typedef struct {
    int active, type, nb;
    int frozen, asleep, spawned; // spawned: from the spawn menu, not a dead NPC
    float age, still, hit_cooldown;
    vec3 p[RAG_MAX_BONES], prev[RAG_MAX_BONES]; // bone pivots now and a step ago (verlet)
    float rest[RAG_MAX_BONES];                  // distance to the parent bone
    float rest_gp[RAG_MAX_BONES];               // distance to the grandparent at death
    int aim[RAG_MAX_BONES];                     // the child bone that steers this bone's rotation
    int aim2[RAG_MAX_BONES];                    // a second child off that line (pelvis: a thigh), fixing the twist
    float rot0[RAG_MAX_BONES][9];               // world rotation at death
    vec3 p0[RAG_MAX_BONES];                     // the points at death (multiplayer clients rebuild from them)
    vec3 dir0[RAG_MAX_BONES], side0[RAG_MAX_BONES]; // directions to the aim children at death
    // Distance constraints: each bone to its parent, plus the children of a branching bone to each
    // other, which keeps the pelvis, chest and hands rigid
    uint8_t ca[RAG_MAX_BONES * 3], cb[RAG_MAX_BONES * 3];
    float crest[RAG_MAX_BONES * 3];
    int nc;
    float scale[RAG_MAX_BONES]; // inflator: each bone's size (1 = normal)
    int8_t stump[RAG_MAX_BONES]; // a hidden (scale 0) bone's mesh closes up at this bone's point (-1: its own)
    int held; // 1 + the bone the physics gun holds (it doesn't give to the constraints), else 0
    vec3 held_from; // where the held bone was before this frame's drag
    unsigned int ambient;
    int sunlit, light_age;
} Ragdoll;
static Ragdoll g_rags[MAX_RAGDOLLS];
// Multiplayer: the host simulates every ragdoll and clients mirror them (npc_ragdoll_net_*)
static int g_rag_remote;                                 // client: don't make or simulate our own
static uint8_t g_rag_gen[MAX_RAGDOLLS];                  // a slot's ragdoll number: 1-255, new each time
static vec3 g_rag_target[MAX_RAGDOLLS][RAG_MAX_BONES];   // client: where the host last had each point
static int g_snd_rag = -1;

// Voices (pack flags bits 8-15, in npcbuild.py's VOICES order): pain, death and spotting-an-enemy lines
enum { VOICE_PAIN, VOICE_DIE, VOICE_ALERT, VOICE_KINDS };
static const char *const k_voices[] = {"", "male", "female", "combine", "metro", "zombie", "fastzombie", "poison",
                                     "antlion", "headcrab", "guard", "vort", "alyx", "barney", "zombine",
                                     "scout", "soldier", "pyro", "demoman", "heavy", "engineer", "medic", "sniper",
                                     "spy"};
#define NUM_VOICES (int)(sizeof(k_voices) / sizeof(k_voices[0]))
static int g_voice_snd[NUM_VOICES][VOICE_KINDS];
static void ragdolls_clear(void);

static void *g_file;
static const NpcModel *g_models;
static int g_num_models;
static const NpcMesh *g_meshes;
static const NpcVertex *g_verts;
static const uint16_t *g_indices;
static const NpcSeq *g_seqs;
static const float *g_mats;
static const PmapTexture *g_textures;
static const RagBone *g_ragbones; // NULL in packs made before ragdolls
static const NpcWeapon *g_weapons; // NULL in packs made before NPC weapons
static const uint32_t *g_flags;    // NPCF: 1 ragdoll only, 2 player model; NULL in older packs
enum { NPCF_RAGDOLL_ONLY = 1, NPCF_PLAYER = 2, NPCF_RELEASE_CRAB = 4, NPCF_WORKSHOP = 8, NPCF_HALVES = 16 };
static const uint64_t *g_crab_bones; // NPCH: per type, the palette bones of its headcrab (NULL in older packs)
static const uint64_t *g_leg_bones;  // NPCL: per type, the palette bones below the waist (NULL in older packs)
static int g_crab_type = -1;          // the Headcrab NPC a zombie's crab becomes
static int g_torso_type = -1;         // the Zombie Torso a sawblade leaves crawling on
static int g_blade_model = -1;        // the spawnable prop (lib 0) that cuts zombies in half
static void (*g_shot)(const char *weapon, vec3 from, vec3 to);
static void (*g_minge)(int npc, int action, vec3 at, vec3 target);

// Bug bait (as in HL2): once a player has used it, antlions are on the players' side for the rest of the
// map. They follow whoever squeezed it last, or swarm the spot it was thrown at, and fight what players fight.
static int g_ant_allies;
static int g_ant_leader = -1; // the target (player) they follow, or -1
static vec3 g_bait_pos;
static float g_bait_time;     // > 0 while they make for a thrown bait
#define BAIT_SWARM_TIME 15.0f
static Npc g_npcs[MAX_NPCS];
static int g_snd_hit = -1, g_snd_die = -1;

int npcs_load(void) {
#ifdef PS2
    // The PS2's 32 MB goes further on maps with low detail; its pack adds the ragdoll-only characters
    const char *path = mem_tier() == MEM_TIER_HIGH ? "data/npcs.ps2.pnpc" : "data/npcs.low.pnpc";
    if (!pak_file_exists(path)) path = "data/npcs.low.pnpc";
#else
    const char *path = mem_tier() == MEM_TIER_HIGH ? "data/npcs.pnpc" : "data/npcs.low.pnpc";
#endif
    if (!pak_file_exists(path)) path = "data/npcs.pnpc";
    unsigned int size;
    g_file = pak_read_file(path, &size);
    if (!g_file) return -1;
    const unsigned char *base = g_file;
#define SEC(var, type, a, b, c, d)                                   \
    do {                                                             \
        const PmapSection *s = pak_find(g_file, PMAP_SECTION_ID(a, b, c, d)); \
        if (!s) return -2;                                           \
        var = (const type *)(base + s->offset);                      \
    } while (0)
    SEC(g_models, NpcModel, 'N', 'P', 'C', 'M');
    SEC(g_meshes, NpcMesh, 'M', 'E', 'S', 'H');
    SEC(g_verts, NpcVertex, 'V', 'E', 'R', 'T');
    SEC(g_indices, uint16_t, 'I', 'N', 'D', 'X');
    SEC(g_seqs, NpcSeq, 'S', 'E', 'Q', 'S');
    SEC(g_mats, float, 'M', 'A', 'T', 'S');
    SEC(g_textures, PmapTexture, 'T', 'E', 'X', 'S');
#undef SEC
    const PmapSection *rs = pak_find(g_file, PMAP_SECTION_ID('N', 'P', 'C', 'B'));
    g_ragbones = rs ? (const RagBone *)(base + rs->offset) : NULL;
    const PmapSection *ws = pak_find(g_file, PMAP_SECTION_ID('N', 'P', 'C', 'W'));
    g_weapons = ws ? (const NpcWeapon *)(base + ws->offset) : NULL;
    const PmapSection *fs = pak_find(g_file, PMAP_SECTION_ID('N', 'P', 'C', 'F'));
    g_flags = fs ? (const uint32_t *)(base + fs->offset) : NULL;
    const PmapSection *hs = pak_find(g_file, PMAP_SECTION_ID('N', 'P', 'C', 'H'));
    g_crab_bones = hs ? (const uint64_t *)(base + hs->offset) : NULL;
    const PmapSection *ls = pak_find(g_file, PMAP_SECTION_ID('N', 'P', 'C', 'L'));
    g_leg_bones = ls ? (const uint64_t *)(base + ls->offset) : NULL;
    g_num_models = (int)pak_find(g_file, PMAP_SECTION_ID('N', 'P', 'C', 'M'))->count;
    g_snd_hit = snd_find("impact_flesh");
    g_snd_die = snd_find("impact_flesh");
    g_snd_rag = snd_find("ragdoll_hit");
    g_crab_type = g_torso_type = -1;
    for (int t = 0; t < g_num_models; t++) {
        if (g_crab_type < 0 && strcmp(g_models[t].name, "Headcrab") == 0) g_crab_type = t;
        // Only a crawling one (with animations) can carry on after the cut
        if (g_torso_type < 0 && strcmp(g_models[t].name, "Zombie Torso") == 0 && g_models[t].seq[SEQ_IDLE] != 0xFFFFFFFFu)
            g_torso_type = t;
    }
    static const char *const kinds[VOICE_KINDS] = {"pain", "die", "alert"};
    for (int v = 0; v < NUM_VOICES; v++) {
        for (int k = 0; k < VOICE_KINDS; k++) {
            char name[32];
            snprintf(name, sizeof(name), "vo_%s_%s", k_voices[v], kinds[k]);
            g_voice_snd[v][k] = v ? snd_find(name) : -1;
        }
    }
    for (int t = 0; t < g_num_models && g_puppet_type < 0; t++) {
        if (strcmp(g_models[t].ai, "citizen") == 0) g_puppet_type = t;
    }
    if (g_puppet_type < 0 && g_num_models) g_puppet_type = 0;
    sceKernelDcacheWritebackAll();
    return 0;
}

int npc_type_count(void) {
    return g_num_models;
}

const char *npc_type_name(int type) {
    return (type >= 0 && type < g_num_models) ? g_models[type].name : "";
}

int npc_type_ragdoll_only(int type) {
    if (type < 0 || type >= g_num_models) return 0;
    return g_flags ? (g_flags[type] & NPCF_RAGDOLL_ONLY) != 0 : strcmp(g_models[type].ai, "ragdoll") == 0;
}

// Has the animations a walking, running player needs
static int animated(int type) {
    return type >= 0 && type < g_num_models && g_models[type].seq[SEQ_IDLE] != 0xFFFFFFFFu &&
           g_models[type].seq[SEQ_WALK] != 0xFFFFFFFFu;
}

int npc_type_workshop(int type) {
    return g_flags && type >= 0 && type < g_num_models && (g_flags[type] & NPCF_WORKSHOP);
}

int npc_type_player_model(int type) {

    return g_flags && type >= 0 && type < g_num_models && (g_flags[type] & NPCF_PLAYER) && animated(type);
}

static int ai_of(const NpcModel *md) {
    if (strcmp(md->ai, "zombie") == 0) return AI_ZOMBIE;
    if (strcmp(md->ai, "antlion") == 0) return AI_ANTLION;
    if (strcmp(md->ai, "combine") == 0) return AI_COMBINE;
    if (strcmp(md->ai, "rebel") == 0) return AI_REBEL;
    if (strcmp(md->ai, "minge") == 0) return AI_MINGE;
    if (strcmp(md->ai, "tf_red") == 0) return AI_TF_RED;
    if (strcmp(md->ai, "tf_blue") == 0) return AI_TF_BLUE;
    return AI_CITIZEN;
}

int npc_spawn(int type, vec3 pos, float yaw) {
    if (type < 0 || type >= g_num_models || npc_type_ragdoll_only(type)) return -1;
    for (int i = 0; i < MAX_NPCS; i++) {
        Npc *n = &g_npcs[i];
        if (n->active) continue;
        memset(n, 0, sizeof(*n));
        n->active = 1;
        n->type = type;
        n->ai = ai_of(&g_models[type]);
        n->bait = n->ai == AI_ANTLION && strncmp(g_models[type].name, "Antlion", 7) == 0 &&
                  !strstr(g_models[type].name, "Guard");
        n->health = g_models[type].health;
        n->yaw = yaw;
        n->wander_yaw = yaw;
        player_spawn(&n->mover, pos);
        n->last_pos = pos;
        n->light_age = 1000;
        return i;
    }
    return -1;
}

void npc_remove(int i) {
    if (i >= 0 && i < MAX_NPCS) g_npcs[i].active = 0;
}

void npc_bugbait_squeeze(int target) {
    g_ant_allies = 1;
    g_ant_leader = target;
    g_bait_time = 0.0f; // back to whoever squeezed it
}

void npc_bugbait_throw(vec3 at) {
    g_ant_allies = 1;
    g_bait_pos = at;
    g_bait_time = BAIT_SWARM_TIME;
}

int npc_antlions_allied(void) {
    return g_ant_allies;
}

void npcs_clear(void) {
    g_ant_allies = 0;
    g_ant_leader = -1;
    g_bait_time = 0.0f;
    memset(g_npcs, 0, sizeof(g_npcs));
    memset(g_puppets, 0, sizeof(g_puppets));
    memset(&g_self, 0, sizeof(g_self));
    ragdolls_clear();
}

int npc_active(int i) {
    return i >= 0 && i < MAX_NPCS && g_npcs[i].active;
}

int npc_alive_count(void) {
    int c = 0;
    for (int i = 0; i < MAX_NPCS; i++) c += g_npcs[i].active;
    return c;
}

static void hull(const Npc *n, vec3 *mins, vec3 *maxs) {
    *mins = v3_add(n->mover.origin, v3(-16, -16, 0));
    *maxs = v3_add(n->mover.origin, v3(16, 16, n->state == ST_DEAD ? 16.0f : 72.0f));
}

int npc_boxes(CmBox *out, int max) {
    int c = 0;
    for (int i = 0; i < MAX_NPCS && c < max; i++) {
        if (!g_npcs[i].active || g_npcs[i].state == ST_DEAD) continue;
        vec3 lo, hi;
        hull(&g_npcs[i], &lo, &hi);
        cm_box_aabb(&out[c], lo, hi, NPC_ID_BASE + i);
        c++;
    }
    for (int i = 0; i < MAX_PUPPETS && c < max; i++) {
        if (!g_puppets[i].active) continue;
        vec3 lo, hi;
        hull(&g_puppets[i], &lo, &hi);
        cm_box_aabb(&out[c], lo, hi, PUPPET_ID_BASE + i);
        c++;
    }
    return c;
}

int npc_raycast(vec3 o, vec3 d, float max_t, float *t_out) {
    int best = -1;
    float best_t = max_t;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (!g_npcs[i].active || g_npcs[i].ragdolled) continue;
        vec3 lo, hi;
        hull(&g_npcs[i], &lo, &hi);
        float tmin = 0.0f, tmax = best_t;
        const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
        int hit = 1;
        for (int k = 0; k < 3 && hit; k++) {
            if (fabsf(dd[k]) < 1e-6f) {
                if (oo[k] < l[k] || oo[k] > h[k]) hit = 0;
            } else {
                float t1 = (l[k] - oo[k]) / dd[k], t2 = (h[k] - oo[k]) / dd[k];
                if (t1 > t2) {
                    float t = t1;
                    t1 = t2;
                    t2 = t;
                }
                if (t1 > tmin) tmin = t1;
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) hit = 0;
            }
        }
        if (hit && tmin < best_t) {
            best_t = tmin;
            best = i;
        }
    }
    if (best >= 0 && t_out) *t_out = best_t;
    return best;
}

// Says a line: pain at most every 1.2 seconds, an alert every 8; a death always
static void npc_say(Npc *n, int kind) {
    int v = g_flags ? (int)((g_flags[n->type] >> 8) & 0xFF) : 0;
    if (v <= 0 || v >= NUM_VOICES || g_voice_snd[v][kind] < 0) return;
    if (kind != VOICE_DIE && n->voice_cooldown > 0.0f) return;
    snd_play_at(g_voice_snd[v][kind], v3(n->mover.origin.x, n->mover.origin.y, n->mover.origin.z + 60.0f), 1.0f);
    n->voice_cooldown = kind == VOICE_ALERT ? 8.0f : 1.2f;
}

void npc_damage(int i, float amount) {
    npc_damage_dir(i, amount, v3(0, 0, 0));
}

static int become_ragdoll(Npc *n, vec3 dv);
static void npc_die(Npc *n, vec3 dv);

void npc_set_blade_model(int model) {
    g_blade_model = model;
}

// A flying sawblade through a zombie, as in HL2: the legs fall as a ragdoll and the top half crawls on
// as a Zombie Torso, headcrab and all. Returns 0 if this NPC can't be cut (then it's an ordinary hit).
static int npc_cut(int i, vec3 dv) {
    Npc *n = &g_npcs[i];
    if (g_torso_type < 0 || !g_flags || !(g_flags[n->type] & NPCF_HALVES) || !g_leg_bones || !g_leg_bones[n->type]) return 0;
    vec3 at = n->mover.origin;
    float yaw = n->yaw;
    int r = become_ragdoll(n, v3_scale(dv, 0.3f));
    if (r >= 0) {
        // Everything above the waist goes with the torso: it shrinks into the top of the legs (the
        // pelvis, the leg bone with no leg bone above it), which closes the cut
        Ragdoll *g = &g_rags[r];
        const RagBone *rb = g_ragbones + g_models[n->type].first_rag;
        uint64_t legs = g_leg_bones[n->type];
        int pelvis = -1;
        for (int b = 0; b < g->nb && pelvis < 0; b++) {
            int par = rb[b].parent;
            if ((legs & ((uint64_t)1 << b)) && (par < 0 || !(legs & ((uint64_t)1 << par)))) pelvis = b;
        }
        for (int b = 0; b < g->nb; b++) {
            if (legs & ((uint64_t)1 << b)) continue;
            g->scale[b] = 0.0f;
            g->stump[b] = (int8_t)pelvis;
        }
    }
    npc_die(n, dv);
    game_blood(v3(at.x, at.y, at.z + 40.0f), v3_norm(dv));
    game_blood(v3(at.x, at.y, at.z + 36.0f), v3(0, 0, 1));
    int t = npc_spawn(g_torso_type, v3(at.x, at.y, at.z + 36.0f), yaw);
    if (t >= 0) {
        g_npcs[t].mover.velocity = v3_add(v3_scale(dv, 0.25f), v3(0, 0, 120.0f)); // knocked off its legs
        g_npcs[t].voice_cooldown = 0.0f;
        g_npcs[t].hurt_cooldown = 0.6f; // the blade that's still passing through doesn't finish it off
        npc_say(&g_npcs[t], VOICE_PAIN);
    }
    return 1;
}

static void npc_die(Npc *n, vec3 dv) {
    become_ragdoll(n, dv); // from the pose it's in right now
    n->state = ST_DEAD;
    n->state_time = 0.0f;
    n->seq = SEQ_DEATH;
    n->anim_time = 0.0f;
    snd_play_at(g_snd_die, n->mover.origin, 1.0f);
}

void npc_damage_dir(int i, float amount, vec3 dv) {
    if (!npc_active(i)) return;
    Npc *n = &g_npcs[i];
    if (n->state == ST_DEAD) return;
    n->health -= amount;
    snd_play_at(g_snd_hit, n->mover.origin, 0.8f);
    if (n->health <= 0.0f) {
        npc_die(n, dv);
        npc_say(n, VOICE_DIE);
        if (n->ai == AI_MINGE && g_minge) g_minge(i, MINGE_DIED, n->mover.origin, n->mover.origin);

        // Its headcrab lives on: off the corpse and after you
        if (g_flags && (g_flags[n->type] & NPCF_RELEASE_CRAB) && g_crab_type >= 0) {
            vec3 at = v3(n->mover.origin.x, n->mover.origin.y, n->mover.origin.z + 24.0f);
            int c = npc_spawn(g_crab_type, at, n->yaw);
            if (c >= 0) g_npcs[c].voice_cooldown = 0.0f;
        }
        return;
    }
    npc_say(n, VOICE_PAIN);
    if (n->ai == AI_CITIZEN || n->ai == AI_MINGE) {
        n->state = ST_FLEE;
        n->state_time = 0.0f;
        n->wander_yaw = n->yaw + 180.0f; // run the other way
    }
    if (n->ai == AI_MINGE && g_minge) g_minge((int)(n - g_npcs), MINGE_HURT, n->mover.origin, n->mover.origin);
}

static const NpcSeq *seq_of(const Npc *n, int kind) {
    uint32_t s = g_models[n->type].seq[kind];
    if (s == 0xFFFFFFFFu && kind == SEQ_RUN) s = g_models[n->type].seq[SEQ_WALK];
    if (s == 0xFFFFFFFFu) s = g_models[n->type].seq[SEQ_IDLE];
    return s == 0xFFFFFFFFu ? NULL : &g_seqs[s];
}

static float move_speed(const Npc *n, int kind) {
    const NpcSeq *s = seq_of(n, kind);
    float sp = s && s->speed > 1.0f ? s->speed : (kind == SEQ_RUN ? 180.0f : 60.0f);
    return sp > 300.0f ? 300.0f : sp;
}

static float yaw_to(vec3 from, vec3 to) {
    return atan2f(to.y - from.y, to.x - from.x) / DEG2RAD;
}

static float turn_towards(float yaw, float target, float max_step) {
    float d = fmodf(target - yaw + 540.0f, 360.0f) - 180.0f;
    if (d > max_step) d = max_step;
    if (d < -max_step) d = -max_step;
    return yaw + d;
}

// An antlion bug bait has put on the players' side
static int bait_ally(const Npc *n) {
    return n->bait && g_ant_allies;
}

static int nearest_hostile(const Npc *self, vec3 *pos) {
    float best = 400.0f;
    int found = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        const Npc *o = &g_npcs[i];
        if (!o->active || o == self || o->state == ST_DEAD || o->ai == AI_CITIZEN || o->ai == AI_REBEL || bait_ally(o)) continue;
        float d = v3_len(v3_sub(o->mover.origin, self->mover.origin));
        if (d < best) {
            best = d;
            *pos = o->mover.origin;
            found = 1;
        }
    }
    return found;
}

// --- Fighting -------------------------------------------------------------------------------------

static const NpcWeapon *weapon_of(const Npc *n);

int npc_weapon(int i, const char **model_name, ScePspFMatrix4 *world) {
    if (i < 0 || i >= MAX_NPCS) return 0;
    const Npc *n = &g_npcs[i];
    if (!n->active || n->ragdolled || !n->weapon_drawn) return 0;
    *model_name = weapon_of(n)->name;
    *world = n->weapon_world;
    return 1;
}

void npc_set_minge_handler(void (*act)(int npc, int action, vec3 at, vec3 target)) {
    g_minge = act;
}

void npc_set_shot_handler(void (*shot)(const char *weapon, vec3 from, vec3 to)) {
    g_shot = shot;
}

static const NpcWeapon *weapon_of(const Npc *n) {
    if (!g_weapons || !g_weapons[n->type].name[0]) return NULL;
    return &g_weapons[n->type];
}

static const NpcGun *gun_of(const Npc *n) {
    const NpcWeapon *w = weapon_of(n);
    if (!w) return NULL;
    for (int k = 0; k < NUM_GUNS; k++) {
        if (strncmp(w->name, k_guns[k].name, 16) == 0) return &k_guns[k];
    }
    return NULL;
}

// Who fights whom. Rebels are on the player's side; everything else armed or with claws isn't.
static int hates_players(int ai) {
    return ai == AI_ZOMBIE || ai == AI_ANTLION || ai == AI_COMBINE || ai == AI_MINGE;
}

static int hates(int ai, int other) {
    switch (ai) {
    case AI_ZOMBIE:
    case AI_ANTLION:
        return other == AI_CITIZEN || other == AI_REBEL || other == AI_COMBINE || other == AI_MINGE;
    case AI_COMBINE:
        return other == AI_REBEL || other == AI_ZOMBIE || other == AI_ANTLION || other == AI_MINGE;
    case AI_REBEL:
        return other == AI_COMBINE || other == AI_ZOMBIE || other == AI_ANTLION;
    case AI_TF_RED: // TF2's teams only fight each other
        return other == AI_TF_BLUE;
    case AI_TF_BLUE:
        return other == AI_TF_RED;

    default:
        return 0;
    }
}

// Once bug bait's been used, the antlions it controls fight the Combine, zombies and mingebags, and rebels
// leave them be
static int npc_hates(const Npc *n, const Npc *o) {
    if (bait_ally(n)) return o->ai == AI_COMBINE || o->ai == AI_ZOMBIE || o->ai == AI_MINGE;
    if (bait_ally(o) && n->ai == AI_REBEL) return 0;
    return hates(n->ai, o->ai);
}

static vec3 eye_of(vec3 feet) {
    return v3(feet.x, feet.y, feet.z + 56.0f);
}

static int can_see(Map *m, vec3 from_feet, vec3 to_feet) {
    Trace tr;
    cm_trace_line(m, eye_of(from_feet), v3(to_feet.x, to_feet.y, to_feet.z + 40.0f), MASK_PROPSOLID, NULL, 0, &tr);
    return tr.fraction >= 0.99f;
}

// The nearest enemy within range (that it can see, for gunners). Sets n->enemy_*.
static void find_enemy(Npc *n, Map *m, const NpcTarget *targets, int num_targets, float range, int need_sight) {
    float best = range;
    int had = n->enemy_kind;
    n->enemy_kind = 0;
    if (hates_players(n->ai) && !bait_ally(n)) {
        for (int t = 0; t < num_targets; t++) {
            if (!targets[t].valid) continue;
            float d = v3_len(v3_sub(targets[t].pos, n->mover.origin));
            if (d < best && (!need_sight || can_see(m, n->mover.origin, targets[t].pos))) {
                best = d;
                n->enemy_kind = 1;
                n->enemy_idx = t;
                n->enemy_pos = targets[t].pos;
            }
        }
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        const Npc *o = &g_npcs[i];
        if (!o->active || o == n || o->state == ST_DEAD || !npc_hates(n, o)) continue;
        float d = v3_len(v3_sub(o->mover.origin, n->mover.origin));
        if (d < best && (!need_sight || can_see(m, n->mover.origin, o->mover.origin))) {
            best = d;
            n->enemy_kind = 2;
            n->enemy_idx = i;
            n->enemy_pos = o->mover.origin;
        }
    }
    if (!had && n->enemy_kind) npc_say(n, VOICE_ALERT); // just spotted one
}

// Keeps the enemy's position current between looks; drops it if it died or left
static void track_enemy(Npc *n, const NpcTarget *targets, int num_targets) {
    if (n->enemy_kind == 1) {
        if (n->enemy_idx < num_targets && targets[n->enemy_idx].valid) {
            n->enemy_pos = targets[n->enemy_idx].pos;
        } else {
            n->enemy_kind = 0;
        }
    } else if (n->enemy_kind == 2) {
        const Npc *o = &g_npcs[n->enemy_idx];
        if (o->active && o->state != ST_DEAD) {
            n->enemy_pos = o->mover.origin;
        } else {
            n->enemy_kind = 0;
        }
    }
}

static float frand(void) {
    return (float)(rand() % 1000) * 0.001f;
}

// Hits the enemy for `amount` (players through the game's damage callback)
static void hurt_enemy(Npc *n, float amount, vec3 dir, void (*damage)(int target, float amount)) {
    if (n->enemy_kind == 1) {
        damage(n->enemy_idx, amount);
    } else if (n->enemy_kind == 2) {
        npc_damage_dir(n->enemy_idx, amount, v3_scale(dir, 120.0f));
    }
}

// One shot (all its pellets) at the enemy. Misses get likelier with distance.
static void fire_at_enemy(Npc *n, const NpcGun *gun, void (*damage)(int target, float amount)) {
    float fy = n->yaw * DEG2RAD;
    vec3 from = v3_add(eye_of(n->mover.origin), v3(cosf(fy) * 24.0f, sinf(fy) * 24.0f, -8.0f));
    vec3 aim = v3(n->enemy_pos.x, n->enemy_pos.y, n->enemy_pos.z + 40.0f);
    float dist = v3_len(v3_sub(aim, from));
    float hit_chance = clampf(0.9f - dist / 2200.0f, 0.25f, 0.9f);
    for (int p = 0; p < gun->pellets; p++) {
        int hit = frand() < hit_chance;
        float spread = hit ? 6.0f : 40.0f + dist * 0.05f;
        vec3 to = v3_add(aim, v3((frand() - 0.5f) * spread, (frand() - 0.5f) * spread, (frand() - 0.5f) * spread));
        if (hit) hurt_enemy(n, gun->damage, v3_norm(v3_sub(to, from)), damage);
        if (g_shot && p == 0) g_shot(gun->name, from, to);
    }
}

static void advance_animation(Npc *n, int want_seq, float dt) {
    if (want_seq != n->seq) {
        n->seq = want_seq;
        n->anim_time = 0.0f;
    }
    float rate = 1.0f;
    if (want_seq == SEQ_WALK || want_seq == SEQ_RUN) {
        const NpcSeq *s = seq_of(n, want_seq);
        if (s && s->speed > 1.0f) rate = clampf(n->speed / s->speed, 0.4f, 2.0f);
    }
    n->anim_time += dt * rate;
}

static int g_ai_disabled, g_ignore_players;

void npc_set_ai(int disabled, int ignore_players) {
    g_ai_disabled = disabled;
    g_ignore_players = ignore_players;
}

void npcs_update(Map *m, float dt, const NpcTarget *targets, int num_targets, PhysWorld *phys,
                 void (*damage)(int target, float amount)) {
    // Players block NPCs
    CmBox pboxes[MAX_PUPPETS + 1];
    int npb = 0;
    for (int t = 0; t < num_targets && npb < MAX_PUPPETS + 1; t++) {
        if (!targets[t].valid) continue;
        cm_box_aabb(&pboxes[npb], v3_add(targets[t].pos, v3(-16, -16, 0)), v3_add(targets[t].pos, v3(16, 16, 72)), -3);
        npb++;
    }
    // Options > NPCs ignore players: players still block them, but nobody targets or follows one
    if (g_ignore_players) num_targets = 0;
    if (g_bait_time > 0.0f) g_bait_time -= dt;
    int ant_leader_ok = g_ant_leader >= 0 && g_ant_leader < num_targets && targets[g_ant_leader].valid;
    for (int i = 0; i < MAX_NPCS; i++) {
        Npc *n = &g_npcs[i];
        if (!n->active) continue;
        n->state_time += dt;
        n->attack_cooldown -= dt;
        n->hurt_cooldown -= dt;
        n->voice_cooldown -= dt;

        // Fast props hurt: a flying barrel or a dropped car kills a zombie
        for (int b = 0; b < MAX_BODIES && n->state != ST_DEAD; b++) {
            Body *body = &phys->bodies[b];
            if (!body->active || body->asleep || n->hurt_cooldown > 0.0f) continue;
            float speed = v3_len(body->vel);
            if (speed < 250.0f) continue;
            vec3 lo, hi, bl, bh;
            hull(n, &lo, &hi);
            phys_body_aabb(body, &bl, &bh);
            if (bl.x > hi.x || bh.x < lo.x || bl.y > hi.y || bh.y < lo.y || bl.z > hi.z || bh.z < lo.z) continue;
            if (body->lib == 0 && body->model == g_blade_model && g_blade_model >= 0 && npc_cut(i, v3_scale(body->vel, 0.5f))) {
                body->vel = v3_scale(body->vel, 0.8f); // it carries on through
                continue;
            }
            npc_damage_dir(i, clampf(speed * body->mass / 300.0f, 5.0f, 300.0f), v3_scale(body->vel, 0.5f));
            game_blood(v3((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.6f), v3_scale(body->vel, 1.0f / speed));
            n->hurt_cooldown = 0.3f;
        }

        // Options > NPC thinking off: they stand where they are (still falling, pushed and hurt)
        if (g_ai_disabled && n->state != ST_DEAD) {
            n->state = ST_IDLE;
            n->enemy_kind = 0;
            n->burst_left = 0;
            MoveCmd idle = {0};
            idle.yaw = n->yaw;
            player_move(&n->mover, m, &idle, pboxes, npb, dt);
            n->speed = sqrtf(n->mover.velocity.x * n->mover.velocity.x + n->mover.velocity.y * n->mover.velocity.y);
            advance_animation(n, SEQ_IDLE, dt);
            continue;
        }

        // Who to fight: looked for a couple of times a second (sight checks cost traces), tracked in between
        const NpcGun *gun = gun_of(n);
        int gunner = gun && !gun->melee;
        n->think -= dt;
        if (n->state != ST_DEAD && n->think <= 0.0f) {
            n->think = 0.4f + (float)(i % 4) * 0.05f;
            if (n->ai != AI_CITIZEN) find_enemy(n, m, targets, num_targets, gunner ? gun->range : 1200.0f, gunner);
        } else {
            track_enemy(n, targets, num_targets);
        }
        float to_enemy = n->enemy_kind ? v3_len(v3_sub(n->enemy_pos, n->mover.origin)) : 1e9f;
        vec3 pp = n->enemy_kind ? n->enemy_pos : n->mover.origin;

        // The nearest player, for rebels to follow
        int near_player = -1;
        float to_player = 1e9f;
        for (int t = 0; t < num_targets; t++) {
            if (!targets[t].valid) continue;
            float d = v3_len(v3_sub(targets[t].pos, n->mover.origin));
            if (d < to_player) {
                to_player = d;
                near_player = t;
            }
        }

        MoveCmd cmd = {0};
        float speed = 0.0f;
        int want_seq = SEQ_IDLE;
        float melee_damage = gun && gun->melee ? gun->damage : n->ai == AI_ANTLION ? 8.0f : 12.0f;
        if (n->ai == AI_MINGE) melee_damage = 4.0f; // annoying, not deadly
        int hop = 0;

        if (n->state == ST_DEAD) {
            want_seq = SEQ_DEATH;
            // The corpse fades out; a ragdolled one lingers briefly so multiplayer clients see the death
            if (n->state_time > (n->ragdolled ? 2.0f : 8.0f)) n->active = 0;
        } else if (n->state == ST_ATTACK) {
            want_seq = SEQ_ATTACK;
            const NpcSeq *s = seq_of(n, SEQ_ATTACK);
            float dur = s ? (float)s->num_frames / s->fps : 1.0f;
            if (n->enemy_kind) n->yaw = turn_towards(n->yaw, yaw_to(n->mover.origin, pp), 360.0f * dt);
            // The hit lands halfway through the swing, if the target is still close
            if (!n->hit_done && n->state_time > dur * 0.5f) {
                n->hit_done = 1;
                if (n->enemy_kind && to_enemy < 80.0f) hurt_enemy(n, melee_damage, v3_norm(v3_sub(pp, n->mover.origin)), damage);
            }
            if (n->state_time >= dur) {
                n->state = ST_CHASE;
                n->state_time = 0.0f;
                n->attack_cooldown = n->ai == AI_MINGE ? 2.0f + frand() * 2.0f : 0.6f;

            }
        } else if (gunner && n->enemy_kind) {
            // Close in to two thirds of the gun's range, then stand, face the enemy and fire bursts
            n->yaw = turn_towards(n->yaw, yaw_to(n->mover.origin, pp), 300.0f * dt);
            if (to_enemy > gun->range * 0.66f) {
                n->state = ST_CHASE;
                speed = move_speed(n, SEQ_RUN);
                want_seq = SEQ_RUN;
            } else {
                n->state = ST_SHOOT;
                n->fire_timer -= dt;
                float facing = fabsf(fmodf(yaw_to(n->mover.origin, pp) - n->yaw + 540.0f, 360.0f) - 180.0f);
                want_seq = n->burst_left > 0 || n->fire_timer < 0.3f ? SEQ_ATTACK : SEQ_IDLE;
                if (n->fire_timer <= 0.0f && facing < 25.0f) {
                    if (n->burst_left <= 0) n->burst_left = gun->burst_min + rand() % (gun->burst_max - gun->burst_min + 1);
                    fire_at_enemy(n, gun, damage);
                    n->burst_left--;
                    n->fire_timer = n->burst_left > 0 ? gun->interval : 0.8f + frand() * 0.9f;
                }
            }
        } else if (n->ai == AI_MINGE && n->enemy_kind == 1 && !(n->state == ST_FLEE && n->state_time < 1.2f)) {
            // A mingebag: runs rings round the player, bunny-hopping, now and then darting in for a crowbar
            // whack; every few seconds it spawns a prop, throws one at them or says something
            float toward = yaw_to(n->mover.origin, pp);
            n->minge_turn -= dt;
            if (n->minge_turn <= 0.0f || !n->minge_side) {
                n->minge_turn = 1.0f + frand() * 2.5f;
                n->minge_side = rand() & 1 ? 1 : -1;
            }
            if (to_enemy < 60.0f && n->attack_cooldown <= 0.0f) {
                n->state = ST_ATTACK;
                n->state_time = 0.0f;
                n->hit_done = 0;
                n->anim_time = 0.0f;
                n->attack_cooldown = 1.5f + frand() * 2.0f;
            } else {
                n->state = ST_CHASE;
                int dart = n->attack_cooldown <= 0.0f && n->minge_turn < 0.6f;
                float aim = to_enemy > 300.0f || dart ? toward : toward + (to_enemy < 140.0f ? 120.0f : 75.0f) * n->minge_side;
                n->yaw = turn_towards(n->yaw, aim, 400.0f * dt);
                n->wander_yaw = n->yaw;
                speed = move_speed(n, SEQ_RUN);
                want_seq = SEQ_RUN;
                hop = frand() < dt * 1.2f;
            }
            n->minge_timer -= dt;
            if (n->minge_timer <= 0.0f) {
                n->minge_timer = 2.0f + frand() * 3.5f;
                int r = rand() % 10;
                int action = r < 4 ? MINGE_SPAWN : r < 7 ? MINGE_THROW : MINGE_CHAT;
                float fy = n->yaw * DEG2RAD;
                vec3 front = v3_add(n->mover.origin, v3(cosf(fy) * 60.0f, sinf(fy) * 60.0f, 40.0f));
                if (g_minge) g_minge((int)(n - g_npcs), action, front, pp);
            }
        } else if (n->enemy_kind && n->ai != AI_CITIZEN) {
            // Claws, or a stunstick: chase and swing
            n->state = ST_CHASE;
            if (to_enemy < 60.0f && n->attack_cooldown <= 0.0f) {
                n->state = ST_ATTACK;
                n->state_time = 0.0f;
                n->hit_done = 0;
                n->anim_time = 0.0f;
            } else {
                int kind = n->ai == AI_ANTLION || n->ai == AI_COMBINE || n->ai == AI_REBEL ? SEQ_RUN : SEQ_WALK;
                speed = move_speed(n, kind);
                want_seq = kind;
                n->yaw = turn_towards(n->yaw, yaw_to(n->mover.origin, pp), 270.0f * dt);
            }
        } else if (bait_ally(n) &&
                   ((g_bait_time > 0.0f && v3_len(v3_sub(g_bait_pos, n->mover.origin)) > 80.0f) ||
                    (g_bait_time <= 0.0f && ant_leader_ok &&
                     v3_len(v3_sub(targets[g_ant_leader].pos, n->mover.origin)) > 180.0f))) {
            // Bug bait: nothing to fight, so make for the thrown bait, or keep up with whoever squeezed it
            vec3 goal = g_bait_time > 0.0f ? g_bait_pos : targets[g_ant_leader].pos;
            n->state = ST_CHASE;
            speed = move_speed(n, SEQ_RUN);
            want_seq = SEQ_RUN;
            n->yaw = turn_towards(n->yaw, yaw_to(n->mover.origin, goal), 300.0f * dt);
        } else if (n->ai == AI_REBEL && near_player >= 0 && to_player > 220.0f && to_player < 3000.0f) {
            // Nothing to fight: keep up with the player
            n->state = ST_CHASE;
            int kind = to_player > 500.0f ? SEQ_RUN : SEQ_WALK;
            speed = move_speed(n, kind);
            want_seq = kind;
            n->yaw = turn_towards(n->yaw, yaw_to(n->mover.origin, targets[near_player].pos), 270.0f * dt);
        } else {
            n->burst_left = 0;
            vec3 threat;
            if ((n->ai == AI_CITIZEN || n->ai == AI_MINGE) && n->state == ST_FLEE && n->state_time < (n->ai == AI_MINGE ? 1.2f : 4.0f)) {
                // Keep running from where it got hurt
                speed = move_speed(n, SEQ_RUN);
                want_seq = SEQ_RUN;
            } else if (n->ai == AI_CITIZEN && nearest_hostile(n, &threat)) {
                if (n->state != ST_FLEE) npc_say(n, VOICE_ALERT); // "Run for your life!"
                n->state = ST_FLEE;
                n->state_time = 0.0f;
                n->wander_yaw = yaw_to(threat, n->mover.origin);
                speed = move_speed(n, SEQ_RUN);
                want_seq = SEQ_RUN;
            } else if (n->ai == AI_REBEL && to_player <= 220.0f) {
                // Standing by the player
                n->state = ST_IDLE;
            } else {
                // Wander: walk a few seconds, idle a few seconds
                if (n->state != ST_WANDER && n->state != ST_IDLE) {
                    n->state = ST_IDLE;
                    n->state_time = 0.0f;
                }
                if (n->state == ST_IDLE && n->state_time > 2.0f + (float)(i % 3)) {
                    n->state = ST_WANDER;
                    n->state_time = 0.0f;
                    n->wander_yaw = n->yaw + (float)((rand() % 240) - 120);
                } else if (n->state == ST_WANDER && n->state_time > 4.0f) {
                    n->state = ST_IDLE;
                    n->state_time = 0.0f;
                }
                if (n->state == ST_WANDER) {
                    speed = move_speed(n, SEQ_WALK);
                    want_seq = SEQ_WALK;
                }
            }
            if (speed > 0.0f) n->yaw = turn_towards(n->yaw, n->wander_yaw, 180.0f * dt);
        }

        // Stuck against a wall? Pick a new direction.
        if (speed > 0.0f) {
            float moved = v3_len(v3_sub(n->mover.origin, n->last_pos));
            n->stuck_time = moved < speed * dt * 0.25f ? n->stuck_time + dt : 0.0f;
            if (n->stuck_time > 0.6f) {
                n->wander_yaw = n->yaw + 90.0f + (float)(rand() % 180);
                n->yaw = n->wander_yaw;
                n->stuck_time = 0.0f;
            }
        }
        n->last_pos = n->mover.origin;

        if (n->state != ST_DEAD) {
            cmd.forward = speed / 250.0f;
            cmd.walk_speed = speed;
            cmd.yaw = n->yaw;
            cmd.jump = hop;

            player_move(&n->mover, m, &cmd, pboxes, npb, dt);
        }
        n->speed = sqrtf(n->mover.velocity.x * n->mover.velocity.x + n->mover.velocity.y * n->mover.velocity.y);
        advance_animation(n, want_seq, dt);
    }
}

// --- Multiplayer mirroring ---------------------------------------------------------------------

void npc_net_get(int i, NpcNet *out) {
    const Npc *n = &g_npcs[i];
    memset(out, 0, sizeof(*out));
    out->active = (uint8_t)n->active;
    if (!n->active) return;
    out->type = (uint8_t)n->type;
    out->seq = (uint8_t)n->seq;
    out->dead = n->state == ST_DEAD;
    out->pos[0] = n->mover.origin.x;
    out->pos[1] = n->mover.origin.y;
    out->pos[2] = n->mover.origin.z;
    out->yaw = n->yaw;
}

void npc_net_set(int i, const NpcNet *in) {
    Npc *n = &g_npcs[i];
    if (!in->active || in->type >= g_num_models) {
        n->active = 0;
        return;
    }
    vec3 pos = v3(in->pos[0], in->pos[1], in->pos[2]);
    if (!n->active || n->type != in->type) {
        memset(n, 0, sizeof(*n));
        n->active = 1;
        n->type = in->type;
        n->ai = ai_of(&g_models[in->type]);
        n->mover.origin = pos;
        n->yaw = in->yaw;
        n->light_age = 1000;
    }
    n->net_pos = pos;
    n->net_yaw = in->yaw;
    if (in->dead && n->state != ST_DEAD) {
        npc_say(n, VOICE_DIE); // a client hears the host's NPC die
        n->state = ST_DEAD;
        n->state_time = 0.0f;
        become_ragdoll(n, v3(0, 0, 0)); // our own copy of the host's ragdoll
    } else if (!in->dead) {
        n->state = ST_IDLE;
    }
    if (in->seq != n->seq && in->seq < SEQ_COUNT) {
        n->seq = in->seq;
        n->anim_time = 0.0f;
    }
}

// Slides a mirrored character toward its last received position and facing
static void follow_net(Npc *n, float dt) {
    float k = clampf(dt * 12.0f, 0.0f, 1.0f);
    vec3 before = n->mover.origin;
    vec3 d = v3_sub(n->net_pos, before);
    if (v3_len(d) > 300.0f) {
        n->mover.origin = n->net_pos; // teleported
    } else {
        n->mover.origin = v3_mad(before, d, k);
    }
    n->yaw = turn_towards(n->yaw, n->net_yaw, 720.0f * dt);
    if (dt > 0.0f) {
        vec3 v = v3_sub(n->mover.origin, before);
        float hs = sqrtf(v.x * v.x + v.y * v.y) / dt;
        n->speed += (hs - n->speed) * clampf(dt * 8.0f, 0.0f, 1.0f);
    }
}

void npcs_animate(float dt) {
    for (int i = 0; i < MAX_NPCS; i++) {
        Npc *n = &g_npcs[i];
        if (!n->active) continue;
        n->state_time += dt;
        follow_net(n, dt);
        advance_animation(n, n->seq, dt);
    }
}

// A player's character: (re)made when it appears or changes model. Returns 0 if there's nothing to show.
static int player_body(Npc *n, int active, int type, vec3 pos, float yaw) {
    if (!animated(type)) type = g_puppet_type;
    if (!active || type < 0) {
        n->active = 0;
        return 0;
    }
    if (!n->active || n->type != type) {
        memset(n, 0, sizeof(*n));
        n->active = 1;
        n->type = type;
        n->mover.origin = pos;
        n->yaw = yaw;
        n->light_age = 1000;
    }
    return 1;
}

static void player_body_animate(Npc *n, float dt) {
    int want = n->speed > 140.0f ? SEQ_RUN : n->speed > 20.0f ? SEQ_WALK : SEQ_IDLE;
    advance_animation(n, want, dt);
}

void npc_puppet_set(int slot, int active, int type, vec3 pos, float yaw, float dt) {
    if (slot < 0 || slot >= MAX_PUPPETS) return;
    Npc *n = &g_puppets[slot];
    if (!player_body(n, active, type, pos, yaw)) return;
    n->net_pos = pos;
    n->net_yaw = yaw;
    follow_net(n, dt);
    player_body_animate(n, dt);
}

void npc_puppet_tint(int slot, unsigned int abgr) {
    Npc *n = slot < 0 ? &g_self : slot < MAX_PUPPETS ? &g_puppets[slot] : NULL;
    if (n) n->tint = abgr;
}

void npc_self_set(int active, int type, vec3 pos, float yaw, float dt) {
    Npc *n = &g_self;
    if (!player_body(n, active, type, pos, yaw)) return;
    // Exactly where the player is (the camera follows it): no smoothing as for network puppets
    vec3 before = n->mover.origin;
    n->mover.origin = pos;
    n->yaw = yaw;
    if (dt > 0.0f) {
        float hs = sqrtf((pos.x - before.x) * (pos.x - before.x) + (pos.y - before.y) * (pos.y - before.y)) / dt;
        if (hs > 1000.0f) hs = 0.0f; // teleported
        n->speed += (hs - n->speed) * clampf(dt * 8.0f, 0.0f, 1.0f);
    }
    player_body_animate(n, dt);
}

// --- Rendering --------------------------------------------------------------------------------

static void blended_palette(const Npc *n, float out[][12]) {
    const NpcModel *md = &g_models[n->type];
    int kind = n->seq;
    const NpcSeq *s = (kind == SEQ_DEATH) ? seq_of(n, SEQ_IDLE) : seq_of(n, kind);
    int nb = (int)md->num_bones > MAX_PALETTE ? MAX_PALETTE : (int)md->num_bones;
    if (!s) {
        for (int b = 0; b < nb; b++) {
            const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            memcpy(out[b], id, sizeof(id));
        }
        return;
    }
    float f = n->anim_time * s->fps;
    int nf = (int)s->num_frames;
    int f0, f1;
    float t;
    if (s->loop || kind == SEQ_DEATH) {
        float wrapped = fmodf(f, (float)nf);
        f0 = (int)wrapped;
        f1 = (f0 + 1) % nf;
        t = wrapped - f0;
    } else {
        if (f > nf - 1) f = (float)(nf - 1);
        f0 = (int)f;
        f1 = f0 + 1 < nf ? f0 + 1 : f0;
        t = f - f0;
    }
    if (kind == SEQ_DEATH) { // no death animation: freeze the idle pose
        f0 = f1 = 0;
        t = 0.0f;
    }
    const float *a = g_mats + ((size_t)s->first_matrix + (size_t)f0 * md->num_bones) * 12;
    const float *b = g_mats + ((size_t)s->first_matrix + (size_t)f1 * md->num_bones) * 12;
    for (int k = 0; k < nb; k++) {
        for (int j = 0; j < 12; j++) out[k][j] = a[k * 12 + j] + (b[k * 12 + j] - a[k * 12 + j]) * t;
    }
}

static unsigned int scale_abgr(unsigned int c, float k) {
    unsigned int out = c & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float v = (float)((c >> s) & 0xFF) * k;
        out |= (v > 255.0f ? 255u : (unsigned int)v) << s;
    }
    return out;
}

// --- Ragdolls -------------------------------------------------------------------------------------
// Verlet ragdolls: a point at each skinned bone's pivot, held at its bind-pose distance from its
// parent bone (and kept from folding through its grandparent), colliding with the world and props
// as a small box. Each bone's rotation is its rotation at death turned by however much the line to
// its main child bone has swung since, which is enough to skin the mesh convincingly.

#define RAG_GRAVITY 600.0f
#define RAG_RADIUS 5.0f // about a limb's thickness, so bodies don't sink into the floor
#define RAG_ITERATIONS 10
#define RAG_SCALE_MIN 0.2f // inflator limits
#define RAG_SCALE_MAX 4.0f
#define RAG_PICK_RADIUS 10.0f
#define RAG_MAX_BOXES 24

static void ragdolls_clear(void) {
    memset(g_rags, 0, sizeof(g_rags));
}

static int g_rag_dbg[2]; // startsolid traces, hits (debug)
void npc_ragdoll_debug(int *startsolid, int *hits, int *asleep) {
    *startsolid = g_rag_dbg[0];
    *hits = g_rag_dbg[1];
    *asleep = g_rags[0].asleep;
}

static int rag_slot(void) {
    int best = 0;
    float oldest = -1.0f;
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        if (!g_rags[r].active) return r;
        // Reuse the oldest, dead NPCs before ragdolls spawned from the menu
        float age = g_rags[r].age + (g_rags[r].spawned ? 0.0f : 100000.0f);
        if (age > oldest) {
            oldest = age;
            best = r;
        }
    }
    return best;
}

// World-space skin matrices (3 rows of rotation | translation) for an NPC's current pose
static void world_pose(const Npc *n, float (*out)[12]) {
    static float pal[MAX_PALETTE][12];
    blended_palette(n, pal);
    float c = cosf(n->yaw * DEG2RAD), s = sinf(n->yaw * DEG2RAD);
    vec3 o = n->mover.origin;
    int nb = (int)g_models[n->type].num_bones < MAX_PALETTE ? (int)g_models[n->type].num_bones : MAX_PALETTE;
    for (int b = 0; b < nb; b++) {
        for (int j = 0; j < 4; j++) {
            float x = pal[b][j], y = pal[b][4 + j], z = pal[b][8 + j];
            out[b][j] = c * x - s * y;
            out[b][4 + j] = s * x + c * y;
            out[b][8 + j] = z;
        }
        out[b][3] += o.x;
        out[b][7] += o.y;
        out[b][11] += o.z;
    }
}

// Everything else a ragdoll needs, from its points and rotations at death: bone lengths, which child
// steers each bone's rotation, and the bracing constraints
static void rag_build(Ragdoll *g, const RagBone *rb) {
    for (int b = 0; b < g->nb; b++) {
        int par = rb[b].parent;
        if (par < 0 || par >= b) continue;
        g->rest[b] = v3_len(v3_sub(g->p[b], g->p[par]));
        int gp = rb[par].parent;
        if (gp >= 0 && gp < par) g->rest_gp[b] = v3_len(v3_sub(g->p[b], g->p[gp]));
        // A bone's rotation follows its farthest child
        float best = g->aim[par] >= 0 ? v3_len(v3_sub(g->p[g->aim[par]], g->p[par])) : 0.5f;
        if (g->rest[b] > best) g->aim[par] = b;
        if (g->nc < RAG_MAX_BONES * 3) {
            g->ca[g->nc] = (uint8_t)par, g->cb[g->nc] = (uint8_t)b, g->crest[g->nc] = g->rest[b];
            g->nc++;
        }
    }
    for (int b = 0; b < g->nb; b++) {
        g->aim2[b] = -1;
        if (g->aim[b] < 0) continue;
        g->dir0[b] = v3_norm(v3_sub(g->p[g->aim[b]], g->p[b]));
        // The second child: the one farthest off the aim line
        float best = 1.0f;
        for (int c = b + 1; c < g->nb; c++) {
            if (rb[c].parent != b || c == g->aim[b]) continue;
            vec3 d = v3_sub(g->p[c], g->p[b]);
            vec3 off = v3_mad(d, g->dir0[b], -v3_dot(d, g->dir0[b]));
            float l = v3_len(off);
            if (l > best) {
                best = l;
                g->aim2[b] = c;
                g->side0[b] = v3_scale(off, 1.0f / l);
            }
        }
        // Brace the children of a branching bone against each other
        for (int c1 = b + 1; c1 < g->nb; c1++) {
            if (rb[c1].parent != b) continue;
            for (int c2 = c1 + 1; c2 < g->nb && g->nc < RAG_MAX_BONES * 3; c2++) {
                if (rb[c2].parent != b) continue;
                float d = v3_len(v3_sub(g->p[c1], g->p[c2]));
                if (d < 0.5f) continue;
                g->ca[g->nc] = (uint8_t)c1, g->cb[g->nc] = (uint8_t)c2, g->crest[g->nc] = d;
                g->nc++;
            }
        }
    }
}

static int rag_create(int type, float (*pose)[12], vec3 vel) {
    // A multiplayer client's ragdolls are the host's (npc_ragdoll_net_set_info): its own would take their slots
    if (!g_ragbones || g_rag_remote || type < 0 || type >= g_num_models) return -1;
    const NpcModel *md = &g_models[type];
    const RagBone *rb = g_ragbones + md->first_rag;
    int r = rag_slot();
    Ragdoll *g = &g_rags[r];
    memset(g, 0, sizeof(*g));
    g->active = 1;
    g->type = type;
    g->nb = (int)md->num_bones < RAG_MAX_BONES ? (int)md->num_bones : RAG_MAX_BONES;
    g->light_age = 1000;
    for (int b = 0; b < g->nb; b++) {
        const float *m = pose[b], *pv = rb[b].pivot;
        g->p[b] = v3(m[0] * pv[0] + m[1] * pv[1] + m[2] * pv[2] + m[3], m[4] * pv[0] + m[5] * pv[1] + m[6] * pv[2] + m[7],
                     m[8] * pv[0] + m[9] * pv[1] + m[10] * pv[2] + m[11]);
        float *R = g->rot0[b];
        R[0] = m[0], R[1] = m[1], R[2] = m[2];
        R[3] = m[4], R[4] = m[5], R[5] = m[6];
        R[6] = m[8], R[7] = m[9], R[8] = m[10];
        g->prev[b] = v3_mad(g->p[b], vel, -1.0f / 60.0f);
        g->aim[b] = -1;
        g->scale[b] = 1.0f;
        g->stump[b] = -1;
    }
    memcpy(g->p0, g->p, sizeof(vec3) * (size_t)g->nb);
    g_rag_gen[r] = (uint8_t)(g_rag_gen[r] % 255 + 1);
    rag_build(g, rb);
    return r;
}

static int become_ragdoll(Npc *n, vec3 dv) {
    if (!g_ragbones || n->ragdolled) return -1;
    if (g_rag_remote) {
        n->ragdolled = 1; // the host's ragdoll arrives over the network
        return -1;
    }
    static float pose[MAX_PALETTE][12];
    world_pose(n, pose);
    vec3 vel = v3_add(v3(n->mover.velocity.x, n->mover.velocity.y, 0.0f), dv);
    int r = rag_create(n->type, pose, vel);
    if (r < 0) return -1;
    n->ragdolled = 1;
    // A zombie's headcrab has jumped off (as an NPC of its own): hide it on the corpse
    if (g_crab_bones && g_flags && (g_flags[n->type] & NPCF_RELEASE_CRAB) && g_crab_type >= 0) {
        for (int b = 0; b < g_rags[r].nb; b++) {
            if (g_crab_bones[n->type] & ((uint64_t)1 << b)) g_rags[r].scale[b] = 0.0f;
        }
    }
    return r;
}

int npc_ragdoll_spawn(int type, vec3 pos, float yaw) {
    if (!g_ragbones || type < 0 || type >= g_num_models) return -1;
    static Npc tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.type = type;
    tmp.seq = SEQ_IDLE;
    tmp.mover.origin = pos;
    tmp.yaw = yaw;
    static float pose[MAX_PALETTE][12];
    world_pose(&tmp, pose);
    int r = rag_create(type, pose, v3(0, 0, 0));
    if (r >= 0) g_rags[r].spawned = 1;
    return r;
}

int npc_ragdolls_available(void) {
    return g_ragbones != NULL;
}

int npc_ragdoll_active(int r) {
    return r >= 0 && r < MAX_RAGDOLLS && g_rags[r].active;
}

void npc_ragdoll_remove(int r) {
    if (r >= 0 && r < MAX_RAGDOLLS) g_rags[r].active = 0;
}

vec3 npc_ragdoll_point(int r, int bone) {
    return g_rags[r].p[bone];
}

static void rag_wake(Ragdoll *g) {
    g->asleep = 0;
    g->still = 0.0f;
}

void npc_ragdoll_push(int r, int bone, vec3 dv) {
    if (g_rag_remote || !npc_ragdoll_active(r) || bone < 0 || bone >= g_rags[r].nb) return;
    Ragdoll *g = &g_rags[r];
    if (g->frozen) return;
    rag_wake(g);
    g->prev[bone] = v3_mad(g->prev[bone], dv, -1.0f / 60.0f);
}

void npc_ragdoll_drag(int r, int bone, vec3 target, float dt) {
    if (g_rag_remote || !npc_ragdoll_active(r) || bone < 0 || bone >= g_rags[r].nb) return;
    Ragdoll *g = &g_rags[r];
    g->frozen = 0;
    rag_wake(g);
    // Move the grabbed point most of the way; verlet turns the motion into velocity, so it throws
    g->held_from = g->p[bone];
    g->p[bone] = v3_lerp(g->p[bone], target, clampf(dt * 12.0f, 0.0f, 1.0f));
    g->held = bone + 1; // the body hangs from it (until the next step)
}

float npc_ragdoll_inflate(int r, int bone, float factor) {
    if (!npc_ragdoll_active(r) || bone < 0 || bone >= g_rags[r].nb) return 0.0f;
    float *s = &g_rags[r].scale[bone];
    *s = clampf(*s * factor, RAG_SCALE_MIN, RAG_SCALE_MAX);
    return *s;
}

void npc_ragdoll_freeze(int r, int frozen) {
    if (!npc_ragdoll_active(r)) return;
    Ragdoll *g = &g_rags[r];
    g->frozen = frozen;
    if (frozen) {
        for (int b = 0; b < g->nb; b++) g->prev[b] = g->p[b];
    } else {
        rag_wake(g);
    }
}

// --- Multiplayer ragdolls ---------------------------------------------------------------------------
// The host sends each ragdoll's points and rotations at death once (RagInfo, repeated now and then in
// case it's lost), and a client builds the same skeleton from them with rag_build. After that the host
// streams the points (RagPose) and the client glides its copy to them.

void npc_ragdolls_set_remote(int on) {
    g_rag_remote = on;
}

int npc_ragdoll_gen(int r) {
    return npc_ragdoll_active(r) ? g_rag_gen[r] : 0;
}

int npc_ragdoll_awake(int r) {
    return npc_ragdoll_active(r) && (!g_rags[r].asleep || g_rags[r].held) && !g_rags[r].frozen;
}

static int16_t q8(float v) { // a point relative to the ragdoll's origin, in 1/8 units
    float s = v * 8.0f;
    return (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : (int)lrintf(s));
}

void npc_ragdoll_net_info(int r, RagInfo *out) {
    const Ragdoll *g = &g_rags[r];
    memset(out, 0, sizeof(*out));
    out->slot = (uint8_t)r;
    out->gen = g_rag_gen[r];
    out->type = (uint8_t)g->type;
    out->nb = (uint8_t)(g->nb < RAG_NET_MAX_BONES ? g->nb : RAG_NET_MAX_BONES);
    out->spawned = (uint8_t)g->spawned;
    vec3 o = g->p0[0];
    out->origin[0] = o.x, out->origin[1] = o.y, out->origin[2] = o.z;
    // The rotations at death are a rotation times the character's (uniform) size
    const float *R0 = g->rot0[0];
    out->size = sqrtf(R0[0] * R0[0] + R0[3] * R0[3] + R0[6] * R0[6]);
    float inv = out->size > 1e-6f ? 1.0f / out->size : 1.0f;
    for (int b = 0; b < out->nb; b++) {
        vec3 d = v3_sub(g->p0[b], o);
        out->pos[b][0] = q8(d.x), out->pos[b][1] = q8(d.y), out->pos[b][2] = q8(d.z);
        // Rotation matrix (rows) to a quaternion
        const float *m = g->rot0[b];
        float a = m[0] * inv, e = m[4] * inv, i = m[8] * inv, w, x, y, z;
        float tr = a + e + i;
        if (tr > 0.0f) {
            float s = sqrtf(tr + 1.0f) * 2.0f;
            w = 0.25f * s, x = (m[7] - m[5]) * inv / s, y = (m[2] - m[6]) * inv / s, z = (m[3] - m[1]) * inv / s;
        } else if (a > e && a > i) {
            float s = sqrtf(1.0f + a - e - i) * 2.0f;
            w = (m[7] - m[5]) * inv / s, x = 0.25f * s, y = (m[1] + m[3]) * inv / s, z = (m[2] + m[6]) * inv / s;
        } else if (e > i) {
            float s = sqrtf(1.0f + e - a - i) * 2.0f;
            w = (m[2] - m[6]) * inv / s, x = (m[1] + m[3]) * inv / s, y = 0.25f * s, z = (m[5] + m[7]) * inv / s;
        } else {
            float s = sqrtf(1.0f + i - a - e) * 2.0f;
            w = (m[3] - m[1]) * inv / s, x = (m[2] + m[6]) * inv / s, y = (m[5] + m[7]) * inv / s, z = 0.25f * s;
        }
        out->rot[b][0] = (int16_t)lrintf(x * 32767.0f), out->rot[b][1] = (int16_t)lrintf(y * 32767.0f);
        out->rot[b][2] = (int16_t)lrintf(z * 32767.0f), out->rot[b][3] = (int16_t)lrintf(w * 32767.0f);
        float sc = g->scale[b] * 32.0f;
        out->scale[b] = (uint8_t)(sc > 255.0f ? 255 : sc < 0.0f ? 0 : (int)lrintf(sc));
        out->stump[b] = g->stump[b];
    }
}

int npc_ragdoll_net_pose(int r, RagPose *out) {
    const Ragdoll *g = &g_rags[r];
    out->slot = (uint8_t)r;
    out->gen = g_rag_gen[r];
    out->nb = (uint8_t)(g->nb < RAG_NET_MAX_BONES ? g->nb : RAG_NET_MAX_BONES);
    out->flags = (uint8_t)(g->frozen ? 1 : 0);
    vec3 o = g->p[0];
    out->origin[0] = o.x, out->origin[1] = o.y, out->origin[2] = o.z;
    for (int b = 0; b < out->nb; b++) {
        vec3 d = v3_sub(g->p[b], o);
        out->pos[b][0] = q8(d.x), out->pos[b][1] = q8(d.y), out->pos[b][2] = q8(d.z);
    }
    return (int)(offsetof(RagPose, pos) + (size_t)out->nb * sizeof(out->pos[0]));
}

void npc_ragdoll_net_set_info(const RagInfo *in) {
    if (!g_ragbones || in->slot >= MAX_RAGDOLLS || in->type >= g_num_models || in->nb == 0) return;
    Ragdoll *g = &g_rags[in->slot];
    int nb = in->nb < RAG_MAX_BONES ? in->nb : RAG_MAX_BONES;
    if (!g->active || g_rag_gen[in->slot] != in->gen || g->type != in->type) {
        const NpcModel *md = &g_models[in->type];
        if ((int)md->num_bones < nb) return; // a different NPC pack from the host's
        memset(g, 0, sizeof(*g));
        g->active = 1;
        g->type = in->type;
        g->nb = nb;
        g->spawned = in->spawned;
        g->light_age = 1000;
        vec3 o = v3(in->origin[0], in->origin[1], in->origin[2]);
        for (int b = 0; b < nb; b++) {
            g->p[b] = g->prev[b] = g->p0[b] = g_rag_target[in->slot][b] =
                v3_add(o, v3(in->pos[b][0] / 8.0f, in->pos[b][1] / 8.0f, in->pos[b][2] / 8.0f));
            float x = in->rot[b][0] / 32767.0f, y = in->rot[b][1] / 32767.0f, z = in->rot[b][2] / 32767.0f,
                  w = in->rot[b][3] / 32767.0f, s = in->size;
            float *R = g->rot0[b];
            R[0] = (1 - 2 * (y * y + z * z)) * s, R[1] = 2 * (x * y - w * z) * s, R[2] = 2 * (x * z + w * y) * s;
            R[3] = 2 * (x * y + w * z) * s, R[4] = (1 - 2 * (x * x + z * z)) * s, R[5] = 2 * (y * z - w * x) * s;
            R[6] = 2 * (x * z - w * y) * s, R[7] = 2 * (y * z + w * x) * s, R[8] = (1 - 2 * (x * x + y * y)) * s;
            g->aim[b] = -1;
        }
        rag_build(g, g_ragbones + md->first_rag);
        g_rag_gen[in->slot] = in->gen;
    }
    // Size (inflator) and sawblade cuts can change any time
    for (int b = 0; b < nb; b++) {
        g->scale[b] = in->scale[b] / 32.0f;
        g->stump[b] = in->stump[b];
    }
}

void npc_ragdoll_net_set_pose(const RagPose *in) {
    if (in->slot >= MAX_RAGDOLLS) return;
    Ragdoll *g = &g_rags[in->slot];
    if (!g->active || g_rag_gen[in->slot] != in->gen) return; // its info hasn't arrived yet
    int nb = in->nb < g->nb ? in->nb : g->nb;
    vec3 o = v3(in->origin[0], in->origin[1], in->origin[2]);
    for (int b = 0; b < nb; b++) {
        g_rag_target[in->slot][b] = v3_add(o, v3(in->pos[b][0] / 8.0f, in->pos[b][1] / 8.0f, in->pos[b][2] / 8.0f));
    }
    g->frozen = in->flags & 1;
}

void npc_ragdoll_net_keep(uint8_t active_mask, const uint8_t *gens) {
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        if (g_rags[r].active && (!(active_mask & (1u << r)) || gens[r] != g_rag_gen[r])) g_rags[r].active = 0;
    }
}

int npc_ragdoll_raycast(vec3 o, vec3 d, float max_t, float *t_out, int *bone_out) {
    int best = -1;
    float best_t = max_t, r2 = RAG_PICK_RADIUS * RAG_PICK_RADIUS;
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        const Ragdoll *g = &g_rags[r];
        if (!g->active) continue;
        for (int b = 0; b < g->nb; b++) {
            vec3 oc = v3_sub(g->p[b], o);
            float tc = v3_dot(oc, d);
            if (tc < 0.0f) continue;
            float d2 = v3_dot(oc, oc) - tc * tc;
            if (d2 > r2) continue;
            float th = tc - sqrtf(r2 - d2);
            if (th < best_t) {
                best_t = th;
                best = r;
                if (bone_out) *bone_out = b;
            }
        }
    }
    if (best >= 0 && t_out) *t_out = best_t;
    return best;
}

// By the bits: -ffast-math assumes floats are always finite, so x == x and isfinite() fold to 1
static int finite3(vec3 v) {
    const float f[3] = {v.x, v.y, v.z};
    for (int k = 0; k < 3; k++) {
        uint32_t u;
        memcpy(&u, &f[k], 4);
        if (((u >> 23) & 0xFF) == 0xFF) return 0; // Inf or NaN
    }
    return 1;
}

static void rag_step(Ragdoll *g, Map *m, float dt, PhysWorld *phys, const CmBox *boxes, int nboxes) {
    g->age += dt;
    g->hit_cooldown -= dt;
    if (g->frozen || g->asleep) return;
    const RagBone *rb = g_ragbones + g_models[g->type].first_rag;
    vec3 rmin = v3(-RAG_RADIUS, -RAG_RADIUS, -RAG_RADIUS), rmax = v3(RAG_RADIUS, RAG_RADIUS, RAG_RADIUS);
    float impact = 0.0f;
    static vec3 safe[RAG_MAX_BONES];

    int held = g->held - 1;
    g->held = 0;

    // Integrate and collide each point
    for (int b = 0; b < g->nb; b++) {
        if (b == held) { // the physics gun has already placed it
            g->prev[b] = g->held_from;
            safe[b] = g->p[b];
            continue;
        }
        vec3 v = v3_scale(v3_sub(g->p[b], g->prev[b]), 0.995f);
        vec3 np = v3_add(v3_add(g->p[b], v), v3(0, 0, -RAG_GRAVITY * dt * dt));
        Trace tr;
        cm_trace(m, g->p[b], np, rmin, rmax, MASK_PROPSOLID, boxes, nboxes, &tr);
        if (tr.startsolid) {
            // Touching or inside something: stay put rather than sink through it
            g_rag_dbg[0]++;
            np = g->p[b];
            g->prev[b] = np;
        } else if (tr.fraction < 1.0f) {
            g_rag_dbg[1]++;
            np = v3_mad(tr.endpos, tr.normal, 0.05f);
            float vn = v3_dot(v, tr.normal);
            vec3 vt = v3_mad(v, tr.normal, -vn);
            if (-vn / dt > impact) impact = -vn / dt;
            g->prev[b] = v3_mad(np, vt, -0.7f); // keep most of the slide, lose the bounce
        } else {
            g->prev[b] = g->p[b];
        }
        g->p[b] = np;
        safe[b] = np;
    }

    // Fast props shove the points they pass through
    for (int i = 0; i < MAX_BODIES; i++) {
        const Body *bd = &phys->bodies[i];
        if (!bd->active || bd->asleep || v3_dot(bd->vel, bd->vel) < 120.0f * 120.0f) continue;
        vec3 lo, hi;
        phys_body_aabb(bd, &lo, &hi);
        for (int b = 0; b < g->nb; b++) {
            vec3 p = g->p[b];
            if (p.x < lo.x || p.x > hi.x || p.y < lo.y || p.y > hi.y || p.z < lo.z || p.z > hi.z) continue;
            g->prev[b] = v3_mad(p, bd->vel, -dt);
        }
    }

    // Bone lengths and braces. Sweeping both ways each iteration spreads a correction along the whole
    // body at once, so a body hanging from one bone doesn't stretch apart.
    for (int it = 0; it < RAG_ITERATIONS; it++) {
        for (int s = 0; s < g->nc; s++) {
            int i = (it & 1) ? g->nc - 1 - s : s;
            int a = g->ca[i], b = g->cb[i];
            float wa = a == held ? 0.0f : 1.0f, wb = b == held ? 0.0f : 1.0f;
            if (wa + wb == 0.0f) continue;
            vec3 d = v3_sub(g->p[b], g->p[a]);
            float len = v3_len(d);
            if (len < 1e-4f) continue;
            float k = (len - g->crest[i]) / (len * (wa + wb));
            g->p[a] = v3_mad(g->p[a], d, k * wa);
            g->p[b] = v3_mad(g->p[b], d, -k * wb);
        }
        // Don't fold a limb back through its grandparent
        for (int b = 0; b < g->nb; b++) {
            int par = rb[b].parent;
            if (par < 0 || par >= b) continue;
            int gp = rb[par].parent;
            if (g->rest_gp[b] <= 0.0f || gp < 0) continue;
            vec3 d2 = v3_sub(g->p[b], g->p[gp]);
            float l2 = v3_len(d2), lim = g->rest_gp[b] * 0.6f;
            float wa = gp == held ? 0.0f : 1.0f, wb = b == held ? 0.0f : 1.0f;
            if (l2 > 1e-4f && l2 < lim && wa + wb > 0.0f) {
                float k = (l2 - lim) / (l2 * (wa + wb));
                g->p[b] = v3_mad(g->p[b], d2, -k * wb);
                g->p[gp] = v3_mad(g->p[gp], d2, k * wa);
            }
        }
    }
    // Constraints must not drag points into walls
    float moved = 0.0f;
    for (int b = 0; b < g->nb; b++) {
        // A point gone to NaN/Inf (a degenerate pose) takes its parent's place rather than spreading
        if (!finite3(g->p[b])) {
            int par = rb[b].parent;
            vec3 to = finite3(safe[b]) ? safe[b] : (par >= 0 && par < b) ? g->p[par] : g->p[0];
            if (!finite3(to)) {
                g->active = 0; // nothing sane left to rebuild it from
                return;
            }
            g->p[b] = g->prev[b] = to;
        }
        if (cm_point_contents(m, g->p[b]) & MASK_PROPSOLID) g->p[b] = safe[b];
        float mv = v3_len(v3_sub(g->p[b], g->prev[b]));
        if (mv > moved) moved = mv;
    }
    g->still = moved < 0.06f ? g->still + dt : 0.0f;
    if (g->still > 1.0f) g->asleep = 1;
    if (impact > 160.0f && g->hit_cooldown <= 0.0f) {
        snd_play_at(g_snd_rag, g->p[0], clampf(impact / 700.0f, 0.15f, 1.0f));
        g->hit_cooldown = 0.3f;
    }
}

void npc_ragdolls_update(Map *m, float dt, PhysWorld *phys) {
    if (!g_ragbones) return;
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
    if (g_rag_remote) {
        float k = clampf(dt * 15.0f, 0.0f, 1.0f);
        for (int r = 0; r < MAX_RAGDOLLS; r++) {
            Ragdoll *g = &g_rags[r];
            if (!g->active) continue;
            g->age += dt;
            for (int b = 0; b < g->nb; b++) {
                vec3 d = v3_sub(g_rag_target[r][b], g->p[b]);
                g->p[b] = v3_len(d) > 200.0f ? g_rag_target[r][b] : v3_mad(g->p[b], d, k);
                g->prev[b] = g->p[b];
            }
        }
        return;
    }
    CmBox boxes[RAG_MAX_BOXES];
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        Ragdoll *g = &g_rags[r];
        if (!g->active || g->asleep || g->frozen) {
            if (g->active) g->age += dt;
            continue;
        }
        // Props near the ragdoll are solid to it
        int nb = 0;
        for (int i = 0; i < MAX_BODIES && nb < RAG_MAX_BOXES; i++) {
            const Body *bd = &phys->bodies[i];
            if (!bd->active || bd->held) continue;
            vec3 d = v3_sub(bd->pos, g->p[0]);
            if (fabsf(d.x) > 300.0f || fabsf(d.y) > 300.0f || fabsf(d.z) > 300.0f) continue;
            phys_body_cmbox(bd, &boxes[nb], i);
            nb++;
        }
        rag_step(g, m, dt, phys, boxes, nb);
    }
}

static quat arc_between(vec3 a, vec3 b) {
    float d = v3_dot(a, b);
    if (d < -0.9999f) {
        vec3 ax = fabsf(a.x) < 0.9f ? v3_cross(a, v3(1, 0, 0)) : v3_cross(a, v3(0, 1, 0));
        return q_axis_angle(v3_norm(ax), 3.14159265f);
    }
    vec3 c = v3_cross(a, b);
    return q_norm((quat){c.x, c.y, c.z, 1.0f + d});
}

static void rag_palette(const Ragdoll *g, float (*out)[12]) {
    const RagBone *rb = g_ragbones + g_models[g->type].first_rag;
    // Each bone's rotation since death (row-major 3x3)
    static float rot[RAG_MAX_BONES][9];
    static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int b = 0; b < g->nb; b++) {
        int par = rb[b].parent;
        const float *base = (par >= 0 && par < b) ? rot[par] : ident;
        float *Q = rot[b];
        if (g->aim[b] >= 0 && g->aim2[b] >= 0) {
            // Two children pin down the whole rotation: frame now times frame at death, transposed
            vec3 u = v3_norm(v3_sub(g->p[g->aim[b]], g->p[b]));
            vec3 s = v3_sub(g->p[g->aim2[b]], g->p[b]);
            s = v3_mad(s, u, -v3_dot(s, u));
            float sl = v3_len(s);
            if (sl < 1e-3f) {
                memcpy(Q, base, sizeof(rot[b]));
                continue;
            }
            s = v3_scale(s, 1.0f / sl);
            vec3 n = v3_cross(u, s), u0 = g->dir0[b], s0 = g->side0[b], n0 = v3_cross(u0, s0);
            float F[9] = {u.x, s.x, n.x, u.y, s.y, n.y, u.z, s.z, n.z};
            float F0[9] = {u0.x, s0.x, n0.x, u0.y, s0.y, n0.y, u0.z, s0.z, n0.z};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    Q[i * 3 + j] = F[i * 3] * F0[j * 3] + F[i * 3 + 1] * F0[j * 3 + 1] + F[i * 3 + 2] * F0[j * 3 + 2];
                }
            }
        } else if (g->aim[b] >= 0) {
            // One child: swing from where the parent's rotation would put it, keeping the parent's twist
            vec3 d0 = g->dir0[b];
            vec3 u0 = v3(base[0] * d0.x + base[1] * d0.y + base[2] * d0.z, base[3] * d0.x + base[4] * d0.y + base[5] * d0.z,
                         base[6] * d0.x + base[7] * d0.y + base[8] * d0.z);
            quat q = arc_between(u0, v3_norm(v3_sub(g->p[g->aim[b]], g->p[b])));
            float x = q.x, y = q.y, z = q.z, w = q.w;
            float A[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
                          2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                          2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    Q[i * 3 + j] = A[i * 3] * base[j] + A[i * 3 + 1] * base[3 + j] + A[i * 3 + 2] * base[6 + j];
                }
            }
        } else {
            memcpy(Q, base, sizeof(rot[b]));
        }
    }
    for (int b = 0; b < g->nb; b++) {
        const float *Q = rot[b];
        const float *R0 = g->rot0[b], *pv = rb[b].pivot;
        float *o = out[b];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                o[i * 4 + j] = (Q[i * 3] * R0[j] + Q[i * 3 + 1] * R0[3 + j] + Q[i * 3 + 2] * R0[6 + j]) * g->scale[b];
            }
        }
        vec3 at = g->stump[b] >= 0 ? g->p[g->stump[b]] : g->p[b];
        o[3] = at.x - (o[0] * pv[0] + o[1] * pv[1] + o[2] * pv[2]);
        o[7] = at.y - (o[4] * pv[0] + o[5] * pv[1] + o[6] * pv[2]);
        o[11] = at.z - (o[8] * pv[0] + o[9] * pv[1] + o[10] * pv[2]);
    }
}

// --- Drawing --------------------------------------------------------------------------------------

// Area ambient plus the sun if nothing blocks it, refreshed every few frames
static void apply_light(Map *m, vec3 c, unsigned int *ambient, int *sunlit, int *age) {
    const float *sd = m->info->sun_dir;
    if ((*age)++ > 15) {
        *age = 0;
        int leaf = map_point_leaf(m, &c);
        *ambient = (leaf >= 0 && leaf < m->num_leafs) ? m->leafs[leaf].ambient_abgr : m->info->ambient_abgr;
        Trace tr;
        cm_trace_line(m, c, v3_mad(c, v3(sd[0], sd[1], sd[2]), 3000.0f), MASK_PROPSOLID, NULL, 0, &tr);
        *sunlit = tr.fraction >= 1.0f;
    }
    sceGuAmbient(scale_abgr(*ambient, *sunlit ? 1.1f : 1.4f));
    sceGuLightColor(0, GU_DIFFUSE, *sunlit ? scale_abgr(m->info->sun_abgr, 0.75f) : 0xFF000000);
}

// CPU skinning: each vertex follows its bone's matrix, then the GE draws it with the model matrix
// nscale: per bone, what undoes its inflation on the normals (NULL: none)
// Skinned vertices go in the frame's display list (1MB on the PSP, shared with the map and props):
// past this many in one frame, further characters are skipped rather than overflow it
#define SKIN_VERT_BUDGET 14000
static int g_skin_verts;

static void draw_skinned(const NpcModel *md, float (*palette)[12], int num_matrices, const float *nscale) {
    for (uint32_t k = 0; k < md->num_meshes; k++) {
        const NpcMesh *mesh = &g_meshes[md->first_mesh + k];
        if (g_skin_verts + (int)mesh->num_vertices > SKIN_VERT_BUDGET) return;
        g_skin_verts += (int)mesh->num_vertices;
        DrawVert *out = sceGuGetMemory(mesh->num_vertices * sizeof(DrawVert));
        const NpcVertex *src = g_verts + mesh->first_vertex;
        for (uint32_t v = 0; v < mesh->num_vertices; v++) {
            const NpcVertex *sv = &src[v];
            int bone = (int)sv->bone < num_matrices ? (int)sv->bone : 0;
            const float *p = palette[bone];
            float ns = nscale ? nscale[bone] : 1.0f;
            DrawVert *o = &out[v];
            o->u = sv->u;
            o->v = sv->v;
            o->x = p[0] * sv->x + p[1] * sv->y + p[2] * sv->z + p[3];
            o->y = p[4] * sv->x + p[5] * sv->y + p[6] * sv->z + p[7];
            o->z = p[8] * sv->x + p[9] * sv->y + p[10] * sv->z + p[11];
            o->nx = (p[0] * sv->nx + p[1] * sv->ny + p[2] * sv->nz) * ns;
            o->ny = (p[4] * sv->nx + p[5] * sv->ny + p[6] * sv->nz) * ns;
            o->nz = (p[8] * sv->nx + p[9] * sv->ny + p[10] * sv->nz) * ns;
        }
        pak_bind_texture(g_file, &g_textures[mesh->tex]);
        if (mesh->flags & (PMAP_BATCH_ALPHATEST | PMAP_BATCH_TRANSLUCENT)) {
            sceGuEnable(GU_ALPHA_TEST);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        } else {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        }
        sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                        mesh->num_indices, g_indices + mesh->first_index, out);
    }
}

static int visible(const Camera *cam, vec3 p, float fwd_x, float fwd_y) {
    vec3 d = v3_sub(p, cam->pos);
    float dist = v3_len(d);
    if (dist > DRAW_DIST || dist > cam->zfar) return 0; // zfar: Options > Draw distance
    return dist < 120.0f || (d.x * fwd_x + d.y * fwd_y) >= -60.0f; // not behind the camera
}

void npcs_draw(Map *m, const Camera *cam) {
    if (!g_file) return;
    g_skin_verts = 0;
    float fwd_x = cosf(cam->yaw * DEG2RAD), fwd_y = sinf(cam->yaw * DEG2RAD);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_LIGHTING);
    sceGuEnable(GU_LIGHT0);
#ifndef PS2
    gfx_flash_lit_begin();
#endif
    sceGuEnable(GU_CULL_FACE);
    sceGuFrontFace(GU_CCW);
    sceGuTexFilter(GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xFF);
    const float *sd = m->info->sun_dir;
    ScePspFVector3 sun = {sd[0], sd[1], sd[2]};
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &sun);
    sceGuColor(0xFFFFFFFF);

    static float palette[MAX_PALETTE][12];
    for (int i = 0; i < MAX_NPCS; i++) g_npcs[i].weapon_drawn = 0; // only what's drawn below gets a weapon
    for (int i = 0; i < MAX_NPCS + MAX_PUPPETS + 1; i++) {
        Npc *n = i < MAX_NPCS ? &g_npcs[i] : i < MAX_NPCS + MAX_PUPPETS ? &g_puppets[i - MAX_NPCS] : &g_self;
        if (!n->active || n->ragdolled || !visible(cam, n->mover.origin, fwd_x, fwd_y)) continue;
        apply_light(m, v3_add(n->mover.origin, v3(0, 0, 40)), &n->ambient, &n->sunlit, &n->light_age);

        // World transform: position, facing, and a fall-over tilt when dead (packs without ragdolls)
        float yr = n->yaw * DEG2RAD;
        float tilt = n->state == ST_DEAD ? clampf(n->state_time * 3.0f, 0.0f, 1.0f) * 1.5f : 0.0f;
        quat q = q_mul(q_axis_angle(v3(0, 0, 1), yr), q_axis_angle(v3(0, 1, 0), -tilt));
        vec3 ax = q_rotate(q, v3(1, 0, 0)), ay = q_rotate(q, v3(0, 1, 0)), az = q_rotate(q, v3(0, 0, 1));
        ScePspFMatrix4 w;
        w.x = (ScePspFVector4){ax.x, ax.y, ax.z, 0};
        w.y = (ScePspFVector4){ay.x, ay.y, ay.z, 0};
        w.z = (ScePspFVector4){az.x, az.y, az.z, 0};
        w.w = (ScePspFVector4){n->mover.origin.x, n->mover.origin.y, n->mover.origin.z, 1};
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadMatrix(&w);
        blended_palette(n, palette);
        if (n->tint) sceGuColor(n->tint);
        draw_skinned(&g_models[n->type], palette, MAX_PALETTE, NULL);
        if (n->tint) sceGuColor(0xFFFFFFFF);

        n->weapon_drawn = 0;
        const NpcWeapon *wp = i < MAX_NPCS ? weapon_of(n) : NULL;
        if (wp && wp->bone >= 0 && wp->bone < MAX_PALETTE) {
            // Hand bone's pose (bind -> posed, 3x4) after the weapon's bind placement, then the NPC's world matrix
            const float *p = palette[wp->bone], *a = wp->m;
            float c[12];
            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 4; k++) {
                    c[r * 4 + k] = p[r * 4 + 0] * a[0 * 4 + k] + p[r * 4 + 1] * a[1 * 4 + k] + p[r * 4 + 2] * a[2 * 4 + k] +
                                   (k == 3 ? p[r * 4 + 3] : 0.0f);
                }
            }
            // As a row-vector matrix (the GU's convention), then into the world
            float lm[16] = {c[0], c[4], c[8], 0, c[1], c[5], c[9], 0, c[2], c[6], c[10], 0, c[3], c[7], c[11], 1};
            const float *wm = (const float *)&w;
            float *o = (float *)&n->weapon_world;
            for (int r = 0; r < 4; r++) {
                for (int k = 0; k < 4; k++) {
                    o[r * 4 + k] = lm[r * 4 + 0] * wm[0 * 4 + k] + lm[r * 4 + 1] * wm[1 * 4 + k] + lm[r * 4 + 2] * wm[2 * 4 + k] +
                                   lm[r * 4 + 3] * wm[3 * 4 + k];
                }
            }
            n->weapon_drawn = 1;
        }
    }

    // Ragdolls: their matrices are already in world space
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    for (int r = 0; r < MAX_RAGDOLLS; r++) {
        Ragdoll *g = &g_rags[r];
        if (!g->active || !visible(cam, g->p[0], fwd_x, fwd_y)) continue;
        apply_light(m, v3_add(g->p[0], v3(0, 0, 8)), &g->ambient, &g->sunlit, &g->light_age);
        rag_palette(g, palette);
        static float nscale[RAG_MAX_BONES];
        for (int b = 0; b < g->nb; b++) nscale[b] = g->scale[b] > 0.01f ? 1.0f / g->scale[b] : 1.0f;
        draw_skinned(&g_models[g->type], palette, g->nb, nscale);
    }

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_LIGHT0);
#ifndef PS2
    gfx_flash_lit_end();
#endif
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_TEXTURE_2D);
}

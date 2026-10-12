#pragma once

// On-disk layout of .pmap maps, written by tools/bspconv/pmap.py and convert.py. Keep the two in sync.
// The whole file is loaded into one buffer. Sections are 16-byte aligned, offsets are from file start.

#include <stdint.h>

#define PMAP_MAGIC "PMAP"
#define PMAP_VERSION 5
#define PMAP_MAX_TEX_LEVELS 5

#define PMAP_SECTION_ID(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t num_sections;
    uint32_t reserved;
} PmapHeader;

typedef struct {
    uint32_t id;
    uint32_t offset;
    uint32_t size;
    uint32_t count;
} PmapSection;

// "INFO": one record
typedef struct {
    uint32_t tier;          // 0 = low (32MB), 1 = high (64MB)
    uint32_t pvs_row_words; // uint32 words per cluster row in "PVS "
    uint32_t sky_abgr;
    uint32_t fog_abgr;
    float fog_start, fog_end;
    float mins[3], maxs[3];
    uint32_t sky_top_abgr, sky_bottom_abgr; // env_skypaint gradient
    float sun_dir[3];                       // unit vector pointing toward the sun
    uint32_t sun_abgr;
    uint32_t ambient_abgr;
    uint32_t sprp_row_words; // uint32 words per cluster row in "SPVS"
    uint32_t reserved[4];
} PmapInfo;

// "SPWN": player spawns (feet position)
typedef struct {
    float x, y, z, yaw;
} PmapSpawn;

// "TEXS": T8 textures with a 256-entry 8888 CLUT, swizzled, mip levels largest first
#define PMAP_TEX_ALPHATEST 1
#define PMAP_TEX_TRANSLUCENT 2
typedef struct {
    uint16_t width, height;
    uint8_t levels;
    uint8_t flags;
    uint16_t reserved;
    uint32_t clut_ofs;
    uint32_t level_ofs[PMAP_MAX_TEX_LEVELS];
} PmapTexture;

// "VERT": GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT, 16 bytes.
// The GE reads 16-bit values as 1.15 fixed point. Each batch says how to expand them:
// world = origin + pos * step, uv = uv_off + uv * uv_scale / 32768.
typedef struct {
    uint16_t u, v;
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
} PmapVertex;

// "INDX": uint16 indices, relative to the batch's first_vertex

// "BTCH": one draw call each, sorted by (translucent, texture)
#define PMAP_BATCH_ALPHATEST 1
#define PMAP_BATCH_TRANSLUCENT 2
#define PMAP_BATCH_NOCULL 4
typedef struct {
    uint32_t first_vertex;
    uint32_t first_index;
    uint32_t num_indices;
    uint16_t tex;
    uint16_t flags;
    float mins[3];
    float maxs[3];
    float origin[3];    // position decode
    float uv_off[2];    // texture coordinate decode
    float uv_scale[2];
    uint32_t step_fp;   // position step in 16.16 fixed point (1/16 unit normally)
    uint32_t reserved[2];
} PmapBatch;

// "PLAN"
typedef struct {
    float nx, ny, nz, dist;
} PmapPlane;

// "NODE": child >= 0 is a node index, child < 0 is leaf (-1 - child)
typedef struct {
    int32_t plane;
    int32_t children[2];
} PmapNode;

// "LEAF"
typedef struct {
    int32_t cluster; // -1 = solid / outside the map
    uint32_t contents;
    uint32_t first_brush; // into "LBRS"
    uint32_t num_brushes;
    uint32_t ambient_abgr; // average ambient light in this leaf, for lighting props
    uint32_t reserved;
} PmapLeaf;

// "PVS ": count = number of clusters; each row is a bitset over batches, pvs_row_words long.

// "BRSH": convex collision brushes; a point is inside when it's behind every side plane
#define PMAP_CONTENTS_SOLID 0x1
#define PMAP_CONTENTS_WINDOW 0x2
#define PMAP_CONTENTS_GRATE 0x8
#define PMAP_CONTENTS_WATER 0x20
#define PMAP_CONTENTS_MOVEABLE 0x4000
#define PMAP_CONTENTS_PLAYERCLIP 0x10000
typedef struct {
    uint32_t first_side; // into "BSID"
    uint32_t num_sides;
    uint32_t contents;
} PmapBrush;

// "ENTS": brush entities that move or trigger (static ones are merged into the world)
// PMAP_ENT_LIGHT: a named switchable light; 'distance' holds its light style (32-63)
enum { PMAP_ENT_DOOR = 1, PMAP_ENT_DOOR_ROTATING, PMAP_ENT_BUTTON, PMAP_ENT_ROTATING, PMAP_ENT_TRIGGER, PMAP_ENT_LIGHT };
#define PMAP_ENT_TRIGGER_ONCE 0x80000000u // in flags
typedef struct {
    uint32_t type;
    uint32_t flags; // Source spawnflags
    char name[32];  // targetname
    float origin[3];
    float axis[3];  // door/button: move direction; rotating: rotation axis
    float distance; // units, or degrees for rotating doors
    float speed;    // units/s or degrees/s
    float wait;     // seconds before returning; -1 = stay
    uint32_t solid;
    float mins[3], maxs[3]; // local bounds
    uint32_t first_batch, num_batches; // into "EBAT" (PmapBatch, local space)
    uint32_t first_brush, num_brushes; // into "EBRL" (brush indices, local space)
    uint32_t first_output, num_outputs; // into "EOUT"
} PmapEnt;

// "EOUT": an output fires 'input' on entity 'target' after 'delay' when 'event' happens
enum { PMAP_EV_ACTIVATE = 0, PMAP_EV_OPEN, PMAP_EV_CLOSE };
enum { PMAP_IN_OPEN = 1, PMAP_IN_CLOSE, PMAP_IN_TOGGLE, PMAP_IN_START, PMAP_IN_STOP, PMAP_IN_PRESS, PMAP_IN_REVERSE };
typedef struct {
    uint32_t event;
    int32_t target;
    uint32_t input;
    float delay;
} PmapOutput;

// "SPRP": static props. Model indices refer to the map's own prop pack (<map>.ppak).
typedef struct {
    float axis_x[3], axis_y[3], axis_z[3]; // model axes in world space
    float origin[3];
    uint32_t model;
    uint32_t solid;
    float fade_dist; // 0 = default draw distance
    uint32_t reserved;
} PmapStaticProp;

// "SPVS": count = clusters; each row is a bitset over static props, sprp_row_words long
// "PHYS": physics props to spawn at load
typedef struct {
    uint32_t model;
    float origin[3];
    float rot[4]; // quaternion x y z w
} PmapPhysProp;

// "LSTY": light style colour deltas, sorted by vertex. A vertex's colour is base + sum(value(style) * delta).
typedef struct {
    uint32_t vertex; // into "VERT"
    uint32_t base;   // ABGR with every non-zero style off
    uint16_t style;
    int16_t dr, dg, db;
} PmapLightStyle;

// "LSTP": 64 flicker patterns, one per style ('a' dark .. 'm' normal .. 'z' double), 10 steps/s.
//         An empty pattern means steady.
typedef char PmapLightPattern[64];
// "SKYT": int32 index of the first of 6 skybox textures (rt bk lf ft up dn) in "TEXS", or -1

// "BSID": uint32 plane index per brush side (into "PLAN"). PMAP_SIDE_BEVEL marks bevel planes,
// which matter for box sweeps but aren't real surfaces.
#define PMAP_SIDE_BEVEL 0x80000000u
#define PMAP_SIDE_PLANE(s) ((s) & 0x7FFFFFFFu)
// "LBRS": uint32 brush indices, referenced by leaves

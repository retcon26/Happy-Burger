#pragma once

#include "camera.h"
#include "pmap_format.h"

// Where a texture's CLUT and mip levels live at runtime: the file buffer, or VRAM for the busiest ones.
typedef struct {
    const void *clut;
    const void *level[PMAP_MAX_TEX_LEVELS];
} TexRef;

typedef struct {
    void *file;      // whole .pmap, 64-byte aligned; every pointer below points into it
    unsigned int file_size;
    const PmapInfo *info;
    const PmapSpawn *spawns;
    int num_spawns;
    const PmapTexture *textures;
    int num_textures;
    const PmapVertex *verts;
    const uint16_t *indices;
    const PmapBatch *batches;
    int num_batches;
    const PmapPlane *planes;
    const PmapNode *nodes;
    int num_nodes;
    const PmapLeaf *leafs;
    int num_leafs;
    const uint32_t *pvs;
    int num_clusters;
    const PmapBrush *brushes;
    int num_brushes;
    const uint32_t *brush_sides;
    const uint32_t *leaf_brushes;
    uint32_t *brush_check; // per-brush stamp so a trace tests each brush once
    uint32_t check_count;
    // Entities (see ents.h for their runtime state)
    const PmapEnt *ents;
    int num_ents;
    const PmapBatch *ent_batches;
    const uint32_t *ent_brushes;
    const PmapOutput *outputs;
    struct EntState *ent_state;
    // Static and physics props placed in the map
    const PmapStaticProp *sprops;
    int num_sprops;
    const uint32_t *spvs;
    const PmapPhysProp *phys_props;
    int num_phys_props;
    TexRef *texref;
    int vram_textures;       // how many textures were copied to VRAM
    unsigned int vram_bytes;
    // Light styles
    const PmapLightStyle *lstyles;
    int num_lstyles;
    const PmapLightPattern *lpatterns;
    unsigned char style_switch[64];  // switchable lights: 1 on, 0 off (set by ents.c)
    unsigned char style_applied[64]; // brightness last written into the vertex colours (0..255, 'm' = 100)
    int sky_first_tex;               // -1 = no skybox textures
} Map;

typedef struct {
    int cluster;
    int batches_visible; // passed the PVS test
    int batches_drawn;   // also passed frustum and distance culling
    int tris_drawn;
    int clipped_tris; // made on the CPU by near-camera clipping
    int tex_binds;
} MapDrawStats;

// Loads data/maps/<name>.pmap, or <name>.low.pmap on 32MB systems (falling back to whichever exists).
// Returns 0 on success, or a negative code and an error message in err.
int map_load(Map *m, const char *name, char *err, int err_len);
void map_free(Map *m);

int map_point_leaf(const Map *m, const ScePspFVector3 *p);
void map_draw(const Map *m, const Camera *cam, MapDrawStats *stats);

// Packed-vertex batches: load the decode transform (optionally under a parent transform, for
// moving entities) and the texture-coordinate scale. map_batch_end restores identity.
void map_batch_matrix(const PmapBatch *b, const ScePspFMatrix4 *parent, ScePspFMatrix4 *out);
void map_batch_begin(const PmapBatch *b, const ScePspFMatrix4 *parent);
void map_batch_end(void);
void map_bind_texture(const Map *m, int tex);

// Steps flicker patterns and applies switched lights to the vertex colours (cheap when nothing changed).
void map_update_lightstyles(Map *m, float time_s);

// Fills names (without extension, skipping .low variants) of the maps in data/maps. Returns the count.
int map_list(char names[][32], int max);
// Farthest distance map_draw draws (Options > Draw distance); 1e9 = the map's own fog distance
void map_set_view_limit(float dist);
// The map whose files a map name loads (itself, unless it's an alias such as gm_construct_13)
const char *map_file_name(const char *name);

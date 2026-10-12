#pragma once

#include <psptypes.h>

#include "ppak_format.h"

// The spawnable prop models (data/props.ppak, or props.low.ppak on 32MB systems).
typedef struct {
    void *file;
    const PpakModel *models;
    int num_models;
    const PpakMesh *meshes;
    const PpakVertex *verts;
    const uint16_t *indices;
    const PmapTexture *textures;
    const char (*categories)[24]; // spawn menu categories (model flags bits 16-23 index this)
    int num_categories;
    // Convex collision hulls (NULL in packs built before hulls existed)
    const PpakHull *hulls;
    const PpakHullPiece *hull_pieces;
    const PmapPlane *hull_planes;
    const float (*hull_points)[3];
} PropLib;

// Loads <base>.ppak, or <base>.low.ppak on 32MB systems (falling back to whichever exists).
int proplib_load(PropLib *lib, const char *base, char *err, int err_len);
// The same from a pack already in memory (64-byte aligned, malloc'd: the lib owns it from now on,
// even on failure). path is only for error messages.
int proplib_load_buffer(PropLib *lib, void *file, const char *path, char *err, int err_len);
void proplib_free(PropLib *lib);

typedef struct {
    ScePspFVector3 sun_dir; // toward the sun
    unsigned int sun_abgr;  // 0 when the prop is in shadow
    unsigned int ambient_abgr;
    unsigned int tint_abgr;
} PropLight;

// Draws one model with the given world matrix and GE lighting.
void proplib_draw(const PropLib *lib, int model, const ScePspFMatrix4 *world, const PropLight *light);
// Call around a run of proplib_draw calls; sets and restores lighting/texture state.
void proplib_begin(void);
void proplib_end(void);

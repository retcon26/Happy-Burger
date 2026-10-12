#pragma once

// Prop pack layout, written by tools/bspconv/props.py. Same sectioned container as .pmap (pmap_format.h).

#include <stdint.h>

#include "pmap_format.h"

#define PPAK_MAGIC "PPAK"
#define PPAK_VERSION 1

// "MDLS"
#define PPAK_SHAPE_MASK 0xFF
#define PPAK_SHAPE_BOX 0
#define PPAK_SHAPE_SPHERE 1
#define PPAK_SHAPE_CYLINDER 2 // along model Z
#define PPAK_SHAPE_HULL 3     // convex pieces from the model's .phy ("HULL" section)
#define PPAK_HIDDEN 0x100     // not in the spawn menu (weapon models)
// Breakable: shatters into the hidden models named _gib<group> (flag bits 9-11 = group, 0 = solid)
#define PPAK_BREAK_SHIFT 9
#define PPAK_BREAK_MASK (7u << PPAK_BREAK_SHIFT)

// Surface classes, for impact sounds and buoyancy
enum { SURF_GENERIC, SURF_WOOD, SURF_METAL, SURF_PLASTIC, SURF_CONCRETE, SURF_GLASS, SURF_CARDBOARD, SURF_FLESH, SURF_RUBBER };

typedef struct {
    char name[32];
    float mins[3], maxs[3]; // model-space bounds; the physics shape fits inside
    float mass;             // kg, from the model's .phy
    uint32_t first_mesh, num_meshes;
    uint32_t tris;
    uint32_t flags;   // PPAK_SHAPE_* | PPAK_HIDDEN
    uint32_t surface; // SURF_*
} PpakModel;

// "MESH": one texture each
typedef struct {
    uint32_t first_vertex, num_vertices;
    uint32_t first_index, num_indices;
    uint16_t tex;
    uint16_t flags; // PMAP_BATCH_*
} PpakMesh;

// "VERT": GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF
typedef struct {
    float u, v;
    float nx, ny, nz;
    float x, y, z;
} PpakVertex;

// "INDX": uint16, relative to the mesh's first_vertex

// "HULL": per model, its convex collision pieces and contact sample points (0 pieces = none)
typedef struct {
    uint32_t first_piece, num_pieces; // into "HPCE"
    uint32_t first_point, num_points; // into "HPNT"
} PpakHull;
// "HPCE": one convex piece: its outward planes in "HPLN" (PmapPlane, model space)
typedef struct {
    uint32_t first_plane, num_planes;
} PpakHullPiece;
// "HPNT": float[3] sample points, model space
// "TEXS" / "TXDT": as in .pmap (PmapTexture)

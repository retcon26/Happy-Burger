#pragma once

#include "map.h"
#include "ppak_format.h"
#include "vec.h"

#define MASK_PLAYERSOLID (PMAP_CONTENTS_SOLID | PMAP_CONTENTS_WINDOW | PMAP_CONTENTS_GRATE | PMAP_CONTENTS_MOVEABLE | PMAP_CONTENTS_PLAYERCLIP)
#define MASK_PROPSOLID (PMAP_CONTENTS_SOLID | PMAP_CONTENTS_WINDOW | PMAP_CONTENTS_GRATE | PMAP_CONTENTS_MOVEABLE)

#define TRACE_HIT_NONE -1
#define TRACE_HIT_WORLD -2

typedef struct {
    float fraction; // how much of the move completed, 0..1
    vec3 endpos;
    vec3 normal;    // surface normal at the hit
    int startsolid; // the start position was inside something
    int allsolid;   // the whole move was inside something
    int hit;        // TRACE_HIT_NONE, TRACE_HIT_WORLD, or the id of a CmBox
} Trace;

// Extra things to collide with besides the world (props, NPCs, players). mins/maxs is always the
// world-space bounding box. With "oriented" set, the real shape is a box of half-extents "half" (or the
// convex hull pieces, in model space) at pos/rot, and the bounding box is only used to skip it quickly.
typedef struct {
    vec3 mins, maxs;
    int id;
    int oriented;
    vec3 pos, half, offset; // the shape's frame; model space = local + offset (for hull planes)
    quat rot;
    const PpakHullPiece *pieces;
    int num_pieces;
    const PmapPlane *planes;
} CmBox;

static inline void cm_box_aabb(CmBox *b, vec3 mins, vec3 maxs, int id) {
    b->mins = mins;
    b->maxs = maxs;
    b->id = id;
    b->oriented = 0;
    b->num_pieces = 0;
}

// Sweeps the box [mins, maxs] (relative to the moving point) from start to end.
void cm_trace(Map *m, vec3 start, vec3 end, vec3 mins, vec3 maxs, unsigned int mask, const CmBox *boxes,
              int num_boxes, Trace *tr);
// Line trace (zero-size box).
static inline void cm_trace_line(Map *m, vec3 start, vec3 end, unsigned int mask, const CmBox *boxes, int num_boxes,
                                 Trace *tr) {
    vec3 z = {0.0f, 0.0f, 0.0f};
    cm_trace(m, start, end, z, z, mask, boxes, num_boxes, tr);
}
unsigned int cm_point_contents(Map *m, vec3 p);

// Deepest-penetration contact of a point inside solid world brushes, for physics.
// Returns 1 with the push-out normal and depth if the point is inside solid.
int cm_point_penetration(Map *m, vec3 p, unsigned int mask, vec3 *normal, float *depth);

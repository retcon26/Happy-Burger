#pragma once

#include <stdint.h>

#include "collision.h"

// NPC navigation: the map's own node graph (its info_nodes, linked by tools/bspconv/nav.py into
// data/maps/<map>.pnav). Maps without one have no graph, and NPCs head straight for where they're going.

#define NAV_ROUTE_MAX 32

// base: the map's file path without extension ("data/maps/gm_construct"). Returns the node count.
int nav_load(const char *base);
void nav_free(void);
int nav_available(void);
int nav_node_count(void);
vec3 nav_node_pos(int i);
// The nearest node within max_dist that p can see (a clear line), or -1
int nav_nearest(Map *m, vec3 p, float max_dist);
// The shortest route from node a to node b (A*): the nodes after a, ending with b, up to max of
// them. Returns how many, 0 if a == b, or -1 if b can't be reached.
int nav_route(int a, int b, int16_t *out, int max);
// A link something blocks (a closed door, props): routes go round it for a while
void nav_block(int a, int b, float seconds);
void nav_update(float dt);
// Somewhere to wander: a node a few links from i. Somewhere to run: the node near i farthest from p.
int nav_wander_node(int i);
int nav_flee_node(int i, vec3 from);

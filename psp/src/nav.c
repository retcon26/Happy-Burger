#include "nav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pak.h"

// "NAVN" (one per node) and "NAVL" (u16 node indices), see tools/bspconv/nav.py
typedef struct {
    float x, y, z;
    uint16_t first_link;
    uint8_t num_links, flags;
} NavNode;

#define NAV_ID(a, b, c, d) PMAP_SECTION_ID(a, b, c, d)
#define MAX_BLOCKED 16
#define HEAP_MAX 4096

static void *g_file;
static const NavNode *g_nodes;
static const uint16_t *g_links;
static int g_num;
// A* working space, num nodes each
static float *g_cost;
static int16_t *g_from;
static uint16_t *g_stamp; // which search reached a node (no clearing between searches)
static uint16_t g_search;
static struct {
    float f;
    int16_t node;
} g_heap[HEAP_MAX];
static struct {
    int16_t a, b;
    float time;
} g_blocked[MAX_BLOCKED];

void nav_free(void) {
    free(g_file);
    free(g_cost);
    free(g_from);
    free(g_stamp);
    g_file = NULL;
    g_cost = NULL;
    g_from = NULL;
    g_stamp = NULL;
    g_nodes = NULL;
    g_links = NULL;
    g_num = 0;
    memset(g_blocked, 0, sizeof(g_blocked));
}

int nav_load(const char *base) {
    nav_free();
    char path[96];
    snprintf(path, sizeof(path), "%s.pnav", base);
    if (!pak_file_exists(path)) return 0;
    unsigned int size;
    g_file = pak_read_file(path, &size);
    if (!g_file) return 0;
    const PmapHeader *h = g_file;
    const PmapSection *ns = pak_find(g_file, NAV_ID('N', 'A', 'V', 'N'));
    const PmapSection *ls = pak_find(g_file, NAV_ID('N', 'A', 'V', 'L'));
    if (memcmp(h->magic, "PNAV", 4) != 0 || h->version != 1 || !ns || !ls || ns->count == 0 || ns->count > 30000) {
        nav_free();
        return 0;
    }
    g_nodes = (const NavNode *)((const char *)g_file + ns->offset);
    g_links = (const uint16_t *)((const char *)g_file + ls->offset);
    g_num = (int)ns->count;
    for (int i = 0; i < g_num; i++) {
        if (g_nodes[i].first_link + g_nodes[i].num_links > ls->count) { // a damaged file
            nav_free();
            return 0;
        }
    }
    g_cost = malloc(sizeof(float) * (size_t)g_num);
    g_from = malloc(sizeof(int16_t) * (size_t)g_num);
    g_stamp = calloc((size_t)g_num, sizeof(uint16_t));
    if (!g_cost || !g_from || !g_stamp) {
        nav_free();
        return 0;
    }
    g_search = 0;
    return g_num;
}

int nav_available(void) {
    return g_num > 0;
}

int nav_node_count(void) {
    return g_num;
}

vec3 nav_node_pos(int i) {
    return v3(g_nodes[i].x, g_nodes[i].y, g_nodes[i].z);
}

int nav_nearest(Map *m, vec3 p, float max_dist) {
    // The few closest (by distance alone), then the first of those p can see
    enum { K = 6 };
    int best[K];
    float bd[K];
    int nb = 0;
    for (int i = 0; i < g_num; i++) {
        float dx = g_nodes[i].x - p.x, dy = g_nodes[i].y - p.y, dz = g_nodes[i].z - p.z;
        if (fabsf(dz) > 120.0f) continue;
        float d = dx * dx + dy * dy + dz * dz * 4.0f; // another floor counts as far
        if (d > max_dist * max_dist) continue;
        if (nb == K && d >= bd[K - 1]) continue;
        int k = nb < K ? nb++ : K - 1;
        while (k > 0 && bd[k - 1] > d) {
            bd[k] = bd[k - 1];
            best[k] = best[k - 1];
            k--;
        }
        bd[k] = d;
        best[k] = i;
    }
    for (int k = 0; k < nb; k++) {
        Trace tr;
        vec3 n = nav_node_pos(best[k]);
        cm_trace_line(m, v3_add(p, v3(0, 0, 24)), v3_add(n, v3(0, 0, 24)), MASK_PLAYERSOLID, NULL, 0, &tr);
        if (tr.fraction >= 1.0f && !tr.startsolid) return best[k];
    }
    return -1;
}

static int blocked(int a, int b) {
    for (int i = 0; i < MAX_BLOCKED; i++) {
        if (g_blocked[i].time > 0.0f && ((g_blocked[i].a == a && g_blocked[i].b == b) || (g_blocked[i].a == b && g_blocked[i].b == a)))
            return 1;
    }
    return 0;
}

void nav_block(int a, int b, float seconds) {
    if (a < 0 || b < 0 || a >= g_num || b >= g_num) return;
    int slot = 0;
    for (int i = 0; i < MAX_BLOCKED; i++) {
        if ((g_blocked[i].a == a && g_blocked[i].b == b) || g_blocked[i].time <= 0.0f) {
            slot = i;
            break;
        }
        if (g_blocked[i].time < g_blocked[slot].time) slot = i;
    }
    g_blocked[slot].a = (int16_t)a;
    g_blocked[slot].b = (int16_t)b;
    g_blocked[slot].time = seconds;
}

void nav_update(float dt) {
    for (int i = 0; i < MAX_BLOCKED; i++) g_blocked[i].time -= dt;
}

static float dist(int a, int b) {
    float dx = g_nodes[a].x - g_nodes[b].x, dy = g_nodes[a].y - g_nodes[b].y, dz = g_nodes[a].z - g_nodes[b].z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int heap_n;

static void heap_push(float f, int node) {
    if (heap_n >= HEAP_MAX) return;
    int i = heap_n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_heap[p].f <= f) break;
        g_heap[i] = g_heap[p];
        i = p;
    }
    g_heap[i].f = f;
    g_heap[i].node = (int16_t)node;
}

static int heap_pop(void) {
    int top = g_heap[0].node;
    float f = g_heap[--heap_n].f;
    int16_t node = g_heap[heap_n].node;
    int i = 0;
    for (;;) {
        int c = i * 2 + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && g_heap[c + 1].f < g_heap[c].f) c++;
        if (g_heap[c].f >= f) break;
        g_heap[i] = g_heap[c];
        i = c;
    }
    if (heap_n > 0) {
        g_heap[i].f = f;
        g_heap[i].node = node;
    }
    return top;
}

int nav_route(int a, int b, int16_t *out, int max) {
    if (a < 0 || b < 0 || a >= g_num || b >= g_num) return -1;
    if (a == b) return 0;
    if (++g_search == 0) { // the stamps wrapped: start them again
        memset(g_stamp, 0, sizeof(uint16_t) * (size_t)g_num);
        g_search = 1;
    }
    heap_n = 0;
    g_stamp[a] = g_search;
    g_cost[a] = 0.0f;
    g_from[a] = -1;
    heap_push(dist(a, b), a);
    int found = 0;
    while (heap_n > 0) {
        int n = heap_pop();
        if (n == b) {
            found = 1;
            break;
        }
        const NavNode *nd = &g_nodes[n];
        for (int k = 0; k < nd->num_links; k++) {
            int o = g_links[nd->first_link + k];
            if (o >= g_num || blocked(n, o)) continue;
            float c = g_cost[n] + dist(n, o);
            if (g_stamp[o] == g_search && c >= g_cost[o]) continue;
            g_stamp[o] = g_search;
            g_cost[o] = c;
            g_from[o] = (int16_t)n;
            heap_push(c + dist(o, b), o);
        }
    }
    if (!found) return -1;
    // Walk back from b, then keep the first max nodes after a
    int len = 0;
    for (int n = b; n != a && n >= 0; n = g_from[n]) len++;
    int skip = len > max ? len - max : 0, i = len - 1 - skip;
    for (int n = b, k = len - 1; n != a && n >= 0; n = g_from[n], k--) {
        if (k <= i) out[k] = (int16_t)n;
    }
    return len - skip;
}

int nav_wander_node(int i) {
    if (i < 0 || i >= g_num) return -1;
    int hops = 1 + rand() % 3;
    for (int h = 0; h < hops; h++) {
        const NavNode *nd = &g_nodes[i];
        if (!nd->num_links) break;
        i = g_links[nd->first_link + rand() % nd->num_links];
    }
    return i;
}

int nav_flee_node(int i, vec3 from) {
    if (i < 0 || i >= g_num) return -1;
    // Two links out, the node farthest from the danger
    int best = i;
    float bd = v3_len(v3_sub(nav_node_pos(i), from));
    const NavNode *nd = &g_nodes[i];
    for (int k = 0; k < nd->num_links; k++) {
        int o = g_links[nd->first_link + k];
        const NavNode *od = &g_nodes[o];
        for (int j = -1; j < od->num_links; j++) {
            int c = j < 0 ? o : g_links[od->first_link + j];
            float d = v3_len(v3_sub(nav_node_pos(c), from));
            if (d > bd) bd = d, best = c;
        }
    }
    return best;
}

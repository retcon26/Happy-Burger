#include "mem.h"

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

// A PSP-1000 heap tops out around 20MB and the bigger models' around 48MB. The full-detail packs (props,
// NPCs, sounds) take about 17MB and a big map 8-18MB more, so full detail needs a heap of 40MB or more:
// a smaller one gets the reduced packs, and maps still try full detail first (map.c falls back).
#define HIGH_TIER_MIN_BYTES (40u * 1024u * 1024u)
#define USER_RAM_START 0x08800000u
#define USER_RAM_END 0x0C000000u // the end of a 64MB model's; a PSP-1000's stops at 0x0A000000

static size_t g_total;
static MemTier g_tier;

// The heap is sized at launch to "everything but 1MB" (see main.c), so the largest
// block we can grab right now is effectively the heap size.
static size_t largest_block(void) {
    // Emulators can hand out more than real hardware, so search well past 64MB.
    size_t lo = 0, hi = 256u * 1024u * 1024u;
    while (hi - lo > 64u * 1024u) {
        size_t mid = lo + (hi - lo) / 2;
        void *p = malloc(mid);
        // Only a block inside the PSP's user RAM counts: some emulator setups hand out impossible
        // ones (a 256MB block on a PSP-1000), which made the game load full-detail packs into 20MB
        uintptr_t a = (uintptr_t)p;
        int real = p && a >= USER_RAM_START && a + mid <= USER_RAM_END;
        if (p) free(p);
        if (real) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void mem_init(void) {
    g_total = largest_block();
    g_tier = g_total >= HIGH_TIER_MIN_BYTES ? MEM_TIER_HIGH : MEM_TIER_LOW;
}

size_t mem_largest_free(void) {
    return largest_block();
}

MemTier mem_tier(void) {
    return g_tier;
}

const char *mem_tier_name(void) {
    return g_tier == MEM_TIER_HIGH ? "64MB" : "32MB";
}

size_t mem_heap_total(void) {
    return g_total;
}

size_t mem_heap_used(void) {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

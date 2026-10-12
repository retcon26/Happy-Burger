#pragma once

#include <stddef.h>

// HIGH means a PSP-2000/3000/Go with about 52MB of user RAM. LOW means a PSP-1000 with about 24MB.
// Asset loading picks full or reduced-detail files from this.
typedef enum {
    MEM_TIER_LOW = 0,
    MEM_TIER_HIGH = 1,
} MemTier;

// Call once at startup, before any big allocations.
void mem_init(void);
MemTier mem_tier(void);
const char *mem_tier_name(void);
size_t mem_heap_total(void); // Heap size measured at startup
size_t mem_heap_used(void);
size_t mem_largest_free(void); // the biggest single block malloc could hand out right now

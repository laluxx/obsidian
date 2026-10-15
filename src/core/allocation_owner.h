#pragma once
#include <stdint.h>
#define OBS_ALLOCATION_OWNER_LIMIT 64
#define OBS_ALLOCATION_OWNER_NAME_LIMIT 64
typedef struct {
    char name[OBS_ALLOCATION_OWNER_NAME_LIMIT];
    uint64_t live_bytes;
    uint64_t peak_bytes;
    uint64_t live_allocations;
    uint64_t peak_allocations;
} ObsAllocationOwner;

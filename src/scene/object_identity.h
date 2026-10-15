/// object_identity.h — stable logical identities for compacted scene arrays
#ifndef OBSIDIAN_SCENE_OBJECT_IDENTITY_H
#define OBSIDIAN_SCENE_OBJECT_IDENTITY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Identity types
/* These are engine-side identities, NOT pointers or script-owned memory. */
typedef enum {
    OBS_ID_MESH = 1,
    OBS_ID_NODE = 2,
    OBS_ID_MATERIAL = 3,
} ObsIdentityKind;

enum { OBS_ID_PAGE_SIZE = 128 };
typedef struct ObsIdentitySlot {
    uint32_t generation;
    uint32_t next_free;  /* one-based slot index, 0 = none */
    size_t dense_index;
    ObsIdentityKind kind;
    bool live;
    bool seen;
} ObsIdentitySlot;
typedef struct ObsIdentityPage {
    ObsIdentitySlot slots[OBS_ID_PAGE_SIZE];
} ObsIdentityPage;
typedef struct ObsIdentityRegistry {
    /* Page addresses never move. Old host references remain safely comparable. */
    ObsIdentityPage **pages;
    size_t page_count;
    size_t page_capacity;
    uint32_t issued;
    uint32_t free_head;
    uint64_t synced_revision[4];
    bool synced[4];
} ObsIdentityRegistry;
typedef struct ObsIdentityView {
    void *items;
    size_t stride;
    size_t count;
    size_t identity_offset;
    uint64_t revision;
    ObsIdentityKind kind;
} ObsIdentityView;

/// Public interface
/* Synchronization is allocation-free; only new borrowed identities allocate.
 * All functions are owner-thread-only. A slot generation never wraps: its
 * address is retired permanently instead. */
bool obs_identity_sync(ObsIdentityRegistry *r, ObsIdentityView view);
bool obs_identity_borrow(ObsIdentityRegistry *r, ObsIdentityView view, size_t index,
                         const void **token, uint32_t *generation);
bool obs_identity_generation(ObsIdentityRegistry *r, ObsIdentityView view,
                             const void *token, uint32_t *generation);
bool obs_identity_resolve(ObsIdentityRegistry *r, ObsIdentityView view,
                          const void *token, uint32_t generation, size_t *index);
void obs_identity_destroy(ObsIdentityRegistry *r); /* only after all host refs die */
#endif

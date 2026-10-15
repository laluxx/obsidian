#ifndef OBSIDIAN_TEXTURE_REFS_H
#define OBSIDIAN_TEXTURE_REFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Lifetime model
/* Indices are legacy renderer pool slots. They are NEVER reused in this
 * generation of the pool: a descriptor index may still be present in an old
 * draw packet. This registry manages only textures acquired through the new
 * retained interface, not legacy texture_pool_add() references.
 *
 * LIVE -> PENDING (last release) -> RETIRED (after GPU quiescence).
 * A new acquire can rescue a PENDING entry before its retirement. No code
 * outside the renderer is permitted to call retire() on an actual GPU entry:
 * successful fence/device quiescence and cooker completion are prerequisites.
 * All methods are engine-owner-thread only. */
#define OBS_TEXTURE_REF_LIMIT 8192u
#define OBS_TEXTURE_REF_PATH_MAX 512u
#define OBS_TEXTURE_REF_TABLE_SIZE 16384u

typedef enum {
    OBS_TEXTURE_REF_UNUSED = 0,
    OBS_TEXTURE_REF_LIVE,
    OBS_TEXTURE_REF_PENDING,
    OBS_TEXTURE_REF_RETIRED
} ObsTextureRefState;

typedef struct {
    uint32_t references;
    ObsTextureRefState state;
    char path[OBS_TEXTURE_REF_PATH_MAX];
} ObsTextureRefEntry;

typedef struct {
    ObsTextureRefEntry entries[OBS_TEXTURE_REF_LIMIT];
    /* 0 = empty; UINT32_MAX = tombstone; otherwise slot + 1. */
    uint32_t lookup[OBS_TEXTURE_REF_TABLE_SIZE];
    uint32_t pending;
} ObsTextureRefs;

/// Owner-thread registry operations
void obs_texture_refs_init(ObsTextureRefs *registry);
bool obs_texture_refs_find(const ObsTextureRefs *registry, const char *path,
                           uint32_t *slot);
bool obs_texture_refs_enroll(ObsTextureRefs *registry, uint32_t slot,
                             const char *path);
bool obs_texture_refs_retain(ObsTextureRefs *registry, uint32_t slot);
bool obs_texture_refs_release(ObsTextureRefs *registry, uint32_t slot);
bool obs_texture_refs_retire(ObsTextureRefs *registry, uint32_t slot);
ObsTextureRefState obs_texture_refs_state(const ObsTextureRefs *registry,
                                          uint32_t slot);
uint32_t obs_texture_refs_count(const ObsTextureRefs *registry, uint32_t slot);
uint32_t obs_texture_refs_pending(const ObsTextureRefs *registry);

#endif

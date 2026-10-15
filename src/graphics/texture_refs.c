#include "texture_refs.h"
#include <limits.h>
#include <string.h>

/// Stable hash and validated path
static bool path_length(const char *path, size_t *length) {
    if (!path || !length) return false;
    size_t n = 0;
    while (n < OBS_TEXTURE_REF_PATH_MAX && path[n]) ++n;
    if (!n || n == OBS_TEXTURE_REF_PATH_MAX) return false;
    *length = n;
    return true;
}

static uint64_t hash_path(const char *path, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char)path[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t bucket_for(const char *path, size_t length) {
    return (size_t)hash_path(path, length) & (OBS_TEXTURE_REF_TABLE_SIZE - 1u);
}

void obs_texture_refs_init(ObsTextureRefs *registry) {
    if (registry) memset(registry, 0, sizeof(*registry));
}

ObsTextureRefState obs_texture_refs_state(const ObsTextureRefs *registry,
                                          uint32_t slot) {
    return registry && slot < OBS_TEXTURE_REF_LIMIT
        ? registry->entries[slot].state : OBS_TEXTURE_REF_UNUSED;
}

uint32_t obs_texture_refs_count(const ObsTextureRefs *registry, uint32_t slot) {
    return registry && slot < OBS_TEXTURE_REF_LIMIT
        ? registry->entries[slot].references : 0;
}

uint32_t obs_texture_refs_pending(const ObsTextureRefs *registry) {
    return registry ? registry->pending : 0;
}

bool obs_texture_refs_find(const ObsTextureRefs *registry, const char *path,
                           uint32_t *slot) {
    size_t length;
    if (!registry || !slot || !path_length(path, &length)) return false;
    size_t bucket = bucket_for(path, length);
    for (size_t i = 0; i < OBS_TEXTURE_REF_TABLE_SIZE; ++i) {
        uint32_t value = registry->lookup[bucket];
        if (!value) return false;
        if (value != UINT32_MAX) {
            uint32_t index = value - 1u;
            if (index < OBS_TEXTURE_REF_LIMIT &&
                (registry->entries[index].state == OBS_TEXTURE_REF_LIVE ||
                 registry->entries[index].state == OBS_TEXTURE_REF_PENDING) &&
                strcmp(registry->entries[index].path, path) == 0) {
                *slot = index;
                return true;
            }
        }
        bucket = (bucket + 1u) & (OBS_TEXTURE_REF_TABLE_SIZE - 1u);
    }
    return false;
}

/// Enrollment, with tombstones for retired entries
bool obs_texture_refs_enroll(ObsTextureRefs *registry, uint32_t slot,
                             const char *path) {
    size_t length;
    uint32_t found;
    if (!registry || slot >= OBS_TEXTURE_REF_LIMIT ||
        registry->entries[slot].state != OBS_TEXTURE_REF_UNUSED ||
        !path_length(path, &length) ||
        obs_texture_refs_find(registry, path, &found)) return false;

    size_t bucket = bucket_for(path, length);
    size_t first_tombstone = OBS_TEXTURE_REF_TABLE_SIZE;
    for (size_t i = 0; i < OBS_TEXTURE_REF_TABLE_SIZE; ++i) {
        uint32_t value = registry->lookup[bucket];
        if (value == UINT32_MAX && first_tombstone == OBS_TEXTURE_REF_TABLE_SIZE)
            first_tombstone = bucket;
        if (!value) {
            if (first_tombstone != OBS_TEXTURE_REF_TABLE_SIZE)
                bucket = first_tombstone;
            ObsTextureRefEntry *entry = &registry->entries[slot];
            memcpy(entry->path, path, length + 1u);
            entry->references = 1;
            entry->state = OBS_TEXTURE_REF_LIVE;
            registry->lookup[bucket] = slot + 1u;
            return true;
        }
        bucket = (bucket + 1u) & (OBS_TEXTURE_REF_TABLE_SIZE - 1u);
    }
    return false;
}

bool obs_texture_refs_retain(ObsTextureRefs *registry, uint32_t slot) {
    if (!registry || slot >= OBS_TEXTURE_REF_LIMIT) return false;
    ObsTextureRefEntry *entry = &registry->entries[slot];
    if ((entry->state != OBS_TEXTURE_REF_LIVE &&
         entry->state != OBS_TEXTURE_REF_PENDING) ||
        entry->references == UINT32_MAX) return false;
    if (entry->state == OBS_TEXTURE_REF_PENDING) {
        if (!registry->pending) return false;
        --registry->pending;
        entry->state = OBS_TEXTURE_REF_LIVE;
    }
    ++entry->references;
    return true;
}

bool obs_texture_refs_release(ObsTextureRefs *registry, uint32_t slot) {
    if (!registry || slot >= OBS_TEXTURE_REF_LIMIT) return false;
    ObsTextureRefEntry *entry = &registry->entries[slot];
    if (entry->state != OBS_TEXTURE_REF_LIVE || !entry->references) return false;
    if (--entry->references == 0) {
        entry->state = OBS_TEXTURE_REF_PENDING;
        ++registry->pending;
    }
    return true;
}

bool obs_texture_refs_retire(ObsTextureRefs *registry, uint32_t slot) {
    if (!registry || slot >= OBS_TEXTURE_REF_LIMIT) return false;
    ObsTextureRefEntry *entry = &registry->entries[slot];
    if (entry->state != OBS_TEXTURE_REF_PENDING || entry->references != 0 ||
        !registry->pending) return false;
    size_t length = strlen(entry->path);
    size_t bucket = bucket_for(entry->path, length);
    bool found = false;
    for (size_t i = 0; i < OBS_TEXTURE_REF_TABLE_SIZE; ++i) {
        uint32_t value = registry->lookup[bucket];
        if (!value) break;
        if (value == slot + 1u) {
            registry->lookup[bucket] = UINT32_MAX;
            found = true;
            break;
        }
        bucket = (bucket + 1u) & (OBS_TEXTURE_REF_TABLE_SIZE - 1u);
    }
    if (!found) return false;
    entry->state = OBS_TEXTURE_REF_RETIRED;
    --registry->pending;
    return true;
}

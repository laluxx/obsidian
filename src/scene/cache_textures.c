#include "cache_textures.h"
#include <stdlib.h>
#include <string.h>

/// Stable path hashing
static uint64_t path_hash(const char *p, size_t n) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

void obs_cache_textures_dispose(ObsCacheTexturePlan *plan) {
    if (!plan) return;
    for (size_t i = 0; i < plan->unique_count; ++i) free(plan->paths[i]);
    free(plan->paths);
    free(plan->indices);
    free(plan->references);
    *plan = (ObsCacheTexturePlan){0};
}

/// Failure-atomic plan preparation
bool obs_cache_textures_prepare(ObsCacheTexturePlan *out, size_t meshes,
                                ObsCacheTexturePath get_path,
                                const void *source) {
    if (!out || out->paths || out->indices || out->references ||
        out->mesh_count || out->unique_count || out->resolved ||
        !get_path || !source ||
        meshes > UINT32_MAX / OBS_CACHE_TEXTURE_CHANNELS ||
        meshes > SIZE_MAX / OBS_CACHE_TEXTURE_CHANNELS) return false;
    const size_t slots = meshes * OBS_CACHE_TEXTURE_CHANNELS;
    ObsCacheTexturePlan plan = {.mesh_count = meshes};
    if (!slots) { *out = plan; return true; }
    if (slots > SIZE_MAX / sizeof(*plan.paths) ||
        slots > SIZE_MAX / sizeof(*plan.indices) ||
        slots > SIZE_MAX / sizeof(*plan.references) ||
        slots > (SIZE_MAX / 2) - 1) return false;

    /* Open addressing keeps deduplication near O(n) even on large scenes.
     * The table is temporary and is never retained by an engine object. */
    size_t table_capacity = 8;
    while (table_capacity < slots * 2) {
        if (table_capacity > SIZE_MAX / 2) return false;
        table_capacity *= 2;
    }
    uint32_t *table = calloc(table_capacity, sizeof(*table));
    plan.paths = calloc(slots, sizeof(*plan.paths));
    plan.indices = malloc(slots * sizeof(*plan.indices));
    plan.references = malloc(slots * sizeof(*plan.references));
    if (!table || !plan.paths || !plan.indices || !plan.references) goto fail;
    for (size_t i = 0; i < slots; ++i)
        plan.references[i] = OBS_CACHE_TEXTURE_MISSING;

    for (size_t mesh = 0; mesh < meshes; ++mesh) {
        for (unsigned channel = 0; channel < OBS_CACHE_TEXTURE_CHANNELS; ++channel) {
            const char *path = get_path(source, mesh, (ObsCacheTextureChannel)channel);
            if (!path) goto fail;
            /* Never scan past an OMDL fixed-width metadata field. */
            const char *end = memchr(path, '\0', OBS_CACHE_TEXTURE_PATH_MAX);
            if (!end) goto fail;
            size_t len = (size_t)(end - path);
            if (!len) continue;
            size_t bucket = (size_t)path_hash(path, len) & (table_capacity - 1);
            size_t id;
            for (;;) {
                if (!table[bucket]) {
                    id = plan.unique_count++;
                    char *copy = malloc(len + 1);
                    if (!copy) goto fail;
                    memcpy(copy, path, len + 1);
                    plan.paths[id] = copy;
                    table[bucket] = (uint32_t)id + 1;
                    break;
                }
                id = (size_t)table[bucket] - 1;
                if (strcmp(plan.paths[id], path) == 0) break;
                bucket = (bucket + 1) & (table_capacity - 1);
            }
            plan.references[mesh * OBS_CACHE_TEXTURE_CHANNELS + channel] = (uint32_t)id;
        }
    }
    free(table);
    *out = plan;
    return true;
fail:
    free(table);
    obs_cache_textures_dispose(&plan);
    return false;
}

/// Deferred engine texture registration
bool obs_cache_textures_resolve(ObsCacheTexturePlan *plan,
                                ObsCacheTextureLoad load, void *user) {
    if (!plan || !load || plan->resolved ||
        (plan->unique_count && (!plan->paths || !plan->indices))) return false;
    for (size_t i = 0; i < plan->unique_count; ++i)
        plan->indices[i] = load(user, plan->paths[i]);
    plan->resolved = true;
    return true;
}

bool obs_cache_textures_indices(const ObsCacheTexturePlan *plan, size_t mesh,
                                 int32_t result[OBS_CACHE_TEXTURE_CHANNELS]) {
    if (!plan || !plan->resolved || !result || mesh >= plan->mesh_count ||
        !plan->references) return false;
    int32_t staged[OBS_CACHE_TEXTURE_CHANNELS];
    for (unsigned i = 0; i < OBS_CACHE_TEXTURE_CHANNELS; ++i) {
        uint32_t ref = plan->references[mesh * OBS_CACHE_TEXTURE_CHANNELS + i];
        if (ref != OBS_CACHE_TEXTURE_MISSING && ref >= plan->unique_count) return false;
        staged[i] = ref == OBS_CACHE_TEXTURE_MISSING ? -1 : plan->indices[ref];
    }
    memcpy(result, staged, sizeof(staged));
    return true;
}

/// Owned texture-resource transactions
void obs_cache_texture_lease_release(ObsCacheTextureLease *lease) {
    if (!lease) return;
    if (lease->release) {
        /* Release once PER acquisition. Id -1 is a missing optional map. */
        for (size_t i = lease->count; i > 0; --i)
            lease->release(lease->user, lease->handles[i - 1]);
    }
    free(lease->handles);
    *lease = (ObsCacheTextureLease){0};
}

bool obs_cache_texture_lease_move(ObsCacheTextureLease *dst,
                                   ObsCacheTextureLease *src) {
    if (!dst || !src || dst == src || !src->initialized ||
        dst->initialized || dst->handles || dst->count || dst->release ||
        dst->user) return false;
    *dst = *src;
    *src = (ObsCacheTextureLease){0};
    return true;
}

bool obs_cache_textures_stage_retained(ObsCacheTexturePlan *plan,
                                       ObsCacheTextureAcquire acquire,
                                       ObsCacheTextureRelease release,
                                       void *user,
                                       ObsCacheTextureLease *out) {
    if (!plan || !acquire || !release || !out || plan->resolved ||
        out->initialized || out->handles || out->count ||
        out->release || out->user ||
        (plan->unique_count && (!plan->paths || !plan->indices)) ||
        plan->unique_count > SIZE_MAX / sizeof(int32_t)) return false;

    /* Allocate BEFORE the first host resource is acquired. No allocator
     * or copying is required at publication/lease transfer time. */
    int32_t *owned = plan->unique_count
        ? malloc(plan->unique_count * sizeof(*owned)) : NULL;
    if (plan->unique_count && !owned) return false;
    ObsCacheTextureLease staged = {
        .handles = owned, .release = release, .user = user,
        .initialized = true
    };
    for (size_t i = 0; i < plan->unique_count; ++i) {
        int32_t id = -1;
        if (!acquire(user, plan->paths[i], &id) || id < -1) {
            obs_cache_texture_lease_release(&staged);
            /* Never let a failed transaction expose partially filled IDs. */
            memset(plan->indices, 0, plan->unique_count * sizeof(int32_t));
            return false;
        }
        plan->indices[i] = id;
        if (id >= 0) staged.handles[staged.count++] = id;
    }
    plan->resolved = true;
    *out = staged;
    return true;
}

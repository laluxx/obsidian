/// cache_textures — owned, side-effect-free cache texture descriptor staging
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Semantic slots
/* This ordering is the native OMDL mesh material channel order. It is not
 * an on-disk structure and does not extend the cache ABI. */
typedef enum {
    OBS_CACHE_ALBEDO = 0,
    OBS_CACHE_NORMAL,
    OBS_CACHE_METALLIC_ROUGHNESS,
    OBS_CACHE_OCCLUSION,
    OBS_CACHE_EMISSIVE,
    OBS_CACHE_TRANSMISSION,
    OBS_CACHE_THICKNESS,
    OBS_CACHE_TEXTURE_CHANNELS
} ObsCacheTextureChannel;

#define OBS_CACHE_TEXTURE_MISSING UINT32_MAX
#define OBS_CACHE_TEXTURE_PATH_MAX 512u

typedef const char *(*ObsCacheTexturePath)(const void *source, size_t mesh,
                                          ObsCacheTextureChannel channel);
typedef int32_t (*ObsCacheTextureLoad)(void *user, const char *path);

/// Releasable texture capabilities for pending scenes
/* An acquire callback must either return one owned reference (id >= 0),
 * report an optional missing texture (id == -1), or reject without retaining
 * any reference itself. Every successful id >= 0 acquisition must be paired
 * with exactly one release, even if different paths return the same id.
 * Resource callbacks run only on the engine owner thread. */
typedef bool (*ObsCacheTextureAcquire)(void *user, const char *path,
                                        int32_t *out_id);
typedef void (*ObsCacheTextureRelease)(void *user, int32_t id);
typedef struct {
    int32_t *handles;
    size_t count;
    ObsCacheTextureRelease release;
    void *user;
    bool initialized;
} ObsCacheTextureLease;

/// Owned private manifest
/* Paths are copied into this owner and interned across all meshes/channels.
 * `references[mesh * CHANNELS + channel]` is an interned-path index or MISSING.
 * No Vulkan operations, filesystem access or texture-pool registrations occur
 * during prepare. A successful load() resolves each distinct path once.
 * A negative texture-pool index denotes an unavailable optional texture,
 * matching the legacy material fallback. GPU residency is NOT guaranteed. */
typedef struct {
    char **paths;
    int32_t *indices;
    uint32_t *references;
    size_t mesh_count;
    size_t unique_count;
    bool resolved;
} ObsCacheTexturePlan;

bool obs_cache_textures_prepare(ObsCacheTexturePlan *out, size_t meshes,
                                ObsCacheTexturePath get_path,
                                const void *source);
bool obs_cache_textures_resolve(ObsCacheTexturePlan *plan,
                                ObsCacheTextureLoad load, void *user);
bool obs_cache_textures_indices(const ObsCacheTexturePlan *plan, size_t mesh,
                                 int32_t result[OBS_CACHE_TEXTURE_CHANNELS]);
void obs_cache_textures_dispose(ObsCacheTexturePlan *plan);

/// Owned staging for asynchronous scene candidates
/* Unlike legacy resolve(), stage_retained() returns an independently owned
 * lease. All successful acquisitions are released in reverse order on a
 * subsequent failure; plan->resolved remains false and the output lease
 * remains empty. Success grants NO scene visibility or GPU residency proof.
 * A candidate must retain the lease through GPU completion, then move it to
 * the published scene's owner. A cancelled candidate releases it instead.
 * This API cannot safely wrap a texture_pool_add() without a matching
 * per-acquisition release operation: the current Vulkan pool lacks that. */
bool obs_cache_textures_stage_retained(ObsCacheTexturePlan *plan,
                                       ObsCacheTextureAcquire acquire,
                                       ObsCacheTextureRelease release,
                                       void *user,
                                       ObsCacheTextureLease *out);
bool obs_cache_texture_lease_move(ObsCacheTextureLease *dst,
                                   ObsCacheTextureLease *src);
void obs_cache_texture_lease_release(ObsCacheTextureLease *lease);

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "gltf_async.h"
#include "renderer.h"
#include "context.h"
#include "cache_textures.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

/// Private scene asset owner
/* Renderer-thread-owned payload. Worker publishes only its CPU cache record. */
typedef struct {
    Scene *scene;
    ObsGltfCacheCandidate *cpu;
    ObsCacheTextureLease textures;
    size_t mesh_start;
    size_t instance_start;
    int32_t tree_start;
    uint64_t mesh_revision, gltf_revision, tree_revision;
    bool activated;
} ObsAsyncPreparedScene;

static void dispose_scene(void *object, void *user) {
    (void)user;
    ObsAsyncPreparedScene *p = object;
    if (!p) return;
    obs_cache_texture_lease_release(&p->textures);
    obs_gltf_cache_destroy(p->cpu);
    free(p);
}

static bool all_textures_ready(ObsAsyncPreparedScene *p) {
    for (size_t i = 0; i < p->textures.count; ++i) {
        int32_t handle = p->textures.handles[i];
        if (handle < 0) continue;
        Texture2D *texture = texture_pool_get(handle);
        if (!texture || texture->status == TEXTURE_STATUS_FAILED) return false;
        if (texture->status != TEXTURE_STATUS_READY || !texture->loaded) return false;
    }
    return true;
}

static bool scene_ready(const void *object, void *user) {
    (void)user;
    const ObsAsyncPreparedScene *p = object;
    if (!p || !p->scene || !p->cpu || !p->cpu->graph) return false;
    const Scene *scene = p->scene;
    /* No third-party scene mutation may invalidate our reserved capacity. */
    return scene->meshes.count == p->mesh_start &&
           scene->meshes.capacity >= p->mesh_start + p->cpu->mesh_count &&
           scene->gltf_instance_count == p->instance_start &&
           scene->gltf_instance_capacity > p->instance_start &&
           scene->tree.count == p->tree_start &&
           scene->mesh_revision == p->mesh_revision &&
           scene->gltf_revision == p->gltf_revision &&
           scene->tree.revision == p->tree_revision &&
           all_textures_ready((ObsAsyncPreparedScene *)p);
}

static void scene_activate(void *object, void *user, const ObsSceneGpuBases *bases) {
    (void)user;
    ObsAsyncPreparedScene *p = object;
    Scene *s = p->scene;
    ObsGltfCacheCandidate *c = p->cpu;
    const size_t base_mesh = s->meshes.count;
    size_t morph_offset = 0;
    size_t joint_offset = 0;
    for (size_t i = 0; i < base_mesh; ++i) morph_offset += s->meshes.items[i].morphCount;
    for (size_t i = 0; i < s->gltf_instance_count; ++i) {
        OmdlSceneGraph *g = (OmdlSceneGraph *)s->gltf_instances[i].gltf_data;
        if (!g) continue;
        for (uint32_t k = 0; k < g->skin_count; ++k)
            joint_offset += g->skins[k].joints_count;
    }
    for (size_t i = 0; i < c->mesh_count; ++i) {
        Mesh *m = &c->meshes[i];
        OmdlMeshMeta *meta = &c->metadata[i];
        m->megaBaseVertex = bases->vertices + meta->vertexOffset;
        m->megaBaseIndex = meta->indexCount ? bases->indices + meta->indexOffset : UINT32_MAX;
        m->megaBaseMeshlet = bases->meshlets + meta->meshletOffset;
        m->megaBaseMeshletVertex = bases->meshlet_vertices + meta->meshletVertexOffset;
        m->megaBaseMeshletTriangle = bases->meshlet_triangles + meta->meshletTriangleOffset;
        m->morphDeltaOffset = meta->morphCount ? bases->morphs + meta->morphOffset : UINT32_MAX;
        m->morphWeightOffset = (int)morph_offset;
        morph_offset += m->morphCount;
        if (m->jointCount) {
            int32_t skin = -1;
            /* Node 0 is represented by a null legacy pointer: consult the
             * validated graph identity rather than testing pointer truth. */
            for (uint32_t n = 0; n < c->graph->node_count; ++n)
                if (c->graph->nodes[n].mesh_idx == (int32_t)i) {
                    skin = c->graph->nodes[n].skin_idx;
                    break;
                }
            if (skin >= 0) {
                size_t base = joint_offset;
                for (int32_t k = 0; k < skin; ++k)
                    base += c->graph->skins[k].joints_count;
                m->jointOffset = (int)base;
            }
        }
        int32_t indices[OBS_CACHE_TEXTURE_CHANNELS];
        bool resolved = obs_cache_textures_indices(&c->textures, i, indices);
        /* Guaranteed by successful private texture-manifest staging. */
        if (resolved) {
            m->textureIndex = indices[OBS_CACHE_ALBEDO];
            m->normalMapIndex = indices[OBS_CACHE_NORMAL];
            m->metallicRoughIndex = indices[OBS_CACHE_METALLIC_ROUGHNESS];
            m->aoIndex = indices[OBS_CACHE_OCCLUSION];
            m->emissiveIndex = indices[OBS_CACHE_EMISSIVE];
            m->transmissionIndex = indices[OBS_CACHE_TRANSMISSION];
            m->thicknessIndex = indices[OBS_CACHE_THICKNESS];
        }
    }
    GLTFInstance *instance = &s->gltf_instances[s->gltf_instance_count];
    *instance = (GLTFInstance){0};
    instance->mesh_start_index = base_mesh;
    glm_mat4_identity(instance->instance_transform);
    instance->mesh_count = c->mesh_count;
    instance->gltf_data = c->graph;
    c->graph = NULL;
    /* Move leases without allocation or another acquire. */
    (void)obs_cache_texture_lease_move(&instance->texture_lease, &p->textures);
    (void)meshes_append_prepared(&s->meshes, c->meshes, c->mesh_count);
    /* Mesh names/morphs now belong to Scene, not the private CPU package. */
    free(c->meshes);
    c->meshes = NULL;
    c->mesh_count = 0;
    ++s->gltf_instance_count;
    scene_bump_gltf_revision(s);
    scene_tree_register_gltf(s, instance, c->source_path);
    markMeshesSSBODirty(&context);
    p->activated = true;
    dispose_scene(p, NULL);
}

/// Worker thread — no Vulkan, Scene mutation or texture ownership
static void *prepare_worker(void *user) {
    ObsAsyncGltf *owner = user;
    ObsGltfCacheCandidate *candidate = NULL;
    bool accepted = obs_gltf_cache_prepare_cpu(owner->source_path, &candidate);
    if (!accepted) accepted = obs_gltf_cook_static_cpu(owner->source_path, &candidate);
    ObsGltfSourcePlan plan = {0};
    ObsGltfPlanError probe_error = OBS_GLTF_PLAN_OK;
    bool needs_cook = false;
    if (!accepted) {
        /* An uncached model is not malformed just because OMDL is missing.
         * Inspect the real cgltf source off-thread; do NOT call load_gltf(),
         * which mutates global cooker and renderer state. */
        probe_error = obs_gltf_source_plan(owner->source_path, &plan);
        needs_cook = probe_error == OBS_GLTF_PLAN_OK;
    }
    pthread_mutex_lock(&owner->mutex);
    owner->source_plan = plan;
    owner->source_plan_error = probe_error;
    owner->worker_needs_cook = needs_cook;
    owner->prepared = candidate;
    owner->worker_ok = accepted;
    owner->worker_done = true;
    pthread_mutex_unlock(&owner->mutex);
    return NULL;
}

static bool start_worker(ObsAsyncGltf *owner, uint64_t ticket, char *path) {
    owner->source_path = path;
    owner->ticket = ticket;
    owner->prepared = NULL;
    owner->worker_done = false;
    owner->worker_ok = false;
    owner->worker_needs_cook = false;
    owner->source_plan = (ObsGltfSourcePlan){0};
    owner->source_plan_error = OBS_GLTF_PLAN_OK;
    owner->cancellation_requested = false;
    owner->worker_running = true;
    owner->status = OBS_GLTF_ASYNC_PREPARING;
    if (pthread_create(&owner->worker, NULL, prepare_worker, owner) != 0) {
        /* A failed launch never produced a worker. Retire this ticket now,
         * including when it was previously returned as a queued request.
         * Otherwise poll() can report a terminal state without retiring its
         * ownership, and the next request can silently overwrite the ticket. */
        owner->worker_running = false;
        owner->status = OBS_GLTF_ASYNC_FAILED;
        owner->last_ticket = ticket;
        owner->last_status = OBS_GLTF_ASYNC_FAILED;
        owner->ticket = 0;
        owner->cancellation_requested = false;
        ++owner->failed;
        free(owner->source_path);
        owner->source_path = NULL;
        return false;
    }
    return true;
}

bool obs_gltf_async_init(ObsAsyncGltf *owner, Scene *scene, VulkanContext *context) {
    if (!owner || !scene || !context) return false;
    memset(owner, 0, sizeof(*owner));
    if (pthread_mutex_init(&owner->mutex, NULL) != 0) return false;
    if (!obs_vk_scene_load_bridge_init(&owner->bridge, context)) {
        pthread_mutex_destroy(&owner->mutex);
        return false;
    }
    ObsSceneLoadBackend backend = obs_vk_scene_load_backend(&owner->bridge);
    if (!obs_scene_load_init(&owner->pending, &backend)) {
        pthread_mutex_destroy(&owner->mutex);
        return false;
    }
    owner->target = scene;
    owner->initialized = true;
    return true;
}

uint64_t obs_gltf_async_request(ObsAsyncGltf *owner, const char *path) {
    if (!owner || !owner->initialized || !path) return 0;
    const size_t length = strnlen(path, 4096);
    if (!length || length >= 4096) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    if (owner->serial == UINT64_MAX) { free(copy); return 0; }
    uint64_t issued = ++owner->serial;
    ++owner->requests;
    if (owner->worker_running || owner->pending.state != OBS_LOAD_EMPTY ||
        owner->status == OBS_GLTF_ASYNC_TEXTURES) {
        free(owner->next_path);
        owner->next_path = copy;
        owner->next_ticket = issued;
        owner->cancellation_requested = true;
    } else if (!start_worker(owner, issued, copy)) {
        /* There is no accepted request when the initial thread cannot be
         * started. A queued request is different: its ticket was already
         * issued, and start_worker records terminal failure if it cannot run. */
        return 0;
    }
    return issued;
}

static void finish_request(ObsAsyncGltf *owner, ObsGltfAsyncStatus status) {
    owner->status = status;
    owner->last_ticket = owner->ticket;
    owner->last_status = status;
    if (status == OBS_GLTF_ASYNC_CANCELLED) ++owner->cancelled;
    if (status == OBS_GLTF_ASYNC_FAILED) ++owner->failed;
    if (status == OBS_GLTF_ASYNC_NEEDS_COOK) ++owner->cache_misses;
    if (status == OBS_GLTF_ASYNC_PUBLISHED) ++owner->published;
    owner->ticket = 0;
    owner->gpu_ticket = 0;
    owner->cancellation_requested = false;
    free(owner->source_path);
    owner->source_path = NULL;
    if (owner->next_path) {
        char *next = owner->next_path;
        uint64_t ticket = owner->next_ticket;
        owner->next_path = NULL;
        owner->next_ticket = 0;
        (void)start_worker(owner, ticket, next);
    }
}

static bool adopt_worker_result(ObsAsyncGltf *owner, ObsGltfCacheCandidate *candidate) {
    Scene *scene = owner->target;
    if (!candidate || candidate->mesh_count > (SCENE_TREE_MAX_NODES - 1) / 2 ||
        scene->tree.count < 0 ||
        (size_t)scene->tree.count > SCENE_TREE_MAX_NODES - 1 - candidate->mesh_count * 2 ||
        candidate->mesh_count > SIZE_MAX - scene->meshes.count ||
        scene->meshes.count + candidate->mesh_count > INT32_MAX ||
        !meshes_reserve(&scene->meshes, scene->meshes.count + candidate->mesh_count)) {
        fprintf(stderr, "adopt GLTF rejected mesh/tree capacity\n");
        return false;
    }
    /* GPU joint/morph arrays have fixed shader-visible limits; reject before
     * submitting rather than silently overflowing their host offset fields. */
    size_t morph_used = 0;
    size_t joints_used = 0;
    for (size_t i = 0; i < scene->meshes.count; ++i) {
        int count = scene->meshes.items[i].morphCount;
        if (count < 0 || (size_t)count > 262144u - morph_used) return false;
        morph_used += (size_t)count;
    }
    for (size_t i = 0; i < candidate->mesh_count; ++i) {
        int count = candidate->meshes[i].morphCount;
        if (count < 0 || (size_t)count > 262144u - morph_used) return false;
        morph_used += (size_t)count;
    }
    for (size_t i = 0; i < scene->gltf_instance_count; ++i) {
        OmdlSceneGraph *g = (OmdlSceneGraph *)scene->gltf_instances[i].gltf_data;
        if (!g) continue;
        for (uint32_t k = 0; k < g->skin_count; ++k) {
            if (g->skins[k].joints_count > INT_MAX - joints_used) return false;
            joints_used += g->skins[k].joints_count;
        }
    }
    for (uint32_t k = 0; k < candidate->graph->skin_count; ++k) {
        if (candidate->graph->skins[k].joints_count > INT_MAX - joints_used) return false;
        joints_used += candidate->graph->skins[k].joints_count;
    }
    if (scene->gltf_instance_count == scene->gltf_instance_capacity) {
        size_t old = scene->gltf_instance_capacity;
        size_t cap = old ? old * 2 : 4;
        if (cap < old || cap > SIZE_MAX / sizeof(GLTFInstance)) return false;
        GLTFInstance *arr = realloc(scene->gltf_instances, cap * sizeof(GLTFInstance));
        if (!arr) return false;
        scene->gltf_instances = arr;
        scene->gltf_instance_capacity = cap;
    }
    ObsAsyncPreparedScene *payload = calloc(1, sizeof(*payload));
    if (!payload) return false;
    payload->scene = scene;
    payload->cpu = candidate;
    payload->mesh_start = scene->meshes.count;
    payload->instance_start = scene->gltf_instance_count;
    payload->tree_start = scene->tree.count;
    payload->mesh_revision = scene->mesh_revision;
    payload->gltf_revision = scene->gltf_revision;
    payload->tree_revision = scene->tree.revision;
    if (!obs_cache_textures_stage_retained(&candidate->textures,
                                            texture_pool_lease_acquire,
                                            texture_pool_lease_release,
                                            &context, &payload->textures)) {
        payload->cpu = NULL;
        dispose_scene(payload, NULL);
        return false;
    }
    ObsSceneLoadPayload owned = {
        .object = payload, .dispose = dispose_scene,
        .ready = scene_ready, .activate = scene_activate
    };
    uint64_t gpu_ticket = 0;
    if (!obs_scene_load_prepare(&owner->pending, owner->ticket,
                                 candidate->counts, candidate->spans,
                                 &owned, &gpu_ticket)) {
        payload->cpu = NULL;
        dispose_scene(payload, NULL);
        return false;
    }
    owner->gpu_ticket = gpu_ticket;
    owner->owned_payload = payload;
    owner->status = OBS_GLTF_ASYNC_UPLOADING;
    return true;
}

ObsGltfAsyncStatus obs_gltf_async_poll(ObsAsyncGltf *owner, uint64_t ticket) {
    if (!owner || !owner->initialized || !ticket) return OBS_GLTF_ASYNC_IDLE;
    /* Superseded or completed ticket: never touch current generation. */
    if (ticket == owner->last_ticket) return owner->last_status;
    if (ticket == owner->next_ticket) return OBS_GLTF_ASYNC_PREPARING;
    if (ticket != owner->ticket) return OBS_GLTF_ASYNC_CANCELLED;
    if (owner->status == OBS_GLTF_ASYNC_PREPARING) {
        pthread_mutex_lock(&owner->mutex);
        bool done = owner->worker_done;
        pthread_mutex_unlock(&owner->mutex);
        if (!done) return owner->status;
        /* A finished-result flag precedes the worker's actual return.
         * pthread_tryjoin_np guarantees this checkpoint never waits for its
         * final scheduler timeslice; shutdown alone permits a blocking join. */
        int joined = pthread_tryjoin_np(owner->worker, NULL);
        if (joined == EBUSY) return OBS_GLTF_ASYNC_PREPARING;
        if (joined != 0) {
            owner->status = OBS_GLTF_ASYNC_QUARANTINED;
            return owner->status;
        }
        owner->worker_running = false;
        ObsGltfCacheCandidate *candidate = owner->prepared;
        owner->prepared = NULL;
        if (owner->cancellation_requested) {
            obs_gltf_cache_destroy(candidate);
            finish_request(owner, OBS_GLTF_ASYNC_CANCELLED);
            return OBS_GLTF_ASYNC_CANCELLED;
        }
        if (!owner->worker_ok && owner->worker_needs_cook) {
            obs_gltf_cache_destroy(candidate);
            finish_request(owner, OBS_GLTF_ASYNC_NEEDS_COOK);
            return OBS_GLTF_ASYNC_NEEDS_COOK;
        }
        if (!owner->worker_ok || !adopt_worker_result(owner, candidate)) {
            obs_gltf_cache_destroy(candidate);
            finish_request(owner, OBS_GLTF_ASYNC_FAILED);
            return OBS_GLTF_ASYNC_FAILED;
        }
    }
    if (owner->pending.state != OBS_LOAD_EMPTY) {
        ObsSceneLoadState state = obs_scene_load_poll(&owner->pending, owner->gpu_ticket);
        if (state == OBS_LOAD_QUARANTINED) {
            owner->status = OBS_GLTF_ASYNC_QUARANTINED;
            return owner->status;
        }
        if (state == OBS_LOAD_IN_FLIGHT) return OBS_GLTF_ASYNC_UPLOADING;
        if (state == OBS_LOAD_READY) {
            if (owner->cancellation_requested) {
                if (!obs_scene_load_cancel(&owner->pending, owner->gpu_ticket)) {
                    owner->status = OBS_GLTF_ASYNC_QUARANTINED;
                    return owner->status;
                }
                owner->owned_payload = NULL;
                finish_request(owner, OBS_GLTF_ASYNC_CANCELLED);
                return OBS_GLTF_ASYNC_CANCELLED;
            }
            ObsAsyncPreparedScene *payload = owner->owned_payload;
            if (!payload) {
                owner->status = OBS_GLTF_ASYNC_QUARANTINED;
                return owner->status;
            }
            bool failed_texture = false;
            for (size_t i = 0; i < payload->textures.count; ++i) {
                Texture2D *tex = texture_pool_get(payload->textures.handles[i]);
                if (!tex || tex->status == TEXTURE_STATUS_FAILED) {
                    failed_texture = true;
                    break;
                }
            }
            if (failed_texture) {
                if (!obs_scene_load_cancel(&owner->pending, owner->gpu_ticket)) {
                    owner->status = OBS_GLTF_ASYNC_QUARANTINED;
                    return owner->status;
                }
                owner->owned_payload = NULL;
                finish_request(owner, OBS_GLTF_ASYNC_FAILED);
                return OBS_GLTF_ASYNC_FAILED;
            }
            if (!scene_ready(payload, NULL)) {
                /* Scene changed concurrently at the checkpoint: reject rather
                 * than keep a permanently unpublishable candidate. */
                if (payload->scene->meshes.count != payload->mesh_start ||
                    payload->scene->gltf_instance_count != payload->instance_start ||
                    payload->scene->tree.count != payload->tree_start ||
                    payload->scene->meshes.capacity < payload->mesh_start + payload->cpu->mesh_count ||
                    payload->scene->mesh_revision != payload->mesh_revision ||
                    payload->scene->gltf_revision != payload->gltf_revision ||
                    payload->scene->tree.revision != payload->tree_revision) {
                    if (!obs_scene_load_cancel(&owner->pending, owner->gpu_ticket)) {
                        owner->status = OBS_GLTF_ASYNC_QUARANTINED;
                        return owner->status;
                    }
                    owner->owned_payload = NULL;
                    finish_request(owner, OBS_GLTF_ASYNC_CANCELLED);
                    return OBS_GLTF_ASYNC_CANCELLED;
                }
                owner->status = OBS_GLTF_ASYNC_TEXTURES;
                return owner->status;
            }
            ObsSceneGpuBases bases;
            if (!obs_scene_load_publish(&owner->pending, owner->gpu_ticket, &bases)) {
                owner->status = OBS_GLTF_ASYNC_QUARANTINED;
                return owner->status;
            }
            owner->owned_payload = NULL;
            finish_request(owner, OBS_GLTF_ASYNC_PUBLISHED);
            return OBS_GLTF_ASYNC_PUBLISHED;
        }
    }
    return owner->status;
}

bool obs_gltf_async_cancel(ObsAsyncGltf *owner, uint64_t ticket) {
    if (!owner || !owner->initialized || !ticket) return false;
    if (ticket == owner->next_ticket) {
        free(owner->next_path);
        owner->next_path = NULL;
        owner->next_ticket = 0;
        return true;
    }
    if (ticket != owner->ticket) return false;
    owner->cancellation_requested = true;
    return true;
}

bool obs_gltf_async_close(ObsAsyncGltf *owner) {
    if (!owner || !owner->initialized || owner->worker_running ||
        owner->pending.state != OBS_LOAD_EMPTY || owner->next_path) return false;
    if (!obs_scene_load_close(&owner->pending)) return false;
    pthread_mutex_destroy(&owner->mutex);
    free(owner->source_path);
    owner->initialized = false;
    return true;
}

/// Shutdown-only GPU quiescence and worker drainage
bool obs_gltf_async_shutdown_quiesced(ObsAsyncGltf *owner) {
    if (!owner || !owner->initialized) return false;
    free(owner->next_path);
    owner->next_path = NULL;
    owner->next_ticket = 0;
    owner->cancellation_requested = true;
    if (owner->worker_running) {
        /* A join is forbidden in poll(), but permitted at process shutdown. */
        pthread_join(owner->worker, NULL);
        owner->worker_running = false;
        obs_gltf_cache_destroy(owner->prepared);
        owner->prepared = NULL;
    }
    if (owner->pending.state != OBS_LOAD_EMPTY) {
        ObsSceneLoadState state = obs_scene_load_poll(&owner->pending,
                                                       owner->gpu_ticket);
        if (state == OBS_LOAD_READY) {
            if (!obs_scene_load_cancel(&owner->pending, owner->gpu_ticket))
                return false;
        } else if (state == OBS_LOAD_QUARANTINED) {
            if (!obs_scene_load_recover_quiesced(&owner->pending,
                                                 owner->gpu_ticket, true))
                return false;
        } else return false; /* Device-idle proof was not reflected by fence. */
    }
    owner->owned_payload = NULL;
    free(owner->source_path);
    owner->source_path = NULL;
    return obs_gltf_async_close(owner);
}

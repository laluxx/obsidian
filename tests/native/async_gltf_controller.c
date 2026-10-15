/// async_gltf_controller — execute actual production controller with fake GPU only
#include "async_gltf_contract.h"
#include <assert.h>
#include <sched.h>
#include <time.h>

VulkanContext context = {0};
bool fake_gpu_ready = false;
unsigned fake_acquires=0,fake_releases=0,fake_dirty=0;
static Texture2D texture = {TEXTURE_STATUS_READY,true};
static uint64_t gpu_serial;
static bool fail_next_worker_launch;
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attrs,
                          void *(*entry)(void *), void *arg) {
    if (fail_next_worker_launch) {
        fail_next_worker_launch = false;
        return EAGAIN;
    }
    return __real_pthread_create(thread, attrs, entry, arg);
}
static const char *fake_path(const void *source,size_t mesh,ObsCacheTextureChannel channel) {
    (void)source;(void)mesh;
    return channel == OBS_CACHE_ALBEDO ? "fixture/texture.png" : "";
}

bool obs_gltf_cook_static_cpu(const char *source,ObsGltfCacheCandidate **out) {
    *out = NULL;
    if (!strcmp(source, "cold-hierarchy"))
        return obs_gltf_cache_prepare_cpu("cached-hierarchy", out);
    if (strcmp(source, "cold-static")) return false;
    /* The real cooker has its own cgltf/geometry test. This controller seam
     * exercises the cold-result route through the actual publication code. */
    return obs_gltf_cache_prepare_cpu("cached-cold", out);
}

bool obs_gltf_cache_prepare_cpu(const char *source,ObsGltfCacheCandidate **out) {
    *out = NULL;
    if (!strcmp(source,"bad-cache") || !strcmp(source,"fresh-uncached") || !strcmp(source,"cold-static") || !strcmp(source,"cold-hierarchy")) return false;
    const size_t meshes = strcmp(source, "cached-hierarchy") == 0 ? 3 : 1;
    ObsGltfCacheCandidate *c = calloc(1,sizeof(*c));
    if (!c) return false;
    c->source_path = strdup(source);
    c->graph = calloc(1,sizeof(*c->graph));
    c->metadata = calloc(meshes,sizeof(*c->metadata));
    c->meshes = calloc(meshes,sizeof(*c->meshes));
    c->mesh_count = meshes;
    if (c->graph && meshes > 1) {
        c->graph->node_count = (uint32_t)meshes;
        c->graph->nodes = calloc(meshes,sizeof(*c->graph->nodes));
    }
    for (size_t i = 0; c->meshes && i < meshes; ++i) {
        c->meshes[i].name = strdup("cold-primitive");
        if (c->metadata) {
            c->metadata[i].vertexOffset = (uint32_t)i * 3;
            c->metadata[i].indexOffset = (uint32_t)i * 3;
            c->metadata[i].indexCount = 3;
            c->metadata[i].meshletOffset = (uint32_t)i;
            c->metadata[i].meshletVertexOffset = (uint32_t)i * 3;
            c->metadata[i].meshletTriangleOffset = (uint32_t)i * 4;
        }
        if (c->graph && c->graph->nodes)
            c->graph->nodes[i].mesh_idx = (int32_t)i;
    }
    c->counts[OBS_GEOM_VERTEX] = meshes > 1 ? 9 : 1;
    c->spans[OBS_GEOM_VERTEX] = (ObsGeometrySourceSpan){
        "012345678901234567890123456789012345", meshes > 1 ? 36 : 4
    };
    if (!c->source_path || !c->graph || !c->metadata || !c->meshes ||
        (meshes > 1 && !c->graph->nodes)) {
        obs_gltf_cache_destroy(c);
        return false;
    }
    for (size_t i=0; i<meshes; ++i) if (!c->meshes[i].name) {
        obs_gltf_cache_destroy(c);
        return false;
    }
    if (!obs_cache_textures_prepare(&c->textures,meshes,fake_path,c)) {
        obs_gltf_cache_destroy(c);
        return false;
    }
    *out=c;
    return true;
}
ObsGltfPlanError obs_gltf_source_plan(const char *path, ObsGltfSourcePlan *out) {
    *out = (ObsGltfSourcePlan){0};
    if (!strcmp(path, "fresh-uncached")) {
        out->meshes = 1;
        out->vertices = 3;
        out->triangles = 1;
        return OBS_GLTF_PLAN_OK;
    }
    return OBS_GLTF_PLAN_INVALID;
}
void obs_gltf_cache_destroy(ObsGltfCacheCandidate *c) {
    if (!c) return;
    for(size_t i=0;i<c->mesh_count;i++) free(c->meshes[i].name);
    free(c->meshes);free(c->metadata);
    if (c->graph) free(c->graph->nodes);
    free(c->graph);free(c->source_path);
    obs_cache_textures_dispose(&c->textures);
    free(c);
}
bool meshes_reserve(Meshes *m,size_t needed) {
    if (needed<=m->capacity) return true;
    Mesh *a=realloc(m->items,needed*sizeof(*a));
    if (!a) return false;
    m->items=a;
    uint32_t *i=realloc(m->draw_indices,needed*sizeof(*i));
    if (!i) return false;
    m->draw_indices=i;m->capacity=needed;
    return true;
}
bool meshes_append_prepared(Meshes *m,Mesh *p,size_t n) {
    if (m->count+n>m->capacity) return false;
    memcpy(&m->items[m->count],p,n*sizeof(*p));
    m->count+=n;return true;
}
void scene_bump_gltf_revision(Scene *s) {s->gltf_revision++;}
void scene_tree_register_gltf(Scene *s,GLTFInstance *i,const char *name) {
    (void)name;s->tree.count+=(int32_t)(1+2*i->mesh_count);s->tree.revision++;
}
void markMeshesSSBODirty(VulkanContext *c) {(void)c;fake_dirty++;}
bool texture_pool_lease_acquire(void *u,const char *p,int32_t *id) {
    (void)u;(void)p;fake_acquires++;*id=9;return true;
}
void texture_pool_lease_release(void *u,int32_t id) {
    (void)u;assert(id==9);fake_releases++;
}
Texture2D *texture_pool_get(int32_t id) {return id==9 ? &texture : NULL;}

static ObsSceneStartResult b_start(void *user,uint64_t id,const uint32_t *count,
                  const ObsGeometrySourceSpan *spans,uint64_t *ticket) {
    ObsVkSceneLoadBridge *b=user;
    if (!id || b->active_ticket ||
        !((count[OBS_GEOM_VERTEX]==1 && spans[OBS_GEOM_VERTEX].bytes==4) ||
          (count[OBS_GEOM_VERTEX]==9 && spans[OBS_GEOM_VERTEX].bytes==36)))
        return OBS_SCENE_START_REJECTED;
    b->active_ticket=*ticket=++gpu_serial;
    fake_gpu_ready=false;
    return OBS_SCENE_START_ACCEPTED;
}
static ObsAsyncState b_poll(void *u,uint64_t ticket) {
    ObsVkSceneLoadBridge *b=u;
    assert(ticket==b->active_ticket);
    return fake_gpu_ready ? OBS_ASYNC_READY : OBS_ASYNC_IN_FLIGHT;
}
static bool b_commit(void *u,uint64_t ticket,ObsSceneGpuBases *out) {
    ObsVkSceneLoadBridge *b=u;
    if (!fake_gpu_ready || ticket!=b->active_ticket) return false;
    *out=(ObsSceneGpuBases){.vertices=10,.indices=20,.meshlets=30,
        .meshlet_vertices=40,.meshlet_triangles=50,.morphs=60};
    b->active_ticket=0;return true;
}
static bool b_discard(void *u,uint64_t ticket) {
    ObsVkSceneLoadBridge *b=u;
    if (!fake_gpu_ready || ticket!=b->active_ticket) return false;
    b->active_ticket=0;return true;
}
static bool b_recover(void *u,uint64_t ticket,bool quiescent) {
    ObsVkSceneLoadBridge *b=u;
    if (!quiescent || ticket!=b->active_ticket) return false;
    b->active_ticket=0;return true;
}
bool obs_vk_scene_load_bridge_init(ObsVkSceneLoadBridge *b,VulkanContext *c) {
    b->context=c;return true;
}
ObsSceneLoadBackend obs_vk_scene_load_backend(ObsVkSceneLoadBridge *b) {
    return (ObsSceneLoadBackend){b_start,b_poll,b_commit,b_discard,b_recover,b};
}
static ObsGltfAsyncStatus step_until(ObsAsyncGltf *o,uint64_t ticket,ObsGltfAsyncStatus desired) {
    for (int n=0;n<20000;n++) {
        ObsGltfAsyncStatus s=obs_gltf_async_poll(o,ticket);
        if (s==desired || s==OBS_GLTF_ASYNC_FAILED || s==OBS_GLTF_ASYNC_CANCELLED ||
            s==OBS_GLTF_ASYNC_QUARANTINED || s==OBS_GLTF_ASYNC_PUBLISHED ||
            s==OBS_GLTF_ASYNC_NEEDS_COOK) {
            if (s != desired) fprintf(stderr, "unexpected status %d desired %d state %d gpu %llu\n", (int)s, (int)desired, (int)o->status, (unsigned long long)o->gpu_ticket);
            return s;
        }
        struct timespec nap = {0, 100000};
        nanosleep(&nap, NULL);
    }
    fprintf(stderr, "timeout ticket=%llu desired=%d state=%d active=%llu last=%llu done=%d running=%d pending=%d needs=%d next=%llu\n",(unsigned long long)ticket, (int)desired, (int)o->status,(unsigned long long)o->ticket,(unsigned long long)o->last_ticket,o->worker_done,o->worker_running,o->pending.state,o->worker_needs_cook,(unsigned long long)o->next_ticket);
    assert(0 && "worker did not advance");return OBS_GLTF_ASYNC_FAILED;
}
int main(void) {
    Scene scene={0};
    assert(meshes_reserve(&scene.meshes,1));
    memset(&scene.meshes.items[0], 0, sizeof(Mesh));
    scene.meshes.items[0].name=strdup("old-scene");
    scene.meshes.count=1;scene.tree.count=1;
    ObsAsyncGltf owner={0};
    assert(obs_gltf_async_init(&owner,&scene,&context));
    /* The owner must never return a live ticket if pthread_create failed.
     * A queued ticket may already have been returned, so failure there must
     * be observable as a completed FAILED ticket and allow a later request. */
    fail_next_worker_launch=true;
    assert(obs_gltf_async_request(&owner,"unlaunchable") == 0);
    assert(!owner.ticket && !owner.source_path && !owner.worker_running);
    assert(owner.status == OBS_GLTF_ASYNC_FAILED && owner.failed == 1);
    uint64_t first=obs_gltf_async_request(&owner,"cached-a");
    assert(first);
    assert(step_until(&owner,first,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    /* Even after CPU preparation and GPU admission, old scene remains live. */
    assert(scene.meshes.count==1 && !strcmp(scene.meshes.items[0].name,"old-scene"));
    assert(obs_gltf_async_poll(&owner,first)==OBS_GLTF_ASYNC_UPLOADING);
    /* Complete geometry but hold textures; no premature scene visibility. */
    texture.loaded=false;fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,first)==OBS_GLTF_ASYNC_TEXTURES);
    assert(scene.meshes.count==1);
    texture.loaded=true;
    assert(obs_gltf_async_poll(&owner,first)==OBS_GLTF_ASYNC_PUBLISHED);
    assert(scene.meshes.count==2 && !strcmp(scene.meshes.items[0].name,"old-scene"));
    assert(scene.meshes.items[1].megaBaseVertex==10);
    assert(scene.gltf_instance_count==1 && scene.gltf_instances[0].texture_lease.count==1);
    assert(obs_gltf_async_poll(&owner,first)==OBS_GLTF_ASYNC_PUBLISHED);
    uint64_t second=obs_gltf_async_request(&owner,"cached-b");
    assert(second && second!=first);
    assert(step_until(&owner,second,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    assert(obs_gltf_async_cancel(&owner,second));
    assert(obs_gltf_async_poll(&owner,second)==OBS_GLTF_ASYNC_UPLOADING);
    fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,second)==OBS_GLTF_ASYNC_CANCELLED);
    assert(scene.meshes.count==2 && scene.gltf_instance_count==1);
    /* A validated source with no OMDL must not be labelled corrupt and
     * must not trigger the unsafe synchronous cooker from a render frame. */
    uint64_t fresh=obs_gltf_async_request(&owner,"fresh-uncached");
    assert(step_until(&owner,fresh,OBS_GLTF_ASYNC_NEEDS_COOK)==OBS_GLTF_ASYNC_NEEDS_COOK);
    assert(owner.cache_misses==1 && owner.source_plan.vertices==3);
    assert(scene.meshes.count==2 && owner.pending.state==OBS_LOAD_EMPTY);
    uint64_t third=obs_gltf_async_request(&owner,"bad-cache");
    assert(step_until(&owner,third,OBS_GLTF_ASYNC_FAILED)==OBS_GLTF_ASYNC_FAILED);
    assert(scene.meshes.count==2);
    /* A failed real texture owner must not publish or leak a scene. */
    uint64_t texture_failure=obs_gltf_async_request(&owner,"cached-texture-failure");
    assert(step_until(&owner,texture_failure,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    texture.status=TEXTURE_STATUS_FAILED;
    fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,texture_failure)==OBS_GLTF_ASYNC_FAILED);
    assert(scene.meshes.count==2 && scene.gltf_instance_count==1);
    texture.status=TEXTURE_STATUS_READY;
    /* A scene structurally replaced before GPU readiness invalidates its
     * prepared publication plan even when mesh counts did not change. */
    uint64_t changed=obs_gltf_async_request(&owner,"cached-stale-scene");
    assert(step_until(&owner,changed,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    scene.mesh_revision++;
    fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,changed)==OBS_GLTF_ASYNC_CANCELLED);
    assert(scene.meshes.count==2);
    /* Newest request supersedes a worker that has not published. Stale
     * tokens never change the active GPU reservation or visible scene. */
    uint64_t staged=obs_gltf_async_request(&owner,"cached-queued-first");
    uint64_t queued=obs_gltf_async_request(&owner,"cached-queued-second");
    assert(staged && queued);
    fail_next_worker_launch=true;
    assert(step_until(&owner,staged,OBS_GLTF_ASYNC_CANCELLED)==OBS_GLTF_ASYNC_CANCELLED);
    assert(obs_gltf_async_poll(&owner,queued)==OBS_GLTF_ASYNC_FAILED);
    assert(!owner.ticket && !owner.worker_running);
    assert(owner.failed == 4); /* initial launch, bad source, texture, queued launch */
    uint64_t fourth=obs_gltf_async_request(&owner,"cached-c");
    uint64_t fifth=obs_gltf_async_request(&owner,"cached-d");
    assert(fifth>fourth && obs_gltf_async_poll(&owner,fifth)==OBS_GLTF_ASYNC_PREPARING);
    assert(step_until(&owner,fourth,OBS_GLTF_ASYNC_CANCELLED)==OBS_GLTF_ASYNC_CANCELLED);
    assert(scene.meshes.count==2);
    assert(step_until(&owner,fifth,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    assert(obs_gltf_async_poll(&owner,fourth)==OBS_GLTF_ASYNC_CANCELLED);
    /* Request cancellation after accepted submission cannot release CPU or
     * GPU owners until completion is observed. */
    assert(obs_gltf_async_cancel(&owner,fifth));
    assert(obs_gltf_async_poll(&owner,fifth)==OBS_GLTF_ASYNC_UPLOADING);
    fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,fifth)==OBS_GLTF_ASYNC_CANCELLED);
    assert(scene.meshes.count==2);
    uint64_t cold = obs_gltf_async_request(&owner,"cold-static");
    assert(cold);
    assert(step_until(&owner,cold,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    assert(scene.meshes.count==2 && scene.gltf_instance_count==1);
    fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,cold)==OBS_GLTF_ASYNC_PUBLISHED);
    assert(scene.meshes.count==3 && scene.gltf_instance_count==2);
    /* Multi-primitive cold candidate: all mesh offsets must be patched once
     * at GPU completion, with no premature publication or lease duplication. */
    uint64_t hierarchy = obs_gltf_async_request(&owner, "cold-hierarchy");
    assert(hierarchy);
    assert(step_until(&owner,hierarchy,OBS_GLTF_ASYNC_UPLOADING)==OBS_GLTF_ASYNC_UPLOADING);
    assert(scene.meshes.count==3 && scene.gltf_instance_count==2);
    fake_gpu_ready=true;
    assert(obs_gltf_async_poll(&owner,hierarchy)==OBS_GLTF_ASYNC_PUBLISHED);
    assert(scene.meshes.count==6 && scene.gltf_instance_count==3);
    assert(scene.gltf_instances[2].mesh_count == 3);
    assert(scene.gltf_instances[2].texture_lease.count == 1);
    for (uint32_t i=0;i<3;++i) {
        Mesh *mesh=&scene.meshes.items[3+i];
        assert(mesh->megaBaseVertex == 10 + 3*i);
        assert(mesh->megaBaseIndex == 20 + 3*i);
        assert(mesh->megaBaseMeshlet == 30 + i);
        assert(mesh->megaBaseMeshletVertex == 40 + 3*i);
        assert(mesh->megaBaseMeshletTriangle == 50 + 4*i);
    }
    assert(obs_gltf_async_close(&owner));
    for(size_t i=0;i<scene.meshes.count;i++) free(scene.meshes.items[i].name);
    free(scene.meshes.items);free(scene.meshes.draw_indices);
    for(size_t i=0;i<scene.gltf_instance_count;i++) {
        obs_cache_texture_lease_release(&scene.gltf_instances[i].texture_lease);
        OmdlSceneGraph *g = scene.gltf_instances[i].gltf_data;
        if (g) free(g->nodes);
        free(g);
    }
    free(scene.gltf_instances);
    assert(fake_acquires==fake_releases);
    assert(fake_dirty==3);
    puts("async production controller: worker, fence, textures, commit, cancel, failure OK");
    return 0;
}

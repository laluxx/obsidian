/// obj — assets subsystem
#include "renderer.h"
#include "scene.h"
#include "vulkan_setup.h"
#include "context.h"
#include "obj.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TINYOBJ_LOADER_C_IMPLEMENTATION
#include "tinyobj_loader_c.h"

static void file_reader(void *ctx, const char *filename, int is_mtl,
                        const char *obj_filename, char **out_buf,
                        size_t *out_len) {
    (void)ctx;
    (void)is_mtl;
    (void)obj_filename;
    FILE *file = fopen(filename, "rb");
    if (!file) {
        *out_buf = NULL;
        *out_len = 0;
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        *out_buf = NULL;
        *out_len = 0;
        return;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0 ||
        (uintmax_t)length >= SIZE_MAX) {
        fclose(file);
        *out_buf = NULL;
        *out_len = 0;
        return;
    }
    char *buffer = malloc((size_t)length + 1);
    if (!buffer || fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        *out_buf = NULL;
        *out_len = 0;
        return;
    }
    buffer[length] = '\0';
    *out_buf = buffer;
    *out_len = (size_t)length;
    fclose(file);
}

static bool parse_obj_vertices(const char *path, const vec4 color,
                               Vertex **out_vertices, uint32_t *out_count) {
    tinyobj_attrib_t attrib = {0};
    tinyobj_shape_t *shapes = NULL;
    size_t shape_count = 0;
    tinyobj_material_t *materials = NULL;
    size_t material_count = 0;
    Vertex *vertices = NULL;
    bool success = false;
    int result = tinyobj_parse_obj(&attrib, &shapes, &shape_count,
                                   &materials, &material_count, path,
                                   file_reader, NULL,
                                   TINYOBJ_FLAG_TRIANGULATE);
    if (result != TINYOBJ_SUCCESS) {
        fprintf(stderr, "[OBJ] Failed to parse '%s' (error %d)\n", path,
                result);
        goto done;
    }

    size_t vertex_count = 0;
    for (size_t i = 0; i < shape_count; ++i) {
        if (shapes[i].length > (SIZE_MAX - vertex_count) / 3) goto done;
        vertex_count += shapes[i].length * 3;
    }
    if (!vertex_count || vertex_count > UINT32_MAX ||
        vertex_count > SIZE_MAX / sizeof(*vertices))
        goto done;
    vertices = malloc(vertex_count * sizeof(*vertices));
    if (!vertices) goto done;

    size_t out = 0;
    for (size_t s = 0; s < shape_count; ++s) {
        const tinyobj_shape_t *shape = &shapes[s];
        for (size_t f = shape->face_offset;
             f < shape->face_offset + shape->length; ++f) {
            vec3 positions[3], normal;
            for (size_t v = 0; v < 3; ++v) {
                tinyobj_vertex_index_t index = attrib.faces[3 * f + v];
                if (index.v_idx < 0 ||
                    (size_t)index.v_idx >= attrib.num_vertices)
                    goto done;
                glm_vec3_copy(&attrib.vertices[3 * index.v_idx], positions[v]);
            }
            vec3 edge1, edge2;
            glm_vec3_sub(positions[1], positions[0], edge1);
            glm_vec3_sub(positions[2], positions[0], edge2);
            glm_vec3_cross(edge1, edge2, normal);
            if (glm_vec3_norm(normal) > 0.0001f)
                glm_vec3_normalize(normal);
            else
                glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, normal);
            for (size_t v = 0; v < 3; ++v) {
                Vertex *vertex = &vertices[out++];
                glm_vec3_copy(positions[v], vertex->pos);
                glm_vec4_copy((float *)color, vertex->color);
                glm_vec3_copy(normal, vertex->normal);
            }
        }
    }
    *out_vertices = vertices;
    *out_count = (uint32_t)vertex_count;
    vertices = NULL;
    success = true;

done:
    free(vertices);
    tinyobj_attrib_free(&attrib);
    tinyobj_shapes_free(shapes, shape_count);
    tinyobj_materials_free(materials, material_count);
    return success;
}

static Mesh upload_obj(Scene *target, const char *path, char *name,
                       const vec4 color, Vertex *vertices, uint32_t count) {
    Mesh mesh = {0};
    mesh.name = name;
    mesh.megaBaseVertex = UINT32_MAX;
    mesh.megaBaseIndex = UINT32_MAX;
    mesh.dynamicBaseVertex = UINT32_MAX;
    mesh.textureIndex = -1;
    mesh.normalMapIndex = -1;
    mesh.metallicRoughIndex = -1;
    mesh.aoIndex = -1;
    mesh.emissiveIndex = -1;
    mesh.transmissionIndex = -1;
    mesh.thicknessIndex = -1;
    glm_mat4_identity(mesh.model);
    glm_mat4_identity(mesh.local_transform);
    glm_vec4_copy((float *)color, mesh.baseColorFactor);
    mesh.metallicFactor = 0.0f;
    mesh.roughnessFactor = 1.0f;
    mesh.visible = true;
    if (!target || !name || !vertices || !count ||
        target->meshes.count >= INT32_MAX ||
        !meshes_reserve(&target->meshes, target->meshes.count + 1))
        return mesh;
    mesh.megaBaseVertex = megaBufferAllocate(&context, vertices, count);
    if (mesh.megaBaseVertex == UINT32_MAX) return mesh;
    mesh.vertexCount = count;
    mesh.indexCount = 0;
    glm_vec3_copy(vertices[0].pos, mesh.aabbMin);
    glm_vec3_copy(vertices[0].pos, mesh.aabbMax);
    for (uint32_t i = 1; i < count; ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            if (vertices[i].pos[axis] < mesh.aabbMin[axis])
                mesh.aabbMin[axis] = vertices[i].pos[axis];
            if (vertices[i].pos[axis] > mesh.aabbMax[axis])
                mesh.aabbMax[axis] = vertices[i].pos[axis];
        }
    }
    int32_t mesh_index = (int32_t)target->meshes.count;
    if (!meshes_append_prepared(&target->meshes, &mesh, 1))
        return mesh;
    int32_t node = scene_tree_add_node(&target->tree, name, 0, mesh_index);
    if (node >= 0) {
        target->meshes.items[mesh_index].node = (void *)(uintptr_t)node;
        scene_tree_add_node(&target->tree, "Material", node, -1);
    }
    if (target != scene) scene_bump_mesh_revision(target);
    if (target == scene) {
        markMeshesSSBODirty(&context);
    }
    printf("Loaded mesh '%s': %u vertices, %u triangles\n", path, count,
           count / 3);
    return mesh;
}

Mesh load_obj(const char *path, char *name, vec4 color) {
    Mesh empty = {0};
    Vertex *vertices = NULL;
    uint32_t count = 0;
    if (!path || !name || !scene ||
        !parse_obj_vertices(path, color, &vertices, &count)) {
        return empty;
    }
    char *owned_name = strdup(name);
    if (!owned_name) {
        free(vertices);
        return empty;
    }
    Mesh mesh = upload_obj(scene, path, owned_name, color, vertices, count);
    free(vertices);
    if (mesh.megaBaseVertex == UINT32_MAX) free(mesh.name);
    return mesh;
}

struct ObsObjAsync {
    pthread_t worker;
    pthread_mutex_t mutex;
    bool worker_running;
    bool worker_done;
    bool worker_ok;
    Scene *target;
    char *path;
    char *name;
    vec4 color;
    Vertex *vertices;
    uint32_t vertex_count;
};

static void *obj_worker(void *opaque) {
    ObsObjAsync *owner = opaque;
    Vertex *vertices = NULL;
    uint32_t count = 0;
    bool ok = parse_obj_vertices(owner->path, owner->color, &vertices, &count);
    pthread_mutex_lock(&owner->mutex);
    owner->vertices = vertices;
    owner->vertex_count = count;
    owner->worker_ok = ok;
    owner->worker_done = true;
    pthread_mutex_unlock(&owner->mutex);
    return NULL;
}

ObsObjAsync *obs_obj_async_create(void) {
    ObsObjAsync *owner = calloc(1, sizeof(*owner));
    if (!owner) return NULL;
    if (pthread_mutex_init(&owner->mutex, NULL) != 0) {
        free(owner);
        return NULL;
    }
    return owner;
}

bool obs_obj_async_request(ObsObjAsync *owner, Scene *target,
                           const char *path, const vec4 color) {
    if (!owner || !target || !path || !*path) return false;
    pthread_mutex_lock(&owner->mutex);
    if (owner->worker_running || owner->worker_done) {
        pthread_mutex_unlock(&owner->mutex);
        return false;
    }
    owner->path = strdup(path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    owner->name = strdup(base);
    if (!owner->path || !owner->name) {
        free(owner->path);
        free(owner->name);
        owner->path = owner->name = NULL;
        pthread_mutex_unlock(&owner->mutex);
        return false;
    }
    char *extension = strrchr(owner->name, '.');
    if (extension) *extension = '\0';
    owner->target = target;
    glm_vec4_copy((float *)color, owner->color);
    owner->worker_done = false;
    owner->worker_ok = false;
    owner->worker_running = true;
    if (pthread_create(&owner->worker, NULL, obj_worker, owner) != 0) {
        owner->worker_running = false;
        free(owner->path);
        free(owner->name);
        owner->path = owner->name = NULL;
        pthread_mutex_unlock(&owner->mutex);
        return false;
    }
    pthread_mutex_unlock(&owner->mutex);
    return true;
}

ObsObjAsyncStatus obs_obj_async_poll(ObsObjAsync *owner, int32_t *mesh_index) {
    if (!owner) return OBS_OBJ_ASYNC_FAILED;
    pthread_mutex_lock(&owner->mutex);
    if (!owner->worker_running || !owner->worker_done) {
        pthread_mutex_unlock(&owner->mutex);
        return OBS_OBJ_ASYNC_PENDING;
    }
    pthread_mutex_unlock(&owner->mutex);
    pthread_join(owner->worker, NULL);

    pthread_mutex_lock(&owner->mutex);
    owner->worker_running = false;
    Vertex *vertices = owner->vertices;
    uint32_t count = owner->vertex_count;
    bool ok = owner->worker_ok;
    Scene *target = owner->target;
    char *path = owner->path;
    char *name = owner->name;
    owner->vertices = NULL;
    owner->vertex_count = 0;
    owner->target = NULL;
    owner->path = owner->name = NULL;
    owner->worker_done = false;
    pthread_mutex_unlock(&owner->mutex);

    if (!ok) {
        free(vertices);
        free(path);
        free(name);
        return OBS_OBJ_ASYNC_FAILED;
    }
    Mesh mesh = upload_obj(target, path, name, owner->color, vertices, count);
    free(vertices);
    free(path);
    if (mesh.megaBaseVertex == UINT32_MAX) {
        free(mesh.name);
        return OBS_OBJ_ASYNC_FAILED;
    }
    if (mesh_index) *mesh_index = (int32_t)(target->meshes.count - 1);
    return OBS_OBJ_ASYNC_PUBLISHED;
}

void obs_obj_async_destroy(ObsObjAsync *owner) {
    if (!owner) return;
    if (owner->worker_running) pthread_join(owner->worker, NULL);
    free(owner->vertices);
    free(owner->path);
    free(owner->name);
    pthread_mutex_destroy(&owner->mutex);
    free(owner);
}

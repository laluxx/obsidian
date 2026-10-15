#define _POSIX_C_SOURCE 200809L
/// gltf_source_plan — validated, engine-independent glTF source inventory
#include "gltf_source_plan.h"
#include "cgltf.h"
#include <stdio.h>
#include <sys/stat.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/// Bounded arithmetic and source signature
#define OBS_GLTF_MAX_SOURCE_BYTES (128u * 1024u * 1024u)
#define OBS_GLTF_MAX_DECODED_BYTES (512u * 1024u * 1024u)
#define OBS_GLTF_MAX_NODES 100000u
#define OBS_GLTF_MAX_PRIMITIVES 100000u
#define OBS_GLTF_MAX_VERTICES 100000000u
#define OBS_GLTF_MAX_INDICES 300000000u

static bool add_u64(uint64_t *sum, uint64_t n, uint64_t max) {
    if (n > max || *sum > max - n) return false;
    *sum += n;
    return true;
}
static uint64_t digest_bytes(uint64_t h, const void *bytes, size_t count) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < count; ++i) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}
static uint64_t digest_integer(uint64_t h, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char b = (unsigned char)(value >> (8 * i));
        h = digest_bytes(h, &b, 1);
    }
    return h;
}

/// Source document identity, including materials and node properties
static bool digest_document(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char chunk[16384];
    uint64_t digest = UINT64_C(14695981039346656037);
    size_t total = 0, n;
    bool ok = true;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) != 0) {
        if (n > OBS_GLTF_MAX_SOURCE_BYTES - total) { ok = false; break; }
        total += n;
        digest = digest_bytes(digest, chunk, n);
    }
    if (ferror(f)) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok || !total) return false;
    *out = digest;
    return true;
}

/// External image identity
/* The glTF document + buffer digest alone is insufficient for texture changes:
 * a URI may refer to an image that mutates without the .gltf file changing.
 * Hash externally referenced image bytes under the same resource-size budget.
 * This is a source *observation*, NOT an atomic multi-file snapshot or an
 * ownership transfer to the cooker/renderer. */
static ObsGltfPlanError digest_image(const char *source, const char *uri,
                                      uint64_t *hash, uint64_t *budget) {
    if (!uri || !*uri) return OBS_GLTF_PLAN_INVALID;
    if (strncmp(uri, "data:", 5) == 0) return OBS_GLTF_PLAN_OK;
    if (strstr(uri, "://")) return OBS_GLTF_PLAN_UNSUPPORTED;
    size_t base_len = strlen(source), uri_len = strlen(uri);
    if (base_len > 4096 || uri_len > 4096 || uri_len > SIZE_MAX - base_len - 1)
        return OBS_GLTF_PLAN_LIMIT;
    char *path = malloc(base_len + uri_len + 1);
    if (!path) return OBS_GLTF_PLAN_NOMEM;
    /* Match cgltf's base-directory URI resolution (including percent decode). */
    const char *sep = strrchr(source, '/');
    const char *back = strrchr(source, '\\');
    if (!sep || (back && back > sep)) sep = back;
    size_t prefix = sep ? (size_t)(sep - source + 1) : 0;
    memcpy(path, source, prefix);
    memcpy(path + prefix, uri, uri_len + 1);
    size_t decoded = cgltf_decode_uri(path + prefix);
    if (decoded != strlen(path + prefix)) {
        free(path); /* Reject percent-encoded NUL/truncated paths. */
        return OBS_GLTF_PLAN_INVALID;
    }
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return OBS_GLTF_PLAN_IO;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0) {
        fclose(f);
        return OBS_GLTF_PLAN_IO;
    }
    if ((uint64_t)st.st_size > OBS_GLTF_MAX_DECODED_BYTES - *budget) {
        fclose(f);
        return OBS_GLTF_PLAN_LIMIT;
    }
    uint8_t bytes[16384];
    ObsGltfPlanError result = OBS_GLTF_PLAN_OK;
    uint64_t file_bytes = 0;
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), f)) != 0) {
        if (!add_u64(budget, count, OBS_GLTF_MAX_DECODED_BYTES)) {
            result = OBS_GLTF_PLAN_LIMIT;
            break;
        }
        file_bytes += count;
        *hash = digest_bytes(*hash, bytes, count);
    }
    if (ferror(f)) result = OBS_GLTF_PLAN_IO;
    if (fclose(f) != 0) result = OBS_GLTF_PLAN_IO;
    if (result == OBS_GLTF_PLAN_OK) *hash = digest_integer(*hash, file_bytes);
    return result;
}

/// Pure metadata and index verification
static ObsGltfPlanError inspect(const cgltf_data *data,
                                ObsGltfSourcePlan *p) {
    if (!data || data->nodes_count > OBS_GLTF_MAX_NODES ||
        data->meshes_count > OBS_GLTF_MAX_PRIMITIVES ||
        data->images_count > UINT32_MAX ||
        data->skins_count > UINT32_MAX ||
        data->animations_count > UINT32_MAX)
        return OBS_GLTF_PLAN_LIMIT;
    p->nodes = (uint32_t)data->nodes_count;
    p->meshes = (uint32_t)data->meshes_count;
    p->images = (uint32_t)data->images_count;
    p->skins = (uint32_t)data->skins_count;
    p->animations = (uint32_t)data->animations_count;
    p->has_skinning = data->skins_count != 0;

    for (size_t mesh = 0; mesh < data->meshes_count; ++mesh) {
        const cgltf_mesh *m = &data->meshes[mesh];
        if (m->primitives_count > OBS_GLTF_MAX_PRIMITIVES - p->primitives)
            return OBS_GLTF_PLAN_LIMIT;
        p->primitives += (uint32_t)m->primitives_count;
        for (size_t j = 0; j < m->primitives_count; ++j) {
            const cgltf_primitive *prim = &m->primitives[j];
            if (prim->type != cgltf_primitive_type_triangles)
                return OBS_GLTF_PLAN_UNSUPPORTED;
            const cgltf_accessor *pos = NULL;
            for (size_t k = 0; k < prim->attributes_count; ++k)
                if (prim->attributes[k].type == cgltf_attribute_type_position)
                    pos = prim->attributes[k].data;
            if (!pos || pos->type != cgltf_type_vec3 || !pos->count ||
                pos->count > OBS_GLTF_MAX_VERTICES)
                return OBS_GLTF_PLAN_INVALID;
            size_t count = prim->indices ? prim->indices->count : pos->count;
            if (count < 3 || count % 3 || count > OBS_GLTF_MAX_INDICES)
                return OBS_GLTF_PLAN_INVALID;
            if (!add_u64(&p->vertices, pos->count, OBS_GLTF_MAX_VERTICES) ||
                !add_u64(&p->indices, count, OBS_GLTF_MAX_INDICES) ||
                !add_u64(&p->triangles, count / 3, OBS_GLTF_MAX_INDICES / 3))
                return OBS_GLTF_PLAN_LIMIT;
            if (prim->indices) {
                for (size_t k = 0; k < count; ++k)
                    if (cgltf_accessor_read_index(prim->indices, k) >= pos->count)
                        return OBS_GLTF_PLAN_INVALID;
            }
            if (prim->targets_count) p->has_morph_targets = true;
        }
    }
    if (!p->primitives) return OBS_GLTF_PLAN_UNSUPPORTED;
    return OBS_GLTF_PLAN_OK;
}

/// Worker-safe source preparation
ObsGltfPlanError obs_gltf_source_plan(const char *path,
                                      ObsGltfSourcePlan *out) {
    if (!out) return OBS_GLTF_PLAN_INVALID;
    *out = (ObsGltfSourcePlan){0};
    if (!path || !*path || strnlen(path, 4096) == 4096)
        return OBS_GLTF_PLAN_INVALID;
    struct stat file_stat;
    if (stat(path, &file_stat) != 0) return OBS_GLTF_PLAN_IO;
    if (file_stat.st_size < 0 ||
        (uint64_t)file_stat.st_size > OBS_GLTF_MAX_SOURCE_BYTES)
        return OBS_GLTF_PLAN_LIMIT;
    uint64_t document_before = 0, document_after = 0;
    if (!digest_document(path, &document_before)) return OBS_GLTF_PLAN_IO;
    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result parsed = cgltf_parse_file(&options, path, &data);
    if (parsed != cgltf_result_success)
        return parsed == cgltf_result_file_not_found ||
               parsed == cgltf_result_io_error ? OBS_GLTF_PLAN_IO :
                                                OBS_GLTF_PLAN_INVALID;
    ObsGltfPlanError result = OBS_GLTF_PLAN_INVALID;
    uint64_t total_buffer_bytes = 0;
    for (size_t i = 0; i < data->buffers_count; ++i) {
        if (!add_u64(&total_buffer_bytes, data->buffers[i].size,
                     OBS_GLTF_MAX_DECODED_BYTES)) {
            result = OBS_GLTF_PLAN_LIMIT;
            goto cleanup;
        }
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success ||
        cgltf_validate(data) != cgltf_result_success)
        goto cleanup;
    ObsGltfSourcePlan plan = {0};
    result = inspect(data, &plan);
    if (result != OBS_GLTF_PLAN_OK) goto cleanup;
    /* Fingerprint content in each decoded buffer, not just paths or mtimes.
     * This fingerprints the inventory, not the complete glTF source document. */
    uint64_t hash = document_before;
    hash = digest_integer(hash, data->buffers_count);
    for (size_t i = 0; i < data->buffers_count; ++i) {
        const cgltf_buffer *buffer = &data->buffers[i];
        if (buffer->size && !buffer->data) {
            result = OBS_GLTF_PLAN_INVALID;
            goto cleanup;
        }
        hash = digest_integer(hash, buffer->size);
        hash = digest_bytes(hash, buffer->data, buffer->size);
    }
    /* Texture identity is part of a cache-miss plan: independent external
     * image content changes must invalidate its observed fingerprint. */
    hash = digest_integer(hash, data->images_count);
    for (size_t i = 0; i < data->images_count; ++i) {
        const cgltf_image *img = &data->images[i];
        hash = digest_integer(hash, i);
        if (!img->uri) continue; /* Embedded bufferView is already digested. */
        result = digest_image(path, img->uri, &hash, &total_buffer_bytes);
        if (result != OBS_GLTF_PLAN_OK) goto cleanup;
    }
    if (!digest_document(path, &document_after) ||
        document_after != document_before) {
        result = OBS_GLTF_PLAN_INVALID;
        goto cleanup;
    }
    plan.source_fingerprint = hash;
    *out = plan;
cleanup:
    cgltf_free(data);
    return result;
}

const char *obs_gltf_plan_error_name(ObsGltfPlanError error) {
    switch (error) {
    case OBS_GLTF_PLAN_OK: return "ok";
    case OBS_GLTF_PLAN_IO: return "file-or-parse";
    case OBS_GLTF_PLAN_INVALID: return "invalid";
    case OBS_GLTF_PLAN_UNSUPPORTED: return "unsupported";
    case OBS_GLTF_PLAN_LIMIT: return "limit";
    case OBS_GLTF_PLAN_NOMEM: return "allocation";
    }
    return "unknown";
}

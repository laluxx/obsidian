#include "renderer.h"
#include "scene.h"
#include "context.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TINYOBJ_LOADER_C_IMPLEMENTATION
#include "tinyobj_loader_c.h"

static void file_reader(void* ctx, const char* filename, int is_mtl, const char* obj_filename, char** out_buf, size_t* out_len) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        *out_buf = NULL;
        *out_len = 0;
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out_buf = malloc(len + 1);
    fread(*out_buf, 1, len, f);
    (*out_buf)[len] = 0;
    *out_len = (size_t)len;
    fclose(f);
}

Mesh load_obj(const char* path, char* name, vec4 color){
    Mesh mesh = {0};
    mesh.name = name;
    glm_mat4_identity(mesh.model);
    tinyobj_attrib_t attrib;
    tinyobj_shape_t* shapes = NULL;
    size_t num_shapes = 0;
    tinyobj_material_t* materials = NULL;
    size_t num_materials = 0;

    int ret = tinyobj_parse_obj(
        &attrib, &shapes, &num_shapes,
        &materials, &num_materials,
        path,
        file_reader,
        NULL,
        TINYOBJ_FLAG_TRIANGULATE
    );

    if (ret != TINYOBJ_SUCCESS) {
        fprintf(stderr, "[OBJ] Failed to load '%s': tinyobj_parse_obj error code %d\n", path, ret);
        goto cleanup;
    }

    // Count total triangles
    size_t vertexCount = 0;
    for (size_t s = 0; s < num_shapes; ++s) {
        vertexCount += shapes[s].length * 3; // 3 verts per face (triangulated)
    }

    Vertex* vertices = malloc(sizeof(Vertex) * vertexCount);
    if (!vertices) goto cleanup;

    size_t vert = 0;
    for (size_t s = 0; s < num_shapes; ++s) {
        const tinyobj_shape_t* shape = &shapes[s];
        size_t face_offset = shape->face_offset;
        size_t face_end = face_offset + shape->length;
        
        for (size_t f = face_offset; f < face_end; ++f) {
            // Get the three vertices of this triangle
            vec3 v0, v1, v2;
            vec3 normal;
            
            for (size_t v = 0; v < 3; ++v) {
                tinyobj_vertex_index_t idx = attrib.faces[3 * f + v];
                float* vp = &attrib.vertices[3 * idx.v_idx];
                
                if (v == 0) {
                    glm_vec3_copy(vp, v0);
                } else if (v == 1) {
                    glm_vec3_copy(vp, v1);
                } else {
                    glm_vec3_copy(vp, v2);
                }
            }
            
            // Calculate face normal using cross product
            vec3 edge1, edge2;
            glm_vec3_sub(v1, v0, edge1);
            glm_vec3_sub(v2, v0, edge2);
            glm_vec3_cross(edge1, edge2, normal);
            
            // Check if normal is valid (non-zero length)
            float norm_length = glm_vec3_norm(normal);
            if (norm_length > 0.0001f) {
                glm_vec3_normalize(normal);
            } else {
                // Degenerate triangle, use default normal
                glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, normal);
            }
            
            // Now assign position, color, and normal to each vertex
            for (size_t v = 0; v < 3; ++v) {
                tinyobj_vertex_index_t idx = attrib.faces[3 * f + v];
                float* vp = &attrib.vertices[3 * idx.v_idx];
                
                glm_vec3_copy(vp, vertices[vert].pos);
                glm_vec4_copy(color, vertices[vert].color);
                glm_vec3_copy(normal, vertices[vert].normal);
                
                vert++;
            }
        }
    }

    // Create Vulkan buffer (HOST_VISIBLE for simplicity)
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(Vertex) * vertexCount,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (vkCreateBuffer(context.device, &bufferInfo, NULL, &mesh.vertexBuffer) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create vertex buffer\n");
        free(vertices);
        goto cleanup;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context.device, mesh.vertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(context.physicalDevice,
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    if (vkAllocateMemory(context.device, &allocInfo, NULL, &mesh.vertexBufferMemory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate vertex buffer memory\n");
        vkDestroyBuffer(context.device, mesh.vertexBuffer, NULL);
        free(vertices);
        goto cleanup;
    }
    vkBindBufferMemory(context.device, mesh.vertexBuffer, mesh.vertexBufferMemory, 0);

    // Copy vertex data to buffer
    void* data;
    vkMapMemory(context.device, mesh.vertexBufferMemory, 0, sizeof(Vertex) * vertexCount, 0, &data);
    memcpy(data, vertices, sizeof(Vertex) * vertexCount);
    vkUnmapMemory(context.device, mesh.vertexBufferMemory);

    mesh.vertexCount = vertexCount;

    printf("Loaded mesh '%s': %zu vertices, %zu triangles, named: %s\n", path, vertexCount, vertexCount / 3, mesh.name);

    free(vertices);

cleanup:
    tinyobj_attrib_free(&attrib);
    tinyobj_shapes_free(shapes, num_shapes);
    tinyobj_materials_free(materials, num_materials);

    meshes_add(&scene.meshes, mesh);

    return mesh;
}

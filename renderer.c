#include <stdlib.h>
#include <string.h>
#include "renderer.h"
#include "context.h"
#include "common.h"
#include "scene.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// --- Texture Pool Management ---
static Texture2D texturePool[MAX_TEXTURES];
static uint32_t textureCount = 0;

void texture_pool_init() {
    textureCount = 0;
    memset(texturePool, 0, sizeof(texturePool));
}

void texture_pool_cleanup(VulkanContext* context) {
    for (uint32_t i = 0; i < textureCount; i++) {
        if (texturePool[i].loaded) {
            destroy_texture(context, &texturePool[i]);
        }
    }
    textureCount = 0;
}

int32_t texture_pool_add(VulkanContext* context, const char* filename) {
    if (textureCount >= MAX_TEXTURES) {
        fprintf(stderr, "Texture pool full! Cannot load %s\n", filename);
        return -1;
    }
    
    printf("Loading texture %u: %s\n", textureCount, filename);
    
    if (load_texture(context, filename, &texturePool[textureCount])) {
        texturePool[textureCount].loaded = true;
        printf("  -> Successfully loaded as texture #%u\n", textureCount);
        return textureCount++;
    }
    
    printf("  -> Failed to load\n");
    return -1;
}

Texture2D* texture_pool_get(int32_t index) {
    if (index < 0 || index >= (int32_t)textureCount) {
        return NULL;
    }
    return &texturePool[index];
}

// --- 3D Renderer ---
static Vertex vertices[MAX_VERTICES];
static uint32_t vertex_count = 0;

static VkDevice device;
static VkPhysicalDevice physicalDevice;
static VkCommandPool commandPool;
static VkQueue graphicsQueue;
 
static VkBuffer vertexBuffer;
static VkDeviceMemory vertexBufferMemory;

PushConstants pushConstants;

void renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(vertices),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufferInfo, NULL, &vertexBuffer);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, vertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    vkAllocateMemory(device, &allocInfo, NULL, &vertexBufferMemory);
    vkBindBufferMemory(device, vertexBuffer, vertexBufferMemory, 0);
}

void vertex_with_normal(vec3 pos, Color color, vec3 normal) {
    if (vertex_count >= MAX_VERTICES) return;
    glm_vec3_copy(pos, vertices[vertex_count].pos);
    
    // Direct assignment instead of glm_vec4_copy
    vertices[vertex_count].color[0] = color.r;
    vertices[vertex_count].color[1] = color.g;
    vertices[vertex_count].color[2] = color.b;
    vertices[vertex_count].color[3] = color.a;
    
    glm_vec3_copy(normal, vertices[vertex_count].normal);
    glm_vec2_copy((vec2){0.0f, 0.0f}, vertices[vertex_count].texCoord);
    vertex_count++;
}

void vertex(vec3 pos, vec4 color) {
    if (vertex_count >= MAX_VERTICES) return;
    glm_vec3_copy(pos, vertices[vertex_count].pos);
    glm_vec4_copy(color, vertices[vertex_count].color);
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, vertices[vertex_count].normal); // Default normal
    glm_vec2_copy((vec2){0.0f, 0.0f}, vertices[vertex_count].texCoord); // Default tex coords
    vertex_count++;
}

void triangle(vec3 a, vec3 b, vec3 c, Color color) {
    vec3 edge1, edge2, normal;
    glm_vec3_sub(b, a, edge1);
    glm_vec3_sub(c, a, edge2);
    glm_vec3_cross(edge1, edge2, normal);
    glm_vec3_normalize(normal);
    
    vertex_with_normal(a, color, normal);
    vertex_with_normal(b, color, normal);
    vertex_with_normal(c, color, normal);
}

void texturedPlane(vec3 origin, vec2 size, Texture2D* texture, Color tint, float tileX, float tileZ) {
    if (vertex_count_3D_textured + 6 >= MAX_VERTICES || !texture || !texture->loaded) {
        return;
    }

    float w = size[0] / 2.0f;
    float h = size[1] / 2.0f;
    float x = origin[0], y = origin[1], z = origin[2];

    Vertex plane[6] = {
        // First triangle - use separate tileX and tileZ for UV coordinates
        {{x-w, y, z-h}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {0.0f, tileZ}, 0},
        {{x+w, y, z-h}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {tileX, tileZ}, 0},
        {{x+w, y, z+h}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {tileX, 0.0f}, 0},
        
        // Second triangle
        {{x-w, y, z-h}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {0.0f, tileZ}, 0},
        {{x+w, y, z+h}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {tileX, 0.0f}, 0},
        {{x-w, y, z+h}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, 0}
    };

    // Batch management (same as before)
    int batchIndex = -1;
    if (texture3DBatchCount > 0 && texture3DBatches[texture3DBatchCount - 1].texture == texture) {
        batchIndex = texture3DBatchCount - 1;
    } else {
        if (texture3DBatchCount >= MAX_TEXTURES) return;
        batchIndex = texture3DBatchCount++;
        texture3DBatches[batchIndex].texture = texture;
        texture3DBatches[batchIndex].startVertex = vertex_count_3D_textured;
        texture3DBatches[batchIndex].vertexCount = 0;
    }

    memcpy(&vertices3D_textured[vertex_count_3D_textured], plane, sizeof(plane));
    vertex_count_3D_textured += 6;
    texture3DBatches[batchIndex].vertexCount += 6;
}

void plane(vec3 origin, vec2 size, Color color) {
    float w = size[0] / 2.0f;
    float h = size[1] / 2.0f;
    
    vec3 a, b, c, d;
    
    // Define the four corners of the plane
    glm_vec3_add(origin, (vec3){-w, 0.0f, -h}, a);
    glm_vec3_add(origin, (vec3){+w, 0.0f, -h}, b);
    glm_vec3_add(origin, (vec3){+w, 0.0f, +h}, c);
    glm_vec3_add(origin, (vec3){-w, 0.0f, +h}, d);
    
    // Normal pointing up (Y-axis)
    vec3 normal = {0.0f, 1.0f, 0.0f};
    
    // Create two triangles to form the plane
    // First triangle
    vertex_with_normal(a, color, normal);
    vertex_with_normal(b, color, normal);
    vertex_with_normal(c, color, normal);
    
    // Second triangle
    vertex_with_normal(a, color, normal);
    vertex_with_normal(c, color, normal);
    vertex_with_normal(d, color, normal);
}

//    h-------g
//   /|      /|
//  d-------c |
//  | |  .  | |  <- Origin
//  | e-----|-f
//  |/      |/
//  a-------b

void cube(vec3 origin, float size, Color color) {
    float s = size / 2.0f;
    vec3 a, b, c, d, e, f, g, h;

    glm_vec3_add(origin, (vec3){-s, -s, -s}, a);
    glm_vec3_add(origin, (vec3){+s, -s, -s}, b);
    glm_vec3_add(origin, (vec3){+s, +s, -s}, c);
    glm_vec3_add(origin, (vec3){-s, +s, -s}, d);
    glm_vec3_add(origin, (vec3){-s, -s, +s}, e);
    glm_vec3_add(origin, (vec3){+s, -s, +s}, f);
    glm_vec3_add(origin, (vec3){+s, +s, +s}, g);
    glm_vec3_add(origin, (vec3){-s, +s, +s}, h);

    vec3 n_front = {0.0f, 0.0f, -1.0f};
    vertex_with_normal(a, color, n_front);
    vertex_with_normal(b, color, n_front);
    vertex_with_normal(c, color, n_front);
    vertex_with_normal(a, color, n_front);
    vertex_with_normal(c, color, n_front);
    vertex_with_normal(d, color, n_front);

    vec3 n_back = {0.0f, 0.0f, 1.0f};
    vertex_with_normal(e, color, n_back);
    vertex_with_normal(h, color, n_back);
    vertex_with_normal(g, color, n_back);
    vertex_with_normal(e, color, n_back);
    vertex_with_normal(g, color, n_back);
    vertex_with_normal(f, color, n_back);

    vec3 n_left = {-1.0f, 0.0f, 0.0f};
    vertex_with_normal(a, color, n_left);
    vertex_with_normal(d, color, n_left);
    vertex_with_normal(h, color, n_left);
    vertex_with_normal(a, color, n_left);
    vertex_with_normal(h, color, n_left);
    vertex_with_normal(e, color, n_left);

    vec3 n_right = {1.0f, 0.0f, 0.0f};
    vertex_with_normal(b, color, n_right);
    vertex_with_normal(f, color, n_right);
    vertex_with_normal(g, color, n_right);
    vertex_with_normal(b, color, n_right);
    vertex_with_normal(g, color, n_right);
    vertex_with_normal(c, color, n_right);

    vec3 n_top = {0.0f, 1.0f, 0.0f};
    vertex_with_normal(d, color, n_top);
    vertex_with_normal(c, color, n_top);
    vertex_with_normal(g, color, n_top);
    vertex_with_normal(d, color, n_top);
    vertex_with_normal(g, color, n_top);
    vertex_with_normal(h, color, n_top);

    vec3 n_bottom = {0.0f, -1.0f, 0.0f};
    vertex_with_normal(a, color, n_bottom);
    vertex_with_normal(e, color, n_bottom);
    vertex_with_normal(f, color, n_bottom);
    vertex_with_normal(a, color, n_bottom);
    vertex_with_normal(f, color, n_bottom);
    vertex_with_normal(b, color, n_bottom);
}

void texturedCube(vec3 position, float size, Texture2D* texture, Color tint) {
    if (vertex_count_3D_textured + 36 >= MAX_VERTICES || !texture || !texture->loaded) {
        return;
    }

    float s = size / 2.0f;
    float x = position[0], y = position[1], z = position[2];

    // Define cube vertices with proper normals and texture coordinates
    // Each face is properly rotated to form a cube
    Vertex cube[36] = {
        // Front face (facing +Z)
        {{x-s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, 0},
        {{x+s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, 0},

        // Back face (facing -Z) - rotated 180 degrees
        {{x+s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, 0},
        {{x-s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, 0},
        {{x-s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, 0},
        {{x+s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, 0},
        {{x-s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, 0},
        {{x+s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, 0},

        // Right face (facing +X) - rotated 90 degrees around Y
        {{x+s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, 0},
        {{x+s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x+s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x+s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, 0},

        // Left face (facing -X) - rotated -90 degrees around Y
        {{x-s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x-s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, 0},
        {{x-s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x-s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, 0},

        // Top face (facing +Y) - rotated 90 degrees around X
        {{x-s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, 0},
        {{x+s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y+s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y+s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, 0},

        // Bottom face (facing -Y) - rotated -90 degrees around X
        {{x-s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, 0},
        {{x+s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y-s, z-s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, 0},
        {{x+s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, 0},
        {{x-s, y-s, z+s}, {tint.r, tint.g, tint.b, tint.a}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, 0}
    };

    // Find or create batch for this texture
    int batchIndex = -1;
    if (texture3DBatchCount > 0 && texture3DBatches[texture3DBatchCount - 1].texture == texture) {
        batchIndex = texture3DBatchCount - 1;
    } else {
        if (texture3DBatchCount >= MAX_TEXTURES) return;
        batchIndex = texture3DBatchCount++;
        texture3DBatches[batchIndex].texture = texture;
        texture3DBatches[batchIndex].startVertex = vertex_count_3D_textured;
        texture3DBatches[batchIndex].vertexCount = 0;
    }

    memcpy(&vertices3D_textured[vertex_count_3D_textured], cube, sizeof(cube));
    vertex_count_3D_textured += 36;
    texture3DBatches[batchIndex].vertexCount += 36;
}

void sphere(vec3 center, float radius, int latDiv, int longDiv, Color color) {
    for (int lat = 0; lat < latDiv; ++lat) {
        float theta1 = (float)lat / latDiv * GLM_PI;
        float theta2 = (float)(lat + 1) / latDiv * GLM_PI;

        for (int lon = 0; lon < longDiv; ++lon) {
            float phi1 = (float)lon / longDiv * 2.0f * GLM_PI;
            float phi2 = (float)(lon + 1) / longDiv * 2.0f * GLM_PI;

            vec3 v0 = {
                center[0] + radius * sinf(theta1) * cosf(phi1),
                center[1] + radius * cosf(theta1),
                center[2] + radius * sinf(theta1) * sinf(phi1)
            };
            vec3 v1 = {
                center[0] + radius * sinf(theta2) * cosf(phi1),
                center[1] + radius * cosf(theta2),
                center[2] + radius * sinf(theta2) * sinf(phi1)
            };
            vec3 v2 = {
                center[0] + radius * sinf(theta2) * cosf(phi2),
                center[1] + radius * cosf(theta2),
                center[2] + radius * sinf(theta2) * sinf(phi2)
            };
            vec3 v3 = {
                center[0] + radius * sinf(theta1) * cosf(phi2),
                center[1] + radius * cosf(theta1),
                center[2] + radius * sinf(theta1) * sinf(phi2)
            };

            vec3 n0, n1, n2, n3;
            glm_vec3_sub(v0, center, n0);
            glm_vec3_normalize(n0);
            glm_vec3_sub(v1, center, n1);
            glm_vec3_normalize(n1);
            glm_vec3_sub(v2, center, n2);
            glm_vec3_normalize(n2);
            glm_vec3_sub(v3, center, n3);
            glm_vec3_normalize(n3);

            vertex_with_normal(v0, color, n0);
            vertex_with_normal(v1, color, n1);
            vertex_with_normal(v2, color, n2);

            vertex_with_normal(v0, color, n0);
            vertex_with_normal(v2, color, n2);
            vertex_with_normal(v3, color, n3);
        }
    }
}

void renderer_upload() {
    void* data;
    vkMapMemory(device, vertexBufferMemory, 0, sizeof(vertices), 0, &data);
    memcpy(data, vertices, vertex_count * sizeof(Vertex));
    vkUnmapMemory(device, vertexBufferMemory);
}

void renderer_draw(VkCommandBuffer cmd) {
    glm_mat4_identity(pushConstants.model);
    
    vkCmdPushConstants(
        cmd,
        context.pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants
    );
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);
    vkCmdDraw(cmd, vertex_count, 1, 0, 0);
}

void renderer_clear() {
    vertex_count = 0;
}

// WITH TEXTURES
void mesh(VkCommandBuffer cmd, Mesh* mesh) {
    glm_mat4_copy(mesh->model, pushConstants.model);
    
    if (mesh->texture && mesh->texture->loaded) {
        // Use textured 3D pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipelineTextured3D);
        
        // Bind descriptor sets
        VkDescriptorSet descriptorSets[2] = {
            descriptorSet,              // Set 0: Camera UBO
            mesh->texture->descriptorSet // Set 1: Texture
        };
        
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            context.pipelineLayoutTextured3D,
            0, 2,
            descriptorSets,
            0, NULL
        );
        
        vkCmdPushConstants(
            cmd,
            context.pipelineLayoutTextured3D,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(PushConstants),
            &pushConstants
        );
    } else {
        // Use regular colored pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipeline);
        
        vkCmdPushConstants(
            cmd,
            context.pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(PushConstants),
            &pushConstants
        );
    }
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, offsets);
    vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
}


void mesh_destroy(VkDevice device, Mesh* mesh) {
    if (mesh->vertexBuffer) vkDestroyBuffer(device, mesh->vertexBuffer, NULL);
    if (mesh->vertexBufferMemory) vkFreeMemory(device, mesh->vertexBufferMemory, NULL);

    mesh->vertexBuffer = VK_NULL_HANDLE;
    mesh->vertexBufferMemory = VK_NULL_HANDLE;
    mesh->vertexCount = 0;
}

void meshes_init(Meshes* meshes) {
    meshes->items = NULL;
    meshes->count = 0;
    meshes->capacity = 0;
}

void meshes_add(Meshes* meshes, Mesh mesh) {
    if (meshes->count == meshes->capacity) {
        size_t new_capacity = meshes->capacity ? meshes->capacity * 2 : 4;
        meshes->items = realloc(meshes->items, new_capacity * sizeof(Mesh));
        meshes->capacity = new_capacity;
    }
    meshes->items[meshes->count++] = mesh;
}

void meshes_remove(Meshes* meshes, size_t index) {
    if (index >= meshes->count) return;
    for (size_t i = index; i < meshes->count - 1; ++i)
        meshes->items[i] = meshes->items[i + 1];
    meshes->count--;
}

void meshes_draw(VkCommandBuffer cmd, Meshes* meshes) {
    for (size_t i = 0; i < meshes->count; ++i) {
        mesh(cmd, &meshes->items[i]);
    }
}

void meshes_destroy(VkDevice device, Meshes* meshes) {
    for (size_t i = 0; i < meshes->count; ++i) {
        mesh_destroy(device, &meshes->items[i]);
    }
    free(meshes->items);
    meshes->items = NULL;
    meshes->count = 0;
    meshes->capacity = 0;
}

Mesh* get_mesh(const char* name) {
    if (!name) return NULL;
    
    for (size_t i = 0; i < scene.meshes.count; i++) {
        if (scene.meshes.items[i].name && 
            strcmp(scene.meshes.items[i].name, name) == 0) {
            return &scene.meshes.items[i];
        }
    }
    return NULL;
}

// --- 2D Renderer ---

// Batch structure for textured quads
typedef struct {
    Texture2D* texture;
    uint32_t startVertex;
    uint32_t vertexCount;
} TextureBatch;

static Vertex2D vertices2D[MAX_VERTICES];
static uint32_t vertexCount2D = 0;
static uint32_t coloredVertexCount = 0;

static TextureBatch textureBatches[MAX_TEXTURES];
static uint32_t textureBatchCount = 0;

void renderer2D_init() {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(vertices2D),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(context.device, &bufferInfo, NULL, &context.vertexBuffer2D);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context.device, context.vertexBuffer2D, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(context.physicalDevice, memRequirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    vkAllocateMemory(context.device, &allocInfo, NULL, &context.vertexBufferMemory2D);
    vkBindBufferMemory(context.device, context.vertexBuffer2D, context.vertexBufferMemory2D, 0);
}

void quad2D(vec2 position, vec2 size, Color color) {
    if (vertexCount2D + 6 > MAX_VERTICES) return;

    float x = position[0], y = position[1];
    float w = size[0], h = size[1];

    Vertex2D quad[6] = {
        {{x, y}, color, {0.0f, 0.0f}, 0},
        {{x + w, y}, color, {1.0f, 0.0f}, 0},
        {{x + w, y + h}, color, {1.0f, 1.0f}, 0},
        
        {{x, y}, color, {0.0f, 0.0f}, 0},
        {{x + w, y + h}, color, {1.0f, 1.0f}, 0},
        {{x, y + h}, color, {0.0f, 1.0f}, 0}
    };

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
    coloredVertexCount += 6;
}

void texture2D(vec2 position, vec2 size, Texture2D* texture, Color tint) {
    if (vertexCount2D + 6 > MAX_VERTICES || !texture || !texture->loaded) {
        if (!texture) printf("texture2D: NULL texture\n");
        else if (!texture->loaded) printf("texture2D: texture not loaded\n");
        return;
    }

    float x = position[0], y = position[1];
    float w = size[0], h = size[1];

    // Check if the LAST batch is for this texture (batching optimization)
    int batchIndex = -1;
    if (textureBatchCount > 0 && textureBatches[textureBatchCount - 1].texture == texture) {
        // Continue the existing batch
        batchIndex = textureBatchCount - 1;
    } else {
        // Create new batch for this texture
        if (textureBatchCount >= MAX_TEXTURES) {
            fprintf(stderr, "Too many texture batches!\n");
            return;
        }
        batchIndex = textureBatchCount++;
        textureBatches[batchIndex].texture = texture;
        textureBatches[batchIndex].startVertex = vertexCount2D;
        textureBatches[batchIndex].vertexCount = 0;
        /* printf("Created batch %d for texture %p at vertex %u\n", batchIndex, (void*)texture, vertexCount2D); */
    }

    Vertex2D quad[6] = {
        {{x, y}, tint, {0.0f, 1.0f}, 0},
        {{x + w, y}, tint, {1.0f, 1.0f}, 0},
        {{x + w, y + h}, tint, {1.0f, 0.0f}, 0},
        
        {{x, y}, tint, {0.0f, 1.0f}, 0},
        {{x + w, y + h}, tint, {1.0f, 0.0f}, 0},
        {{x, y + h}, tint, {0.0f, 0.0f}, 0}
    };

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
    textureBatches[batchIndex].vertexCount += 6;
}

void renderer2D_upload() {
    if (vertexCount2D == 0) return;
    
    void* data;
    vkMapMemory(context.device, context.vertexBufferMemory2D, 0, sizeof(vertices2D), 0, &data);
    memcpy(data, vertices2D, vertexCount2D * sizeof(Vertex2D));
    vkUnmapMemory(context.device, context.vertexBufferMemory2D);
}

void renderer2D_draw(VkCommandBuffer cmd) {
    if (vertexCount2D == 0) return;

    mat4 projection;
    glm_ortho(0.0f, (float)context.swapChainExtent.width,
              (float)context.swapChainExtent.height, 0.0f,
              -1.0f, 1.0f, projection);

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &context.vertexBuffer2D, offsets);

    // Draw colored content first
    if (coloredVertexCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipeline2D);
        
        vkCmdPushConstants(
            cmd,
            context.pipelineLayout2D,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(mat4),
            &projection
        );
        
        vkCmdDraw(cmd, coloredVertexCount, 1, 0, 0);
    }

    // Draw each texture batch
    if (textureBatchCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipelineTextured2D);
        
        vkCmdPushConstants(
            cmd,
            context.pipelineLayoutTextured2D,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(mat4),
            &projection
        );

        for (uint32_t i = 0; i < textureBatchCount; i++) {
            TextureBatch* batch = &textureBatches[i];
            
            if (batch->vertexCount == 0) continue; // Skip empty batches
            
            // Debug output
            /* printf("Drawing batch %u: texture=%p, start=%u, count=%u\n",  */
            /*        i, (void*)batch->texture, batch->startVertex, batch->vertexCount); */
            
            // Bind this texture's descriptor set
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                context.pipelineLayoutTextured2D,
                0, 1,
                &batch->texture->descriptorSet,
                0, NULL
            );
            
            // Draw this batch
            vkCmdDraw(cmd, batch->vertexCount, 1, batch->startVertex, 0);
        }
    }
}

void renderer2D_clear(void) {
    vertexCount2D = 0;
    coloredVertexCount = 0;
    textureBatchCount = 0;
}

// --- Texture Loading ---

bool load_texture(VulkanContext* context, const char* filename, Texture2D* texture) {
    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(filename, &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    
    if (!pixels) {
        fprintf(stderr, "Failed to load texture image: %s\n", filename);
        return false;
    }

    VkDeviceSize imageSize = texWidth * texHeight * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(context->device, &bufferInfo, NULL, &stagingBuffer) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create staging buffer for texture\n");
        stbi_image_free(pixels);
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context->device, stagingBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(context->physicalDevice, memRequirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    if (vkAllocateMemory(context->device, &allocInfo, NULL, &stagingBufferMemory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate staging memory for texture\n");
        vkDestroyBuffer(context->device, stagingBuffer, NULL);
        stbi_image_free(pixels);
        return false;
    }

    vkBindBufferMemory(context->device, stagingBuffer, stagingBufferMemory, 0);

    void* data;
    vkMapMemory(context->device, stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, imageSize);
    vkUnmapMemory(context->device, stagingBufferMemory);

    stbi_image_free(pixels);

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = texWidth,
        .extent.height = texHeight,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .samples = VK_SAMPLE_COUNT_1_BIT
    };

    if (vkCreateImage(context->device, &imageInfo, NULL, &texture->image) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create texture image\n");
        vkDestroyBuffer(context->device, stagingBuffer, NULL);
        vkFreeMemory(context->device, stagingBufferMemory, NULL);
        return false;
    }

    vkGetImageMemoryRequirements(context->device, texture->image, &memRequirements);

    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(context->physicalDevice, memRequirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(context->device, &allocInfo, NULL, &texture->memory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate texture memory\n");
        vkDestroyImage(context->device, texture->image, NULL);
        vkDestroyBuffer(context->device, stagingBuffer, NULL);
        vkFreeMemory(context->device, stagingBufferMemory, NULL);
        return false;
    }

    vkBindImageMemory(context->device, texture->image, texture->memory, 0);

    VkCommandBuffer commandBuffer = beginSingleTimeCommands(context->device, context->commandPool);
    
    transitionImageLayout(commandBuffer, texture->image, VK_FORMAT_R8G8B8A8_SRGB, 
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    copyBufferToImage(commandBuffer, stagingBuffer, texture->image, texWidth, texHeight);
    
    transitionImageLayout(commandBuffer, texture->image, VK_FORMAT_R8G8B8A8_SRGB,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    endSingleTimeCommands(context->device, context->commandPool, context->graphicsQueue, commandBuffer);

    vkDestroyBuffer(context->device, stagingBuffer, NULL);
    vkFreeMemory(context->device, stagingBufferMemory, NULL);

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    if (vkCreateImageView(context->device, &viewInfo, NULL, &texture->view) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create texture image view\n");
        vkDestroyImage(context->device, texture->image, NULL);
        vkFreeMemory(context->device, texture->memory, NULL);
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .mipLodBias = 0.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f
    };

    if (vkCreateSampler(context->device, &samplerInfo, NULL, &texture->sampler) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create texture sampler\n");
        vkDestroyImageView(context->device, texture->view, NULL);
        vkDestroyImage(context->device, texture->image, NULL);
        vkFreeMemory(context->device, texture->memory, NULL);
        return false;
    }

    // Allocate descriptor set for this texture
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = context->descriptorPool2D,
        .descriptorSetCount = 1,
        .pSetLayouts = &context->descriptorSetLayout2D
    };

    if (vkAllocateDescriptorSets(context->device, &descriptorAllocInfo, &texture->descriptorSet) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate descriptor set for texture\n");
        vkDestroySampler(context->device, texture->sampler, NULL);
        vkDestroyImageView(context->device, texture->view, NULL);
        vkDestroyImage(context->device, texture->image, NULL);
        vkFreeMemory(context->device, texture->memory, NULL);
        return false;
    }

    // Update descriptor set
    VkDescriptorImageInfo imageDescInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = texture->view,
        .sampler = texture->sampler
    };

    VkWriteDescriptorSet descriptorWrite = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = texture->descriptorSet,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &imageDescInfo
    };

    vkUpdateDescriptorSets(context->device, 1, &descriptorWrite, 0, NULL);

    texture->width = texWidth;
    texture->height = texHeight;

    return true;
}

void destroy_texture(VulkanContext* context, Texture2D* texture) {
    if (texture->sampler) vkDestroySampler(context->device, texture->sampler, NULL);
    if (texture->view) vkDestroyImageView(context->device, texture->view, NULL);
    if (texture->image) vkDestroyImage(context->device, texture->image, NULL);
    if (texture->memory) vkFreeMemory(context->device, texture->memory, NULL);
    
    texture->sampler = VK_NULL_HANDLE;
    texture->view = VK_NULL_HANDLE;
    texture->image = VK_NULL_HANDLE;
    texture->memory = VK_NULL_HANDLE;
    texture->descriptorSet = VK_NULL_HANDLE;
    texture->loaded = false;
}



/// 3D TEXTURES

// Initialize 3D textured vertex buffer
void renderer_init_textured3D() {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(vertices3D_textured),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufferInfo, NULL, &vertexBuffer3D_textured);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, vertexBuffer3D_textured, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    vkAllocateMemory(device, &allocInfo, NULL, &vertexBufferMemory3D_textured);
    vkBindBufferMemory(device, vertexBuffer3D_textured, vertexBufferMemory3D_textured, 0);
}

void texture3D(vec3 position, vec2 size, Texture2D* texture, Color tint) {
    if (vertex_count_3D_textured + 6 >= MAX_VERTICES || !texture || !texture->loaded) {
        if (!texture) printf("texture3D: NULL texture\n");
        else if (!texture->loaded) printf("texture3D: texture not loaded\n");
        return;
    }

    float x = position[0], y = position[1], z = position[2];
    float w = size[0], h = size[1];

    // Find or create batch for this texture
    int batchIndex = -1;
    if (texture3DBatchCount > 0 && texture3DBatches[texture3DBatchCount - 1].texture == texture) {
        batchIndex = texture3DBatchCount - 1;
    } else {
        if (texture3DBatchCount >= MAX_TEXTURES) {
            fprintf(stderr, "Too many texture batches in 3D!\n");
            return;
        }
        batchIndex = texture3DBatchCount++;
        texture3DBatches[batchIndex].texture = texture;
        texture3DBatches[batchIndex].startVertex = vertex_count_3D_textured;
        texture3DBatches[batchIndex].vertexCount = 0;
        /* printf("Created 3D batch %d for texture %p at vertex %u\n",  */
        /*        batchIndex, (void*)texture, vertex_count_3D_textured); */
    }

    // Create a quad facing the camera (billboard style)
    Vertex quad[6] = {
        // Triangle 1
        { .pos = {x - w/2, y - h/2, z}, 
          .color = {tint.r, tint.g, tint.b, tint.a}, 
          .normal = {0.0f, 0.0f, 1.0f}, 
          .texCoord = {0.0f, 1.0f} },
        
        { .pos = {x + w/2, y - h/2, z}, 
          .color = {tint.r, tint.g, tint.b, tint.a}, 
          .normal = {0.0f, 0.0f, 1.0f}, 
          .texCoord = {1.0f, 1.0f} },
        
        { .pos = {x + w/2, y + h/2, z}, 
          .color = {tint.r, tint.g, tint.b, tint.a}, 
          .normal = {0.0f, 0.0f, 1.0f}, 
          .texCoord = {1.0f, 0.0f} },
        
        // Triangle 2
        { .pos = {x - w/2, y - h/2, z}, 
          .color = {tint.r, tint.g, tint.b, tint.a}, 
          .normal = {0.0f, 0.0f, 1.0f}, 
          .texCoord = {0.0f, 1.0f} },
        
        { .pos = {x + w/2, y + h/2, z}, 
          .color = {tint.r, tint.g, tint.b, tint.a}, 
          .normal = {0.0f, 0.0f, 1.0f}, 
          .texCoord = {1.0f, 0.0f} },
        
        { .pos = {x - w/2, y + h/2, z}, 
          .color = {tint.r, tint.g, tint.b, tint.a}, 
          .normal = {0.0f, 0.0f, 1.0f}, 
          .texCoord = {0.0f, 0.0f} }
    };

    memcpy(&vertices3D_textured[vertex_count_3D_textured], quad, sizeof(quad));
    vertex_count_3D_textured += 6;
    texture3DBatches[batchIndex].vertexCount += 6;
}


void renderer_upload_textured3D() {
    if (vertex_count_3D_textured == 0) return;
    
    void* data;
    vkMapMemory(device, vertexBufferMemory3D_textured, 0, sizeof(vertices3D_textured), 0, &data);
    memcpy(data, vertices3D_textured, vertex_count_3D_textured * sizeof(Vertex));
    vkUnmapMemory(device, vertexBufferMemory3D_textured);
}

void renderer_draw_textured3D(VkCommandBuffer cmd) {
    if (texture3DBatchCount == 0) return;

    /* printf("Drawing %u 3D texture batches (%u vertices total)\n",  */
    /*        texture3DBatchCount, vertex_count_3D_textured); */

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipelineTextured3D);
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer3D_textured, offsets);

    // Identity model matrix for billboards
    glm_mat4_identity(pushConstants.model);

    for (uint32_t i = 0; i < texture3DBatchCount; i++) {
        Texture3DBatch* batch = &texture3DBatches[i];
        
        if (batch->vertexCount == 0) continue;
        
        /* printf("  Batch %u: texture=%p, start=%u, count=%u\n",  */
        /*        i, (void*)batch->texture, batch->startVertex, batch->vertexCount); */
        
        // Bind descriptor sets: Set 0 = UBO (camera), Set 1 = Texture
        VkDescriptorSet descriptorSets[2] = {
            descriptorSet,                  // Set 0: Camera UBO
            batch->texture->descriptorSet   // Set 1: Texture sampler
        };
        
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            context.pipelineLayoutTextured3D,
            0, 2,
            descriptorSets,
            0, NULL
        );
        
        // Push constants for model matrix and AO
        vkCmdPushConstants(
            cmd,
            context.pipelineLayoutTextured3D,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(PushConstants),
            &pushConstants
        );
        
        // Draw this batch
        vkCmdDraw(cmd, batch->vertexCount, 1, batch->startVertex, 0);
    }
}

// Clear 3D textured content
void renderer_clear_textured3D() {
    vertex_count_3D_textured = 0;
    texture3DBatchCount = 0;
}

void renderer_shutdown() {
    vkDestroyBuffer(device, vertexBuffer, NULL);
    vkFreeMemory(device, vertexBufferMemory, NULL);
    
    // Add cleanup for 3D textured buffer
    if (vertexBuffer3D_textured) {
        vkDestroyBuffer(device, vertexBuffer3D_textured, NULL);
        vkFreeMemory(device, vertexBufferMemory3D_textured, NULL);
    }
}


/// LINE

// --- Line Renderer ---
static Vertex lineVertices[MAX_VERTICES];
uint32_t lineVertexCount = 0;
static VkBuffer lineVertexBuffer;
static VkDeviceMemory lineVertexBufferMemory;

void line_renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(lineVertices),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufferInfo, NULL, &lineVertexBuffer);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, lineVertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    vkAllocateMemory(device, &allocInfo, NULL, &lineVertexBufferMemory);
    vkBindBufferMemory(device, lineVertexBuffer, lineVertexBufferMemory, 0);
}

void line(vec3 start, vec3 end, Color color) {
    if (lineVertexCount + 2 >= MAX_VERTICES) return;
    
    vec4 colorVec4 = {color.r, color.g, color.b, color.a};
    vec3 normal = {0.0f, 1.0f, 0.0f}; // Default normal
    
    // Add to line vertex buffer
    glm_vec3_copy(start, lineVertices[lineVertexCount].pos);
    glm_vec4_copy(colorVec4, lineVertices[lineVertexCount].color);
    glm_vec3_copy(normal, lineVertices[lineVertexCount].normal);
    glm_vec2_copy((vec2){0.0f, 0.0f}, lineVertices[lineVertexCount].texCoord);
    lineVertexCount++;
    
    glm_vec3_copy(end, lineVertices[lineVertexCount].pos);
    glm_vec4_copy(colorVec4, lineVertices[lineVertexCount].color);
    glm_vec3_copy(normal, lineVertices[lineVertexCount].normal);
    glm_vec2_copy((vec2){0.0f, 0.0f}, lineVertices[lineVertexCount].texCoord);
    lineVertexCount++;
}

void line_renderer_upload() {
    if (lineVertexCount == 0) return;
    
    void* data;
    vkMapMemory(device, lineVertexBufferMemory, 0, sizeof(lineVertices), 0, &data);
    memcpy(data, lineVertices, lineVertexCount * sizeof(Vertex));
    vkUnmapMemory(device, lineVertexBufferMemory);
}

void line_renderer_draw(VkCommandBuffer cmd) {
    if (lineVertexCount == 0) return;
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &lineVertexBuffer, offsets);
    vkCmdDraw(cmd, lineVertexCount, 1, 0, 0);
}

void line_renderer_clear() {
    lineVertexCount = 0;
}

void line_renderer_shutdown() {
    if (lineVertexBuffer) {
        vkDestroyBuffer(device, lineVertexBuffer, NULL);
        vkFreeMemory(device, lineVertexBufferMemory, NULL);
        lineVertexBuffer = VK_NULL_HANDLE;
        lineVertexBufferMemory = VK_NULL_HANDLE;
    }
}

#include <stdlib.h>
#include <string.h>
#include "renderer.h"
#include "context.h"
#include "common.h"
#include "scene.h"
#include "vulkan_setup.h"

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

typedef struct { uint32_t firstVertex; uint32_t count; int slot; } ImmDrawCall;
static ImmDrawCall immDrawList[IMM_SSBO_MAX_ENTRIES];
static uint32_t   immDrawCount = 0;

uint32_t imm_append_vertices(const Vertex* verts, uint32_t count) {
    if (vertex_count + count > MAX_VERTICES) return UINT32_MAX;
    uint32_t first = vertex_count;
    memcpy(&vertices[first], verts, count * sizeof(Vertex));
    vertex_count += count;
    return first;
}

static VkDevice device;
static VkPhysicalDevice physicalDevice;
static VkCommandPool commandPool;
static VkQueue graphicsQueue;

static VkBuffer       vertexBuffer[MAX_FRAMES_IN_FLIGHT];
static VkDeviceMemory vertexBufferMemory[MAX_FRAMES_IN_FLIGHT];
static void*          vertexBufferMapped[MAX_FRAMES_IN_FLIGHT];
static uint32_t       imm_frame_index = 0;

PushConstants pushConstants;

/* ── Immediate-mode SSBO ─────────────────────────────────────────────────── */
static VkBuffer             immSSBOBuffer[MAX_FRAMES_IN_FLIGHT];
static VkDeviceMemory       immSSBOMemory[MAX_FRAMES_IN_FLIGHT];
static MeshGPUData*         immSSBOMapped[MAX_FRAMES_IN_FLIGHT];
static VkDescriptorSetLayout immSSBOLayout;
static VkDescriptorPool     immSSBOPool;
static VkDescriptorSet      immSSBOSets[MAX_FRAMES_IN_FLIGHT];
static uint32_t             immSlotCount;   /* slots used this frame        */
static uint32_t             immFrameIndex;  /* set at begin_frame           */

static ImmMaterial immCurrentMaterial = {
    .baseColorFactor    = {1.0f, 1.0f, 1.0f, 1.0f},
    .metallicFactor     = 0.0f,
    .roughnessFactor    = 0.5f,
    .emissiveStrength   = 1.0f,
    .isUnlit            = 0,
    .emissiveFactor     = {0.0f, 0.0f, 0.0f},
    .albedoIndex        = -1,
    .normalMapIndex     = -1,
    .metallicRoughIndex = -1,
    .aoIndex            = -1,
    .emissiveIndex      = -1,
};

void imm_set_material(const ImmMaterial* mat) { immCurrentMaterial = *mat; }

void imm_reset_material(void) {
    immCurrentMaterial = (ImmMaterial){
        .baseColorFactor    = {1.0f, 1.0f, 1.0f, 1.0f},
        .metallicFactor     = 0.0f,
        .roughnessFactor    = 0.5f,
        .emissiveStrength   = 1.0f,
        .isUnlit            = 0,
        .emissiveFactor     = {0.0f, 0.0f, 0.0f},
        .albedoIndex        = -1,
        .normalMapIndex     = -1,
        .metallicRoughIndex = -1,
        .aoIndex            = -1,
        .emissiveIndex      = -1,
    };
}

int imm_alloc_slot(mat4 model) {
    if (immSlotCount >= IMM_SSBO_MAX_ENTRIES) {
        static uint32_t overflow_count = 0;
        if (overflow_count++ == 0)
            fprintf(stderr, "imm SSBO overflow: renderer_clear() not called each frame, "
                            "or more than %d draw calls issued\n", IMM_SSBO_MAX_ENTRIES);
        return 0;
    }
    int slot = (int)immSlotCount++;
    MeshGPUData* d = &immSSBOMapped[immFrameIndex][slot];
    glm_mat4_copy(model, d->model);
    mat4 inv; glm_mat4_inv(model, inv); glm_mat4_transpose(inv);
    glm_mat4_copy(inv, d->normalMatrix);
    glm_vec4_copy(immCurrentMaterial.baseColorFactor, d->baseColorFactor);
    d->metallicFactor     = immCurrentMaterial.metallicFactor;
    d->roughnessFactor    = immCurrentMaterial.roughnessFactor;
    d->emissiveStrength   = immCurrentMaterial.emissiveStrength;
    d->isUnlit            = immCurrentMaterial.isUnlit;
    d->alphaMode          = 0;
    d->alphaCutoff        = 0.5f;
    glm_vec3_copy(immCurrentMaterial.emissiveFactor, d->emissiveFactor);
    d->albedoIndex        = immCurrentMaterial.albedoIndex;
    d->normalMapIndex     = immCurrentMaterial.normalMapIndex;
    d->metallicRoughIndex = immCurrentMaterial.metallicRoughIndex;
    d->aoIndex            = immCurrentMaterial.aoIndex;
    d->emissiveIndex      = immCurrentMaterial.emissiveIndex;
    /* AABB not used for culling on imm draws */
    glm_vec3_copy((vec3){-1e10f,-1e10f,-1e10f}, d->aabbMin);
    glm_vec3_copy((vec3){ 1e10f, 1e10f, 1e10f}, d->aabbMax);
    return slot;
}

void imm_ssbo_init(VulkanContext* ctx) {
    VkDeviceSize size = IMM_SSBO_MAX_ENTRIES * sizeof(MeshGPUData);

    VkDescriptorSetLayoutBinding b = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b
    };
    vkCreateDescriptorSetLayout(ctx->device, &lci, NULL, &immSSBOLayout);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_FRAMES_IN_FLIGHT, .poolSizeCount = 1, .pPoolSizes = &ps
    };
    vkCreateDescriptorPool(ctx->device, &pci, NULL, &immSSBOPool);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        vkCreateBuffer(ctx->device, &bci, NULL, &immSSBOBuffer[i]);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(ctx->device, immSSBOBuffer[i], &mr);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = findMemoryType(ctx->physicalDevice, mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        };
        vkAllocateMemory(ctx->device, &ai, NULL, &immSSBOMemory[i]);
        vkBindBufferMemory(ctx->device, immSSBOBuffer[i], immSSBOMemory[i], 0);
        vkMapMemory(ctx->device, immSSBOMemory[i], 0, size, 0, (void**)&immSSBOMapped[i]);

        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = immSSBOPool, .descriptorSetCount = 1,
            .pSetLayouts = &immSSBOLayout
        };
        vkAllocateDescriptorSets(ctx->device, &dsai, &immSSBOSets[i]);

        VkDescriptorBufferInfo dbi = { .buffer = immSSBOBuffer[i], .offset = 0, .range = size };
        VkWriteDescriptorSet w = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = immSSBOSets[i], .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .pBufferInfo = &dbi
        };
        vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
    fprintf(stdout, "Immediate SSBO: %.1f MB x%d frames\n",
            (double)(IMM_SSBO_MAX_ENTRIES * sizeof(MeshGPUData)) / (1024*1024),
            MAX_FRAMES_IN_FLIGHT);
}

void imm_ssbo_shutdown(VulkanContext* ctx) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (immSSBOMapped[i])   { vkUnmapMemory(ctx->device, immSSBOMemory[i]); immSSBOMapped[i] = NULL; }
        if (immSSBOBuffer[i])   { vkDestroyBuffer(ctx->device, immSSBOBuffer[i], NULL); immSSBOBuffer[i] = VK_NULL_HANDLE; }
        if (immSSBOMemory[i])   { vkFreeMemory(ctx->device, immSSBOMemory[i], NULL);    immSSBOMemory[i] = VK_NULL_HANDLE; }
    }
    if (immSSBOLayout) { vkDestroyDescriptorSetLayout(ctx->device, immSSBOLayout, NULL); immSSBOLayout = VK_NULL_HANDLE; }
    if (immSSBOPool)   { vkDestroyDescriptorPool(ctx->device, immSSBOPool, NULL);        immSSBOPool   = VK_NULL_HANDLE; }
}

void imm_ssbo_begin_frame(VulkanContext* ctx, uint32_t frameIndex) {
    (void)ctx;
    immFrameIndex   = frameIndex;
    imm_frame_index = frameIndex;
    immSlotCount    = 0;
    immDrawCount    = 0;
    vertex_count    = 0;
    lineVertexCount = 0;
}

VkDescriptorSet      imm_ssbo_get_set(uint32_t frameIndex)  { return immSSBOSets[frameIndex]; }
VkDescriptorSetLayout imm_ssbo_get_layout(void)             { return immSSBOLayout; }

static void create_mapped_buffer(VkDevice dev, VkPhysicalDevice physDev, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    vkCreateBuffer(dev, &bufferInfo, NULL, buffer);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(dev, *buffer, &memReq);
    VkMemoryAllocateInfo allocInfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = memReq.size, .memoryTypeIndex = findMemoryType(physDev, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    vkAllocateMemory(dev, &allocInfo, NULL, memory);
    vkBindBufferMemory(dev, *buffer, *memory, 0);
    vkMapMemory(dev, *memory, 0, size, 0, mapped);
}

void renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        create_mapped_buffer(device, physicalDevice, sizeof(vertices),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             &vertexBuffer[i], &vertexBufferMemory[i], &vertexBufferMapped[i]);
    }
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
    uint32_t first = vertex_count;
    vec3 edge1, edge2, normal;
    glm_vec3_sub(b, a, edge1);
    glm_vec3_sub(c, a, edge2);
    glm_vec3_cross(edge1, edge2, normal);
    glm_vec3_normalize(normal);
    vertex_with_normal(a, color, normal);
    vertex_with_normal(b, color, normal);
    vertex_with_normal(c, color, normal);
    mat4 identity; glm_mat4_identity(identity);
    imm_emit(first, 3, identity);
}

void plane(vec3 origin, vec2 size, Color color) {
    uint32_t first = vertex_count;
    float w = size[0] / 2.0f;
    float h = size[1] / 2.0f;
    vec3 a, b, c, d;
    glm_vec3_add(origin, (vec3){-w, 0.0f, -h}, a);
    glm_vec3_add(origin, (vec3){+w, 0.0f, -h}, b);
    glm_vec3_add(origin, (vec3){+w, 0.0f, +h}, c);
    glm_vec3_add(origin, (vec3){-w, 0.0f, +h}, d);
    vec3 normal = {0.0f, 1.0f, 0.0f};
    vertex_with_normal(a, color, normal);
    vertex_with_normal(b, color, normal);
    vertex_with_normal(c, color, normal);
    vertex_with_normal(a, color, normal);
    vertex_with_normal(c, color, normal);
    vertex_with_normal(d, color, normal);
    mat4 identity; glm_mat4_identity(identity);
    imm_emit(first, 6, identity);
}

//    h-------g
//   /|      /|
//  d-------c |
//  | |  .  | |  <- Origin
//  | e-----|-f
//  |/      |/
//  a-------b

void cube(vec3 origin, float size, Color color) {
    uint32_t first = vertex_count;
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
    mat4 identity; glm_mat4_identity(identity);
    imm_emit(first, 36, identity);
}

// TODO It should be shader based so it's cheaper and higher quality
void sphere(vec3 center, float radius, int latDiv, int longDiv, Color color) {
    uint32_t first = vertex_count;
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
    mat4 identity; glm_mat4_identity(identity);
    imm_emit(first, vertex_count - first, identity);
}


/* Call after filling vertices for one primitive — records the draw call */
void imm_emit(uint32_t firstVertex, uint32_t count, mat4 model) {
    if (immDrawCount >= IMM_SSBO_MAX_ENTRIES) return;
    int slot = imm_alloc_slot(model);
    immDrawList[immDrawCount++] = (ImmDrawCall){ firstVertex, count, slot };
}

/* Emit with a pre-allocated slot — use when batching multiple primitives
   under the same transform/material to save SSBO entries (e.g. text3D).  */
void imm_emit_with_slot(uint32_t firstVertex, uint32_t count, int slot) {
    if (immDrawCount >= IMM_SSBO_MAX_ENTRIES) return;
    immDrawList[immDrawCount++] = (ImmDrawCall){ firstVertex, count, slot };
}

void renderer_upload() {
    if (vertex_count == 0) return;
    memcpy(vertexBufferMapped[imm_frame_index], vertices, vertex_count * sizeof(Vertex));
}

void renderer_draw(VkCommandBuffer cmd) {
    if (immDrawCount == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipeline);
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer[imm_frame_index], offsets);
    for (uint32_t i = 0; i < immDrawCount; i++) {
        pushConstants.meshIndex = immDrawList[i].slot;
        vkCmdPushConstants(cmd, context.pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &pushConstants);
        vkCmdDraw(cmd, immDrawList[i].count, 1, immDrawList[i].firstVertex, 0);
    }
}

/* Draw a range of vertices with a specific imm SSBO slot */
void renderer_draw_single(VkCommandBuffer cmd, uint32_t firstVertex,
                          uint32_t count, int slot) {
    pushConstants.meshIndex = slot;
    vkCmdPushConstants(cmd, context.pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pushConstants);
    vkCmdDraw(cmd, count, 1, firstVertex, 0);
}

void renderer_clear() {
    vertex_count  = 0;
    immDrawCount  = 0;
}

void sort_meshes_by_alpha(Meshes* meshes, vec3 cameraPos) {
    if (meshes->count <= 1) return;

    // Pass 1: partition opaque/mask to front, blend to back (stable, one pass)
    size_t write_idx = 0;
    for (size_t i = 0; i < meshes->count; i++) {
        if (meshes->items[i].alpha_mode != 2) {
            if (i != write_idx) {
                Mesh tmp = meshes->items[write_idx];
                meshes->items[write_idx] = meshes->items[i];
                meshes->items[i] = tmp;
            }
            write_idx++;
        }
    }
    size_t non_blended_count = write_idx;

    // Pass 2: insertion sort blended subset back-to-front using distance²  (no sqrt)
    for (size_t i = non_blended_count + 1; i < meshes->count; i++) {
        Mesh key = meshes->items[i];
        vec3 kp  = { key.model[3][0], key.model[3][1], key.model[3][2] };
        float kd = glm_vec3_distance2(cameraPos, kp);
        size_t j = i;
        while (j > non_blended_count) {
            vec3 pp = { meshes->items[j-1].model[3][0],
                        meshes->items[j-1].model[3][1],
                        meshes->items[j-1].model[3][2] };
            if (glm_vec3_distance2(cameraPos, pp) >= kd) break;
            meshes->items[j] = meshes->items[j-1];
            j--;
        }
        meshes->items[j] = key;
    }
}

// WITH TEXTURES AND UNLIT
void mesh(VkCommandBuffer cmd, Mesh* mesh) {
    /* legacy direct-draw path — model/material data comes from SSBO for
       indirect meshes; this path is only used for dynamic/morph meshes  */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipelineTextured3D);
    vkCmdPushConstants(cmd, context.pipelineLayoutTextured3D,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pushConstants);
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, offsets);
    vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
}

void mesh_update_morph(Mesh* mesh) {
    if (!mesh->morph_data || !mesh->morph_data->base_vertices) return;

    MorphData* morph = mesh->morph_data;

    // Skip entirely if all weights are zero — nothing to blend
    bool any_active = false;
    for (size_t t = 0; t < morph->target_count; t++) {
        if (morph->weights[t] != 0.0f) { any_active = true; break; }
    }
    if (!any_active) return;

    Vertex* morphed_base = malloc(morph->base_vertex_count * sizeof(Vertex));
    memcpy(morphed_base, morph->base_vertices, morph->base_vertex_count * sizeof(Vertex));

    // Track if any morph target has normal deltas
    bool has_normal_deltas = false;

    // Apply morph targets to the base vertices
    for (size_t t = 0; t < morph->target_count; t++) {
        MorphTarget* target = &morph->targets[t];
        float weight = morph->weights[t];

        if (weight == 0.0f || !target->positions) {
            continue;
        }

        if (target->normals) {
            has_normal_deltas = true;
        }

        // Apply position and normal deltas to base vertices
        for (size_t v = 0; v < morph->base_vertex_count && v < target->vertex_count; v++) {
            morphed_base[v].pos[0] += target->positions[v][0] * weight;
            morphed_base[v].pos[1] += target->positions[v][1] * weight;
            morphed_base[v].pos[2] += target->positions[v][2] * weight;

            if (target->normals) {
                morphed_base[v].normal[0] += target->normals[v][0] * weight;
                morphed_base[v].normal[1] += target->normals[v][1] * weight;
                morphed_base[v].normal[2] += target->normals[v][2] * weight;
            }
        }
    }

    // If no normal deltas were provided, recalculate normals for smooth surfaces
    if (!has_normal_deltas) {
        // For smooth surfaces like spheres, normals should point from center
        // Calculate average center of the mesh
        vec3 center = {0.0f, 0.0f, 0.0f};
        for (size_t v = 0; v < morph->base_vertex_count; v++) {
            center[0] += morphed_base[v].pos[0];
            center[1] += morphed_base[v].pos[1];
            center[2] += morphed_base[v].pos[2];
        }
        center[0] /= morph->base_vertex_count;
        center[1] /= morph->base_vertex_count;
        center[2] /= morph->base_vertex_count;

        // Recalculate normals as vectors from center to vertex
        for (size_t v = 0; v < morph->base_vertex_count; v++) {
            vec3 normal;
            glm_vec3_sub(morphed_base[v].pos, center, normal);
            glm_vec3_normalize(normal);
            glm_vec3_copy(normal, morphed_base[v].normal);
        }
    } else {
        // Renormalize normals if deltas were provided
        for (size_t v = 0; v < morph->base_vertex_count; v++) {
            vec3 normal;
            glm_vec3_copy(morphed_base[v].normal, normal);
            glm_vec3_normalize(normal);
            glm_vec3_copy(normal, morphed_base[v].normal);
        }
    }

    // Now expand to final vertex buffer using index mapping
    Vertex* final_vertices = malloc(mesh->vertexCount * sizeof(Vertex));

    if (morph->index_map) {
        // Indexed mesh - use mapping
        for (size_t i = 0; i < mesh->vertexCount; i++) {
            uint32_t base_idx = morph->index_map[i];
            final_vertices[i] = morphed_base[base_idx];
        }
    } else {
        // Non-indexed mesh - direct copy
        memcpy(final_vertices, morphed_base, mesh->vertexCount * sizeof(Vertex));
    }

    // Upload to GPU
    void* data;
    VkDeviceSize size = mesh->vertexCount * sizeof(Vertex);
    vkMapMemory(context.device, mesh->vertexBufferMemory, 0, size, 0, &data);
    memcpy(data, final_vertices, size);
    vkUnmapMemory(context.device, mesh->vertexBufferMemory);

    free(morphed_base);
    free(final_vertices);
}

void mesh_destroy(VkDevice device, Mesh* mesh) {
    if (mesh->vertexBuffer) vkDestroyBuffer(device, mesh->vertexBuffer, NULL);
    if (mesh->vertexBufferMemory) vkFreeMemory(device, mesh->vertexBufferMemory, NULL);
    if (mesh->indexBuffer) vkDestroyBuffer(device, mesh->indexBuffer, NULL);
    if (mesh->indexBufferMemory) vkFreeMemory(device, mesh->indexBufferMemory, NULL);

    if (mesh->morph_data) {
        for (size_t t = 0; t < mesh->morph_data->target_count; t++) {
            if (mesh->morph_data->targets[t].positions) {
                free(mesh->morph_data->targets[t].positions);
            }
            if (mesh->morph_data->targets[t].normals) {
                free(mesh->morph_data->targets[t].normals);
            }
        }
        if (mesh->morph_data->targets) free(mesh->morph_data->targets);
        if (mesh->morph_data->weights) free(mesh->morph_data->weights);
        if (mesh->morph_data->base_vertices) free(mesh->morph_data->base_vertices);
        if (mesh->morph_data->index_map) free(mesh->morph_data->index_map);  // ADD THIS
        free(mesh->morph_data);
        mesh->morph_data = NULL;
    }

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
    if (meshes->count == 0) return;

    VkPipeline   bound_pipeline = VK_NULL_HANDLE;
    VkBuffer     bound_vbuf     = VK_NULL_HANDLE;
    VkDeviceSize zero_offset    = 0;

    /* sets 0/1/2 already bound in recordCommandBuffer before meshes_draw is called */

    if (context.megaVertexBuffer != VK_NULL_HANDLE) {
        vkCmdBindVertexBuffers(cmd, 0, 1, &context.megaVertexBuffer, &zero_offset);
        bound_vbuf = context.megaVertexBuffer;
    }

    for (size_t i = 0; i < meshes->count; ++i) {
        Mesh* m = &meshes->items[i];

        /* Static meshes (in mega buffer) are drawn by the indirect pass —
           only draw dynamic meshes (morph targets) here.                  */
        if (m->megaBaseVertex != UINT32_MAX) continue;

        VkPipeline       want_pipe   = context.graphicsPipeline;
        VkPipelineLayout want_layout = context.pipelineLayout;

        if (want_pipe != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want_pipe);
            bound_pipeline = want_pipe;
        }

        vkCmdPushConstants(cmd, want_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &pushConstants);

        if (m->megaBaseVertex == UINT32_MAX) {
            if (bound_vbuf != m->vertexBuffer) {
                vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertexBuffer, &zero_offset);
                bound_vbuf = m->vertexBuffer;
            }
            vkCmdDraw(cmd, m->vertexCount, 1, 0, 0);
        } else {
            if (bound_vbuf != context.megaVertexBuffer) {
                vkCmdBindVertexBuffers(cmd, 0, 1, &context.megaVertexBuffer, &zero_offset);
                bound_vbuf = context.megaVertexBuffer;
            }
            vkCmdDraw(cmd, m->vertexCount, 1, m->megaBaseVertex, 0);
        }
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

Vertex2D vertices2D[MAX_VERTICES];
uint32_t vertexCount2D = 0;

void renderer2D_init() {
    /* allocate MAX_FRAMES_IN_FLIGHT buffers; store all in context using index 0
       as the base — context holds vertexBuffer2D[MAX_FRAMES_IN_FLIGHT] so we
       write each slot directly */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        create_mapped_buffer(context.device, context.physicalDevice, sizeof(vertices2D),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             &context.vertexBuffer2D[i],
                             &context.vertexBufferMemory2D[i],
                             &context.vertexBuffer2DMapped[i]);
    }
}

void quad2D(vec2 position, vec2 size, Color color) {
    if (vertexCount2D + 6 > MAX_VERTICES) return;

    float x = position[0], y = position[1];
    float w = size[0], h = size[1];

    /* textureIndex = -1: shader outputs fragColor directly, no texture sample */
    Vertex2D quad[6] = {
        {{x,     y    }, color, {0.0f, 0.0f}, -1},
        {{x + w, y    }, color, {1.0f, 0.0f}, -1},
        {{x + w, y + h}, color, {1.0f, 1.0f}, -1},
        {{x,     y    }, color, {0.0f, 0.0f}, -1},
        {{x + w, y + h}, color, {1.0f, 1.0f}, -1},
        {{x,     y + h}, color, {0.0f, 1.0f}, -1}
    };

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void texture2D(vec2 position, vec2 size, Texture2D* texture, Color tint) {
    if (vertexCount2D + 6 > MAX_VERTICES || !texture || !texture->loaded) {
        if (!texture) printf("texture2D: NULL texture\n");
        else if (!texture->loaded) printf("texture2D: texture not loaded\n");
        return;
    }

    float x = position[0], y = position[1];
    float w = size[0], h = size[1];

    if (!texture->loaded) return;

    int32_t slot = (int32_t)texture->bindlessSlot;
    Vertex2D quad[6] = {
        {{x,     y    }, tint, {0.0f, 1.0f}, slot},
        {{x + w, y    }, tint, {1.0f, 1.0f}, slot},
        {{x + w, y + h}, tint, {1.0f, 0.0f}, slot},
        {{x,     y    }, tint, {0.0f, 1.0f}, slot},
        {{x + w, y + h}, tint, {1.0f, 0.0f}, slot},
        {{x,     y + h}, tint, {0.0f, 0.0f}, slot}
    };

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void renderer2D_upload() {
    if (vertexCount2D == 0) return;
    memcpy(context.vertexBuffer2DMapped[imm_frame_index], vertices2D, vertexCount2D * sizeof(Vertex2D));
}

void renderer2D_draw(VkCommandBuffer cmd) {
    if (vertexCount2D == 0) return;

    mat4 projection;
    glm_ortho(0.0f, (float)context.swapChainExtent.width,
              (float)context.swapChainExtent.height, 0.0f,
              -1.0f, 1.0f, projection);

    /* Unified 2D pipeline: set=0 is the bindless texture array.
       Colored quads have textureIndex=-1 in the vertex data so the
       shader skips sampling. Textured quads carry the bindless slot.
       Everything is interleaved in one buffer — single draw call.    */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipeline2D);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            context.pipelineLayoutTextured2D, 0, 1,
                            &context.bindlessSet, 0, NULL);
    vkCmdPushConstants(cmd, context.pipelineLayoutTextured2D,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), &projection);

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &context.vertexBuffer2D[imm_frame_index], offsets);
    vkCmdDraw(cmd, vertexCount2D, 1, 0, 0);
}

void renderer2D_clear(void) {
    vertexCount2D = 0;
}

// --- Texture Loading ---


static bool alloc_texture_image(VulkanContext* ctx, uint32_t w, uint32_t h, VkFormat fmt, VkImage* image, VkDeviceMemory* memory);
static bool upload_pixels_to_image(VulkanContext* ctx, unsigned char* pixels, VkDeviceSize imageSize, VkImage image, uint32_t w, uint32_t h, VkFormat fmt, VkImageLayout srcLayout);
static bool finalize_texture(VulkanContext* ctx, Texture2D* texture, VkFormat fmt, VkSamplerAddressMode addrMode);

bool load_texture_from_rgba_with_format(VulkanContext* context, unsigned char* rgba_data,
                                        uint32_t width, uint32_t height,
                                        Texture2D* texture, VkFormat format) {
    bool ok = alloc_texture_image(context, width, height, format, &texture->image, &texture->memory) &&
              upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image,
                                     width, height, format, VK_IMAGE_LAYOUT_UNDEFINED) &&
              finalize_texture(context, texture, format, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (ok) {
        texture->width = width;
        texture->height = height;
        texture->loaded = true;
    }
    return ok;
}

// Wrapper for backward compatibility
bool load_texture_from_rgba(VulkanContext* context, unsigned char* rgba_data,
                            uint32_t width, uint32_t height, Texture2D* texture) {
    return load_texture_from_rgba_with_format(context, rgba_data, width, height,
                                              texture, VK_FORMAT_R8G8B8A8_UNORM);
}

bool update_texture_from_rgba(VulkanContext* context, Texture2D* texture,
                              unsigned char* rgba_data, int width, int height) {
    if ((uint32_t)width != texture->width || (uint32_t)height != texture->height) {
        if (texture->view) vkDestroyImageView(context->device, texture->view, NULL);
        if (texture->image) vkDestroyImage(context->device, texture->image, NULL);
        if (texture->memory) vkFreeMemory(context->device, texture->memory, NULL);

        alloc_texture_image(context, width, height, VK_FORMAT_R8G8B8A8_SRGB, &texture->image, &texture->memory);
        upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image, width, height, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED);

        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = texture->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        vkCreateImageView(context->device, &viewInfo, NULL, &texture->view);

        VkDescriptorImageInfo imageDescInfo = { .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .imageView = texture->view, .sampler = texture->sampler };
        VkWriteDescriptorSet descriptorWrite = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = texture->descriptorSet, .dstBinding = 0, .dstArrayElement = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .pImageInfo = &imageDescInfo };
        vkUpdateDescriptorSets(context->device, 1, &descriptorWrite, 0, NULL);

        texture->width = width; texture->height = height;
        return true;
    }
    return upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image, width, height, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Shared staging buffer upload helper — stages pixels, transitions, copies, cleans up.
// Does NOT create image/view/sampler/descriptor — caller owns those.
static bool upload_pixels_to_image(VulkanContext* ctx, unsigned char* pixels,
                                   VkDeviceSize imageSize, VkImage image,
                                   uint32_t w, uint32_t h, VkFormat fmt,
                                   VkImageLayout srcLayout)
{
    VkBuffer stagingBuf; VkDeviceMemory stagingMem;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (vkCreateBuffer(ctx->device, &bci, NULL, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx->device, stagingBuf, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    if (vkAllocateMemory(ctx->device, &mai, NULL, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(ctx->device, stagingBuf, NULL); return false;
    }
    vkBindBufferMemory(ctx->device, stagingBuf, stagingMem, 0);

    void* mapped;
    vkMapMemory(ctx->device, stagingMem, 0, imageSize, 0, &mapped);
    memcpy(mapped, pixels, imageSize);
    vkUnmapMemory(ctx->device, stagingMem);

    VkCommandBuffer cmd = beginSingleTimeCommands(ctx->device, ctx->commandPool);
    transitionImageLayout(cmd, image, fmt, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(cmd, stagingBuf, image, w, h);
    transitionImageLayout(cmd, image, fmt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue, cmd);

    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
    return true;
}

// Allocate a VkImage + memory for a 2D texture.
static bool alloc_texture_image(VulkanContext* ctx, uint32_t w, uint32_t h,
                                VkFormat fmt, VkImage* image, VkDeviceMemory* memory)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .format = fmt,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .samples = VK_SAMPLE_COUNT_1_BIT
    };
    if (vkCreateImage(ctx->device, &ici, NULL, image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->device, *image, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    if (vkAllocateMemory(ctx->device, &mai, NULL, memory) != VK_SUCCESS) {
        vkDestroyImage(ctx->device, *image, NULL); return false;
    }
    vkBindImageMemory(ctx->device, *image, *memory, 0);
    return true;
}

// Attach a view + sampler + descriptor set to a texture whose image is already uploaded.
static bool finalize_texture(VulkanContext* ctx, Texture2D* texture,
                             VkFormat fmt, VkSamplerAddressMode addrMode)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    if (vkCreateImageView(ctx->device, &vci, NULL, &texture->view) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = addrMode, .addressModeV = addrMode, .addressModeW = addrMode,
        .anisotropyEnable = VK_FALSE, .maxAnisotropy = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR
    };
    if (vkCreateSampler(ctx->device, &sci, NULL, &texture->sampler) != VK_SUCCESS) {
        vkDestroyImageView(ctx->device, texture->view, NULL); return false;
    }

    VkDescriptorSetAllocateInfo dai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = ctx->descriptorPool2D, .descriptorSetCount = 1,
        .pSetLayouts = &ctx->descriptorSetLayout2D
    };
    if (vkAllocateDescriptorSets(ctx->device, &dai, &texture->descriptorSet) != VK_SUCCESS) {
        vkDestroySampler(ctx->device, texture->sampler, NULL);
        vkDestroyImageView(ctx->device, texture->view, NULL); return false;
    }

    VkDescriptorImageInfo dii = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = texture->view, .sampler = texture->sampler
    };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = texture->descriptorSet, .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .pImageInfo = &dii
    };
    vkUpdateDescriptorSets(ctx->device, 1, &wds, 0, NULL);

    /* Register into bindless array */
    texture->bindlessSlot = ctx->bindlessTextureCount;
    bindlessRegisterTexture(ctx, texture->bindlessSlot, texture->view, texture->sampler);

    return true;
}

static bool load_texture_from_pixels(VulkanContext* ctx, stbi_uc* pixels, int w, int h, Texture2D* texture) {
    if (!pixels) return false;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB;
    bool ok = alloc_texture_image(ctx, w, h, fmt, &texture->image, &texture->memory) &&
              upload_pixels_to_image(ctx, pixels, (VkDeviceSize)w*h*4, texture->image, w, h, fmt, VK_IMAGE_LAYOUT_UNDEFINED) &&
              finalize_texture(ctx, texture, fmt, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    stbi_image_free(pixels);
    if (ok) { texture->width = w; texture->height = h; texture->loaded = true; }
    return ok;
}

bool load_texture_from_memory(VulkanContext* ctx, unsigned char* data, size_t data_size, Texture2D* texture) {
    int w, h, ch;
    stbi_uc* pixels = stbi_load_from_memory(data, (int)data_size, &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) fprintf(stderr, "Failed to decode texture from memory\n");
    return load_texture_from_pixels(ctx, pixels, w, h, texture);
}

int32_t texture_pool_add_from_memory(unsigned char* data, size_t data_size) {
    if (textureCount >= MAX_TEXTURES) {
        fprintf(stderr, "Texture pool full!\n"); return -1;
    }
    if (load_texture_from_memory(&context, data, data_size, &texturePool[textureCount])) {
        return textureCount++;
    }
    return -1;
}

bool load_texture(VulkanContext* ctx, const char* filename, Texture2D* texture) {
    int w, h, ch;
    stbi_uc* pixels = stbi_load(filename, &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) fprintf(stderr, "Failed to load texture: %s\n", filename);
    return load_texture_from_pixels(ctx, pixels, w, h, texture);
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

void renderer_shutdown() {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vertexBuffer[i])       { vkDestroyBuffer(device, vertexBuffer[i],       NULL); vertexBuffer[i]       = VK_NULL_HANDLE; }
        if (vertexBufferMemory[i]) { vkFreeMemory   (device, vertexBufferMemory[i], NULL); vertexBufferMemory[i] = VK_NULL_HANDLE; }
    }
}

/// LINE

// --- Line Renderer ---
static Vertex         lineVertices[MAX_VERTICES];
uint32_t              lineVertexCount = 0;
static VkBuffer       lineVertexBuffer[MAX_FRAMES_IN_FLIGHT];
static VkDeviceMemory lineVertexBufferMemory[MAX_FRAMES_IN_FLIGHT];
static void*          lineVertexBufferMapped[MAX_FRAMES_IN_FLIGHT];

void line_renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        create_mapped_buffer(device, physicalDevice, sizeof(lineVertices),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             &lineVertexBuffer[i], &lineVertexBufferMemory[i], &lineVertexBufferMapped[i]);
    }
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
    memcpy(lineVertexBufferMapped[imm_frame_index], lineVertices, lineVertexCount * sizeof(Vertex));
}

void line_renderer_draw(VkCommandBuffer cmd) {
    if (lineVertexCount == 0) return;

    /* Lines use the PBR pipeline/layout — bind the imm SSBO so the shader
       has a valid set=2, and push meshIndex=-1 (no SSBO lookup needed for
       lines since color comes from the vertex attribute directly).         */
    VkDescriptorSet immSet = imm_ssbo_get_set(imm_frame_index);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            context.pipelineLayout, 2, 1, &immSet, 0, NULL);
    pushConstants.meshIndex = -2; /* sentinel: line draw, ignore SSBO material */
    vkCmdPushConstants(cmd, context.pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pushConstants);

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &lineVertexBuffer[imm_frame_index], offsets);
    vkCmdDraw(cmd, lineVertexCount, 1, 0, 0);
}

void line_renderer_clear() {
    lineVertexCount = 0;
}

void line_renderer_shutdown() {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (lineVertexBuffer[i]) {
            vkDestroyBuffer(device, lineVertexBuffer[i], NULL);
            vkFreeMemory(device, lineVertexBufferMemory[i], NULL);
            lineVertexBuffer[i]       = VK_NULL_HANDLE;
            lineVertexBufferMemory[i] = VK_NULL_HANDLE;
        }
    }
}

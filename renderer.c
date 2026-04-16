#include <stdlib.h>
#include <string.h>
#include "renderer.h"
#include "context.h"
#include "common.h"
#include "scene.h"
#include "vulkan_setup.h"
#include "tinyexr_c.h"
#include "camera.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <dirent.h>

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
static uint32_t vertex_count = 0;
static uint32_t dynamic_draw_count = 0;
static uint32_t frame_index = 0;

uint32_t opaqueMeshCount = 0;
uint32_t transparentMeshCount = 0;

extern uint64_t megaVertexBufferAddr;
extern uint64_t dynamicVertexBufferAddr;

uint32_t append_vertices(const Vertex* verts, uint32_t count) {
    if (vertex_count + count > MAX_DYNAMIC_VERTICES) return UINT32_MAX;
    uint32_t first = vertex_count;
    Vertex* dynVerts = (Vertex*)context.dynamicStagingMapped;
    uint32_t offset = (frame_index * MAX_DYNAMIC_VERTICES) + first;
    memcpy(&dynVerts[offset], verts, count * sizeof(Vertex));
    vertex_count += count;
    return first;
}

static VkDevice device;
static VkPhysicalDevice physicalDevice;
static VkCommandPool commandPool;
static VkQueue graphicsQueue;

PushConstants pushConstants;

static Material currentMaterial = {
    .baseColorFactor    = {1.0f, 1.0f, 1.0f, 1.0f},
    .metallicFactor     = 0.0f,
    .roughnessFactor    = 0.5f,
    .emissiveStrength   = 1.0f,
    .isUnlit            = 0,
    .alphaMode          = 2,
    .emissiveFactor     = {0.0f, 0.0f, 0.0f},
    .albedoIndex        = -1,
    .normalMapIndex     = -1,
    .metallicRoughIndex = -1,
    .aoIndex            = -1,
    .emissiveIndex      = -1,
};

void set_material(const Material* mat) { currentMaterial = *mat; }

void reset_material(void) {
    currentMaterial = (Material){
        .baseColorFactor    = {1.0f, 1.0f, 1.0f, 1.0f},
        .metallicFactor     = 0.0f,
        .roughnessFactor    = 0.5f,
        .emissiveStrength   = 1.0f,
        .isUnlit            = 0,
        .alphaMode          = 2,
        .emissiveFactor     = {0.0f, 0.0f, 0.0f},
        .albedoIndex        = -1,
        .normalMapIndex     = -1,
        .metallicRoughIndex = -1,
        .aoIndex            = -1,
        .emissiveIndex      = -1,
        .displacementIndex  = -1,
        .displacementScale  = 0.1f, // Default 10cm displacement
        .transmissionFactor = 0.0f,
        .ior                = 1.5f,
        .thicknessFactor    = 0.0f,
        .transmissionIndex  = -1,
        .thicknessIndex     = -1,
        .attenuationColor   = {1.0f, 1.0f, 1.0f},
        .attenuationDistance = 100000.0f,
        .dispersion         = 0.0f,
    };
}

static bool load_texture_from_pixels(VulkanContext* ctx, stbi_uc* pixels, int w, int h, Texture2D* texture);

static float* load_exr_as_float(const char* path, int* out_w, int* out_h, bool is_normal, bool is_roughness) {
    printf("\n[EXR] Decoding %s (HDR 32-bit float)...\n", path);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* file_data = malloc(file_size);
    fread(file_data, 1, file_size, f);
    fclose(f);

    ExrContextCreateInfo ctx_info = { .api_version = TINYEXR_C_API_VERSION };
    ExrContext exr_ctx;
    if (exr_context_create(&ctx_info, &exr_ctx) != EXR_SUCCESS) {
        free(file_data); return NULL;
    }

    ExrDataSource src;
    exr_data_source_from_memory(file_data, file_size, &src);

    ExrDecoderCreateInfo dec_info = { .source = src };
    ExrDecoder decoder;
    if (exr_decoder_create(exr_ctx, &dec_info, &decoder) != EXR_SUCCESS) {
        exr_context_destroy(exr_ctx); free(file_data); return NULL;
    }

    ExrImage image;
    if (exr_decoder_parse_header(decoder, &image) != EXR_SUCCESS) {
        exr_decoder_destroy(decoder); exr_context_destroy(exr_ctx); free(file_data); return NULL;
    }

    ExrImageInfo img_info;
    exr_image_get_info(image, &img_info);
    *out_w = img_info.width;
    *out_h = img_info.height;

    uint32_t c = img_info.num_channels;
    ExrPart part;
    exr_image_get_part(image, 0, &part);

    size_t num_pixels = (size_t)img_info.width * img_info.height;
    float* float_data = malloc(num_pixels * c * sizeof(float));

    ExrCommandBufferCreateInfo cmd_info = { .decoder = decoder };
    ExrCommandBuffer cmd;
    exr_command_buffer_create(exr_ctx, &cmd_info, &cmd);
    exr_command_buffer_begin(cmd);

    ExrFullImageRequest req = {
        .part = part,
        .output = { .data = float_data, .size = num_pixels * c * sizeof(float) },
        .channels_mask = 0,
        .output_pixel_type = EXR_PIXEL_FLOAT,
        .output_layout = EXR_LAYOUT_INTERLEAVED
    };
    exr_cmd_request_full_image(cmd, &req);
    exr_command_buffer_end(cmd);

    ExrSubmitInfo submit = { .command_buffer_count = 1, .command_buffers = &cmd };
    exr_submit(decoder, &submit);
    exr_decoder_wait_idle(decoder);

    exr_command_buffer_destroy(cmd);
    exr_image_destroy(image);
    exr_decoder_destroy(decoder);
    exr_context_destroy(exr_ctx);
    free(file_data);

    // Pack into pristine 32-bit RGBA float
    float* rgba_float = malloc(num_pixels * 4 * sizeof(float));
    for (size_t i = 0; i < num_pixels; i++) {
        float r = float_data[i * c + 0];
        float g = (c > 1) ? float_data[i * c + 1] : r;
        float b = (c > 2) ? float_data[i * c + 2] : r;
        float a = (c > 3) ? float_data[i * c + 3] : 1.0f;

        if (is_normal) {
            // Polyhaven EXR normals are already encoded in [0.0, 1.0] space.
            // We pass the pristine 32-bit floats directly to the GPU without mangling them!
        }

        if (is_roughness) {
            // EXR roughness is usually in R. glTF expects Roughness in G.
            float rough = r;
            r = 1.0f;       // AO default
            g = rough;      // Roughness
            b = 0.0f;       // Metallic default
            a = 1.0f;
        }

        rgba_float[i * 4 + 0] = r;
        rgba_float[i * 4 + 1] = g;
        rgba_float[i * 4 + 2] = b;
        rgba_float[i * 4 + 3] = a;
    }
    free(float_data);
    printf("[EXR] -> Successfully unpacked and formatted HDR float data.\n");

    return rgba_float;
}

// Forward declarations for texture utilities
static bool alloc_texture_image(VulkanContext* ctx, uint32_t w, uint32_t h, VkFormat fmt, VkImage* image, VkDeviceMemory* memory);
static bool upload_pixels_to_image(VulkanContext* ctx, unsigned char* pixels, VkDeviceSize imageSize, VkImage image, uint32_t w, uint32_t h, VkFormat fmt, VkImageLayout srcLayout);
static bool finalize_texture(VulkanContext* ctx, Texture2D* texture, VkFormat fmt, VkSamplerAddressMode addrMode);

static bool load_texture_from_float_pixels(VulkanContext* ctx, float* pixels, int w, int h, Texture2D* texture) {
    if (!pixels) return false;
    // Massive precision upgrade! 32-bit floats preserve the raw EXR values perfectly
    VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    bool ok = alloc_texture_image(ctx, w, h, fmt, &texture->image, &texture->memory) &&
              upload_pixels_to_image(ctx, (unsigned char*)pixels, (VkDeviceSize)w*h*16, texture->image, w, h, fmt, VK_IMAGE_LAYOUT_UNDEFINED) &&
              finalize_texture(ctx, texture, fmt, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    if (ok) { texture->width = w; texture->height = h; texture->loaded = true; }
    return ok;
}

static int32_t texture_pool_load_roughness_to_gltf(VulkanContext* ctx, const char* roughPath) {
    if (!roughPath) return -1;
    int w, h, ch;

    if (strstr(roughPath, ".exr")) {
        float* floatPixels = load_exr_as_float(roughPath, &w, &h, false, true);
        if (!floatPixels) return -1;
        if (textureCount >= MAX_TEXTURES) { free(floatPixels); return -1; }
        if (load_texture_from_float_pixels(ctx, floatPixels, w, h, &texturePool[textureCount])) {
            free(floatPixels);
            return textureCount++;
        }
        free(floatPixels);
        return -1;
    }

    stbi_uc* roughPixels = stbi_load(roughPath, &w, &h, &ch, 1);
    if (!roughPixels) {
        fprintf(stderr, "[WARNING] Failed to load roughness: %s\n", roughPath);
        return -1;
    }

    size_t size = (size_t)w * h * 4;
    stbi_uc* packed = malloc(size);
    for (int i = 0; i < w * h; i++) {
        packed[i*4 + 0] = 255;             // AO default
        packed[i*4 + 1] = roughPixels[i];  // Roughness correctly packed in G!
        packed[i*4 + 2] = 0;               // Metallic default
        packed[i*4 + 3] = 255;
    }
    free(roughPixels);

    if (textureCount >= MAX_TEXTURES) {
        free(packed);
        return -1;
    }

    bool ok = load_texture_from_pixels(ctx, packed, w, h, &texturePool[textureCount]);
    free(packed); // Always free the packed roughness buffer after uploading it to the GPU!

    if (ok) {
        return textureCount++;
    }
    return -1;
}

Material load_pbr_material(const char* albedoPath, const char* normalPath, const char* roughnessPath) {
    Material mat;
    reset_material();
    mat = currentMaterial;

    int32_t pIdx;
    if (albedoPath) {
        pIdx = texture_pool_add(&context, albedoPath);
        if (pIdx >= 0) mat.albedoIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR] Loaded Albedo: %s (Slot: %d)\n", albedoPath, mat.albedoIndex);
    }
    if (normalPath) {
        pIdx = texture_pool_add(&context, normalPath);
        if (pIdx >= 0) mat.normalMapIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR] Loaded Normal: %s (Slot: %d)\n", normalPath, mat.normalMapIndex);
    }
    if (roughnessPath) {
        pIdx = texture_pool_load_roughness_to_gltf(&context, roughnessPath);
        if (pIdx >= 0) mat.metallicRoughIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR] Loaded Roughness: %s (Slot: %d)\n", roughnessPath, mat.metallicRoughIndex);
    }

    // Clear default factors since we are using textures
    if (albedoPath) glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, mat.baseColorFactor);
    if (roughnessPath) mat.roughnessFactor = 1.0f;
    if (roughnessPath) mat.metallicFactor = 1.0f;

    return mat;
}

Material load_pbr_material_dir(const char* dirPath) {
    DIR *dir;
    struct dirent *ent;
    char albedoPath[512] = {0};
    char normalPath[512] = {0};
    char roughPath[512] = {0};
    char aoPath[512] = {0};
    char dispPath[512] = {0};

    printf("\n[PBR SCAN] ==========================================\n");
    printf("[PBR SCAN] Scanning directory: %s\n", dirPath);

    if ((dir = opendir(dirPath)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            // Check for keywords in the filename
            if (strstr(ent->d_name, "diff") || strstr(ent->d_name, "albedo") || strstr(ent->d_name, "basecolor")) {
                snprintf(albedoPath, sizeof(albedoPath), "%s/%s", dirPath, ent->d_name);
            } else if (strstr(ent->d_name, "nor")) {
                snprintf(normalPath, sizeof(normalPath), "%s/%s", dirPath, ent->d_name);
            } else if (strstr(ent->d_name, "rough")) {
                snprintf(roughPath, sizeof(roughPath), "%s/%s", dirPath, ent->d_name);
            } else if (strstr(ent->d_name, "ao") || strstr(ent->d_name, "ambient")) {
                snprintf(aoPath, sizeof(aoPath), "%s/%s", dirPath, ent->d_name);
            } else if (strstr(ent->d_name, "disp") || strstr(ent->d_name, "height")) {
                snprintf(dispPath, sizeof(dispPath), "%s/%s", dirPath, ent->d_name);
            }
        }
        closedir(dir);
    } else {
        fprintf(stderr, "[WARNING] Could not open material directory: %s\n", dirPath);
    }

    Material mat;
    reset_material();
    mat = currentMaterial;

    int32_t pIdx;
    if (albedoPath[0]) {
        pIdx = texture_pool_add(&context, albedoPath);
        if (pIdx >= 0) mat.albedoIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Albedo mapped to Bindless Slot: %d\n", mat.albedoIndex);
    }
    if (normalPath[0]) {
        pIdx = texture_pool_add(&context, normalPath);
        if (pIdx >= 0) mat.normalMapIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Normal mapped to Bindless Slot: %d\n", mat.normalMapIndex);
    }
    if (roughPath[0]) {
        pIdx = texture_pool_load_roughness_to_gltf(&context, roughPath);
        if (pIdx >= 0) mat.metallicRoughIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Roughness mapped to Bindless Slot: %d\n", mat.metallicRoughIndex);
    }
    if (aoPath[0]) {
        pIdx = texture_pool_add(&context, aoPath);
        if (pIdx >= 0) mat.aoIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> AO mapped to Bindless Slot: %d\n", mat.aoIndex);
    }
    if (dispPath[0]) {
        pIdx = texture_pool_add(&context, dispPath);
        if (pIdx >= 0) mat.displacementIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Displacement mapped to Bindless Slot: %d\n", mat.displacementIndex);

        // Push the scale aggressively so the physical extrusion is undeniably visible!
        mat.displacementScale = 0.5f;
    }
    printf("[PBR SCAN] ==========================================\n\n");

    if (albedoPath[0]) glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, mat.baseColorFactor);
    if (roughPath[0]) mat.roughnessFactor = 1.0f;
    if (roughPath[0]) mat.metallicFactor = 1.0f;

    return mat;
}

int alloc_slot(mat4 model) {
    if (dynamic_draw_count >= MAX_DYNAMIC_MESHES) return 0;
    uint32_t staticCount = (uint32_t)scene.meshes.count;
    int slot = staticCount + dynamic_draw_count;

    MeshGPUData* d = &((MeshGPUData*)context.meshSSBOMapped[frame_index])[slot];
    glm_mat4_copy(model, d->model);
    glm_vec4_copy(currentMaterial.baseColorFactor, d->baseColorFactor);
    d->metallicFactor     = currentMaterial.metallicFactor;
    d->roughnessFactor    = currentMaterial.roughnessFactor;
    d->emissiveStrength   = currentMaterial.emissiveStrength;
    d->isUnlit            = currentMaterial.isUnlit;
    d->alphaMode          = currentMaterial.alphaMode;
    d->alphaCutoff        = 0.5f;
    glm_vec3_copy(currentMaterial.emissiveFactor, d->emissiveFactor);
    d->albedoIndex        = currentMaterial.albedoIndex;
    d->normalMapIndex     = currentMaterial.normalMapIndex;
    d->metallicRoughIndex = currentMaterial.metallicRoughIndex;
    d->aoIndex            = currentMaterial.aoIndex;
    d->emissiveIndex      = currentMaterial.emissiveIndex;
    d->displacementIndex  = currentMaterial.displacementIndex;
    d->displacementScale  = currentMaterial.displacementScale;
    d->transmissionFactor = currentMaterial.transmissionFactor;
    d->ior                = currentMaterial.ior;
    d->thicknessFactor    = currentMaterial.thicknessFactor;
    d->transmissionIndex  = currentMaterial.transmissionIndex;
    d->thicknessIndex     = currentMaterial.thicknessIndex;
    d->attenuationColorR  = currentMaterial.attenuationColor[0];
    d->attenuationColorG  = currentMaterial.attenuationColor[1];
    d->attenuationColorB  = currentMaterial.attenuationColor[2];
    d->attenuationDistance = currentMaterial.attenuationDistance;
    d->dispersion         = currentMaterial.dispersion;
    d->thicknessFactor    = currentMaterial.thicknessFactor;
    glm_vec3_copy((vec3){-1e5f, -1e5f, -1e5f}, d->aabbMin);
    glm_vec3_copy((vec3){ 1e5f,  1e5f,  1e5f}, d->aabbMax);

    d->jointOffset        = -1;
    d->morphCount         = 0;
    d->morphDeltaOffset   = 0;
    d->morphWeightOffset  = 0;
    return slot;
}

void begin_frame(void) {
    frame_index = context.currentFrame;
    dynamic_draw_count = 0;
    vertex_count = 0;
    line_renderer_clear();
    context.indirectDrawCount = (uint32_t)scene.meshes.count;

    // AAA FIX: Sort meshes every single frame!
    // Shell sort is O(N log N) and runs in microseconds. This guarantees
    // opaqueMeshCount is perfectly synced with compact.comp to prevent flickering.
    vec3 camPos = { camera.position[0], camera.position[1], camera.position[2] };
    sort_meshes_by_alpha(&scene.meshes, camPos);

    // AAA ARCHITECTURE FIX: We MUST resync the CPU sorted array with the GPU!
    // If we sort the meshes in memory but don't rebuild the indirect commands and
    // mark the SSBO dirty, the GPU will draw the wrong meshes with the wrong materials,
    // resulting in invisible objects or corrupted rendering.
    updateMeshSSBOAndIndirect(&context, &scene.meshes);
    flushMeshSSBO(&context, &scene.meshes);
}

/* void begin_frame(void) { */
/*     frame_index = context.currentFrame; */
/*     dynamic_draw_count = 0; */
/*     vertex_count = 0; */
/*     lineVertexCount = 0; */
/*     context.indirectDrawCount = (uint32_t)scene.meshes.count; */
/* } */

static void create_mapped_buffer(VkDevice dev, VkPhysicalDevice physDev, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
    VkBufferCreateInfo bufferInfo = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    vkCreateBuffer(dev, &bufferInfo, NULL, buffer);

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(dev, *buffer, &memReq);

    VkMemoryAllocateFlagsInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo allocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &flagsInfo,
        .allocationSize  = memReq.size,
        .memoryTypeIndex = findMemoryType(physDev, memReq.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    vkAllocateMemory(dev, &allocInfo, NULL, memory);
    vkBindBufferMemory(dev, *buffer, *memory, 0);
    vkMapMemory(dev, *memory, 0, size, 0, mapped);
}

void renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;
}

static inline void emit_vertex(vec3 pos, vec4 color, vec3 normal, vec2 uv, vec4 tangent) {
    if (vertex_count >= MAX_DYNAMIC_VERTICES) return;
    Vertex* dynVerts = (Vertex*)context.dynamicStagingMapped;
    uint32_t offset = (frame_index * MAX_DYNAMIC_VERTICES) + vertex_count;
    glm_vec3_copy(pos, dynVerts[offset].pos);
    glm_vec4_copy(color, dynVerts[offset].color);
    glm_vec3_copy(normal, dynVerts[offset].normal);
    glm_vec2_copy(uv, dynVerts[offset].texCoord);
    glm_vec4_copy(tangent, dynVerts[offset].tangent);

    // Explicitly zero weights/joints so unskinned dynamic primitives don't NaN out
    dynVerts[offset].weights[0] = 0.0f; dynVerts[offset].weights[1] = 0.0f;
    dynVerts[offset].weights[2] = 0.0f; dynVerts[offset].weights[3] = 0.0f;
    dynVerts[offset].joints[0] = 0; dynVerts[offset].joints[1] = 0;
    dynVerts[offset].joints[2] = 0; dynVerts[offset].joints[3] = 0;

    vertex_count++;
}

void vertex_full(vec3 pos, Color color, vec3 normal, vec2 uv, vec4 tangent) {
    emit_vertex(pos, (vec4){color.r, color.g, color.b, color.a}, normal, uv, tangent);
}

void vertex_with_normal(vec3 pos, Color color, vec3 normal) {
    emit_vertex(pos, (vec4){color.r, color.g, color.b, color.a}, normal, (vec2){0.0f, 0.0f}, (vec4){1.0f, 0.0f, 0.0f, 1.0f});
}

void vertex(vec3 pos, vec4 color) {
    emit_vertex(pos, color, (vec3){0.0f, 1.0f, 0.0f}, (vec2){0.0f, 0.0f}, (vec4){1.0f, 0.0f, 0.0f, 1.0f});
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
    emit_draw(first, 3, identity);
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
    emit_draw(first, 6, identity);
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

    /* Each face is emitted with correct UVs and tangents for PBR.
       vertex_full(pos, color, normal, uv, tangent)
       Tangent w=+1 means bitangent = cross(normal, tangent).        */

    /* Front face (Z-) — tangent points +X */
    vec3 nf = {0.0f, 0.0f, -1.0f};
    vec4 tf = {1.0f, 0.0f, 0.0f, 1.0f};
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]-s}, color, nf, (vec2){0.0f,1.0f}, tf);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]-s}, color, nf, (vec2){1.0f,1.0f}, tf);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]-s}, color, nf, (vec2){1.0f,0.0f}, tf);
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]-s}, color, nf, (vec2){0.0f,1.0f}, tf);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]-s}, color, nf, (vec2){1.0f,0.0f}, tf);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]-s}, color, nf, (vec2){0.0f,0.0f}, tf);

    /* Back face (Z+) — tangent points -X */
    vec3 nb = {0.0f, 0.0f, 1.0f};
    vec4 tb = {-1.0f, 0.0f, 0.0f, 1.0f};
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]+s}, color, nb, (vec2){0.0f,1.0f}, tb);
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]+s}, color, nb, (vec2){1.0f,1.0f}, tb);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]+s}, color, nb, (vec2){1.0f,0.0f}, tb);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]+s}, color, nb, (vec2){0.0f,1.0f}, tb);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]+s}, color, nb, (vec2){1.0f,0.0f}, tb);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]+s}, color, nb, (vec2){0.0f,0.0f}, tb);

    /* Left face (X-) — tangent points -Z */
    vec3 nl = {-1.0f, 0.0f, 0.0f};
    vec4 tl = {0.0f, 0.0f, -1.0f, 1.0f};
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]+s}, color, nl, (vec2){0.0f,1.0f}, tl);
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]-s}, color, nl, (vec2){1.0f,1.0f}, tl);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]-s}, color, nl, (vec2){1.0f,0.0f}, tl);
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]+s}, color, nl, (vec2){0.0f,1.0f}, tl);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]-s}, color, nl, (vec2){1.0f,0.0f}, tl);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]+s}, color, nl, (vec2){0.0f,0.0f}, tl);

    /* Right face (X+) — tangent points +Z */
    vec3 nr = {1.0f, 0.0f, 0.0f};
    vec4 tr = {0.0f, 0.0f, 1.0f, 1.0f};
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]-s}, color, nr, (vec2){0.0f,1.0f}, tr);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]+s}, color, nr, (vec2){1.0f,1.0f}, tr);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]+s}, color, nr, (vec2){1.0f,0.0f}, tr);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]-s}, color, nr, (vec2){0.0f,1.0f}, tr);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]+s}, color, nr, (vec2){1.0f,0.0f}, tr);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]-s}, color, nr, (vec2){0.0f,0.0f}, tr);

    /* Top face (Y+) — tangent points +X */
    vec3 nt = {0.0f, 1.0f, 0.0f};
    vec4 tt = {1.0f, 0.0f, 0.0f, 1.0f};
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]-s}, color, nt, (vec2){0.0f,1.0f}, tt);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]-s}, color, nt, (vec2){1.0f,1.0f}, tt);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]+s}, color, nt, (vec2){1.0f,0.0f}, tt);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]-s}, color, nt, (vec2){0.0f,1.0f}, tt);
    vertex_full((vec3){origin[0]+s, origin[1]+s, origin[2]+s}, color, nt, (vec2){1.0f,0.0f}, tt);
    vertex_full((vec3){origin[0]-s, origin[1]+s, origin[2]+s}, color, nt, (vec2){0.0f,0.0f}, tt);

    /* Bottom face (Y-) — tangent points +X */
    vec3 nbo = {0.0f, -1.0f, 0.0f};
    vec4 tbo = {1.0f, 0.0f, 0.0f, 1.0f};
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]+s}, color, nbo, (vec2){0.0f,1.0f}, tbo);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]+s}, color, nbo, (vec2){1.0f,1.0f}, tbo);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]-s}, color, nbo, (vec2){1.0f,0.0f}, tbo);
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]+s}, color, nbo, (vec2){0.0f,1.0f}, tbo);
    vertex_full((vec3){origin[0]+s, origin[1]-s, origin[2]-s}, color, nbo, (vec2){1.0f,0.0f}, tbo);
    vertex_full((vec3){origin[0]-s, origin[1]-s, origin[2]-s}, color, nbo, (vec2){0.0f,0.0f}, tbo);

    mat4 identity; glm_mat4_identity(identity);
    emit_draw(first, 36, identity);
}

// High-performance Sphere with UVs, Tangents, and pre-calculated trig
void sphere(vec3 center, float radius, int latDiv, int longDiv, Color color) {
    uint32_t first = vertex_count;

    float sin_lon[longDiv + 1];
    float cos_lon[longDiv + 1];
    for (int lon = 0; lon <= longDiv; ++lon) {
        float phi = (float)lon / longDiv * 2.0f * GLM_PI;
        sin_lon[lon] = sinf(phi);
        cos_lon[lon] = cosf(phi);
    }

    for (int lat = 0; lat < latDiv; ++lat) {
        float v1 = 1.0f - (float)lat / latDiv;
        float v2 = 1.0f - (float)(lat + 1) / latDiv;
        float theta1 = (float)lat / latDiv * GLM_PI;
        float theta2 = (float)(lat + 1) / latDiv * GLM_PI;

        float st1 = sinf(theta1), ct1 = cosf(theta1);
        float st2 = sinf(theta2), ct2 = cosf(theta2);

        for (int lon = 0; lon < longDiv; ++lon) {
            float u1 = (float)lon / longDiv;
            float u2 = (float)(lon + 1) / longDiv;

            vec3 p0 = { center[0] + radius * st1 * cos_lon[lon],     center[1] + radius * ct1, center[2] + radius * st1 * sin_lon[lon] };
            vec3 p1 = { center[0] + radius * st2 * cos_lon[lon],     center[1] + radius * ct2, center[2] + radius * st2 * sin_lon[lon] };
            vec3 p2 = { center[0] + radius * st2 * cos_lon[lon + 1], center[1] + radius * ct2, center[2] + radius * st2 * sin_lon[lon + 1] };
            vec3 p3 = { center[0] + radius * st1 * cos_lon[lon + 1], center[1] + radius * ct1, center[2] + radius * st1 * sin_lon[lon + 1] };

            vec3 n0, n1, n2, n3;
            glm_vec3_sub(p0, center, n0); glm_vec3_normalize(n0);
            glm_vec3_sub(p1, center, n1); glm_vec3_normalize(n1);
            glm_vec3_sub(p2, center, n2); glm_vec3_normalize(n2);
            glm_vec3_sub(p3, center, n3); glm_vec3_normalize(n3);

            vec4 t0 = { -sin_lon[lon],     0.0f, cos_lon[lon],     1.0f };
            vec4 t1 = { -sin_lon[lon],     0.0f, cos_lon[lon],     1.0f };
            vec4 t2 = { -sin_lon[lon + 1], 0.0f, cos_lon[lon + 1], 1.0f };
            vec4 t3 = { -sin_lon[lon + 1], 0.0f, cos_lon[lon + 1], 1.0f };

            // Reversed winding order to Counter-Clockwise to match glTF and Vulkan pipeline
            vertex_full(p0, color, n0, (vec2){u1, v1}, t0);
            vertex_full(p2, color, n2, (vec2){u2, v2}, t2);
            vertex_full(p1, color, n1, (vec2){u1, v2}, t1);

            vertex_full(p0, color, n0, (vec2){u1, v1}, t0);
            vertex_full(p3, color, n3, (vec2){u2, v1}, t3);
            vertex_full(p2, color, n2, (vec2){u2, v2}, t2);
        }
    }
    mat4 identity; glm_mat4_identity(identity);
    emit_draw(first, vertex_count - first, identity);
}

/* Call after filling vertices for one primitive — records the draw call */
void emit_draw(uint32_t firstVertex, uint32_t count, mat4 model) {
    if (dynamic_draw_count >= MAX_DYNAMIC_MESHES) return;
    int slot = alloc_slot(model);
    emit_draw_with_slot(firstVertex, count, slot);
}

void emit_draw_with_slot(uint32_t firstVertex, uint32_t count, int slot) {
    if (dynamic_draw_count >= MAX_DYNAMIC_MESHES) return;

    VkDeviceSize drawSize = (16384 + 4096) * sizeof(VkDrawIndexedIndirectCommand);
    VkDrawIndexedIndirectCommand* cmds = (VkDrawIndexedIndirectCommand*)((uint8_t*)context.srcIndirectBufferMapped + (frame_index * drawSize));
    cmds[slot].indexCount = count;
    cmds[slot].instanceCount = 1;
    cmds[slot].firstIndex = 0;
    uint32_t dynamicBase = context.megaVertexBufferOffset + (frame_index * MAX_DYNAMIC_VERTICES);
    cmds[slot].vertexOffset = dynamicBase + firstVertex;
    cmds[slot].firstInstance = slot;

    dynamic_draw_count++;
    context.indirectDrawCount = (uint32_t)scene.meshes.count + dynamic_draw_count;
}

void renderer_clear() {
    vertex_count  = 0;
    dynamic_draw_count = 0;
}

uint32_t get_dynamic_vertex_count(void) {
    return vertex_count;
}

Vertex* get_dynamic_vertices(void) {
    Vertex* dynVerts = (Vertex*)context.dynamicStagingMapped;
    return &dynVerts[frame_index * MAX_DYNAMIC_VERTICES];
}


void sort_meshes_by_alpha(Meshes* meshes, vec3 cameraPos) {
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
    opaqueMeshCount = (uint32_t)write_idx;
    transparentMeshCount = (uint32_t)(meshes->count - write_idx);

    size_t blend_count = meshes->count - write_idx;
    if (blend_count <= 1) return;

    // In-place Shell Sort!
    // Slashes O(N^2) to O(N log N) without dynamic allocations or deep recursion.
    Mesh* blend = &meshes->items[write_idx];
    size_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };

    for (int g = 0; g < 8; g++) {
        size_t gap = gaps[g];
        for (size_t i = gap; i < blend_count; i++) {
            Mesh temp = blend[i];
            float d_temp = glm_vec3_distance2(cameraPos, (vec3){temp.model[3][0], temp.model[3][1], temp.model[3][2]});

            size_t j;
            for (j = i; j >= gap; j -= gap) {
                float d_j = glm_vec3_distance2(cameraPos, (vec3){blend[j - gap].model[3][0], blend[j - gap].model[3][1], blend[j - gap].model[3][2]});
                if (d_j >= d_temp) break;
                blend[j] = blend[j - gap];
            }
            blend[j] = temp;
        }
    }

    opaqueMeshCount = write_idx;
    transparentMeshCount = meshes->count - write_idx;
}

// WITH TEXTURES AND UNLIT
void mesh(VkCommandBuffer cmd, Mesh* mesh) {
    /* legacy direct-draw path — model/material data comes from SSBO for
       indirect meshes; this path is only used for dynamic/morph meshes  */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipelineTextured3D);
    pushConstants.vertexBufferAddr = (mesh->megaBaseVertex != UINT32_MAX) ? megaVertexBufferAddr : dynamicVertexBufferAddr;
    vkCmdPushConstants(cmd, context.pipelineLayoutTextured3D,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pushConstants);

    uint32_t first_vertex = (mesh->megaBaseVertex != UINT32_MAX) ? mesh->megaBaseVertex : (context.megaVertexBufferOffset + (frame_index * MAX_DYNAMIC_VERTICES) + mesh->dynamicBaseVertex);
    vkCmdDraw(cmd, mesh->vertexCount, 1, first_vertex, 0);
}

void mesh_update_morph(Mesh* mesh) {
    (void)mesh;
    /* Intentionally empty: weight uploads are now batched in flushMeshSSBO
       via a single contiguous memcpy of the entire weight region.
       AABB expansion is handled by the GPU refit compute pass.            */
}

void mesh_destroy(VkDevice device, Mesh* mesh) {
    (void)device;
    if (mesh->morph_data) {
        if (mesh->morph_data->weights) free(mesh->morph_data->weights);
        free(mesh->morph_data);
        mesh->morph_data = NULL;
    }
    if (mesh->name) { free(mesh->name); mesh->name = NULL; }
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

    /* sets 0/1/2 already bound in recordCommandBuffer before meshes_draw is called */

    for (size_t i = 0; i < meshes->count; ++i) {
        Mesh* m = &meshes->items[i];

        /* Static meshes (in mega buffer) are drawn by the indirect pass —
           only draw dynamic meshes (morph targets) here.                 */
        if (m->megaBaseVertex != UINT32_MAX) continue;

        VkPipeline       want_pipe   = context.graphicsPipeline;
        VkPipelineLayout want_layout = context.pipelineLayout;

        if (want_pipe != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want_pipe);
            bound_pipeline = want_pipe;
        }

        pushConstants.vertexBufferAddr = (m->megaBaseVertex != UINT32_MAX) ? megaVertexBufferAddr : dynamicVertexBufferAddr;
        vkCmdPushConstants(cmd, want_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &pushConstants);

        uint32_t first_vertex = (m->megaBaseVertex != UINT32_MAX) ? m->megaBaseVertex : (context.megaVertexBufferOffset + (frame_index * MAX_DYNAMIC_VERTICES) + m->dynamicBaseVertex);
        vkCmdDraw(cmd, m->vertexCount, 1, first_vertex, 0);
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

    /* We pass 'color' to the unused border param so C doesn't complain about uninitialized fields */
    Vertex2D quad[6] = {
        {{x,     y    }, color, {0.0f, 0.0f}, -1, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{x + w, y    }, color, {1.0f, 0.0f}, -1, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{x + w, y + h}, color, {1.0f, 1.0f}, -1, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{x,     y    }, color, {0.0f, 0.0f}, -1, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{x + w, y + h}, color, {1.0f, 1.0f}, -1, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{x,     y + h}, color, {0.0f, 1.0f}, -1, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color}
    };

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void shaderQuad2D(vec2 position, vec2 size, int shaderId, vec4 customParams) {
    if (vertexCount2D + 6 > MAX_VERTICES) return;
    float x = position[0], y = position[1];
    float w = size[0], h = size[1];
    Color col = {1.0f, 1.0f, 1.0f, 1.0f}; // Default color
    Vertex2D quad[6] = {
        {{x,     y    }, col, {0.0f, 0.0f}, shaderId, {w, h}, {customParams[0], customParams[1], customParams[2], customParams[3]}, 0.0f, col},
        {{x + w, y    }, col, {1.0f, 0.0f}, shaderId, {w, h}, {customParams[0], customParams[1], customParams[2], customParams[3]}, 0.0f, col},
        {{x + w, y + h}, col, {1.0f, 1.0f}, shaderId, {w, h}, {customParams[0], customParams[1], customParams[2], customParams[3]}, 0.0f, col},
        {{x,     y    }, col, {0.0f, 0.0f}, shaderId, {w, h}, {customParams[0], customParams[1], customParams[2], customParams[3]}, 0.0f, col},
        {{x + w, y + h}, col, {1.0f, 1.0f}, shaderId, {w, h}, {customParams[0], customParams[1], customParams[2], customParams[3]}, 0.0f, col},
        {{x,     y + h}, col, {0.0f, 1.0f}, shaderId, {w, h}, {customParams[0], customParams[1], customParams[2], customParams[3]}, 0.0f, col}
    };
    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void exQuad2D(vec2 position, vec2 size, vec4 radii, float borderThickness, Color borderColor, Color color) {
    if (vertexCount2D + 6 > MAX_VERTICES) return;

    float x = position[0], y = position[1];
    float w = size[0], h = size[1];

    Vertex2D quad[6] = {
        {{x,     y    }, color, {0.0f, 0.0f}, -2, {w, h}, {radii[0], radii[1], radii[2], radii[3]}, borderThickness, borderColor},
        {{x + w, y    }, color, {1.0f, 0.0f}, -2, {w, h}, {radii[0], radii[1], radii[2], radii[3]}, borderThickness, borderColor},
        {{x + w, y + h}, color, {1.0f, 1.0f}, -2, {w, h}, {radii[0], radii[1], radii[2], radii[3]}, borderThickness, borderColor},
        {{x,     y    }, color, {0.0f, 0.0f}, -2, {w, h}, {radii[0], radii[1], radii[2], radii[3]}, borderThickness, borderColor},
        {{x + w, y + h}, color, {1.0f, 1.0f}, -2, {w, h}, {radii[0], radii[1], radii[2], radii[3]}, borderThickness, borderColor},
        {{x,     y + h}, color, {0.0f, 1.0f}, -2, {w, h}, {radii[0], radii[1], radii[2], radii[3]}, borderThickness, borderColor}
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
        {{x,     y    }, tint, {0.0f, 1.0f}, slot, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, tint},
        {{x + w, y    }, tint, {1.0f, 1.0f}, slot, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, tint},
        {{x + w, y + h}, tint, {1.0f, 0.0f}, slot, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, tint},
        {{x,     y    }, tint, {0.0f, 1.0f}, slot, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, tint},
        {{x + w, y + h}, tint, {1.0f, 0.0f}, slot, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, tint},
        {{x,     y + h}, tint, {0.0f, 0.0f}, slot, {w, h}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, tint}
    };

memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void circle2D(vec2 center, float radius, Color color) {
    float d = radius * 2.0f;
    vec2 pos = {center[0] - radius, center[1] - radius};
    vec4 radii = {radius, radius, radius, radius};
    exQuad2D(pos, (vec2){d, d}, radii, 0.0f, color, color);
}

void line2D(vec2 start, vec2 end, Color color) {
    float thickness = 1.0f;
    vec2 dir = {end[0] - start[0], end[1] - start[1]};
    float len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1]);
    if (len < 0.0001f) return;
    dir[0] /= len; dir[1] /= len;

    vec2 perp = {-dir[1] * (thickness * 0.5f), dir[0] * (thickness * 0.5f)};

    if (vertexCount2D + 6 > MAX_VERTICES) return;

    Vertex2D quad[6] = {
        {{start[0] - perp[0], start[1] - perp[1]}, color, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{end[0] - perp[0], end[1] - perp[1]}, color, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{end[0] + perp[0], end[1] + perp[1]}, color, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{start[0] - perp[0], start[1] - perp[1]}, color, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{end[0] + perp[0], end[1] + perp[1]}, color, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color},
        {{start[0] + perp[0], start[1] + perp[1]}, color, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, color}
    };
    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void triangle_col(vec2 p0, Color c0, vec2 p1, Color c1, vec2 p2, Color c2) {
    if (vertexCount2D + 3 > MAX_VERTICES) return;
    Vertex2D tri[3] = {
        {{p0[0], p0[1]}, c0, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, c0},
        {{p1[0], p1[1]}, c1, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, c1},
        {{p2[0], p2[1]}, c2, {0.0f, 0.0f}, -1, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, c2}
    };
    memcpy(&vertices2D[vertexCount2D], tri, sizeof(tri));
    vertexCount2D += 3;
}

void renderer2D_upload() {
    if (vertexCount2D == 0) return;
    memcpy(context.vertexBuffer2DMapped[frame_index], vertices2D, vertexCount2D * sizeof(Vertex2D));
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
    vkCmdBindVertexBuffers(cmd, 0, 1, &context.vertexBuffer2D[frame_index], offsets);
    vkCmdDraw(cmd, vertexCount2D, 1, 0, 0);
}

void renderer2D_clear(void) {
    vertexCount2D = 0;
}

// --- Texture Loading ---


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

        alloc_texture_image(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, &texture->image, &texture->memory);
        upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED);

        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = texture->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        vkCreateImageView(context->device, &viewInfo, NULL, &texture->view);

        VkDescriptorImageInfo imageDescInfo = { .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .imageView = texture->view, .sampler = texture->sampler };
        VkWriteDescriptorSet descriptorWrite = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = texture->descriptorSet, .dstBinding = 0, .dstArrayElement = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .pImageInfo = &imageDescInfo };
        vkUpdateDescriptorSets(context->device, 1, &descriptorWrite, 0, NULL);

        texture->width = width; texture->height = height;
        return true;
    }
    return upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        .anisotropyEnable = VK_TRUE, .maxAnisotropy = 16.0f, // AAA Texture Sharpness
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
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    bool ok = alloc_texture_image(ctx, w, h, fmt, &texture->image, &texture->memory) &&
              upload_pixels_to_image(ctx, pixels, (VkDeviceSize)w*h*4, texture->image, w, h, fmt, VK_IMAGE_LAYOUT_UNDEFINED) &&
              finalize_texture(ctx, texture, fmt, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    // Removed the rogue stbi_image_free(pixels) here! Memory ownership stays with the caller.
    if (ok) { texture->width = w; texture->height = h; texture->loaded = true; }
    return ok;
}

bool load_texture_from_memory(VulkanContext* ctx, unsigned char* data, size_t data_size, Texture2D* texture) {
    int w, h, ch;
    stbi_uc* pixels = stbi_load_from_memory(data, (int)data_size, &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
        fprintf(stderr, "Failed to decode texture from memory\n");
        return false;
    }
    bool res = load_texture_from_pixels(ctx, pixels, w, h, texture);
    stbi_image_free(pixels);
    return res;
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

    if (strstr(filename, ".exr")) {
        bool is_normal = (strstr(filename, "nor") != NULL);
        // Do not pack roughness here, that is handled explicitly by texture_pool_load_roughness_to_gltf
        float* floatPixels = load_exr_as_float(filename, &w, &h, is_normal, false);
        if (!floatPixels) return false;
        bool res = load_texture_from_float_pixels(ctx, floatPixels, w, h, texture);
        free(floatPixels);
        return res;
    }

    stbi_uc* pixels = stbi_load(filename, &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
        fprintf(stderr, "[WARNING] Failed to load texture: %s\n", filename);
        return false;
    }
    bool res = load_texture_from_pixels(ctx, pixels, w, h, texture);
    stbi_image_free(pixels);
    return res;
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
}

/// LINE

// --- Line Renderer ---
typedef struct {
    uint32_t first;
    uint32_t count;
    float width;
} LineBatch;
static LineBatch      lineBatches[128];
static uint32_t       lineBatchCount = 0;
static float          currentLineWidth = 2.0f;

static Vertex         lineVertices[MAX_VERTICES];
uint32_t              lineVertexCount = 0;
static VkBuffer       lineVertexBuffer[MAX_FRAMES_IN_FLIGHT];
static VkDeviceMemory lineVertexBufferMemory[MAX_FRAMES_IN_FLIGHT];
static void* lineVertexBufferMapped[MAX_FRAMES_IN_FLIGHT];
static uint64_t       lineVertexBufferAddr[MAX_FRAMES_IN_FLIGHT];

void line_renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        create_mapped_buffer(device, physicalDevice, sizeof(lineVertices),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             &lineVertexBuffer[i], &lineVertexBufferMemory[i], &lineVertexBufferMapped[i]);
        VkBufferDeviceAddressInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = lineVertexBuffer[i] };
        lineVertexBufferAddr[i] = vkGetBufferDeviceAddress(device, &info);
    }
}

void line(vec3 start, vec3 end, Color color) {
    if (lineVertexCount + 2 >= MAX_VERTICES) return;

    if (lineBatchCount == 0 || fabsf(lineBatches[lineBatchCount - 1].width - currentLineWidth) > 0.01f) {
        if (lineBatchCount >= 128) return; // Safeguard
        lineBatches[lineBatchCount].first = lineVertexCount;
        lineBatches[lineBatchCount].count = 0;
        lineBatches[lineBatchCount].width = currentLineWidth;
        lineBatchCount++;
    }

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

    lineBatches[lineBatchCount - 1].count += 2;
}

void line_set_width(float width) {
    currentLineWidth = width;
}

void line_renderer_upload() {
    if (lineVertexCount == 0) return;
    memcpy(lineVertexBufferMapped[frame_index], lineVertices, lineVertexCount * sizeof(Vertex));
}

void line_renderer_draw(VkCommandBuffer cmd) {
    if (lineVertexCount == 0) return;

    // Lines must allocate a dedicated unlit slot so they don't inherit transforms/materials
    // from gl_DrawID 0 (which would be the first mesh in the scene)
    Material oldMat = currentMaterial;
    Material lineMat;
    memset(&lineMat, 0, sizeof(Material));
    lineMat.baseColorFactor[0] = 1.0f;
    lineMat.baseColorFactor[1] = 1.0f;
    lineMat.baseColorFactor[2] = 1.0f;
    lineMat.baseColorFactor[3] = 1.0f;
    lineMat.isUnlit = 1;
    lineMat.alphaMode = 2;
    lineMat.albedoIndex = -1;
    lineMat.normalMapIndex = -1;
    lineMat.metallicRoughIndex = -1;
    lineMat.aoIndex = -1;
    lineMat.emissiveIndex = -1;
    set_material(&lineMat);

    mat4 identity;
    glm_mat4_identity(identity);
    int slot = alloc_slot(identity);

    // Write a dummy indirect command with 0 instances so the compute culler safely ignores it
    VkDrawIndexedIndirectCommand* cmds = (VkDrawIndexedIndirectCommand*)context.srcIndirectBufferMapped;
    cmds[slot].indexCount = 0;
    cmds[slot].instanceCount = 0;
    cmds[slot].firstIndex = 0;
    cmds[slot].vertexOffset = 0;
    cmds[slot].firstInstance = 0;

    dynamic_draw_count++;
    context.indirectDrawCount = (uint32_t)scene.meshes.count + dynamic_draw_count;

    set_material(&oldMat);

    extern uint64_t jointSSBOAddr[MAX_FRAMES_IN_FLIGHT];
    extern uint64_t megaMorphBufferAddr;
    extern uint64_t morphWeightAddr[MAX_FRAMES_IN_FLIGHT];
    pushConstants.meshIndex        = slot;
    pushConstants.vertexBufferAddr = lineVertexBufferAddr[frame_index];
    pushConstants.jointBufferAddr  = jointSSBOAddr[frame_index];
    pushConstants.morphBufferAddr  = megaMorphBufferAddr;
    pushConstants.morphWeightAddr  = morphWeightAddr[frame_index];
    vkCmdPushConstants(cmd, context.pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pushConstants);

    // Loop through batches and explicitly set the dynamic line width per batch!
    for (uint32_t i = 0; i < lineBatchCount; i++) {
        vkCmdSetLineWidth(cmd, lineBatches[i].width);
        vkCmdDraw(cmd, lineBatches[i].count, 1, lineBatches[i].first, 0);
    }
}

void line_renderer_clear() {
    lineVertexCount = 0;
    lineBatchCount = 0;
    currentLineWidth = 2.0f;
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

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
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

#define DDS_MAGIC 0x20534444
typedef struct {
    uint32_t dwSize; uint32_t dwFlags; uint32_t dwHeight; uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize; uint32_t dwDepth; uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    struct { uint32_t dwSize; uint32_t dwFlags; uint32_t dwFourCC; uint32_t dwRGBBitCount;
             uint32_t dwRBitMask; uint32_t dwGBitMask; uint32_t dwBBitMask; uint32_t dwABitMask; } ddspf;
    uint32_t dwCaps; uint32_t dwCaps2; uint32_t dwCaps3; uint32_t dwCaps4; uint32_t dwReserved2;
} DDS_HEADER;
typedef struct { uint32_t dxgiFormat; uint32_t resourceDimension; uint32_t miscFlag; uint32_t arraySize; uint32_t miscFlags2; } DDS_HEADER_DXT10;

// --- OBSIDIAN MEMORY MANAGER & ASYNC QUEUE ---
#define MAX_TEXTURE_PAGES 64 // 16GB max pool
typedef struct { VkDeviceMemory memory; VkDeviceSize size; VkDeviceSize offset; uint32_t active_allocations; } TexturePage;
static TexturePage texture_pages[MAX_TEXTURE_PAGES];
static uint32_t texture_page_count = 0;

static bool alloc_texture_vram(VulkanContext* ctx, VkMemoryRequirements req, VkDeviceMemory* out_mem, VkDeviceSize* out_offset) {
    VkDeviceSize alignment = req.alignment; // Trust driver alignment entirely
    for (uint32_t i = 0; i < texture_page_count; i++) {
        VkDeviceSize aligned_offset = (texture_pages[i].offset + alignment - 1) & ~(alignment - 1);
        if (aligned_offset + req.size <= texture_pages[i].size) {
            *out_mem = texture_pages[i].memory; *out_offset = aligned_offset;
            texture_pages[i].offset = aligned_offset + req.size;
            texture_pages[i].active_allocations++;
            return true;
        }
    }
    if (texture_page_count >= MAX_TEXTURE_PAGES) {
        fprintf(stderr, "\033[31m[OMM] CRITICAL: Max texture pages reached!\033[0m\n");
        return false;
    }
    VkDeviceSize page_size = 256 * 1024 * 1024;
    if (req.size > page_size) page_size = (req.size + alignment - 1) & ~(alignment - 1);
    uint32_t memType = findMemoryType(ctx->physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocInfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = page_size, .memoryTypeIndex = memType };
    VkDeviceMemory new_mem;
    if (vkAllocateMemory(ctx->device, &allocInfo, NULL, &new_mem) != VK_SUCCESS) return false;

    texture_pages[texture_page_count] = (TexturePage){ .memory = new_mem, .size = page_size, .offset = req.size, .active_allocations = 1 };
    *out_mem = new_mem; *out_offset = 0;
    texture_page_count++;
    fprintf(stdout, "\033[35m[OMM] Allocated %llu MB VRAM Texture Block (Page %u)\033[0m\n", page_size / (1024*1024), texture_page_count);
    return true;
}

static void free_texture_vram(VkDeviceMemory mem) {
    for (uint32_t i = 0; i < texture_page_count; i++) {
        if (texture_pages[i].memory == mem) {
            if (texture_pages[i].active_allocations > 0) {
                texture_pages[i].active_allocations--;
                if (texture_pages[i].active_allocations == 0) {
                    texture_pages[i].offset = 0; // Compacting reset!
                    fprintf(stdout, "\033[35m[OMM] Page %u compacted (0 active allocations).\033[0m\n", i);
                }
            }
            return;
        }
    }
}

typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkFence fence; VkCommandBuffer cmd; bool* loaded_flag; } AsyncUpload;
static AsyncUpload* async_uploads = NULL;
static uint32_t async_upload_count = 0;
static uint32_t async_upload_capacity = 0;

static VkCommandPool async_cmd_pool = VK_NULL_HANDLE;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkCommandBuffer cmd; bool* loaded_flag; } PendingUpload;
static PendingUpload pending_uploads[8192];
static uint32_t pending_upload_count = 0;
static pthread_mutex_t upload_mutex = PTHREAD_MUTEX_INITIALIZER;

static VkCommandBuffer begin_async_cmd(VulkanContext* ctx) {
    pthread_mutex_lock(&upload_mutex);
    if (async_cmd_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo poolInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = ctx->graphicsQueueFamily
        };
        vkCreateCommandPool(ctx->device, &poolInfo, NULL, &async_cmd_pool);
    }
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = async_cmd_pool,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(ctx->device, &allocInfo, &cmd);
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

static void end_async_cmd(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    pthread_mutex_unlock(&upload_mutex);
}

static void submit_async_upload(VulkanContext* ctx, VkBuffer buf, VkDeviceMemory mem, VkCommandBuffer cmd, bool* loaded_flag) {
    pthread_mutex_lock(&upload_mutex);
    if (pending_upload_count < 8192) {
        pending_uploads[pending_upload_count++] = (PendingUpload){ buf, mem, cmd, loaded_flag };
    } else {
        fprintf(stderr, "\033[31m[CRITICAL] Pending uploads overflow! Skipping upload.\033[0m\n");
    }
    pthread_mutex_unlock(&upload_mutex);
}

void pump_async_uploads(VulkanContext* ctx) {
    for (uint32_t i = 0; i < async_upload_count; i++) {
        if (vkGetFenceStatus(ctx->device, async_uploads[i].fence) == VK_SUCCESS) {
            vkDestroyBuffer(ctx->device, async_uploads[i].buffer, NULL);
            vkFreeMemory(ctx->device, async_uploads[i].memory, NULL);
            pthread_mutex_lock(&upload_mutex);
            vkFreeCommandBuffers(ctx->device, async_cmd_pool, 1, &async_uploads[i].cmd);
            pthread_mutex_unlock(&upload_mutex);
            vkDestroyFence(ctx->device, async_uploads[i].fence, NULL);
            if (async_uploads[i].loaded_flag) *async_uploads[i].loaded_flag = true;
            async_uploads[i] = async_uploads[--async_upload_count]; i--;
        }
    }
    pthread_mutex_lock(&upload_mutex);
    for (uint32_t i = 0; i < pending_upload_count; i++) {
        VkSubmitInfo submitInfo = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &pending_uploads[i].cmd };
        VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence; vkCreateFence(ctx->device, &fenceInfo, NULL, &fence);
        vkQueueSubmit(ctx->graphicsQueue, 1, &submitInfo, fence);

        if (async_upload_count == async_upload_capacity) {
            uint32_t new_cap = async_upload_capacity == 0 ? 128 : async_upload_capacity * 2;
            AsyncUpload* new_arr = realloc(async_uploads, new_cap * sizeof(AsyncUpload));
            if (new_arr) {
                async_uploads = new_arr;
                async_upload_capacity = new_cap;
            } else {
                fprintf(stderr, "\033[31m[CRITICAL] Failed to allocate memory for async uploads!\033[0m\n");
                continue;
            }
        }
        async_uploads[async_upload_count++] = (AsyncUpload){ pending_uploads[i].buffer, pending_uploads[i].memory, fence, pending_uploads[i].cmd, pending_uploads[i].loaded_flag };
    }
    pending_upload_count = 0;
    pthread_mutex_unlock(&upload_mutex);
}

// --- BACKGROUND TEXTURE COOKER ---
typedef struct {
    char filepath[512];
    int32_t pool_index;
    bool is_roughness;
} CookerJob;

#define MAX_COOKER_JOBS 8192
static CookerJob cooker_queue[MAX_COOKER_JOBS];
static volatile uint32_t cooker_head = 0;
static volatile uint32_t cooker_tail = 0;
static pthread_mutex_t cooker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cooker_cond = PTHREAD_COND_INITIALIZER;
static pthread_t cooker_threads[8];
static bool cooker_running = true;

// Forward declarations for cooker thread
static float* load_exr_as_float(const char* path, int* out_w, int* out_h, bool is_normal, bool is_roughness);
static const char* get_texture_cache_path(const char* original_path, bool is_roughness);
static bool load_texture_dds(VulkanContext* ctx, const char* filepath, Texture2D* texture);

typedef struct {
    uint32_t magic; // 0x5845544F 'OTEX'
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t data_size;
} OtexHeader;

// --- Texture Pool Management ---
static Texture2D texturePool[MAX_TEXTURES];
static char texturePoolPaths[MAX_TEXTURES][512];
static bool texturePoolIsRoughness[MAX_TEXTURES];
static uint32_t textureCount = 0;
static Texture2D dummyWhiteTexture;

static void* cooker_worker_thread(void* arg) {
    (void)arg;
    while (cooker_running) {
        pthread_mutex_lock(&cooker_mutex);
        while (cooker_head == cooker_tail && cooker_running) {
            pthread_cond_wait(&cooker_cond, &cooker_mutex);
        }
        if (!cooker_running) { pthread_mutex_unlock(&cooker_mutex); break; }

        CookerJob job = cooker_queue[cooker_head];
        cooker_head = (cooker_head + 1) % MAX_COOKER_JOBS;
        pthread_mutex_unlock(&cooker_mutex);

        Texture2D* tex = &texturePool[job.pool_index];
        tex->status = TEXTURE_STATUS_COOKING;

        fprintf(stdout, "\033[35m[BACKGROUND COOKER] Compiling %s...\033[0m\n", job.filepath);

        const char* cache_path = get_texture_cache_path(job.filepath, job.is_roughness);
        int w, h, ch;

        if (job.is_roughness) {
            if (strstr(job.filepath, ".exr")) {
                float* floatPixels = load_exr_as_float(job.filepath, &w, &h, false, true);
                if (floatPixels) {
                    FILE* fout = fopen(cache_path, "wb");
                    if (fout) {
                        OtexHeader header = { 0x5845544F, (uint32_t)w, (uint32_t)h, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(w * h * 16) };
                        fwrite(&header, sizeof(OtexHeader), 1, fout);
                        fwrite(floatPixels, 1, header.data_size, fout);
                        fclose(fout);
                    }
                    free(floatPixels);
                    tex->status = TEXTURE_STATUS_READY_FOR_UPLOAD;
                } else { tex->status = TEXTURE_STATUS_FAILED; }
            } else {
                stbi_uc* roughPixels = stbi_load(job.filepath, &w, &h, &ch, 1);
                if (roughPixels) {
                    size_t size = (size_t)w * h * 4;
                    stbi_uc* packed = malloc(size);
                    for (int i = 0; i < w * h; i++) {
                        packed[i*4 + 0] = 255;             // AO default
                        packed[i*4 + 1] = roughPixels[i];  // Roughness correctly packed in G!
                        packed[i*4 + 2] = 0;               // Metallic default
                        packed[i*4 + 3] = 255;
                    }
                    free(roughPixels);
                    FILE* fout = fopen(cache_path, "wb");
                    if (fout) {
                        OtexHeader header = { 0x5845544F, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)(size) };
                        fwrite(&header, sizeof(OtexHeader), 1, fout);
                        fwrite(packed, 1, header.data_size, fout);
                        fclose(fout);
                    }
                    free(packed);
                    tex->status = TEXTURE_STATUS_READY_FOR_UPLOAD;
                } else { tex->status = TEXTURE_STATUS_FAILED; }
            }
        } else {
            // Standard Albedo/Normal/etc JIT to DDS
            if (strstr(job.filepath, ".exr")) {
                bool is_normal = (strstr(job.filepath, "nor") != NULL);
                float* float_pixels = load_exr_as_float(job.filepath, &w, &h, is_normal, false);
                if (float_pixels) {
                    FILE* fout = fopen(cache_path, "wb");
                    if (fout) {
                        OtexHeader header = { 0x5845544F, (uint32_t)w, (uint32_t)h, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(w * h * 16) };
                        fwrite(&header, sizeof(OtexHeader), 1, fout);
                        fwrite(float_pixels, 1, header.data_size, fout);
                        fclose(fout);
                    }
                    free(float_pixels);
                    tex->status = TEXTURE_STATUS_READY_FOR_UPLOAD;
                } else { tex->status = TEXTURE_STATUS_FAILED; }
            } else {
                unsigned char* pixels = stbi_load(job.filepath, &w, &h, &ch, STBI_rgb_alpha);
                if (pixels) {
                    uint32_t num_blocks_x = (w + 3) / 4;
                    uint32_t num_blocks_y = (h + 3) / 4;
                    uint32_t dxt_size = num_blocks_x * num_blocks_y * 16;
                    unsigned char* dxt_data = malloc(dxt_size);

                    for (uint32_t by = 0; by < num_blocks_y; by++) {
                        for (uint32_t bx = 0; bx < num_blocks_x; bx++) {
                            unsigned char block[64];
                            for (uint32_t y = 0; y < 4; y++) {
                                for (uint32_t x = 0; x < 4; x++) {
                                    uint32_t px = (bx * 4) + x;
                                    uint32_t py = (by * 4) + y;
                                    uint32_t idx = ((py < (uint32_t)h ? py : (uint32_t)h - 1) * w + (px < (uint32_t)w ? px : (uint32_t)w - 1)) * 4;
                                    memcpy(&block[(y * 4 + x) * 4], &pixels[idx], 4);
                                }
                            }
                            stb_compress_dxt_block(&dxt_data[(by * num_blocks_x + bx) * 16], block, 1, STB_DXT_NORMAL);
                        }
                    }

                    char out_dds_path[512];
                    strncpy(out_dds_path, job.filepath, sizeof(out_dds_path) - 1);
                    char* out_ext = strrchr(out_dds_path, '.');
                    if (out_ext) strcpy(out_ext, ".dds");
                    const char* out_dds_cache = get_texture_cache_path(out_dds_path, false);

                    FILE* fdds = fopen(out_dds_cache, "wb");
                    if (fdds) {
                        uint32_t magic = DDS_MAGIC;
                        DDS_HEADER header = {0};
                        header.dwSize = 124;
                        header.dwFlags = 0x1007 | 0x80000;
                        header.dwHeight = h;
                        header.dwWidth = w;
                        header.dwPitchOrLinearSize = dxt_size;
                        header.dwMipMapCount = 1;
                        header.ddspf.dwSize = 32;
                        header.ddspf.dwFlags = 0x4;
                        header.ddspf.dwFourCC = 0x35545844;
                        header.dwCaps = 0x1000;

                        fwrite(&magic, 4, 1, fdds);
                        fwrite(&header, sizeof(DDS_HEADER), 1, fdds);
                        fwrite(dxt_data, 1, dxt_size, fdds);
                        fclose(fdds);
                    }
                    free(dxt_data);
                    stbi_image_free(pixels);
                    tex->status = TEXTURE_STATUS_READY_FOR_UPLOAD;
                } else { tex->status = TEXTURE_STATUS_FAILED; }
            }
        }
    }
    return NULL;
}

void texture_pool_init() {
    textureCount = 0;
    memset(texturePool, 0, sizeof(texturePool));

    // Generate Dummy 1x1 White Texture Fallback
    unsigned char white_pixel[4] = {255, 255, 255, 255};
    dummyWhiteTexture.loaded = false;
    load_texture_from_rgba(&context, white_pixel, 1, 1, &dummyWhiteTexture);
    dummyWhiteTexture.status = TEXTURE_STATUS_READY;

    // Launch 8 Background Cooker Threads
    cooker_running = true;
    for(int i = 0; i < 8; i++) {
        pthread_create(&cooker_threads[i], NULL, cooker_worker_thread, NULL);
    }
}

void texture_pool_cleanup(VulkanContext* context) {
    cooker_running = false;
    pthread_cond_broadcast(&cooker_cond);
    for(int i = 0; i < 8; i++) {
        if (cooker_threads[i] != 0) {
            pthread_join(cooker_threads[i], NULL);
            cooker_threads[i] = 0;
        }
    }

    pump_async_uploads(context);
    for (uint32_t i = 0; i < async_upload_count; i++) {
        vkWaitForFences(context->device, 1, &async_uploads[i].fence, VK_TRUE, UINT64_MAX);
    }
    pump_async_uploads(context);

    destroy_texture(context, &dummyWhiteTexture);

    for (uint32_t i = 0; i < textureCount; i++) {
        destroy_texture(context, &texturePool[i]);
    }
    textureCount = 0;

    for (uint32_t i = 0; i < texture_page_count; i++) {
        if (texture_pages[i].memory) {
            vkFreeMemory(context->device, texture_pages[i].memory, NULL);
            texture_pages[i].memory = VK_NULL_HANDLE;
        }
    }
    texture_page_count = 0;

    if (async_uploads) { free(async_uploads); async_uploads = NULL; }
    async_upload_capacity = 0;
    async_upload_count = 0;

    if (async_cmd_pool) {
        vkDestroyCommandPool(context->device, async_cmd_pool, NULL);
        async_cmd_pool = VK_NULL_HANDLE;
    }
}

int32_t texture_pool_add(VulkanContext* context, const char* filename) {
    if (textureCount >= MAX_TEXTURES) {
        fprintf(stderr, "Texture pool full! Cannot load %s\n", filename);
        return -1;
    }

    printf("Loading texture %u: %s\n", textureCount, filename);

    if (load_texture(context, filename, &texturePool[textureCount])) {
        printf("  -> Successfully queued texture #%u\n", textureCount);
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

bool scene_topology_dirty = true;
static bool indirect_buffer_dirty[MAX_FRAMES_IN_FLIGHT] = {true, true};
static vec3 last_camera_pos = { -1e7f, -1e7f, -1e7f };
static float* mesh_distances = NULL;
static size_t mesh_distances_capacity = 0;

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
static bool upload_pixels_to_image(VulkanContext* ctx, unsigned char* pixels, VkDeviceSize imageSize, VkImage image, uint32_t w, uint32_t h, VkFormat fmt, VkImageLayout srcLayout, bool* loaded_flag);
static bool finalize_texture(VulkanContext* ctx, Texture2D* texture, VkFormat fmt, VkSamplerAddressMode addrMode);

static bool load_texture_from_float_pixels(VulkanContext* ctx, float* pixels, int w, int h, Texture2D* texture) {
    if (!pixels) return false;
    VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    bool ok = alloc_texture_image(ctx, w, h, fmt, &texture->image, &texture->memory) &&
              upload_pixels_to_image(ctx, (unsigned char*)pixels, (VkDeviceSize)w*h*16, texture->image, w, h, fmt, VK_IMAGE_LAYOUT_UNDEFINED, &texture->loaded) &&
              finalize_texture(ctx, texture, fmt, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    if (ok) { texture->width = w; texture->height = h; }
    return ok;
}

static const char* get_texture_cache_path(const char* original_path, bool is_roughness) {
    static char cache_path[512];
    const char* home = getenv("HOME");
    if (!home) home = ".";
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.cache", home); mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home); mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/textures", home); mkdir(dir_path, 0777);

    char safe_name[256];
    strncpy(safe_name, original_path, sizeof(safe_name) - 1);
    safe_name[sizeof(safe_name) - 1] = '\0';
    for (int i = 0; safe_name[i]; i++) {
        if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.') safe_name[i] = '_';
    }
    snprintf(cache_path, sizeof(cache_path), "%s/%s%s.otex", dir_path, safe_name, is_roughness ? "_rough" : "");
    return cache_path;
}

// Forward declaration
static bool upload_file_to_image_direct(VulkanContext *ctx, const char *filepath,
                                        VkDeviceSize imageSize, VkImage image,
                                        uint32_t w, uint32_t h, VkFormat fmt,
                                        size_t file_offset, bool* loaded_flag);

static int32_t texture_pool_load_roughness_to_gltf(VulkanContext* ctx, const char* roughPath) {
    if (!roughPath) return -1;

    const char* cache_path = get_texture_cache_path(roughPath, true);
    FILE* f = fopen(cache_path, "rb");
    if (f) {
        OtexHeader header;
        if (fread(&header, sizeof(OtexHeader), 1, f) == 1 && header.magic == 0x5845544F) {
            fclose(f);
            fprintf(stdout, "\033[32m[TEXTURE] Cache Hit Roughness (ZERO-COPY): %s\033[0m\n", cache_path);
            if (textureCount >= MAX_TEXTURES) return -1;
            Texture2D* texture = &texturePool[textureCount];
            if (alloc_texture_image(ctx, header.width, header.height, header.format, &texture->image, &texture->memory)) {
                if (upload_file_to_image_direct(ctx, cache_path, header.data_size, texture->image, header.width, header.height, header.format, sizeof(OtexHeader), &texture->loaded)) {
                    if (finalize_texture(ctx, texture, header.format, VK_SAMPLER_ADDRESS_MODE_REPEAT)) {
                        texture->width = header.width; texture->height = header.height;
                        texture->status = TEXTURE_STATUS_READY;
                        return textureCount++;
                    }
                }
            }
        } else {
            fclose(f);
        }
    }

    fprintf(stdout, "\033[33m[TEXTURE] Cache Miss Roughness. Queueing Background Cook: %s\033[0m\n", roughPath);
    if (textureCount >= MAX_TEXTURES) return -1;

    int32_t tIdx = textureCount++;
    Texture2D* texture = &texturePool[tIdx];

    // AAA Instant metadata parsing for Editor UI sizing
    int w = 1, h = 1, ch;
    if (stbi_info(roughPath, &w, &h, &ch)) {
        texture->width = w;
        texture->height = h;
    } else {
        texture->width = 1;
        texture->height = 1;
    }

    texture->status = TEXTURE_STATUS_QUEUED_FOR_COOKING;

    // Pre-allocate bindless slot to eliminate gray flickering
    if (texture->bindlessSlot == 0) {
        texture->bindlessSlot = ctx->bindlessTextureCount++;
        bindlessRegisterTexture(ctx, texture->bindlessSlot, dummyWhiteTexture.view, dummyWhiteTexture.sampler);
    }

    strncpy(texturePoolPaths[tIdx], roughPath, 511);
    texturePoolIsRoughness[tIdx] = true;

    pthread_mutex_lock(&cooker_mutex);
    uint32_t next = (cooker_tail + 1) % MAX_COOKER_JOBS;
    if (next != cooker_head) {
        strncpy(cooker_queue[cooker_tail].filepath, roughPath, 511);
        cooker_queue[cooker_tail].pool_index = tIdx;
        cooker_queue[cooker_tail].is_roughness = true;
        cooker_tail = next;
        pthread_cond_signal(&cooker_cond);
    }
    pthread_mutex_unlock(&cooker_mutex);

    return tIdx;
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

typedef struct {
    uint32_t magic; // 0x54414D4F 'OMAT'
    char albedoPath[512];
    char normalPath[512];
    char roughPath[512];
    char aoPath[512];
    char dispPath[512];
    float displacementScale;
} OmatCache;

static const char* get_material_cache_path(const char* dirPath) {
    static char cache_path[512];
    const char* home = getenv("HOME");
    if (!home) home = ".";
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.cache", home); mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home); mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/materials", home); mkdir(dir_path, 0777);

    char safe_name[256];
    strncpy(safe_name, dirPath, sizeof(safe_name) - 1);
    safe_name[sizeof(safe_name) - 1] = '\0';
    for (int i = 0; safe_name[i]; i++) {
        if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.') safe_name[i] = '_';
    }
    snprintf(cache_path, sizeof(cache_path), "%s/%s.omat", dir_path, safe_name);
    return cache_path;
}

Material load_pbr_material_dir(const char* dirPath) {
    printf("\n[PBR SCAN] ==========================================\n");

    const char* cache_path = get_material_cache_path(dirPath);
    FILE* f = fopen(cache_path, "rb");
    OmatCache cache = {0};
    bool cache_hit = false;

    if (f) {
        if (fread(&cache, sizeof(OmatCache), 1, f) == 1 && cache.magic == 0x54414D4F) {
            fprintf(stdout, "\033[32m[MATERIAL] Cache Hit (OMAT Manifest): %s\033[0m\n", cache_path);
            cache_hit = true;
        }
        fclose(f);
    }

    if (!cache_hit) {
        fprintf(stdout, "\033[33m[MATERIAL] Cache Miss. Scanning directory (OS Syscalls): %s\033[0m\n", dirPath);
        cache.magic = 0x54414D4F;
        cache.displacementScale = 0.5f;

        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir(dirPath)) != NULL) {
            while ((ent = readdir(dir)) != NULL) {
                if (strstr(ent->d_name, "diff") || strstr(ent->d_name, "albedo") || strstr(ent->d_name, "basecolor")) {
                    snprintf(cache.albedoPath, sizeof(cache.albedoPath), "%s/%s", dirPath, ent->d_name);
                } else if (strstr(ent->d_name, "nor")) {
                    snprintf(cache.normalPath, sizeof(cache.normalPath), "%s/%s", dirPath, ent->d_name);
                } else if (strstr(ent->d_name, "rough")) {
                    snprintf(cache.roughPath, sizeof(cache.roughPath), "%s/%s", dirPath, ent->d_name);
                } else if (strstr(ent->d_name, "ao") || strstr(ent->d_name, "ambient")) {
                    snprintf(cache.aoPath, sizeof(cache.aoPath), "%s/%s", dirPath, ent->d_name);
                } else if (strstr(ent->d_name, "disp") || strstr(ent->d_name, "height")) {
                    snprintf(cache.dispPath, sizeof(cache.dispPath), "%s/%s", dirPath, ent->d_name);
                }
            }
            closedir(dir);
        } else {
            fprintf(stderr, "[WARNING] Could not open material directory: %s\n", dirPath);
        }

        FILE* fout = fopen(cache_path, "wb");
        if (fout) {
            fwrite(&cache, sizeof(OmatCache), 1, fout);
            fclose(fout);
        }
    }

    Material mat;
    reset_material();
    mat = currentMaterial;

    int32_t pIdx;
    if (cache.albedoPath[0]) {
        pIdx = texture_pool_add(&context, cache.albedoPath);
        if (pIdx >= 0) mat.albedoIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Albedo mapped to Bindless Slot: %d\n", mat.albedoIndex);
    }
    if (cache.normalPath[0]) {
        pIdx = texture_pool_add(&context, cache.normalPath);
        if (pIdx >= 0) mat.normalMapIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Normal mapped to Bindless Slot: %d\n", mat.normalMapIndex);
    }
    if (cache.roughPath[0]) {
        pIdx = texture_pool_load_roughness_to_gltf(&context, cache.roughPath);
        if (pIdx >= 0) mat.metallicRoughIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Roughness mapped to Bindless Slot: %d\n", mat.metallicRoughIndex);
    }
    if (cache.aoPath[0]) {
        pIdx = texture_pool_add(&context, cache.aoPath);
        if (pIdx >= 0) mat.aoIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> AO mapped to Bindless Slot: %d\n", mat.aoIndex);
    }
    if (cache.dispPath[0]) {
        pIdx = texture_pool_add(&context, cache.dispPath);
        if (pIdx >= 0) mat.displacementIndex = texture_pool_get(pIdx)->bindlessSlot;
        printf("[PBR SCAN] -> Displacement mapped to Bindless Slot: %d\n", mat.displacementIndex);
        mat.displacementScale = cache.displacementScale;
    }
    printf("[PBR SCAN] ==========================================\n\n");

    if (cache.albedoPath[0]) glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, mat.baseColorFactor);
    if (cache.roughPath[0]) mat.roughnessFactor = 1.0f;
    if (cache.roughPath[0]) mat.metallicFactor = 1.0f;

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
    pump_async_uploads(&context); // Garbage collect finished staging buffers asynchronously

    // Async GPU Upload Pipeline: Pump compiled cache files back to VRAM
    for (uint32_t i = 0; i < textureCount; i++) {
        if (texturePool[i].status == TEXTURE_STATUS_READY_FOR_UPLOAD) {
            texturePool[i].status = TEXTURE_STATUS_UPLOADING;
            texturePool[i].loaded = false; // Arm the DMA fence flag

            if (texturePoolIsRoughness[i]) {
                const char* cache_path = get_texture_cache_path(texturePoolPaths[i], true);
                FILE* f = fopen(cache_path, "rb");
                if (f) {
                    OtexHeader header;
                    if (fread(&header, sizeof(OtexHeader), 1, f) == 1) {
                        alloc_texture_image(&context, header.width, header.height, header.format, &texturePool[i].image, &texturePool[i].memory);
                        upload_file_to_image_direct(&context, cache_path, header.data_size, texturePool[i].image, header.width, header.height, header.format, sizeof(OtexHeader), &texturePool[i].loaded);
                        finalize_texture(&context, &texturePool[i], header.format, VK_SAMPLER_ADDRESS_MODE_REPEAT);
                        texturePool[i].width = header.width; texturePool[i].height = header.height;
                    }
                    fclose(f);
                }
            } else {
                char dds_path[512];
                strncpy(dds_path, texturePoolPaths[i], sizeof(dds_path) - 1);
                char* ext = strrchr(dds_path, '.');
                if (ext) strcpy(ext, ".dds");

                const char* dds_cache = get_texture_cache_path(dds_path, false);
                if (ext && load_texture_dds(&context, dds_cache, &texturePool[i])) {
                    // load_texture_dds handles async flag setting
                } else {
                    const char* cache_path = get_texture_cache_path(texturePoolPaths[i], false);
                    FILE* f = fopen(cache_path, "rb");
                    if (f) {
                        OtexHeader header;
                        if (fread(&header, sizeof(OtexHeader), 1, f) == 1) {
                            alloc_texture_image(&context, header.width, header.height, header.format, &texturePool[i].image, &texturePool[i].memory);
                            upload_file_to_image_direct(&context, cache_path, header.data_size, texturePool[i].image, header.width, header.height, header.format, sizeof(OtexHeader), &texturePool[i].loaded);
                            finalize_texture(&context, &texturePool[i], header.format, VK_SAMPLER_ADDRESS_MODE_REPEAT);
                            texturePool[i].width = header.width; texturePool[i].height = header.height;
                        }
                        fclose(f);
                    }
                }
            }
        }

        // Zero-flicker DMA validation
        // Only swap the global bindless slot ONCE the GPU fence signals the DMA transfer is fully complete
        if (texturePool[i].status == TEXTURE_STATUS_UPLOADING && texturePool[i].loaded == true) {
            bindlessRegisterTexture(&context, texturePool[i].bindlessSlot, texturePool[i].view, texturePool[i].sampler);
            texturePool[i].status = TEXTURE_STATUS_READY;
            markMeshesSSBODirty(&context); // Force SSBO to drop the -1 fallback and use the real texture slot
            fprintf(stdout, "\033[36m[GPU PUMP] Zero-flicker swap complete! Texture %d is live.\033[0m\n", i);
        }
    }

    frame_index = context.currentFrame;

    dynamic_draw_count = 0;
    vertex_count = 0;
    line_renderer_clear();
    context.indirectDrawCount = (uint32_t)scene.meshes.count;

    vec3 camPos = { camera.position[0], camera.position[1], camera.position[2] };
    float cam_delta = glm_vec3_distance2(camPos, last_camera_pos);

    // AAA Zero-Tick Architecture: Only sort and write to PCIe if the observer or topology changes
    if (scene_topology_dirty || cam_delta > 0.01f) {
        sort_meshes_by_alpha(&scene.meshes, camPos);
        glm_vec3_copy(camPos, last_camera_pos);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            indirect_buffer_dirty[i] = true;
        }
        scene_topology_dirty = false;
    }

    if (indirect_buffer_dirty[frame_index]) {
        updateMeshSSBOAndIndirect(&context, &scene.meshes);
        indirect_buffer_dirty[frame_index] = false;
    }

    // flushMeshSSBO is extremely cheap (O(1) bitmask scan) so it safely runs every frame
    flushMeshSSBO(&context, &scene.meshes);
}

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
    if (!meshes->draw_indices || meshes->count == 0) return;

    if (meshes->capacity > mesh_distances_capacity) {
        mesh_distances_capacity = meshes->capacity;
        mesh_distances = realloc(mesh_distances, mesh_distances_capacity * sizeof(float));
    }

    size_t write_idx = 0;
    for (size_t i = 0; i < meshes->count; i++) {
        uint32_t mesh_idx = meshes->draw_indices[i];
        if (meshes->items[mesh_idx].alpha_mode != 2) {
            if (i != write_idx) {
                uint32_t tmp = meshes->draw_indices[write_idx];
                meshes->draw_indices[write_idx] = meshes->draw_indices[i];
                meshes->draw_indices[i] = tmp;
            }
            write_idx++;
        }
    }
    opaqueMeshCount = (uint32_t)write_idx;
    transparentMeshCount = (uint32_t)(meshes->count - write_idx);

    size_t blend_count = meshes->count - write_idx;
    if (blend_count <= 1) return;

    uint32_t* blend = &meshes->draw_indices[write_idx];

    // AAA Optimization: Pre-calculate distance exactly once per transparent mesh!
    // Slashes massive redundant matrix array accesses inside the sorting loops.
    for (size_t i = 0; i < blend_count; i++) {
        uint32_t mesh_idx = blend[i];
        Mesh* m = &meshes->items[mesh_idx];
        mesh_distances[mesh_idx] = glm_vec3_distance2(cameraPos, (vec3){m->model[3][0], m->model[3][1], m->model[3][2]});
    }

    // Data-Oriented Shell Sort! Sorts indices using cached O(1) L1-cache float lookups.
    size_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (int g = 0; g < 8; g++) {
        size_t gap = gaps[g];
        for (size_t i = gap; i < blend_count; i++) {
            uint32_t temp_idx = blend[i];
            float d_temp = mesh_distances[temp_idx];

            size_t j;
            for (j = i; j >= gap; j -= gap) {
                uint32_t comp_idx = blend[j - gap];
                if (mesh_distances[comp_idx] >= d_temp) break;
                blend[j] = blend[j - gap];
            }
            blend[j] = temp_idx;
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
    meshes->draw_indices = NULL;
    meshes->count = 0;
    meshes->capacity = 0;
}

void meshes_add(Meshes* meshes, Mesh mesh) {
    if (meshes->count == meshes->capacity) {
        size_t new_capacity = meshes->capacity ? meshes->capacity * 2 : 4;
        meshes->items = realloc(meshes->items, new_capacity * sizeof(Mesh));
        meshes->draw_indices = realloc(meshes->draw_indices, new_capacity * sizeof(uint32_t));
        meshes->capacity = new_capacity;
    }
    meshes->draw_indices[meshes->count] = meshes->count;
    meshes->items[meshes->count++] = mesh;
    scene_topology_dirty = true;
}

void meshes_remove(Meshes* meshes, size_t index) {
    if (index >= meshes->count) return;
    for (size_t i = index; i < meshes->count - 1; ++i)
        meshes->items[i] = meshes->items[i + 1];
    meshes->count--;
    for (size_t i = 0; i < meshes->count; i++) {
        meshes->draw_indices[i] = (uint32_t)i;
    }
    scene_topology_dirty = true;
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
    free(meshes->draw_indices);
    meshes->items = NULL;
    meshes->draw_indices = NULL;
    meshes->count = 0;
    meshes->capacity = 0;
    scene_topology_dirty = true;
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
                                     width, height, format, VK_IMAGE_LAYOUT_UNDEFINED, &texture->loaded) &&
              finalize_texture(context, texture, format, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (ok) {
        texture->width = width;
        texture->height = height;
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
        if (texture->memory) {
            free_texture_vram(texture->memory);
            texture->memory = VK_NULL_HANDLE;
        }

        texture->loaded = false; // Suspend rendering until upload completes
        alloc_texture_image(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, &texture->image, &texture->memory);
        upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, &texture->loaded);

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
    texture->loaded = false;
    return upload_pixels_to_image(context, rgba_data, (VkDeviceSize)width * height * 4, texture->image, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &texture->loaded);
}

// Zero-copy direct-to-GPU upload helper. Reads from disk directly into Vulkan mapped memory.
static bool upload_file_to_image_direct(VulkanContext* ctx, const char* filepath,
                                        VkDeviceSize imageSize, VkImage image,
                                        uint32_t w, uint32_t h, VkFormat fmt, size_t file_offset, bool* loaded_flag)
{
    FILE* f = fopen(filepath, "rb");
    if (!f) return false;

    if (file_offset > 0) fseek(f, file_offset, SEEK_SET);

    VkBuffer stagingBuf; VkDeviceMemory stagingMem;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (vkCreateBuffer(ctx->device, &bci, NULL, &stagingBuf) != VK_SUCCESS) { fclose(f); return false; }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx->device, stagingBuf, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    if (vkAllocateMemory(ctx->device, &mai, NULL, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(ctx->device, stagingBuf, NULL); fclose(f); return false;
    }
    vkBindBufferMemory(ctx->device, stagingBuf, stagingMem, 0);

    void* mapped;
    vkMapMemory(ctx->device, stagingMem, 0, imageSize, 0, &mapped);

    // THE MAGIC: fread directly into Vulkan's PCIe-mapped staging buffer! Zero malloc! Zero memcpy!
    size_t bytes_read = fread(mapped, 1, imageSize, f);
    fclose(f);
    vkUnmapMemory(ctx->device, stagingMem);

    if (bytes_read != imageSize) {
        vkDestroyBuffer(ctx->device, stagingBuf, NULL);
        vkFreeMemory(ctx->device, stagingMem, NULL);
        return false;
    }

    VkCommandBuffer cmd = begin_async_cmd(ctx);
    transitionImageLayout(cmd, image, fmt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(cmd, stagingBuf, image, w, h);
    transitionImageLayout(cmd, image, fmt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_async_cmd(cmd);
    submit_async_upload(ctx, stagingBuf, stagingMem, cmd, loaded_flag);
    return true;
}

// Shared staging buffer upload helper — stages pixels, transitions, copies, cleans up.
// Does NOT create image/view/sampler/descriptor — caller owns those.
static bool upload_pixels_to_image(VulkanContext* ctx, unsigned char* pixels,
                                   VkDeviceSize imageSize, VkImage image,
                                   uint32_t w, uint32_t h, VkFormat fmt,
                                   VkImageLayout srcLayout, bool* loaded_flag)
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

    VkCommandBuffer cmd = begin_async_cmd(ctx);
    transitionImageLayout(cmd, image, fmt, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(cmd, stagingBuf, image, w, h);
    transitionImageLayout(cmd, image, fmt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_async_cmd(cmd);
    submit_async_upload(ctx, stagingBuf, stagingMem, cmd, loaded_flag);
    return true;
}

static bool alloc_texture_image_mips(VulkanContext* ctx, uint32_t w, uint32_t h, uint32_t mipLevels,
                                     VkFormat fmt, VkImage* image, VkDeviceMemory* memory)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .extent = {w, h, 1}, .mipLevels = mipLevels, .arrayLayers = 1, .format = fmt,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .samples = VK_SAMPLE_COUNT_1_BIT
    };
    if (vkCreateImage(ctx->device, &ici, NULL, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(ctx->device, *image, &req);
    VkDeviceSize offset;
    if (!alloc_texture_vram(ctx, req, memory, &offset)) {
        vkDestroyImage(ctx->device, *image, NULL); return false;
    }
    vkBindImageMemory(ctx->device, *image, *memory, offset);
    return true;
}

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
    VkMemoryRequirements req; vkGetImageMemoryRequirements(ctx->device, *image, &req);
    VkDeviceSize offset;
    if (!alloc_texture_vram(ctx, req, memory, &offset)) {
        vkDestroyImage(ctx->device, *image, NULL); return false;
    }
    vkBindImageMemory(ctx->device, *image, *memory, offset);
    return true;
}

static bool finalize_texture_mips(VulkanContext* ctx, Texture2D* texture, VkFormat fmt, VkSamplerAddressMode addrMode, uint32_t mipLevels) {
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 }
    };
    if (vkCreateImageView(ctx->device, &vci, NULL, &texture->view) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = addrMode, .addressModeV = addrMode, .addressModeW = addrMode,
        .anisotropyEnable = VK_TRUE, .maxAnisotropy = 16.0f, .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .minLod = 0.0f, .maxLod = (float)mipLevels, .mipLodBias = 0.0f
    };
    if (vkCreateSampler(ctx->device, &sci, NULL, &texture->sampler) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = ctx->descriptorPool2D, .descriptorSetCount = 1, .pSetLayouts = &ctx->descriptorSetLayout2D };
    if (vkAllocateDescriptorSets(ctx->device, &dai, &texture->descriptorSet) != VK_SUCCESS) return false;
    VkDescriptorImageInfo dii = { .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .imageView = texture->view, .sampler = texture->sampler };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = texture->descriptorSet, .dstBinding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .pImageInfo = &dii };
    vkUpdateDescriptorSets(ctx->device, 1, &wds, 0, NULL);
    if (texture->bindlessSlot == 0) {
        texture->bindlessSlot = ctx->bindlessTextureCount++;
    }
    if (texture->status != TEXTURE_STATUS_UPLOADING) {
        bindlessRegisterTexture(ctx, texture->bindlessSlot, texture->view, texture->sampler);
    }
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
    if (texture->bindlessSlot == 0) {
        texture->bindlessSlot = ctx->bindlessTextureCount++;
    }

    if (texture->status != TEXTURE_STATUS_UPLOADING) {
        bindlessRegisterTexture(ctx, texture->bindlessSlot, texture->view, texture->sampler);
    }

    return true;
}

static bool load_texture_from_pixels(VulkanContext* ctx, stbi_uc* pixels, int w, int h, Texture2D* texture) {
    if (!pixels) return false;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    bool ok = alloc_texture_image(ctx, w, h, fmt, &texture->image, &texture->memory) &&
              upload_pixels_to_image(ctx, pixels, (VkDeviceSize)w*h*4, texture->image, w, h, fmt, VK_IMAGE_LAYOUT_UNDEFINED, &texture->loaded) &&
              finalize_texture(ctx, texture, fmt, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    if (ok) { texture->width = w; texture->height = h; }
    return ok;
}

int32_t texture_pool_add_embedded(VulkanContext* ctx, const char* virtual_filename, unsigned char* data, size_t data_size) {
    if (textureCount >= MAX_TEXTURES) {
        fprintf(stderr, "Texture pool full!\n"); return -1;
    }
    if (load_texture_ex(ctx, virtual_filename, data, data_size, &texturePool[textureCount])) {
        printf("  -> Successfully queued embedded texture #%u\n", textureCount);
        return textureCount++;
    }
    return -1;
}

// --- SVG High-Performance Caching ---

// Generates: ~/.cache/obsidian/svg/assets_icons_Close_svg_32x32.orgb
static const char* get_svg_cache_path(const char* original_path, int w, int h) {
    static char cache_path[512];
    const char* home = getenv("HOME");
    if (!home) home = "."; // Fallback

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.cache", home);
    mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home);
    mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/svg", home);
    mkdir(dir_path, 0777);

    // Strip leading './' or '/' to avoid creating hidden files in Linux/Unix
    const char* start = original_path;
    while (*start == '.' && *(start + 1) == '/') start += 2;
    while (*start == '/') start++;

    // Flatten original path to make a safe filesystem name
    char safe_name[256];
    strncpy(safe_name, start, sizeof(safe_name) - 1);
    safe_name[sizeof(safe_name) - 1] = '\0';
    for (int i = 0; safe_name[i]; i++) {
        if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.') safe_name[i] = '_';
    }

    snprintf(cache_path, sizeof(cache_path), "%s/%s_%dx%d.orgb", dir_path, safe_name, w, h);
    return cache_path;
}

int32_t texture_pool_add_svg(VulkanContext* ctx, const char* filename, int width, int height) {
    if (textureCount >= MAX_TEXTURES) {
        fprintf(stderr, "Texture pool full! Cannot load SVG %s\n", filename);
        return -1;
    }

    // Our deterministic key is the cache file path generated from the filename + resolution
    const char* cache_file = get_svg_cache_path(filename, width, height);
    size_t size = (size_t)width * height * 4;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    Texture2D* tex = &texturePool[textureCount];

    // 1. Try blazing-fast ZERO-COPY binary cache read
    if (alloc_texture_image(ctx, width, height, fmt, &tex->image, &tex->memory)) {
        // Attempt to read from disk DIRECTLY into the mapped Vulkan staging buffer
        if (upload_file_to_image_direct(ctx, cache_file, size, tex->image, width, height, fmt, 0, &tex->loaded)) {
            // UI Icons must use CLAMP_TO_EDGE, not REPEAT, to prevent edge bleeding!
            if (finalize_texture(ctx, tex, fmt, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)) {
                fprintf(stdout, "\033[32m[SVG] Cache Hit (ZERO-COPY to GPU): %s\033[0m\n", cache_file);
                tex->width = width;
                tex->height = height;
                return textureCount++;
            }
        }

        // Cache miss or read failure: clean up the image to prepare for rasterization
        vkDestroyImage(ctx->device, tex->image, NULL);
        free_texture_vram(tex->memory);
        tex->image = VK_NULL_HANDLE;
        tex->memory = VK_NULL_HANDLE;
    }

    // 2. Cache Miss: Parse and Rasterize via NanoSVG
    fprintf(stdout, "\033[33m[SVG] Cache Miss. Rasterizing: %s at %dx%d\033[0m\n", filename, width, height);
    unsigned char* pixels = malloc(size);
    NSVGimage* image = nsvgParseFromFile(filename, "px", 96.0f);
    if (!image) {
        fprintf(stderr, "[SVG] Failed to parse SVG: %s\n", filename);
        free(pixels);
        return -1;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image); free(pixels); return -1;
    }

    // Scale to fit target dimensions exactly while maintaining aspect ratio
    float scale = MIN((float)width / image->width, (float)height / image->height);
    float tx = (width - image->width * scale) * 0.5f;
    float ty = (height - image->height * scale) * 0.5f;

    memset(pixels, 0, size); // Clear to fully transparent
    nsvgRasterize(rast, image, tx, ty, scale, pixels, width, height, width * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // 3. Save raw RGBA binary for instant loading next time
    FILE* f = fopen(cache_file, "wb");
    if (f) {
        fwrite(pixels, 1, size, f);
        fclose(f);
    }

    // 4. Upload to Vulkan using standard pixel upload
    bool ok = alloc_texture_image(ctx, width, height, fmt, &tex->image, &tex->memory) &&
        upload_pixels_to_image(ctx, pixels, size, tex->image, width, height, fmt, VK_IMAGE_LAYOUT_UNDEFINED, &tex->loaded) &&
        finalize_texture(ctx, tex, fmt, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    free(pixels);

    if (ok) {
        tex->width = width;
        tex->height = height;
        return textureCount++;
    }

    return -1;
}

static bool load_texture_dds(VulkanContext* ctx, const char* filepath, Texture2D* texture) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return false;

    uint32_t magic;
    if (fread(&magic, 4, 1, f) != 1 || magic != DDS_MAGIC) { fclose(f); return false; }

    DDS_HEADER header;
    fread(&header, sizeof(DDS_HEADER), 1, f);

    uint32_t mipMapCount = header.dwMipMapCount > 0 ? header.dwMipMapCount : 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t blockSize = 16;

    if (header.ddspf.dwFourCC == 0x30315844) { // "DX10"
        DDS_HEADER_DXT10 dx10;
        fread(&dx10, sizeof(DDS_HEADER_DXT10), 1, f);
        if (dx10.dxgiFormat == 98 || dx10.dxgiFormat == 99) { format = VK_FORMAT_BC7_UNORM_BLOCK; blockSize = 16; }
        else if (dx10.dxgiFormat == 83 || dx10.dxgiFormat == 84) { format = VK_FORMAT_BC5_UNORM_BLOCK; blockSize = 16; }
        else if (dx10.dxgiFormat == 77 || dx10.dxgiFormat == 78) { format = VK_FORMAT_BC3_UNORM_BLOCK; blockSize = 16; }
        else if (dx10.dxgiFormat == 71 || dx10.dxgiFormat == 72) { format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; blockSize = 8; }
    } else if (header.ddspf.dwFourCC == 0x35545844) { // "DXT5"
        format = VK_FORMAT_BC3_UNORM_BLOCK; blockSize = 16;
    } else if (header.ddspf.dwFourCC == 0x31545844) { // "DXT1"
        format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; blockSize = 8;
    }

    if (format == VK_FORMAT_UNDEFINED) { fclose(f); return false; }

    size_t dataOffset = ftell(f);
    fseek(f, 0, SEEK_END);
    size_t dataSize = ftell(f) - dataOffset;
    fseek(f, dataOffset, SEEK_SET);

    if (!alloc_texture_image_mips(ctx, header.dwWidth, header.dwHeight, mipMapCount, format, &texture->image, &texture->memory)) {
        fclose(f); return false;
    }

    VkBuffer stagingBuf; VkDeviceMemory stagingMem;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = dataSize, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    vkCreateBuffer(ctx->device, &bci, NULL, &stagingBuf);
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(ctx->device, stagingBuf, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    vkAllocateMemory(ctx->device, &mai, NULL, &stagingMem);
    vkBindBufferMemory(ctx->device, stagingBuf, stagingMem, 0);

    void* mapped;
    vkMapMemory(ctx->device, stagingMem, 0, dataSize, 0, &mapped);
    fread(mapped, 1, dataSize, f);
    vkUnmapMemory(ctx->device, stagingMem);
    fclose(f);

    VkCommandBuffer cmd = begin_async_cmd(ctx);

    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = texture->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipMapCount, 0, 1}, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy* regions = malloc(mipMapCount * sizeof(VkBufferImageCopy));
    uint32_t offset = 0; uint32_t mipW = header.dwWidth; uint32_t mipH = header.dwHeight;
    for (uint32_t i = 0; i < mipMapCount; i++) {
        regions[i] = (VkBufferImageCopy){ .bufferOffset = offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}, .imageExtent = {mipW, mipH, 1} };
        uint32_t blocksW = (mipW + 3) / 4; uint32_t blocksH = (mipH + 3) / 4;
        offset += blocksW * blocksH * blockSize;
        if (mipW > 1) mipW /= 2; if (mipH > 1) mipH /= 2;
    }
    vkCmdCopyBufferToImage(cmd, stagingBuf, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipMapCount, regions);
    free(regions);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    end_async_cmd(cmd);
    submit_async_upload(ctx, stagingBuf, stagingMem, cmd, &texture->loaded);

    if (finalize_texture_mips(ctx, texture, format, VK_SAMPLER_ADDRESS_MODE_REPEAT, mipMapCount)) {
        texture->width = header.dwWidth; texture->height = header.dwHeight;
        return true;
    }
    return false;
}

bool load_texture_ex(VulkanContext* ctx, const char* filename, unsigned char* mem_data, size_t mem_size, Texture2D* texture) {
    char dds_path[512];
    strncpy(dds_path, filename, sizeof(dds_path) - 1);
    char* ext = strrchr(dds_path, '.');
    if (ext) strcpy(ext, ".dds");

    if (!mem_data && ext && load_texture_dds(ctx, dds_path, texture)) {
        fprintf(stdout, "\033[36m[TEXTURE] AAA Pipeline: Zero-Copy Local DDS Loaded -> %s\033[0m\n", dds_path);
        texture->status = TEXTURE_STATUS_READY;
        return true;
    }

    const char* dds_cache = get_texture_cache_path(dds_path, false);
    if (ext && load_texture_dds(ctx, dds_cache, texture)) {
        fprintf(stdout, "\033[32m[TEXTURE] Cache Hit (JIT DDS): %s\033[0m\n", dds_cache);
        texture->status = TEXTURE_STATUS_READY;
        return true;
    }

    const char* cache_path = get_texture_cache_path(filename, false);
    FILE* f = fopen(cache_path, "rb");
    if (f) {
        OtexHeader header;
        if (fread(&header, sizeof(OtexHeader), 1, f) == 1 && header.magic == 0x5845544F) {
            fclose(f);
            fprintf(stdout, "\033[32m[TEXTURE] Cache Hit (ZERO-COPY): %s\033[0m\n", cache_path);
            if (alloc_texture_image(ctx, header.width, header.height, header.format, &texture->image, &texture->memory)) {
                if (upload_file_to_image_direct(ctx, cache_path, header.data_size, texture->image, header.width, header.height, header.format, sizeof(OtexHeader), &texture->loaded)) {
                    if (finalize_texture(ctx, texture, header.format, VK_SAMPLER_ADDRESS_MODE_REPEAT)) {
                        texture->width = header.width; texture->height = header.height;
                        texture->status = TEXTURE_STATUS_READY;
                        return true;
                    }
                }
            }
        } else {
            fclose(f);
        }
    }

    if (mem_data) {
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(mem_data, (int)mem_size, &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) return false;
        bool ok = load_texture_from_pixels(ctx, pixels, w, h, texture);
        stbi_image_free(pixels);
        if(ok) texture->status = TEXTURE_STATUS_READY;
        return ok;
    }

    fprintf(stdout, "\033[33m[TEXTURE] Cache Miss. Queueing Background Cook: %s\033[0m\n", filename);

    // Instant metadata parsing for Editor UI sizing
    int w = 1, h = 1, ch;
    if (stbi_info(filename, &w, &h, &ch)) {
        texture->width = w;
        texture->height = h;
    } else {
        texture->width = 1;
        texture->height = 1;
    }

    texture->status = TEXTURE_STATUS_QUEUED_FOR_COOKING;

    // Pre-allocate bindless slot to eliminate gray flickering
    if (texture->bindlessSlot == 0) {
        texture->bindlessSlot = ctx->bindlessTextureCount++;
        bindlessRegisterTexture(ctx, texture->bindlessSlot, dummyWhiteTexture.view, dummyWhiteTexture.sampler);
    }

    int32_t tIdx = (int32_t)(texture - texturePool);

    strncpy(texturePoolPaths[tIdx], filename, 511);
    texturePoolIsRoughness[tIdx] = false;

    pthread_mutex_lock(&cooker_mutex);
    uint32_t next = (cooker_tail + 1) % MAX_COOKER_JOBS;
    if (next != cooker_head) {
        strncpy(cooker_queue[cooker_tail].filepath, filename, 511);
        cooker_queue[cooker_tail].pool_index = tIdx;
        cooker_queue[cooker_tail].is_roughness = false;
        cooker_tail = next;
        pthread_cond_signal(&cooker_cond);
    }
    pthread_mutex_unlock(&cooker_mutex);

    return true;
}

bool load_texture(VulkanContext* ctx, const char* filename, Texture2D* texture) {
    return load_texture_ex(ctx, filename, NULL, 0, texture);
}

void destroy_texture(VulkanContext* context, Texture2D* texture) {
    if (texture->sampler) vkDestroySampler(context->device, texture->sampler, NULL);
    if (texture->view) vkDestroyImageView(context->device, texture->view, NULL);
    if (texture->image) vkDestroyImage(context->device, texture->image, NULL);
    if (texture->memory) {
        free_texture_vram(texture->memory);
        texture->memory = VK_NULL_HANDLE;
    }

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

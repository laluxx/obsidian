#include "vulkan_setup.h"
#include "../monad/runtime.h"
#include "antialiasing_policy.h"
#include "camera.h"
#include "context.h"
#include "editor_frames.h"
#include "geometry_async.h"
#include "geometry_upload_batch.h"
#include "geometry_upload_flight.h"
#include "renderer.h"
#include "resize_policy.h"
#include "scene.h"
#include "scene_buffers.h"
#include "sdf.h"
#include "shadow_cache.h"
#include "stb_image.h"
#include "window.h"
#include <cglm/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include "2D.frag.spv.h"
#include "2D.vert.spv.h"
#include "brdf_lut.comp.spv.h"
#include "equirect2cube.comp.spv.h"
#include "equirect2cube.fp16.comp.spv.h"
#include "irradiance.comp.spv.h"
#include "irradiance.fp16.comp.spv.h"
#include "pbr.frag.spv.h"
#include "pbr.mesh.spv.h"
#include "pbr.task.spv.h"
#include "pbr.vert.spv.h"
#include "prefilter.comp.spv.h"
#include "prefilter.fp16.comp.spv.h"
#include "shadow.vert.spv.h"
#include "skybox.frag.spv.h"
#include "skybox.vert.spv.h"

/// Implementation

/// Globals
static bool s_geometry_upload_poisoned = false;
/* Opt-in nonblocking *geometry* flight. The main glTF scene loader remains
 * synchronous until it owns a pending scene graph, textures and physics.
 * Exactly one upload can be outstanding; all legacy mega-buffer appenders
 * are excluded while its destination reservations are live. */
static ObsGeometryAsync s_geometry_async;
static uint32_t s_async_requested[OBS_GEOM_POOL_COUNT];
typedef struct {
  bool active;
  VulkanContext *ctx;
  uint64_t ticket;
  VkBuffer staging;
  VkDeviceMemory memory;
  VkCommandBuffer command;
  VkFence fence;
} ObsVkAsyncGeometry;
static ObsVkAsyncGeometry s_async_vk;
static bool geometry_legacy_allowed(void) {
  return !s_geometry_upload_poisoned &&
         !obs_geometry_async_busy(&s_geometry_async);
}

/* A single quarantined transfer at most: poisoning disables all subsequent
 * geometry submissions until the device is torn down. Resource handles remain
 * live if the fence did not establish completion. */
typedef struct {
  bool active;
  VkDevice device;
  VkCommandPool pool;
  VkCommandBuffer command;
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkFence fence;
  ObsGeometryFlight flight;
  uint64_t ticket;
} ObsUnconfirmedUpload;
static ObsUnconfirmedUpload s_unconfirmed_upload;

static void obs_retire_unconfirmed_upload(bool device_idle) {
  ObsUnconfirmedUpload *u = &s_unconfirmed_upload;
  if (!u->active)
    return;
  if (!device_idle && vkGetFenceStatus(u->device, u->fence) != VK_SUCCESS)
    return;
  if (u->fence)
    vkDestroyFence(u->device, u->fence, NULL);
  if (u->command)
    vkFreeCommandBuffers(u->device, u->pool, 1, &u->command);
  if (u->buffer)
    vkDestroyBuffer(u->device, u->buffer, NULL);
  if (u->memory)
    vkFreeMemory(u->device, u->memory, NULL);
  /* Device-idle or a signaled fence is the only safe recovery evidence. */
  if (u->flight.state != OBS_FLIGHT_EMPTY)
    (void)obs_geometry_flight_recover_quiesced(&u->flight, u->ticket, true);
  memset(u, 0, sizeof(*u));
}

static uint64_t meshSSBOAddr[MAX_FRAMES_IN_FLIGHT];
uint64_t megaVertexBufferAddr = 0;
uint64_t megaMorphBufferAddr = 0;

uint64_t megaMeshletBufferAddr = 0;
uint64_t megaMeshletBoundsBufferAddr = 0;
uint64_t megaMeshletVertexAddr = 0;
uint64_t megaMeshletTriangleAddr = 0;

VkBuffer megaMeshletBuffer = VK_NULL_HANDLE;
VkDeviceMemory megaMeshletBufferMemory = VK_NULL_HANDLE;
uint64_t megaMeshletBufferSize = 0;
static uint64_t megaMeshletBoundsBufferSize = 0;
static uint64_t megaMeshletSkinBufferSize = 0;
uint32_t megaMeshletBufferOffset = 0;

VkBuffer megaMeshletBoundsBuffer = VK_NULL_HANDLE;
VkDeviceMemory megaMeshletBoundsBufferMemory = VK_NULL_HANDLE;

uint64_t megaMeshletSkinBufferAddr = 0;
VkBuffer megaMeshletSkinBuffer = VK_NULL_HANDLE;
VkDeviceMemory megaMeshletSkinBufferMemory = VK_NULL_HANDLE;

uint64_t dynamicBoundsBufferAddr[MAX_FRAMES_IN_FLIGHT] = {0};
VkBuffer dynamicBoundsBuffer[MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE};
VkDeviceMemory dynamicBoundsBufferMemory[MAX_FRAMES_IN_FLIGHT] = {
    VK_NULL_HANDLE};
static VkDeviceSize dynamicBoundsBufferSize = 0;

VkPipeline refitBoundsPipeline = VK_NULL_HANDLE;
VkPipelineLayout refitBoundsPipelineLayout = VK_NULL_HANDLE;

VkSampler hzbSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout hzbSetLayout = VK_NULL_HANDLE;
VkPipeline hzbReducePipeline = VK_NULL_HANDLE;
VkPipelineLayout hzbReducePipelineLayout = VK_NULL_HANDLE;

VkBuffer megaMeshletVertexBuffer = VK_NULL_HANDLE;
VkDeviceMemory megaMeshletVertexMemory = VK_NULL_HANDLE;
uint64_t megaMeshletVertexBufferSize = 0;
uint32_t megaMeshletVertexOffset = 0;

VkBuffer megaMeshletTriangleBuffer = VK_NULL_HANDLE;
VkDeviceMemory megaMeshletTriangleMemory = VK_NULL_HANDLE;
uint64_t megaMeshletTriangleBufferSize = 0;
uint32_t megaMeshletTriangleOffset = 0;

// Forward declaration for internal use by gltf_loader
uint32_t megaMorphBufferAllocate(VulkanContext *ctx, MorphDelta *deltas,
                                 uint32_t deltaCount);
void createSdfPipeline(VulkanContext *ctx);
static void destroy_multisample_attachments(VulkanContext *ctx, ObsWindow *w);
static bool create_multisample_attachments(VulkanContext *ctx, ObsWindow *w);
uint64_t morphWeightAddr[MAX_FRAMES_IN_FLIGHT] = {0};
uint64_t jointSSBOAddr[MAX_FRAMES_IN_FLIGHT] = {0};
VkBuffer jointSSBO[MAX_FRAMES_IN_FLIGHT] = {0};
VkDeviceMemory jointSSBOMemory[MAX_FRAMES_IN_FLIGHT] = {0};
mat4 *jointSSBOMapped[MAX_FRAMES_IN_FLIGHT] = {0};

uint64_t sdfSSBOAddr[MAX_FRAMES_IN_FLIGHT] = {0};
VkBuffer sdfSSBO[MAX_FRAMES_IN_FLIGHT] = {0};
VkDeviceMemory sdfSSBOMemory[MAX_FRAMES_IN_FLIGHT] = {0};
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT] = {0};

VkPipeline shadowPipeline = VK_NULL_HANDLE;
bool skyboxEnabled = true;
bool iblLightingEnabled = true;
bool shadowsEnabled = true;

VkSampler transmissionSampler = VK_NULL_HANDLE;

extern uint32_t opaqueMeshCount;
extern uint32_t transparentMeshCount;
VkPipeline skyboxPipeline = VK_NULL_HANDLE;
VkPipelineLayout skyboxPipelineLayout = VK_NULL_HANDLE;
PFN_vkCmdDrawMeshTasksEXT pfnCmdDrawMeshTasksEXT = NULL;
VkImage iblSkyboxImage = VK_NULL_HANDLE;
VkDeviceMemory iblSkyboxMemory = VK_NULL_HANDLE;
VkImageView iblSkyboxView = VK_NULL_HANDLE;

/// Validation

#ifndef ENABLE_VALIDATION_LAYERS
/* Validation is an opt-in diagnostic.  Leaving it enabled in the normal
 * editor build routes every command-buffer operation through the validation
 * layer and can dominate the frame CPU budget while typing.  Debug builds can
 * opt in with -DENABLE_VALIDATION_LAYERS=1. */
#define ENABLE_VALIDATION_LAYERS 0
#endif

#if ENABLE_VALIDATION_LAYERS
#define VALIDATION_LAYERS_COUNT 1
static const char *validationLayers[VALIDATION_LAYERS_COUNT] = {
    "VK_LAYER_KHRONOS_validation"};
#else
#define VALIDATION_LAYERS_COUNT 0
static const char **validationLayers = NULL;
#endif

/// Globals

bool ambientOcclusionEnabled = true;
bool cullingFrozen = false;

static VkPipelineCache pipelineCache = VK_NULL_HANDLE;

VkPipeline pipelineIndirectSolid = VK_NULL_HANDLE;
VkPipeline pipelineIndirectTextured = VK_NULL_HANDLE;
VkPipeline graphicsPipelineWireframe = VK_NULL_HANDLE;
VkPipeline sdfPipeline = VK_NULL_HANDLE;

/// Helpers

static void createBuffer(VulkanContext *ctx, VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkBuffer *buffer,
                         VkDeviceMemory *bufferMemory);

/* Create a shader module from an embedded SPIR-V byte array. */
static VkShaderModule createShaderModule(VkDevice device, const uint8_t *code,
                                         size_t codeSize) {
  VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = codeSize,
      .pCode = (const uint32_t *)code};
  VkShaderModule mod;
  if (vkCreateShaderModule(device, &ci, NULL, &mod) != VK_SUCCESS) {
    fprintf(stderr, "vkCreateShaderModule failed\n");
    exit(EXIT_FAILURE);
  }
  return mod;
}

/* Shared vertex-input / input-assembly structs for 2D geometry. */
static void fill2DVertexInput(VkPipelineVertexInputStateCreateInfo *vi,
                              VkPipelineInputAssemblyStateCreateInfo *ia,
                              VkVertexInputBindingDescription *bind,
                              VkVertexInputAttributeDescription attrs[14]) {
  *bind = (VkVertexInputBindingDescription){.binding = 0,
                                            .stride = sizeof(Vertex2D),
                                            .inputRate =
                                                VK_VERTEX_INPUT_RATE_VERTEX};
  attrs[0] =
      (VkVertexInputAttributeDescription){.location = 0,
                                          .binding = 0,
                                          .format = VK_FORMAT_R32G32_SFLOAT,
                                          .offset = offsetof(Vertex2D, pos)};
  attrs[1] = (VkVertexInputAttributeDescription){
      .location = 1,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex2D, color)};
  attrs[2] = (VkVertexInputAttributeDescription){
      .location = 2,
      .binding = 0,
      .format = VK_FORMAT_R32G32_SFLOAT,
      .offset = offsetof(Vertex2D, texCoord)};
  attrs[3] = (VkVertexInputAttributeDescription){
      .location = 3,
      .binding = 0,
      .format = VK_FORMAT_R32G32_SINT,
      .offset = offsetof(Vertex2D, textureIndex)};
  attrs[4] =
      (VkVertexInputAttributeDescription){.location = 4,
                                          .binding = 0,
                                          .format = VK_FORMAT_R32G32_SFLOAT,
                                          .offset = offsetof(Vertex2D, size)};
  attrs[5] = (VkVertexInputAttributeDescription){
      .location = 5,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex2D, cornerRadius)};
  attrs[6] = (VkVertexInputAttributeDescription){
      .location = 6,
      .binding = 0,
      .format = VK_FORMAT_R32_SFLOAT,
      .offset = offsetof(Vertex2D, borderThickness)};
  attrs[7] = (VkVertexInputAttributeDescription){
      .location = 7,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex2D, borderColor)};
  attrs[8] = (VkVertexInputAttributeDescription){
      .location = 8,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .offset = offsetof(Vertex2D, faceData0)};
  attrs[9] = (VkVertexInputAttributeDescription){
      .location = 9,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .offset = offsetof(Vertex2D, faceData1)};
  attrs[10] = (VkVertexInputAttributeDescription){
      .location = 10,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex2D, faceParams)};
  attrs[11] = (VkVertexInputAttributeDescription){
      .location = 11,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .offset = offsetof(Vertex2D, faceEventData)};
  attrs[12] = (VkVertexInputAttributeDescription){
      .location = 12,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex2D, faceEvent)};
  attrs[13] = (VkVertexInputAttributeDescription){
      .location = 13,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex2D, clipRect)};

  *vi = (VkPipelineVertexInputStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = bind,
      .vertexAttributeDescriptionCount = 14,
      .pVertexAttributeDescriptions = attrs};
  *ia = (VkPipelineInputAssemblyStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .primitiveRestartEnable = VK_FALSE};
}

/* Programmable Vertex Pulling - Input Assembler (IA) generator */
static VkPipelineInputAssemblyStateCreateInfo
makeIA(VkPrimitiveTopology topology) {
  return (VkPipelineInputAssemblyStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = topology,
      .primitiveRestartEnable = VK_FALSE};
}

/* Shared rasterizer, multisample, color-blend, dynamic-state blocks. */
static VkPipelineRasterizationStateCreateInfo makeRasterizer(float lineWidth) {
  return (VkPipelineRasterizationStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .lineWidth = lineWidth,
      .cullMode = VK_CULL_MODE_BACK_BIT,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
  };
}

static const VkPipelineColorBlendAttachmentState kBlendAlpha = {
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    .blendEnable = VK_TRUE,
    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
    .alphaBlendOp = VK_BLEND_OP_ADD,
};

static VkPipelineColorBlendStateCreateInfo
makeColorBlend(const VkPipelineColorBlendAttachmentState *att) {
  return (VkPipelineColorBlendStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = att,
  };
}

static const VkDynamicState kDynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR,
                                            VK_DYNAMIC_STATE_DEPTH_BIAS};
static const VkPipelineDynamicStateCreateInfo kDynamicState = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 3,
    .pDynamicStates = kDynStates};

static const VkDynamicState kDynStatesLine[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH};
static const VkPipelineDynamicStateCreateInfo kDynamicStateLine = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 4,
    .pDynamicStates = kDynStatesLine};

static const VkPipelineViewportStateCreateInfo kViewportState = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .scissorCount = 1,
};

/// makeDepth — implementation group
static VkPipelineDepthStencilStateCreateInfo makeDepth(bool test, bool write) {
  return (VkPipelineDepthStencilStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = write ? VK_TRUE : VK_FALSE,
      .depthCompareOp = test ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS,
  };
}

/// Instance / Device / Surface

bool checkExtensionSupport(const char **requiredExtensions,
                           uint32_t requiredCount) {
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
  VkExtensionProperties *avail = malloc(count * sizeof(*avail));
  vkEnumerateInstanceExtensionProperties(NULL, &count, avail);

  for (uint32_t i = 0; i < requiredCount; i++) {
    bool found = false;
    for (uint32_t j = 0; j < count; j++) {
      if (strcmp(requiredExtensions[i], avail[j].extensionName) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      free(avail);
      return false;
    }
  }
  free(avail);
  return true;
}

bool checkValidationLayerSupport(void) {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, NULL);
  VkLayerProperties *avail = malloc(count * sizeof(*avail));
  vkEnumerateInstanceLayerProperties(&count, avail);

  for (uint32_t i = 0; i < VALIDATION_LAYERS_COUNT; i++) {
    bool found = false;
    for (uint32_t j = 0; j < count; j++) {
      if (strcmp(validationLayers[i], avail[j].layerName) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      free(avail);
      return false;
    }
  }
  free(avail);
  return true;
}

void createInstance(VulkanContext *ctx) {
#if ENABLE_VALIDATION_LAYERS
  if (!checkValidationLayerSupport()) {
    fprintf(stderr, "Validation layers requested but not available!\n");
    exit(EXIT_FAILURE);
  }
#endif
  VkApplicationInfo appInfo = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                               .pApplicationName = "Obsidian",
                               .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                               .pEngineName = "Obsidian Engine",
                               .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                               .apiVersion = VK_API_VERSION_1_3};
  uint32_t glfwExtCount = 0;
  const char **glfwExt =
      ctx->headless ? NULL : glfwGetRequiredInstanceExtensions(&glfwExtCount);
  const char *debug_utils_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  const char *debug_labels_env = getenv("OBSIDIAN_VK_DEBUG_LABELS");
  bool debug_labels_requested =
      debug_labels_env && strcmp(debug_labels_env, "1") == 0;
  bool debug_labels_available =
      debug_labels_requested &&
      checkExtensionSupport(&debug_utils_extension, 1);
  uint32_t enabled_ext_count =
      glfwExtCount + (debug_labels_available ? 1u : 0u);
  const char **enabled_extensions = calloc(
      enabled_ext_count ? enabled_ext_count : 1, sizeof(*enabled_extensions));
  if (!enabled_extensions) {
    fprintf(stderr, "Failed to allocate Vulkan instance extension list\n");
    exit(EXIT_FAILURE);
  }
  for (uint32_t i = 0; i < glfwExtCount; ++i)
    enabled_extensions[i] = glfwExt[i];
  if (debug_labels_available)
    enabled_extensions[glfwExtCount] = debug_utils_extension;

  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &appInfo,
                             .enabledExtensionCount = enabled_ext_count,
                             .ppEnabledExtensionNames = enabled_extensions,
                             .enabledLayerCount = VALIDATION_LAYERS_COUNT,
                             .ppEnabledLayerNames = validationLayers};
  VkResult instance_result = vkCreateInstance(&ci, NULL, &ctx->instance);
  free(enabled_extensions);
  if (instance_result != VK_SUCCESS) {
    fprintf(stderr, "Failed to create Vulkan instance\n");
    exit(EXIT_FAILURE);
  }
  if (debug_labels_available) {
    ctx->cmdBeginDebugUtilsLabel =
        (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(
            ctx->instance, "vkCmdBeginDebugUtilsLabelEXT");
    ctx->cmdEndDebugUtilsLabel =
        (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(
            ctx->instance, "vkCmdEndDebugUtilsLabelEXT");
    ctx->debugLabelsEnabled =
        ctx->cmdBeginDebugUtilsLabel && ctx->cmdEndDebugUtilsLabel;
    if (!ctx->debugLabelsEnabled) {
      ctx->cmdBeginDebugUtilsLabel = NULL;
      ctx->cmdEndDebugUtilsLabel = NULL;
      fprintf(stderr,
              "VK_EXT_debug_utils enabled but command labels unavailable\n");
    } else {
      fprintf(stderr, "Vulkan command labels enabled (VK_EXT_debug_utils)\n");
    }
  } else if (debug_labels_requested) {
    fprintf(stderr,
            "VK_EXT_debug_utils unavailable; command labels disabled\n");
  }
}

static uint32_t findGraphicsQueueFamily(VkPhysicalDevice dev) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, NULL);
  VkQueueFamilyProperties *props = malloc(count * sizeof(*props));
  vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props);
  for (uint32_t i = 0; i < count; i++) {
    if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      free(props);
      return i;
    }
  }
  free(props);
  return UINT32_MAX;
}

static int scoreDevice(VkPhysicalDevice dev) {
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceMemoryProperties mem;
  vkGetPhysicalDeviceProperties(dev, &props);
  vkGetPhysicalDeviceMemoryProperties(dev, &mem);

  if (findGraphicsQueueFamily(dev) == UINT32_MAX)
    return -1;

  int score = 0;
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    score += 100000;
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    score += 10000;

  for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
    if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      score += (int)(mem.memoryHeaps[i].size / (1024 * 1024));

  return score;
}

void pickPhysicalDevice(VulkanContext *ctx) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);
  if (count == 0) {
    fprintf(stderr, "No Vulkan-capable GPU found\n");
    exit(EXIT_FAILURE);
  }

  VkPhysicalDevice *devs = malloc(count * sizeof(*devs));
  vkEnumeratePhysicalDevices(ctx->instance, &count, devs);

  VkPhysicalDevice best = VK_NULL_HANDLE;
  int bestScore = -1;
  for (uint32_t i = 0; i < count; i++) {
    int s = scoreDevice(devs[i]);
    if (s > bestScore) {
      bestScore = s;
      best = devs[i];
    }
  }
  free(devs);

  if (best == VK_NULL_HANDLE) {
    fprintf(stderr, "No suitable GPU found\n");
    exit(EXIT_FAILURE);
  }
  ctx->physicalDevice = best;
  ctx->graphicsQueueFamily = findGraphicsQueueFamily(best);

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(best, &props);
  fprintf(stdout, "Selected GPU: %s\n", props.deviceName);
  VkPhysicalDeviceDepthStencilResolveProperties resolve_properties = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
  VkPhysicalDeviceProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &resolve_properties};
  vkGetPhysicalDeviceProperties2(best, &properties2);
  ctx->supportedSampleCounts = props.limits.framebufferColorSampleCounts &
                               props.limits.framebufferDepthSampleCounts;
  ctx->depthResolveMode =
      (resolve_properties.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT)
          ? VK_RESOLVE_MODE_MAX_BIT
          : VK_RESOLVE_MODE_NONE;
  /* The HZB is built from resolved nearest-depth values. Other resolve modes
   * can hide a farther covered sample and invalidate its conservative bound. */
  if (ctx->depthResolveMode == VK_RESOLVE_MODE_NONE)
    ctx->supportedSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  ctx->rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
}

static bool checkDeviceExtensionSupport(VkPhysicalDevice device,
                                        const char *extension_name) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) !=
      VK_SUCCESS)
    return false;
  VkExtensionProperties *available = calloc(count, sizeof(*available));
  if (!available)
    return false;
  VkResult result =
      vkEnumerateDeviceExtensionProperties(device, NULL, &count, available);
  bool found = false;
  if (result == VK_SUCCESS) {
    for (uint32_t i = 0; i < count; ++i) {
      if (strcmp(available[i].extensionName, extension_name) == 0) {
        found = true;
        break;
      }
    }
  }
  free(available);
  return found;
}

void createLogicalDevice(VulkanContext *ctx) {
  float pri = 1.0f;
  VkDeviceQueueCreateInfo qci = {.sType =
                                     VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = ctx->graphicsQueueFamily,
                                 .queueCount = 1,
                                 .pQueuePriorities = &pri};

  // Extensions
  const char *enabled_extensions[3];
  uint32_t enabled_extension_count = 0;
  if (!ctx->headless)
    enabled_extensions[enabled_extension_count++] =
        VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  enabled_extensions[enabled_extension_count++] =
      VK_EXT_MESH_SHADER_EXTENSION_NAME;
  ctx->memoryBudgetEnabled = checkDeviceExtensionSupport(
      ctx->physicalDevice, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (ctx->memoryBudgetEnabled)
    enabled_extensions[enabled_extension_count++] =
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;

  VkPhysicalDeviceFeatures supported_features = {0};
  vkGetPhysicalDeviceFeatures(ctx->physicalDevice, &supported_features);
  ctx->shaderStorageImageExtendedFormatsEnabled =
      supported_features.shaderStorageImageExtendedFormats == VK_TRUE;
  VkPhysicalDeviceFeatures features = {.wideLines = VK_TRUE,
                                       .multiDrawIndirect = VK_TRUE,
                                       .samplerAnisotropy = VK_TRUE,
                                       .fillModeNonSolid = VK_TRUE,
                                       .shaderStorageImageExtendedFormats =
                                           supported_features
                                               .shaderStorageImageExtendedFormats};

  VkPhysicalDeviceVulkan11Features features11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = NULL,
      .shaderDrawParameters = VK_TRUE,
  };

  // All descriptor indexing flags now live here — no separate EXT struct needed
  VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &features11,
      .bufferDeviceAddress = VK_TRUE,
      .drawIndirectCount = VK_TRUE,
      .descriptorIndexing = VK_TRUE,
      .descriptorBindingPartiallyBound = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .samplerFilterMinmax =
          VK_TRUE, //  Optimization: Hardware MAX reduction for HZB!
  };

  VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features12,
      .dynamicRendering = VK_TRUE,
      .shaderDemoteToHelperInvocation = VK_TRUE,
      .maintenance4 = VK_TRUE // : Enabled natively in Vulkan 1.3!
  };

  VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
      .pNext = &features13,
      .meshShader = VK_TRUE,
      .taskShader = VK_TRUE};

  VkDeviceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &meshFeatures,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = enabled_extension_count,
      .ppEnabledExtensionNames = enabled_extensions,
      .pEnabledFeatures = &features,
      // Validation layers belong to VkInstanceCreateInfo, not the device.
      // Supplying them here is invalid since Vulkan 1.0 and triggers a
      // validation error on every application startup.
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = NULL};
  VkResult device_result =
      vkCreateDevice(ctx->physicalDevice, &ci, NULL, &ctx->device);
  if (device_result != VK_SUCCESS) {
    fprintf(stderr, "Failed to create logical device: VkResult %d\n",
            device_result);
    exit(EXIT_FAILURE);
  }
  if (ctx->debugLabelsEnabled)
    ctx->setDebugUtilsObjectName =
        (PFN_vkSetDebugUtilsObjectNameEXT)vkGetDeviceProcAddr(
            ctx->device, "vkSetDebugUtilsObjectNameEXT");
  vkGetDeviceQueue(ctx->device, ctx->graphicsQueueFamily, 0,
                   &ctx->graphicsQueue);
  pfnCmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(
      ctx->device, "vkCmdDrawMeshTasksEXT");
  if (!pfnCmdDrawMeshTasksEXT)
    fprintf(stderr,
            "[WARN] vkCmdDrawMeshTasksEXT not available on this device\n");
  if (!obs_vk_memory_tracker_init(ctx->device, ctx->physicalDevice))
    fprintf(stderr,
            "[WARN] Vulkan device-memory allocation tracking unavailable\n");
}

void obs_vk_name_object(VulkanContext *ctx, VkObjectType type, uint64_t handle,
                        const char *name) {
  if (!ctx || !ctx->debugLabelsEnabled || !ctx->setDebugUtilsObjectName ||
      !handle || !name)
    return;
  const VkDebugUtilsObjectNameInfoEXT info = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
      .objectType = type,
      .objectHandle = handle,
      .pObjectName = name,
  };
  VkResult result = ctx->setDebugUtilsObjectName(ctx->device, &info);
  if (result != VK_SUCCESS)
    fprintf(stderr, "Vulkan debug object naming failed for %s (%d)\n", name,
            result);
}

bool obs_vk_memory_budget_snapshot(VulkanContext *ctx, uint32_t *heap_count,
                                   VkDeviceSize usage[VK_MAX_MEMORY_HEAPS],
                                   VkDeviceSize budget[VK_MAX_MEMORY_HEAPS]) {
  if (!ctx || !ctx->physicalDevice || !ctx->memoryBudgetEnabled ||
      !heap_count || !usage || !budget)
    return false;
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
  };
  VkPhysicalDeviceMemoryProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
      .pNext = &budget_properties,
  };
  vkGetPhysicalDeviceMemoryProperties2(ctx->physicalDevice, &properties);
  *heap_count = properties.memoryProperties.memoryHeapCount;
  for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) {
    usage[i] = i < *heap_count ? budget_properties.heapUsage[i] : 0;
    budget[i] = i < *heap_count ? budget_properties.heapBudget[i] : 0;
  }
  return true;
}

/// SWAPCHAIN

void createSwapChain(VulkanContext *ctx, ObsWindow *w) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, w->surface,
                                            &caps);

  VkExtent2D ext;
  if (caps.currentExtent.width != UINT32_MAX) {
    ext = caps.currentExtent;
  } else {
    int fw2, fh2;
    glfwGetFramebufferSize(w->window, &fw2, &fh2);
    ext.width = (uint32_t)fw2;
    ext.height = (uint32_t)fh2;
    ext.width =
        ext.width < caps.minImageExtent.width   ? caps.minImageExtent.width
        : ext.width > caps.maxImageExtent.width ? caps.maxImageExtent.width
                                                : ext.width;
    ext.height =
        ext.height < caps.minImageExtent.height   ? caps.minImageExtent.height
        : ext.height > caps.maxImageExtent.height ? caps.maxImageExtent.height
                                                  : ext.height;
  }

  uint32_t imgCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
    imgCount = caps.maxImageCount;

  VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = w->surface,
      .minImageCount = imgCount,
      .imageFormat = VK_FORMAT_B8G8R8A8_SRGB,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = ext,
      .imageArrayLayers = 1,
      //  FIX: Allow Vulkan to copy FROM the swapchain for our screen-space
      //  refraction!
      .imageUsage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = VK_NULL_HANDLE};
  if (vkCreateSwapchainKHR(ctx->device, &ci, NULL, &w->swapChain) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to create swap chain\n");
    exit(EXIT_FAILURE);
  }

  vkGetSwapchainImagesKHR(ctx->device, w->swapChain, &w->swapChainImageCount,
                          NULL);
  w->swapChainImages = malloc(w->swapChainImageCount * sizeof(VkImage));
  vkGetSwapchainImagesKHR(ctx->device, w->swapChain, &w->swapChainImageCount,
                          w->swapChainImages);
  w->swapChainImageFormat = VK_FORMAT_B8G8R8A8_SRGB;
  w->swapChainExtent = ext;
}

void createImageViews(VulkanContext *ctx, ObsWindow *w) {
  w->swapChainImageViews = malloc(w->swapChainImageCount * sizeof(VkImageView));
  for (uint32_t i = 0; i < w->swapChainImageCount; i++) {
    VkImageViewCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = w->swapChainImages[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = w->swapChainImageFormat,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1}};
    if (vkCreateImageView(ctx->device, &ci, NULL, &w->swapChainImageViews[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to create image view %u\n", i);
      exit(EXIT_FAILURE);
    }
  }
}

/// Descriptor layouts / Pools

void createDescriptorSetLayout(VulkanContext *ctx) {
  /* set=0 : UBO (used by all 3D pipelines — vertex AND fragment read
   * cameraPos/time) */
  VkDescriptorSetLayoutBinding b = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT |
                    VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_VERTEX_BIT |
                    VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &b};
  vkCreateDescriptorSetLayout(ctx->device, &ci, NULL,
                              &ctx->descriptorSetLayout);
}

void create2DDescriptorSetLayout(VulkanContext *ctx) {
  /* set=0 (2D) or set=1 (3D textured/SDF) : combined-image-sampler */
  VkDescriptorSetLayoutBinding b = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &b};
  if (vkCreateDescriptorSetLayout(ctx->device, &ci, NULL,
                                  &ctx->descriptorSetLayout2D) != VK_SUCCESS) {
    fprintf(stderr, "Failed to create 2D descriptor set layout\n");
    exit(EXIT_FAILURE);
  }
}

void createDescriptorPool(VulkanContext *ctx) {
  VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             .descriptorCount = MAX_FRAMES_IN_FLIGHT};
  VkDescriptorPoolCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .poolSizeCount = 1,
      .pPoolSizes = &ps,
      .maxSets = MAX_FRAMES_IN_FLIGHT};
  if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &ctx->descriptorPool) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to create descriptor pool\n");
    exit(EXIT_FAILURE);
  }
}

void create2DDescriptorPool(VulkanContext *ctx) {
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             MAX_TEXTURES};
  VkDescriptorPoolCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .poolSizeCount = 1,
      .pPoolSizes = &ps,
      .maxSets = MAX_TEXTURES};
  if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &ctx->descriptorPool2D) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to create 2D descriptor pool\n");
    exit(EXIT_FAILURE);
  }
}

void createDescriptorSet(VulkanContext *ctx) {
  VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    layouts[i] = ctx->descriptorSetLayout;

  VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = ctx->descriptorPool,
      .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
      .pSetLayouts = layouts};
  if (vkAllocateDescriptorSets(ctx->device, &ai, ctx->descriptorSets) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to allocate descriptor sets\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VkDescriptorBufferInfo bi = {.buffer = ctx->uniformBuffers[i],
                                 .offset = 0,
                                 .range = sizeof(UniformBufferObject)};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = ctx->descriptorSets[i],
                              .dstBinding = 0,
                              .descriptorType =
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              .descriptorCount = 1,
                              .pBufferInfo = &bi};
    vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
  }
}

/// Pipeline layouts
//
//  (created once, shared across merged pipelines)
//
//   2D family  (color & textured & SDF):
//      layout2D    ->  no descriptor sets, push mat4 in VS
//      layoutTex2D ->  set=0:sampler, push mat4 in VS
//
//   3D family  (solid & textured & SDF & line):
//      layout3D    ->  set=0:UBO, push PushConstants in VS+FS
//      layoutTex3D ->  set=0:UBO  set=1:sampler, push PushConstants in VS+FS
//
void createAllPipelineLayouts(VulkanContext *ctx) {
  /* 2D push: mat4 projection, vertex stage only */
  VkPushConstantRange mat4Range = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                                 VK_SHADER_STAGE_FRAGMENT_BIT,
                                   .offset = 0,
                                   .size = sizeof(Obs2DPushConstants)};

  /* 3D push: PushConstants (16 bytes), both stages */
  VkPushConstantRange pcRange = {.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT |
                                               VK_SHADER_STAGE_MESH_BIT_EXT |
                                               VK_SHADER_STAGE_VERTEX_BIT |
                                               VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .offset = 0,
                                 .size = sizeof(PushConstants)};

  /* ── 2D unified layout: set=0 bindless array, push mat4 in VS ───── */
  vkCreatePipelineLayout(
      ctx->device,
      &(VkPipelineLayoutCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 1,
          .pSetLayouts = &ctx->bindlessSetLayout,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &mat4Range},
      NULL, &ctx->pipelineLayoutTextured2D);
  /* aliases — both 2D paths use the same unified layout */
  ctx->pipelineLayout2D = ctx->pipelineLayoutTextured2D;

  VkDescriptorSetLayout pbr3DSets[4] = {
      ctx->descriptorSetLayout, // set=0 UBO
      ctx->bindlessSetLayout,   // set=1 bindless
      ctx->bindlessSetLayout,   // set=2 alias   (never bound, satisfies layout)
      ctx->lightingSetLayout,   // set=3 lighting
  };
  vkCreatePipelineLayout(
      ctx->device,
      &(VkPipelineLayoutCreateInfo){
          .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          .setLayoutCount = 4,
          .pSetLayouts = pbr3DSets,
          .pushConstantRangeCount = 1,
          .pPushConstantRanges = &pcRange},
      NULL, &ctx->pipelineLayout);

  /* All 3D variants share the same layout */
  ctx->pipelineLayoutLine = ctx->pipelineLayout;
  ctx->pipelineLayoutTextured3D = ctx->pipelineLayout;
  ctx->pipelineLayoutIndirect = ctx->pipelineLayout;
}

/// GRAPHICS PIPELINES
// all created in ONE batch call
enum { OBS_PIPELINE_CACHE_PATH_CAPACITY = 512 };

static const char *get_pipeline_cache_path() {
  static char cache_path[OBS_PIPELINE_CACHE_PATH_CAPACITY];
  const char *home = getenv("HOME");
  if (!home)
    home = ".";
  char dir_path[512];
  snprintf(dir_path, sizeof(dir_path), "%s/.cache", home);
  mkdir(dir_path, 0777);
  snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home);
  mkdir(dir_path, 0777);
  snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/pipelines", home);
  mkdir(dir_path, 0777);
  snprintf(cache_path, sizeof(cache_path), "%s/vk_pipeline_cache.bin",
           dir_path);
  return cache_path;
}

static bool save_pipeline_cache_atomically(const char *path,
                                           const void *data, size_t size) {
  char temp_path[OBS_PIPELINE_CACHE_PATH_CAPACITY + 32];
  const int path_length =
      snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", path);
  if (path_length < 0 || (size_t)path_length >= sizeof(temp_path))
    return false;

  const int fd = mkstemp(temp_path);
  if (fd < 0)
    return false;
  FILE *file = fdopen(fd, "wb");
  if (!file) {
    close(fd);
    unlink(temp_path);
    return false;
  }

  bool ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
  if (fclose(file) != 0)
    ok = false;
  if (ok && rename(temp_path, path) != 0)
    ok = false;
  if (!ok)
    unlink(temp_path);
  return ok;
}

void createGraphicsPipelines(VulkanContext *ctx) {
  const VkPipelineMultisampleStateCreateInfo multisampling = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = ctx->rasterizationSamples};
  const VkPipelineMultisampleStateCreateInfo singleSampling = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkDescriptorSetLayout skyboxLayouts[2] = {ctx->descriptorSetLayout,
                                            ctx->lightingSetLayout};
  VkPipelineLayoutCreateInfo skyboxPlci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 2,
      .pSetLayouts = skyboxLayouts};
  vkCreatePipelineLayout(ctx->device, &skyboxPlci, NULL, &skyboxPipelineLayout);

  VkPipelineCacheCreateInfo cacheCI = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};

  size_t cacheSize = 0;
  void *cacheData = NULL;
  const char *cachePath = get_pipeline_cache_path();
  FILE *f = fopen(cachePath, "rb");
  if (f) {
    if (fseek(f, 0, SEEK_END) == 0) {
      const long file_size = ftell(f);
      if (file_size > 0 && fseek(f, 0, SEEK_SET) == 0) {
        cacheSize = (size_t)file_size;
        cacheData = malloc(cacheSize);
        if (cacheData && fread(cacheData, 1, cacheSize, f) == cacheSize) {
          cacheCI.initialDataSize = cacheSize;
          cacheCI.pInitialData = cacheData;
          fprintf(stdout,
                  "\033[32m[PIPELINE] Cache Hit: Loaded %zu bytes from %s\033[0m\n",
                  cacheSize, cachePath);
        } else {
          free(cacheData);
          cacheData = NULL;
          cacheSize = 0;
        }
      }
    }
    fclose(f);
  } else {
    fprintf(stdout, "\033[33m[PIPELINE] Cache Miss: Building pipelines from "
                    "scratch\033[0m\n");
  }

  VkResult cache_result =
      vkCreatePipelineCache(ctx->device, &cacheCI, NULL, &pipelineCache);
  if (cache_result != VK_SUCCESS && cacheCI.initialDataSize > 0) {
    fprintf(stderr,
            "[PIPELINE] Saved cache rejected; retrying with an empty cache\n");
    cacheCI.initialDataSize = 0;
    cacheCI.pInitialData = NULL;
    pipelineCache = VK_NULL_HANDLE;
    cache_result =
        vkCreatePipelineCache(ctx->device, &cacheCI, NULL, &pipelineCache);
  }
  if (cache_result != VK_SUCCESS) {
    pipelineCache = VK_NULL_HANDLE;
    fprintf(stderr, "Failed to create pipeline cache (VkResult %d)\n",
            (int)cache_result);
  }
  if (cacheData)
    free(cacheData);

  VkPipelineRasterizationStateCreateInfo rast1 = makeRasterizer(1.0f);
  VkPipelineRasterizationStateCreateInfo rastLW = makeRasterizer(2.0f);

  VkPipelineRasterizationStateCreateInfo rastWireframe = makeRasterizer(1.0f);
  rastWireframe.polygonMode = VK_POLYGON_MODE_LINE;
  rastWireframe.depthBiasEnable =
      VK_TRUE; // Push lines forward to prevent z-fighting with the surface
  VkPipelineColorBlendStateCreateInfo blend = makeColorBlend(&kBlendAlpha);
  VkPipelineDepthStencilStateCreateInfo depth3D = makeDepth(true, true);
  VkPipelineDepthStencilStateCreateInfo depth2D = makeDepth(false, false);

  VkPipelineVertexInputStateCreateInfo viEmpty = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia3D =
      makeIA(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  VkPipelineInputAssemblyStateCreateInfo iaLine =
      makeIA(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);

  VkVertexInputBindingDescription bind2D;
  VkVertexInputAttributeDescription attr2D[14];
  VkPipelineVertexInputStateCreateInfo vi2D;
  VkPipelineInputAssemblyStateCreateInfo ia2D;
  fill2DVertexInput(&vi2D, &ia2D, &bind2D, attr2D);

  /* ── shader modules ── */
  VkShaderModule tsPBR =
      createShaderModule(ctx->device, pbr_task_spv, sizeof(pbr_task_spv));
  VkShaderModule msPBR =
      createShaderModule(ctx->device, pbr_mesh_spv, sizeof(pbr_mesh_spv));
  VkShaderModule vsPBR =
      createShaderModule(ctx->device, pbr_vert_spv,
                         sizeof(pbr_vert_spv)); // RESTORED FOR LINES/DYNAMIC
  VkShaderModule fsPBR =
      createShaderModule(ctx->device, pbr_frag_spv, sizeof(pbr_frag_spv));
  VkShaderModule vs2D =
      createShaderModule(ctx->device, __2D_vert_spv, sizeof(__2D_vert_spv));
  VkShaderModule fs2D =
      createShaderModule(ctx->device, __2D_frag_spv, sizeof(__2D_frag_spv));

#define STAGE(stg, mod)                                                        \
  {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,                        \
   NULL,                                                                       \
   0,                                                                          \
   stg,                                                                        \
   mod,                                                                        \
   "main",                                                                     \
   NULL}

  VkPipelineShaderStageCreateInfo ssPBR[3] = {
      STAGE(VK_SHADER_STAGE_TASK_BIT_EXT, tsPBR),
      STAGE(VK_SHADER_STAGE_MESH_BIT_EXT, msPBR),
      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsPBR)};

  // : Legacy pipeline for primitives that don't have pre-calculated Meshlets!
  VkPipelineShaderStageCreateInfo ssPBR_Legacy[2] = {
      STAGE(VK_SHADER_STAGE_VERTEX_BIT, vsPBR),
      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsPBR)};
  VkPipelineShaderStageCreateInfo ss2DColor[2] = {
      STAGE(VK_SHADER_STAGE_VERTEX_BIT, vs2D),
      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fs2D)};

  VkShaderModule vsSkybox =
      createShaderModule(ctx->device, skybox_vert_spv, sizeof(skybox_vert_spv));
  VkShaderModule fsSkybox =
      createShaderModule(ctx->device, skybox_frag_spv, sizeof(skybox_frag_spv));
  VkPipelineShaderStageCreateInfo ssSkybox[2] = {
      STAGE(VK_SHADER_STAGE_VERTEX_BIT, vsSkybox),
      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsSkybox)};

  // We reuse the Task and Mesh shaders for shadows! No fragment shader needed
  // for depth-only passes.
  VkPipelineShaderStageCreateInfo ssShadowMesh[2] = {
      STAGE(VK_SHADER_STAGE_TASK_BIT_EXT, tsPBR),
      STAGE(VK_SHADER_STAGE_MESH_BIT_EXT, msPBR)};

  VkPipelineRasterizationStateCreateInfo rastShadow = makeRasterizer(1.0f);
  rastShadow.depthBiasEnable = VK_TRUE;
  // Cull front faces for shadows. Halves geometry load,
  // natively eliminates Peter Panning, and skyrockets FPS.
  rastShadow.cullMode = VK_CULL_MODE_FRONT_BIT;

  VkPipelineRenderingCreateInfo shadowRenderingCI = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
  };

  VkPipelineDepthStencilStateCreateInfo depthSkybox = makeDepth(true, false);
  depthSkybox.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkFormat colorFormat = ctx->primary->swapChainImageFormat;
  VkPipelineRenderingCreateInfo pipelineRenderingCI = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorFormat,
      .depthAttachmentFormat = ctx->primary->depthFormat,
  };

  // : Dedicated un-culled rasterizer for the Skybox (since we are inside it!)
  VkPipelineRasterizationStateCreateInfo rastSkybox = makeRasterizer(1.0f);
  rastSkybox.cullMode = VK_CULL_MODE_NONE;
  /* UI polygons are screen-space and may have either winding. */
  VkPipelineRasterizationStateCreateInfo rast2D = makeRasterizer(1.0f);
  rast2D.cullMode = VK_CULL_MODE_NONE;

  /*
     Index  Pipeline
     0      3D PBR triangles  (direct + indirect, same pipeline)
     1      3D PBR lines
     2      2D color
     3      2D textured
  */
  VkGraphicsPipelineCreateInfo pci[7] = {
      /* 0: 3D PBR triangles */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &pipelineRenderingCI,
          .stageCount = 3,
          .pStages = ssPBR,
          .pVertexInputState = NULL,
          .pInputAssemblyState =
              NULL, // Mesh Shaders completely bypass Input Assembly!
          .pViewportState = &kViewportState,
          .pRasterizationState = &rast1,
          .pMultisampleState = &multisampling,
          .pColorBlendState = &blend,
          .pDepthStencilState = &depth3D,
          .pDynamicState = &kDynamicState,
          .layout = ctx->pipelineLayout,
      },
      /* 1: 3D PBR lines */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &pipelineRenderingCI,
          .stageCount = 2,
          .pStages = ssPBR_Legacy,
          .pVertexInputState = &viEmpty,
          .pInputAssemblyState = &iaLine,
          .pViewportState = &kViewportState,
          .pRasterizationState = &rastLW,
          .pMultisampleState = &multisampling,
          .pColorBlendState = &blend,
          .pDepthStencilState = &depth3D,
          .pDynamicState = &kDynamicStateLine,
          .layout = ctx->pipelineLayout,
      },
      /* 2: 2D unified (color + texture, driven by textureIndex) */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &pipelineRenderingCI,
          .stageCount = 2,
          .pStages = ss2DColor,
          .pVertexInputState = &vi2D,
          .pInputAssemblyState = &ia2D,
          .pViewportState = &kViewportState,
          .pRasterizationState = &rast2D,
          .pMultisampleState = &multisampling,
          .pColorBlendState = &blend,
          .pDepthStencilState = &depth2D,
          .pDynamicState = &kDynamicState,
          .layout = ctx->pipelineLayoutTextured2D,
      },
      /* 3: Skybox */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &pipelineRenderingCI,
          .stageCount = 2,
          .pStages = ssSkybox,
          .pVertexInputState = &viEmpty,
          .pInputAssemblyState = &ia3D,
          .pViewportState = &kViewportState,
          .pRasterizationState = &rastSkybox,
          .pMultisampleState = &multisampling,
          .pColorBlendState = &blend,
          .pDepthStencilState = &depthSkybox,
          .pDynamicState = &kDynamicState,
          .layout = skyboxPipelineLayout,
      },
      /* 4: Shadows */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &shadowRenderingCI,
          .stageCount = 2,
          .pStages = ssShadowMesh, // : Use the Mesh Shaders!
          .pVertexInputState = NULL,
          .pInputAssemblyState = NULL, // : Mesh Shaders skip input assembly
          .pViewportState = &kViewportState,
          .pRasterizationState = &rastShadow,
          .pMultisampleState = &singleSampling,
          .pColorBlendState = NULL,
          .pDepthStencilState = &depth3D,
          .pDynamicState = &kDynamicState,
          .layout = ctx->pipelineLayout,
      },
      /* 5: 3D PBR Wireframe */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &pipelineRenderingCI,
          .stageCount = 2,
          .pStages = ssPBR_Legacy,
          .pVertexInputState = &viEmpty,
          .pInputAssemblyState = &ia3D,
          .pViewportState = &kViewportState,
          .pRasterizationState = &rastWireframe,
          .pMultisampleState = &multisampling,
          .pColorBlendState = &blend,
          // Depth TEST on so wireframe respects real geometry occlusion.
          // Depth WRITE off so wireframe edges never contaminate the depth
          // buffer or the HZB, which would cause the occlusion culler to kill
          // valid meshlets on the next frame wherever wireframe edges wrote
          // depth.
          .pDepthStencilState =
              &(VkPipelineDepthStencilStateCreateInfo){
                  .sType =
                      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                  .depthTestEnable = VK_TRUE,
                  .depthWriteEnable = VK_FALSE,
                  .depthCompareOp = VK_COMPARE_OP_LESS,
              },
          .pDynamicState = &kDynamicStateLine,
          .layout = ctx->pipelineLayout,
      },
      /* 6: Legacy 3D PBR Solid (For Immediate Mode / Gizmos) */
      {
          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
          .pNext = &pipelineRenderingCI,
          .stageCount = 2,
          .pStages = ssPBR_Legacy,
          .pVertexInputState = &viEmpty,
          .pInputAssemblyState = &ia3D,
          .pViewportState = &kViewportState,
          .pRasterizationState = &rast1,
          .pMultisampleState = &multisampling,
          .pColorBlendState = &blend,
          .pDepthStencilState = &depth3D,
          .pDynamicState = &kDynamicState,
          .layout = ctx->pipelineLayout,
      }};

  VkPipeline pipelines[7];
  if (vkCreateGraphicsPipelines(ctx->device, pipelineCache, 7, pci, NULL,
                                pipelines) != VK_SUCCESS) {
    fprintf(stderr, "Failed to create graphics pipelines\n");
    exit(EXIT_FAILURE);
  }

#undef STAGE

  /* One PBR pipeline serves all 3D draw paths */
  ctx->graphicsPipeline = pipelines[0];
  ctx->graphicsPipelineTextured3D =
      pipelines[6]; // : Immediate mode uses the legacy pipeline!
  ctx->graphicsPipelineLine = pipelines[1];
  pipelineIndirectSolid = pipelines[0];
  pipelineIndirectTextured = pipelines[0];
  /* Single unified 2D pipeline — handles colored and textured quads */
  ctx->graphicsPipeline2D = pipelines[2];
  ctx->graphicsPipelineTextured2D = pipelines[2];
  skyboxPipeline = pipelines[3];
  shadowPipeline = pipelines[4];
  graphicsPipelineWireframe = pipelines[5];
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)ctx->graphicsPipeline,
                     "Obsidian PBR mesh pipeline");
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)ctx->graphicsPipelineLine,
                     "Obsidian line pipeline");
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)ctx->graphicsPipeline2D,
                     "Obsidian unified 2D pipeline");
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)skyboxPipeline,
                     "Obsidian skybox pipeline");
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)shadowPipeline,
                     "Obsidian shadow pipeline");
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)graphicsPipelineWireframe,
                     "Obsidian wireframe pipeline");
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)ctx->graphicsPipelineTextured3D,
                     "Obsidian legacy textured 3D pipeline");

  vkDestroyShaderModule(ctx->device, tsPBR, NULL);
  vkDestroyShaderModule(ctx->device, msPBR, NULL);
  vkDestroyShaderModule(ctx->device, vsPBR, NULL);
  vkDestroyShaderModule(ctx->device, fsPBR, NULL);
  vkDestroyShaderModule(ctx->device, vs2D, NULL);
  vkDestroyShaderModule(ctx->device, fs2D, NULL);
  vkDestroyShaderModule(ctx->device, vsSkybox, NULL);
  vkDestroyShaderModule(ctx->device, fsSkybox, NULL);

  createSdfPipeline(ctx);
}

static void destroy_sample_dependent_pipelines(VulkanContext *ctx) {
  if (pipelineIndirectSolid)
    vkDestroyPipeline(ctx->device, pipelineIndirectSolid, NULL);
  pipelineIndirectSolid = VK_NULL_HANDLE;
  pipelineIndirectTextured = VK_NULL_HANDLE;
  ctx->graphicsPipeline = VK_NULL_HANDLE;
  ctx->graphicsPipelineTextured3D = VK_NULL_HANDLE;
  ctx->graphicsPipelineLine = VK_NULL_HANDLE;
  ctx->graphicsPipeline2D = VK_NULL_HANDLE;
  ctx->graphicsPipelineTextured2D = VK_NULL_HANDLE;

  if (graphicsPipelineWireframe)
    vkDestroyPipeline(ctx->device, graphicsPipelineWireframe, NULL);
  graphicsPipelineWireframe = VK_NULL_HANDLE;
  if (skyboxPipeline)
    vkDestroyPipeline(ctx->device, skyboxPipeline, NULL);
  skyboxPipeline = VK_NULL_HANDLE;
  if (shadowPipeline)
    vkDestroyPipeline(ctx->device, shadowPipeline, NULL);
  shadowPipeline = VK_NULL_HANDLE;
  if (sdfPipeline)
    vkDestroyPipeline(ctx->device, sdfPipeline, NULL);
  sdfPipeline = VK_NULL_HANDLE;
  if (skyboxPipelineLayout)
    vkDestroyPipelineLayout(ctx->device, skyboxPipelineLayout, NULL);
  skyboxPipelineLayout = VK_NULL_HANDLE;
  if (pipelineCache)
    vkDestroyPipelineCache(ctx->device, pipelineCache, NULL);
  pipelineCache = VK_NULL_HANDLE;
}

bool obs_vk_set_antialiasing_request(VulkanContext *ctx, int64_t requested) {
  if (!ctx || !ctx->device || !ctx->primary || !ctx->windows)
    return false;

  VkSampleCountFlags supported = ctx->supportedSampleCounts;
  for (ObsWindow *w = ctx->windows; w; w = w->next) {
    if (w->swapChainImageFormat != ctx->primary->swapChainImageFormat)
      return false;
    VkImageFormatProperties color_properties = {0};
    VkImageFormatProperties depth_properties = {0};
    if (vkGetPhysicalDeviceImageFormatProperties(
            ctx->physicalDevice, w->swapChainImageFormat, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
            &color_properties) != VK_SUCCESS ||
        vkGetPhysicalDeviceImageFormatProperties(
            ctx->physicalDevice, w->depthFormat, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0,
            &depth_properties) != VK_SUCCESS)
      return false;
    supported &= color_properties.sampleCounts & depth_properties.sampleCounts;
  }

  uint32_t resolved_samples = 0;
  if (!obs_antialiasing_resolve_request(requested, supported,
                                        &resolved_samples))
    return false;
  VkSampleCountFlagBits next_samples = (VkSampleCountFlagBits)resolved_samples;
  if (ctx->rasterizationSamples == next_samples)
    return true;
  if (vkDeviceWaitIdle(ctx->device) != VK_SUCCESS)
    return false;

  VkSampleCountFlagBits previous_samples = ctx->rasterizationSamples;
  ctx->rasterizationSamples = next_samples;
  bool attachments_ready = true;
  for (ObsWindow *w = ctx->windows; w; w = w->next) {
    destroy_multisample_attachments(ctx, w);
    if (!create_multisample_attachments(ctx, w)) {
      attachments_ready = false;
      break;
    }
  }
  if (!attachments_ready) {
    for (ObsWindow *w = ctx->windows; w; w = w->next)
      destroy_multisample_attachments(ctx, w);
    ctx->rasterizationSamples = previous_samples;
    for (ObsWindow *w = ctx->windows; w; w = w->next) {
      if (!create_multisample_attachments(ctx, w)) {
        fprintf(stderr,
                "Unable to restore multisample attachments after rejection\n");
        abort();
      }
    }
    return false;
  }

  destroy_sample_dependent_pipelines(ctx);
  createGraphicsPipelines(ctx);
  for (ObsWindow *w = ctx->windows; w; w = w->next)
    memset(w->hzbValid, 0, sizeof(w->hzbValid));
  return true;
}

/* Indirect pipeline layout — created AFTER createMeshSSBO so ssboSetLayout is
 * valid */
void createIndirectPipelineLayout(VulkanContext *ctx) {
  /* Unified layout already created in createAllPipelineLayouts — nothing to do
   */
  (void)ctx;
}

#include "sdf.frag.spv.h"
#include "sdf.vert.spv.h"

void createSdfPipeline(VulkanContext *ctx) {
  VkShaderModule vs =
      createShaderModule(ctx->device, sdf_vert_spv, sizeof(sdf_vert_spv));
  VkShaderModule fs =
      createShaderModule(ctx->device, sdf_frag_spv, sizeof(sdf_frag_spv));
  VkPipelineShaderStageCreateInfo ss[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};

  VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1};
  VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .lineWidth = 1.0f};
  VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = ctx->rasterizationSamples};

  VkPipelineColorBlendAttachmentState cba = {
      .colorWriteMask = 0xF,
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .alphaBlendOp = VK_BLEND_OP_ADD};
  VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba};

  // Test depth to intersect with meshes, write depth so meshes occlude the SDF
  // properly!
  VkPipelineDepthStencilStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS};

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynStates};

  VkFormat colorFormat = ctx->primary->swapChainImageFormat;
  VkPipelineRenderingCreateInfo prci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &colorFormat,
      .depthAttachmentFormat = ctx->primary->depthFormat};

  VkGraphicsPipelineCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &prci,
      .stageCount = 2,
      .pStages = ss,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pDepthStencilState = &ds,
      .pColorBlendState = &cb,
      .pDynamicState = &dyn,
      .layout = ctx->pipelineLayout};
  vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &ci, NULL,
                            &sdfPipeline);
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)sdfPipeline,
                     "Obsidian SDF graphics pipeline");

  vkDestroyShaderModule(ctx->device, vs, NULL);
  vkDestroyShaderModule(ctx->device, fs, NULL);
}

#include "hzb_reduce.comp.spv.h"
#include "refit_bounds.comp.spv.h"

void createComputeCullPipeline(VulkanContext *ctx) {
  // --- HZB Reduce Pipeline ---
  VkPushConstantRange hzbPcRange = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                    .offset = 0,
                                    .size = sizeof(float) * 2};
  VkPipelineLayoutCreateInfo hzbPlci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &hzbSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &hzbPcRange};
  vkCreatePipelineLayout(ctx->device, &hzbPlci, NULL, &hzbReducePipelineLayout);

  VkShaderModule hzbModule = createShaderModule(
      ctx->device, hzb_reduce_comp_spv, sizeof(hzb_reduce_comp_spv));
  VkComputePipelineCreateInfo hzbCpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = hzbModule,
                .pName = "main"},
      .layout = hzbReducePipelineLayout};
  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &hzbCpci, NULL,
                           &hzbReducePipeline);
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)hzbReducePipeline,
                     "Obsidian HZB reduce compute pipeline");
  vkDestroyShaderModule(ctx->device, hzbModule, NULL);

  // --- Refit Bounds Pipeline ---
  VkPushConstantRange pcRange = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                 .offset = 0,
                                 .size = sizeof(PushConstants)};

  VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 0,
      .pSetLayouts = NULL,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcRange};
  vkCreatePipelineLayout(ctx->device, &plci, NULL, &refitBoundsPipelineLayout);

  VkShaderModule compModule = createShaderModule(
      ctx->device, refit_bounds_comp_spv, sizeof(refit_bounds_comp_spv));

  VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = compModule,
                .pName = "main"},
      .layout = refitBoundsPipelineLayout};

  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                           &refitBoundsPipeline);
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_PIPELINE,
                     (uint64_t)(uintptr_t)refitBoundsPipeline,
                     "Obsidian bounds refit compute pipeline");
  vkDestroyShaderModule(ctx->device, compModule, NULL);
}

void createComputeCompactPipeline(VulkanContext *ctx) { (void)ctx; }

extern uint32_t active_sdf_count;

/// fill_gpu_addresses — implementation group
static void fill_gpu_addresses(VulkanContext *ctx) {
  pushConstants.meshBufferAddr = meshSSBOAddr[ctx->currentFrame];
  pushConstants.vertexBufferAddr = megaVertexBufferAddr;
  pushConstants.jointBufferAddr = jointSSBOAddr[ctx->currentFrame];
  pushConstants.morphBufferAddr = megaMorphBufferAddr;
  pushConstants.morphWeightAddr = morphWeightAddr[ctx->currentFrame];
  pushConstants.meshletBufferAddr = megaMeshletBufferAddr;
  pushConstants.meshletBoundsAddr = megaMeshletBoundsBufferAddr;
  pushConstants.meshletSkinAddr = megaMeshletSkinBufferAddr;
  pushConstants.dynamicBoundsAddr = dynamicBoundsBufferAddr[ctx->currentFrame];
  pushConstants.meshletVertexAddr = megaMeshletVertexAddr;
  pushConstants.meshletTriangleAddr = megaMeshletTriangleAddr;
  pushConstants.sdfBufferAddr = sdfSSBOAddr[ctx->currentFrame];
  pushConstants.sdfCount = active_sdf_count;
}

static void update_cascade_matrices(VulkanContext *ctx) {
  vec3 lightDir = {0.3f, -1.0f, 0.2f};
  glm_vec3_normalize(lightDir);
  CTX_LIGHTING(ctx)->sun.direction[0] = lightDir[0];
  CTX_LIGHTING(ctx)->sun.direction[1] = lightDir[1];
  CTX_LIGHTING(ctx)->sun.direction[2] = lightDir[2];

  // Tight Cascades: Cascade 0 is now extremely dense for objects close to the
  // camera!
  /* float cascadeSplits[5] = { 0.1f, 5.0f, 15.0f, 50.0f, 200.0f }; */
  float cascadeSplits[5] = {0.1f, 40.0f, 100.0f, 220.0f, 500.0f};

  for (int i = 0; i < 4; i++)
    CTX_LIGHTING(ctx)->cascadeSplits[i] = cascadeSplits[i + 1];

  float fov = glm_rad(camera.fov);
  float aspect = camera.aspect_ratio;
  float tanHalfFov = tanf(fov / 2.0f);

  vec3 camPos = {camera.position[0], camera.position[1], camera.position[2]};
  vec3 camForward = {-camera.view_matrix[0][2], -camera.view_matrix[1][2],
                     -camera.view_matrix[2][2]};
  glm_vec3_normalize(camForward);

  for (int i = 0; i < 4; i++) {
    float n = cascadeSplits[i];
    float f = cascadeSplits[i + 1];

    // 1. Analytic Bounding Sphere Center (Fixed along Camera Z)
    // This stops the shadow matrix from morphing when you look around!
    float centerZ = (f + n) / 2.0f;
    vec3 center;
    glm_vec3_scale(camForward, centerZ, center);
    glm_vec3_add(camPos, center, center);

    // 2. Analytic Radius (Derived strictly from FOV and depths)
    float xf = f * tanHalfFov * aspect;
    float yf = f * tanHalfFov;
    float dz = f - centerZ;
    float radius = sqrtf(xf * xf + yf * yf + dz * dz);
    radius = ceilf(radius * 16.0f) /
             16.0f; // Round up to prevent float precision micro-stutters

    // 3. Create Light View Matrix
    vec3 eye;
    glm_vec3_scale(lightDir, -radius * 5.0f, eye);
    glm_vec3_add(center, eye, eye);

    mat4 lightView;
    glm_lookat(eye, center, (vec3){0.0f, 1.0f, 0.0f}, lightView);

    // 4. Create Projection Matrix
    mat4 lightProj;
    float zNear = 0.0f;
    float zFar = radius * 10.0f;
    glm_ortho(-radius, radius, -radius, radius, zNear, zFar, lightProj);

    // Converts OpenGL [-1, 1] Z to Vulkan [0, 1] Z.
    // This stops shadows from randomly disappearing when objects are behind
    // you!
    lightProj[1][1] *= -1.0f;
    lightProj[2][2] *= 0.5f;
    lightProj[3][2] = lightProj[3][2] * 0.5f + 0.5f;

    // 5. Sub-Texel Snapping (Eliminates Shimmering)
    mat4 shadowMatrix;
    glm_mat4_mul(lightProj, lightView, shadowMatrix);

    float shadowMapRes = 1024.0f;
    vec4 shadowOrigin = {0.0f, 0.0f, 0.0f, 1.0f};
    glm_mat4_mulv(shadowMatrix, shadowOrigin, shadowOrigin);
    shadowOrigin[0] *= (shadowMapRes / 2.0f);
    shadowOrigin[1] *= (shadowMapRes / 2.0f);

    vec2 roundedOrigin = {roundf(shadowOrigin[0]), roundf(shadowOrigin[1])};
    vec2 roundOffset = {roundedOrigin[0] - shadowOrigin[0],
                        roundedOrigin[1] - shadowOrigin[1]};

    lightProj[3][0] += roundOffset[0] * (2.0f / shadowMapRes);
    lightProj[3][1] += roundOffset[1] * (2.0f / shadowMapRes);

    glm_mat4_mul(lightProj, lightView, CTX_LIGHTING(ctx)->cascadeSpace[i]);
  }
}

static void profile_stage(VulkanContext *ctx, VkCommandBuffer cmd,
                          ObsGpuProfileStage stage, uint32_t endpoint) {
  if (ctx->profileWorkCountersEnabled) {
    if (endpoint) {
      if (ctx->profileWorkStageDepth > 0 &&
          ctx->profileWorkStageStack[ctx->profileWorkStageDepth - 1] == stage)
        ctx->profileWorkStageDepth--;
    } else if (ctx->profileWorkStageDepth <
               sizeof(ctx->profileWorkStageStack) /
                   sizeof(ctx->profileWorkStageStack[0])) {
      ctx->profileWorkStageStack[ctx->profileWorkStageDepth++] = stage;
    }
  }
  if (endpoint && ctx->debugLabelsEnabled)
    ctx->cmdEndDebugUtilsLabel(cmd);
  if (ctx->profileTimestampPool) {
    vkCmdWriteTimestamp(
        cmd,
        endpoint ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        ctx->profileTimestampPool,
        obs_gpu_profile_query_index(ctx->currentFrame, stage, endpoint));
  }
  if (!endpoint && ctx->debugLabelsEnabled) {
    static const float colors[][4] = {
        {0.28f, 0.58f, 0.95f, 1.0f}, {0.30f, 0.78f, 0.62f, 1.0f},
        {0.95f, 0.62f, 0.25f, 1.0f}, {0.78f, 0.40f, 0.82f, 1.0f},
        {0.88f, 0.36f, 0.34f, 1.0f}, {0.55f, 0.67f, 0.89f, 1.0f},
    };
    const VkDebugUtilsLabelEXT label = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pLabelName = obs_gpu_profile_stage_name(stage),
        .color =
            {
                colors[stage % (sizeof(colors) / sizeof(colors[0]))][0],
                colors[stage % (sizeof(colors) / sizeof(colors[0]))][1],
                colors[stage % (sizeof(colors) / sizeof(colors[0]))][2],
                colors[stage % (sizeof(colors) / sizeof(colors[0]))][3],
            },
    };
    ctx->cmdBeginDebugUtilsLabel(cmd, &label);
  }
}

static void execute_shadow_pass(VkCommandBuffer cmd, void *user_data) {
  VulkanContext *ctx = (VulkanContext *)user_data;
  if (ctx->shadowCacheReuse || ctx->indirectDrawCount == 0 ||
      !obs_lighting_needs_shadow_map(CTX_LIGHTING(ctx), shadowsEnabled))
    return;
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_SHADOWS, 0);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
  vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f); // Constant, clamp, slope

  VkDescriptorSet gltfSets[4] = {
      ctx->descriptorSets[ctx->currentFrame], ctx->bindlessSet,
      ctx->bindlessSet, /* set=2 placeholder — no shader reads this slot */
      ctx->lightingSets[ctx->currentFrame]};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ctx->pipelineLayout, 0, 4, gltfSets, 0, NULL);

  vkCmdBindIndexBuffer(cmd, ctx->megaIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

  for (int i = 0; i < 4; i++) {
    ObsGpuProfileStage cascade_stage =
        (ObsGpuProfileStage)(OBS_GPU_PROFILE_SHADOW_CASCADE_0 + i);
    profile_stage(ctx, cmd, cascade_stage, 0);
    VkViewport vp = {.width = 1024.0f,
                     .height = 1024.0f,
                     .minDepth = 0.0f,
                     .maxDepth = 1.0f};
    vp.x = (i % 2) * 1024.0f;
    vp.y = (i / 2) * 1024.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = {{(int32_t)vp.x, (int32_t)vp.y}, {1024, 1024}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    pushConstants.cascadeIndex = i;
    pushConstants.meshIndex = -1;
    fill_gpu_addresses(ctx);
    vkCmdPushConstants(
        cmd, ctx->pipelineLayout,
        VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PushConstants), &pushConstants);

    if (pfnCmdDrawMeshTasksEXT) {
      for (uint32_t mIdx = 0; mIdx < scene->meshes.count; mIdx++) {
        Mesh *m = &scene->meshes.items[mIdx];
        if (!m->visible || m->meshletCount == 0 || m->wireframe)
          continue;
        if (m->megaBaseMeshlet == UINT32_MAX)
          continue;
        if (m->megaBaseVertex == UINT32_MAX)
          continue;

        pushConstants.meshIndex = mIdx;
        vkCmdPushConstants(
            cmd, ctx->pipelineLayout,
            VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstants), &pushConstants);

        uint32_t taskGroups = (m->meshletCount + 31) / 32;
        obs_profile_count_mesh_tasks(ctx, taskGroups, 1, 1);
        pfnCmdDrawMeshTasksEXT(cmd, taskGroups, 1, 1);
      }
    }
    profile_stage(ctx, cmd, cascade_stage, 1);
  }
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_SHADOWS, 1);
}

static void execute_main_pass(VkCommandBuffer cmd, void *user_data) {
  VulkanContext *ctx = (VulkanContext *)user_data;
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_MAIN_FORWARD, 0);

  if (!cullingFrozen) {
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix,
                 CTX_LIGHTING(ctx)->cullSpace);
    CTX_LIGHTING(ctx)->cullCameraPos[0] = camera.position[0];
    CTX_LIGHTING(ctx)->cullCameraPos[1] = camera.position[1];
    CTX_LIGHTING(ctx)->cullCameraPos[2] = camera.position[2];
  }
  CTX_LIGHTING(ctx)->freezeCulling = cullingFrozen ? 1 : 0;

  VkViewport viewport = {.width = (float)ctx->drawExtent.width,
                         .height = (float)ctx->drawExtent.height,
                         .minDepth = 0.0f,
                         .maxDepth = 1.0f};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor = {{0, 0}, ctx->drawExtent};
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  pushConstants.ambientOcclusionEnabled = ambientOcclusionEnabled ? 1 : 0;
  pushConstants.iblEnabled = (ctx->iblLoaded && iblLightingEnabled) ? 1 : 0;

  CTX_LIGHTING(ctx)->shadowsEnabled =
      obs_lighting_needs_shadow_map(CTX_LIGHTING(ctx), shadowsEnabled) ? 1 : 0;
  CTX_LIGHTING(ctx)->iblEnabled =
      (ctx->iblLoaded && iblLightingEnabled) ? 1 : 0;
  CTX_LIGHTING(ctx)->ambientIntensity = 1.0f;

  updateLightingUBO(ctx);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    ctx->graphicsPipeline);

  VkDescriptorSet gltfSets[4] = {
      ctx->descriptorSets[ctx->currentFrame],
      ctx->bindlessSet,
      ctx->bindlessSet, /* set=2 placeholder — no shader reads this slot */
      ctx->lightingSets[ctx->currentFrame],
  };
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ctx->pipelineLayout, 0, 4, gltfSets, 0, NULL);

  pushConstants.cascadeIndex = -1; //  CRITICAL FIX: Reset from shadow pass!
  pushConstants.meshIndex = -1;
  fill_gpu_addresses(ctx);

  if (scene->meshes.count > 0 && pfnCmdDrawMeshTasksEXT) {
    for (uint32_t i = 0; i < scene->meshes.count; i++) {
      Mesh *m = &scene->meshes.items[i];
      if (!m->visible || m->meshletCount == 0 || m->alpha_mode == 2 ||
          m->transmissionFactor > 0.0f || m->wireframe)
        continue;
      if (m->megaBaseMeshlet == UINT32_MAX)
        continue;
      if (m->megaBaseVertex == UINT32_MAX)
        continue;

      pushConstants.meshIndex = i;
      vkCmdPushConstants(
          cmd, ctx->pipelineLayout,
          VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
          0, sizeof(PushConstants), &pushConstants);

      // Dispatch 1 task workgroup per 32 meshlets!
      uint32_t taskGroups = (m->meshletCount + 31) / 32;
      obs_profile_count_mesh_tasks(ctx, taskGroups, 1, 1);
      pfnCmdDrawMeshTasksEXT(cmd, taskGroups, 1, 1);
    }
  }

  // --- Immediate Mode / Legacy Primitives (Sphere, Cube, etc.) ---
  if (ctx->indirectDrawCount > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ctx->graphicsPipelineTextured3D);
    vkCmdBindIndexBuffer(cmd, ctx->megaIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    pushConstants.cascadeIndex = -1;
    pushConstants.meshIndex = -1; // Use indirect instance ID
    fill_gpu_addresses(ctx);      // Inherently binds the megaVertexBufferAddr
                                  // containing the dynamic uploads

    vkCmdPushConstants(
        cmd, ctx->pipelineLayout,
        VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PushConstants), &pushConstants);

    VkDeviceSize drawSize =
        (16384 + 4096) * sizeof(VkDrawIndexedIndirectCommand);
    VkDeviceSize offset =
        (ctx->currentFrame *
         drawSize); // Start from 0 to include non-meshlet scene meshes!

    obs_profile_count_indirect_draws(ctx, ctx->indirectDrawCount);
    vkCmdDrawIndexedIndirect(cmd, ctx->indirectBuffer, offset,
                             ctx->indirectDrawCount,
                             sizeof(VkDrawIndexedIndirectCommand));
  }

  if (skyboxEnabled && iblSkyboxView != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline);
    VkDescriptorSet skyboxSets[2] = {ctx->descriptorSets[ctx->currentFrame],
                                     ctx->lightingSets[ctx->currentFrame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            skyboxPipelineLayout, 0, 2, skyboxSets, 0, NULL);
    obs_profile_count_draw(ctx, 1, 36, 0);
    vkCmdDraw(cmd, 36, 1, 0, 0);
  }

  extern uint32_t active_sdf_count;
  if (active_sdf_count > 0 && sdfPipeline != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfPipeline);
    VkDescriptorSet sdfSets[4] = {ctx->descriptorSets[ctx->currentFrame],
                                  ctx->bindlessSet, ctx->bindlessSet,
                                  ctx->lightingSets[ctx->currentFrame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 0, 4, sdfSets, 0, NULL);

    pushConstants.cascadeIndex = -1;
    pushConstants.meshIndex = -1;
    fill_gpu_addresses(ctx);

    vkCmdPushConstants(
        cmd, ctx->pipelineLayout,
        VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PushConstants), &pushConstants);
    obs_profile_count_draw(ctx, 1, 3, 0);
    vkCmdDraw(cmd, 3, 1, 0, 0); // Raymarch Fullscreen Triangle!
  }

  if (lineVertexCount > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ctx->graphicsPipelineLine);

    // RE-BIND descriptor sets because the skybox pipeline disrupted them!
    VkDescriptorSet gltfSets[4] = {
        ctx->descriptorSets[ctx->currentFrame],
        ctx->bindlessSet,
        ctx->bindlessSet, /* set=2 placeholder — no shader reads this slot */
        ctx->lightingSets[ctx->currentFrame],
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 0, 4, gltfSets, 0, NULL);

    line_renderer_draw(cmd);
  }
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_MAIN_FORWARD, 1);
}

static void execute_2d_editor_pass(VkCommandBuffer cmd, void *user_data) {
  VulkanContext *ctx = user_data;
  VkViewport viewport = {.width = (float)ctx->drawExtent.width,
                         .height = (float)ctx->drawExtent.height,
                         .minDepth = 0.0f,
                         .maxDepth = 1.0f};
  VkRect2D scissor = {{0, 0}, ctx->drawExtent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_UI, 0);
  renderer2D_draw(cmd);
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_UI, 1);
}

void create2DGraphicsPipeline(
    VulkanContext *ctx) { /* handled by createGraphicsPipelines */ }
void createTextured2DGraphicsPipeline(
    VulkanContext *ctx) { /* handled by createGraphicsPipelines */ }
void create3DTexturedGraphicsPipeline(
    VulkanContext *ctx) { /* handled by createGraphicsPipelines */ }
void createLineGraphicsPipeline(
    VulkanContext *ctx) { /* handled by createGraphicsPipelines */ }

/// Framebuffers / Command pool / Command buffers

void createCommandPool(VulkanContext *ctx) {
  VkCommandPoolCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
               VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = ctx->graphicsQueueFamily};
  if (vkCreateCommandPool(ctx->device, &ci, NULL, &ctx->commandPool) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to create command pool\n");
    exit(EXIT_FAILURE);
  }
  obs_vk_name_object(ctx, VK_OBJECT_TYPE_COMMAND_POOL,
                     (uint64_t)(uintptr_t)ctx->commandPool,
                     "Obsidian graphics command pool");
}

void createCommandBuffers(VulkanContext *ctx, ObsWindow *w) {
  if (w->commandBuffers) {
    vkFreeCommandBuffers(ctx->device, ctx->commandPool, w->swapChainImageCount,
                         w->commandBuffers);
    free(w->commandBuffers);
    w->commandBuffers = NULL;
  }
  w->commandBuffers = malloc(w->swapChainImageCount * sizeof(VkCommandBuffer));
  VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->commandPool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = w->swapChainImageCount};
  if (vkAllocateCommandBuffers(ctx->device, &ai, w->commandBuffers) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to allocate command buffers\n");
    exit(EXIT_FAILURE);
  }
  for (uint32_t i = 0; i < w->swapChainImageCount; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "Obsidian frame command buffer %u", i);
    obs_vk_name_object(ctx, VK_OBJECT_TYPE_COMMAND_BUFFER,
                       (uint64_t)(uintptr_t)w->commandBuffers[i], name);
  }
}

/// SYNC OBJECTS

void createFrameSyncObjects(VulkanContext *ctx) {
  if (!ctx->headless)
    ctx->imageAvailableSemaphores =
        malloc(MAX_FRAMES_IN_FLIGHT * sizeof(VkSemaphore));
  ctx->inFlightFences = malloc(MAX_FRAMES_IN_FLIGHT * sizeof(VkFence));

  VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                          .flags = VK_FENCE_CREATE_SIGNALED_BIT};

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if ((!ctx->headless &&
         vkCreateSemaphore(ctx->device, &si, NULL,
                           &ctx->imageAvailableSemaphores[i]) != VK_SUCCESS) ||
        vkCreateFence(ctx->device, &fi, NULL, &ctx->inFlightFences[i]) !=
            VK_SUCCESS) {
      fprintf(stderr, "Failed to create sync objects for frame %u\n", i);
      exit(EXIT_FAILURE);
    }
  }
}

void createWindowSyncObjects(VulkanContext *ctx, ObsWindow *w) {
  w->renderFinishedSemaphores =
      malloc(w->swapChainImageCount * sizeof(VkSemaphore));
  w->imagesInFlight = malloc(w->swapChainImageCount * sizeof(VkFence));

  VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

  for (uint32_t i = 0; i < w->swapChainImageCount; i++) {
    if (vkCreateSemaphore(ctx->device, &si, NULL,
                          &w->renderFinishedSemaphores[i]) != VK_SUCCESS) {
      fprintf(stderr, "Failed to create renderFinishedSemaphore %u\n", i);
      exit(EXIT_FAILURE);
    }
    w->imagesInFlight[i] = VK_NULL_HANDLE;
  }
}

/// Depth resources

void createShadowResources(VulkanContext *ctx) {
  // Create 2K Shadow Map Atlas (1024x1024 per cascade)
  VkImageCreateInfo imgCI = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_D32_SFLOAT,
                             .extent = {2048, 2048, 1},
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage =
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    if (vkCreateImage(ctx->device, &imgCI, NULL, &ctx->shadowImage[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to create shadow atlas %u\n", i);
      exit(EXIT_FAILURE);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->device, ctx->shadowImage[i], &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = findMemoryType(
                                   ctx->physicalDevice, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(ctx->device, &ai, NULL, &ctx->shadowMemory[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to allocate shadow atlas memory %u\n", i);
      exit(EXIT_FAILURE);
    }
    if (vkBindImageMemory(ctx->device, ctx->shadowImage[i],
                          ctx->shadowMemory[i], 0) != VK_SUCCESS) {
      fprintf(stderr, "Failed to bind shadow atlas memory %u\n", i);
      exit(EXIT_FAILURE);
    }
    VkImageViewCreateInfo viewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = ctx->shadowImage[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(ctx->device, &viewCI, NULL, &ctx->shadowView[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to create shadow atlas view %u\n", i);
      exit(EXIT_FAILURE);
    }
  }

  VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
  vkCreateSampler(ctx->device, &sci, NULL, &ctx->shadowSampler);

  // Map it to Binding 5 right away (if lighting sets are already created)!
  if (ctx->lightingSets[0] != VK_NULL_HANDLE) {
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
      VkDescriptorImageInfo dInfo = {
          .sampler = ctx->shadowSampler,
          .imageView = ctx->shadowView[f],
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = ctx->lightingSets[f],
                                .dstBinding = 5,
                                .descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .descriptorCount = 1,
                                .pImageInfo = &dInfo};
      vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
  }
}

static void destroy_multisample_attachments(VulkanContext *ctx, ObsWindow *w) {
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    if (w->multisampleColorView[i])
      vkDestroyImageView(ctx->device, w->multisampleColorView[i], NULL);
    if (w->multisampleColorImage[i])
      vkDestroyImage(ctx->device, w->multisampleColorImage[i], NULL);
    if (w->multisampleColorMemory[i])
      vkFreeMemory(ctx->device, w->multisampleColorMemory[i], NULL);
    if (w->multisampleDepthView[i])
      vkDestroyImageView(ctx->device, w->multisampleDepthView[i], NULL);
    if (w->multisampleDepthImage[i])
      vkDestroyImage(ctx->device, w->multisampleDepthImage[i], NULL);
    if (w->multisampleDepthMemory[i])
      vkFreeMemory(ctx->device, w->multisampleDepthMemory[i], NULL);
    w->multisampleColorView[i] = VK_NULL_HANDLE;
    w->multisampleColorImage[i] = VK_NULL_HANDLE;
    w->multisampleColorMemory[i] = VK_NULL_HANDLE;
    w->multisampleDepthView[i] = VK_NULL_HANDLE;
    w->multisampleDepthImage[i] = VK_NULL_HANDLE;
    w->multisampleDepthMemory[i] = VK_NULL_HANDLE;
  }
}

static bool create_multisample_attachments(VulkanContext *ctx, ObsWindow *w) {
  if (ctx->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT || w->suspended ||
      !w->swapChainExtent.width || !w->swapChainExtent.height)
    return true;
  VkImageCreateInfo image_infos[2] = {
      {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
       .imageType = VK_IMAGE_TYPE_2D,
       .format = w->swapChainImageFormat,
       .extent = {w->swapChainExtent.width, w->swapChainExtent.height, 1},
       .mipLevels = 1,
       .arrayLayers = 1,
       .samples = ctx->rasterizationSamples,
       .tiling = VK_IMAGE_TILING_OPTIMAL,
       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
       .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED},
      {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
       .imageType = VK_IMAGE_TYPE_2D,
       .format = w->depthFormat,
       .extent = {w->swapChainExtent.width, w->swapChainExtent.height, 1},
       .mipLevels = 1,
       .arrayLayers = 1,
       .samples = ctx->rasterizationSamples,
       .tiling = VK_IMAGE_TILING_OPTIMAL,
       .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
       .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}};
  VkImage *images[2][MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory *memories[2][MAX_FRAMES_IN_FLIGHT];
  VkImageView *views[2][MAX_FRAMES_IN_FLIGHT];
  VkImageAspectFlags aspects[2] = {VK_IMAGE_ASPECT_COLOR_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT};
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    images[0][i] = &w->multisampleColorImage[i];
    memories[0][i] = &w->multisampleColorMemory[i];
    views[0][i] = &w->multisampleColorView[i];
    images[1][i] = &w->multisampleDepthImage[i];
    memories[1][i] = &w->multisampleDepthMemory[i];
    views[1][i] = &w->multisampleDepthView[i];
  }
  for (uint32_t target = 0; target < 2; ++target) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
      VkMemoryRequirements requirements;
      if (vkCreateImage(ctx->device, &image_infos[target], NULL,
                        images[target][i]) != VK_SUCCESS)
        goto fail;
      vkGetImageMemoryRequirements(ctx->device, *images[target][i],
                                   &requirements);
      VkMemoryAllocateInfo allocation = {
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = requirements.size,
          .memoryTypeIndex =
              findMemoryType(ctx->physicalDevice, requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      if (vkAllocateMemory(ctx->device, &allocation, NULL,
                           memories[target][i]) != VK_SUCCESS ||
          vkBindImageMemory(ctx->device, *images[target][i],
                            *memories[target][i], 0) != VK_SUCCESS)
        goto fail;
      VkImageViewCreateInfo view = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = *images[target][i],
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = image_infos[target].format,
          .subresourceRange = {aspects[target], 0, 1, 0, 1}};
      if (vkCreateImageView(ctx->device, &view, NULL, views[target][i]) !=
          VK_SUCCESS)
        goto fail;
    }
  }
  return true;

fail:
  destroy_multisample_attachments(ctx, w);
  return false;
}

void createDepthResources(VulkanContext *ctx, ObsWindow *w) {
  w->depthFormat = VK_FORMAT_D32_SFLOAT;

  VkImageCreateInfo imgCI = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = w->depthFormat,
      .extent = {w->swapChainExtent.width, w->swapChainExtent.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      //  Optimization: Allow the Compute Shader to read Mip 0 of the depth
      //  buffer for HZB Generation!
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    if (vkCreateImage(ctx->device, &imgCI, NULL, &w->depthImage[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to create depth image\n");
      exit(EXIT_FAILURE);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->device, w->depthImage[i], &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = findMemoryType(
                                   ctx->physicalDevice, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(ctx->device, &ai, NULL, &w->depthImageMemory[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to allocate depth image memory\n");
      exit(EXIT_FAILURE);
    }
    vkBindImageMemory(ctx->device, w->depthImage[i], w->depthImageMemory[i], 0);
    VkImageViewCreateInfo viewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = w->depthImage[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = w->depthFormat,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(ctx->device, &viewCI, NULL, &w->depthImageView[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to create depth image view\n");
      exit(EXIT_FAILURE);
    }
  }

  if (!create_multisample_attachments(ctx, w)) {
    fprintf(stderr, "Failed to create multisample attachments\n");
    exit(EXIT_FAILURE);
  }

  //  Screen-Space Transmission Buffer (Multi-Buffered to prevent temporal race
  //  conditions!)
  if (transmissionSampler == VK_NULL_HANDLE) {
    VkSamplerCreateInfo tSampCI = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    vkCreateSampler(ctx->device, &tSampCI, NULL, &transmissionSampler);
  }

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VkImageCreateInfo tImgCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = w->swapChainImageFormat,
        .extent = {w->swapChainExtent.width, w->swapChainExtent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(ctx->device, &tImgCI, NULL, &w->transmissionImage[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to create transmission image %u\n", i);
      exit(EXIT_FAILURE);
    }

    VkMemoryRequirements tReq;
    vkGetImageMemoryRequirements(ctx->device, w->transmissionImage[i], &tReq);
    VkMemoryAllocateInfo tAlloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = tReq.size,
        .memoryTypeIndex =
            findMemoryType(ctx->physicalDevice, tReq.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(ctx->device, &tAlloc, NULL,
                         &w->transmissionMemory[i]) != VK_SUCCESS) {
      fprintf(stderr, "Failed to allocate transmission image memory %u\n", i);
      exit(EXIT_FAILURE);
    }
    if (vkBindImageMemory(ctx->device, w->transmissionImage[i],
                          w->transmissionMemory[i], 0) != VK_SUCCESS) {
      fprintf(stderr, "Failed to bind transmission image memory %u\n", i);
      exit(EXIT_FAILURE);
    }

    VkImageViewCreateInfo tViewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = w->transmissionImage[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = w->swapChainImageFormat,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(ctx->device, &tViewCI, NULL, &w->transmissionView[i]);
  }

  // ---  HZB RESOURCE ALLOCATION ---
  uint32_t hzbBaseW =
      w->swapChainExtent.width > 1 ? w->swapChainExtent.width / 2 : 1;
  uint32_t hzbBaseH =
      w->swapChainExtent.height > 1 ? w->swapChainExtent.height / 2 : 1;

  w->hzbMipCount =
      (uint32_t)floor(log2(hzbBaseW > hzbBaseH ? hzbBaseW : hzbBaseH)) + 1;
  if (w->hzbMipCount > 16)
    w->hzbMipCount = 16;

  if (hzbSampler == VK_NULL_HANDLE) {
    VkSamplerReductionModeCreateInfoEXT redInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .reductionMode = VK_SAMPLER_REDUCTION_MODE_MAX};
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &redInfo,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 16.0f};
    vkCreateSampler(ctx->device, &sci, NULL, &hzbSampler);
  }

  if (hzbSetLayout == VK_NULL_HANDLE) {
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = b};
    vkCreateDescriptorSetLayout(ctx->device, &lci, NULL, &hzbSetLayout);
  }

  if (w->hzbDescriptorPool == VK_NULL_HANDLE) {
    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT * 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_FRAMES_IN_FLIGHT * 16}};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_FRAMES_IN_FLIGHT * 16,
        .poolSizeCount = 2,
        .pPoolSizes = ps};
    vkCreateDescriptorPool(ctx->device, &pci, NULL, &w->hzbDescriptorPool);
  }

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    //  Fix: Base extent is strictly Half-Resolution so the 2x2 Compute Shader
    //  downsample mathematically aligns with Mip 0!
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R32_SFLOAT,
                             .extent = {hzbBaseW, hzbBaseH, 1},
                             .mipLevels = w->hzbMipCount,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_STORAGE_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(ctx->device, &ici, NULL, &w->hzbImage[i]) != VK_SUCCESS) {
      fprintf(stderr, "Failed to create HZB image %u\n", i);
      exit(EXIT_FAILURE);
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->device, w->hzbImage[i], &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = findMemoryType(
                                   ctx->physicalDevice, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(ctx->device, &ai, NULL, &w->hzbMemory[i]) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to allocate HZB image memory %u\n", i);
      exit(EXIT_FAILURE);
    }
    if (vkBindImageMemory(ctx->device, w->hzbImage[i], w->hzbMemory[i], 0) !=
        VK_SUCCESS) {
      fprintf(stderr, "Failed to bind HZB image memory %u\n", i);
      exit(EXIT_FAILURE);
    }

    VkImageViewCreateInfo vci = {.sType =
                                     VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .image = w->hzbImage[i],
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R32_SFLOAT,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                      0, w->hzbMipCount, 0, 1}};
    vkCreateImageView(ctx->device, &vci, NULL, &w->hzbView[i]);

    for (uint32_t m = 0; m < w->hzbMipCount; m++) {
      vci.subresourceRange.baseMipLevel = m;
      vci.subresourceRange.levelCount = 1;
      vkCreateImageView(ctx->device, &vci, NULL, &w->hzbMipViews[i][m]);

      VkDescriptorSetAllocateInfo dsai = {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .descriptorPool = w->hzbDescriptorPool,
          .descriptorSetCount = 1,
          .pSetLayouts = &hzbSetLayout};
      vkAllocateDescriptorSets(ctx->device, &dsai, &w->hzbSets[i][m]);

      VkDescriptorImageInfo dImgSamp = {
          .sampler = hzbSampler,
          .imageView =
              (m == 0) ? w->depthImageView[i] : w->hzbMipViews[i][m - 1],
          .imageLayout = (m == 0)
                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo dImgStor = {.imageView = w->hzbMipViews[i][m],
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

      VkWriteDescriptorSet writes[2] = {
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
           .dstSet = w->hzbSets[i][m],
           .dstBinding = 0,
           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .descriptorCount = 1,
           .pImageInfo = &dImgSamp},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
           .dstSet = w->hzbSets[i][m],
           .dstBinding = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
           .descriptorCount = 1,
           .pImageInfo = &dImgStor}};
      vkUpdateDescriptorSets(ctx->device, 2, writes, 0, NULL);
    }
  }
}

/// Uniform buffer

static void createBuffer(VulkanContext *ctx, VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkBuffer *buffer,
                         VkDeviceMemory *bufferMemory) {
  VkBufferCreateInfo bufferInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  vkCreateBuffer(ctx->device, &bufferInfo, NULL, buffer);

  VkMemoryRequirements memReq;
  vkGetBufferMemoryRequirements(ctx->device, *buffer, &memReq);

  /* VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT is required whenever the buffer
     carries VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT (which we always add). */
  VkMemoryAllocateFlagsInfo flagsInfo = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
  };
  VkMemoryAllocateInfo allocInfo = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &flagsInfo,
      .allocationSize = memReq.size,
      .memoryTypeIndex = findMemoryType(ctx->physicalDevice,
                                        memReq.memoryTypeBits, properties)};
  vkAllocateMemory(ctx->device, &allocInfo, NULL, bufferMemory);
  vkBindBufferMemory(ctx->device, *buffer, *bufferMemory, 0);
}

void createUniformBuffer(VulkanContext *ctx) {
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    createBuffer(ctx, sizeof(UniformBufferObject),
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &ctx->uniformBuffers[i], &ctx->uniformBuffersMemory[i]);
    vkMapMemory(ctx->device, ctx->uniformBuffersMemory[i], 0,
                sizeof(UniformBufferObject), 0, &ctx->uboMapped[i]);
  }
}

void updateUniformBuffer(VulkanContext *ctx) {
  //  Optimization: Capture prev_vp from the PREVIOUS frame's UBO mapped data
  //  before we do anything else
  uint32_t prevFrame =
      (ctx->currentFrame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
  UniformBufferObject *prev_ubo =
      (UniformBufferObject *)ctx->uboMapped[prevFrame];
  glm_mat4_copy(prev_ubo->vp, CTX_LIGHTING(ctx)->prev_vp);
  CTX_LIGHTING(ctx)->hzbValid =
      ctx->primary && ctx->primary->hzbValid[prevFrame];

  UniformBufferObject ubo;
  glm_mat4_copy(camera.view_matrix, ubo.view);
  glm_mat4_copy(camera.projection_matrix, ubo.proj);
  glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp);
  ubo.cameraPos[0] = camera.position[0];
  ubo.cameraPos[1] = camera.position[1];
  ubo.cameraPos[2] = camera.position[2];
  ubo.cameraPos[3] = 0.0f;
  ubo.time = ctx->headless ? 0.0f : (float)glfwGetTime();
  memcpy(ctx->uboMapped[ctx->currentFrame], &ubo, sizeof(ubo));
}

/// Record command buffer

static void finish_color_output(VulkanContext *ctx, ObsWindow *w,
                                VkCommandBuffer cmd, uint32_t imageIndex) {
  if (!w->headless || ctx->headlessReadbackEnabled) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = w->headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                 : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = w->swapChainImages[imageIndex],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = w->headless ? VK_ACCESS_TRANSFER_READ_BIT : 0};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         w->headless ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                     : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
  }
  if (w->headless && ctx->headlessReadbackEnabled) {
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_READBACK, 0);
    VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {w->swapChainExtent.width, w->swapChainExtent.height, 1}};
    vkCmdCopyImageToBuffer(cmd, w->swapChainImages[imageIndex],
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          w->rawReadbackBuffer[ctx->currentFrame], 1, &copy);
    VkBufferMemoryBarrier host_read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = w->rawReadbackBuffer[ctx->currentFrame],
        .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &host_read, 0, NULL);
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_READBACK, 1);
  }
}

void recordCommandBuffer(VulkanContext *ctx, ObsWindow *w,
                         uint32_t imageIndex) {
  if (ctx->profileWorkCountersEnabled) {
    ctx->profileDrawCalls = 0;
    ctx->profileIndirectDraws = 0;
    ctx->profileSubmittedVertices = 0;
    ctx->profileSubmittedIndices = 0;
    ctx->profileMeshTaskWorkgroups = 0;
    ctx->profileComputeDispatches = 0;
    ctx->profileComputeWorkgroups = 0;
    ctx->profileWorkStageDepth = 0;
    memset(ctx->profileDrawCallsByStage, 0,
           sizeof(ctx->profileDrawCallsByStage));
    memset(ctx->profileIndirectDrawsByStage, 0,
           sizeof(ctx->profileIndirectDrawsByStage));
    memset(ctx->profileVerticesByStage, 0, sizeof(ctx->profileVerticesByStage));
    memset(ctx->profileIndicesByStage, 0, sizeof(ctx->profileIndicesByStage));
    memset(ctx->profileMeshTaskWorkgroupsByStage, 0,
           sizeof(ctx->profileMeshTaskWorkgroupsByStage));
    memset(ctx->profileDispatchesByStage, 0,
           sizeof(ctx->profileDispatchesByStage));
    memset(ctx->profileComputeWorkgroupsByStage, 0,
           sizeof(ctx->profileComputeWorkgroupsByStage));
  }
  VkCommandBuffer cmd = w->commandBuffers[imageIndex];
  const uint32_t frame_slot = ctx->currentFrame;
  const VkImage depth_image = w->depthImage[frame_slot];
  const VkImageView depth_image_view = w->depthImageView[frame_slot];
  vkResetCommandBuffer(cmd, 0);

  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    fprintf(stderr, "Failed to begin command buffer\n");
    exit(EXIT_FAILURE);
  }

  if (ctx->profileTimestampPool) {
    uint32_t query_base = obs_gpu_profile_query_index(ctx->currentFrame,
                                                      OBS_GPU_PROFILE_FRAME, 0);
    vkCmdResetQueryPool(cmd, ctx->profileTimestampPool, query_base,
                        OBS_GPU_PROFILE_STAGE_COUNT * 2);
  }
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_FRAME, 0);

  if (w->render2DOnly) {
    if (!ctx->renderGraph)
      ctx->renderGraph = rg_create();
    if (!ctx->renderGraph) {
      fprintf(stderr, "Unable to allocate the 2D frame render graph\n");
      exit(EXIT_FAILURE);
    }
    rg_reset(ctx->renderGraph);
    RgResId swapchain = rg_import_acquired_image(
        ctx->renderGraph, "2DEditorSwapchain",
        w->swapChainImages[imageIndex], w->swapChainImageViews[imageIndex],
        w->swapChainImageFormat, w->swapChainExtent.width,
        w->swapChainExtent.height, VK_IMAGE_LAYOUT_UNDEFINED);
    RgPass *ui = rg_add_pass(ctx->renderGraph, "2DEditor");
    bool admitted = ui && swapchain != RG_INVALID_ID;
    if (admitted && ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
      RgResId multisample = rg_import_image_samples(
          ctx->renderGraph, "2DEditorMultisampleColor",
          w->multisampleColorImage[frame_slot],
          w->multisampleColorView[frame_slot], w->swapChainImageFormat,
          w->swapChainExtent.width, w->swapChainExtent.height,
          VK_IMAGE_LAYOUT_UNDEFINED, ctx->rasterizationSamples);
      admitted = multisample != RG_INVALID_ID &&
          rg_pass_color_attachment_resolve(ui, multisample, swapchain, true,
                                           ctx->clearColor);
    } else if (admitted) {
      admitted = rg_pass_color_attachment(ui, swapchain, true, ctx->clearColor);
    }
    if (admitted)
      rg_pass_execute(ui, execute_2d_editor_pass, ctx);
    if (!admitted ||
        !rg_execute_checked(ctx->renderGraph, cmd)) {
      fprintf(stderr, "Invalid 2D frame render graph: %s\n",
              rg_result_name(rg_status(ctx->renderGraph)));
      exit(EXIT_FAILURE);
    }
    finish_color_output(ctx, w, cmd, imageIndex);
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_FRAME, 1);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
      fprintf(stderr, "Failed to end 2D-only command buffer\n");
      exit(EXIT_FAILURE);
    }
    return;
  }

  // --- UPLOAD DYNAMIC GEOMETRY TO MEGABUFFER ---
  uint32_t dynVertCount = get_dynamic_vertex_count();
  uint32_t dynamic_frame_base =
      ctx->currentFrame * OBS_DYNAMIC_VERTEX_STRIDE(ctx);
  if (dynVertCount > 0) {
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_DYNAMIC_GEOMETRY, 0);
    VkBufferCopy copyRegion = {
        .srcOffset = (VkDeviceSize)dynamic_frame_base * sizeof(Vertex),
        .dstOffset =
            (VkDeviceSize)(ctx->megaVertexBufferOffset + dynamic_frame_base) *
            sizeof(Vertex),
        .size = dynVertCount * sizeof(Vertex)};
    vkCmdCopyBuffer(cmd, ctx->dynamicStagingBuffer, ctx->megaVertexBuffer, 1,
                    &copyRegion);

    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_DYNAMIC_GEOMETRY, 1);
  }

  if (!ctx->renderGraph) {
    ctx->renderGraph = rg_create();
  }
  if (!ctx->renderGraph) {
    fprintf(stderr, "Unable to allocate the frame render graph\n");
    exit(EXIT_FAILURE);
  }
  rg_reset(ctx->renderGraph);

  // Update dynamic mesh vertex offsets for the current frame
  if (scene->meshes.count > 0) {
    VkDeviceSize drawSize =
        (16384 + 4096) * sizeof(VkDrawIndexedIndirectCommand);
    VkDrawIndexedIndirectCommand *cmds =
        (VkDrawIndexedIndirectCommand *)((uint8_t *)
                                             ctx->srcIndirectBufferMapped +
                                         (ctx->currentFrame * drawSize));
    for (size_t i = 0; i < scene->meshes.count; i++) {
      Mesh *m = &scene->meshes.items[i];
      if (m->megaBaseVertex == UINT32_MAX &&
          m->dynamicBaseVertex != UINT32_MAX) {
        cmds[i].vertexOffset =
            (int32_t)(ctx->megaVertexBufferOffset +
                      dynamic_frame_base + m->dynamicBaseVertex);
      }
    }
  }

  update_cascade_matrices(ctx); // MUST happen before updateFrustumUBO!

  uint32_t f = ctx->currentFrame;

  // Lighting descriptors are device-global for historical reasons, while
  // transmission/HZB images belong to the native window being recorded.
  // Rebind those two descriptors immediately before recording so multiple
  // windows never sample one another's post-processing images.
  if (ctx->lightingSets[f] != VK_NULL_HANDLE) {
    uint32_t prevFrame = (f + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
    VkDescriptorImageInfo transmission = {
        .sampler = transmissionSampler,
        .imageView = w->transmissionView[f],
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo hzb = {.sampler = hzbSampler,
                                 .imageView = w->hzbView[prevFrame],
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ctx->lightingSets[f],
         .dstBinding = 6,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .pImageInfo = &transmission},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = ctx->lightingSets[f],
         .dstBinding = 7,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .pImageInfo = &hzb}};
    vkUpdateDescriptorSets(ctx->device, 2, writes, 0, NULL);
  }

  // --- GPU MESHLET BOUNDS REFIT PASS ---
  bool profile_bounds_refit = false;
  if ((ctx->profileTimestampPool || ctx->debugLabelsEnabled) &&
      refitBoundsPipeline != VK_NULL_HANDLE) {
    for (uint32_t i = 0; i < scene->meshes.count; i++) {
      const Mesh *m = &scene->meshes.items[i];
      if (m->visible && m->meshletCount > 0 && m->jointOffset >= 0) {
        profile_bounds_refit = true;
        break;
      }
    }
  }
  if (profile_bounds_refit)
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_BOUNDS_REFIT, 0);
  if (refitBoundsPipeline != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, refitBoundsPipeline);
    fill_gpu_addresses(ctx);

    for (uint32_t i = 0; i < scene->meshes.count; i++) {
      Mesh *m = &scene->meshes.items[i];
      if (!m->visible || m->meshletCount == 0 || m->jointOffset < 0)
        continue; // SKIPPED IF STATIC!

      pushConstants.meshIndex = i;
      vkCmdPushConstants(cmd, refitBoundsPipelineLayout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                         &pushConstants);
      uint32_t groups = (m->meshletCount + 31) / 32;
      obs_profile_count_dispatch(ctx, groups, 1, 1);
      vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Memory barrier to guarantee compute writes finish before the Task shader
    // reads
    VkMemoryBarrier boundsBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT, 0, 1,
                         &boundsBarrier, 0, NULL, 0, NULL);
  }
  if (profile_bounds_refit)
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_BOUNDS_REFIT, 1);
  uint32_t f_shadow = ctx->currentFrame + MAX_FRAMES_IN_FLIGHT;

  // 1. Import Resources
  RgResId swapchainImg = rg_import_acquired_image(
      ctx->renderGraph, "Swapchain", w->swapChainImages[imageIndex],
      w->swapChainImageViews[imageIndex], w->swapChainImageFormat,
      w->swapChainExtent.width, w->swapChainExtent.height,
      VK_IMAGE_LAYOUT_UNDEFINED);
  RgResId depthImg =
      rg_import_image(ctx->renderGraph, "Depth", depth_image, depth_image_view,
                      w->depthFormat, w->swapChainExtent.width,
                      w->swapChainExtent.height, VK_IMAGE_LAYOUT_UNDEFINED);
  RgResId colorAttachment = swapchainImg;
  RgResId depthAttachment = depthImg;
  if (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
    colorAttachment = rg_import_image_samples(
        ctx->renderGraph, "MultisampleColor",
        w->multisampleColorImage[frame_slot],
        w->multisampleColorView[frame_slot], w->swapChainImageFormat,
        w->swapChainExtent.width, w->swapChainExtent.height,
        VK_IMAGE_LAYOUT_UNDEFINED, ctx->rasterizationSamples);
    depthAttachment = rg_import_image_samples(
        ctx->renderGraph, "MultisampleDepth",
        w->multisampleDepthImage[frame_slot],
        w->multisampleDepthView[frame_slot], w->depthFormat,
        w->swapChainExtent.width, w->swapChainExtent.height,
        VK_IMAGE_LAYOUT_UNDEFINED, ctx->rasterizationSamples);
  }
  RgResId indirectBuf =
      rg_import_buffer(ctx->renderGraph, "IndirectBuffer", ctx->indirectBuffer);

  // 2. Culling is now 100% handled implicitly by the Task Shaders!
  // We completely bypassed the old compute pipeline.

  // 3. Define Shadow Pass (Creates the Atlas)
  uint32_t frameSlot = ctx->currentFrame;
  uint64_t shadowKey = 0;
  const bool shadow_map_enabled =
      obs_lighting_needs_shadow_map(CTX_LIGHTING(ctx), shadowsEnabled);
  const bool shadowKeyValid =
      obs_shadow_cache_build_key(scene, CTX_LIGHTING(ctx), shadow_map_enabled,
                                 ctx->indirectDrawCount, &shadowKey);
  ctx->shadowCacheReuse =
      shadowKeyValid && ctx->shadowCacheValid[frameSlot] &&
      ctx->shadowCacheKey[frameSlot] == shadowKey;
  ctx->shadowCachePending = true;
  ctx->shadowCachePendingInvalidate = !shadowKeyValid;
  ctx->shadowCachePendingFrameSlot = frameSlot;
  ctx->shadowCachePendingKey = shadowKey;
  RgResId shadowAtlasId = rg_import_image(
      ctx->renderGraph, "ShadowAtlas", ctx->shadowImage[frameSlot],
      ctx->shadowView[frameSlot], VK_FORMAT_D32_SFLOAT, 2048, 2048,
      ctx->shadowCacheReuse ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED);

  RgPass *shadowPass = rg_add_pass(ctx->renderGraph, "CascadedShadows");
  rg_pass_read_buffer(shadowPass, indirectBuf,
                      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                      VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  rg_pass_depth_attachment(shadowPass, shadowAtlasId,
                           !ctx->shadowCacheReuse);
  rg_pass_execute(shadowPass, execute_shadow_pass, ctx);
  // 4. Define Main Geometry Pass

  RgPass *mainPass = rg_add_pass(ctx->renderGraph, "MainForward");
  rg_pass_read_image(
      mainPass, shadowAtlasId, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  rg_pass_read_buffer(mainPass, indirectBuf,
                      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                      VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  if (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
    rg_pass_color_attachment_resolve(mainPass, colorAttachment, swapchainImg,
                                     true, ctx->clearColor);
    rg_pass_depth_attachment_resolve(mainPass, depthAttachment, depthImg,
                                     ctx->depthResolveMode, true);
  } else {
    rg_pass_color_attachment(mainPass, colorAttachment, true, ctx->clearColor);
    rg_pass_depth_attachment(mainPass, depthAttachment, true);
  }
  rg_pass_execute(mainPass, execute_main_pass, ctx);

  // 5. Execute the Graph!
  uint64_t cpu_render_graph_start = obs_frame_profile_command_scope_begin();
  if (!rg_execute_checked(ctx->renderGraph, cmd)) {
    /* Do not submit a partially rendered swapchain image after a graph
     * admission failure: Vulkan would observe undefined layouts. */
    fprintf(stderr, "Invalid frame render graph: %s\n",
            rg_result_name(rg_status(ctx->renderGraph)));
    exit(EXIT_FAILURE);
  }
  obs_frame_profile_command_scope_end(OBS_FRAME_COMMAND_RENDER_GRAPH,
                                      cpu_render_graph_start);

  // 6.  SCREEN-SPACE TRANSMISSION COPY, TRANSMISSION PASS & TRANSPARENT PASS
  // Always do the screen copy — compact.comp may have put transmission meshes
  // in stream 1 even if indirectDrawCount == opaqueMeshCount from the CPU side.
  // The copy is cheap (single blit) and the draw calls are guarded by indirect
  // counts.
  uint64_t cpu_transparency_start = obs_frame_profile_command_scope_begin();
  {
    const uint32_t sStride =
        20480u / 4u; // 5120 — must match compact.comp streamStride

    // --- Screen copy: swapchain opaque result -> transmissionImage ---
    VkImageMemoryBarrier copyBarriers[2] = {0};
    copyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copyBarriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    copyBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyBarriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    copyBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    copyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].image = w->swapChainImages[imageIndex];
    copyBarriers[0].subresourceRange =
        (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    copyBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copyBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    copyBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyBarriers[1].srcAccessMask = 0;
    copyBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[1].image = w->transmissionImage[ctx->currentFrame];
    copyBarriers[1].subresourceRange =
        (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
                         copyBarriers);

    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {w->swapChainExtent.width, w->swapChainExtent.height, 1}};
    vkCmdCopyImage(cmd, w->swapChainImages[imageIndex],
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   w->transmissionImage[ctx->currentFrame],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition: swapchain back to color attachment, transmissionImage to
    // shader read
    copyBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    copyBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    copyBarriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    copyBarriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyBarriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    copyBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 2, copyBarriers);

    // --- Transmission + Transparent pass ---
    VkRenderingAttachmentInfo colorAtt = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                         ? w->multisampleColorView[frame_slot]
                         : w->swapChainImageViews[imageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                           ? VK_RESOLVE_MODE_AVERAGE_BIT
                           : VK_RESOLVE_MODE_NONE,
        .resolveImageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                                ? w->swapChainImageViews[imageIndex]
                                : VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo depthAtt = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                         ? w->multisampleDepthView[frame_slot]
                         : depth_image_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .resolveMode = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                           ? ctx->depthResolveMode
                           : VK_RESOLVE_MODE_NONE,
        .resolveImageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                                ? depth_image_view
                                : VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo renderInfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                  .renderArea = {{0, 0}, w->swapChainExtent},
                                  .layerCount = 1,
                                  .colorAttachmentCount = 1,
                                  .pColorAttachments = &colorAtt,
                                  .pDepthAttachment = &depthAtt};

    profile_stage(ctx, cmd, OBS_GPU_PROFILE_TRANSPARENT_WIREFRAME, 0);
    vkCmdBeginRendering(cmd, &renderInfo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ctx->graphicsPipeline);
    VkDescriptorSet gltfSets[4] = {ctx->descriptorSets[ctx->currentFrame],
                                   ctx->bindlessSet, ctx->bindlessSet,
                                   ctx->lightingSets[ctx->currentFrame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 0, 4, gltfSets, 0, NULL);

    //  CRITICAL FIX: line_renderer_draw (executed in the main pass) corrupted
    //  the global
    // pushConstants state (setting meshIndex > 0 and pointing to the line
    // vertex buffer). We MUST restore the state to use indirect instances and
    // the mega buffer before drawing glass!
    pushConstants.cascadeIndex = -1;
    pushConstants.meshIndex = -1;
    fill_gpu_addresses(ctx);

    if (pfnCmdDrawMeshTasksEXT) {
      for (uint32_t stream = 1; stream <= 2; stream++) {
        for (uint32_t i = 0; i < scene->meshes.count; i++) {
          Mesh *m = &scene->meshes.items[i];
          if (!m->visible || m->meshletCount == 0)
            continue;
          if (m->megaBaseMeshlet == UINT32_MAX)
            continue;
          if (m->megaBaseVertex == UINT32_MAX)
            continue;

          bool isTransmission = m->transmissionFactor > 0.0f;
          bool isBlend = m->alpha_mode == 2 && !isTransmission;

          if (stream == 1 && !isTransmission)
            continue;
          if (stream == 2 && !isBlend)
            continue;
          if (m->wireframe)
            continue;

          pushConstants.meshIndex = i;
          vkCmdPushConstants(
              cmd, ctx->pipelineLayout,
              VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
              0, sizeof(PushConstants), &pushConstants);

          uint32_t taskGroups = (m->meshletCount + 31) / 32;
          obs_profile_count_mesh_tasks(ctx, taskGroups, 1, 1);
          pfnCmdDrawMeshTasksEXT(cmd, taskGroups, 1, 1);
        }
      }
    }

    // --- Stream 3: Wireframe Pass ---
    // Uses the legacy vertex pipeline (pbr.vert) with polygon mode LINE.
    // Mesh shaders do not support polygon mode overrides, so this path is
    // intentionally kept on the traditional vertex pipeline.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      graphicsPipelineWireframe);
    vkCmdSetLineWidth(cmd, 1.0f);
    // Negative depth bias floats wireframe lines in front of the surface,
    // eliminating z-fighting without modifying actual depth writes (which are
    // OFF).
    vkCmdSetDepthBias(cmd, -1.0f, 0.0f, -1.0f);
    vkCmdBindIndexBuffer(cmd, ctx->megaIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Re-bind descriptor sets: the graphicsPipelineWireframe uses
    // pipelineLayout (same as the main 3D layout) but the previous
    // skybox/transmission bind may have used a different layout, corrupting the
    // binding state.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 0, 4, gltfSets, 0, NULL);

    for (uint32_t i = 0; i < scene->meshes.count; i++) {
      Mesh *m = &scene->meshes.items[i];
      if (!m->visible || !m->wireframe)
        continue;

      // PVP wireframe: indices in the mega index buffer are LOCAL (0-based
      // within the mesh's vertex block), not absolute mega-buffer addresses.
      // vertexOffset = megaBaseVertex shifts every fetched index into the
      // correct absolute slot: gl_VertexIndex = localIndex + megaBaseVertex.
      //
      // For dynamic/non-indexed meshes: use the pre-filled linear identity
      // index buffer at firstIndex=0 with vertexOffset=megaBaseVertex,
      // giving gl_VertexIndex = k + megaBaseVertex = absolute vertex address.
      uint32_t index_count =
          (m->indexCount > 0) ? m->indexCount : m->vertexCount;
      uint32_t first_index =
          (m->megaBaseIndex != UINT32_MAX) ? m->megaBaseIndex : 0;
      int32_t vert_offset =
          (int32_t)((m->megaBaseVertex != UINT32_MAX)
                        ? m->megaBaseVertex
                        : (ctx->megaVertexBufferOffset +
                           dynamic_frame_base + m->dynamicBaseVertex));

      pushConstants.meshIndex = (int)i;
      pushConstants.cascadeIndex = -1;
      fill_gpu_addresses(ctx);
      vkCmdPushConstants(
          cmd, ctx->pipelineLayout,
          VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT |
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
          0, sizeof(PushConstants), &pushConstants);

      // firstInstance = i so pbr.vert gl_BaseInstanceARB fallback also works
      obs_profile_count_draw(ctx, 1, 0, index_count);
      vkCmdDrawIndexed(cmd, index_count, 1, first_index, vert_offset, i);
    }

    vkCmdEndRendering(cmd);
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_TRANSPARENT_WIREFRAME, 1);
  }
  obs_frame_profile_command_scope_end(OBS_FRAME_COMMAND_TRANSPARENCY,
                                      cpu_transparency_start);

  // 7.  2D UI PASS (Always draws last, OVER the glass!)
  VkRenderingAttachmentInfo uiColorAtt = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                       ? w->multisampleColorView[frame_slot]
                       : w->swapChainImageViews[imageIndex],
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .resolveMode = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                         ? VK_RESOLVE_MODE_AVERAGE_BIT
                         : VK_RESOLVE_MODE_NONE,
      .resolveImageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                              ? w->swapChainImageViews[imageIndex]
                              : VK_NULL_HANDLE,
      .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
  VkRenderingAttachmentInfo uiDepthAtt = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                       ? w->multisampleDepthView[frame_slot]
                       : depth_image_view,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .resolveMode = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                         ? ctx->depthResolveMode
                         : VK_RESOLVE_MODE_NONE,
      .resolveImageView = (ctx->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
                              ? depth_image_view
                              : VK_NULL_HANDLE,
      .resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
  VkRenderingInfo uiRenderInfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                  .renderArea = {{0, 0}, w->swapChainExtent},
                                  .layerCount = 1,
                                  .colorAttachmentCount = 1,
                                  .pColorAttachments = &uiColorAtt,
                                  .pDepthAttachment = &uiDepthAtt};

  uint64_t cpu_ui_start = obs_frame_profile_command_scope_begin();
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_UI, 0);
  vkCmdBeginRendering(cmd, &uiRenderInfo);
  renderer2D_draw(cmd);
  vkCmdEndRendering(cmd);
  profile_stage(ctx, cmd, OBS_GPU_PROFILE_UI, 1);
  obs_frame_profile_command_scope_end(OBS_FRAME_COMMAND_UI, cpu_ui_start);

  finish_color_output(ctx, w, cmd, imageIndex);

  // 8.  HZB DOWNSAMPLE PASS (Runs asynchronously at the very end of the frame)
  if (hzbReducePipeline != VK_NULL_HANDLE) {
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_HZB, 0);
    // Transition main depth buffer to read-only for Mip 0 sampling
    VkImageMemoryBarrier depthBar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_image,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &depthBar);

    // Transition entire HZB image to GENERAL for compute writes
    VkImageMemoryBarrier hzbBar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = w->hzbImage[ctx->currentFrame],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, w->hzbMipCount, 0,
                             1},
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &hzbBar);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hzbReducePipeline);

    uint32_t hzbBaseW =
        w->swapChainExtent.width > 1 ? w->swapChainExtent.width / 2 : 1;
    uint32_t hzbBaseH =
        w->swapChainExtent.height > 1 ? w->swapChainExtent.height / 2 : 1;
    uint32_t mipW = hzbBaseW;
    uint32_t mipH = hzbBaseH;

    for (uint32_t m = 0; m < w->hzbMipCount; m++) {
      ObsGpuProfileStage mip_stage =
          (ObsGpuProfileStage)(OBS_GPU_PROFILE_HZB_MIP_0 + m);
      profile_stage(ctx, cmd, mip_stage, 0);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              hzbReducePipelineLayout, 0, 1,
                              &w->hzbSets[ctx->currentFrame][m], 0, NULL);

      float invSize[2] = {1.0f / (float)mipW, 1.0f / (float)mipH};
      vkCmdPushConstants(cmd, hzbReducePipelineLayout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 2,
                         invSize);
      uint32_t groups_x = (mipW + 15) / 16;
      uint32_t groups_y = (mipH + 15) / 16;
      obs_profile_count_dispatch(ctx, groups_x, groups_y, 1);
      vkCmdDispatch(cmd, groups_x, groups_y, 1);

      // Wait for this mip to finish writing before the next mip reads from it
      VkImageMemoryBarrier mipSync = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = w->hzbImage[ctx->currentFrame],
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1},
          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                           NULL, 1, &mipSync);
      profile_stage(ctx, cmd, mip_stage, 1);

      if (mipW > 1)
        mipW /= 2;
      if (mipH > 1)
        mipH /= 2;
    }

    // Publish every generated mip to the next frame's task-shader reads.
    VkImageMemoryBarrier hzbReadBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = w->hzbImage[ctx->currentFrame],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, w->hzbMipCount, 0,
                             1},
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT, 0, 0, NULL, 0,
                         NULL, 1, &hzbReadBarrier);

    // Revert depthImage so it's ready for the next frame's clear and render
    // pass
    depthBar.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthBar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthBar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL,
                         0, NULL, 1, &depthBar);
    profile_stage(ctx, cmd, OBS_GPU_PROFILE_HZB, 1);
  }

  profile_stage(ctx, cmd, OBS_GPU_PROFILE_FRAME, 1);
  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    fprintf(stderr, "Failed to record command buffer\n");
    exit(EXIT_FAILURE);
  }
}

void shadowCacheFrameSubmitted(VulkanContext *ctx) {
  if (!ctx || !ctx->shadowCachePending)
    return;
  const uint32_t slot = ctx->shadowCachePendingFrameSlot;
  if (ctx->shadowCachePendingInvalidate) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
      ctx->shadowCacheValid[i] = false;
  } else if (slot < MAX_FRAMES_IN_FLIGHT) {
    ctx->shadowCacheKey[slot] = ctx->shadowCachePendingKey;
    ctx->shadowCacheValid[slot] = true;
  }
  ctx->shadowCachePending = false;
}

/// Draw frame

// ---------------------------------------------------------------------------
// Runtime GLTF deferred load queue.
// The UI drop handler calls gltf_load_enqueue().  drawFrame() drains it here,
// AFTER all in-flight fences are waited on, so vkQueueSubmit inside load_gltf
// can never race with a pending frame submission.
// ---------------------------------------------------------------------------
#define MAX_PENDING_GLTF_LOADS 16
static char s_pending_gltf_paths[MAX_PENDING_GLTF_LOADS][512];
static int s_pending_gltf_count = 0;

void gltf_load_enqueue(const char *filepath) {
  if (s_pending_gltf_count < MAX_PENDING_GLTF_LOADS) {
    strncpy(s_pending_gltf_paths[s_pending_gltf_count], filepath, 511);
    s_pending_gltf_paths[s_pending_gltf_count][511] = '\0';
    s_pending_gltf_count++;
  }
}

#include "gltf_loader.h"

void drawFrame(VulkanContext *ctx) {
  ObsWindow *w = ctx->primary; // legacy single-window loop

  uint32_t frameIndex = ctx->currentFrame;
  VkFence fence = ctx->inFlightFences[frameIndex];

  // Wait for ALL in-flight fences first so the queue is truly idle
  // before we potentially call load_gltf (which does vkQueueWaitIdle).
  if (vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX) !=
      VK_SUCCESS) {
    fprintf(stderr, "Obsidian: legacy drawFrame fence failed\n");
    abort();
  }

  // Drain deferred GLTF loads at the only safe inter-frame point.
  if (s_pending_gltf_count > 0) {
    extern Scene *scene;
    // Wait for all frames, not just the current slot, because
    // load_gltf calls vkQueueWaitIdle which stalls the whole queue.
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
      if (ctx->inFlightFences[f] != VK_NULL_HANDLE)
        vkWaitForFences(ctx->device, 1, &ctx->inFlightFences[f], VK_TRUE,
                        UINT64_MAX);
    for (int i = 0; i < s_pending_gltf_count; i++) {
      fprintf(stdout, "[DEFERRED LOAD] %s\n", s_pending_gltf_paths[i]);
      load_gltf(s_pending_gltf_paths[i], scene);
      markMeshesSSBODirty(ctx);
    }
    s_pending_gltf_count = 0;
  }

  uint32_t imageIndex;
  VkResult result = vkAcquireNextImageKHR(
      ctx->device, w->swapChain, UINT64_MAX,
      ctx->imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapChain(ctx, w);
    return;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    fprintf(stderr, "Failed to acquire swap chain image\n");
    exit(EXIT_FAILURE);
  }

  if (w->imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    vkWaitForFences(ctx->device, 1, &w->imagesInFlight[imageIndex], VK_TRUE,
                    UINT64_MAX);
  w->imagesInFlight[imageIndex] = fence;

  updateUniformBuffer(ctx);
  begin_frame();
  /* Legacy rendering also consumes only its proven-quiescent slot. */
  if (!obs_sdf_sync_frame(frameIndex)) {
    fprintf(stderr, "Obsidian: legacy SDF slot upload rejected\n");
    abort();
  }
  recordCommandBuffer(ctx, w, imageIndex);

  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &ctx->imageAvailableSemaphores[frameIndex],
      .pWaitDstStageMask = waitStages,
      .commandBufferCount = 1,
      .pCommandBuffers = &w->commandBuffers[imageIndex],
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &w->renderFinishedSemaphores[imageIndex]};
  vkResetFences(ctx->device, 1, &fence);
  if (vkQueueSubmit(ctx->graphicsQueue, 1, &si, fence) != VK_SUCCESS) {
    fprintf(stderr, "Failed to submit draw command buffer\n");
    exit(EXIT_FAILURE);
  }
  shadowCacheFrameSubmitted(ctx);
  w->hzbValid[frameIndex] = true;

  VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                         .waitSemaphoreCount = 1,
                         .pWaitSemaphores =
                             &w->renderFinishedSemaphores[imageIndex],
                         .swapchainCount = 1,
                         .pSwapchains = &w->swapChain,
                         .pImageIndices = &imageIndex};
  result = vkQueuePresentKHR(ctx->graphicsQueue, &pi);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    recreateSwapChain(ctx, w);
  else if (result != VK_SUCCESS) {
    fprintf(stderr, "Failed to present swap chain image\n");
    exit(EXIT_FAILURE);
  }

  ctx->currentFrame = (ctx->currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

/// Swap Chain recreation

void cleanupSwapChainResources(VulkanContext *ctx, ObsWindow *w,
                               VkSwapchainKHR oldSwapchain) {
  vkDeviceWaitIdle(ctx->device);

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (w->transmissionView[i]) {
      vkDestroyImageView(ctx->device, w->transmissionView[i], NULL);
      w->transmissionView[i] = VK_NULL_HANDLE;
    }
    if (w->transmissionImage[i]) {
      vkDestroyImage(ctx->device, w->transmissionImage[i], NULL);
      w->transmissionImage[i] = VK_NULL_HANDLE;
    }
    if (w->transmissionMemory[i]) {
      vkFreeMemory(ctx->device, w->transmissionMemory[i], NULL);
      w->transmissionMemory[i] = VK_NULL_HANDLE;
    }
  }

  if (w->commandBuffers && ctx->commandPool) {
    vkFreeCommandBuffers(ctx->device, ctx->commandPool, w->swapChainImageCount,
                         w->commandBuffers);
    free(w->commandBuffers);
    w->commandBuffers = NULL;
  }
  if (w->swapChainImageViews) {
    for (uint32_t i = 0; i < w->swapChainImageCount; i++)
      vkDestroyImageView(ctx->device, w->swapChainImageViews[i], NULL);
    free(w->swapChainImageViews);
    w->swapChainImageViews = NULL;
  }
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (w->hzbView[i]) {
      vkDestroyImageView(ctx->device, w->hzbView[i], NULL);
      w->hzbView[i] = VK_NULL_HANDLE;
    }
    if (w->hzbImage[i]) {
      vkDestroyImage(ctx->device, w->hzbImage[i], NULL);
      w->hzbImage[i] = VK_NULL_HANDLE;
    }
    if (w->hzbMemory[i]) {
      vkFreeMemory(ctx->device, w->hzbMemory[i], NULL);
      w->hzbMemory[i] = VK_NULL_HANDLE;
    }
    for (uint32_t m = 0; m < 16; m++) {
      if (w->hzbMipViews[i][m]) {
        vkDestroyImageView(ctx->device, w->hzbMipViews[i][m], NULL);
        w->hzbMipViews[i][m] = VK_NULL_HANDLE;
      }
    }
  }
  if (w->hzbDescriptorPool) {
    vkDestroyDescriptorPool(ctx->device, w->hzbDescriptorPool, NULL);
    w->hzbDescriptorPool = VK_NULL_HANDLE;
  }

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    if (w->multisampleColorView[i]) {
      vkDestroyImageView(ctx->device, w->multisampleColorView[i], NULL);
      w->multisampleColorView[i] = VK_NULL_HANDLE;
    }
    if (w->multisampleColorImage[i]) {
      vkDestroyImage(ctx->device, w->multisampleColorImage[i], NULL);
      w->multisampleColorImage[i] = VK_NULL_HANDLE;
    }
    if (w->multisampleColorMemory[i]) {
      vkFreeMemory(ctx->device, w->multisampleColorMemory[i], NULL);
      w->multisampleColorMemory[i] = VK_NULL_HANDLE;
    }
    if (w->multisampleDepthView[i]) {
      vkDestroyImageView(ctx->device, w->multisampleDepthView[i], NULL);
      w->multisampleDepthView[i] = VK_NULL_HANDLE;
    }
    if (w->multisampleDepthImage[i]) {
      vkDestroyImage(ctx->device, w->multisampleDepthImage[i], NULL);
      w->multisampleDepthImage[i] = VK_NULL_HANDLE;
    }
    if (w->multisampleDepthMemory[i]) {
      vkFreeMemory(ctx->device, w->multisampleDepthMemory[i], NULL);
      w->multisampleDepthMemory[i] = VK_NULL_HANDLE;
    }
    if (w->depthImageView[i]) {
      vkDestroyImageView(ctx->device, w->depthImageView[i], NULL);
      w->depthImageView[i] = VK_NULL_HANDLE;
    }
    if (w->depthImage[i]) {
      vkDestroyImage(ctx->device, w->depthImage[i], NULL);
      w->depthImage[i] = VK_NULL_HANDLE;
    }
    if (w->depthImageMemory[i]) {
      vkFreeMemory(ctx->device, w->depthImageMemory[i], NULL);
      w->depthImageMemory[i] = VK_NULL_HANDLE;
    }
  }
  if (oldSwapchain != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(ctx->device, oldSwapchain, NULL);
  if (w->swapChainImages) {
    free(w->swapChainImages);
    w->swapChainImages = NULL;
  }
  w->swapChainImageCount = 0;
}

void cleanupSwapChain(VulkanContext *ctx, ObsWindow *w) {
  cleanupSwapChainResources(ctx, w, w->swapChain);
}

void recreateSwapChain(VulkanContext *ctx, ObsWindow *w) {
  int fw = 0, fh = 0;
  glfwGetFramebufferSize(w->window, &fw, &fh);
  while (fw == 0 || fh == 0) {
    glfwGetFramebufferSize(w->window, &fw, &fh);
    glfwWaitEvents();
  }

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    if (ctx->inFlightFences[i] != VK_NULL_HANDLE)
      vkWaitForFences(ctx->device, 1, &ctx->inFlightFences[i], VK_TRUE,
                      UINT64_MAX);

  VkSwapchainKHR oldSwapchain = w->swapChain;

  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, w->surface,
                                            &caps);

  uint32_t imgCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
    imgCount = caps.maxImageCount;

  VkExtent2D ext;
  if (caps.currentExtent.width != UINT32_MAX) {
    ext = caps.currentExtent;
  } else {
    ext.width = (uint32_t)fw;
    ext.height = (uint32_t)fh;
    ext.width =
        ext.width < caps.minImageExtent.width   ? caps.minImageExtent.width
        : ext.width > caps.maxImageExtent.width ? caps.maxImageExtent.width
                                                : ext.width;
    ext.height =
        ext.height < caps.minImageExtent.height   ? caps.minImageExtent.height
        : ext.height > caps.maxImageExtent.height ? caps.maxImageExtent.height
                                                  : ext.height;
  }

  VkSwapchainCreateInfoKHR ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = w->surface,
      .minImageCount = imgCount,
      .imageFormat = w->swapChainImageFormat,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = ext,
      .imageArrayLayers = 1,
      //  FIX: Allow Vulkan to copy FROM the swapchain for our screen-space
      //  refraction!
      .imageUsage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = oldSwapchain};
  VkSwapchainKHR newSwapchain;
  if (vkCreateSwapchainKHR(ctx->device, &ci, NULL, &newSwapchain) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to recreate swap chain\n");
    exit(EXIT_FAILURE);
  }

  cleanupSwapChainResources(ctx, w, oldSwapchain);

  w->swapChain = newSwapchain;
  w->swapChainExtent = ext;

  vkGetSwapchainImagesKHR(ctx->device, w->swapChain, &w->swapChainImageCount,
                          NULL);
  w->swapChainImages = malloc(w->swapChainImageCount * sizeof(VkImage));
  vkGetSwapchainImagesKHR(ctx->device, w->swapChain, &w->swapChainImageCount,
                          w->swapChainImages);

  createImageViews(ctx, w);
  createDepthResources(ctx, w);
  createCommandBuffers(ctx, w);

  camera.aspect_ratio =
      (float)w->swapChainExtent.width / (float)w->swapChainExtent.height;
  glm_perspective(glm_rad(camera.fov), camera.aspect_ratio, camera.near_plane,
                  camera.far_plane, camera.projection_matrix);
  camera.projection_matrix[1][1] *= -1.0f;
  obs_resize_reset_after_rebuild(&w->swapchainOutOfDate,
                                 &w->framebufferResized);
}

/// Cleanup

void cleanup(VulkanContext *ctx) {
  /* Retire Monad references and callbacks before GPU/scene destruction. */
  obs_monad_engine_stop();
  obs_editor_frames_shutdown();
  const VkResult shutdown_idle = vkDeviceWaitIdle(ctx->device);
  obs_retire_unconfirmed_upload(shutdown_idle == VK_SUCCESS);
  if (shutdown_idle == VK_SUCCESS &&
      obs_geometry_async_busy(&s_geometry_async) &&
      !obs_geometry_async_retire_quiesced(&s_geometry_async,
                                          s_geometry_async.ticket, true))
    fprintf(stderr,
            "[GEOMETRY ASYNC] failed to retire upload after device idle\n");
  if (shutdown_idle == VK_SUCCESS)
    s_geometry_upload_poisoned = false;

  renderer_shutdown();
  line_renderer_shutdown();
  obs_scene_buffers_cleanup();
  texture_pool_cleanup(ctx);

  /* descriptor resources */
  if (ctx->descriptorSetLayout2D) {
    vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptorSetLayout2D, NULL);
    ctx->descriptorSetLayout2D = VK_NULL_HANDLE;
  }
  if (ctx->descriptorPool2D) {
    vkDestroyDescriptorPool(ctx->device, ctx->descriptorPool2D, NULL);
    ctx->descriptorPool2D = VK_NULL_HANDLE;
  }
  if (ctx->bindlessSetLayout) {
    vkDestroyDescriptorSetLayout(ctx->device, ctx->bindlessSetLayout, NULL);
    ctx->bindlessSetLayout = VK_NULL_HANDLE;
  }
  if (ctx->bindlessPool) {
    vkDestroyDescriptorPool(ctx->device, ctx->bindlessPool, NULL);
    ctx->bindlessPool = VK_NULL_HANDLE;
  }
  /* lighting UBO */
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (ctx->lightingUBOMapped[i]) {
      vkUnmapMemory(ctx->device, ctx->lightingUBOMemory[i]);
      ctx->lightingUBOMapped[i] = NULL;
    }
    if (ctx->lightingUBO[i]) {
      vkDestroyBuffer(ctx->device, ctx->lightingUBO[i], NULL);
      ctx->lightingUBO[i] = VK_NULL_HANDLE;
    }
    if (ctx->lightingUBOMemory[i]) {
      vkFreeMemory(ctx->device, ctx->lightingUBOMemory[i], NULL);
      ctx->lightingUBOMemory[i] = VK_NULL_HANDLE;
    }
  }
  if (ctx->lightingSetLayout) {
    vkDestroyDescriptorSetLayout(ctx->device, ctx->lightingSetLayout, NULL);
    ctx->lightingSetLayout = VK_NULL_HANDLE;
  }
  if (ctx->lightingPool) {
    vkDestroyDescriptorPool(ctx->device, ctx->lightingPool, NULL);
    ctx->lightingPool = VK_NULL_HANDLE;
  }
  /* IBL */
  destroyIBL(ctx);

  /* per-window resources: every live window's command buffers, sync
     objects, swapchain and surface */
  for (ObsWindow *w = ctx->windows; w;) {
    ObsWindow *next = w->next;
    if (w->commandBuffers && ctx->commandPool) {
      vkFreeCommandBuffers(ctx->device, ctx->commandPool,
                           w->swapChainImageCount, w->commandBuffers);
      free(w->commandBuffers);
      w->commandBuffers = NULL;
    }
    if (w->renderFinishedSemaphores) {
      for (uint32_t i = 0; i < w->swapChainImageCount; i++)
        if (w->renderFinishedSemaphores[i])
          vkDestroySemaphore(ctx->device, w->renderFinishedSemaphores[i], NULL);
      free(w->renderFinishedSemaphores);
      w->renderFinishedSemaphores = NULL;
    }
    free(w->imagesInFlight);
    w->imagesInFlight = NULL;
    cleanupSwapChainResources(ctx, w, w->swapChain);
    w->swapChain = VK_NULL_HANDLE;
    if (w->headless) {
      for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (w->rawReadbackMapped[i])
          vkUnmapMemory(ctx->device, w->rawReadbackMemory[i]);
        if (w->rawReadbackBuffer[i])
          vkDestroyBuffer(ctx->device, w->rawReadbackBuffer[i], NULL);
        if (w->rawReadbackMemory[i])
          vkFreeMemory(ctx->device, w->rawReadbackMemory[i], NULL);
        if (w->rawColorImage[i])
          vkDestroyImage(ctx->device, w->rawColorImage[i], NULL);
        if (w->rawColorMemory[i])
          vkFreeMemory(ctx->device, w->rawColorMemory[i], NULL);
      }
    }
    if (w->surface) {
      vkDestroySurfaceKHR(ctx->instance, w->surface, NULL);
      w->surface = VK_NULL_HANDLE;
    }
    if (w->window) {
      glfwDestroyWindow(w->window);
      w->window = NULL;
    }
    if (w->headless)
      free(w);
    w = next;
  }

  /* The shadow atlas is device-global, not a swapchain resource.  It used
   * to be destroyed as part of the first window's cleanup, which made
   * another live window sample a dead image and also leaked it on shutdown
   * after the per-window refactor. */
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (ctx->shadowView[i]) {
      vkDestroyImageView(ctx->device, ctx->shadowView[i], NULL);
      ctx->shadowView[i] = VK_NULL_HANDLE;
    }
    if (ctx->shadowImage[i]) {
      vkDestroyImage(ctx->device, ctx->shadowImage[i], NULL);
      ctx->shadowImage[i] = VK_NULL_HANDLE;
    }
    if (ctx->shadowMemory[i]) {
      vkFreeMemory(ctx->device, ctx->shadowMemory[i], NULL);
      ctx->shadowMemory[i] = VK_NULL_HANDLE;
    }
  }
  if (ctx->shadowSampler) {
    vkDestroySampler(ctx->device, ctx->shadowSampler, NULL);
    ctx->shadowSampler = VK_NULL_HANDLE;
  }

  /* global frame-slot sync */
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (ctx->imageAvailableSemaphores && ctx->imageAvailableSemaphores[i])
      vkDestroySemaphore(ctx->device, ctx->imageAvailableSemaphores[i], NULL);
    if (ctx->inFlightFences[i])
      vkDestroyFence(ctx->device, ctx->inFlightFences[i], NULL);
  }
  free(ctx->imageAvailableSemaphores);
  free(ctx->inFlightFences);

  if (ctx->commandPool) {
    vkDestroyCommandPool(ctx->device, ctx->commandPool, NULL);
    ctx->commandPool = VK_NULL_HANDLE;
  }

  /* pipelines */
#define DESTROY_PIPELINE(p)                                                    \
  if (ctx->p) {                                                                \
    vkDestroyPipeline(ctx->device, ctx->p, NULL);                              \
    ctx->p = VK_NULL_HANDLE;                                                   \
  }
#define DESTROY_LAYOUT(l)                                                      \
  if (ctx->l) {                                                                \
    vkDestroyPipelineLayout(ctx->device, ctx->l, NULL);                        \
    ctx->l = VK_NULL_HANDLE;                                                   \
  }
  /* pipelineIndirectSolid and pipelineIndirectTextured alias
     graphicsPipeline[0] — null them out first so DESTROY_PIPELINE doesn't
     double-free.                    */
  pipelineIndirectSolid = VK_NULL_HANDLE;
  pipelineIndirectTextured = VK_NULL_HANDLE;

  /* graphicsPipelineTextured2D aliases graphicsPipeline2D (pipelines[2]).
     Keep the legacy 3D pipeline (pipelines[6]) distinct and destroy it. */
  ctx->graphicsPipelineTextured2D = VK_NULL_HANDLE;

  DESTROY_PIPELINE(graphicsPipeline)           /* pipelines[0] */
  DESTROY_PIPELINE(graphicsPipelineTextured3D) /* pipelines[6] */
  DESTROY_PIPELINE(graphicsPipelineLine)       /* pipelines[1] */
  DESTROY_PIPELINE(graphicsPipeline2D)         /* pipelines[2] */

  if (graphicsPipelineWireframe) {
    vkDestroyPipeline(ctx->device, graphicsPipelineWireframe, NULL);
    graphicsPipelineWireframe = VK_NULL_HANDLE;
  }

  /* These all alias other layouts — null before destroy to prevent double-free
   */
  ctx->pipelineLayoutLine = VK_NULL_HANDLE;
  ctx->pipelineLayoutTextured3D = VK_NULL_HANDLE; /* aliases pipelineLayout */
  ctx->pipelineLayoutIndirect = VK_NULL_HANDLE;   /* aliases pipelineLayout */
  ctx->pipelineLayout2D = VK_NULL_HANDLE; /* aliases pipelineLayoutTextured2D */

  DESTROY_LAYOUT(pipelineLayout)           /* the real 3D PBR layout */
  DESTROY_LAYOUT(pipelineLayoutTextured2D) /* the real 2D layout */

  if (skyboxPipeline) {
    vkDestroyPipeline(ctx->device, skyboxPipeline, NULL);
    skyboxPipeline = VK_NULL_HANDLE;
  }
  if (skyboxPipelineLayout) {
    vkDestroyPipelineLayout(ctx->device, skyboxPipelineLayout, NULL);
    skyboxPipelineLayout = VK_NULL_HANDLE;
  }
  if (sdfPipeline) {
    vkDestroyPipeline(ctx->device, sdfPipeline, NULL);
    sdfPipeline = VK_NULL_HANDLE;
  }
  if (shadowPipeline) {
    vkDestroyPipeline(ctx->device, shadowPipeline, NULL);
    shadowPipeline = VK_NULL_HANDLE;
  }
  if (refitBoundsPipeline) {
    vkDestroyPipeline(ctx->device, refitBoundsPipeline, NULL);
    refitBoundsPipeline = VK_NULL_HANDLE;
  }
  if (refitBoundsPipelineLayout) {
    vkDestroyPipelineLayout(ctx->device, refitBoundsPipelineLayout, NULL);
    refitBoundsPipelineLayout = VK_NULL_HANDLE;
  }

  if (hzbReducePipeline) {
    vkDestroyPipeline(ctx->device, hzbReducePipeline, NULL);
    hzbReducePipeline = VK_NULL_HANDLE;
  }
  if (hzbReducePipelineLayout) {
    vkDestroyPipelineLayout(ctx->device, hzbReducePipelineLayout, NULL);
    hzbReducePipelineLayout = VK_NULL_HANDLE;
  }
  if (hzbSetLayout) {
    vkDestroyDescriptorSetLayout(ctx->device, hzbSetLayout, NULL);
    hzbSetLayout = VK_NULL_HANDLE;
  }
  if (hzbSampler) {
    vkDestroySampler(ctx->device, hzbSampler, NULL);
    hzbSampler = VK_NULL_HANDLE;
  }
  if (transmissionSampler) {
    vkDestroySampler(ctx->device, transmissionSampler, NULL);
    transmissionSampler = VK_NULL_HANDLE;
  }

#undef DESTROY_PIPELINE
#undef DESTROY_LAYOUT

  destroyUploadStagingBuffer(ctx);
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (jointSSBOMapped[i]) {
      vkUnmapMemory(ctx->device, jointSSBOMemory[i]);
      jointSSBOMapped[i] = NULL;
    }
    if (jointSSBO[i]) {
      vkDestroyBuffer(ctx->device, jointSSBO[i], NULL);
      jointSSBO[i] = VK_NULL_HANDLE;
    }
    if (jointSSBOMemory[i]) {
      vkFreeMemory(ctx->device, jointSSBOMemory[i], NULL);
      jointSSBOMemory[i] = VK_NULL_HANDLE;
    }
  }

  if (pipelineCache != VK_NULL_HANDLE) {
    size_t cacheSize = 0;
    if (vkGetPipelineCacheData(ctx->device, pipelineCache, &cacheSize, NULL) ==
            VK_SUCCESS &&
        cacheSize > 0) {
      void *cacheData = malloc(cacheSize);
      if (cacheData &&
          vkGetPipelineCacheData(ctx->device, pipelineCache, &cacheSize,
                                 cacheData) == VK_SUCCESS) {
        const char *cachePath = get_pipeline_cache_path();
        if (save_pipeline_cache_atomically(cachePath, cacheData, cacheSize)) {
          fprintf(stdout,
                  "\033[32m[PIPELINE] Saved %zu bytes to cache\033[0m\n",
                  cacheSize);
        }
      }
      free(cacheData);
    }
    vkDestroyPipelineCache(ctx->device, pipelineCache, NULL);
    pipelineCache = VK_NULL_HANDLE;
  }

#define CLEANUP_BUFFER(b, m)                                                   \
  if (ctx->b) {                                                                \
    vkDestroyBuffer(ctx->device, ctx->b, NULL);                                \
    ctx->b = VK_NULL_HANDLE;                                                   \
  }                                                                            \
  if (ctx->m) {                                                                \
    vkFreeMemory(ctx->device, ctx->m, NULL);                                   \
    ctx->m = VK_NULL_HANDLE;                                                   \
  }

  /* 2D vertex buffer - one per frame */
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    CLEANUP_BUFFER(vertexBuffer2D[i], vertexBufferMemory2D[i]);
  }

  /* per-frame uniform buffers - unmap then destroy */
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (ctx->uboMapped[i]) {
      vkUnmapMemory(ctx->device, ctx->uniformBuffersMemory[i]);
      ctx->uboMapped[i] = NULL;
    }
    CLEANUP_BUFFER(uniformBuffers[i], uniformBuffersMemory[i]);
  }
  if (ctx->descriptorPool) {
    vkDestroyDescriptorPool(ctx->device, ctx->descriptorPool, NULL);
    ctx->descriptorPool = VK_NULL_HANDLE;
  }
  if (ctx->descriptorSetLayout) {
    vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptorSetLayout, NULL);
    ctx->descriptorSetLayout = VK_NULL_HANDLE;
  }

  /* mega vertex buffer */
  CLEANUP_BUFFER(megaVertexBuffer, megaVertexBufferMemory);

  /* mega index buffer */
  CLEANUP_BUFFER(megaIndexBuffer, megaIndexBufferMemory);

  /* Meshlet geometry pools are module globals, not VulkanContext members. */
  if (megaMeshletBuffer) {
    vkDestroyBuffer(ctx->device, megaMeshletBuffer, NULL);
    megaMeshletBuffer = VK_NULL_HANDLE;
  }
  if (megaMeshletBufferMemory) {
    vkFreeMemory(ctx->device, megaMeshletBufferMemory, NULL);
    megaMeshletBufferMemory = VK_NULL_HANDLE;
  }
  if (megaMeshletBoundsBuffer) {
    vkDestroyBuffer(ctx->device, megaMeshletBoundsBuffer, NULL);
    megaMeshletBoundsBuffer = VK_NULL_HANDLE;
  }
  if (megaMeshletBoundsBufferMemory) {
    vkFreeMemory(ctx->device, megaMeshletBoundsBufferMemory, NULL);
    megaMeshletBoundsBufferMemory = VK_NULL_HANDLE;
  }
  if (megaMeshletVertexBuffer) {
    vkDestroyBuffer(ctx->device, megaMeshletVertexBuffer, NULL);
    megaMeshletVertexBuffer = VK_NULL_HANDLE;
  }
  if (megaMeshletVertexMemory) {
    vkFreeMemory(ctx->device, megaMeshletVertexMemory, NULL);
    megaMeshletVertexMemory = VK_NULL_HANDLE;
  }
  if (megaMeshletTriangleBuffer) {
    vkDestroyBuffer(ctx->device, megaMeshletTriangleBuffer, NULL);
    megaMeshletTriangleBuffer = VK_NULL_HANDLE;
  }
  if (megaMeshletTriangleMemory) {
    vkFreeMemory(ctx->device, megaMeshletTriangleMemory, NULL);
    megaMeshletTriangleMemory = VK_NULL_HANDLE;
  }

  /* meshlet skins and dynamic bounds (these are globals, not in ctx!) */
  if (megaMeshletSkinBuffer) {
    vkDestroyBuffer(ctx->device, megaMeshletSkinBuffer, NULL);
    megaMeshletSkinBuffer = VK_NULL_HANDLE;
  }
  if (megaMeshletSkinBufferMemory) {
    vkFreeMemory(ctx->device, megaMeshletSkinBufferMemory, NULL);
    megaMeshletSkinBufferMemory = VK_NULL_HANDLE;
  }
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (dynamicBoundsBuffer[i]) {
      vkDestroyBuffer(ctx->device, dynamicBoundsBuffer[i], NULL);
      dynamicBoundsBuffer[i] = VK_NULL_HANDLE;
    }
    if (dynamicBoundsBufferMemory[i]) {
      vkFreeMemory(ctx->device, dynamicBoundsBufferMemory[i], NULL);
      dynamicBoundsBufferMemory[i] = VK_NULL_HANDLE;
    }
  }

  /* morph buffers */
  CLEANUP_BUFFER(megaMorphBuffer, megaMorphBufferMemory);
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (ctx->morphWeightMapped[i]) {
      vkUnmapMemory(ctx->device, ctx->morphWeightMemory[i]);
      ctx->morphWeightMapped[i] = NULL;
    }
    CLEANUP_BUFFER(morphWeightBuffer[i], morphWeightMemory[i]);
  }

  /* dynamic buffers */
  if (ctx->dynamicStagingMapped) {
    vkUnmapMemory(ctx->device, ctx->dynamicStagingMemory);
    ctx->dynamicStagingMapped = NULL;
  }
  CLEANUP_BUFFER(dynamicStagingBuffer, dynamicStagingMemory);

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (ctx->meshSSBOMapped[i]) {
      vkUnmapMemory(ctx->device, ctx->meshSSBOMemory[i]);
      ctx->meshSSBOMapped[i] = NULL;
    }
    CLEANUP_BUFFER(meshSSBO[i], meshSSBOMemory[i]);

    if (sdfSSBOMapped[i]) {
      vkUnmapMemory(ctx->device, sdfSSBOMemory[i]);
      sdfSSBOMapped[i] = NULL;
    }
    if (sdfSSBO[i]) {
      vkDestroyBuffer(ctx->device, sdfSSBO[i], NULL);
      sdfSSBO[i] = VK_NULL_HANDLE;
    }
    if (sdfSSBOMemory[i]) {
      vkFreeMemory(ctx->device, sdfSSBOMemory[i], NULL);
      sdfSSBOMemory[i] = VK_NULL_HANDLE;
    }
  }
  if (ctx->meshDirtyBits) {
    free(ctx->meshDirtyBits);
    ctx->meshDirtyBits = NULL;
  }
  if (ctx->ssboSetLayout) {
    vkDestroyDescriptorSetLayout(ctx->device, ctx->ssboSetLayout, NULL);
    ctx->ssboSetLayout = VK_NULL_HANDLE;
  }
  if (ctx->ssboPool) {
    vkDestroyDescriptorPool(ctx->device, ctx->ssboPool, NULL);
    ctx->ssboPool = VK_NULL_HANDLE;
  }

  if (ctx->srcIndirectBufferMapped) {
    vkUnmapMemory(ctx->device, ctx->indirectBufferMemory);
    ctx->srcIndirectBufferMapped = NULL;
  }
  if (ctx->indirectBuffer) {
    vkDestroyBuffer(ctx->device, ctx->indirectBuffer, NULL);
    ctx->indirectBuffer = VK_NULL_HANDLE;
  }
  if (ctx->indirectBufferMemory) {
    vkFreeMemory(ctx->device, ctx->indirectBufferMemory, NULL);
    ctx->indirectBufferMemory = VK_NULL_HANDLE;
  }

#undef CLEANUP_BUFFER

  if (ctx->renderGraph) {
    rg_destroy(ctx->renderGraph);
    ctx->renderGraph = NULL;
  }

  if (ctx->device) {
    obs_vk_memory_tracker_destroy(ctx->device);
    vkDestroyDevice(ctx->device, NULL);
    ctx->device = VK_NULL_HANDLE;
  }
  if (ctx->instance) {
    vkDestroyInstance(ctx->instance, NULL);
    ctx->instance = VK_NULL_HANDLE;
  }
  glfwTerminate();
}

/// Misc

/// Mega vertex buffer + dynamic buffer helpers

void copyBuffer(VkDevice device, VkCommandPool pool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size,
                VkDeviceSize srcOffset, VkDeviceSize dstOffset) {
  VkCommandBuffer cmd = beginSingleTimeCommands(device, pool);
  VkBufferCopy region = {
      .srcOffset = srcOffset, .dstOffset = dstOffset, .size = size};
  vkCmdCopyBuffer(cmd, src, dst, 1, &region);
  endSingleTimeCommands(device, pool, queue, cmd);
}

static VkDeviceSize geometry_growth_capacity(VkDeviceSize current,
                                             VkDeviceSize required) {
  VkDeviceSize capacity = current < 4096 ? 4096 : current;
  while (capacity < required) {
    if (capacity > UINT64_MAX / 2)
      return 0;
    capacity *= 2;
  }
  return capacity;
}

static bool grow_device_buffer(VulkanContext *ctx, VkBuffer *buffer,
                               VkDeviceMemory *memory, VkDeviceSize *capacity,
                               VkDeviceSize required, VkDeviceSize preserve,
                               VkBufferUsageFlags usage, uint64_t *address,
                               bool device_idle) {
  if (required <= *capacity)
    return true;
  VkDeviceSize next = geometry_growth_capacity(*capacity, required);
  if (!next)
    return false;
  VkBuffer replacement = VK_NULL_HANDLE;
  VkDeviceMemory replacement_memory = VK_NULL_HANDLE;
  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = next,
      .usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (vkCreateBuffer(ctx->device, &buffer_info, NULL, &replacement) !=
      VK_SUCCESS)
    return false;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(ctx->device, replacement, &requirements);
  const VkMemoryAllocateFlagsInfo flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
  };
  const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &flags,
      .allocationSize = requirements.size,
      .memoryTypeIndex =
          findMemoryType(ctx->physicalDevice, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
  };
  if (vkAllocateMemory(ctx->device, &allocation, NULL, &replacement_memory) !=
          VK_SUCCESS ||
      vkBindBufferMemory(ctx->device, replacement, replacement_memory, 0) !=
          VK_SUCCESS) {
    if (replacement_memory)
      vkFreeMemory(ctx->device, replacement_memory, NULL);
    vkDestroyBuffer(ctx->device, replacement, NULL);
    return false;
  }
  if (*buffer != VK_NULL_HANDLE) {
    if (!device_idle && vkDeviceWaitIdle(ctx->device) != VK_SUCCESS) {
      vkFreeMemory(ctx->device, replacement_memory, NULL);
      vkDestroyBuffer(ctx->device, replacement, NULL);
      return false;
    }
    if (preserve > *capacity)
      preserve = *capacity;
    if (preserve)
      copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, *buffer,
                 replacement, preserve, 0, 0);
    vkDestroyBuffer(ctx->device, *buffer, NULL);
    vkFreeMemory(ctx->device, *memory, NULL);
  }
  *buffer = replacement;
  *memory = replacement_memory;
  *capacity = next;
  if (address) {
    const VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = replacement,
    };
    *address = vkGetBufferDeviceAddress(ctx->device, &address_info);
  }
  return true;
}

void createMegaVertexBuffer(VulkanContext *ctx, VkDeviceSize size) {
  ctx->megaVertexBufferSize = size;
  ctx->megaVertexBufferOffset = 0;
  createBuffer(ctx, size,
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->megaVertexBuffer,
               &ctx->megaVertexBufferMemory);

  VkBufferDeviceAddressInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = ctx->megaVertexBuffer};
  megaVertexBufferAddr = vkGetBufferDeviceAddress(ctx->device, &info);

  fprintf(stdout, "Mega vertex buffer: %.1f MB (DEVICE_LOCAL, Address: %llx)\n",
          (double)size / (1024.0 * 1024.0),
          (unsigned long long)megaVertexBufferAddr);

  // ── CREATE MEGA MORPH BUFFER ──
  VkDeviceSize morphSize = 4096;
  ctx->megaMorphBufferSize = morphSize;
  ctx->megaMorphBufferOffset = 0;
  createBuffer(ctx, morphSize,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->megaMorphBuffer,
               &ctx->megaMorphBufferMemory);
  VkBufferDeviceAddressInfo morphInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = ctx->megaMorphBuffer};
  megaMorphBufferAddr = vkGetBufferDeviceAddress(ctx->device, &morphInfo);

  // ── CREATE MORPH WEIGHT BUFFER ──
  VkDeviceSize weightSize = 1024 * 1024; // 1 MB for dynamic weights per frame
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    createBuffer(ctx, weightSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &ctx->morphWeightBuffer[i], &ctx->morphWeightMemory[i]);
    vkMapMemory(ctx->device, ctx->morphWeightMemory[i], 0, weightSize, 0,
                &ctx->morphWeightMapped[i]);
    VkBufferDeviceAddressInfo weightInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = ctx->morphWeightBuffer[i]};
    morphWeightAddr[i] = vkGetBufferDeviceAddress(ctx->device, &weightInfo);
  }

  // ── CREATE MEGA MESHLET BUFFERS ──
  VkDeviceSize mlSize = 4096;
  VkDeviceSize mlBoundsSize = 4096;
  VkDeviceSize mlVertSize = 4096;
  VkDeviceSize mlTriSize = 4096;

  megaMeshletBufferSize = mlSize;
  megaMeshletBoundsBufferSize = mlBoundsSize;
  megaMeshletBufferOffset = 0;
  createBuffer(ctx, mlSize,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &megaMeshletBuffer,
               &megaMeshletBufferMemory);
  VkBufferDeviceAddressInfo mlInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = megaMeshletBuffer};
  megaMeshletBufferAddr = vkGetBufferDeviceAddress(ctx->device, &mlInfo);

  createBuffer(ctx, mlBoundsSize,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &megaMeshletBoundsBuffer,
               &megaMeshletBoundsBufferMemory);
  VkBufferDeviceAddressInfo mlBoundsInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = megaMeshletBoundsBuffer};
  megaMeshletBoundsBufferAddr =
      vkGetBufferDeviceAddress(ctx->device, &mlBoundsInfo);

  VkDeviceSize mlSkinSize = 4096;
  megaMeshletSkinBufferSize = mlSkinSize;
  createBuffer(ctx, mlSkinSize,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &megaMeshletSkinBuffer,
               &megaMeshletSkinBufferMemory);
  VkBufferDeviceAddressInfo skinInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = megaMeshletSkinBuffer};
  megaMeshletSkinBufferAddr = vkGetBufferDeviceAddress(ctx->device, &skinInfo);

  VkDeviceSize dynamicBoundsSize = 4096;
  dynamicBoundsBufferSize = dynamicBoundsSize;
  for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    createBuffer(ctx, dynamicBoundsSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dynamicBoundsBuffer[i],
                 &dynamicBoundsBufferMemory[i]);
    VkBufferDeviceAddressInfo dynInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = dynamicBoundsBuffer[i]};
    dynamicBoundsBufferAddr[i] =
        vkGetBufferDeviceAddress(ctx->device, &dynInfo);
  }

  megaMeshletVertexBufferSize = mlVertSize;
  megaMeshletVertexOffset = 0;
  createBuffer(ctx, mlVertSize,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &megaMeshletVertexBuffer,
               &megaMeshletVertexMemory);
  VkBufferDeviceAddressInfo mlVertInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = megaMeshletVertexBuffer};
  megaMeshletVertexAddr = vkGetBufferDeviceAddress(ctx->device, &mlVertInfo);

  megaMeshletTriangleBufferSize = mlTriSize;
  megaMeshletTriangleOffset = 0;
  createBuffer(ctx, mlTriSize,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &megaMeshletTriangleBuffer,
               &megaMeshletTriangleMemory);
  VkBufferDeviceAddressInfo mlTriInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = megaMeshletTriangleBuffer};
  megaMeshletTriangleAddr = vkGetBufferDeviceAddress(ctx->device, &mlTriInfo);

  fprintf(stdout, "Meshlet Buffers allocated: %llx, %llx, %llx, %llx\n",
          (unsigned long long)megaMeshletBufferAddr,
          (unsigned long long)megaMeshletBoundsBufferAddr,
          (unsigned long long)megaMeshletVertexAddr,
          (unsigned long long)megaMeshletTriangleAddr);
}

bool megaVertexBufferReserve(VulkanContext *ctx, uint64_t required_vertices) {
  if (!ctx || !geometry_legacy_allowed() ||
      required_vertices > UINT64_MAX / sizeof(Vertex))
    return false;
  VkDeviceSize required = (VkDeviceSize)required_vertices * sizeof(Vertex);
  if (required <= ctx->megaVertexBufferSize)
    return true;
  if (ctx->megaVertexBuffer != VK_NULL_HANDLE) {
    flushUploadStagingBuffer(ctx);
    if (vkDeviceWaitIdle(ctx->device) != VK_SUCCESS)
      return false;
  }
  VkDeviceSize preserve =
      (VkDeviceSize)ctx->megaVertexBufferOffset * sizeof(Vertex);
  return grow_device_buffer(
      ctx, &ctx->megaVertexBuffer, &ctx->megaVertexBufferMemory,
      &ctx->megaVertexBufferSize, required, preserve,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &megaVertexBufferAddr, true);
}

bool megaIndexBufferReserve(VulkanContext *ctx, uint64_t required_indices) {
  if (!ctx || !geometry_legacy_allowed() ||
      required_indices > UINT64_MAX / sizeof(uint32_t))
    return false;
  VkDeviceSize required = (VkDeviceSize)required_indices * sizeof(uint32_t);
  if (required <= ctx->megaIndexBufferSize)
    return true;
  flushUploadStagingBuffer(ctx);
  if (vkDeviceWaitIdle(ctx->device) != VK_SUCCESS)
    return false;
  VkDeviceSize preserve =
      (VkDeviceSize)ctx->megaIndexBufferOffset * sizeof(uint32_t);
  return grow_device_buffer(ctx, &ctx->megaIndexBuffer,
                            &ctx->megaIndexBufferMemory,
                            &ctx->megaIndexBufferSize, required, preserve,
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, NULL, true);
}

/* Upload vertices into the mega buffer, return the BASE VERTEX INDEX for
   DrawIndexed/Draw offset. Uses a temporary staging buffer so the final data
   lives in DEVICE_LOCAL memory. */
void createUploadStagingBuffer(VulkanContext *ctx, VkDeviceSize size) {
  ctx->uploadStagingSize = size;
  ctx->uploadStagingOffset = 0;
  createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               &ctx->uploadStagingBuffer, &ctx->uploadStagingMemory);
  vkMapMemory(ctx->device, ctx->uploadStagingMemory, 0, size, 0,
              &ctx->uploadStagingMapped);

  /* pre-allocate region arrays */
  ctx->pendingVertexCopyCapacity = 8;
  ctx->pendingVertexCopyCount = 0;
  ctx->pendingVertexCopies = malloc(8 * sizeof(VkBufferCopy));
  ctx->pendingIndexCopyCapacity = 8;
  ctx->pendingIndexCopyCount = 0;
  ctx->pendingIndexCopies = malloc(8 * sizeof(VkBufferCopy));
  ctx->pendingMeshletCopyCapacity = 8;
  ctx->pendingMeshletCopyCount = 0;
  ctx->pendingMeshletCopies = malloc(8 * sizeof(VkBufferCopy));
  ctx->pendingMeshletBoundsCopyCapacity = 8;
  ctx->pendingMeshletBoundsCopyCount = 0;
  ctx->pendingMeshletBoundsCopies = malloc(8 * sizeof(VkBufferCopy));
  ctx->pendingMeshletSkinCopyCapacity = 8;
  ctx->pendingMeshletSkinCopyCount = 0;
  ctx->pendingMeshletSkinCopies = malloc(8 * sizeof(VkBufferCopy));
  ctx->pendingMeshletVertexCopyCapacity = 8;
  ctx->pendingMeshletVertexCopyCount = 0;
  ctx->pendingMeshletVertexCopies = malloc(8 * sizeof(VkBufferCopy));
  ctx->pendingMeshletTriangleCopyCapacity = 8;
  ctx->pendingMeshletTriangleCopyCount = 0;
  ctx->pendingMeshletTriangleCopies = malloc(8 * sizeof(VkBufferCopy));

  fprintf(stdout, "Upload staging buffer: %.1f MB\n",
          (double)size / (1024.0 * 1024.0));
}

void destroyUploadStagingBuffer(VulkanContext *ctx) {
  if (ctx->uploadStagingMapped) {
    vkUnmapMemory(ctx->device, ctx->uploadStagingMemory);
    ctx->uploadStagingMapped = NULL;
  }
  if (ctx->uploadStagingBuffer) {
    vkDestroyBuffer(ctx->device, ctx->uploadStagingBuffer, NULL);
    ctx->uploadStagingBuffer = VK_NULL_HANDLE;
  }
  if (ctx->uploadStagingMemory) {
    vkFreeMemory(ctx->device, ctx->uploadStagingMemory, NULL);
    ctx->uploadStagingMemory = VK_NULL_HANDLE;
  }
  if (ctx->pendingVertexCopies) {
    free(ctx->pendingVertexCopies);
    ctx->pendingVertexCopies = NULL;
  }
  if (ctx->pendingIndexCopies) {
    free(ctx->pendingIndexCopies);
    ctx->pendingIndexCopies = NULL;
  }
  if (ctx->pendingMeshletCopies) {
    free(ctx->pendingMeshletCopies);
    ctx->pendingMeshletCopies = NULL;
  }
  if (ctx->pendingMeshletBoundsCopies) {
    free(ctx->pendingMeshletBoundsCopies);
    ctx->pendingMeshletBoundsCopies = NULL;
  }
  if (ctx->pendingMeshletSkinCopies) {
    free(ctx->pendingMeshletSkinCopies);
    ctx->pendingMeshletSkinCopies = NULL;
  }
  if (ctx->pendingMeshletVertexCopies) {
    free(ctx->pendingMeshletVertexCopies);
    ctx->pendingMeshletVertexCopies = NULL;
  }
  if (ctx->pendingMeshletTriangleCopies) {
    free(ctx->pendingMeshletTriangleCopies);
    ctx->pendingMeshletTriangleCopies = NULL;
  }
  ctx->uploadStagingSize = 0;
  ctx->uploadStagingOffset = 0;
  ctx->pendingVertexCopyCount = 0;
  ctx->pendingVertexCopyCapacity = 0;
  ctx->pendingIndexCopyCount = 0;
  ctx->pendingIndexCopyCapacity = 0;
}

void flushUploadStagingBuffer(VulkanContext *ctx) {
  if (obs_geometry_async_busy(&s_geometry_async))
    return;

  bool any = ctx->pendingVertexCopyCount > 0 ||
             ctx->pendingIndexCopyCount > 0 ||
             ctx->pendingMeshletCopyCount > 0 ||
             ctx->pendingMeshletBoundsCopyCount > 0 ||
             ctx->pendingMeshletSkinCopyCount > 0 ||
             ctx->pendingMeshletVertexCopyCount > 0 ||
             ctx->pendingMeshletTriangleCopyCount > 0;
  if (!any)
    return;

  VkCommandBuffer cmd = beginSingleTimeCommands(ctx->device, ctx->commandPool);

  if (ctx->pendingVertexCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, ctx->megaVertexBuffer,
                    ctx->pendingVertexCopyCount, ctx->pendingVertexCopies);
  if (ctx->pendingIndexCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, ctx->megaIndexBuffer,
                    ctx->pendingIndexCopyCount, ctx->pendingIndexCopies);
  if (ctx->pendingMeshletCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletBuffer,
                    ctx->pendingMeshletCopyCount, ctx->pendingMeshletCopies);
  if (ctx->pendingMeshletBoundsCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletBoundsBuffer,
                    ctx->pendingMeshletBoundsCopyCount,
                    ctx->pendingMeshletBoundsCopies);
  if (ctx->pendingMeshletSkinCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletSkinBuffer,
                    ctx->pendingMeshletSkinCopyCount,
                    ctx->pendingMeshletSkinCopies);
  if (ctx->pendingMeshletVertexCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletVertexBuffer,
                    ctx->pendingMeshletVertexCopyCount,
                    ctx->pendingMeshletVertexCopies);
  if (ctx->pendingMeshletTriangleCopyCount > 0)
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletTriangleBuffer,
                    ctx->pendingMeshletTriangleCopyCount,
                    ctx->pendingMeshletTriangleCopies);

  endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue, cmd);

  uint32_t vc = ctx->pendingVertexCopyCount;
  uint32_t ic = ctx->pendingIndexCopyCount;
  ctx->pendingVertexCopyCount = 0;
  ctx->pendingIndexCopyCount = 0;
  ctx->pendingMeshletCopyCount = 0;
  ctx->pendingMeshletBoundsCopyCount = 0;
  ctx->pendingMeshletSkinCopyCount = 0;
  ctx->pendingMeshletVertexCopyCount = 0;
  ctx->pendingMeshletTriangleCopyCount = 0;
  ctx->uploadStagingOffset = 0;
  fprintf(
      stdout,
      "Flushed upload staging buffer: %u vertex + %u index + meshlet regions\n",
      vc, ic);
}

/// All-stream geometry capacity preflight
/* Keep the three meshlet streams on the same logical offset.  Checking only
 * the primary meshlet buffer is insufficient when bounds/skin types grow. */
static bool geometry_budget_for(const VulkanContext *ctx,
                                const MegaGeometryCounts *counts,
                                ObsGeometryBudget *budget) {
  if (!ctx || !counts || !budget)
    return false;
  ObsGeometryBudget b = {0};
#define BUDGET(K, U, N, T, CAP)                                                \
  b.pools[K] = (ObsGeometryPoolBudget){.used = (U),                            \
                                       .requested = (N),                       \
                                       .element_bytes = sizeof(T),             \
                                       .capacity_bytes = (CAP)}
  BUDGET(OBS_GEOM_VERTEX, ctx->megaVertexBufferOffset, counts->vertices, Vertex,
         ctx->megaVertexBufferSize);
  BUDGET(OBS_GEOM_INDEX, ctx->megaIndexBufferOffset, counts->indices, uint32_t,
         ctx->megaIndexBufferSize);
  BUDGET(OBS_GEOM_MORPH, ctx->megaMorphBufferOffset, counts->morphs, MorphDelta,
         ctx->megaMorphBufferSize);
  BUDGET(OBS_GEOM_MESHLET, megaMeshletBufferOffset, counts->meshlets, Meshlet,
         megaMeshletBufferSize);
  BUDGET(OBS_GEOM_BOUNDS, megaMeshletBufferOffset, counts->meshlets,
         MeshletBounds, megaMeshletBoundsBufferSize);
  BUDGET(OBS_GEOM_SKIN, megaMeshletBufferOffset, counts->meshlets,
         MeshletSkinData, megaMeshletSkinBufferSize);
  BUDGET(OBS_GEOM_MESHLET_VERTEX, megaMeshletVertexOffset,
         counts->meshlet_vertices, uint32_t, megaMeshletVertexBufferSize);
  BUDGET(OBS_GEOM_MESHLET_TRIANGLE, megaMeshletTriangleOffset,
         counts->meshlet_triangles, uint8_t, megaMeshletTriangleBufferSize);
#undef BUDGET
  *budget = b;
  return true;
}

static bool geometry_required_bytes(uint32_t used, uint32_t requested,
                                    size_t element_size, VkDeviceSize *out) {
  uint64_t total = (uint64_t)used + requested;
  if (!out || total > UINT32_MAX || total > UINT64_MAX / element_size)
    return false;
  *out = (VkDeviceSize)(total * element_size);
  return true;
}

static bool resize_upload_staging(VulkanContext *ctx, VkDeviceSize required) {
  if (required <= ctx->uploadStagingSize)
    return true;
  VkDeviceSize capacity =
      geometry_growth_capacity(ctx->uploadStagingSize, required);
  if (!capacity)
    return false;
  if (ctx->uploadStagingOffset)
    flushUploadStagingBuffer(ctx);
  if (ctx->uploadStagingOffset)
    return false;

  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = NULL;
  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = capacity,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (vkCreateBuffer(ctx->device, &buffer_info, NULL, &buffer) != VK_SUCCESS)
    return false;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(ctx->device, buffer, &requirements);
  const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex =
          findMemoryType(ctx->physicalDevice, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
  };
  if (vkAllocateMemory(ctx->device, &allocation, NULL, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(ctx->device, buffer, memory, 0) != VK_SUCCESS ||
      vkMapMemory(ctx->device, memory, 0, capacity, 0, &mapped) != VK_SUCCESS) {
    if (memory)
      vkFreeMemory(ctx->device, memory, NULL);
    vkDestroyBuffer(ctx->device, buffer, NULL);
    return false;
  }
  if (ctx->uploadStagingMapped)
    vkUnmapMemory(ctx->device, ctx->uploadStagingMemory);
  if (ctx->uploadStagingBuffer)
    vkDestroyBuffer(ctx->device, ctx->uploadStagingBuffer, NULL);
  if (ctx->uploadStagingMemory)
    vkFreeMemory(ctx->device, ctx->uploadStagingMemory, NULL);
  ctx->uploadStagingBuffer = buffer;
  ctx->uploadStagingMemory = memory;
  ctx->uploadStagingMapped = mapped;
  ctx->uploadStagingSize = capacity;
  ctx->uploadStagingOffset = 0;
  return true;
}

static bool ensure_geometry_capacity(VulkanContext *ctx,
                                     const MegaGeometryCounts *counts) {
  if (!ctx || !counts || !geometry_legacy_allowed())
    return false;
  VkDeviceSize required[OBS_GEOM_POOL_COUNT];
  if (!geometry_required_bytes(ctx->megaVertexBufferOffset, counts->vertices,
                               sizeof(Vertex), &required[OBS_GEOM_VERTEX]) ||
      !geometry_required_bytes(ctx->megaIndexBufferOffset, counts->indices,
                               sizeof(uint32_t), &required[OBS_GEOM_INDEX]) ||
      !geometry_required_bytes(ctx->megaMorphBufferOffset, counts->morphs,
                               sizeof(MorphDelta), &required[OBS_GEOM_MORPH]) ||
      !geometry_required_bytes(megaMeshletBufferOffset, counts->meshlets,
                               sizeof(Meshlet), &required[OBS_GEOM_MESHLET]) ||
      !geometry_required_bytes(megaMeshletBufferOffset, counts->meshlets,
                               sizeof(MeshletBounds),
                               &required[OBS_GEOM_BOUNDS]) ||
      !geometry_required_bytes(megaMeshletBufferOffset, counts->meshlets,
                               sizeof(MeshletSkinData),
                               &required[OBS_GEOM_SKIN]) ||
      !geometry_required_bytes(megaMeshletVertexOffset,
                               counts->meshlet_vertices, sizeof(uint32_t),
                               &required[OBS_GEOM_MESHLET_VERTEX]) ||
      !geometry_required_bytes(megaMeshletTriangleOffset,
                               counts->meshlet_triangles, sizeof(uint8_t),
                               &required[OBS_GEOM_MESHLET_TRIANGLE]))
    return false;

  uint64_t meshlet_bounds_total = 0;
  for (unsigned i = OBS_GEOM_MESHLET; i <= OBS_GEOM_SKIN; i++) {
    const uint64_t bytes =
        (uint64_t)counts->meshlets *
        (uint64_t)(i == OBS_GEOM_MESHLET  ? sizeof(Meshlet)
                   : i == OBS_GEOM_BOUNDS ? sizeof(MeshletBounds)
                                          : sizeof(MeshletSkinData));
    if (bytes > UINT64_MAX - meshlet_bounds_total)
      return false;
    meshlet_bounds_total += bytes;
  }
  uint64_t staging_required = meshlet_bounds_total;
  const uint64_t staged_pools[] = {
      (uint64_t)counts->vertices * sizeof(Vertex),
      (uint64_t)counts->indices * sizeof(uint32_t),
      (uint64_t)counts->meshlet_vertices * sizeof(uint32_t),
      (uint64_t)counts->meshlet_triangles * sizeof(uint8_t),
  };
  for (size_t i = 0; i < sizeof(staged_pools) / sizeof(staged_pools[0]); i++)
    if (staged_pools[i] > staging_required)
      staging_required = staged_pools[i];

  bool gpu_growth =
      required[OBS_GEOM_VERTEX] > ctx->megaVertexBufferSize ||
      required[OBS_GEOM_INDEX] > ctx->megaIndexBufferSize ||
      required[OBS_GEOM_MORPH] > ctx->megaMorphBufferSize ||
      required[OBS_GEOM_MESHLET] > megaMeshletBufferSize ||
      required[OBS_GEOM_BOUNDS] > megaMeshletBoundsBufferSize ||
      required[OBS_GEOM_SKIN] > megaMeshletSkinBufferSize ||
      required[OBS_GEOM_MESHLET_VERTEX] > megaMeshletVertexBufferSize ||
      required[OBS_GEOM_MESHLET_TRIANGLE] > megaMeshletTriangleBufferSize ||
      required[OBS_GEOM_BOUNDS] > dynamicBoundsBufferSize;
  bool staging_growth = ctx->uploadStagingBuffer != VK_NULL_HANDLE &&
                        staging_required > ctx->uploadStagingSize;
  if (!gpu_growth && !staging_growth)
    return true;

  flushUploadStagingBuffer(ctx);
  if (gpu_growth && vkDeviceWaitIdle(ctx->device) != VK_SUCCESS)
    return false;

  if (!grow_device_buffer(
          ctx, &ctx->megaVertexBuffer, &ctx->megaVertexBufferMemory,
          &ctx->megaVertexBufferSize, required[OBS_GEOM_VERTEX],
          (VkDeviceSize)ctx->megaVertexBufferOffset * sizeof(Vertex),
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &megaVertexBufferAddr, true) ||
      !grow_device_buffer(
          ctx, &ctx->megaIndexBuffer, &ctx->megaIndexBufferMemory,
          &ctx->megaIndexBufferSize, required[OBS_GEOM_INDEX],
          (VkDeviceSize)ctx->megaIndexBufferOffset * sizeof(uint32_t),
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, NULL, true) ||
      !grow_device_buffer(
          ctx, &ctx->megaMorphBuffer, &ctx->megaMorphBufferMemory,
          &ctx->megaMorphBufferSize, required[OBS_GEOM_MORPH],
          (VkDeviceSize)ctx->megaMorphBufferOffset * sizeof(MorphDelta),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &megaMorphBufferAddr, true) ||
      !grow_device_buffer(
          ctx, &megaMeshletBuffer, &megaMeshletBufferMemory,
          &megaMeshletBufferSize, required[OBS_GEOM_MESHLET],
          (VkDeviceSize)megaMeshletBufferOffset * sizeof(Meshlet),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &megaMeshletBufferAddr, true) ||
      !grow_device_buffer(
          ctx, &megaMeshletBoundsBuffer, &megaMeshletBoundsBufferMemory,
          &megaMeshletBoundsBufferSize, required[OBS_GEOM_BOUNDS],
          (VkDeviceSize)megaMeshletBufferOffset * sizeof(MeshletBounds),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &megaMeshletBoundsBufferAddr,
          true) ||
      !grow_device_buffer(
          ctx, &megaMeshletSkinBuffer, &megaMeshletSkinBufferMemory,
          &megaMeshletSkinBufferSize, required[OBS_GEOM_SKIN],
          (VkDeviceSize)megaMeshletBufferOffset * sizeof(MeshletSkinData),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &megaMeshletSkinBufferAddr,
          true) ||
      !grow_device_buffer(
          ctx, &megaMeshletVertexBuffer, &megaMeshletVertexMemory,
          &megaMeshletVertexBufferSize, required[OBS_GEOM_MESHLET_VERTEX],
          (VkDeviceSize)megaMeshletVertexOffset * sizeof(uint32_t),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &megaMeshletVertexAddr, true) ||
      !grow_device_buffer(
          ctx, &megaMeshletTriangleBuffer, &megaMeshletTriangleMemory,
          &megaMeshletTriangleBufferSize, required[OBS_GEOM_MESHLET_TRIANGLE],
          megaMeshletTriangleOffset, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          &megaMeshletTriangleAddr, true))
    return false;

  if (required[OBS_GEOM_BOUNDS] > dynamicBoundsBufferSize) {
    VkDeviceSize expanded_capacity = dynamicBoundsBufferSize;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
      VkDeviceSize frame_capacity = dynamicBoundsBufferSize;
      if (!grow_device_buffer(ctx, &dynamicBoundsBuffer[i],
                              &dynamicBoundsBufferMemory[i], &frame_capacity,
                              required[OBS_GEOM_BOUNDS], 0,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              &dynamicBoundsBufferAddr[i], true))
        return false;
      if (frame_capacity > expanded_capacity)
        expanded_capacity = frame_capacity;
    }
    dynamicBoundsBufferSize = expanded_capacity;
  }
  return !staging_growth ||
         resize_upload_staging(ctx, (VkDeviceSize)staging_required);
}

/// Nonblocking Vulkan geometry backend — exclusive one-flight adapter
/* No vkWaitForFences or vkQueueWaitIdle on the normal polling path. A copy is
 * visible only after the caller explicitly commits a signaled fence. The
 * existing legacy appenders are blocked for the whole pending interval. */
static void geometry_async_vk_release(ObsVkAsyncGeometry *v) {
  if (!v || !v->active || !v->ctx)
    return;
  VkDevice dev = v->ctx->device;
  if (v->fence)
    vkDestroyFence(dev, v->fence, NULL);
  if (v->command)
    vkFreeCommandBuffers(dev, v->ctx->commandPool, 1, &v->command);
  if (v->staging)
    vkDestroyBuffer(dev, v->staging, NULL);
  if (v->memory)
    vkFreeMemory(dev, v->memory, NULL);
  memset(v, 0, sizeof(*v));
}

static ObsAsyncSubmitResult
geometry_async_vk_start(void *user, uint64_t ticket,
                        const ObsGeometryUploadBatch *batch) {
  ObsVkAsyncGeometry *v = user;
  if (!v || !v->ctx || v->active || !batch ||
      batch->state != OBS_UPLOAD_PREPARED)
    return OBS_ASYNC_REJECTED;
  VulkanContext *ctx = v->ctx;
  const size_t size = batch->plan.staging_bytes;
  v->active = true;
  v->ticket = ticket;
  if (!size)
    return OBS_ASYNC_ACCEPTED; /* Empty but valid scene. */

  VkBufferCreateInfo buffer = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (vkCreateBuffer(ctx->device, &buffer, NULL, &v->staging) != VK_SUCCESS)
    goto rejected;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(ctx->device, v->staging, &requirements);
  VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex =
          findMemoryType(ctx->physicalDevice, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
  };
  if (vkAllocateMemory(ctx->device, &alloc, NULL, &v->memory) != VK_SUCCESS ||
      vkBindBufferMemory(ctx->device, v->staging, v->memory, 0) != VK_SUCCESS)
    goto rejected;
  void *mapped = NULL;
  if (vkMapMemory(ctx->device, v->memory, 0, buffer.size, 0, &mapped) !=
      VK_SUCCESS)
    goto rejected;
  memcpy(mapped, batch->bytes, size);
  vkUnmapMemory(ctx->device, v->memory);

  VkCommandBufferAllocateInfo cmd_alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->commandPool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  if (vkAllocateCommandBuffers(ctx->device, &cmd_alloc, &v->command) !=
      VK_SUCCESS)
    goto rejected;
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkBeginCommandBuffer(v->command, &begin) != VK_SUCCESS)
    goto rejected;
  const VkBuffer targets[OBS_GEOM_POOL_COUNT] = {
      ctx->megaVertexBuffer,   ctx->megaIndexBuffer,      ctx->megaMorphBuffer,
      megaMeshletBuffer,       megaMeshletBoundsBuffer,   megaMeshletSkinBuffer,
      megaMeshletVertexBuffer, megaMeshletTriangleBuffer,
  };
  for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
    const ObsGeometryCopy *copy = &batch->plan.copies[i];
    if (!copy->copy_bytes)
      continue;
    if (targets[i] == VK_NULL_HANDLE)
      goto rejected;
    const VkBufferCopy region = {
        .srcOffset = (VkDeviceSize)copy->source_offset,
        .dstOffset = (VkDeviceSize)copy->destination_offset,
        .size = (VkDeviceSize)copy->copy_bytes,
    };
    vkCmdCopyBuffer(v->command, v->staging, targets[i], 1, &region);
  }
  const VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
  };
  vkCmdPipelineBarrier(v->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                       NULL, 0, NULL);
  if (vkEndCommandBuffer(v->command) != VK_SUCCESS)
    goto rejected;
  const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(ctx->device, &fence_info, NULL, &v->fence) != VK_SUCCESS)
    goto rejected;
  const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &v->command,
  };
  /* An error from the driver is NOT a quiescence proof. Retain everything
   * until a device/queue idle or fence success is independently observed. */
  if (vkQueueSubmit(ctx->graphicsQueue, 1, &submit, v->fence) != VK_SUCCESS)
    return OBS_ASYNC_UNKNOWN;
  return OBS_ASYNC_ACCEPTED;
rejected:
  geometry_async_vk_release(v); /* Nothing was submitted. */
  return OBS_ASYNC_REJECTED;
}

static ObsAsyncPollResult geometry_async_vk_poll(void *user, uint64_t ticket) {
  const ObsVkAsyncGeometry *v = user;
  if (!v || !v->active || v->ticket != ticket || !v->ctx)
    return OBS_ASYNC_UNCERTAIN;
  if (!v->fence)
    return OBS_ASYNC_SIGNALED;
  VkResult result = vkGetFenceStatus(v->ctx->device, v->fence);
  return result == VK_SUCCESS     ? OBS_ASYNC_SIGNALED
         : result == VK_NOT_READY ? OBS_ASYNC_PENDING
                                  : OBS_ASYNC_UNCERTAIN;
}

static bool geometry_async_vk_retire(void *user, uint64_t ticket,
                                     bool quiesced) {
  ObsVkAsyncGeometry *v = user;
  if (!v || !v->active || v->ticket != ticket || !quiesced)
    return false;
  geometry_async_vk_release(v);
  return true;
}

/// Explicit Vulkan-facing async geometry API (does NOT publish a Scene)
bool megaGeometryAsyncBeginCooked(VulkanContext *ctx, uint64_t scene_id,
                                  const MegaGeometryCounts *counts,
                                  const MegaCookedGeometry *sources,
                                  uint64_t *ticket) {
  if (!ctx || !counts || !sources || !ticket || !scene_id ||
      !geometry_legacy_allowed())
    return false;
  /* Drain deferred legacy geometry commands BEFORE claiming exclusive
   * ownership; they share the same destination pools and queue. */
  flushUploadStagingBuffer(ctx);
  if (!ensure_geometry_capacity(ctx, counts))
    return false;
  ObsGeometryBudget budget;
  if (!geometry_budget_for(ctx, counts, &budget))
    return false;
  if (!s_geometry_async.initialized) {
    s_async_vk.ctx = ctx;
    const ObsGeometryAsyncBackend backend = {
        .start = geometry_async_vk_start,
        .poll = geometry_async_vk_poll,
        .retire = geometry_async_vk_retire,
        .user = &s_async_vk,
    };
    if (!obs_geometry_async_init(&s_geometry_async, &budget, &backend))
      return false;
  } else if (!obs_geometry_async_sync_idle(&s_geometry_async, &budget)) {
    return false;
  }
  s_async_vk.ctx = ctx;
  const void *data[OBS_GEOM_POOL_COUNT] = {
      sources->vertices,         sources->indices,
      sources->morphs,           sources->meshlets,
      sources->bounds,           sources->skins,
      sources->meshlet_vertices, sources->meshlet_triangles,
  };
  const uint32_t count[OBS_GEOM_POOL_COUNT] = {
      counts->vertices,         counts->indices,           counts->morphs,
      counts->meshlets,         counts->meshlets,          counts->meshlets,
      counts->meshlet_vertices, counts->meshlet_triangles,
  };
  ObsGeometryUploadPlan layout = {0};
  if (!obs_geometry_upload_plan(&budget, &layout))
    return false;
  ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
  for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
    if (layout.copies[i].source_bytes > SIZE_MAX)
      return false;
    spans[i] = (ObsGeometrySourceSpan){
        .data = data[i],
        .bytes = (size_t)layout.copies[i].source_bytes,
    };
  }
  uint64_t issued;
  if (!obs_geometry_async_prepare(&s_geometry_async, scene_id, count, spans,
                                  &issued))
    return false;
  /* Return a ticket even on uncertain submission so the caller can inspect
   * or recover the quarantined flight. The entire renderer stays guarded. */
  memcpy(s_async_requested, count, sizeof(s_async_requested));
  *ticket = issued;
  if (!obs_geometry_async_submit(&s_geometry_async, issued)) {
    if (s_geometry_async.state == OBS_ASYNC_PREPARED)
      (void)obs_geometry_async_cancel_prepared(&s_geometry_async, issued);
    /* UNKNOWN still owns a recoverable ticket; true means the operation
     * was created, not that its fence is already known to have completed. */
    return s_geometry_async.state == OBS_ASYNC_QUARANTINED;
  }
  return true;
}

ObsAsyncState megaGeometryAsyncPoll(uint64_t ticket) {
  return obs_geometry_async_poll(&s_geometry_async, ticket);
}

bool megaGeometryAsyncCommit(VulkanContext *ctx, uint64_t ticket,
                             const MegaGeometryCounts *counts,
                             MegaGeometryBases *bases) {
  if (!ctx || !counts || !bases || !s_async_vk.ctx || s_async_vk.ctx != ctx)
    return false;
  /* These counts must come from the pending scene, not an unrelated caller.
   * The copied actual reservation is validated against the counters. */
  ObsPendingScene snapshot;
  if (!obs_pending_scene_snapshot(&s_geometry_async.uploads.scenes, ticket,
                                  &snapshot))
    return false;
  const uint32_t requested[OBS_GEOM_POOL_COUNT] = {
      counts->vertices,         counts->indices,           counts->morphs,
      counts->meshlets,         counts->meshlets,          counts->meshlets,
      counts->meshlet_vertices, counts->meshlet_triangles,
  };
  for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
    if (requested[i] && snapshot.geometry.end[i] <= snapshot.geometry.base[i])
      return false;
    /* Only the original load owner may commit its reservation; reject
     * an unrelated count structure even if its GPU fence is signaled. */
    if (requested[i] != s_async_requested[i])
      return false;
  }
  ObsGeometryReservation committed;
  if (!obs_geometry_async_commit(&s_geometry_async, ticket, &committed))
    return false;
  const MegaGeometryBases result = {
      .vertices =
          counts->vertices ? committed.base[OBS_GEOM_VERTEX] : UINT32_MAX,
      .indices = counts->indices ? committed.base[OBS_GEOM_INDEX] : UINT32_MAX,
      .morphs = counts->morphs ? committed.base[OBS_GEOM_MORPH] : UINT32_MAX,
      .meshlets =
          counts->meshlets ? committed.base[OBS_GEOM_MESHLET] : UINT32_MAX,
      .meshlet_vertices = counts->meshlet_vertices
                              ? committed.base[OBS_GEOM_MESHLET_VERTEX]
                              : UINT32_MAX,
      .meshlet_triangles = counts->meshlet_triangles
                               ? committed.base[OBS_GEOM_MESHLET_TRIANGLE]
                               : UINT32_MAX,
  };
  ctx->megaVertexBufferOffset = committed.end[OBS_GEOM_VERTEX];
  ctx->megaIndexBufferOffset = committed.end[OBS_GEOM_INDEX];
  ctx->megaMorphBufferOffset = committed.end[OBS_GEOM_MORPH];
  megaMeshletBufferOffset = committed.end[OBS_GEOM_MESHLET];
  megaMeshletVertexOffset = committed.end[OBS_GEOM_MESHLET_VERTEX];
  megaMeshletTriangleOffset = committed.end[OBS_GEOM_MESHLET_TRIANGLE];
  *bases = result;
  return true;
}

bool megaGeometryAsyncDiscardReady(uint64_t ticket) {
  return obs_geometry_async_discard_ready(&s_geometry_async, ticket);
}

/* Emergency path only: waits for a REAL device-idle proof and intentionally
 * abandons the scene, rather than misreporting a failed transfer as ready. */
bool megaGeometryAsyncRecoverDeviceIdle(VulkanContext *ctx, uint64_t ticket) {
  if (!ctx || ctx != s_async_vk.ctx ||
      s_geometry_async.state != OBS_ASYNC_QUARANTINED)
    return false;
  if (vkDeviceWaitIdle(ctx->device) != VK_SUCCESS)
    return false;
  return obs_geometry_async_retire_quiesced(&s_geometry_async, ticket, true);
}

bool megaGeometryPreflight(VulkanContext *ctx,
                           const MegaGeometryCounts *counts) {
  if (!ctx || !counts || !ensure_geometry_capacity(ctx, counts))
    return false;
  ObsGeometryBudget b;
  if (!geometry_legacy_allowed() || !geometry_budget_for(ctx, counts, &b))
    return false;
  ObsGeometryReservation reservation;
  return obs_geometry_reserve_preview(&b, &reservation) &&
         obs_geometry_staging_preflight(&b, ctx->uploadStagingSize,
                                        ctx->uploadStagingBuffer !=
                                                VK_NULL_HANDLE &&
                                            ctx->uploadStagingMapped != NULL) &&
         (ctx->uploadStagingBuffer == VK_NULL_HANDLE ||
          ctx->uploadStagingMapped != NULL);
}

/// Prepared cache transfer and checked fence lifetime
/* Vulkan memory cannot be retired before the GPU signals completion. If a
 * submitted fence cannot be verified, poison further geometry allocations
 * rather than recycle destinations that could still be written by the GPU.
 * This path deliberately waits on the fence; truly nonblocking transfers
 * require explicit frame-graph synchronization and pending-scene ownership. */
static bool mega_geometry_submit_prepared(VulkanContext *ctx,
                                          const MegaGeometryCounts *counts,
                                          ObsGeometryUploadBatch *batch,
                                          MegaGeometryBases *bases) {
  if (!ctx || !counts || !batch || !bases ||
      batch->state != OBS_UPLOAD_PREPARED || !geometry_legacy_allowed())
    return false;
  const ObsGeometryReservation reservation = batch->plan.reservation;
  const ObsGeometryReservation *r = &reservation;
  ObsGeometryFlight flight = {0};
  uint64_t ticket = 0;
  MegaGeometryBases next = {
      .vertices = counts->vertices ? r->base[OBS_GEOM_VERTEX] : UINT32_MAX,
      .indices = counts->indices ? r->base[OBS_GEOM_INDEX] : UINT32_MAX,
      .morphs = counts->morphs ? r->base[OBS_GEOM_MORPH] : UINT32_MAX,
      .meshlets = counts->meshlets ? r->base[OBS_GEOM_MESHLET] : UINT32_MAX,
      .meshlet_vertices = counts->meshlet_vertices
                              ? r->base[OBS_GEOM_MESHLET_VERTEX]
                              : UINT32_MAX,
      .meshlet_triangles = counts->meshlet_triangles
                               ? r->base[OBS_GEOM_MESHLET_TRIANGLE]
                               : UINT32_MAX,
  };
  /* Do not touch Vulkan until every source stream has been read. */
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool submitted = false;
  bool completed = false;
  bool ok = false;
  if (batch->plan.staging_bytes) {
    /* Flush unrelated queued uploads before allocating a dedicated staging
     * buffer, preventing their pending regions from being overwritten. */
    flushUploadStagingBuffer(ctx);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)batch->plan.staging_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(ctx->device, &buffer_info, NULL, &staging) != VK_SUCCESS)
      goto cleanup;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(ctx->device, staging, &requirements);
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex =
            findMemoryType(ctx->physicalDevice, requirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (vkAllocateMemory(ctx->device, &allocation, NULL, &memory) != VK_SUCCESS)
      goto cleanup;
    if (vkBindBufferMemory(ctx->device, staging, memory, 0) != VK_SUCCESS)
      goto cleanup;
    void *mapped = NULL;
    if (vkMapMemory(ctx->device, memory, 0, buffer_info.size, 0, &mapped) !=
        VK_SUCCESS)
      goto cleanup;
    memcpy(mapped, batch->bytes, batch->plan.staging_bytes);
    vkUnmapMemory(ctx->device, memory);

    const VkCommandBufferAllocateInfo cmd_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(ctx->device, &cmd_info, &command) !=
        VK_SUCCESS)
      goto cleanup;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
      goto cleanup;
    const bool profile_upload = ctx->profileTimestampPool != VK_NULL_HANDLE;
    const uint32_t upload_query_base =
        obs_gpu_profile_upload_query_base(MAX_FRAMES_IN_FLIGHT);
    if (profile_upload) {
      vkCmdResetQueryPool(command, ctx->profileTimestampPool, upload_query_base,
                          2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          ctx->profileTimestampPool, upload_query_base);
    }
    const VkBuffer targets[OBS_GEOM_POOL_COUNT] = {
        ctx->megaVertexBuffer,   ctx->megaIndexBuffer,
        ctx->megaMorphBuffer,    megaMeshletBuffer,
        megaMeshletBoundsBuffer, megaMeshletSkinBuffer,
        megaMeshletVertexBuffer, megaMeshletTriangleBuffer,
    };
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
      const ObsGeometryCopy *c = &batch->plan.copies[i];
      if (!c->copy_bytes)
        continue;
      if (targets[i] == VK_NULL_HANDLE)
        goto cleanup;
      const VkBufferCopy region = {
          .srcOffset = (VkDeviceSize)c->source_offset,
          .dstOffset = (VkDeviceSize)c->destination_offset,
          .size = (VkDeviceSize)c->copy_bytes,
      };
      vkCmdCopyBuffer(command, staging, targets[i], 1, &region);
    }
    const VkMemoryBarrier visibility = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &visibility,
                         0, NULL, 0, NULL);
    if (profile_upload)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          ctx->profileTimestampPool, upload_query_base + 1);
    if (vkEndCommandBuffer(command) != VK_SUCCESS)
      goto cleanup;
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(ctx->device, &fence_info, NULL, &fence) != VK_SUCCESS)
      goto cleanup;
    const VkSubmitInfo submission = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    /* All host allocations and GPU commands are ready. Transfer ownership
     * before submitting, so no post-submit allocation can fail. */
    if (!obs_geometry_flight_take(&flight, batch, &ticket))
      goto cleanup;
    if (vkQueueSubmit(ctx->graphicsQueue, 1, &submission, fence) != VK_SUCCESS)
      goto cleanup;
    submitted = true;
    if (!obs_geometry_flight_submitted(&flight, ticket)) {
      s_geometry_upload_poisoned = true;
      goto cleanup;
    }
    completed = vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX) ==
                VK_SUCCESS;
    if (!completed) {
      /* Failed fence waits are not evidence of completion. A queue-idle
       * proof permits reclamation but this load is still rejected. */
      completed = vkQueueWaitIdle(ctx->graphicsQueue) == VK_SUCCESS;
      if (!completed)
        s_geometry_upload_poisoned = true;
      goto cleanup;
    }
    if (profile_upload) {
      uint64_t timestamps[2] = {0, 0};
      if (vkGetQueryPoolResults(ctx->device, ctx->profileTimestampPool,
                                upload_query_base, 2, sizeof(timestamps),
                                timestamps, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        uint64_t elapsed_ns;
        if (obs_gpu_profile_timestamp_delta_ns(
                timestamps[0], timestamps[1], ctx->profileTimestampValidBits,
                ctx->profileTimestampPeriod, &elapsed_ns)) {
          ctx->profileGeometryUploadGpuNs += elapsed_ns;
          ctx->profileGeometryUploadGpuSampled = true;
        }
      }
    }
    if (!obs_geometry_flight_observe(&flight, ticket, OBS_FENCE_SIGNALED)) {
      s_geometry_upload_poisoned = true;
      goto cleanup;
    }
    ObsGeometryReservation confirmed = {0};
    if (!obs_geometry_flight_publish(&flight, ticket, &confirmed)) {
      s_geometry_upload_poisoned = true;
      goto cleanup;
    }
  }
  ctx->megaVertexBufferOffset = r->end[OBS_GEOM_VERTEX];
  ctx->megaIndexBufferOffset = r->end[OBS_GEOM_INDEX];
  ctx->megaMorphBufferOffset = r->end[OBS_GEOM_MORPH];
  megaMeshletBufferOffset = r->end[OBS_GEOM_MESHLET];
  megaMeshletVertexOffset = r->end[OBS_GEOM_MESHLET_VERTEX];
  megaMeshletTriangleOffset = r->end[OBS_GEOM_MESHLET_TRIANGLE];
  *bases = next;
  ok = true;
cleanup:
  if (!submitted || completed) {
    if (fence)
      vkDestroyFence(ctx->device, fence, NULL);
    if (command)
      vkFreeCommandBuffers(ctx->device, ctx->commandPool, 1, &command);
    if (staging)
      vkDestroyBuffer(ctx->device, staging, NULL);
    if (memory)
      vkFreeMemory(ctx->device, memory, NULL);
    /* The move into the flight happened before vkQueueSubmit. On a
     * rejected submission a PREPARED flight can be cancelled; after a
     * queue-idle proof, an IN_FLIGHT flight can be recovered. */
    if (flight.state == OBS_FLIGHT_PREPARED)
      (void)obs_geometry_flight_cancel(&flight, ticket);
    else if (flight.state != OBS_FLIGHT_EMPTY && completed)
      (void)obs_geometry_flight_recover_quiesced(&flight, ticket, true);
    if (batch->state != OBS_UPLOAD_EMPTY)
      (void)obs_geometry_upload_discard(batch);
  } else {
    /* Submission has unknown completion. Preserve the whole flight and
     * the device resources until fence/queue/device-idle quiescence. */
    s_geometry_upload_poisoned = true;
    if (flight.state == OBS_FLIGHT_IN_FLIGHT)
      (void)obs_geometry_flight_observe(&flight, ticket, OBS_FENCE_UNKNOWN);
    s_unconfirmed_upload = (ObsUnconfirmedUpload){
        .active = true,
        .device = ctx->device,
        .pool = ctx->commandPool,
        .command = command,
        .buffer = staging,
        .memory = memory,
        .fence = fence,
        .flight = flight,
        .ticket = ticket,
    };
    fprintf(stderr, "[OMDL] GPU completion unknown; geometry loading disabled "
                    "until device reset\n");
  }
  return ok;
}

/// Unified prepared cache and freshly cooked uploads
bool megaGeometryUploadCacheBatch(VulkanContext *ctx, FILE *file,
                                  const MegaGeometryCounts *counts,
                                  MegaGeometryBases *bases) {
  if (!ctx || !file || !counts || !bases || !geometry_legacy_allowed())
    return false;
  if (!megaGeometryPreflight(ctx, counts))
    return false;
  ObsGeometryBudget budget;
  if (!geometry_budget_for(ctx, counts, &budget))
    return false;
  ObsGeometryUploadBatch batch = {0};
  if (!obs_geometry_upload_prepare_file(file, &budget, &batch))
    return false;
  return mega_geometry_submit_prepared(ctx, counts, &batch, bases);
}

bool megaGeometryUploadCookedBatch(VulkanContext *ctx,
                                   const MegaGeometryCounts *counts,
                                   const MegaCookedGeometry *sources,
                                   MegaGeometryBases *bases) {
  if (!ctx || !counts || !sources || !bases || !geometry_legacy_allowed())
    return false;
  if (!megaGeometryPreflight(ctx, counts))
    return false;
  ObsGeometryBudget budget;
  if (!geometry_budget_for(ctx, counts, &budget))
    return false;
  /* Plan first, then prove each borrowed source is the expected length.
   * All eight are owned in the upload batch before Vulkan is touched. */
  ObsGeometryUploadPlan plan = {0};
  if (!obs_geometry_upload_plan(&budget, &plan))
    return false;
  const void *data[OBS_GEOM_POOL_COUNT] = {
      sources->vertices,         sources->indices,
      sources->morphs,           sources->meshlets,
      sources->bounds,           sources->skins,
      sources->meshlet_vertices, sources->meshlet_triangles,
  };
  ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
  for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
    if (plan.copies[i].source_bytes > SIZE_MAX)
      return false;
    spans[i] = (ObsGeometrySourceSpan){
        .data = data[i],
        .bytes = (size_t)plan.copies[i].source_bytes,
    };
  }
  ObsGeometryUploadBatch batch = {0};
  if (!obs_geometry_upload_prepare_memory(spans, &budget, &batch))
    return false;
  return mega_geometry_submit_prepared(ctx, counts, &batch, bases);
}

uint32_t megaBufferAllocate(VulkanContext *ctx, Vertex *vertices,
                            uint32_t vertexCount) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.vertices = vertexCount};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = vertexCount * sizeof(Vertex);
  VkDeviceSize dstOffset =
      (VkDeviceSize)ctx->megaVertexBufferOffset * sizeof(Vertex);

  if (dstOffset + uploadSize > ctx->megaVertexBufferSize) {
    fprintf(stderr, "Mega vertex buffer overflow!\n");
    return UINT32_MAX;
  }

  /* if this upload won't fit in the staging buffer, flush first */
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize) {
    flushUploadStagingBuffer(ctx);
  }

  if (ctx->uploadStagingBuffer) {
    /* fast path: memcpy into persistent staging, record region */
    memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
           vertices, uploadSize);

    if (ctx->pendingVertexCopyCount == ctx->pendingVertexCopyCapacity) {
      ctx->pendingVertexCopyCapacity *= 2;
      ctx->pendingVertexCopies =
          realloc(ctx->pendingVertexCopies,
                  ctx->pendingVertexCopyCapacity * sizeof(VkBufferCopy));
    }
    ctx->pendingVertexCopies[ctx->pendingVertexCopyCount++] =
        (VkBufferCopy){.srcOffset = ctx->uploadStagingOffset,
                       .dstOffset = dstOffset,
                       .size = uploadSize};
    ctx->uploadStagingOffset += uploadSize;
  } else {
    /* fallback: old per-mesh staging path */
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
    memcpy(mapped, vertices, uploadSize);
    vkUnmapMemory(ctx->device, stagingMem);
    copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, stagingBuf,
               ctx->megaVertexBuffer, uploadSize, 0, dstOffset);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  uint32_t baseVertex = ctx->megaVertexBufferOffset;
  ctx->megaVertexBufferOffset += vertexCount;
  return baseVertex;
}

uint32_t megaBufferAllocateFromFile(VulkanContext *ctx, FILE *f,
                                    uint32_t vertexCount) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.vertices = vertexCount};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = vertexCount * sizeof(Vertex);
  VkDeviceSize dstOffset =
      (VkDeviceSize)ctx->megaVertexBufferOffset * sizeof(Vertex);

  if (dstOffset + uploadSize > ctx->megaVertexBufferSize)
    return UINT32_MAX;

  if (ctx->uploadStagingBuffer && uploadSize > ctx->uploadStagingSize)
    return UINT32_MAX;
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);

  if (ctx->uploadStagingBuffer) {
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, 1,
              uploadSize, f) != (size_t)(uploadSize))
      return UINT32_MAX;
    if (ctx->pendingVertexCopyCount == ctx->pendingVertexCopyCapacity) {
      ctx->pendingVertexCopyCapacity *= 2;
      ctx->pendingVertexCopies =
          realloc(ctx->pendingVertexCopies,
                  ctx->pendingVertexCopyCapacity * sizeof(VkBufferCopy));
    }
    ctx->pendingVertexCopies[ctx->pendingVertexCopyCount++] =
        (VkBufferCopy){.srcOffset = ctx->uploadStagingOffset,
                       .dstOffset = dstOffset,
                       .size = uploadSize};
    ctx->uploadStagingOffset += uploadSize;
  } else {
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
    if (fread(mapped, 1, uploadSize, f) != (size_t)(uploadSize)) {
      vkUnmapMemory(ctx->device, stagingMem);
      vkDestroyBuffer(ctx->device, stagingBuf, NULL);
      vkFreeMemory(ctx->device, stagingMem, NULL);
      return UINT32_MAX;
    }
    vkUnmapMemory(ctx->device, stagingMem);
    copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, stagingBuf,
               ctx->megaVertexBuffer, uploadSize, 0, dstOffset);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  uint32_t baseVertex = ctx->megaVertexBufferOffset;
  ctx->megaVertexBufferOffset += vertexCount;
  return baseVertex;
}

uint32_t megaMorphBufferAllocate(VulkanContext *ctx, MorphDelta *deltas,
                                 uint32_t deltaCount) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.morphs = deltaCount};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = (VkDeviceSize)deltaCount * sizeof(MorphDelta);
  VkDeviceSize dstOffset =
      (VkDeviceSize)ctx->megaMorphBufferOffset * sizeof(MorphDelta);

  if (dstOffset + uploadSize > ctx->megaMorphBufferSize) {
    fprintf(stderr,
            "megaMorphBuffer overflow! Need %.1f MB, have %.1f MB remaining\n",
            (double)uploadSize / (1024 * 1024),
            (double)(ctx->megaMorphBufferSize - dstOffset) / (1024 * 1024));
    return UINT32_MAX;
  }

  // Piggyback on the upload staging buffer if available
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize <= ctx->uploadStagingSize) {
    memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
           deltas, uploadSize);
    // Direct copy to megaMorphBuffer (separate destination from vertex/index)
    VkCommandBuffer cmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);
    VkBufferCopy region = {.srcOffset = ctx->uploadStagingOffset,
                           .dstOffset = dstOffset,
                           .size = uploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, ctx->megaMorphBuffer, 1,
                    &region);
    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          cmd);
    ctx->uploadStagingOffset += uploadSize;
  } else {
    // Fallback: dedicated staging buffer
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
    memcpy(mapped, deltas, uploadSize);
    vkUnmapMemory(ctx->device, stagingMem);
    copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, stagingBuf,
               ctx->megaMorphBuffer, uploadSize, 0, dstOffset);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  uint32_t baseOffset = ctx->megaMorphBufferOffset;
  ctx->megaMorphBufferOffset += deltaCount;
  return baseOffset;
}

uint32_t megaMeshletBufferAllocateFromFile(VulkanContext *ctx, FILE *f,
                                           uint32_t count) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.meshlets = count};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize mlUploadSize = count * sizeof(Meshlet);
  VkDeviceSize boundsUploadSize = count * sizeof(MeshletBounds);
  VkDeviceSize skinUploadSize = count * sizeof(MeshletSkinData);
  VkDeviceSize totalUploadSize =
      mlUploadSize + boundsUploadSize + skinUploadSize;

  VkDeviceSize dstOffsetML = megaMeshletBufferOffset * sizeof(Meshlet);
  VkDeviceSize dstOffsetBounds =
      megaMeshletBufferOffset * sizeof(MeshletBounds);
  VkDeviceSize dstOffsetSkin =
      megaMeshletBufferOffset * sizeof(MeshletSkinData);

  if (dstOffsetML + mlUploadSize > megaMeshletBufferSize)
    return UINT32_MAX;

  if (ctx->uploadStagingBuffer && totalUploadSize > ctx->uploadStagingSize)
    return UINT32_MAX;
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + totalUploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);

  if (ctx->uploadStagingBuffer) {
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, 1,
              mlUploadSize, f) != (size_t)(mlUploadSize))
      return UINT32_MAX;
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset +
                  mlUploadSize,
              1, boundsUploadSize, f) != (size_t)(boundsUploadSize))
      return UINT32_MAX;
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset +
                  mlUploadSize + boundsUploadSize,
              1, skinUploadSize, f) != (size_t)(skinUploadSize))
      return UINT32_MAX;

    VkCommandBuffer cmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);
    VkBufferCopy regionML = {.srcOffset = ctx->uploadStagingOffset,
                             .dstOffset = dstOffsetML,
                             .size = mlUploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletBuffer, 1,
                    &regionML);

    VkBufferCopy regionBounds = {.srcOffset =
                                     ctx->uploadStagingOffset + mlUploadSize,
                                 .dstOffset = dstOffsetBounds,
                                 .size = boundsUploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletBoundsBuffer, 1,
                    &regionBounds);

    VkBufferCopy regionSkin = {.srcOffset = ctx->uploadStagingOffset +
                                            mlUploadSize + boundsUploadSize,
                               .dstOffset = dstOffsetSkin,
                               .size = skinUploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletSkinBuffer, 1,
                    &regionSkin);

    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          cmd);
    ctx->uploadStagingOffset += totalUploadSize;
  }

  else {
    return UINT32_MAX;
  } // No usable staging buffer for cache upload.

  uint32_t baseOffset = megaMeshletBufferOffset;
  megaMeshletBufferOffset += count;
  return baseOffset;
}

uint32_t megaMeshletVertexBufferAllocateFromFile(VulkanContext *ctx, FILE *f,
                                                 uint32_t count) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.meshlet_vertices = count};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = count * sizeof(uint32_t);
  VkDeviceSize dstOffset = megaMeshletVertexOffset * sizeof(uint32_t);

  if (dstOffset + uploadSize > megaMeshletVertexBufferSize)
    return UINT32_MAX;

  if (ctx->uploadStagingBuffer && uploadSize > ctx->uploadStagingSize)
    return UINT32_MAX;
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);

  if (ctx->uploadStagingBuffer) {
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, 1,
              uploadSize, f) != (size_t)(uploadSize))
      return UINT32_MAX;
    VkCommandBuffer cmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);
    VkBufferCopy region = {.srcOffset = ctx->uploadStagingOffset,
                           .dstOffset = dstOffset,
                           .size = uploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletVertexBuffer, 1,
                    &region);
    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          cmd);
    ctx->uploadStagingOffset += uploadSize;
  }

  else {
    return UINT32_MAX;
  } // No usable staging buffer for cache upload.

  uint32_t baseOffset = megaMeshletVertexOffset;
  megaMeshletVertexOffset += count;
  return baseOffset;
}

uint32_t megaMeshletTriangleBufferAllocateFromFile(VulkanContext *ctx, FILE *f,
                                                   uint32_t count) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.meshlet_triangles = count};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = count * sizeof(uint8_t);
  VkDeviceSize dstOffset = megaMeshletTriangleOffset * sizeof(uint8_t);

  if (dstOffset + uploadSize > megaMeshletTriangleBufferSize)
    return UINT32_MAX;

  if (ctx->uploadStagingBuffer && uploadSize > ctx->uploadStagingSize)
    return UINT32_MAX;
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);

  if (ctx->uploadStagingBuffer) {
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, 1,
              uploadSize, f) != (size_t)(uploadSize))
      return UINT32_MAX;
    VkCommandBuffer cmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);
    VkBufferCopy region = {.srcOffset = ctx->uploadStagingOffset,
                           .dstOffset = dstOffset,
                           .size = uploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, megaMeshletTriangleBuffer, 1,
                    &region);
    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          cmd);
    ctx->uploadStagingOffset += uploadSize;
  }

  else {
    return UINT32_MAX;
  } // No usable staging buffer for cache upload.

  uint32_t baseOffset = megaMeshletTriangleOffset;
  megaMeshletTriangleOffset += count;
  return baseOffset;
}

uint32_t megaMeshletBufferAllocate(VulkanContext *ctx, Meshlet *meshlets,
                                   MeshletBounds *bounds,
                                   MeshletSkinData *skins, uint32_t count) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.meshlets = count};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize mlSize = count * sizeof(Meshlet);
  VkDeviceSize boundsSize = count * sizeof(MeshletBounds);
  VkDeviceSize skinSize = count * sizeof(MeshletSkinData);

  VkDeviceSize dstOffsetML = megaMeshletBufferOffset * sizeof(Meshlet);
  VkDeviceSize dstOffsetBounds =
      megaMeshletBufferOffset * sizeof(MeshletBounds);
  VkDeviceSize dstOffsetSkin =
      megaMeshletBufferOffset * sizeof(MeshletSkinData);

  if (dstOffsetML + mlSize > megaMeshletBufferSize) {
    fprintf(stderr, "megaMeshletBuffer overflow!\n");
    return UINT32_MAX;
  }
  if (ctx->uploadStagingOffset + mlSize + boundsSize + skinSize >
      ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);

  memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
         meshlets, mlSize);
  if (ctx->pendingMeshletCopyCount == ctx->pendingMeshletCopyCapacity) {
    ctx->pendingMeshletCopyCapacity *= 2;
    ctx->pendingMeshletCopies =
        realloc(ctx->pendingMeshletCopies,
                ctx->pendingMeshletCopyCapacity * sizeof(VkBufferCopy));
  }
  ctx->pendingMeshletCopies[ctx->pendingMeshletCopyCount++] =
      (VkBufferCopy){ctx->uploadStagingOffset, dstOffsetML, mlSize};
  ctx->uploadStagingOffset += mlSize;

  memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, bounds,
         boundsSize);
  if (ctx->pendingMeshletBoundsCopyCount ==
      ctx->pendingMeshletBoundsCopyCapacity) {
    ctx->pendingMeshletBoundsCopyCapacity *= 2;
    ctx->pendingMeshletBoundsCopies =
        realloc(ctx->pendingMeshletBoundsCopies,
                ctx->pendingMeshletBoundsCopyCapacity * sizeof(VkBufferCopy));
  }
  ctx->pendingMeshletBoundsCopies[ctx->pendingMeshletBoundsCopyCount++] =
      (VkBufferCopy){ctx->uploadStagingOffset, dstOffsetBounds, boundsSize};
  ctx->uploadStagingOffset += boundsSize;

  //  Batched Upload: Queue skins into the pending copy arrays
  memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, skins,
         skinSize);
  if (ctx->pendingMeshletSkinCopyCount == ctx->pendingMeshletSkinCopyCapacity) {
    ctx->pendingMeshletSkinCopyCapacity *= 2;
    ctx->pendingMeshletSkinCopies =
        realloc(ctx->pendingMeshletSkinCopies,
                ctx->pendingMeshletSkinCopyCapacity * sizeof(VkBufferCopy));
  }
  ctx->pendingMeshletSkinCopies[ctx->pendingMeshletSkinCopyCount++] =
      (VkBufferCopy){ctx->uploadStagingOffset, dstOffsetSkin, skinSize};
  ctx->uploadStagingOffset += skinSize;

  uint32_t base = megaMeshletBufferOffset;
  megaMeshletBufferOffset += count;
  return base;
}

uint32_t megaMeshletVertexBufferAllocate(VulkanContext *ctx, uint32_t *vertices,
                                         uint32_t count) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.meshlet_vertices = count};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = count * sizeof(uint32_t);
  VkDeviceSize dstOffset = megaMeshletVertexOffset * sizeof(uint32_t);
  if (dstOffset + uploadSize > megaMeshletVertexBufferSize) {
    fprintf(stderr, "megaMeshletVertexBuffer overflow!\n");
    return UINT32_MAX;
  }
  if (ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);
  memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
         vertices, uploadSize);
  if (ctx->pendingMeshletVertexCopyCount ==
      ctx->pendingMeshletVertexCopyCapacity) {
    ctx->pendingMeshletVertexCopyCapacity *= 2;
    ctx->pendingMeshletVertexCopies =
        realloc(ctx->pendingMeshletVertexCopies,
                ctx->pendingMeshletVertexCopyCapacity * sizeof(VkBufferCopy));
  }
  ctx->pendingMeshletVertexCopies[ctx->pendingMeshletVertexCopyCount++] =
      (VkBufferCopy){ctx->uploadStagingOffset, dstOffset, uploadSize};
  ctx->uploadStagingOffset += uploadSize;
  uint32_t base = megaMeshletVertexOffset;
  megaMeshletVertexOffset += count;
  return base;
}

uint32_t megaMeshletTriangleBufferAllocate(VulkanContext *ctx,
                                           uint8_t *triangles, uint32_t count) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.meshlet_triangles = count};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = count * sizeof(uint8_t);
  VkDeviceSize dstOffset = megaMeshletTriangleOffset * sizeof(uint8_t);
  if (dstOffset + uploadSize > megaMeshletTriangleBufferSize) {
    fprintf(stderr, "megaMeshletTriangleBuffer overflow!\n");
    return UINT32_MAX;
  }
  if (ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);
  memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
         triangles, uploadSize);
  if (ctx->pendingMeshletTriangleCopyCount ==
      ctx->pendingMeshletTriangleCopyCapacity) {
    ctx->pendingMeshletTriangleCopyCapacity *= 2;
    ctx->pendingMeshletTriangleCopies =
        realloc(ctx->pendingMeshletTriangleCopies,
                ctx->pendingMeshletTriangleCopyCapacity * sizeof(VkBufferCopy));
  }
  ctx->pendingMeshletTriangleCopies[ctx->pendingMeshletTriangleCopyCount++] =
      (VkBufferCopy){ctx->uploadStagingOffset, dstOffset, uploadSize};
  ctx->uploadStagingOffset += uploadSize;
  uint32_t base = megaMeshletTriangleOffset;
  megaMeshletTriangleOffset += count;
  return base;
}

uint32_t megaMorphBufferAllocateFromFile(VulkanContext *ctx, FILE *f,
                                         uint32_t deltaCount) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.morphs = deltaCount};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = (VkDeviceSize)deltaCount * sizeof(MorphDelta);
  VkDeviceSize dstOffset =
      (VkDeviceSize)ctx->megaMorphBufferOffset * sizeof(MorphDelta);

  if (dstOffset + uploadSize > ctx->megaMorphBufferSize)
    return UINT32_MAX;

  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize <= ctx->uploadStagingSize) {
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, 1,
              uploadSize, f) != (size_t)(uploadSize))
      return UINT32_MAX;
    VkCommandBuffer cmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);
    VkBufferCopy region = {.srcOffset = ctx->uploadStagingOffset,
                           .dstOffset = dstOffset,
                           .size = uploadSize};
    vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, ctx->megaMorphBuffer, 1,
                    &region);
    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          cmd);
    ctx->uploadStagingOffset += uploadSize;
  } else {
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
    if (fread(mapped, 1, uploadSize, f) != (size_t)(uploadSize)) {
      vkUnmapMemory(ctx->device, stagingMem);
      vkDestroyBuffer(ctx->device, stagingBuf, NULL);
      vkFreeMemory(ctx->device, stagingMem, NULL);
      return UINT32_MAX;
    }
    vkUnmapMemory(ctx->device, stagingMem);
    copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, stagingBuf,
               ctx->megaMorphBuffer, uploadSize, 0, dstOffset);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  uint32_t baseOffset = ctx->megaMorphBufferOffset;
  ctx->megaMorphBufferOffset += deltaCount;
  return baseOffset;
}

void createMegaIndexBuffer(VulkanContext *ctx, VkDeviceSize size) {
  ctx->megaIndexBufferSize = size;
  ctx->megaIndexBufferOffset = 0;
  createBuffer(ctx, size,
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->megaIndexBuffer,
               &ctx->megaIndexBufferMemory);

  uint32_t *linearIndices = malloc(1048576 * sizeof(uint32_t));
  for (uint32_t i = 0; i < 1048576; i++)
    linearIndices[i] = i;
  megaIndexBufferAllocate(ctx, linearIndices, 1048576);
  free(linearIndices);
  flushUploadStagingBuffer(ctx);

  fprintf(stdout, "Mega index buffer: %.1f MB (DEVICE_LOCAL)\n",
          (double)size / (1024.0 * 1024.0));
}

/* Upload indices into the mega index buffer. Returns the BASE INDEX (firstIndex
 * for DrawIndexed). */
uint32_t megaIndexBufferAllocate(VulkanContext *ctx, uint32_t *indices,
                                 uint32_t indexCount) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.indices = indexCount};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = indexCount * sizeof(uint32_t);
  VkDeviceSize dstOffset =
      (VkDeviceSize)ctx->megaIndexBufferOffset * sizeof(uint32_t);

  if (dstOffset + uploadSize > ctx->megaIndexBufferSize) {
    fprintf(stderr, "Mega index buffer overflow!\n");
    return UINT32_MAX;
  }

  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize) {
    flushUploadStagingBuffer(ctx);
  }

  if (ctx->uploadStagingBuffer) {
    memcpy((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
           indices, uploadSize);

    if (ctx->pendingIndexCopyCount == ctx->pendingIndexCopyCapacity) {
      ctx->pendingIndexCopyCapacity *= 2;
      ctx->pendingIndexCopies =
          realloc(ctx->pendingIndexCopies,
                  ctx->pendingIndexCopyCapacity * sizeof(VkBufferCopy));
    }
    ctx->pendingIndexCopies[ctx->pendingIndexCopyCount++] =
        (VkBufferCopy){.srcOffset = ctx->uploadStagingOffset,
                       .dstOffset = dstOffset,
                       .size = uploadSize};
    ctx->uploadStagingOffset += uploadSize;
  } else {
    /* fallback */
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
    memcpy(mapped, indices, uploadSize);
    vkUnmapMemory(ctx->device, stagingMem);
    copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, stagingBuf,
               ctx->megaIndexBuffer, uploadSize, 0, dstOffset);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  uint32_t baseIndex = ctx->megaIndexBufferOffset;
  ctx->megaIndexBufferOffset += indexCount;
  return baseIndex;
}

uint32_t megaIndexBufferAllocateFromFile(VulkanContext *ctx, FILE *f,
                                         uint32_t indexCount) {
  if (!geometry_legacy_allowed())
    return UINT32_MAX;
  const MegaGeometryCounts needed = {.indices = indexCount};
  if (!megaGeometryPreflight(ctx, &needed))
    return UINT32_MAX;
  VkDeviceSize uploadSize = indexCount * sizeof(uint32_t);
  VkDeviceSize dstOffset =
      (VkDeviceSize)ctx->megaIndexBufferOffset * sizeof(uint32_t);

  if (dstOffset + uploadSize > ctx->megaIndexBufferSize)
    return UINT32_MAX;

  if (ctx->uploadStagingBuffer && uploadSize > ctx->uploadStagingSize)
    return UINT32_MAX;
  if (ctx->uploadStagingBuffer &&
      ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize)
    flushUploadStagingBuffer(ctx);

  if (ctx->uploadStagingBuffer) {
    if (fread((uint8_t *)ctx->uploadStagingMapped + ctx->uploadStagingOffset, 1,
              uploadSize, f) != (size_t)(uploadSize))
      return UINT32_MAX;
    if (ctx->pendingIndexCopyCount == ctx->pendingIndexCopyCapacity) {
      ctx->pendingIndexCopyCapacity *= 2;
      ctx->pendingIndexCopies =
          realloc(ctx->pendingIndexCopies,
                  ctx->pendingIndexCopyCapacity * sizeof(VkBufferCopy));
    }
    ctx->pendingIndexCopies[ctx->pendingIndexCopyCount++] =
        (VkBufferCopy){.srcOffset = ctx->uploadStagingOffset,
                       .dstOffset = dstOffset,
                       .size = uploadSize};
    ctx->uploadStagingOffset += uploadSize;
  } else {
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
    if (fread(mapped, 1, uploadSize, f) != (size_t)(uploadSize)) {
      vkUnmapMemory(ctx->device, stagingMem);
      vkDestroyBuffer(ctx->device, stagingBuf, NULL);
      vkFreeMemory(ctx->device, stagingMem, NULL);
      return UINT32_MAX;
    }
    vkUnmapMemory(ctx->device, stagingMem);
    copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue, stagingBuf,
               ctx->megaIndexBuffer, uploadSize, 0, dstOffset);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  uint32_t baseIndex = ctx->megaIndexBufferOffset;
  ctx->megaIndexBufferOffset += indexCount;
  return baseIndex;
}

bool createDynamicBuffers(VulkanContext *ctx, VkDeviceSize size) {
  if (!ctx || size == 0)
    return false;
  if (ctx->dynamicStagingMapped && size <= ctx->dynamicStagingSize)
    return true;

  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = NULL;
  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (vkCreateBuffer(ctx->device, &buffer_info, NULL, &buffer) != VK_SUCCESS)
    return false;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(ctx->device, buffer, &requirements);
  const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex =
          findMemoryType(ctx->physicalDevice, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
  };
  if (vkAllocateMemory(ctx->device, &allocation, NULL, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(ctx->device, buffer, memory, 0) != VK_SUCCESS ||
      vkMapMemory(ctx->device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
    if (memory)
      vkFreeMemory(ctx->device, memory, NULL);
    vkDestroyBuffer(ctx->device, buffer, NULL);
    return false;
  }

  const uint32_t old_stride = OBS_DYNAMIC_VERTEX_STRIDE(ctx);
  const uint32_t new_stride =
      (uint32_t)(size / (MAX_FRAMES_IN_FLIGHT * sizeof(Vertex)));
  if (ctx->dynamicStagingMapped && old_stride && new_stride >= old_stride) {
    Vertex *old_vertices = (Vertex *)ctx->dynamicStagingMapped;
    Vertex *new_vertices = (Vertex *)mapped;
    const size_t frame_bytes = (size_t)old_stride * sizeof(Vertex);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
      memcpy(new_vertices + (size_t)frame * new_stride,
             old_vertices + (size_t)frame * old_stride, frame_bytes);
    }
  }
  if (ctx->dynamicStagingBuffer != VK_NULL_HANDLE) {
    if (vkDeviceWaitIdle(ctx->device) != VK_SUCCESS) {
      vkUnmapMemory(ctx->device, memory);
      vkFreeMemory(ctx->device, memory, NULL);
      vkDestroyBuffer(ctx->device, buffer, NULL);
      return false;
    }
    if (ctx->dynamicStagingMapped)
      vkUnmapMemory(ctx->device, ctx->dynamicStagingMemory);
    vkDestroyBuffer(ctx->device, ctx->dynamicStagingBuffer, NULL);
    vkFreeMemory(ctx->device, ctx->dynamicStagingMemory, NULL);
  }
  ctx->dynamicStagingBuffer = buffer;
  ctx->dynamicStagingMemory = memory;
  ctx->dynamicStagingMapped = mapped;
  ctx->dynamicStagingSize = size;
  return true;
}

/// Bindless texture array

void createBindlessDescriptorLayout(VulkanContext *ctx) {
  /* One binding: array of MAX_TEXTURES combined-image-samplers,
     all accessible from the fragment stage.
     VARIABLE_DESCRIPTOR_COUNT lets us allocate fewer than MAX_TEXTURES
     at pool/set creation time without validation errors.               */
  VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = MAX_TEXTURES,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                    VK_SHADER_STAGE_MESH_BIT_EXT,
      .pImmutableSamplers = NULL};

  VkDescriptorBindingFlagsEXT bindingFlags =
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT |
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;

  VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsCI = {
      .sType =
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT,
      .bindingCount = 1,
      .pBindingFlags = &bindingFlags,
  };

  VkDescriptorSetLayoutCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = &bindingFlagsCI,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT,
      .bindingCount = 1,
      .pBindings = &binding};
  if (vkCreateDescriptorSetLayout(ctx->device, &ci, NULL,
                                  &ctx->bindlessSetLayout) != VK_SUCCESS) {
    fprintf(stderr, "Failed to create bindless descriptor set layout\n");
    exit(EXIT_FAILURE);
  }
}

void createBindlessDescriptorPool(VulkanContext *ctx) {
  VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             .descriptorCount = MAX_TEXTURES};
  VkDescriptorPoolCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      /* UPDATE_AFTER_BIND lets us write slots incrementally as textures load */
      .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &ps};
  if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &ctx->bindlessPool) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to create bindless descriptor pool\n");
    exit(EXIT_FAILURE);
  }
}

void createBindlessDescriptorSet(VulkanContext *ctx) {
  VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = ctx->bindlessPool,
      .descriptorSetCount = 1,
      .pSetLayouts = &ctx->bindlessSetLayout};
  if (vkAllocateDescriptorSets(ctx->device, &ai, &ctx->bindlessSet) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to allocate bindless descriptor set\n");
    exit(EXIT_FAILURE);
  }
  ctx->bindlessTextureCount = 0;
}

/* Call once per texture after it's loaded to register it into the bindless
   array. Returns the slot index the shader will use. */
void bindlessRegisterTexture(VulkanContext *ctx, uint32_t slot,
                             VkImageView view, VkSampler sampler) {
  VkDescriptorImageInfo img = {.imageLayout =
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               .imageView = view,
                               .sampler = sampler};
  VkWriteDescriptorSet w = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = ctx->bindlessSet,
      .dstBinding = 0,
      .dstArrayElement = slot, /* write into slot N of the array */
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .pImageInfo = &img};
  vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
  if (ctx->bindlessTextureCount <= slot)
    ctx->bindlessTextureCount = slot + 1;
}

/// Lighting descriptors

void createLightingDescriptors(VulkanContext *ctx) {
  VkDescriptorSetLayoutBinding b[8] = {
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT |
                     VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_VERTEX_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 2,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 3,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 4,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 5,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 6,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 7,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT} //  HZB Map
  };
  VkDescriptorBindingFlagsEXT flags[8] = {
      0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT};
  VkDescriptorSetLayoutBindingFlagsCreateInfoEXT flagsInfo = {
      .sType =
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT,
      .bindingCount = 8,
      .pBindingFlags = flags};
  VkDescriptorSetLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = &flagsInfo,
      .bindingCount = 8,
      .pBindings = b};
  vkCreateDescriptorSetLayout(ctx->device, &lci, NULL, &ctx->lightingSetLayout);

  VkDescriptorPoolSize ps[2] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT * 6}};
  VkDescriptorPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = MAX_FRAMES_IN_FLIGHT,
      .poolSizeCount = 2,
      .pPoolSizes = ps};
  vkCreateDescriptorPool(ctx->device, &pci, NULL, &ctx->lightingPool);

  VkDeviceSize size = sizeof(LightingData);
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    vkCreateBuffer(ctx->device, &bci, NULL, &ctx->lightingUBO[i]);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(ctx->device, ctx->lightingUBO[i], &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = mr.size,
                               .memoryTypeIndex = findMemoryType(
                                   ctx->physicalDevice, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(ctx->device, &ai, NULL, &ctx->lightingUBOMemory[i]);
    vkBindBufferMemory(ctx->device, ctx->lightingUBO[i],
                       ctx->lightingUBOMemory[i], 0);
    vkMapMemory(ctx->device, ctx->lightingUBOMemory[i], 0, size, 0,
                &ctx->lightingUBOMapped[i]);

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = ctx->lightingPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &ctx->lightingSetLayout};
    vkAllocateDescriptorSets(ctx->device, &dsai, &ctx->lightingSets[i]);

    VkDescriptorBufferInfo dbi = {
        .buffer = ctx->lightingUBO[i], .offset = 0, .range = size};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = ctx->lightingSets[i],
                              .dstBinding = 0,
                              .descriptorType =
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              .descriptorCount = 1,
                              .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
  }

  // If shadows were created before lighting, map Binding 5 now!
  if (ctx->shadowView[0] != VK_NULL_HANDLE) {
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
      VkDescriptorImageInfo dInfo = {
          .sampler = ctx->shadowSampler,
          .imageView = ctx->shadowView[f],
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = ctx->lightingSets[f],
                                .dstBinding = 5,
                                .descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .descriptorCount = 1,
                                .pImageInfo = &dInfo};
      vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
  }

  //  FIX: If depth resources (transmission) were created before lighting, map
  //  Binding 6 now!
  if (ctx->primary && ctx->primary->transmissionView[0] != VK_NULL_HANDLE) {
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
      VkDescriptorImageInfo dInfo = {
          .sampler = transmissionSampler,
          .imageView = ctx->primary->transmissionView[f],
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = ctx->lightingSets[f],
                                .dstBinding = 6,
                                .descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .descriptorCount = 1,
                                .pImageInfo = &dInfo};
      vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
  }

  //  FIX: Ensure HZB is mapped to Binding 7 on startup
  if (ctx->primary && ctx->primary->hzbView[0] != VK_NULL_HANDLE) {
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
      uint32_t prevFrame =
          (f + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
      VkDescriptorImageInfo dInfo = {.sampler = hzbSampler,
                                     .imageView =
                                         ctx->primary->hzbView[prevFrame],
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = ctx->lightingSets[f],
                                .dstBinding = 7,
                                .descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .descriptorCount = 1,
                                .pImageInfo = &dInfo};
      vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
  }

  fprintf(stdout, "Lighting UBO descriptors created\n");
}

void updateLightingUBO(VulkanContext *ctx) {
  memcpy(ctx->lightingUBOMapped[ctx->currentFrame], ctx->lightingDataRaw,
         sizeof(LightingData));
}

/// SSBO + Indirect draw

void createMeshSSBO(VulkanContext *ctx, uint32_t maxMeshes) {
  if (maxMeshes == 0)
    maxMeshes = 1;
  uint64_t capacity64 = (((uint64_t)maxMeshes + 63u) / 64u) * 64u;
  if (capacity64 > UINT32_MAX)
    return;
  uint32_t capacity = (uint32_t)capacity64;
  maxMeshes = capacity;
  VkDeviceSize size = maxMeshes * sizeof(MeshGPUData);

  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    createBuffer(ctx, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &ctx->meshSSBO[i], &ctx->meshSSBOMemory[i]);
    vkMapMemory(ctx->device, ctx->meshSSBOMemory[i], 0, size, 0,
                &ctx->meshSSBOMapped[i]);

    // EXTRACT THE RAW GPU POINTER! No descriptor sets needed ever again.
    VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = ctx->meshSSBO[i]};
    meshSSBOAddr[i] = vkGetBufferDeviceAddress(ctx->device, &info);
  }
  fprintf(stdout, "Mesh SSBO (Physical Pointers): %.1f MB x%d frames\n",
          (double)size / (1024 * 1024), MAX_FRAMES_IN_FLIGHT);

  /* allocate per-mesh dirty bitfield — one uint64 per 64 meshes per frame */
  ctx->meshDirtyCapacity = capacity;
  uint32_t words = ctx->meshDirtyCapacity / 64;
  ctx->meshDirtyBits = malloc(words * MAX_FRAMES_IN_FLIGHT * sizeof(uint64_t));
  /* start fully dirty so first frames upload everything */
  memset(ctx->meshDirtyBits, 0xFF,
         words * MAX_FRAMES_IN_FLIGHT * sizeof(uint64_t));

  // ── CREATE GLOBAL JOINT SSBO ──
  //  Packing: Store 3 vec4s (48 bytes) instead of mat4 (64 bytes). Saves 25%
  //  memory!
  VkDeviceSize jointSize = 16384 * 48;
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    createBuffer(ctx, jointSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &jointSSBO[i], &jointSSBOMemory[i]);
    vkMapMemory(ctx->device, jointSSBOMemory[i], 0, jointSize, 0,
                (void **)&jointSSBOMapped[i]);

    VkBufferDeviceAddressInfo jInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = jointSSBO[i]};
    jointSSBOAddr[i] = vkGetBufferDeviceAddress(ctx->device, &jInfo);
  }
  fprintf(stdout, "Global Joint SSBO (Physical Pointers): %.1f MB x%d frames\n",
          (double)jointSize / (1024 * 1024), MAX_FRAMES_IN_FLIGHT);

  // ── CREATE SDF SSBO ──
  VkDeviceSize sdfSize = MAX_SDF_PRIMITIVES * sizeof(SdfPrimitive);
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    createBuffer(ctx, sdfSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &sdfSSBO[i], &sdfSSBOMemory[i]);
    vkMapMemory(ctx->device, sdfSSBOMemory[i], 0, sdfSize, 0,
                (void **)&sdfSSBOMapped[i]);

    VkBufferDeviceAddressInfo sdfInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = sdfSSBO[i]};
    sdfSSBOAddr[i] = vkGetBufferDeviceAddress(ctx->device, &sdfInfo);
  }
  fprintf(stdout, "SDF SSBO (Physical Pointers): %.2f MB x%d frames\n",
          (double)sdfSize / (1024 * 1024), MAX_FRAMES_IN_FLIGHT);
}

static bool mesh_ssbo_reserve(VulkanContext *ctx, uint32_t required_meshes) {
  if (!ctx)
    return false;
  if (required_meshes <= ctx->meshDirtyCapacity)
    return true;

  uint64_t capacity = ctx->meshDirtyCapacity ? ctx->meshDirtyCapacity : 64u;
  while (capacity < required_meshes) {
    if (capacity > UINT32_MAX / 2u)
      return false;
    capacity *= 2u;
  }
  capacity = ((capacity + 63u) / 64u) * 64u;
  if (capacity > SIZE_MAX / sizeof(MeshGPUData))
    return false;
  const VkDeviceSize new_size = (VkDeviceSize)capacity * sizeof(MeshGPUData);
  const size_t words = (size_t)(capacity / 64u);
  if (words > SIZE_MAX / MAX_FRAMES_IN_FLIGHT / sizeof(uint64_t))
    return false;
  uint64_t *new_dirty =
      malloc(words * MAX_FRAMES_IN_FLIGHT * sizeof(*new_dirty));
  if (!new_dirty)
    return false;

  VkBuffer new_buffers[MAX_FRAMES_IN_FLIGHT] = {0};
  VkDeviceMemory new_memory[MAX_FRAMES_IN_FLIGHT] = {0};
  void *new_mapped[MAX_FRAMES_IN_FLIGHT] = {0};
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    createBuffer(ctx, new_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &new_buffers[i], &new_memory[i]);
    if (vkMapMemory(ctx->device, new_memory[i], 0, new_size, 0,
                    &new_mapped[i]) != VK_SUCCESS) {
      for (uint32_t j = 0; j <= i; ++j) {
        if (new_mapped[j])
          vkUnmapMemory(ctx->device, new_memory[j]);
        if (new_buffers[j])
          vkDestroyBuffer(ctx->device, new_buffers[j], NULL);
        if (new_memory[j])
          vkFreeMemory(ctx->device, new_memory[j], NULL);
      }
      free(new_dirty);
      return false;
    }
  }
  if (vkDeviceWaitIdle(ctx->device) != VK_SUCCESS) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
      vkUnmapMemory(ctx->device, new_memory[i]);
      vkDestroyBuffer(ctx->device, new_buffers[i], NULL);
      vkFreeMemory(ctx->device, new_memory[i], NULL);
    }
    free(new_dirty);
    return false;
  }
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    if (ctx->meshSSBOMapped[i])
      vkUnmapMemory(ctx->device, ctx->meshSSBOMemory[i]);
    if (ctx->meshSSBO[i])
      vkDestroyBuffer(ctx->device, ctx->meshSSBO[i], NULL);
    if (ctx->meshSSBOMemory[i])
      vkFreeMemory(ctx->device, ctx->meshSSBOMemory[i], NULL);
    ctx->meshSSBO[i] = new_buffers[i];
    ctx->meshSSBOMemory[i] = new_memory[i];
    ctx->meshSSBOMapped[i] = new_mapped[i];
    const VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = new_buffers[i],
    };
    meshSSBOAddr[i] = vkGetBufferDeviceAddress(ctx->device, &info);
  }
  free(ctx->meshDirtyBits);
  ctx->meshDirtyBits = new_dirty;
  ctx->meshDirtyCapacity = (uint32_t)capacity;
  memset(new_dirty, 0xFF, words * MAX_FRAMES_IN_FLIGHT * sizeof(*new_dirty));
  fprintf(stdout, "Mesh SSBO grew to %.2f MiB x%d frames\n",
          (double)new_size / (1024.0 * 1024.0), MAX_FRAMES_IN_FLIGHT);
  return true;
}

bool obs_mesh_ssbo_reserve(VulkanContext *ctx, uint32_t required_meshes) {
  return mesh_ssbo_reserve(ctx, required_meshes);
}

void createIndirectBuffer(VulkanContext *ctx, uint32_t maxMeshes) {
  (void)maxMeshes;
  // Mesh shaders draw via vkCmdDrawMeshTasksEXT — no indirect buffer needed
  // there. This scratch buffer exists only for the legacy vertex shader path:
  // immediate-mode primitives, lines, and debug draws via emit_draw_with_slot.
  // Sized for two frames * (max static + max dynamic) slots.
  VkDeviceSize drawSize =
      (VkDeviceSize)(16384 + 4096) * sizeof(VkDrawIndexedIndirectCommand);
  VkDeviceSize totalSize = drawSize * MAX_FRAMES_IN_FLIGHT;

  VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = totalSize,
                            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  if (vkCreateBuffer(ctx->device, &bci, NULL, &ctx->indirectBuffer) !=
      VK_SUCCESS) {
    fprintf(stderr, "Failed to create immediate-mode scratch buffer\n");
    exit(EXIT_FAILURE);
  }

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(ctx->device, ctx->indirectBuffer, &req);

  VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = req.size,
                             .memoryTypeIndex = findMemoryType(
                                 ctx->physicalDevice, req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  if (vkAllocateMemory(ctx->device, &ai, NULL, &ctx->indirectBufferMemory) !=
      VK_SUCCESS) {
    fprintf(stderr,
            "Failed to allocate immediate-mode scratch buffer memory\n");
    exit(EXIT_FAILURE);
  }
  vkBindBufferMemory(ctx->device, ctx->indirectBuffer,
                     ctx->indirectBufferMemory, 0);
  vkMapMemory(ctx->device, ctx->indirectBufferMemory, 0, totalSize, 0,
              &ctx->srcIndirectBufferMapped);
  memset(ctx->srcIndirectBufferMapped, 0, totalSize);

  fprintf(stdout, "Immediate-mode scratch: %.1f MB\n",
          (double)totalSize / (1024.0 * 1024.0));
}

void markMeshesSSBODirty(VulkanContext *ctx) {
  /* mark all meshes dirty across all frames */
  if (ctx->meshDirtyBits) {
    uint32_t words = ctx->meshDirtyCapacity / 64;
    memset(ctx->meshDirtyBits, 0xFF,
           words * MAX_FRAMES_IN_FLIGHT * sizeof(uint64_t));
  }
}

void markMeshDirty(VulkanContext *ctx, uint32_t meshIndex) {
  uint32_t word = meshIndex / 64;
  uint64_t bit = 1ULL << (meshIndex % 64);
  if (ctx->meshDirtyBits && word < ctx->meshDirtyCapacity / 64) {
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
      ctx->meshDirtyBits[word * MAX_FRAMES_IN_FLIGHT + f] |= bit;
  }
}

void updateMeshSSBOAndIndirect(VulkanContext *ctx, Meshes *meshes) {
  if (meshes->count > UINT32_MAX ||
      !mesh_ssbo_reserve(ctx, (uint32_t)meshes->count)) {
    fprintf(stderr, "Could not grow mesh SSBO for %zu meshes\n", meshes->count);
    return;
  }
  uint32_t count = (uint32_t)meshes->count;
  if (!meshes->draw_indices)
    return;
  const uint32_t dynamic_frame_base =
      ctx->currentFrame * OBS_DYNAMIC_VERTEX_STRIDE(ctx);

  // CRITICAL: Shift pointer to the current frame to prevent GPU tearing!
  VkDeviceSize drawSize = (16384 + 4096) * sizeof(VkDrawIndexedIndirectCommand);
  VkDrawIndexedIndirectCommand *cmds =
      (VkDrawIndexedIndirectCommand *)((uint8_t *)ctx->srcIndirectBufferMapped +
                                       (ctx->currentFrame * drawSize));

  for (uint32_t i = 0; i < count; i++) {
    uint32_t mesh_idx = meshes->draw_indices[i];
    Mesh *m = &meshes->items[mesh_idx];

    if (m->megaBaseVertex == UINT32_MAX && m->dynamicBaseVertex == UINT32_MAX) {
      cmds[i].indexCount = 0;
      cmds[i].instanceCount = 0;
      cmds[i].firstIndex = 0;
      cmds[i].vertexOffset = 0;
      cmds[i].firstInstance = mesh_idx;
      continue;
    }

    // CRITICAL FIX: If a mesh has no indices (sequential triangles), we MUST
    // set indexCount to vertexCount regardless of whether it's a dynamic morph
    // target or a static mega-buffer mesh. Otherwise, non-indexed glTF
    // primitives evaluate to 0 and become completely invisible.
    cmds[i].indexCount = (m->indexCount == 0) ? m->vertexCount : m->indexCount;
    cmds[i].instanceCount = (m->megaBaseMeshlet != UINT32_MAX) ? 0 : 1;
    cmds[i].firstIndex =
        (m->megaBaseIndex == UINT32_MAX) ? 0 : m->megaBaseIndex;

    if (m->megaBaseVertex != UINT32_MAX) {
      cmds[i].vertexOffset = (int32_t)m->megaBaseVertex;
    } else {
      cmds[i].vertexOffset =
          (int32_t)(ctx->megaVertexBufferOffset +
                    dynamic_frame_base + m->dynamicBaseVertex);
    }

    // DOD ARCHITECTURE: The firstInstance points to the completely STATIC SSBO
    // slot!
    cmds[i].firstInstance = mesh_idx;
  }
}

/* Call once per frame from beginFrame - uploads SSBO only to currentFrame if
 * dirty */
void flushMeshSSBO(VulkanContext *ctx, Meshes *meshes) {
  if (meshes->count > UINT32_MAX ||
      !mesh_ssbo_reserve(ctx, (uint32_t)meshes->count)) {
    fprintf(stderr, "Could not grow mesh SSBO for %zu meshes\n", meshes->count);
    return;
  }
  /* Batch upload ALL morph weights in one contiguous memcpy.
     The morphWeightBuffer is laid out as a flat float array where
     each mesh's weights sit at mesh->morphWeightOffset.
     We find the total float count from the highest offset+count. */
  if (ctx->morphWeightMapped[ctx->currentFrame]) {
    uint32_t totalWeightFloats = 0;
    for (size_t i = 0; i < meshes->count; i++) {
      Mesh *m = &meshes->items[i];
      if (m->morph_data && m->morphCount > 0) {
        uint32_t end = (uint32_t)m->morphWeightOffset + (uint32_t)m->morphCount;
        if (end > totalWeightFloats)
          totalWeightFloats = end;
      }
    }
    if (totalWeightFloats > 0) {
      /* Single contiguous upload — one memcpy for ALL morph meshes */
      float *dst = (float *)ctx->morphWeightMapped[ctx->currentFrame];
      for (size_t i = 0; i < meshes->count; i++) {
        Mesh *m = &meshes->items[i];
        if (m->morph_data && m->morphCount > 0) {
          memcpy(dst + m->morphWeightOffset, m->morph_data->weights,
                 (size_t)m->morphCount * sizeof(float));
        }
      }
    }
  }

  uint32_t count = (uint32_t)meshes->count;
  if (count == 0)
    return;

  uint32_t f = ctx->currentFrame;
  const uint32_t dynamic_frame_base =
      ctx->currentFrame * OBS_DYNAMIC_VERTEX_STRIDE(ctx);
  uint32_t words = (count + 63) / 64;

  // Check if the current frame actually has any pending uploads
  bool has_dirty = false;
  if (ctx->meshDirtyBits) {
    for (uint32_t w = 0; w < words; w++) {
      if (ctx->meshDirtyBits[w * MAX_FRAMES_IN_FLIGHT + f]) {
        has_dirty = true;
        break;
      }
    }
  } else {
    has_dirty = true; // Fallback if bitset isn't allocated
  }

  if (!has_dirty)
    return;

  MeshGPUData *dst = (MeshGPUData *)ctx->meshSSBOMapped[f];

  for (uint32_t w = 0; w < words; w++) {
    uint64_t mask = (ctx->meshDirtyBits == NULL)
                        ? ~0ULL
                        : ctx->meshDirtyBits[w * MAX_FRAMES_IN_FLIGHT + f];

    /* HARDWARE INTRINSIC:
       Skip up to 64 clean meshes in a single CPU cycle.
       Zero branching on empty space. */
    while (mask) {
      int bitIdx = __builtin_ctzll(
          mask);        // Count trailing zeros = index of lowest set bit
      mask &= mask - 1; // Clear the lowest set bit

      uint32_t i = w * 64 + bitIdx;
      if (i >= count)
        break;

      Mesh *m = &meshes->items[i];
      if (m->megaBaseVertex == UINT32_MAX &&
          m->dynamicBaseVertex == UINT32_MAX) {
        memset(&dst[i], 0, sizeof(MeshGPUData));
        continue;
      }
      glm_mat4_copy(m->model, dst[i].model);

/* Helper macro to safely resolve texture pool index to bindless slot */
#define GET_BINDLESS(pool_idx)                                                 \
  ((pool_idx >= 0 && texture_pool_get(pool_idx) &&                             \
    texture_pool_get(pool_idx)->loaded)                                        \
       ? (int)texture_pool_get(pool_idx)->bindlessSlot                         \
       : -1)

      /* PBR texture slots */
      dst[i].albedoIndex = (m->texture && m->texture->loaded)
                               ? (int)m->texture->bindlessSlot
                               : -1;
      dst[i].normalMapIndex = GET_BINDLESS(m->normalMapIndex);
      dst[i].metallicRoughIndex = GET_BINDLESS(m->metallicRoughIndex);
      dst[i].aoIndex = GET_BINDLESS(m->aoIndex);
      dst[i].aoStrength = m->aoStrength;
      dst[i].emissiveIndex = GET_BINDLESS(m->emissiveIndex);

      /* Initialize displacement data to prevent uninitialized memory
         from sampling garbage textures and blowing up vertices! */
      dst[i].displacementIndex = -1;
      dst[i].displacementScale = 0.0f;

      /* Material constant factors */
      glm_vec4_copy(m->baseColorFactor, dst[i].baseColorFactor);
      dst[i].metallicFactor = m->metallicFactor;
      dst[i].roughnessFactor = m->roughnessFactor;
      dst[i].emissiveStrength = m->emissiveStrength;
      glm_vec3_copy(m->emissiveFactor, dst[i].emissiveFactor);

      dst[i].isUnlit = m->is_unlit ? 1 : 0;
      dst[i].alphaMode = m->alpha_mode;
      dst[i].alphaCutoff = m->alpha_cutoff;
      glm_vec3_copy(m->aabbMin, dst[i].aabbMin);
      glm_vec3_copy(m->aabbMax, dst[i].aabbMax);
      dst[i].aabbMin[3] = 0.0f;
      dst[i].aabbMax[3] = 0.0f;
      dst[i].jointOffset = m->jointOffset;
      dst[i].morphDeltaOffset =
          m->morphDeltaOffset - (int)(m->megaBaseVertex * m->morphCount);
      dst[i].morphWeightOffset = m->morphWeightOffset;
      dst[i].morphCount = m->morphCount;

      dst[i].meshletCount = m->meshletCount;
      dst[i].meshletOffset =
          (m->megaBaseMeshlet != UINT32_MAX) ? m->megaBaseMeshlet : -1;
      dst[i].meshletVertexOffset = (m->megaBaseMeshletVertex != UINT32_MAX)
                                       ? m->megaBaseMeshletVertex
                                       : -1;
      dst[i].meshletTriangleOffset = (m->megaBaseMeshletTriangle != UINT32_MAX)
                                         ? m->megaBaseMeshletTriangle
                                         : -1;

      dst[i].transmissionFactor = m->transmissionFactor;
      dst[i].ior = m->ior;
      dst[i].thicknessFactor = m->thicknessFactor;
      dst[i].transmissionIndex = GET_BINDLESS(m->transmissionIndex);
      dst[i].thicknessIndex = GET_BINDLESS(m->thicknessIndex);
      dst[i].attenuationColorR = m->attenuationColor[0];
      dst[i].attenuationColorG = m->attenuationColor[1];
      dst[i].attenuationColorB = m->attenuationColor[2];
      dst[i].attenuationDistance = m->attenuationDistance;
      dst[i].dispersion = m->dispersion;
      dst[i].isVisible = m->visible ? 1 : 0;
      dst[i].isWireframe = m->wireframe ? 1 : 0;
      dst[i].vertexOffset = (m->megaBaseVertex != UINT32_MAX)
                                ? m->megaBaseVertex
                                : (ctx->megaVertexBufferOffset +
                                   dynamic_frame_base + m->dynamicBaseVertex);
      dst[i].matcapMode = m->matcap_mode;
      dst[i]._pad3 = 0;

#undef GET_BINDLESS
    }

    /* Clear the entire 64-bit block for this frame instantly */
    if (ctx->meshDirtyBits && w < ctx->meshDirtyCapacity / 64) {
      ctx->meshDirtyBits[w * MAX_FRAMES_IN_FLIGHT + f] = 0;
    }
  }
}

/// create_ibl_image — implementation group
static void create_ibl_image(VulkanContext *ctx, uint32_t w, uint32_t h,
                             uint32_t mips, uint32_t layers, VkFormat format,
                             VkImageUsageFlags usage, VkImage *img,
                             VkDeviceMemory *mem) {
  VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                          .imageType = VK_IMAGE_TYPE_2D,
                          .format = format,
                          .extent = {w, h, 1},
                          .mipLevels = mips,
                          .arrayLayers = layers,
                          .samples = VK_SAMPLE_COUNT_1_BIT,
                          .tiling = VK_IMAGE_TILING_OPTIMAL,
                          .usage = usage,
                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  if (layers == 6)
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  vkCreateImage(ctx->device, &ci, NULL, img);
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(ctx->device, *img, &req);
  VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  vkAllocateMemory(ctx->device, &ai, NULL, mem);
  vkBindImageMemory(ctx->device, *img, *mem, 0);
}

static VkImageView create_ibl_view(VulkanContext *ctx, VkImage img,
                                   VkFormat format, VkImageViewType type,
                                   uint32_t baseMip, uint32_t mipCount,
                                   uint32_t layerCount) {
  VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = img,
                              .viewType = type,
                              .format = format,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                   baseMip, mipCount, 0,
                                                   layerCount}};
  VkImageView view;
  vkCreateImageView(ctx->device, &ci, NULL, &view);
  return view;
}

static void transition_image_compute(VkCommandBuffer cmd, VkImage image,
                                     VkImageLayout oldLayout,
                                     VkImageLayout newLayout, uint32_t baseMip,
                                     uint32_t mipCount, uint32_t layerCount) {
  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = oldLayout,
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0,
                           layerCount}};

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    barrier.srcAccessMask = 0;
  else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL)
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;

  if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  else if (newLayout == VK_IMAGE_LAYOUT_GENERAL)
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                       1, &barrier);
}

static void write_ibl_descriptor_set(VkDevice device, VkSampler tempSampler,
                                     VkDescriptorSet set, VkImageView sampView,
                                     VkImageView storView) {
  VkDescriptorImageInfo dSamp = {.sampler = tempSampler,
                                 .imageView = sampView,
                                 .imageLayout =
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo dStor = {.imageView = storView,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = set,
       .dstBinding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .pImageInfo = &dSamp},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = set,
       .dstBinding = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
       .descriptorCount = 1,
       .pImageInfo = &dStor}};
  vkUpdateDescriptorSets(device, sampView ? 2 : 1, sampView ? w : &w[1], 0,
                         NULL);
}

static const char *get_ibl_cache_path(const char *original_path) {
  static char cache_path[512];
  const char *home = getenv("HOME");
  if (!home)
    home = ".";
  char dir_path[512];
  snprintf(dir_path, sizeof(dir_path), "%s/.cache", home);
  mkdir(dir_path, 0777);
  snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home);
  mkdir(dir_path, 0777);
  snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/ibl", home);
  mkdir(dir_path, 0777);

  char safe_name[256];
  strncpy(safe_name, original_path, sizeof(safe_name) - 1);
  safe_name[sizeof(safe_name) - 1] = '\0';
  for (int i = 0; safe_name[i]; i++) {
    if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.')
      safe_name[i] = '_';
  }
  snprintf(cache_path, sizeof(cache_path), "%s/%s.oibl", dir_path, safe_name);
  return cache_path;
}

static bool ibl_half_storage_supported(VulkanContext *ctx) {
  if (!ctx->shaderStorageImageExtendedFormatsEnabled)
    return false;
  VkFormatProperties properties = {0};
  vkGetPhysicalDeviceFormatProperties(
      ctx->physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return (properties.optimalTilingFeatures & required) == required;
}

bool loadIBL(VulkanContext *ctx, const char *hdr_path) {
  const char *cache_path = get_ibl_cache_path(hdr_path);
  const bool use_half_storage = ibl_half_storage_supported(ctx);
  const VkFormat hdr_format = VK_FORMAT_R32G32B32A32_SFLOAT;
  const VkFormat ibl_format =
      use_half_storage ? VK_FORMAT_R16G16B16A16_SFLOAT : hdr_format;
  const size_t ibl_bytes_per_pixel = use_half_storage ? 8 : 16;
  const uint32_t cache_magic =
      use_half_storage ? UINT32_C(0x3242494F) : UINT32_C(0x4C42494F);
  VkFormat brdfFmt = VK_FORMAT_R16G16_SFLOAT;
  fprintf(stdout, "[IBL] Storage output format: %s\n",
          use_half_storage ? "RGBA16F" : "RGBA32F");

  size_t skyboxSize = (size_t)1024 * 1024 * ibl_bytes_per_pixel * 6;
  size_t irradSize = (size_t)32 * 32 * ibl_bytes_per_pixel * 6;
  size_t prefSize = 0;
  uint32_t pw = 128, ph = 128;
  VkBufferImageCopy prefRegions[5];
  for (int i = 0; i < 5; i++) {
    prefRegions[i] = (VkBufferImageCopy){
        .bufferOffset = skyboxSize + irradSize + prefSize,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 6},
        .imageExtent = {pw, ph, 1}};
    prefSize += (size_t)pw * ph * ibl_bytes_per_pixel * 6;
    pw /= 2;
    ph /= 2;
  }
  size_t brdfSize = 512 * 512 * 4;
  size_t totalSize = skyboxSize + irradSize + prefSize + brdfSize;

  FILE *f = fopen(cache_path, "rb");
  uint32_t magic = 0;
  if (f && fread(&magic, sizeof(magic), 1, f) == 1 &&
      magic == cache_magic) {
    fprintf(stdout,
            "\033[32m[IBL] Cache Hit (ZERO-COPY Environment Map): %s\033[0m\n",
            cache_path);

    destroyIBL(ctx);

    create_ibl_image(ctx, 1024, 1024, 1, 6, ibl_format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     &iblSkyboxImage, &iblSkyboxMemory);
    iblSkyboxView = create_ibl_view(ctx, iblSkyboxImage, ibl_format,
                                    VK_IMAGE_VIEW_TYPE_CUBE, 0, 1, 6);

    create_ibl_image(ctx, 32, 32, 1, 6, ibl_format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     &ctx->iblIrradianceImage, &ctx->iblIrradianceMemory);
    ctx->iblIrradianceView = create_ibl_view(
        ctx, ctx->iblIrradianceImage, ibl_format, VK_IMAGE_VIEW_TYPE_CUBE, 0, 1,
        6);

    create_ibl_image(ctx, 128, 128, 5, 6, ibl_format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     &ctx->iblPrefilterImage, &ctx->iblPrefilterMemory);
    ctx->iblPrefilterView = create_ibl_view(ctx, ctx->iblPrefilterImage,
                                            ibl_format,
                                            VK_IMAGE_VIEW_TYPE_CUBE, 0, 5, 6);

    create_ibl_image(ctx, 512, 512, 1, 1, brdfFmt,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     &ctx->iblBrdfLutImage, &ctx->iblBrdfLutMemory);
    ctx->iblBrdfLutView = create_ibl_view(ctx, ctx->iblBrdfLutImage, brdfFmt,
                                          VK_IMAGE_VIEW_TYPE_2D, 0, 1, 1);

    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);
    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, totalSize, 0, &mapped);
    fread(mapped, 1, totalSize, f);
    vkUnmapMemory(ctx->device, stagingMem);
    fclose(f);

    VkCommandBuffer cmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);

    transition_image_compute(cmd, iblSkyboxImage, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 6);
    transition_image_compute(cmd, ctx->iblIrradianceImage,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 6);
    transition_image_compute(cmd, ctx->iblPrefilterImage,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 5, 6);
    transition_image_compute(cmd, ctx->iblBrdfLutImage,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);

    VkBufferImageCopy skyboxRegion = {
        .bufferOffset = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6},
        .imageExtent = {1024, 1024, 1}};
    vkCmdCopyBufferToImage(cmd, stagingBuf, iblSkyboxImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &skyboxRegion);

    VkBufferImageCopy irradRegion = {
        .bufferOffset = skyboxSize,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6},
        .imageExtent = {32, 32, 1}};
    vkCmdCopyBufferToImage(cmd, stagingBuf, ctx->iblIrradianceImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &irradRegion);

    vkCmdCopyBufferToImage(cmd, stagingBuf, ctx->iblPrefilterImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 5,
                           prefRegions);

    VkBufferImageCopy brdfRegion = {
        .bufferOffset = skyboxSize + irradSize + prefSize,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {512, 512, 1}};
    vkCmdCopyBufferToImage(cmd, stagingBuf, ctx->iblBrdfLutImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &brdfRegion);

    transition_image_compute(cmd, iblSkyboxImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);
    transition_image_compute(cmd, ctx->iblIrradianceImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);
    transition_image_compute(cmd, ctx->iblPrefilterImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 5, 6);
    transition_image_compute(cmd, ctx->iblBrdfLutImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          cmd);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);

    goto bind_samplers;
  }
  if (f)
    fclose(f);

  fprintf(stdout,
          "\033[33m[IBL] Cache Miss. Baking HDR environment map (Compute "
          "Shaders): %s\033[0m\n",
          hdr_path);

  int w, h, channels;
  float *pixels = stbi_loadf(hdr_path, &w, &h, &channels, 4);
  if (!pixels) {
    fprintf(stderr, "Failed to load HDR: %s\n", hdr_path);
    return false;
  }

  // 1. Source HDR Image
  VkImage hdrImg;
  VkDeviceMemory hdrMem;
  create_ibl_image(ctx, w, h, 1, 1, hdr_format,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &hdrImg, &hdrMem);
  VkImageView hdrView =
      create_ibl_view(ctx, hdrImg, hdr_format, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 1);

  VkDeviceSize size = w * h * 4 * sizeof(float);
  VkBuffer stgBuf;
  VkDeviceMemory stgMem;
  createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               &stgBuf, &stgMem);
  void *mapped;
  vkMapMemory(ctx->device, stgMem, 0, size, 0, &mapped);
  memcpy(mapped, pixels, size);
  vkUnmapMemory(ctx->device, stgMem);
  stbi_image_free(pixels);

  // 2. Temp Environment Cubemap
  VkImage envImg;
  VkDeviceMemory envMem;
  create_ibl_image(ctx, 1024, 1024, 1, 6, ibl_format,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &envImg, &envMem);
  VkImageView envCubeView =
      create_ibl_view(ctx, envImg, ibl_format, VK_IMAGE_VIEW_TYPE_CUBE, 0, 1,
                      6);
  VkImageView envArrayView =
      create_ibl_view(ctx, envImg, ibl_format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0,
                      1, 6);

  // 3. Final IBL Images
  destroyIBL(ctx);
  create_ibl_image(ctx, 32, 32, 1, 6, ibl_format,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &ctx->iblIrradianceImage, &ctx->iblIrradianceMemory);
  ctx->iblIrradianceView = create_ibl_view(ctx, ctx->iblIrradianceImage,
                                           ibl_format,
                                           VK_IMAGE_VIEW_TYPE_CUBE, 0, 1, 6);
  VkImageView irradArrayView =
      create_ibl_view(ctx, ctx->iblIrradianceImage, ibl_format,
                      VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, 6);

  create_ibl_image(ctx, 128, 128, 5, 6, ibl_format,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &ctx->iblPrefilterImage, &ctx->iblPrefilterMemory);
  ctx->iblPrefilterView = create_ibl_view(ctx, ctx->iblPrefilterImage,
                                          ibl_format,
                                          VK_IMAGE_VIEW_TYPE_CUBE, 0, 5, 6);
  VkImageView prefArrayViews[5];
  for (uint32_t i = 0; i < 5; i++)
    prefArrayViews[i] = create_ibl_view(ctx, ctx->iblPrefilterImage, ibl_format,
                                        VK_IMAGE_VIEW_TYPE_2D_ARRAY, i, 1, 6);

  create_ibl_image(ctx, 512, 512, 1, 1, VK_FORMAT_R16G16_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &ctx->iblBrdfLutImage, &ctx->iblBrdfLutMemory);
  ctx->iblBrdfLutView =
      create_ibl_view(ctx, ctx->iblBrdfLutImage, VK_FORMAT_R16G16_SFLOAT,
                      VK_IMAGE_VIEW_TYPE_2D, 0, 1, 1);

  // Temp Sampler
  VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxAnisotropy = 1.0f};
  VkSampler tempSampler;
  vkCreateSampler(ctx->device, &sci, NULL, &tempSampler);

  // Descriptor Layouts & Pipelines
  VkDescriptorSetLayoutBinding bGen[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       NULL}};
  VkDescriptorSetLayoutBinding bLut[] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          1, VK_SHADER_STAGE_COMPUTE_BIT,
                                          NULL}};

  VkDescriptorSetLayoutCreateInfo lciGen = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bGen};
  VkDescriptorSetLayoutCreateInfo lciLut = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = bLut};
  VkDescriptorSetLayout layoutGen, layoutLut;
  vkCreateDescriptorSetLayout(ctx->device, &lciGen, NULL, &layoutGen);
  vkCreateDescriptorSetLayout(ctx->device, &lciLut, NULL, &layoutLut);

  VkPushConstantRange pcRange = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                 .offset = 0,
                                 .size = sizeof(float)};
  VkPipelineLayoutCreateInfo plciGen = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &layoutGen,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcRange};
  VkPipelineLayoutCreateInfo plciLut = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &layoutLut};
  VkPipelineLayout pipeLayoutGen, pipeLayoutLut;
  vkCreatePipelineLayout(ctx->device, &plciGen, NULL, &pipeLayoutGen);
  vkCreatePipelineLayout(ctx->device, &plciLut, NULL, &pipeLayoutLut);

  const uint8_t *eq_code = use_half_storage ? equirect2cube_fp16_comp_spv
                                             : equirect2cube_comp_spv;
  const size_t eq_code_size =
      use_half_storage ? sizeof(equirect2cube_fp16_comp_spv)
                       : sizeof(equirect2cube_comp_spv);
  const uint8_t *irr_code =
      use_half_storage ? irradiance_fp16_comp_spv : irradiance_comp_spv;
  const size_t irr_code_size = use_half_storage
                                   ? sizeof(irradiance_fp16_comp_spv)
                                   : sizeof(irradiance_comp_spv);
  const uint8_t *pref_code =
      use_half_storage ? prefilter_fp16_comp_spv : prefilter_comp_spv;
  const size_t pref_code_size = use_half_storage
                                    ? sizeof(prefilter_fp16_comp_spv)
                                    : sizeof(prefilter_comp_spv);
  VkShaderModule modEq =
      createShaderModule(ctx->device, eq_code, eq_code_size);
  VkShaderModule modIrr =
      createShaderModule(ctx->device, irr_code, irr_code_size);
  VkShaderModule modPref =
      createShaderModule(ctx->device, pref_code, pref_code_size);
  VkShaderModule modBrdf = createShaderModule(ctx->device, brdf_lut_comp_spv,
                                              sizeof(brdf_lut_comp_spv));

  VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .pName = "main"}};
  VkPipeline pipeEq, pipeIrr, pipePref, pipeBrdf;
  cpci.layout = pipeLayoutGen;
  cpci.stage.module = modEq;
  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                           &pipeEq);
  cpci.layout = pipeLayoutGen;
  cpci.stage.module = modIrr;
  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                           &pipeIrr);
  cpci.layout = pipeLayoutGen;
  cpci.stage.module = modPref;
  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                           &pipePref);
  cpci.layout = pipeLayoutLut;
  cpci.stage.module = modBrdf;
  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                           &pipeBrdf);

  // Descriptor Sets
  VkDescriptorPoolSize pSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10}};
  VkDescriptorPoolCreateInfo poolCI = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 10,
      .poolSizeCount = 2,
      .pPoolSizes = pSizes};
  VkDescriptorPool tempPool;
  vkCreateDescriptorPool(ctx->device, &poolCI, NULL, &tempPool);

  VkDescriptorSetLayout layouts[] = {layoutGen, layoutGen, layoutGen,
                                     layoutGen, layoutGen, layoutGen,
                                     layoutGen, layoutLut};
  VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = tempPool,
      .descriptorSetCount = 8,
      .pSetLayouts = layouts};
  VkDescriptorSet sets[8];
  vkAllocateDescriptorSets(ctx->device, &dsai, sets);

  write_ibl_descriptor_set(ctx->device, tempSampler, sets[0], hdrView,
                           envArrayView);
  write_ibl_descriptor_set(ctx->device, tempSampler, sets[1], envCubeView,
                           irradArrayView);
  for (uint32_t i = 0; i < 5; i++)
    write_ibl_descriptor_set(ctx->device, tempSampler, sets[2 + i], envCubeView,
                             prefArrayViews[i]);

  VkDescriptorImageInfo dLut = {.imageView = ctx->iblBrdfLutView,
                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet wLut = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = sets[7],
                               .dstBinding = 0,
                               .descriptorType =
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               .descriptorCount = 1,
                               .pImageInfo = &dLut};
  vkUpdateDescriptorSets(ctx->device, 1, &wLut, 0, NULL);

  // Execute Compute
  VkCommandBuffer cmd = beginSingleTimeCommands(ctx->device, ctx->commandPool);

  transition_image_compute(cmd, hdrImg, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);
  VkBufferImageCopy region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
  vkCmdCopyBufferToImage(cmd, stgBuf, hdrImg,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  transition_image_compute(cmd, hdrImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

  transition_image_compute(cmd, envImg, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL, 0, 1, 6);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEq);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutGen, 0,
                          1, &sets[0], 0, NULL);
  vkCmdDispatch(cmd, 1024 / 8, 1024 / 8, 6);

  transition_image_compute(cmd, envImg, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);
  transition_image_compute(cmd, ctx->iblIrradianceImage,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                           0, 1, 6);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeIrr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutGen, 0,
                          1, &sets[1], 0, NULL);
  vkCmdDispatch(cmd, 32 / 8, 32 / 8, 6);
  transition_image_compute(cmd, ctx->iblIrradianceImage,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipePref);
  for (uint32_t i = 0; i < 5; i++) {
    uint32_t mipSize = 128 >> i;
    transition_image_compute(cmd, ctx->iblPrefilterImage,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             i, 1, 6);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutGen,
                            0, 1, &sets[2 + i], 0, NULL);
    float roughness = (float)i / 4.0f;
    vkCmdPushConstants(cmd, pipeLayoutGen, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(float), &roughness);
    vkCmdDispatch(cmd, mipSize / 8, mipSize / 8, 6);
    transition_image_compute(cmd, ctx->iblPrefilterImage,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, i, 1, 6);
  }

  transition_image_compute(cmd, ctx->iblBrdfLutImage, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeBrdf);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutLut, 0,
                          1, &sets[7], 0, NULL);
  vkCmdDispatch(cmd, 512 / 8, 512 / 8, 1);
  transition_image_compute(cmd, ctx->iblBrdfLutImage, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

  endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue, cmd);

  FILE *fout = fopen(cache_path, "wb");
  if (fout) {
    uint32_t magic_write = cache_magic;
    fwrite(&magic_write, 4, 1, fout);

    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(ctx, totalSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &stagingBuf, &stagingMem);

    VkCommandBuffer copyCmd =
        beginSingleTimeCommands(ctx->device, ctx->commandPool);

    transition_image_compute(copyCmd, envImg,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 6);
    transition_image_compute(copyCmd, ctx->iblIrradianceImage,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 6);
    transition_image_compute(copyCmd, ctx->iblPrefilterImage,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 5, 6);
    transition_image_compute(copyCmd, ctx->iblBrdfLutImage,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 1);

    VkBufferImageCopy skyboxRegion = {
        .bufferOffset = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6},
        .imageExtent = {1024, 1024, 1}};
    vkCmdCopyImageToBuffer(copyCmd, envImg,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1,
                           &skyboxRegion);

    VkBufferImageCopy irradRegion = {
        .bufferOffset = skyboxSize,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6},
        .imageExtent = {32, 32, 1}};
    vkCmdCopyImageToBuffer(copyCmd, ctx->iblIrradianceImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1,
                           &irradRegion);

    vkCmdCopyImageToBuffer(copyCmd, ctx->iblPrefilterImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 5,
                           prefRegions);

    VkBufferImageCopy brdfRegion = {
        .bufferOffset = skyboxSize + irradSize + prefSize,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {512, 512, 1}};
    vkCmdCopyImageToBuffer(copyCmd, ctx->iblBrdfLutImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1,
                           &brdfRegion);

    transition_image_compute(copyCmd, envImg,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);
    transition_image_compute(copyCmd, ctx->iblIrradianceImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);
    transition_image_compute(copyCmd, ctx->iblPrefilterImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 5, 6);
    transition_image_compute(copyCmd, ctx->iblBrdfLutImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                          copyCmd);

    void *mapped;
    vkMapMemory(ctx->device, stagingMem, 0, totalSize, 0, &mapped);
    fwrite(mapped, 1, totalSize, fout);
    vkUnmapMemory(ctx->device, stagingMem);
    fclose(fout);
    vkDestroyBuffer(ctx->device, stagingBuf, NULL);
    vkFreeMemory(ctx->device, stagingMem, NULL);
  }

  // Persist the Skybox
  iblSkyboxImage = envImg;
  iblSkyboxMemory = envMem;
  iblSkyboxView = envCubeView;

  // Cleanup Temp Resources
  vkDestroyBuffer(ctx->device, stgBuf, NULL);
  vkFreeMemory(ctx->device, stgMem, NULL);
  vkDestroyImageView(ctx->device, hdrView, NULL);
  vkDestroyImage(ctx->device, hdrImg, NULL);
  vkFreeMemory(ctx->device, hdrMem, NULL);
  vkDestroyImageView(ctx->device, envArrayView, NULL);
  for (uint32_t i = 0; i < 5; i++)
    vkDestroyImageView(ctx->device, prefArrayViews[i], NULL);
  vkDestroyImageView(ctx->device, irradArrayView, NULL);
  vkDestroySampler(ctx->device, tempSampler, NULL);
  vkDestroyPipeline(ctx->device, pipeEq, NULL);
  vkDestroyPipeline(ctx->device, pipeIrr, NULL);
  vkDestroyPipeline(ctx->device, pipePref, NULL);
  vkDestroyPipeline(ctx->device, pipeBrdf, NULL);
  vkDestroyPipelineLayout(ctx->device, pipeLayoutGen, NULL);
  vkDestroyPipelineLayout(ctx->device, pipeLayoutLut, NULL);
  vkDestroyDescriptorSetLayout(ctx->device, layoutGen, NULL);
  vkDestroyDescriptorSetLayout(ctx->device, layoutLut, NULL);
  vkDestroyDescriptorPool(ctx->device, tempPool, NULL);
  vkDestroyShaderModule(ctx->device, modEq, NULL);
  vkDestroyShaderModule(ctx->device, modIrr, NULL);
  vkDestroyShaderModule(ctx->device, modPref, NULL);
  vkDestroyShaderModule(ctx->device, modBrdf, NULL);

bind_samplers:
  // Create Permanent Samplers
  VkSamplerCreateInfo finalSci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxAnisotropy = 1.0f};
  finalSci.maxLod = 1.0f;
  vkCreateSampler(ctx->device, &finalSci, NULL, &ctx->iblIrradianceSampler);
  finalSci.maxLod = 5.0f;
  vkCreateSampler(ctx->device, &finalSci, NULL, &ctx->iblPrefilterSampler);
  finalSci.maxLod = 1.0f;
  vkCreateSampler(ctx->device, &finalSci, NULL, &ctx->iblBrdfLutSampler);

  // Inject directly into lighting sets (set = 3) for ALL frames!
  VkDescriptorImageInfo iInfos[4] = {
      {.sampler = ctx->iblIrradianceSampler,
       .imageView = ctx->iblIrradianceView,
       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {.sampler = ctx->iblPrefilterSampler,
       .imageView = ctx->iblPrefilterView,
       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {.sampler = ctx->iblBrdfLutSampler,
       .imageView = ctx->iblBrdfLutView,
       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {.sampler = ctx->iblPrefilterSampler,
       .imageView = iblSkyboxView,
       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
    VkWriteDescriptorSet writes[4];
    for (int i = 0; i < 4; i++) {
      writes[i] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = ctx->lightingSets[f],
          .dstBinding = i + 1,
          .dstArrayElement = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .pImageInfo = &iInfos[i]};
    }
    vkUpdateDescriptorSets(ctx->device, 4, writes, 0, NULL);
  }

  ctx->iblLoaded = true;
  printf("IBL dynamically baked and loaded successfully from: %s\n", hdr_path);
  return true;
}

void destroyIBL(VulkanContext *ctx) {
  if (iblSkyboxView) {
    vkDestroyImageView(ctx->device, iblSkyboxView, NULL);
    iblSkyboxView = VK_NULL_HANDLE;
  }
  if (iblSkyboxImage) {
    vkDestroyImage(ctx->device, iblSkyboxImage, NULL);
    iblSkyboxImage = VK_NULL_HANDLE;
  }
  if (iblSkyboxMemory) {
    vkFreeMemory(ctx->device, iblSkyboxMemory, NULL);
    iblSkyboxMemory = VK_NULL_HANDLE;
  }
  if (ctx->iblIrradianceView) {
    vkDestroyImageView(ctx->device, ctx->iblIrradianceView, NULL);
    ctx->iblIrradianceView = VK_NULL_HANDLE;
  }
  if (ctx->iblIrradianceImage) {
    vkDestroyImage(ctx->device, ctx->iblIrradianceImage, NULL);
    ctx->iblIrradianceImage = VK_NULL_HANDLE;
  }
  if (ctx->iblIrradianceMemory) {
    vkFreeMemory(ctx->device, ctx->iblIrradianceMemory, NULL);
    ctx->iblIrradianceMemory = VK_NULL_HANDLE;
  }
  if (ctx->iblIrradianceSampler) {
    vkDestroySampler(ctx->device, ctx->iblIrradianceSampler, NULL);
    ctx->iblIrradianceSampler = VK_NULL_HANDLE;
  }

  if (ctx->iblPrefilterView) {
    vkDestroyImageView(ctx->device, ctx->iblPrefilterView, NULL);
    ctx->iblPrefilterView = VK_NULL_HANDLE;
  }
  if (ctx->iblPrefilterImage) {
    vkDestroyImage(ctx->device, ctx->iblPrefilterImage, NULL);
    ctx->iblPrefilterImage = VK_NULL_HANDLE;
  }
  if (ctx->iblPrefilterMemory) {
    vkFreeMemory(ctx->device, ctx->iblPrefilterMemory, NULL);
    ctx->iblPrefilterMemory = VK_NULL_HANDLE;
  }
  if (ctx->iblPrefilterSampler) {
    vkDestroySampler(ctx->device, ctx->iblPrefilterSampler, NULL);
    ctx->iblPrefilterSampler = VK_NULL_HANDLE;
  }

  if (ctx->iblBrdfLutView) {
    vkDestroyImageView(ctx->device, ctx->iblBrdfLutView, NULL);
    ctx->iblBrdfLutView = VK_NULL_HANDLE;
  }
  if (ctx->iblBrdfLutImage) {
    vkDestroyImage(ctx->device, ctx->iblBrdfLutImage, NULL);
    ctx->iblBrdfLutImage = VK_NULL_HANDLE;
  }
  if (ctx->iblBrdfLutMemory) {
    vkFreeMemory(ctx->device, ctx->iblBrdfLutMemory, NULL);
    ctx->iblBrdfLutMemory = VK_NULL_HANDLE;
  }
  if (ctx->iblBrdfLutSampler) {
    vkDestroySampler(ctx->device, ctx->iblBrdfLutSampler, NULL);
    ctx->iblBrdfLutSampler = VK_NULL_HANDLE;
  }

  ctx->iblLoaded = false;
}

float renderer_read_depth_at(VulkanContext *ctx, uint32_t x, uint32_t y) {
  if (x >= ctx->drawExtent.width || y >= ctx->drawExtent.height)
    return 1.0f;

  vkDeviceWaitIdle(ctx->device); // Sync to ensure the frame finishes rendering
  uint32_t rendered_frame =
      (ctx->currentFrame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
  VkImage depth_image = ctx->primary->depthImage[rendered_frame];

  VkBuffer stagingBuf;
  VkDeviceMemory stagingMem;
  createBuffer(ctx, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               &stagingBuf, &stagingMem);

  VkCommandBuffer cmd = beginSingleTimeCommands(ctx->device, ctx->commandPool);

  // Transition depth image to TRANSFER_SRC
  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = depth_image,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier);

  // Copy exactly 1 pixel
  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
      .imageOffset = {(int32_t)x, (int32_t)y, 0},
      .imageExtent = {1, 1, 1}};
  vkCmdCopyImageToBuffer(cmd, depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         stagingBuf, 1, &region);

  // Transition back to optimal depth stencil layout
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       0, 0, NULL, 0, NULL, 1, &barrier);

  endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue, cmd);

  float depth = 1.0f;
  void *mapped;
  vkMapMemory(ctx->device, stagingMem, 0, 4, 0, &mapped);
  memcpy(&depth, mapped, 4);
  vkUnmapMemory(ctx->device, stagingMem);

  vkDestroyBuffer(ctx->device, stagingBuf, NULL);
  vkFreeMemory(ctx->device, stagingMem, NULL);

  return depth;
}

void clear_background(Color color) { context.clearColor = color; }
void toggle_ambient_occlusion(void) {
  ambientOcclusionEnabled = !ambientOcclusionEnabled;
}

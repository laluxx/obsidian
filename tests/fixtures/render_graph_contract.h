/// Minimal native-only Vulkan ABI seam. Production uses the system Vulkan SDK.
#ifndef OBS_RENDER_GRAPH_CONTRACT_H
#define OBS_RENDER_GRAPH_CONTRACT_H
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t VkImage, VkImageView, VkBuffer, VkCommandBuffer;
typedef uint32_t VkFormat, VkImageLayout, VkPipelineStageFlags, VkAccessFlags;
typedef uint32_t VkImageAspectFlags, VkStructureType, VkAttachmentLoadOp;
typedef uint32_t VkAttachmentStoreOp, VkResolveMode, VkResolveModeFlagBits;
typedef uint32_t VkSampleCountFlagBits, VkDependencyFlags;
typedef uint64_t VkDeviceSize;
typedef struct {
  float r, g, b, a;
} Color;

#define VK_NULL_HANDLE 0
#define VK_FORMAT_UNDEFINED 0
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_FORMAT_D16_UNORM 124
#define VK_FORMAT_D32_SFLOAT 126
#define VK_FORMAT_D16_UNORM_S8_UINT 128
#define VK_FORMAT_D24_UNORM_S8_UINT 129
#define VK_FORMAT_D32_SFLOAT_S8_UINT 130
#define VK_IMAGE_LAYOUT_UNDEFINED 0
#define VK_IMAGE_LAYOUT_GENERAL 1
#define VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL 2
#define VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL 3
#define VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL 5
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT 0x00000001
#define VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT 0x00000002
#define VK_PIPELINE_STAGE_VERTEX_SHADER_BIT 0x00000008
#define VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT 0x00000080
#define VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT 0x00000100
#define VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT 0x00000200
#define VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT 0x00000400
#define VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT 0x00000800
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT 0x00002000
#define VK_ACCESS_INDIRECT_COMMAND_READ_BIT 0x00000001
#define VK_ACCESS_SHADER_READ_BIT 0x00000020
#define VK_ACCESS_SHADER_WRITE_BIT 0x00000040
#define VK_ACCESS_COLOR_ATTACHMENT_READ_BIT 0x00000080
#define VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT 0x00000100
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT 0x00000200
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT 0x00000400
#define VK_ACCESS_TRANSFER_WRITE_BIT 0x00001000
#define VK_ACCESS_HOST_WRITE_BIT 0x00004000
#define VK_ACCESS_MEMORY_WRITE_BIT 0x00010000
#define VK_IMAGE_ASPECT_COLOR_BIT 1
#define VK_IMAGE_ASPECT_DEPTH_BIT 2
#define VK_IMAGE_ASPECT_STENCIL_BIT 4
#define VK_QUEUE_FAMILY_IGNORED UINT32_MAX
#define VK_WHOLE_SIZE UINT64_MAX
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER 45
#define VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER 44
#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO 1000044001
#define VK_STRUCTURE_TYPE_RENDERING_INFO 1000044000
#define VK_RESOLVE_MODE_NONE 0
#define VK_RESOLVE_MODE_AVERAGE_BIT 1
#define VK_RESOLVE_MODE_MAX_BIT 4
#define VK_SAMPLE_COUNT_1_BIT 1
#define VK_SAMPLE_COUNT_2_BIT 2
#define VK_ATTACHMENT_LOAD_OP_LOAD 0
#define VK_ATTACHMENT_LOAD_OP_CLEAR 1
#define VK_ATTACHMENT_STORE_OP_STORE 0

typedef struct {
  uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount;
} VkImageSubresourceRange;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  VkAccessFlags srcAccessMask, dstAccessMask;
  VkImageLayout oldLayout, newLayout;
  uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
  VkImage image;
  VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  VkAccessFlags srcAccessMask, dstAccessMask;
  uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
  VkBuffer buffer;
  VkDeviceSize offset, size;
} VkBufferMemoryBarrier;
typedef union {
  float float32[4];
  int32_t int32[4];
  uint32_t uint32[4];
} VkClearColorValue;
typedef struct {
  float depth;
  uint32_t stencil;
} VkClearDepthStencilValue;
typedef union {
  VkClearColorValue color;
  VkClearDepthStencilValue depthStencil;
} VkClearValue;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  VkImageView imageView;
  VkImageLayout imageLayout;
  VkResolveMode resolveMode;
  VkImageView resolveImageView;
  VkImageLayout resolveImageLayout;
  VkAttachmentLoadOp loadOp;
  VkAttachmentStoreOp storeOp;
  VkClearValue clearValue;
} VkRenderingAttachmentInfo;
typedef struct {
  int32_t x, y;
} VkOffset2D;
typedef struct {
  uint32_t width, height;
} VkExtent2D;
typedef struct {
  VkOffset2D offset;
  VkExtent2D extent;
} VkRect2D;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  uint32_t flags;
  VkRect2D renderArea;
  uint32_t layerCount, viewMask;
  uint32_t colorAttachmentCount;
  const VkRenderingAttachmentInfo *pColorAttachments;
  const VkRenderingAttachmentInfo *pDepthAttachment;
  const VkRenderingAttachmentInfo *pStencilAttachment;
} VkRenderingInfo;

void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags,
                          VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                          const void *, uint32_t, const VkBufferMemoryBarrier *,
                          uint32_t, const VkImageMemoryBarrier *);
void vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo *);
void vkCmdEndRendering(VkCommandBuffer);
#endif

/// render_graph — validated, ordered Vulkan render graph
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef OBS_RG_CONTRACT_TEST
#include "render_graph_contract.h"
#else
#include "common.h"
#include <vulkan/vulkan.h>
#endif

/// Public interface

typedef uint32_t RgResId;
#define RG_INVALID_ID UINT32_MAX

typedef struct RgGraph RgGraph;
typedef struct RgPass RgPass;
typedef enum {
  RG_OK = 0,
  RG_ERR_CAPACITY,
  RG_ERR_RESOURCE,
  RG_ERR_TYPE,
  RG_ERR_DEPENDENCY,
  RG_ERR_ATTACHMENT,
  RG_ERR_DIMENSIONS,
  RG_ERR_CONFLICT,
  RG_ERR_COMMAND
} RgResult;

RgGraph *rg_create(void);
void rg_destroy(RgGraph *graph);
/* A frame-local graph: clear passes AND imports. Re-import Vulkan handles each
 * frame. */
void rg_reset(RgGraph *graph);
RgResult rg_status(const RgGraph *graph);
const char *rg_result_name(RgResult result);
/* Validates the whole graph BEFORE recording any Vulkan commands. */
RgResult rg_validate(RgGraph *graph);

RgResId rg_import_image(RgGraph *graph, const char *name, VkImage image,
                        VkImageView view, VkFormat format, uint32_t width,
                        uint32_t height, VkImageLayout initial_layout);
/* Acquired swapchain images use COLOR_ATTACHMENT_OUTPUT as the source stage
 * so their first layout transition is ordered with the acquire-semaphore wait. */
RgResId rg_import_acquired_image(RgGraph *graph, const char *name,
                                 VkImage image, VkImageView view,
                                 VkFormat format, uint32_t width,
                                 uint32_t height,
                                 VkImageLayout initial_layout);
RgResId rg_import_image_samples(RgGraph *graph, const char *name, VkImage image,
                                VkImageView view, VkFormat format,
                                uint32_t width, uint32_t height,
                                VkImageLayout initial_layout,
                                VkSampleCountFlagBits samples);
bool rg_update_image(RgGraph *graph, RgResId id, VkImage image,
                     VkImageView view, uint32_t width, uint32_t height);
RgResId rg_import_buffer(RgGraph *graph, const char *name, VkBuffer buffer);
bool rg_update_buffer(RgGraph *graph, RgResId id, VkBuffer buffer);

RgPass *rg_add_pass(RgGraph *graph, const char *name);
/* Checked pass admission. A false return stores a sticky error on the graph;
 * no partially appended dependency or attachment may be executed. */
bool rg_pass_read_image(RgPass *pass, RgResId id, VkPipelineStageFlags stage,
                        VkAccessFlags access, VkImageLayout layout);
bool rg_pass_write_image(RgPass *pass, RgResId id, VkPipelineStageFlags stage,
                         VkAccessFlags access, VkImageLayout layout);
bool rg_pass_read_buffer(RgPass *pass, RgResId id, VkPipelineStageFlags stage,
                         VkAccessFlags access);
bool rg_pass_write_buffer(RgPass *pass, RgResId id, VkPipelineStageFlags stage,
                          VkAccessFlags access);
bool rg_pass_color_attachment(RgPass *pass, RgResId id, bool clear,
                              Color clearColor);
bool rg_pass_color_attachment_resolve(RgPass *pass, RgResId id,
                                      RgResId resolve_id, bool clear,
                                      Color clear_color);
bool rg_pass_depth_attachment(RgPass *pass, RgResId id, bool clear);
bool rg_pass_depth_attachment_resolve(RgPass *pass, RgResId id,
                                      RgResId resolve_id,
                                      VkResolveModeFlagBits resolve_mode,
                                      bool clear);

typedef void (*RgExecuteFn)(VkCommandBuffer cmd, void *user_data);
void rg_pass_execute(RgPass *pass, RgExecuteFn fn, void *user_data);
/* On false, no Vulkan commands from THIS graph have been recorded. */
bool rg_execute_checked(RgGraph *graph, VkCommandBuffer cmd);
/* Kept for existing embedding clients. Production calls the checked form. */
void rg_execute(RgGraph *graph, VkCommandBuffer cmd);

#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include "common.h"

typedef uint32_t RgResId;
#define RG_INVALID_ID 0xFFFFFFFF

typedef struct RgGraph RgGraph;
typedef struct RgPass RgPass;

RgGraph* rg_create(void);
void rg_destroy(RgGraph* graph);
void rg_reset(RgGraph* graph); /* Clears passes, retains registered resources */

/* Resource Registration */
RgResId rg_import_image(RgGraph* graph, const char* name, VkImage image, VkImageView view, VkFormat format, uint32_t width, uint32_t height, VkImageLayout initial_layout);
void rg_update_image(RgGraph* graph, RgResId id, VkImage image, VkImageView view, uint32_t width, uint32_t height);

RgResId rg_import_buffer(RgGraph* graph, const char* name, VkBuffer buffer);
void rg_update_buffer(RgGraph* graph, RgResId id, VkBuffer buffer);

/* Pass Creation */
RgPass* rg_add_pass(RgGraph* graph, const char* name);

/* Dependencies (The Magic of the Graph) */
void rg_pass_read_image(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access, VkImageLayout layout);
void rg_pass_write_image(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access, VkImageLayout layout);

void rg_pass_read_buffer(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access);
void rg_pass_write_buffer(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access);

/* Dynamic Rendering Attachments */
void rg_pass_color_attachment(RgPass* pass, RgResId id, bool clear, Color clearColor);
void rg_pass_depth_attachment(RgPass* pass, RgResId id, bool clear);

/* Execution Callback */
typedef void (*RgExecuteFn)(VkCommandBuffer cmd, void* user_data);
void rg_pass_execute(RgPass* pass, RgExecuteFn fn, void* user_data);

/* Run the graph */
void rg_execute(RgGraph* graph, VkCommandBuffer cmd);

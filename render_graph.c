#include "render_graph.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RG_MAX_RESOURCES 128
#define RG_MAX_PASSES 32
#define RG_MAX_DEPS 16
#define RG_MAX_ATTACHMENTS 4

typedef enum { RG_RES_IMAGE, RG_RES_BUFFER } RgResType;

typedef struct {
    const char* name;
    RgResType type;

    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageLayout layout;

    VkBuffer buffer;

    VkPipelineStageFlags stage;
    VkAccessFlags access;
} RgResource;

typedef struct {
    RgResId res;
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout; // Only for images
} RgDependency;

struct RgPass {
    const char* name;
    RgGraph* graph;

    RgDependency deps[RG_MAX_DEPS];
    uint32_t dep_count;

    RgResId color_attachments[RG_MAX_ATTACHMENTS];
    bool color_clear[RG_MAX_ATTACHMENTS];
    Color color_clear_value[RG_MAX_ATTACHMENTS];
    uint32_t color_count;

    RgResId depth_attachment;
    bool depth_clear;
    bool has_depth;

    RgExecuteFn exec_fn;
    void* user_data;
};

struct RgGraph {
    RgResource resources[RG_MAX_RESOURCES];
    uint32_t res_count;

    RgPass passes[RG_MAX_PASSES];
    uint32_t pass_count;
};

RgGraph* rg_create(void) {
    RgGraph* g = calloc(1, sizeof(RgGraph));
    return g;
}

void rg_destroy(RgGraph* graph) {
    free(graph);
}

void rg_reset(RgGraph* graph) {
    graph->pass_count = 0;
    graph->res_count = 0;
}

RgResId rg_import_image(RgGraph* graph, const char* name, VkImage image, VkImageView view, VkFormat format, uint32_t width, uint32_t height, VkImageLayout initial_layout) {
    if (graph->res_count >= RG_MAX_RESOURCES) return RG_INVALID_ID;
    RgResId id = graph->res_count++;
    RgResource* r = &graph->resources[id];
    r->name = name;
    r->type = RG_RES_IMAGE;
    r->image = image;
    r->view = view;
    r->format = format;
    r->width = width;
    r->height = height;
    r->layout = initial_layout;
    r->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    r->access = 0;
    return id;
}

void rg_update_image(RgGraph* graph, RgResId id, VkImage image, VkImageView view, uint32_t width, uint32_t height) {
    if (id >= graph->res_count) return;
    RgResource* r = &graph->resources[id];
    r->image = image;
    r->view = view;
    r->width = width;
    r->height = height;
}

RgResId rg_import_buffer(RgGraph* graph, const char* name, VkBuffer buffer) {
    if (graph->res_count >= RG_MAX_RESOURCES) return RG_INVALID_ID;
    RgResId id = graph->res_count++;
    RgResource* r = &graph->resources[id];
    r->name = name;
    r->type = RG_RES_BUFFER;
    r->buffer = buffer;
    r->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    r->access = 0;
    return id;
}

void rg_update_buffer(RgGraph* graph, RgResId id, VkBuffer buffer) {
    if (id >= graph->res_count) return;
    graph->resources[id].buffer = buffer;
}

RgPass* rg_add_pass(RgGraph* graph, const char* name) {
    if (graph->pass_count >= RG_MAX_PASSES) return NULL;
    RgPass* p = &graph->passes[graph->pass_count++];
    memset(p, 0, sizeof(RgPass));
    p->name = name;
    p->graph = graph;
    p->depth_attachment = RG_INVALID_ID;
    for(int i=0; i<RG_MAX_ATTACHMENTS; i++) p->color_attachments[i] = RG_INVALID_ID;
    return p;
}

static void add_dep(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access, VkImageLayout layout) {
    if (pass->dep_count >= RG_MAX_DEPS) return;
    RgDependency* d = &pass->deps[pass->dep_count++];
    d->res = id;
    d->stage = stage;
    d->access = access;
    d->layout = layout;
}

void rg_pass_read_image(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access, VkImageLayout layout) { add_dep(pass, id, stage, access, layout); }
void rg_pass_write_image(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access, VkImageLayout layout) { add_dep(pass, id, stage, access, layout); }
void rg_pass_read_buffer(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access) { add_dep(pass, id, stage, access, VK_IMAGE_LAYOUT_UNDEFINED); }
void rg_pass_write_buffer(RgPass* pass, RgResId id, VkPipelineStageFlags stage, VkAccessFlags access) { add_dep(pass, id, stage, access, VK_IMAGE_LAYOUT_UNDEFINED); }

void rg_pass_color_attachment(RgPass* pass, RgResId id, bool clear, Color clearColor) {
    if (pass->color_count >= RG_MAX_ATTACHMENTS) return;
    pass->color_attachments[pass->color_count] = id;
    pass->color_clear[pass->color_count] = clear;
    pass->color_clear_value[pass->color_count] = clearColor;
    pass->color_count++;

    // Automatically tell the graph to transition this image to COLOR_ATTACHMENT_OPTIMAL
    rg_pass_write_image(pass, id, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void rg_pass_depth_attachment(RgPass* pass, RgResId id, bool clear) {
    pass->depth_attachment = id;
    pass->depth_clear = clear;
    pass->has_depth = true;

    // Automatically tell the graph to transition this image to DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    rg_pass_write_image(pass, id, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

void rg_pass_execute(RgPass* pass, RgExecuteFn fn, void* user_data) {
    pass->exec_fn = fn;
    pass->user_data = user_data;
}

void rg_execute(RgGraph* graph, VkCommandBuffer cmd) {
    for (uint32_t i = 0; i < graph->pass_count; i++) {
        RgPass* pass = &graph->passes[i];

        VkImageMemoryBarrier img_barriers[RG_MAX_DEPS];
        VkBufferMemoryBarrier buf_barriers[RG_MAX_DEPS];
        uint32_t img_b_count = 0;
        uint32_t buf_b_count = 0;

        VkPipelineStageFlags src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst_stage_mask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        // Resolve dependencies and generate barriers
        for (uint32_t j = 0; j < pass->dep_count; j++) {
            RgDependency* dep = &pass->deps[j];
            RgResource* res = &graph->resources[dep->res];

            if (res->type == RG_RES_IMAGE) {
                if (res->layout != dep->layout || res->access != dep->access) {
                    VkImageMemoryBarrier* b = &img_barriers[img_b_count++];
                    b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b->pNext = NULL;
                    b->oldLayout = res->layout;
                    b->newLayout = dep->layout;
                    b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b->image = res->image;
                    b->subresourceRange.aspectMask = (dep->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                    b->subresourceRange.baseMipLevel = 0;
                    b->subresourceRange.levelCount = 1;
                    b->subresourceRange.baseArrayLayer = 0;
                    b->subresourceRange.layerCount = 1;
                    b->srcAccessMask = res->access;
                    b->dstAccessMask = dep->access;

                    src_stage_mask |= res->stage;
                    dst_stage_mask |= dep->stage;

                    // Update tracked state
                    res->layout = dep->layout;
                    res->access = dep->access;
                    res->stage = dep->stage;
                }
            } else if (res->type == RG_RES_BUFFER) {
                if (res->access != dep->access || (dep->access & VK_ACCESS_SHADER_WRITE_BIT)) {
                    VkBufferMemoryBarrier* b = &buf_barriers[buf_b_count++];
                    b->sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    b->pNext = NULL;
                    b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b->buffer = res->buffer;
                    b->offset = 0;
                    b->size = VK_WHOLE_SIZE;
                    b->srcAccessMask = res->access;
                    b->dstAccessMask = dep->access;

                    src_stage_mask |= res->stage;
                    dst_stage_mask |= dep->stage;

                    res->access = dep->access;
                    res->stage = dep->stage;
                }
            }
        }

        if (img_b_count > 0 || buf_b_count > 0) {
            vkCmdPipelineBarrier(cmd, src_stage_mask, dst_stage_mask, 0, 0, NULL, buf_b_count, buf_barriers, img_b_count, img_barriers);
        }

        // Begin Dynamic Rendering
        bool use_dynamic_rendering = (pass->color_count > 0 || pass->has_depth);
        if (use_dynamic_rendering) {
            VkRenderingAttachmentInfo colors[RG_MAX_ATTACHMENTS];
            uint32_t width = 0, height = 0;

            for (uint32_t c = 0; c < pass->color_count; c++) {
                RgResource* col_res = &graph->resources[pass->color_attachments[c]];
                width = col_res->width;
                height = col_res->height;

                colors[c].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colors[c].pNext = NULL;
                colors[c].imageView = col_res->view;
                colors[c].imageLayout = col_res->layout;
                colors[c].resolveMode = VK_RESOLVE_MODE_NONE;
                colors[c].resolveImageView = VK_NULL_HANDLE;
                colors[c].resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                colors[c].loadOp = pass->color_clear[c] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                colors[c].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colors[c].clearValue.color = (VkClearColorValue){{ pass->color_clear_value[c].r, pass->color_clear_value[c].g, pass->color_clear_value[c].b, pass->color_clear_value[c].a }};
            }

            VkRenderingAttachmentInfo depth;
            if (pass->has_depth) {
                RgResource* dep_res = &graph->resources[pass->depth_attachment];
                if (width == 0) { width = dep_res->width; height = dep_res->height; }

                depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth.pNext = NULL;
                depth.imageView = dep_res->view;
                depth.imageLayout = dep_res->layout;
                depth.resolveMode = VK_RESOLVE_MODE_NONE;
                depth.resolveImageView = VK_NULL_HANDLE;
                depth.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                depth.loadOp = pass->depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depth.clearValue.depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
            }

            VkRenderingInfo render_info = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext = NULL,
                .flags = 0,
                .renderArea = { {0, 0}, {width, height} },
                .layerCount = 1,
                .viewMask = 0,
                .colorAttachmentCount = pass->color_count,
                .pColorAttachments = pass->color_count > 0 ? colors : NULL,
                .pDepthAttachment = pass->has_depth ? &depth : NULL,
                .pStencilAttachment = NULL
            };

            vkCmdBeginRendering(cmd, &render_info);
        }

        // Execute pass logic
        if (pass->exec_fn) {
            pass->exec_fn(cmd, pass->user_data);
        }

        if (use_dynamic_rendering) {
            vkCmdEndRendering(cmd);
        }
    }
}

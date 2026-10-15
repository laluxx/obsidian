/// render_graph — validated command admission and resource transitions
#include "render_graph.h"
#include <stdlib.h>
#include <string.h>

/// Implementation
#define RG_MAX_RESOURCES 128u
#define RG_MAX_PASSES 32u
#define RG_MAX_DEPS 16u
#define RG_MAX_ATTACHMENTS 4u

typedef enum { RG_RES_IMAGE, RG_RES_BUFFER } RgResType;

typedef struct {
  const char *name;
  RgResType type;
  VkImage image;
  VkImageView view;
  VkFormat format;
  uint32_t width, height;
  VkImageLayout layout;
  VkSampleCountFlagBits samples;
  VkBuffer buffer;
  VkPipelineStageFlags stage;
  VkAccessFlags access;
} RgResource;

typedef struct {
  RgResId res;
  RgResType type;
  VkPipelineStageFlags stage;
  VkAccessFlags access;
  VkImageLayout layout;
} RgDependency;

struct RgPass {
  const char *name;
  RgGraph *graph;
  RgDependency deps[RG_MAX_DEPS];
  uint32_t dep_count;
  RgResId color_attachments[RG_MAX_ATTACHMENTS];
  bool color_clear[RG_MAX_ATTACHMENTS];
  Color color_clear_value[RG_MAX_ATTACHMENTS];
  RgResId color_resolves[RG_MAX_ATTACHMENTS];
  uint32_t color_count;
  RgResId depth_attachment;
  RgResId depth_resolve;
  VkResolveModeFlagBits depth_resolve_mode;
  bool depth_clear, has_depth;
  RgExecuteFn exec_fn;
  void *user_data;
};

struct RgGraph {
  RgResource resources[RG_MAX_RESOURCES];
  uint32_t res_count;
  RgPass passes[RG_MAX_PASSES];
  uint32_t pass_count;
  RgResult error;
};

static bool fail(RgGraph *g, RgResult error) {
  if (g && g->error == RG_OK)
    g->error = error;
  return false;
}
static bool valid_resource(RgGraph *g, RgResId id, RgResType type) {
  if (!g || id == RG_INVALID_ID || id >= g->res_count)
    return fail(g, RG_ERR_RESOURCE);
  if (g->resources[id].type != type)
    return fail(g, RG_ERR_TYPE);
  return true;
}
static bool depth_format(VkFormat fmt) {
  return fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT ||
         fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
         fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
         fmt == VK_FORMAT_D16_UNORM_S8_UINT;
}
static VkImageAspectFlags aspects(VkFormat fmt) {
  if (fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
      fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D16_UNORM_S8_UINT)
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  return depth_format(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT
                           : VK_IMAGE_ASPECT_COLOR_BIT;
}

RgGraph *rg_create(void) { return calloc(1, sizeof(RgGraph)); }
void rg_destroy(RgGraph *g) { free(g); }
void rg_reset(RgGraph *g) {
  if (!g)
    return;
  g->pass_count = 0;
  g->res_count = 0;
  g->error = RG_OK;
}
RgResult rg_status(const RgGraph *g) { return g ? g->error : RG_ERR_RESOURCE; }
const char *rg_result_name(RgResult result) {
  switch (result) {
  case RG_OK:
    return "ok";
  case RG_ERR_CAPACITY:
    return "capacity";
  case RG_ERR_RESOURCE:
    return "invalid-resource";
  case RG_ERR_TYPE:
    return "resource-type";
  case RG_ERR_DEPENDENCY:
    return "invalid-dependency";
  case RG_ERR_ATTACHMENT:
    return "invalid-attachment";
  case RG_ERR_DIMENSIONS:
    return "attachment-dimensions";
  case RG_ERR_CONFLICT:
    return "conflicting-dependency";
  case RG_ERR_COMMAND:
    return "invalid-command-buffer";
  }
  return "unknown";
}
RgResId rg_import_image(RgGraph *g, const char *name, VkImage image,
                        VkImageView view, VkFormat format, uint32_t width,
                        uint32_t height, VkImageLayout initial_layout) {
  return rg_import_image_samples(g, name, image, view, format, width, height,
                                 initial_layout, VK_SAMPLE_COUNT_1_BIT);
}
RgResId rg_import_acquired_image(RgGraph *g, const char *name, VkImage image,
                                 VkImageView view, VkFormat format,
                                 uint32_t width, uint32_t height,
                                 VkImageLayout initial_layout) {
  RgResId id = rg_import_image(g, name, image, view, format, width, height,
                               initial_layout);
  if (id != RG_INVALID_ID)
    g->resources[id].stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  return id;
}
RgResId rg_import_image_samples(RgGraph *g, const char *name, VkImage image,
                                VkImageView view, VkFormat format,
                                uint32_t width, uint32_t height,
                                VkImageLayout initial_layout,
                                VkSampleCountFlagBits samples) {
  if (!g)
    return RG_INVALID_ID;
  if (g->error != RG_OK)
    return RG_INVALID_ID;
  if (g->res_count >= RG_MAX_RESOURCES) {
    fail(g, RG_ERR_CAPACITY);
    return RG_INVALID_ID;
  }
  if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE || !width || !height ||
      format == VK_FORMAT_UNDEFINED || !samples ||
      (samples & (samples - 1)) != 0) {
    fail(g, RG_ERR_RESOURCE);
    return RG_INVALID_ID;
  }
  RgResId id = g->res_count++;
  g->resources[id] = (RgResource){.name = name,
                                  .type = RG_RES_IMAGE,
                                  .image = image,
                                  .view = view,
                                  .format = format,
                                  .width = width,
                                  .height = height,
                                  .layout = initial_layout,
                                  .samples = samples,
                                  .stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
  return id;
}
bool rg_update_image(RgGraph *g, RgResId id, VkImage image, VkImageView view,
                     uint32_t width, uint32_t height) {
  if (!valid_resource(g, id, RG_RES_IMAGE))
    return false;
  if (!image || !view || !width || !height)
    return fail(g, RG_ERR_RESOURCE);
  RgResource *r = &g->resources[id];
  /* Changing backing image invalidates the old image's layout/access tracking.
   */
  if (r->image != image) {
    r->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    r->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    r->access = 0;
  }
  r->image = image;
  r->view = view;
  r->width = width;
  r->height = height;
  return true;
}
RgResId rg_import_buffer(RgGraph *g, const char *name, VkBuffer buffer) {
  if (!g)
    return RG_INVALID_ID;
  if (g->error != RG_OK)
    return RG_INVALID_ID;
  if (g->res_count >= RG_MAX_RESOURCES) {
    fail(g, RG_ERR_CAPACITY);
    return RG_INVALID_ID;
  }
  if (buffer == VK_NULL_HANDLE) {
    fail(g, RG_ERR_RESOURCE);
    return RG_INVALID_ID;
  }
  RgResId id = g->res_count++;
  g->resources[id] = (RgResource){.name = name,
                                  .type = RG_RES_BUFFER,
                                  .buffer = buffer,
                                  .stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
  return id;
}
bool rg_update_buffer(RgGraph *g, RgResId id, VkBuffer buffer) {
  if (!valid_resource(g, id, RG_RES_BUFFER))
    return false;
  if (buffer == VK_NULL_HANDLE)
    return fail(g, RG_ERR_RESOURCE);
  RgResource *r = &g->resources[id];
  if (r->buffer != buffer) {
    r->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    r->access = 0;
  }
  r->buffer = buffer;
  return true;
}
RgPass *rg_add_pass(RgGraph *g, const char *name) {
  if (!g || g->error != RG_OK)
    return NULL;
  if (g->pass_count >= RG_MAX_PASSES) {
    fail(g, RG_ERR_CAPACITY);
    return NULL;
  }
  RgPass *p = &g->passes[g->pass_count++];
  *p = (RgPass){.name = name,
                .graph = g,
                .depth_attachment = RG_INVALID_ID,
                .depth_resolve = RG_INVALID_ID};
  return p;
}
static bool add_dep(RgPass *pass, RgResId id, RgResType type,
                    VkPipelineStageFlags stage, VkAccessFlags access,
                    VkImageLayout layout) {
  if (!pass || !pass->graph)
    return false;
  RgGraph *g = pass->graph;
  if (g->error != RG_OK)
    return false;
  if (!valid_resource(g, id, type))
    return false;
  if (!stage || (type == RG_RES_IMAGE && layout == VK_IMAGE_LAYOUT_UNDEFINED))
    return fail(g, RG_ERR_DEPENDENCY);
  for (uint32_t i = 0; i < pass->dep_count; ++i) {
    RgDependency *d = &pass->deps[i];
    if (d->res != id)
      continue;
    /* A single pass cannot establish two different layouts for one image.
     * Identical layouts can share a barrier with combined stage/access. */
    if (d->layout != layout)
      return fail(g, RG_ERR_CONFLICT);
    d->stage |= stage;
    d->access |= access;
    return true;
  }
  if (pass->dep_count >= RG_MAX_DEPS)
    return fail(g, RG_ERR_CAPACITY);
  pass->deps[pass->dep_count++] =
      (RgDependency){id, type, stage, access, layout};
  return true;
}
bool rg_pass_read_image(RgPass *p, RgResId id, VkPipelineStageFlags s,
                        VkAccessFlags a, VkImageLayout l) {
  return add_dep(p, id, RG_RES_IMAGE, s, a, l);
}
bool rg_pass_write_image(RgPass *p, RgResId id, VkPipelineStageFlags s,
                         VkAccessFlags a, VkImageLayout l) {
  return add_dep(p, id, RG_RES_IMAGE, s, a, l);
}
bool rg_pass_read_buffer(RgPass *p, RgResId id, VkPipelineStageFlags s,
                         VkAccessFlags a) {
  return add_dep(p, id, RG_RES_BUFFER, s, a, VK_IMAGE_LAYOUT_UNDEFINED);
}
bool rg_pass_write_buffer(RgPass *p, RgResId id, VkPipelineStageFlags s,
                          VkAccessFlags a) {
  return add_dep(p, id, RG_RES_BUFFER, s, a, VK_IMAGE_LAYOUT_UNDEFINED);
}
bool rg_pass_color_attachment(RgPass *p, RgResId id, bool clear, Color color) {
  return rg_pass_color_attachment_resolve(p, id, RG_INVALID_ID, clear, color);
}
bool rg_pass_color_attachment_resolve(RgPass *p, RgResId id, RgResId resolve_id,
                                      bool clear, Color color) {
  if (!p || !p->graph)
    return false;
  RgGraph *g = p->graph;
  if (g->error != RG_OK)
    return false;
  if (!valid_resource(g, id, RG_RES_IMAGE))
    return false;
  if (depth_format(g->resources[id].format))
    return fail(g, RG_ERR_ATTACHMENT);
  if (p->color_count >= RG_MAX_ATTACHMENTS)
    return fail(g, RG_ERR_CAPACITY);
  for (uint32_t i = 0; i < p->color_count; ++i)
    if (p->color_attachments[i] == id)
      return fail(g, RG_ERR_CONFLICT);
  if (p->has_depth && p->depth_attachment == id)
    return fail(g, RG_ERR_CONFLICT);
  RgResource *source = &g->resources[id];
  if (resolve_id != RG_INVALID_ID) {
    if (!valid_resource(g, resolve_id, RG_RES_IMAGE))
      return false;
    RgResource *resolve = &g->resources[resolve_id];
    if (resolve_id == id || source->samples == VK_SAMPLE_COUNT_1_BIT ||
        resolve->samples != VK_SAMPLE_COUNT_1_BIT ||
        source->format != resolve->format || source->width != resolve->width ||
        source->height != resolve->height || depth_format(resolve->format))
      return fail(g, RG_ERR_ATTACHMENT);
    if (!add_dep(p, resolve_id, RG_RES_IMAGE,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL))
      return false;
  }
  if (!add_dep(p, id, RG_RES_IMAGE,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                   (clear ? 0 : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT),
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL))
    return false;
  uint32_t i = p->color_count++;
  p->color_attachments[i] = id;
  p->color_resolves[i] = resolve_id;
  p->color_clear[i] = clear;
  p->color_clear_value[i] = color;
  return true;
}
bool rg_pass_depth_attachment(RgPass *p, RgResId id, bool clear) {
  return rg_pass_depth_attachment_resolve(p, id, RG_INVALID_ID,
                                          VK_RESOLVE_MODE_NONE, clear);
}
bool rg_pass_depth_attachment_resolve(RgPass *p, RgResId id, RgResId resolve_id,
                                      VkResolveModeFlagBits resolve_mode,
                                      bool clear) {
  if (!p || !p->graph)
    return false;
  RgGraph *g = p->graph;
  if (g->error != RG_OK)
    return false;
  if (!valid_resource(g, id, RG_RES_IMAGE))
    return false;
  if (!depth_format(g->resources[id].format))
    return fail(g, RG_ERR_ATTACHMENT);
  if (p->has_depth)
    return fail(g, RG_ERR_CONFLICT);
  for (uint32_t i = 0; i < p->color_count; ++i)
    if (p->color_attachments[i] == id)
      return fail(g, RG_ERR_CONFLICT);
  RgResource *source = &g->resources[id];
  if (resolve_id != RG_INVALID_ID) {
    if (!valid_resource(g, resolve_id, RG_RES_IMAGE))
      return false;
    RgResource *resolve = &g->resources[resolve_id];
    if (resolve_id == id || source->samples == VK_SAMPLE_COUNT_1_BIT ||
        resolve->samples != VK_SAMPLE_COUNT_1_BIT ||
        source->format != resolve->format || source->width != resolve->width ||
        source->height != resolve->height || !depth_format(resolve->format) ||
        resolve_mode == VK_RESOLVE_MODE_NONE)
      return fail(g, RG_ERR_ATTACHMENT);
    if (!add_dep(p, resolve_id, RG_RES_IMAGE,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))
      return false;
  } else if (resolve_mode != VK_RESOLVE_MODE_NONE) {
    return fail(g, RG_ERR_ATTACHMENT);
  }
  if (!add_dep(p, id, RG_RES_IMAGE,
               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                   (clear ? 0 : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT),
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))
    return false;
  p->has_depth = true;
  p->depth_attachment = id;
  p->depth_resolve = resolve_id;
  p->depth_resolve_mode = resolve_mode;
  p->depth_clear = clear;
  return true;
}
void rg_pass_execute(RgPass *p, RgExecuteFn fn, void *data) {
  if (!p || !p->graph || p->graph->error != RG_OK)
    return;
  p->exec_fn = fn;
  p->user_data = data;
}

RgResult rg_validate(RgGraph *g) {
  if (!g)
    return RG_ERR_RESOURCE;
  if (g->error != RG_OK)
    return g->error;
  for (uint32_t i = 0; i < g->pass_count; ++i) {
    RgPass *p = &g->passes[i];
    if (p->dep_count > RG_MAX_DEPS || p->color_count > RG_MAX_ATTACHMENTS) {
      fail(g, RG_ERR_CAPACITY);
      break;
    }
    for (uint32_t j = 0; j < p->dep_count; ++j) {
      RgDependency *d = &p->deps[j];
      if (!valid_resource(g, d->res, d->type))
        break;
    }
    if (g->error != RG_OK)
      break;
    uint32_t width = 0, height = 0;
    VkSampleCountFlagBits samples = 0;
    for (uint32_t j = 0; j < p->color_count + (p->has_depth ? 1u : 0u); ++j) {
      bool is_depth = j == p->color_count;
      RgResId id = is_depth ? p->depth_attachment : p->color_attachments[j];
      if (!valid_resource(g, id, RG_RES_IMAGE))
        break;
      RgResource *r = &g->resources[id];
      if (depth_format(r->format) != is_depth) {
        fail(g, RG_ERR_ATTACHMENT);
        break;
      }
      if (width && (width != r->width || height != r->height)) {
        fail(g, RG_ERR_DIMENSIONS);
        break;
      }
      if (samples && samples != r->samples) {
        fail(g, RG_ERR_ATTACHMENT);
        break;
      }
      width = r->width;
      height = r->height;
      samples = r->samples;
      RgResId resolve_id = is_depth ? p->depth_resolve : p->color_resolves[j];
      if (resolve_id != RG_INVALID_ID) {
        if (!valid_resource(g, resolve_id, RG_RES_IMAGE))
          break;
        RgResource *resolve = &g->resources[resolve_id];
        if (resolve->width != r->width || resolve->height != r->height ||
            resolve->format != r->format ||
            resolve->samples != VK_SAMPLE_COUNT_1_BIT ||
            r->samples == VK_SAMPLE_COUNT_1_BIT) {
          fail(g, RG_ERR_ATTACHMENT);
          break;
        }
      }
    }
    if (g->error != RG_OK)
      break;
  }
  return g->error;
}

/* Writes need explicit execution/memory ordering even when the layout and
 * access bits are unchanged (WAW/RAW/WAR). Read-only sharing needs no barrier.
 */
static bool access_writes(VkAccessFlags a) {
  return (a &
          (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
           VK_ACCESS_MEMORY_WRITE_BIT)) != 0;
}

bool rg_execute_checked(RgGraph *g, VkCommandBuffer cmd) {
  if (rg_validate(g) != RG_OK)
    return false;
  if (cmd == VK_NULL_HANDLE)
    return fail(g, RG_ERR_COMMAND);
  for (uint32_t i = 0; i < g->pass_count; ++i) {
    RgPass *p = &g->passes[i];
    VkImageMemoryBarrier images[RG_MAX_DEPS] = {0};
    VkBufferMemoryBarrier buffers[RG_MAX_DEPS] = {0};
    uint32_t ni = 0, nb = 0;
    VkPipelineStageFlags src = 0, dst = 0;
    for (uint32_t j = 0; j < p->dep_count; ++j) {
      RgDependency *d = &p->deps[j];
      RgResource *r = &g->resources[d->res];
      bool hazard = access_writes(r->access | d->access);
      if (r->type == RG_RES_IMAGE) {
        if (r->layout != d->layout || hazard) {
          VkImageMemoryBarrier *b = &images[ni++];
          *b = (VkImageMemoryBarrier){
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .oldLayout = r->layout,
              .newLayout = d->layout,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = r->image,
              .subresourceRange = {aspects(r->format), 0, 1, 0, 1},
              .srcAccessMask = r->access,
              .dstAccessMask = d->access};
          src |= r->stage;
          dst |= d->stage;
        }
        r->layout = d->layout;
      } else {
        if (hazard) {
          VkBufferMemoryBarrier *b = &buffers[nb++];
          *b = (VkBufferMemoryBarrier){
              .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .buffer = r->buffer,
              .offset = 0,
              .size = VK_WHOLE_SIZE,
              .srcAccessMask = r->access,
              .dstAccessMask = d->access};
          src |= r->stage;
          dst |= d->stage;
        }
      }
      r->access = d->access;
      r->stage = d->stage;
    }
    if (ni || nb)
      vkCmdPipelineBarrier(cmd, src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           dst ? dst : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                           0, NULL, nb, buffers, ni, images);
    bool rendering = p->color_count || p->has_depth;
    if (rendering) {
      VkRenderingAttachmentInfo colors[RG_MAX_ATTACHMENTS] = {0};
      VkRenderingAttachmentInfo depth = {0};
      uint32_t width = 0, height = 0;
      for (uint32_t c = 0; c < p->color_count; ++c) {
        RgResource *r = &g->resources[p->color_attachments[c]];
        width = r->width;
        height = r->height;
        Color col = p->color_clear_value[c];
        colors[c] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = r->view,
            .imageLayout = r->layout,
            .resolveMode = p->color_resolves[c] != RG_INVALID_ID
                               ? VK_RESOLVE_MODE_AVERAGE_BIT
                               : VK_RESOLVE_MODE_NONE,
            .resolveImageView = p->color_resolves[c] != RG_INVALID_ID
                                    ? g->resources[p->color_resolves[c]].view
                                    : VK_NULL_HANDLE,
            .resolveImageLayout =
                p->color_resolves[c] != RG_INVALID_ID
                    ? g->resources[p->color_resolves[c]].layout
                    : VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = p->color_clear[c] ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                        : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color = {{col.r, col.g, col.b, col.a}}};
      }
      if (p->has_depth) {
        RgResource *r = &g->resources[p->depth_attachment];
        if (!width) {
          width = r->width;
          height = r->height;
        }
        depth = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = r->view,
            .imageLayout = r->layout,
            .resolveMode = p->depth_resolve_mode,
            .resolveImageView = p->depth_resolve != RG_INVALID_ID
                                    ? g->resources[p->depth_resolve].view
                                    : VK_NULL_HANDLE,
            .resolveImageLayout = p->depth_resolve != RG_INVALID_ID
                                      ? g->resources[p->depth_resolve].layout
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = p->depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                     : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.depthStencil = {1.0f, 0}};
      }
      VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {width, height}},
                              .layerCount = 1,
                              .colorAttachmentCount = p->color_count,
                              .pColorAttachments =
                                  p->color_count ? colors : NULL,
                              .pDepthAttachment = p->has_depth ? &depth : NULL};
      vkCmdBeginRendering(cmd, &info);
    }
    if (p->exec_fn)
      p->exec_fn(cmd, p->user_data);
    if (rendering)
      vkCmdEndRendering(cmd);
  }
  return true;
}
void rg_execute(RgGraph *g, VkCommandBuffer cmd) {
  (void)rg_execute_checked(g, cmd);
}

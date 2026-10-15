/// Native contract for production render_graph.c (real functions, fake Vulkan
/// recording)
#include "render_graph.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int commands, barriers, begins, ends, callbacks;
static VkImageMemoryBarrier last_img;
static VkBufferMemoryBarrier last_buf;
static VkPipelineStageFlags last_src, last_dst;
static uint32_t last_ni, last_nb;
static VkRect2D last_area;
static uint32_t last_colors;
static VkRenderingAttachmentInfo last_color_attachment;
static VkRenderingAttachmentInfo last_depth_attachment;

void vkCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                          VkPipelineStageFlags dst, VkDependencyFlags flags,
                          uint32_t nmem, const void *mem, uint32_t nb,
                          const VkBufferMemoryBarrier *buf, uint32_t ni,
                          const VkImageMemoryBarrier *img) {
  assert(cmd && !flags && !nmem && !mem && src && dst);
  ++commands;
  ++barriers;
  last_src = src;
  last_dst = dst;
  last_ni = ni;
  last_nb = nb;
  if (ni)
    last_img = img[ni - 1];
  if (nb)
    last_buf = buf[nb - 1];
}
void vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
  assert(cmd && info && info->renderArea.extent.width && info->layerCount == 1);
  ++commands;
  ++begins;
  last_area = info->renderArea;
  last_colors = info->colorAttachmentCount;
  if (info->colorAttachmentCount)
    last_color_attachment = info->pColorAttachments[0];
  if (info->pDepthAttachment)
    last_depth_attachment = *info->pDepthAttachment;
}
void vkCmdEndRendering(VkCommandBuffer cmd) {
  assert(cmd);
  ++commands;
  ++ends;
}
static void draw(VkCommandBuffer cmd, void *user) {
  assert(cmd == 55 && user == (void *)123);
  ++commands;
  ++callbacks;
}
static void clear_calls(void) {
  commands = barriers = begins = ends = callbacks = 0;
  last_ni = last_nb = 0;
}
static RgResId image(RgGraph *g, VkImage img, VkFormat fmt, uint32_t w,
                     uint32_t h) {
  return rg_import_image(g, "test", img, img + 100, fmt, w, h,
                         VK_IMAGE_LAYOUT_UNDEFINED);
}

int main(void) {
  Color color = {0.25f, 0.5f, 0.75f, 1.0f};
  RgGraph *g = rg_create();
  assert(g && rg_status(g) == RG_OK);
  assert(!strcmp(rg_result_name(RG_ERR_CONFLICT), "conflicting-dependency"));
  assert(rg_validate(NULL) == RG_ERR_RESOURCE);
  assert(!rg_execute_checked(NULL, 55));
  assert(image(g, 1, VK_FORMAT_R8G8B8A8_UNORM, 640, 480) == 0);
  RgResId depth = image(g, 2, VK_FORMAT_D32_SFLOAT, 640, 480);
  RgResId buf = rg_import_buffer(g, "draw", 9);
  assert(buf == 2);
  RgPass *p = rg_add_pass(g, "main");
  assert(p);
  assert(rg_pass_read_buffer(p, buf, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT));
  assert(rg_pass_color_attachment(p, 0, true, color));
  assert(rg_pass_depth_attachment(p, depth, true));
  rg_pass_execute(p, draw, (void *)123);
  assert(rg_validate(g) == RG_OK);
  clear_calls();
  assert(rg_execute_checked(g, 55));
  assert(begins == 1 && ends == 1 && callbacks == 1 && barriers == 1);
  assert(last_ni == 2 && last_nb == 0);
  assert(last_img.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
  assert(last_area.extent.width == 640 && last_colors == 1);
  assert(last_src && last_dst);

  /* Separate render passes using the same layout and access are still
     ordered when either side writes. */
  RgPass *p2 = rg_add_pass(g, "second");
  assert(rg_pass_write_buffer(p2, buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_SHADER_WRITE_BIT));
  assert(rg_pass_read_image(p2, depth, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
  assert(rg_execute_checked(g, 55));
  assert(last_nb == 1 && last_ni == 1);
  assert(last_buf.dstAccessMask == VK_ACCESS_SHADER_WRITE_BIT);
  assert(last_img.oldLayout ==
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  assert(last_img.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  rg_reset(g);
  clear_calls();
  RgResId acquired = rg_import_acquired_image(
      g, "acquired", 21, 121, VK_FORMAT_R8G8B8A8_UNORM, 640, 480,
      VK_IMAGE_LAYOUT_UNDEFINED);
  p = rg_add_pass(g, "first-acquired-write");
  assert(rg_pass_color_attachment(p, acquired, true, color));
  assert(rg_execute_checked(g, 55));
  assert(last_src == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  assert(last_dst == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  rg_reset(g);
  clear_calls();
  assert(image(g, 4, VK_FORMAT_R8G8B8A8_UNORM, 32, 16) == 0);
  p = rg_add_pass(g, "invalid-id");
  assert(!rg_pass_read_image(p, RG_INVALID_ID, 1, 1, VK_IMAGE_LAYOUT_GENERAL));
  assert(rg_status(g) == RG_ERR_RESOURCE);
  assert(!rg_execute_checked(g, 55) && !commands && !callbacks);
  assert(!rg_add_pass(g, "after-error"));

  rg_reset(g);
  RgResId c = image(g, 5, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  p = rg_add_pass(g, "invalid-type");
  assert(!rg_pass_read_buffer(p, c, 1, 1));
  assert(rg_status(g) == RG_ERR_TYPE);

  rg_reset(g);
  c = image(g, 6, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  p = rg_add_pass(g, "same-layout");
  assert(rg_pass_read_image(p, c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_GENERAL));
  assert(rg_pass_write_image(p, c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_GENERAL));
  assert(!rg_pass_color_attachment(p, c, false, color));
  assert(rg_status(g) == RG_ERR_CONFLICT);
  rg_reset(g);
  c = image(g, 7, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  p = rg_add_pass(g, "duplicated-color");
  assert(rg_pass_color_attachment(p, c, true, color));
  assert(!rg_pass_color_attachment(p, c, false, color));
  assert(rg_status(g) == RG_ERR_CONFLICT);

  rg_reset(g);
  c = image(g, 8, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  depth = image(g, 9, VK_FORMAT_D24_UNORM_S8_UINT, 16, 8);
  p = rg_add_pass(g, "mismatched-attachment");
  assert(rg_pass_color_attachment(p, c, false, color));
  assert(rg_pass_depth_attachment(p, depth, true));
  assert(rg_validate(g) == RG_ERR_DIMENSIONS);
  clear_calls();
  assert(!rg_execute_checked(g, 55) && !commands);

  rg_reset(g);
  c = image(g, 10, VK_FORMAT_D24_UNORM_S8_UINT, 16, 16);
  p = rg_add_pass(g, "wrong-attachment");
  assert(!rg_pass_color_attachment(p, c, false, color));
  assert(rg_status(g) == RG_ERR_ATTACHMENT);

  rg_reset(g);
  c = image(g, 11, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  assert(!rg_update_buffer(g, c, 12) && rg_status(g) == RG_ERR_TYPE);
  rg_reset(g);
  assert(image(g, 0, VK_FORMAT_R8G8B8A8_UNORM, 16, 16) == RG_INVALID_ID);
  assert(rg_status(g) == RG_ERR_RESOURCE);

  rg_reset(g);
  for (int i = 0; i < 128; ++i)
    assert(rg_import_buffer(g, "x", i + 1) == (RgResId)i);
  assert(rg_import_buffer(g, "overflow", 10) == RG_INVALID_ID);
  assert(rg_status(g) == RG_ERR_CAPACITY);

  rg_reset(g);
  for (int i = 0; i < 32; ++i)
    assert(rg_add_pass(g, "p"));
  assert(!rg_add_pass(g, "overflow"));
  assert(rg_status(g) == RG_ERR_CAPACITY);

  rg_reset(g);
  for (int i = 0; i < 17; ++i)
    assert(rg_import_buffer(g, "b", i + 1) == (RgResId)i);
  p = rg_add_pass(g, "overfull");
  for (int i = 0; i < 16; ++i)
    assert(rg_pass_read_buffer(p, (RgResId)i, 1, 1));
  assert(!rg_pass_read_buffer(p, 16, 1, 1));
  assert(rg_status(g) == RG_ERR_CAPACITY);

  rg_reset(g);
  c = image(g, 14, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  p = rg_add_pass(g, "repeat-write");
  assert(rg_pass_color_attachment(p, c, true, color));
  RgPass *q = rg_add_pass(g, "repeat-write-again");
  assert(rg_pass_color_attachment(q, c, false, color));
  clear_calls();
  assert(rg_execute_checked(g, 55) && barriers == 2);
  assert(last_img.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  assert(last_img.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  assert(last_img.dstAccessMask ==
         (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT));

  rg_reset(g);
  depth = image(g, 15, VK_FORMAT_D32_SFLOAT, 16, 16);
  p = rg_add_pass(g, "depth-clear");
  assert(rg_pass_depth_attachment(p, depth, true));
  q = rg_add_pass(g, "depth-load");
  assert(rg_pass_depth_attachment(q, depth, false));
  clear_calls();
  assert(rg_execute_checked(g, 55) && barriers == 2);
  assert(last_img.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  assert(last_img.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  assert(last_img.dstAccessMask ==
         (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT));

  rg_reset(g);
  c = image(g, 20, VK_FORMAT_D24_UNORM_S8_UINT, 16, 16);
  p = rg_add_pass(g, "stencil-aspect");
  assert(rg_pass_depth_attachment(p, c, true));
  clear_calls();
  assert(rg_execute_checked(g, 55));
  assert(last_img.subresourceRange.aspectMask ==
         (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

  rg_reset(g);
  RgResId ms_color = rg_import_image_samples(
      g, "multisample-color", 31, 131, VK_FORMAT_R8G8B8A8_UNORM, 16, 16,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_SAMPLE_COUNT_2_BIT);
  RgResId resolved_color = image(g, 32, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  RgResId ms_depth = rg_import_image_samples(
      g, "multisample-depth", 33, 133, VK_FORMAT_D32_SFLOAT, 16, 16,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_SAMPLE_COUNT_2_BIT);
  RgResId resolved_depth = image(g, 34, VK_FORMAT_D32_SFLOAT, 16, 16);
  p = rg_add_pass(g, "multisample-resolve");
  assert(rg_pass_color_attachment_resolve(p, ms_color, resolved_color, true,
                                          color));
  assert(rg_pass_depth_attachment_resolve(p, ms_depth, resolved_depth,
                                          VK_RESOLVE_MODE_MAX_BIT, true));
  assert(rg_validate(g) == RG_OK);
  clear_calls();
  assert(rg_execute_checked(g, 55));
  assert(last_color_attachment.imageView == 131);
  assert(last_color_attachment.resolveMode == VK_RESOLVE_MODE_AVERAGE_BIT);
  assert(last_color_attachment.resolveImageView == 132);
  assert(last_depth_attachment.imageView == 133);
  assert(last_depth_attachment.resolveMode == VK_RESOLVE_MODE_MAX_BIT);
  assert(last_depth_attachment.resolveImageView == 134);

  rg_reset(g);
  ms_color = rg_import_image_samples(
      g, "multisample-color", 35, 135, VK_FORMAT_R8G8B8A8_UNORM, 16, 16,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_SAMPLE_COUNT_2_BIT);
  resolved_color = image(g, 36, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  p = rg_add_pass(g, "sample-mismatch");
  assert(rg_pass_color_attachment(p, resolved_color, true, color));
  assert(rg_pass_depth_attachment_resolve(p, ms_color, resolved_color,
                                          VK_RESOLVE_MODE_MAX_BIT,
                                          true) == false);
  assert(rg_status(g) != RG_OK);

  rg_reset(g);
  c = image(g, 22, VK_FORMAT_R8G8B8A8_UNORM, 16, 16);
  p = rg_add_pass(g, "invalid-command");
  assert(rg_pass_color_attachment(p, c, true, color));
  clear_calls();
  assert(!rg_execute_checked(g, 0) && !commands);
  assert(rg_status(g) == RG_ERR_COMMAND);
  rg_destroy(g);
  puts("render graph admission, hazards, attachments, capacity and failure "
       "atomicity OK");
  return 0;
}

#include "image_editor.h"

#include "editor.h"
#include "live_editor.h"
#include "renderer.h"
#include "theme.h"
#include "vulkan_setup.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static float image_fit_scale(const ObsImageEditor *image,
                             const Texture2D *texture, float width,
                             float height) {
  if (!image || !texture || !texture->width || !texture->height ||
      width <= 0.0f || height <= 0.0f)
    return 1.0f;
  const float available_width = fmaxf(width - 40.0f, 1.0f);
  const float available_height = fmaxf(height - 92.0f, 1.0f);
  const float fit_x = available_width / (float)texture->width;
  const float fit_y = available_height / (float)texture->height;
  return fminf(fminf(fit_x, fit_y), 1.0f);
}

bool obs_image_editor_open(ObsImageEditor *image, const char *path) {
  if (!image || !path || !*path) return false;
  int32_t texture_index = -1;
  if (!texture_pool_acquire(&context, path, &texture_index)) return false;
  memset(image, 0, sizeof(*image));
  image->texture_index = texture_index;
  image->zoom = 1.0f;
  const int length = snprintf(image->path, sizeof(image->path), "%s", path);
  if (length < 0 || (size_t)length >= sizeof(image->path)) {
    texture_pool_release(texture_index);
    *image = (ObsImageEditor){.texture_index = -1};
    return false;
  }
  return true;
}

void obs_image_editor_close(ObsImageEditor *image) {
  if (!image) return;
  if (image->texture_index >= 0)
    (void)texture_pool_release(image->texture_index);
  *image = (ObsImageEditor){.texture_index = -1};
}

void obs_image_editor_zoom(ObsImageEditor *image, float factor) {
  if (!image || !isfinite(factor) || factor <= 0.0f) return;
  float next_zoom;
  if (obs_monad_live_editor_image_zoom_level(
          image->zoom, factor, &next_zoom))
    image->zoom = next_zoom;
}

void obs_image_editor_fit(ObsImageEditor *image) {
  if (!image) return;
  image->zoom = 1.0f;
  image->pan_x = 0.0f;
  image->pan_y = 0.0f;
}

static bool image_apply_pointer_transition(ObsImageEditor *image, int action,
                                           double x, double y) {
  const ObsMonadImagePointerState current = {
      .dragging = image->dragging,
      .pan_x = image->pan_x,
      .pan_y = image->pan_y,
      .last_x = image->drag_x,
      .last_y = image->drag_y
  };
  ObsMonadImagePointerState next;
  if (!obs_monad_live_editor_image_pointer_transition(
          action, &current, x, y, &next))
    return false;
  image->dragging = next.dragging;
  image->pan_x = next.pan_x;
  image->pan_y = next.pan_y;
  image->drag_x = next.last_x;
  image->drag_y = next.last_y;
  return true;
}

void obs_image_editor_pointer_button(ObsImageEditor *image, int button,
                                     int action, double x, double y) {
  if (!image || button != GLFW_MOUSE_BUTTON_LEFT) return;
  const int transition = action == GLFW_PRESS ? 1
                         : action == GLFW_RELEASE ? 2 : 0;
  if (transition != 0 &&
      !image_apply_pointer_transition(image, transition, x, y))
    fprintf(stderr, "Monad image-editor pointer button policy failed\n");
}

void obs_image_editor_pointer_move(ObsImageEditor *image, double x, double y) {
  if (!image) return;
  if (!image_apply_pointer_transition(image, 3, x, y))
    fprintf(stderr, "Monad image-editor pointer motion policy failed\n");
}

void obs_image_editor_scroll(ObsImageEditor *image, double x, double y,
                             double amount) {
  if (!image || !isfinite(amount) || amount == 0.0) return;
  const float old_zoom = image->zoom;
  const float factor = (float)pow(1.12, amount);
  float next_zoom = old_zoom;
  if (!obs_monad_live_editor_image_zoom_level(
          old_zoom, factor, &next_zoom))
    return;
  const float cx = (float)context.drawExtent.width * 0.5f;
  const float cy = (float)context.drawExtent.height * 0.5f;
  float next_pan_x;
  float next_pan_y;
  if (!obs_monad_live_editor_image_zoom_pan(
          image->pan_x, image->pan_y, old_zoom, next_zoom, cx, cy,
          (float)x, (float)y, &next_pan_x, &next_pan_y))
    return;
  image->zoom = next_zoom;
  image->pan_x = next_pan_x;
  image->pan_y = next_pan_y;
}

void obs_image_editor_render(const ObsImageEditor *image) {
  if (!image) return;
  const float width = (float)context.drawExtent.width;
  const float height = (float)context.drawExtent.height;
  const Color background = {0.055f, 0.063f, 0.078f, 1.0f};
  const Color header = {0.085f, 0.096f, 0.118f, 1.0f};
  const Color stage = {0.025f, 0.030f, 0.039f, 1.0f};
  exQuad2D((vec2){0.0f, 0.0f}, (vec2){width, height},
           (vec4){0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, background, background);
  exQuad2D((vec2){0.0f, 0.0f}, (vec2){width, 54.0f},
           (vec4){0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, header, header);
  exQuad2D((vec2){12.0f, 68.0f},
           (vec2){fmaxf(width - 24.0f, 1.0f),
                  fmaxf(height - 82.0f, 1.0f)},
           (vec4){8.0f, 8.0f, 8.0f, 8.0f}, 1.0f, CT.border, stage);

  Texture2D *texture = texture_pool_get(image->texture_index);
  if (!texture || !texture->loaded) {
    const char *status = texture && atomic_load(&texture->status) ==
                                       TEXTURE_STATUS_FAILED
                             ? "Image load failed"
                             : "Loading image…";
    if (editor.font) text(editor.font, status, 28.0f, 106.0f, CT.text_dim);
  } else {
    const float base_scale =
        image_fit_scale(image, texture, width, height);
    const float scale = base_scale * image->zoom;
    const float image_width = (float)texture->width * scale;
    const float image_height = (float)texture->height * scale;
    const float x = (width - image_width) * 0.5f + image->pan_x;
    const float y = (height - image_height) * 0.5f + image->pan_y;
    renderer2D_set_clip_rect(12.0f, 68.0f,
                            fmaxf(width - 24.0f, 1.0f),
                            fmaxf(height - 108.0f, 1.0f));
    texture2D((vec2){x, y}, (vec2){image_width, image_height}, texture,
              (Color){1.0f, 1.0f, 1.0f, 1.0f});
    renderer2D_clear_clip_rect();
  }

  if (editor.font) {
    char title[160];
    const char *base = strrchr(image->path, '/');
    snprintf(title, sizeof(title), "Image Editor    %.140s",
             base ? base + 1 : image->path);
    text(editor.font, title, 18.0f, 28.0f, CT.text);
    char status[96];
    const float percent = image->zoom * 100.0f;
    snprintf(status, sizeof(status), "%.0f%%    drag to pan · wheel to zoom · 0 to fit",
             (double)percent);
    text(editor.font, status, 18.0f, height - 26.0f, CT.text_dim);
  }
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  int32_t texture_index;
  char path[512];
  float zoom;
  float pan_x;
  float pan_y;
  double drag_x;
  double drag_y;
  bool dragging;
} ObsImageEditor;

bool obs_image_editor_open(ObsImageEditor *image, const char *path);
void obs_image_editor_close(ObsImageEditor *image);
void obs_image_editor_render(const ObsImageEditor *image);
void obs_image_editor_pointer_button(ObsImageEditor *image, int button,
                                     int action, double x, double y);
void obs_image_editor_pointer_move(ObsImageEditor *image, double x, double y);
void obs_image_editor_scroll(ObsImageEditor *image, double x, double y,
                             double amount);
void obs_image_editor_zoom(ObsImageEditor *image, float factor);
void obs_image_editor_fit(ObsImageEditor *image);

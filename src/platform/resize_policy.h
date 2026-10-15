#pragma once

#include <stdbool.h>

#define OBS_RESIZE_SETTLE_SECONDS 0.10

bool obs_resize_rebuild_due(bool swapchain_out_of_date,
                            bool resize_pending,
                            double seconds_since_resize);
void obs_resize_reset_after_rebuild(bool *swapchain_out_of_date,
                                    bool *resize_pending);
bool obs_resize_framebuffer_is_empty(int width, int height);

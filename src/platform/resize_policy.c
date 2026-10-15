#include "resize_policy.h"

bool obs_resize_rebuild_due(bool swapchain_out_of_date,
                            bool resize_pending,
                            double seconds_since_resize) {
    return swapchain_out_of_date ||
           (resize_pending && seconds_since_resize >= OBS_RESIZE_SETTLE_SECONDS);
}

void obs_resize_reset_after_rebuild(bool *swapchain_out_of_date,
                                    bool *resize_pending) {
    *swapchain_out_of_date = false;
    *resize_pending = false;
}

bool obs_resize_framebuffer_is_empty(int width, int height) {
    return width <= 0 || height <= 0;
}

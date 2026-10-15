/// window.h — Headless declaration fixture for embedded frame API checks
/* Only for syntax checking src/monad/frame_api.c against the public MonadC
 * declarations. This fixture DOES NOT stand in for a live Vulkan window. */
#ifndef OBSIDIAN_TEST_WINDOW_H
#define OBSIDIAN_TEST_WINDOW_H
#include <stdbool.h>
#include <stdint.h>
typedef struct ObsFrameProfile {
    uint64_t begin_fence_wait_ns, geometry_upload_ns, acquire_image_ns;
    uint64_t image_fence_wait_ns, command_record_ns, queue_submit_ns;
    uint64_t queue_present_ns, vertex2d_data_bytes;
    uint32_t vertex2d_count;
} ObsFrameProfile;
void obs_frame_profile_enable(bool enabled);
void obs_frame_profile_get(ObsFrameProfile *out);
#endif

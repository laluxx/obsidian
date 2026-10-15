#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

typedef enum ObsGpuProfileStage {
    OBS_GPU_PROFILE_FRAME = 0,
    OBS_GPU_PROFILE_DYNAMIC_GEOMETRY,
    OBS_GPU_PROFILE_BOUNDS_REFIT,
    OBS_GPU_PROFILE_SHADOWS,
    OBS_GPU_PROFILE_SHADOW_CASCADE_0,
    OBS_GPU_PROFILE_SHADOW_CASCADE_1,
    OBS_GPU_PROFILE_SHADOW_CASCADE_2,
    OBS_GPU_PROFILE_SHADOW_CASCADE_3,
    OBS_GPU_PROFILE_MAIN_FORWARD,
    OBS_GPU_PROFILE_TRANSMISSION_COPY,
    OBS_GPU_PROFILE_TRANSPARENT_WIREFRAME,
    OBS_GPU_PROFILE_UI,
    OBS_GPU_PROFILE_READBACK,
    OBS_GPU_PROFILE_HZB,
    OBS_GPU_PROFILE_HZB_MIP_0,
    OBS_GPU_PROFILE_HZB_MIP_1,
    OBS_GPU_PROFILE_HZB_MIP_2,
    OBS_GPU_PROFILE_HZB_MIP_3,
    OBS_GPU_PROFILE_HZB_MIP_4,
    OBS_GPU_PROFILE_HZB_MIP_5,
    OBS_GPU_PROFILE_HZB_MIP_6,
    OBS_GPU_PROFILE_HZB_MIP_7,
    OBS_GPU_PROFILE_HZB_MIP_8,
    OBS_GPU_PROFILE_HZB_MIP_9,
    OBS_GPU_PROFILE_HZB_MIP_10,
    OBS_GPU_PROFILE_HZB_MIP_11,
    OBS_GPU_PROFILE_HZB_MIP_12,
    OBS_GPU_PROFILE_HZB_MIP_13,
    OBS_GPU_PROFILE_HZB_MIP_14,
    OBS_GPU_PROFILE_HZB_MIP_15,
    OBS_GPU_PROFILE_STAGE_COUNT
} ObsGpuProfileStage;

_Static_assert(OBS_GPU_PROFILE_STAGE_COUNT <= 32,
               "GPU profile stage mask must fit in uint32_t");

static inline const char *obs_gpu_profile_stage_name(ObsGpuProfileStage stage) {
    switch (stage) {
    case OBS_GPU_PROFILE_FRAME: return "frame";
    case OBS_GPU_PROFILE_DYNAMIC_GEOMETRY: return "dynamic_geometry";
    case OBS_GPU_PROFILE_BOUNDS_REFIT: return "bounds_refit";
    case OBS_GPU_PROFILE_SHADOWS: return "shadows";
    case OBS_GPU_PROFILE_SHADOW_CASCADE_0: return "shadow.cascade.0";
    case OBS_GPU_PROFILE_SHADOW_CASCADE_1: return "shadow.cascade.1";
    case OBS_GPU_PROFILE_SHADOW_CASCADE_2: return "shadow.cascade.2";
    case OBS_GPU_PROFILE_SHADOW_CASCADE_3: return "shadow.cascade.3";
    case OBS_GPU_PROFILE_MAIN_FORWARD: return "main_forward";
    case OBS_GPU_PROFILE_TRANSMISSION_COPY: return "transmission_copy";
    case OBS_GPU_PROFILE_TRANSPARENT_WIREFRAME: return "transparent_wireframe";
    case OBS_GPU_PROFILE_UI: return "ui";
    case OBS_GPU_PROFILE_READBACK: return "readback";
    case OBS_GPU_PROFILE_HZB: return "hzb";
    case OBS_GPU_PROFILE_HZB_MIP_0: return "hzb.mip.0";
    case OBS_GPU_PROFILE_HZB_MIP_1: return "hzb.mip.1";
    case OBS_GPU_PROFILE_HZB_MIP_2: return "hzb.mip.2";
    case OBS_GPU_PROFILE_HZB_MIP_3: return "hzb.mip.3";
    case OBS_GPU_PROFILE_HZB_MIP_4: return "hzb.mip.4";
    case OBS_GPU_PROFILE_HZB_MIP_5: return "hzb.mip.5";
    case OBS_GPU_PROFILE_HZB_MIP_6: return "hzb.mip.6";
    case OBS_GPU_PROFILE_HZB_MIP_7: return "hzb.mip.7";
    case OBS_GPU_PROFILE_HZB_MIP_8: return "hzb.mip.8";
    case OBS_GPU_PROFILE_HZB_MIP_9: return "hzb.mip.9";
    case OBS_GPU_PROFILE_HZB_MIP_10: return "hzb.mip.10";
    case OBS_GPU_PROFILE_HZB_MIP_11: return "hzb.mip.11";
    case OBS_GPU_PROFILE_HZB_MIP_12: return "hzb.mip.12";
    case OBS_GPU_PROFILE_HZB_MIP_13: return "hzb.mip.13";
    case OBS_GPU_PROFILE_HZB_MIP_14: return "hzb.mip.14";
    case OBS_GPU_PROFILE_HZB_MIP_15: return "hzb.mip.15";
    case OBS_GPU_PROFILE_STAGE_COUNT: break;
    }
    return NULL;
}

static inline int obs_gpu_profile_stage_parent(ObsGpuProfileStage stage) {
    if (stage >= OBS_GPU_PROFILE_SHADOW_CASCADE_0 &&
        stage <= OBS_GPU_PROFILE_SHADOW_CASCADE_3)
        return OBS_GPU_PROFILE_SHADOWS;
    if (stage >= OBS_GPU_PROFILE_HZB_MIP_0 &&
        stage <= OBS_GPU_PROFILE_HZB_MIP_15)
        return OBS_GPU_PROFILE_HZB;
    return -1;
}

typedef struct ObsGpuFrameProfile {
    uint64_t frame_sequence;
    uint64_t gpu_ns;
    uint64_t stage_ns[OBS_GPU_PROFILE_STAGE_COUNT];
    uint32_t stage_mask;
} ObsGpuFrameProfile;

#define OBS_GPU_PROFILE_QUERY_COUNT(frames) \
    ((frames) * OBS_GPU_PROFILE_STAGE_COUNT * 2 + 2)

static inline uint32_t obs_gpu_profile_query_index(uint32_t frame,
                                                    ObsGpuProfileStage stage,
                                                    uint32_t endpoint) {
    return frame * OBS_GPU_PROFILE_STAGE_COUNT * 2 +
           (uint32_t)stage * 2 + endpoint;
}

static inline uint32_t obs_gpu_profile_upload_query_base(uint32_t frames) {
    return frames * OBS_GPU_PROFILE_STAGE_COUNT * 2;
}

/* Convert one same-queue timestamp interval. The caller must ensure both
 * queries are available and the interval is shorter than one full counter
 * wrap; Vulkan timestamps do not identify how many wraps elapsed. */
static inline bool obs_gpu_profile_timestamp_delta_ns(
    uint64_t begin, uint64_t end, uint32_t valid_bits,
    double timestamp_period_ns, uint64_t *nanoseconds) {
    if (!nanoseconds || valid_bits == 0 || valid_bits > 64 ||
        !isfinite(timestamp_period_ns) || timestamp_period_ns <= 0.0)
        return false;

    uint64_t ticks = end - begin;
    if (valid_bits < 64)
        ticks &= (UINT64_C(1) << valid_bits) - 1;

    const long double elapsed_ns =
        (long double)ticks * (long double)timestamp_period_ns;
    if (!isfinite(elapsed_ns) ||
        elapsed_ns >= 18446744073709551616.0L)
        return false;

    *nanoseconds = (uint64_t)elapsed_ns;
    return true;
}

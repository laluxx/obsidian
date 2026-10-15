/// headless_job — bounded CLI agent request and JSON success receipt
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OBS_AGENT_SOURCE_NONE,
    OBS_AGENT_SOURCE_INLINE,
    OBS_AGENT_SOURCE_FILE
} ObsAgentSourceKind;

typedef struct {
    const char *image_path;
    uint32_t width;
    uint32_t height;
    uint32_t completed_frames;
    ObsAgentSourceKind source;
} ObsAgentReceipt;

/* 1..10000, decimal digits only; rejects signs, whitespace and overflow. */
bool obs_headless_parse_frames(const char *text, uint32_t *out);

/* The production caller owns Vulkan and Monad. This driver ensures ordered,
 * bounded frame calls, exactly one final/readback call, and fail-fast errors. */
typedef bool (*ObsAgentFrameFn)(uint32_t index, bool final_frame, void *userdata);
bool obs_headless_execute_frames(uint32_t count, ObsAgentFrameFn frame,
                                 void *userdata, uint32_t *completed);

/* A completed frame and image must exist BEFORE the caller publishes this
 * receipt. JSON path encoding is bounded and does not trust argv contents.
 * This is an output receipt, not proof of GPU validation or Monad QTT safety. */
bool obs_headless_write_receipt(const char *path, const ObsAgentReceipt *value);

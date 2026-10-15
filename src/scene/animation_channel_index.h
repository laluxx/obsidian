#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct OmdlChannel OmdlChannel;
typedef struct OmdlAnimation OmdlAnimation;

typedef struct {
    uint32_t node_count;
    uint32_t animation_channel_offset;
    uint32_t animation_channel_count;
    uint32_t channel_index_count;
    uint32_t weight_index_count;
    uint32_t *node_offsets;
    uint32_t *channel_indices;
    uint32_t *weight_indices;
    bool attempted;
    bool ready;
} ObsAnimationChannelIndex;

bool obs_animation_channel_index_build(uint32_t node_count,
                                       const OmdlChannel *channels,
                                       const OmdlAnimation *animation,
                                       ObsAnimationChannelIndex *index);
void obs_animation_channel_index_destroy(ObsAnimationChannelIndex *index);

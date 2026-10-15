#include "scene.h"

#include <stddef.h>
#include <stdlib.h>

void obs_animation_channel_index_destroy(ObsAnimationChannelIndex *index) {
    if (!index)
        return;
    free(index->node_offsets);
    free(index->channel_indices);
    free(index->weight_indices);
    *index = (ObsAnimationChannelIndex){0};
}

bool obs_animation_channel_index_build(uint32_t node_count,
                                       const OmdlChannel *channels,
                                       const OmdlAnimation *animation,
                                       ObsAnimationChannelIndex *index) {
    if (!index || !animation)
        return false;
    if (index->attempted && index->node_count == node_count &&
        index->animation_channel_offset == animation->channel_offset &&
        index->animation_channel_count == animation->channel_count)
        return index->ready;

    obs_animation_channel_index_destroy(index);
    index->node_count = node_count;
    index->animation_channel_offset = animation->channel_offset;
    index->animation_channel_count = animation->channel_count;
    index->attempted = true;

    if (!node_count || !animation->channel_count || !channels ||
        animation->channel_count > UINT32_MAX - animation->channel_offset ||
        (size_t)node_count + 1 > SIZE_MAX / sizeof(*index->node_offsets))
        return false;
#if SIZE_MAX <= UINT32_MAX
    if (node_count > SIZE_MAX / sizeof(uint32_t) ||
        animation->channel_count > SIZE_MAX / sizeof(uint32_t))
        return false;
#endif

    uint32_t *offsets =
        calloc((size_t)node_count + 1, sizeof(*offsets));
    uint32_t *cursor = malloc((size_t)node_count * sizeof(*cursor));
    if (!offsets || !cursor) {
        free(offsets);
        free(cursor);
        return false;
    }

    uint32_t channel_index_count = 0;
    uint32_t weight_index_count = 0;
    for (uint32_t c = 0; c < animation->channel_count; ++c) {
        const OmdlChannel *channel =
            &channels[(size_t)animation->channel_offset + c];
        if (channel->target_node == UINT32_MAX)
            continue;
        if (channel->path_type == cgltf_animation_path_type_weights) {
            ++weight_index_count;
        } else if (channel->target_node < node_count) {
            ++offsets[(size_t)channel->target_node + 1];
            ++channel_index_count;
        }
    }

    for (uint32_t n = 0; n < node_count; ++n) {
        offsets[n + 1] += offsets[n];
        cursor[n] = offsets[n];
    }

    uint32_t *channel_indices =
        channel_index_count
            ? malloc((size_t)channel_index_count * sizeof(*channel_indices))
            : NULL;
    uint32_t *weight_indices =
        weight_index_count
            ? malloc((size_t)weight_index_count * sizeof(*weight_indices))
            : NULL;
    if ((channel_index_count && !channel_indices) ||
        (weight_index_count && !weight_indices)) {
        free(offsets);
        free(cursor);
        free(channel_indices);
        free(weight_indices);
        return false;
    }

    uint32_t next_weight = 0;
    for (uint32_t c = 0; c < animation->channel_count; ++c) {
        const OmdlChannel *channel =
            &channels[(size_t)animation->channel_offset + c];
        if (channel->target_node == UINT32_MAX)
            continue;
        if (channel->path_type == cgltf_animation_path_type_weights) {
            weight_indices[next_weight++] = c;
        } else if (channel->target_node < node_count) {
            channel_indices[cursor[channel->target_node]++] = c;
        }
    }
    free(cursor);

    index->node_offsets = offsets;
    index->channel_indices = channel_indices;
    index->weight_indices = weight_indices;
    index->channel_index_count = channel_index_count;
    index->weight_index_count = weight_index_count;
    index->ready = true;
    return true;
}

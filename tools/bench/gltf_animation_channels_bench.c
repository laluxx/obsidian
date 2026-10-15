#define _POSIX_C_SOURCE 200809L
#include "animation_editor.h"
#include "context.h"
#include "gltf_loader.h"
#include "vulkan_setup.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

VulkanContext context;
AnimEditorState g_anim_editor;
mat4 *jointSSBOMapped[MAX_FRAMES_IN_FLIGHT];
Scene *scene;

void markMeshDirty(VulkanContext *ctx, uint32_t mesh_index) {
    (void)ctx;
    (void)mesh_index;
}

typedef struct {
    Scene scene_storage;
    GLTFInstance instance;
    OmdlSceneGraph graph;
    OmdlAnimation animation;
    OmdlNode *nodes;
    uint32_t *topology;
    mat4 *local;
    mat4 *world;
    OmdlChannel *channels;
    float *anim_floats;
    size_t node_count;
    size_t channel_count;
} AnimationFixture;

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static void fixture_destroy(AnimationFixture *fixture) {
    obs_animation_channel_index_destroy(
        &fixture->graph.animation_channel_index);
    free(fixture->anim_floats);
    free(fixture->channels);
    free(fixture->world);
    free(fixture->local);
    free(fixture->topology);
    free(fixture->nodes);
    *fixture = (AnimationFixture){0};
}

static bool fixture_init(AnimationFixture *fixture, size_t node_count,
                         size_t channel_count) {
    if (!fixture || !node_count || node_count > UINT32_MAX ||
        channel_count > UINT32_MAX / 8u || channel_count > SIZE_MAX / 8u ||
        channel_count * 8u > SIZE_MAX / sizeof(float) ||
        node_count > SIZE_MAX / sizeof(*fixture->nodes) ||
        node_count > SIZE_MAX / sizeof(*fixture->topology) ||
        node_count > SIZE_MAX / sizeof(*fixture->local) ||
        node_count > SIZE_MAX / sizeof(*fixture->world) ||
        channel_count > SIZE_MAX / sizeof(*fixture->channels))
        return false;

    fixture->node_count = node_count;
    fixture->channel_count = channel_count;
    fixture->nodes = calloc(node_count, sizeof(*fixture->nodes));
    fixture->topology = malloc(node_count * sizeof(*fixture->topology));
    fixture->local = malloc(node_count * sizeof(*fixture->local));
    fixture->world = malloc(node_count * sizeof(*fixture->world));
    fixture->channels = channel_count
                            ? calloc(channel_count, sizeof(*fixture->channels))
                            : NULL;
    fixture->anim_floats = channel_count
                               ? calloc(channel_count * 8u,
                                        sizeof(*fixture->anim_floats))
                               : NULL;
    if (!fixture->nodes || !fixture->topology || !fixture->local ||
        !fixture->world ||
        (channel_count && (!fixture->channels || !fixture->anim_floats))) {
        fixture_destroy(fixture);
        return false;
    }

    for (size_t n = 0; n < node_count; ++n) {
        OmdlNode *node = &fixture->nodes[n];
        node->parent = n ? (int32_t)((n - 1u) / 2u) : -1;
        node->mesh_idx = -1;
        node->skin_idx = -1;
        node->rotation[3] = 1.0f;
        node->scale[0] = node->scale[1] = node->scale[2] = 1.0f;
        glm_mat4_identity(node->base_matrix);
        fixture->topology[n] = (uint32_t)n;
    }
    for (size_t c = 0; c < channel_count; ++c) {
        const size_t offset = c * 8u;
        fixture->channels[c] = (OmdlChannel){
            .target_node = (uint32_t)(c % node_count),
            .path_type = cgltf_animation_path_type_translation,
            .keyframe_count = 2,
            .times_offset = (uint32_t)offset,
            .values_offset = (uint32_t)(offset + 2u),
        };
        fixture->anim_floats[offset] = 0.0f;
        fixture->anim_floats[offset + 1u] = 1.0f;
        fixture->anim_floats[offset + 5u] = 2.0f * (float)(c + 1u);
    }

    fixture->animation = (OmdlAnimation){
        .duration = 1.0f,
        .channel_count = (uint32_t)channel_count,
        .channel_offset = 0,
    };
    fixture->graph = (OmdlSceneGraph){
        .node_count = (uint32_t)node_count,
        .nodes = fixture->nodes,
        .local_transforms = fixture->local,
        .world_transforms = fixture->world,
        .topology_order = fixture->topology,
        .anim_count = channel_count ? 1u : 0u,
        .anims = channel_count ? &fixture->animation : NULL,
        .channels = fixture->channels,
        .anim_floats = fixture->anim_floats,
    };
    fixture->instance = (GLTFInstance){
        .gltf_data = &fixture->graph,
        .mesh_start_index = 0,
        .mesh_count = 0,
    };
    fixture->scene_storage = (Scene){
        .gltf_instances = &fixture->instance,
        .gltf_instance_count = 1,
    };
    return true;
}

static bool verify_sampled_translations(const AnimationFixture *fixture) {
    for (size_t n = 0; n < fixture->node_count; ++n) {
        size_t last_channel = SIZE_MAX;
        for (size_t c = 0; c < fixture->channel_count; ++c)
            if (fixture->channels[c].target_node == n)
                last_channel = c;
        const float expected = last_channel == SIZE_MAX
                                   ? 0.0f
                                   : (float)(last_channel + 1u);
        if (!isfinite(fixture->local[n][3][0]) ||
            fabsf(fixture->local[n][3][0] - expected) > 1e-4f)
            return false;
    }
    return true;
}

static uint64_t median(uint64_t values[5]) {
    for (size_t i = 1; i < 5; ++i) {
        const uint64_t value = values[i];
        size_t j = i;
        while (j && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
    return values[2];
}

static int run_case(size_t node_count, size_t channel_count, unsigned frames) {
    AnimationFixture fixture = {0};
    if (!fixture_init(&fixture, node_count, channel_count)) {
        fputs("animation benchmark: fixture allocation failed\n", stderr);
        return 1;
    }
    scene = &fixture.scene_storage;
    context.currentFrame = 0;
    g_anim_editor.time = 0.5f;

    const uint64_t first_call_start = monotonic_ns();
    animate_scene(scene, 0.0f);
    const uint64_t first_call_ns = monotonic_ns() - first_call_start;
    for (unsigned i = 1; i < 4; ++i)
        animate_scene(scene, 0.0f);
    if (!verify_sampled_translations(&fixture)) {
        fprintf(stderr, "animation benchmark: sample mismatch nodes=%zu channels=%zu\n",
                node_count, channel_count);
        fixture_destroy(&fixture);
        return 1;
    }

    uint64_t samples[5];
    for (size_t repeat = 0; repeat < 5; ++repeat) {
        const uint64_t start = monotonic_ns();
        for (unsigned frame = 0; frame < frames; ++frame)
            animate_scene(scene, 0.0f);
        samples[repeat] = (monotonic_ns() - start) / frames;
    }
    const uint64_t ns_per_frame = median(samples);
    printf("nodes=%zu channels=%zu legacy-pair-checks/frame=%zu "
           "median=%" PRIu64 " ns/frame (%u frames x 5 samples) "
           "first_call=%" PRIu64 " ns\n",
           node_count, channel_count, node_count * channel_count,
           ns_per_frame, frames, first_call_ns);
    fixture_destroy(&fixture);
    scene = NULL;
    return 0;
}

int main(int argc, char **argv) {
    unsigned frames = 16;
    if (argc > 2) {
        fprintf(stderr, "usage: %s [frames: 1..4096]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        char *end = NULL;
        const unsigned long parsed = strtoul(argv[1], &end, 10);
        if (!end || *end || parsed < 1 || parsed > 4096) {
            fprintf(stderr, "usage: %s [frames: 1..4096]\n", argv[0]);
            return 2;
        }
        frames = (unsigned)parsed;
    }

    static const struct {
        size_t nodes;
        size_t channels;
    } cases[] = {
        {512, 0},
        {512, 128},
        {512, 512},
        {1024, 1024},
        {2048, 2048},
    };
    puts("Production animate_scene CPU benchmark; includes local sampling and hierarchy, no GPU/FFI");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        if (run_case(cases[i].nodes, cases[i].channels, frames))
            return 1;
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include "skin_override_index.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define REPEATS 5u

static volatile uint64_t sink;

static uint64_t now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static __attribute__((noinline)) uint64_t nested_scan(
                            const ObsSkinJointRef *source,
                            const uint8_t *mesh_has_skin, size_t node_count,
                            size_t mesh_count,
                            size_t joints_per_mesh,
                            unsigned frames) {
    uint64_t checksum = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        uint64_t frame_sum = 0;
        for (size_t node = 0; node < node_count; ++node) {
            for (size_t mesh = 0; mesh < mesh_count; ++mesh) {
                if (!mesh_has_skin[mesh])
                    continue;
                const size_t start = mesh * joints_per_mesh;
                for (size_t joint = 0; joint < joints_per_mesh; ++joint) {
                    if (source[start + joint].node_index != node)
                        continue;
                    frame_sum += mesh + joint + node;
                    break;
                }
            }
        }
        sink = frame_sum;
        checksum += frame_sum;
    }
    return checksum;
}

static __attribute__((noinline)) uint64_t indexed_scan(
                             const ObsSkinOverrideIndex *index,
                             size_t node_count, unsigned frames) {
    uint64_t checksum = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        uint64_t frame_sum = 0;
        for (size_t node = 0; node < node_count; ++node) {
            for (size_t ref = index->offsets[node];
                 ref < index->offsets[node + 1]; ++ref)
                frame_sum += index->refs[ref].mesh_index +
                             index->refs[ref].joint_index + node;
        }
        sink = frame_sum;
        checksum += frame_sum;
    }
    return checksum;
}

static int run_case(size_t node_count, size_t mesh_count,
                    size_t active_mesh_count, size_t joints_per_mesh,
                    unsigned frames) {
    if (active_mesh_count > mesh_count ||
        (joints_per_mesh && active_mesh_count > SIZE_MAX / joints_per_mesh))
        return 1;
    const size_t ref_count = active_mesh_count * joints_per_mesh;
    if (ref_count > SIZE_MAX / sizeof(ObsSkinJointRef))
        return 1;
    ObsSkinJointRef *source =
        ref_count ? malloc(ref_count * sizeof(*source)) : NULL;
    uint8_t *mesh_has_skin = calloc(mesh_count, sizeof(*mesh_has_skin));
    if ((ref_count && !source) || !mesh_has_skin) {
        free(source);
        free(mesh_has_skin);
        return 1;
    }
    for (size_t mesh = 0; mesh < active_mesh_count; ++mesh)
        mesh_has_skin[mesh] = 1;
    for (size_t mesh = 0; mesh < active_mesh_count; ++mesh) {
        for (size_t joint = 0; joint < joints_per_mesh; ++joint) {
            source[mesh * joints_per_mesh + joint] = (ObsSkinJointRef){
                .node_index = (uint32_t)((mesh * 31u + joint) % node_count),
                .mesh_index = (uint32_t)mesh,
                .joint_index = (uint32_t)joint,
            };
        }
    }

    ObsSkinOverrideIndex index = {0};
    const uint64_t build_start = now_ns();
    const bool built = obs_skin_override_index_build(
        node_count, mesh_count, source, ref_count, &index);
    const uint64_t build_ns = now_ns() - build_start;
    if (!built) {
        free(source);
        free(mesh_has_skin);
        return 1;
    }

    uint64_t old_times[REPEATS], new_times[REPEATS];
    uint64_t expected = 0;
    for (unsigned sample = 0; sample < REPEATS; ++sample) {
        uint64_t begin = now_ns();
        const uint64_t old_sum = nested_scan(source, mesh_has_skin,
                                             node_count, mesh_count,
                                             joints_per_mesh,
                                             frames);
        old_times[sample] = now_ns() - begin;
        begin = now_ns();
        const uint64_t new_sum = indexed_scan(&index, node_count, frames);
        new_times[sample] = now_ns() - begin;
        if (old_sum != new_sum) {
            fprintf(stderr, "skin override benchmark checksum mismatch\n");
            obs_skin_override_index_destroy(&index);
            free(source);
            free(mesh_has_skin);
            return 1;
        }
        expected = old_sum;
    }
    qsort(old_times, REPEATS, sizeof(old_times[0]), compare_u64);
    qsort(new_times, REPEATS, sizeof(new_times[0]), compare_u64);
    const double old_per_frame = (double)old_times[REPEATS / 2] / frames;
    const double new_per_frame = (double)new_times[REPEATS / 2] / frames;
    printf("skin overrides nodes=%zu meshes=%zu active-meshes=%zu "
           "joints/mesh=%zu refs=%zu frames=%u: "
           "nested %.2f us/frame, indexed %.2f us/frame, "
           "speedup %.2fx, build %.2f us, checksum=%" PRIu64 "\n",
           node_count, mesh_count, active_mesh_count, joints_per_mesh,
           ref_count, frames,
           old_per_frame / 1000.0, new_per_frame / 1000.0,
           old_per_frame / new_per_frame, build_ns / 1000.0, expected);
    obs_skin_override_index_destroy(&index);
    free(source);
    free(mesh_has_skin);
    return 0;
}

int main(int argc, char **argv) {
    const unsigned frames =
        argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 4u;
    if (!frames || frames > 4096u) {
        fputs("usage: skin-override-index-bench [frames: 1..4096]\n", stderr);
        return 2;
    }
    if (run_case(1024, 256, 0, 0, frames) ||
        run_case(1024, 256, 32, 32, frames) ||
        run_case(2048, 128, 128, 64, frames))
        return 1;
    return 0;
}

/* sdf_undo_bench.c — benchmark the production SDF inspector journal. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_inspector.h"
#include "sdf_sculpt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static Scene scene_storage = {
    .sdf_lifetime_epoch = 1,
    .tree = {.count = 1},
};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];

void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }

int32_t scene_tree_add_node(SceneTree *tree, const char *name,
                            int32_t parent, int32_t mesh_index) {
    if (!tree || !name || parent < 0 || mesh_index != -1 ||
        tree->count >= SCENE_TREE_MAX_NODES)
        return -1;
    int32_t index = tree->count++;
    tree->nodes[index].sdf_index = -1;
    return index;
}

static int fail(const char *operation, unsigned objects,
                ObsSdfEditStatus status) {
    fprintf(stderr, "sdf undo benchmark: %s(%u) failed: status=%d\n",
            operation, objects, status);
    return 1;
}

static uint64_t now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static int prepare(unsigned objects, uint64_t tickets[OBS_SDF_EDIT_HISTORY_CAP]) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;

    for (unsigned i = 0; i < objects; ++i) {
        if (!obs_sculpt_sphere((double[]){20.0 * i, 0, 0}, .5,
                               (double[]){.3, .5, .7}, &tickets[i]))
            return fail("create sphere", objects, OBS_SDF_EDIT_INVALID);
    }
    if (obs_sdf_inspector_begin_group() != OBS_SDF_EDIT_OK)
        return fail("begin group", objects, OBS_SDF_EDIT_INVALID);
    for (unsigned i = 0; i < objects; ++i) {
        ObsSdfEditStatus status = obs_sdf_inspector_select(tickets[i]);
        if (status != OBS_SDF_EDIT_OK)
            return fail("select", objects, status);
        status = obs_sdf_inspector_move((double[]){20.0 * i + 1, 0, 0});
        if (status != OBS_SDF_EDIT_OK)
            return fail("move", objects, status);
    }
    ObsSdfEditStatus status = obs_sdf_inspector_end_group();
    if (status != OBS_SDF_EDIT_OK)
        return fail("commit group", objects, status);
    return 0;
}

static int verify(unsigned objects,
                  const uint64_t tickets[OBS_SDF_EDIT_HISTORY_CAP],
                  double offset, uint32_t undo_count, uint32_t redo_count) {
    for (unsigned i = 0; i < objects; ++i) {
        double actual = 0;
        const double expected = 20.0 * i + offset;
        if (!obs_sculpt_x(tickets[i], &actual) || actual != expected) {
            fprintf(stderr, "sdf undo benchmark: scene mismatch object=%u "
                            "expected=%.17g actual=%.17g\n",
                    i, expected, actual);
            return 1;
        }
    }
    ObsSdfInspectorState state = obs_sdf_inspector_state();
    if (state.undo_count != undo_count || state.redo_count != redo_count) {
        fprintf(stderr, "sdf undo benchmark: history mismatch undo=%u redo=%u "
                        "expected=%u/%u\n",
                state.undo_count, state.redo_count, undo_count, redo_count);
        return 1;
    }
    return 0;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void report(const char *operation, unsigned objects, unsigned samples,
                   uint64_t values[4096]) {
    qsort(values, samples, sizeof(values[0]), compare_u64);
    printf("%-4s group=%2u samples=%4u p50=%" PRIu64
           " ns p95=%" PRIu64 " ns max=%" PRIu64 " ns\n",
           operation, objects, samples, values[samples / 2],
           values[(samples * 95) / 100], values[samples - 1]);
}

int main(int argc, char **argv) {
    unsigned samples = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1500;
    if (!samples || samples > 4096) {
        fputs("usage: sdf-undo-bench [samples: 1..4096]\n", stderr);
        return 2;
    }
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];

    static const unsigned sizes[] = {1, 16, 64};
    static uint64_t undo_samples[4096], redo_samples[4096];
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const unsigned objects = sizes[s];
        uint64_t tickets[OBS_SDF_EDIT_HISTORY_CAP] = {0};
        if (prepare(objects, tickets) != 0)
            return 1;
        for (unsigned i = 0; i < samples; ++i) {
            uint64_t start = now_ns();
            ObsSdfEditStatus status = obs_sdf_inspector_undo();
            uint64_t undo_end = now_ns();
            if (status != OBS_SDF_EDIT_OK)
                return fail("undo", objects, status);
            if (verify(objects, tickets, 0, 0, 1) != 0)
                return 1;
            uint64_t redo_start = now_ns();
            status = obs_sdf_inspector_redo();
            uint64_t redo_end = now_ns();
            if (status != OBS_SDF_EDIT_OK)
                return fail("redo", objects, status);
            if (verify(objects, tickets, 1, 1, 0) != 0)
                return 1;
            undo_samples[i] = undo_end - start;
            redo_samples[i] = redo_end - redo_start;
        }
        report("undo", objects, samples, undo_samples);
        report("redo", objects, samples, redo_samples);
    }
    free(scene->sdfs);
    return 0;
}

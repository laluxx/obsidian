/* Cycle 67: deep inverse-chain invariants through the production scene journal.
 * 72,000 deterministic mixed commands exercise Emacs-like undo, undo-only,
 * explicit redo, object-scoped inversion and command boundaries. The oracle
 * captures scene positions independently, not from the journal's records.
 */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];
void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name,
                            int32_t parent, int32_t mesh_index) {
    assert(name && parent >= 0 && mesh_index == -1);
    if (tree->count >= SCENE_TREE_MAX_NODES) return -1;
    int32_t index = tree->count++;
    tree->nodes[index].sdf_index = -1;
    return index;
}

static uint64_t tickets[64];
static uint64_t random_state = UINT64_C(0x0bad5eed1092);
static uint32_t next_random(void) {
    uint64_t x = random_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    random_state = x;
    return (uint32_t)((x * UINT64_C(2685821657736338717)) >> 32);
}
static void fresh_scene(unsigned count) {
    assert(count <= 64);
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    for (unsigned i = 0; i < count; ++i)
        assert(obs_sculpt_sphere((double[]){20.0 * i, 0, 0}, .5,
                                 (double[]){.3, .5, .7}, &tickets[i]));
}
static double x_position(unsigned i) {
    double x = 0;
    assert(obs_sculpt_x(tickets[i], &x));
    return x;
}
static void snapshot_scene(unsigned count, double *values) {
    for (unsigned i = 0; i < count; ++i) values[i] = x_position(i);
}
static void check_scene(unsigned count, const double *expected, const char *phase,
                        unsigned round, unsigned iter) {
    for (unsigned i = 0; i < count; ++i) {
        double actual = x_position(i);
        if (actual != expected[i]) {
            fprintf(stderr,
                    "Cycle67 %s: round=%u iter=%u ticket=%u expected=%.9g "
                    "got=%.9g seed=%" PRIu64 "\n",
                    phase, round, iter, i, expected[i], actual, random_state);
            abort();
        }
    }
}
static void edit(unsigned i, double x) {
    assert(obs_sdf_inspector_select(tickets[i]) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_move((double[]){x, 0, 0}) == OBS_SDF_EDIT_OK);
}
static void stress(void) {
    for (unsigned round = 0; round < 80; ++round) {
        const unsigned count = round % 3 == 0 ? 64 : round % 3 == 1 ? 4 : 1;
        fresh_scene(count);
        for (unsigned iter = 0; iter < 900; ++iter) {
            const uint32_t action = next_random() % 15;
            const unsigned chosen = next_random() % count;
            double before[64], inverted[64];
            snapshot_scene(count, before);
            ObsSdfEditStatus status = OBS_SDF_EDIT_EMPTY;
            if (action < 7) {
                edit(chosen, chosen * 20.0 + 1 + next_random() % 500);
                continue;
            }
            if (action == 7) {
                assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
                unsigned edits = 1 + next_random() % count;
                for (unsigned j = 0; j < edits; ++j) {
                    unsigned i = next_random() % count;
                    edit(i, i * 20.0 + 1 + next_random() % 500);
                }
                assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
                continue;
            }
            if (action == 8) {
                obs_sdf_inspector_command_boundary();
                continue;
            }
            if (action == 9) {
                assert(obs_sdf_inspector_select(tickets[chosen]) == OBS_SDF_EDIT_OK);
                status = obs_sdf_inspector_undo_selected();
            } else if (action == 10 || action == 11) {
                status = obs_sdf_inspector_undo();
            } else if (action == 12) {
                status = obs_sdf_inspector_undo_only();
            } else if (action == 13) {
                (void)obs_sdf_inspector_redo();
                continue;
            } else {
                assert(obs_sdf_inspector_clear() == OBS_SDF_EDIT_OK);
                check_scene(count, before, "clear did not mutate", round, iter);
                continue;
            }
            if (status != OBS_SDF_EDIT_OK) {
                assert(status == OBS_SDF_EDIT_CONFLICT ||
                       status == OBS_SDF_EDIT_STALE ||
                       status == OBS_SDF_EDIT_EMPTY ||
                       status == OBS_SDF_EDIT_INVALID);
                check_scene(count, before, "failed undo atomic", round, iter);
                continue;
            }
            snapshot_scene(count, inverted);
            ObsSdfEditStatus redo = obs_sdf_inspector_redo();
            if (redo != OBS_SDF_EDIT_OK) {
                fprintf(stderr, "Cycle67 redo rejected=%d round=%u iter=%u "
                                "action=%u seed=%" PRIu64 "\n",
                        redo, round, iter, action, random_state);
                abort();
            }
            check_scene(count, before, "undo-redo", round, iter);
            /* A selected-object undo can skip newer independent commands.
             * Its subsequent global undo-only may correctly target one of
             * those newer commands, so only chronological undo is required
             * to reproduce the same inverse twice. */
            if (action != 9 && obs_sdf_inspector_undo_only() == OBS_SDF_EDIT_OK) {
                check_scene(count, inverted, "undo-redo-undo", round, iter);
                assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
                check_scene(count, before, "undo-redo-undo-redo", round, iter);
            }
            if (iter % 37 == 0) {
                unsigned slot = iter % MAX_FRAMES_IN_FLIGHT;
                assert(obs_sdf_sync_frame(slot));
                for (unsigned i = 0; i < count; ++i)
                    assert(gpu[slot][i].inverseTransform[3][0] ==
                           scene->sdfs[i].inverseTransform[3][0]);
            }
        }
    }
}
int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];
    stress();
    free(scene->sdfs);
    puts("Cycle 67 deep undo: 72,000 mixed commands and inverse rounds PASS");
    return 0;
}

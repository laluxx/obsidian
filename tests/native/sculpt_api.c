#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sculpt_api_contract.h"
#include "sdf_appearance_clipboard.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static ObsMonadRuntime runtime = {.active = 1};
static monad_object_space_t space = {.active = 1};
static ObsHostBinding host[24];
static size_t n_host;
static monad_value_t made;
static bool owner = true, effect = true;
void scene_bump_sdf_revision(Scene *s) { s->sdf_revision++; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name, int32_t parent,
                            int32_t mesh_index) {
    assert(name && parent >= 0 && mesh_index == -1);
    if (tree->count >= SCENE_TREE_MAX_NODES) return -1;
    return tree->count++;
}
bool obs_host_owner(const ObsMonadRuntime *h) { return owner && h == &runtime; }
bool obs_monad_effect_allowed(unsigned effects, unsigned lifetime, bool frame) {
    assert(effects == 6 && lifetime == 2 && !frame);
    return effect;
}
monad_status_t obs_host_invalid(monad_error_t *e, const char *message) {
    if (e) {e->status = 1; e->message = message;}
    return 1;
}
bool obs_host_register(ObsMonadRuntime *h, const ObsHostBinding *entries,
                       size_t count, monad_error_t *e) {
    (void)e;
    assert(h == &runtime && count <= 24);
    memcpy(host, entries, count * sizeof(host[0])); n_host = count;
    return true;
}
monad_status_t monad_value_as_f64(monad_value_t *v, double *out, monad_error_t *e) {
    if (!v || v->kind != 2) return obs_host_invalid(e, "not Float");
    *out = v->number; return MONAD_OK;
}
monad_status_t monad_value_as_i64(monad_value_t *v, int64_t *out, monad_error_t *e) {
    if (!v || v->kind != 1) return obs_host_invalid(e, "not Int");
    *out = v->integer; return MONAD_OK;
}
monad_status_t monad_value_make_i64(monad_object_space_t *sp, int64_t n,
                                    monad_value_t **out, monad_error_t *e) {
    (void)e; assert(sp == &space); made = (monad_value_t){.kind=1,.integer=n}; *out = &made; return MONAD_OK;
}
monad_status_t monad_value_make_f64(monad_object_space_t *sp, double n,
                                    monad_value_t **out, monad_error_t *e) {
    (void)e; assert(sp == &space); made = (monad_value_t){.kind=2,.number=n}; *out = &made; return MONAD_OK;
}
monad_value_t *monad_value_nil(monad_object_space_t *sp) {
    assert(sp == &space); static monad_value_t nil; return &nil;
}
bool obs_monad_register_sculpt_api(ObsMonadRuntime *, monad_error_t *);
static monad_status_t call(const char *name, size_t count, monad_value_t *const *args,
                            monad_value_t **ret, monad_error_t *e) {
    for (size_t i = 0; i < n_host; ++i)
        if (strcmp(host[i].name, name) == 0)
            return host[i].callback(&space, count, args, ret, &runtime, e);
    abort();
}
int main(void) {
    monad_error_t error = {0};
    assert(obs_monad_register_sculpt_api(&runtime, &error));
    assert(n_host == 22);
    monad_value_t inputs[7]; monad_value_t *a[7];
    const double raw[] = {1., 2., -3., 0.5, 0.8, 0.1, 0.2};
    for (int i = 0; i < 7; ++i) {
        inputs[i] = (monad_value_t){.kind=2,.number=raw[i]}; a[i] = &inputs[i];
    }
    monad_value_t *out = NULL;
    effect = false;
    assert(call("sdf-sphere!", 7, a, &out, &error) != MONAD_OK);
    assert(scene->sdf_count == 0);
    effect = true;
    assert(call("sdf-sphere!", 7, a, &out, &error) == MONAD_OK);
    int64_t id = out->integer;
    assert(id > 0 && scene->sdf_count == 1);
    monad_value_t handle = {.kind=1,.integer=id};
    monad_value_t zero = {.kind=1,.integer=0};
    monad_value_t *at[] = {&zero};
    assert(call("sdf-ticket", 1, at, &out, &error) == MONAD_OK && out->integer == id);
    monad_value_t *read[] = {&handle};
    assert(call("sdf-x", 1, read, &out, &error) == MONAD_OK && out->number == 1.0);
    monad_value_t coords[3] = {{.kind=2,.number=4},{.kind=2,.number=0},{.kind=2,.number=2}};
    monad_value_t *move[] = {&handle,&coords[0],&coords[1],&coords[2]};
    assert(call("sdf-move!", 4, move, &out, &error) == MONAD_OK);
    assert(call("sdf-x", 1, read, &out, &error) == MONAD_OK && out->number == 4.0);
    coords[0].number = NAN;
    assert(call("sdf-move!", 4, move, &out, &error) != MONAD_OK);
    /* Read-only language picking returns the very same ticket. */
    monad_value_t pick_inputs[7], *pick_args[7];
    const double ray[] = {4, 0, -3, 0, 0, 2, 20};
    for (int i = 0; i < 7; ++i) {
        pick_inputs[i] = (monad_value_t){.kind=2,.number=ray[i]};
        pick_args[i] = &pick_inputs[i];
    }
    effect = false; /* No mutation effect required for read-only picking. */
    assert(call("sdf-raycast", 7, pick_args, &out, &error) == MONAD_OK &&
           out->integer == id);
    pick_inputs[0].number = 50;
    assert(call("sdf-raycast", 7, pick_args, &out, &error) == MONAD_OK &&
           out->integer == 0);
    pick_inputs[3].number = 0; pick_inputs[4].number = 0;
    pick_inputs[5].number = 0;
    assert(call("sdf-raycast", 7, pick_args, &out, &error) != MONAD_OK);
    effect = true;
    monad_value_t *select_args[] = {&handle};
    effect = false;
    assert(call("sdf-inspect-select!", 1, select_args, &out, &error) != MONAD_OK);
    effect = true;
    assert(call("sdf-inspect-select!", 1, select_args, &out, &error) == MONAD_OK);
    assert(call("sdf-inspect-selected", 0, NULL, &out, &error) == MONAD_OK && out->integer == id);
    effect = false;
    assert(call("sdf-inspect-undo-count", 0, NULL, &out, &error) == MONAD_OK && out->integer == 0);
    assert(call("sdf-inspect-tint!", 3, move + 1, &out, &error) != MONAD_OK);
    effect = true;
    monad_value_t crimson[3] = {{.kind=2,.number=0.5},
                                 {.kind=2,.number=0.2}, {.kind=2,.number=0.3}};
    monad_value_t *tint_args[] = {&crimson[0],&crimson[1],&crimson[2]};
    assert(call("sdf-inspect-tint!", 3, tint_args, &out, &error) == MONAD_OK);
    assert(fabs(scene->sdfs[0].color[0] - 0.5f) < 1e-6f);
    assert(call("sdf-inspect-undo-count", 0, NULL, &out, &error) == MONAD_OK && out->integer == 1);
    assert(call("sdf-inspect-undo!", 0, NULL, &out, &error) == MONAD_OK);
    assert(call("sdf-inspect-redo-count", 0, NULL, &out, &error) == MONAD_OK && out->integer == 1);
    assert(call("sdf-inspect-redo!", 0, NULL, &out, &error) == MONAD_OK);
    assert(fabs(scene->sdfs[0].color[0] - 0.5f) < 1e-6f);
    effect = false;
    assert(call("sdf-inspect-undo-selected!", 0, NULL, &out, &error) != MONAD_OK);
    effect = true;
    assert(call("sdf-inspect-undo-selected!", 0, NULL, &out, &error) == MONAD_OK);
    assert(fabs(scene->sdfs[0].color[0] - 0.8f) < 1e-6f);
    assert(call("sdf-inspect-redo!", 0, NULL, &out, &error) == MONAD_OK);
    assert(fabs(scene->sdfs[0].color[0] - 0.5f) < 1e-6f);
    /* A command break followed by undo-only never redoes an undo. */
    assert(call("sdf-inspect-undo!", 0, NULL, &out, &error) == MONAD_OK);
    assert(call("sdf-inspect-undo-boundary!", 0, NULL, &out, &error) == MONAD_OK);
    assert(call("sdf-inspect-undo-only!", 0, NULL, &out, &error) != MONAD_OK);
    assert(call("sdf-inspect-redo!", 0, NULL, &out, &error) == MONAD_OK);
    assert(call("sdf-inspect-begin-group!", 0, NULL, &out, &error) == MONAD_OK);
    assert(call("sdf-inspect-begin-group!", 0, NULL, &out, &error) != MONAD_OK);
    assert(call("sdf-inspect-end-group!", 0, NULL, &out, &error) == MONAD_OK);
    /* Cancelled language-owned live preview cannot pollute undo history. */
    assert(call("sdf-inspect-undo-count", 0, NULL, &out, &error) == MONAD_OK);
    int64_t history_before = out->integer;
    assert(call("sdf-inspect-begin-group!", 0, NULL, &out, &error) == MONAD_OK);
    coords[0].number = 12; coords[1].number = 0; coords[2].number = 2;
    monad_value_t *preview_args[] = {&coords[0],&coords[1],&coords[2]};
    assert(call("sdf-inspect-move!", 3, preview_args, &out, &error) == MONAD_OK);
    assert(call("sdf-x", 1, read, &out, &error) == MONAD_OK && out->number == 12);
    assert(call("sdf-inspect-undo-count", 0, NULL, &out, &error) == MONAD_OK &&
           out->integer == history_before);
    effect = false;
    assert(call("sdf-inspect-abort-group!", 0, NULL, &out, &error) != MONAD_OK);
    effect = true;
    assert(call("sdf-inspect-abort-group!", 0, NULL, &out, &error) == MONAD_OK);
    assert(call("sdf-x", 1, read, &out, &error) == MONAD_OK && out->number == 4);
    assert(call("sdf-inspect-abort-group!", 0, NULL, &out, &error) != MONAD_OK);
    assert(call("sdf-inspect-undo-count", 0, NULL, &out, &error) == MONAD_OK &&
           out->integer == history_before);
    /* Monad callbacks share the exact editor clipboard and enforce effects. */
    assert(obs_sdf_inspector_clear() == OBS_SDF_EDIT_OK);
    effect = false;
    assert(call("sdf-appearance-copy!", 1, at, &out, &error) != MONAD_OK);
    effect = true;
    assert(call("sdf-appearance-copy!", 1, at, &out, &error) == MONAD_OK);
    scene->sdfs[0].color[0] = 0.1f;
    effect = false;
    assert(call("sdf-appearance-paste!", 1, at, &out, &error) != MONAD_OK);
    effect = true;
    assert(call("sdf-appearance-paste!", 1, at, &out, &error) == MONAD_OK);
    assert(scene->sdfs[0].color[0] == 0.5f);
    assert(call("sdf-inspect-undo!", 0, NULL, &out, &error) == MONAD_OK);
    assert(scene->sdfs[0].color[0] == 0.1f);
    monad_value_t wrong_index = {.kind=1,.integer=-1};
    monad_value_t *bad[] = {&wrong_index};
    assert(call("sdf-appearance-copy!", 1, bad, &out, &error) != MONAD_OK);
    scene->sdf_lifetime_epoch++;
    assert(call("sdf-x", 1, read, &out, &error) != MONAD_OK);
    free(scene->sdfs);
    puts("Cycle 61 real Monad host bindings, scene appearance transfer and undo PASS");
    return 0;
}

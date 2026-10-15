/* Cycle 61 — production scene, journal, bridge and clipboard with mapped GPU
 * slot fixtures. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_appearance_clipboard.h"
#include "sdf_property_bridge.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];

static uint32_t create(int type, float x) {
    vec3 at = {x, 0, 0};
    vec4 shape = {type == SDF_TYPE_TERRAIN ? .5f : 1, 1, 2, 3};
    vec4 color = {.25f, .5f, .75f, 1};
    int32_t idx = obs_sdf_add_checked(scene, "Clipboard", type, at, shape, color);
    assert(idx >= 0);
    if (type == SDF_TYPE_TERRAIN) scene->sdfs[idx]._pad1 = 8;
    return (uint32_t)idx;
}
void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }
int32_t scene_tree_add_node(SceneTree *t, const char *name,
                            int32_t parent, int32_t mesh_index) {
    assert(name && parent >= 0 && mesh_index == -1);
    if (t->count >= SCENE_TREE_MAX_NODES) return -1;
    int32_t idx = t->count++;
    t->nodes[idx].sdf_index = -1;
    return idx;
}
static void read_appearance(uint32_t index, ObsSdfEditValues *out) {
    const uint64_t ticket = obs_sculpt_ticket_any_at(index);
    assert(ticket != 0);
    assert(obs_sdf_inspector_read_appearance(ticket, out) == OBS_SDF_EDIT_OK);
}
static void assert_appearance(uint32_t index, const ObsSdfEditValues *expected) {
    ObsSdfEditValues v;
    read_appearance(index, &v);
    for (unsigned i = 0; i < 4; ++i) assert(v.rgba[i] == expected->rgba[i]);
    for (unsigned i = 0; i < 3; ++i) assert(v.emissive[i] == expected->emissive[i]);
    assert(v.roughness == expected->roughness && v.metallic == expected->metallic);
    assert(v.emissive_strength == expected->emissive_strength);
}
static void change(uint32_t index, double red, double green, double strength) {
    ObsSdfEditValues newv = {.rgba = {red, green, .4, 1},
                             .roughness = .125, .metallic = .875,
                             .emissive = {.1, .2, .3},
                             .emissive_strength = strength};
    assert(obs_sdf_property_apply(index,
        OBS_SDF_EDIT_MATERIAL | OBS_SDF_EDIT_EMISSION, &newv) == OBS_SDF_EDIT_OK);
}
static void reset(void) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
}
static uint32_t test_crc32(const unsigned char *data, size_t len) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static void test_put32(unsigned char *p, uint32_t n) {
    for (unsigned j = 0; j < 4; ++j) p[j] = (unsigned char)(n >> (8u*j));
}
static void rewrite(const char *path, const unsigned char *data, size_t n) {
    FILE *fp = fopen(path, "wb");
    assert(fp && fwrite(data, 1, n, fp) == n && fclose(fp) == 0);
}
static void preset_roundtrip(uint32_t source) {
    char dir[] = "/tmp/obs-look-cycle62-XXXXXX";
    assert(mkdtemp(dir));
    char filename[256], missing[256], linkname[256];
    assert(snprintf(filename, sizeof(filename), "%s/look.obslook", dir) > 0);
    assert(snprintf(missing, sizeof(missing), "%s/absent/look", dir) > 0);
    assert(snprintf(linkname, sizeof(linkname), "%s/link", dir) > 0);
    assert(setenv("OBSIDIAN_SDF_LOOK_PATH", filename, 1) == 0);
    assert(strcmp(obs_sdf_appearance_preset_path(), filename) == 0);
    obs_sdf_appearance_clear();
    assert(!obs_sdf_appearance_save_file(filename));
    assert(!obs_sdf_appearance_load_file(filename));
    assert(obs_sdf_appearance_copy(source) == OBS_SDF_EDIT_OK);
    ObsSdfEditValues expected;
    read_appearance(source, &expected);
    assert(!obs_sdf_appearance_save_file(missing));
    assert(obs_sdf_appearance_save_file(filename));
    unsigned char good[100];
    FILE *fp = fopen(filename, "rb");
    assert(fp && fread(good, 1, sizeof(good), fp) == sizeof(good));
    assert(fgetc(fp) == EOF && fclose(fp) == 0);
    assert(memcmp(good, "OBSLOOK1", 8) == 0 && good[8] == 1 && good[12] == 80);
    const uint32_t crc = test_crc32(good, 96);
    assert(good[96] == (unsigned char)crc &&
           good[97] == (unsigned char)(crc >> 8));
    const uint32_t target = create(SDF_TYPE_TERRAIN, 9);
    const SdfPrimitive shape = scene->sdfs[target];
    ObsSdfEditValues before;
    read_appearance(target, &before);
    obs_sdf_appearance_clear();
    assert(obs_sdf_appearance_load_file(filename));
    assert(obs_sdf_appearance_paste(target) == OBS_SDF_EDIT_OK);
    assert_appearance(target, &expected);
    assert(scene->sdfs[target].type == shape.type);
    assert(memcmp(shape.size, scene->sdfs[target].size, sizeof(shape.size)) == 0);
    assert(scene->sdfs[target]._pad1 == shape._pad1);
    assert(obs_sdf_sync_frame(0));
    assert(gpu[0][target].color[0] == (float)expected.rgba[0]);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert_appearance(target, &before);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert_appearance(target, &expected);

    /* Existing good clipboard must survive every failed file replacement. */
    change(source, .43, .38, 7);
    ObsSdfEditValues survivor;
    read_appearance(source, &survivor);
    assert(obs_sdf_appearance_copy(source) == OBS_SDF_EDIT_OK);
    unsigned char bad[100];
    memcpy(bad, good, sizeof(bad));
    bad[32] ^= 0x80;
    rewrite(filename, bad, sizeof(bad));
    assert(!obs_sdf_appearance_load_file(filename));
    memcpy(bad, good, sizeof(bad)); bad[8] = 2;
    rewrite(filename, bad, sizeof(bad));
    assert(!obs_sdf_appearance_load_file(filename));
    memcpy(bad, good, sizeof(bad)); bad[12] = 79;
    rewrite(filename, bad, sizeof(bad));
    assert(!obs_sdf_appearance_load_file(filename));
    rewrite(filename, good, 99);
    assert(!obs_sdf_appearance_load_file(filename));
    unsigned char oversized[101];
    memcpy(oversized, good, sizeof(good)); oversized[100] = 0;
    rewrite(filename, oversized, sizeof(oversized));
    assert(!obs_sdf_appearance_load_file(filename));
    memcpy(bad, good, sizeof(bad));
    memset(bad + 16, 0xff, 8); /* NaN */
    test_put32(bad + 96, test_crc32(bad, 96));
    rewrite(filename, bad, sizeof(bad));
    assert(!obs_sdf_appearance_load_file(filename));
    memcpy(bad, good, sizeof(bad));
    memset(bad + 16, 0, 8);
    bad[22] = 0xf0; bad[23] = 0x3f; /* 1.0 */
    bad[16] = 1; /* next representable binary64 above 1.0 */
    test_put32(bad + 96, test_crc32(bad, 96));
    rewrite(filename, bad, sizeof(bad));
    assert(!obs_sdf_appearance_load_file(filename));
    assert(symlink(filename, linkname) == 0);
    assert(!obs_sdf_appearance_load_file(linkname));
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    assert(!obs_sdf_appearance_save_file(filename));
    rewrite(filename, good, sizeof(good));
    assert(!obs_sdf_appearance_load_file(filename));
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_appearance_paste(target) == OBS_SDF_EDIT_OK);
    assert_appearance(target, &survivor);
    assert(obs_sdf_appearance_load_file(filename));
    assert(obs_sdf_appearance_paste(target) == OBS_SDF_EDIT_OK);
    assert_appearance(target, &expected);
    assert(unlink(linkname) == 0 && unlink(filename) == 0 && rmdir(dir) == 0);
    assert(unsetenv("OBSIDIAN_SDF_LOOK_PATH") == 0);
    assert(strcmp(obs_sdf_appearance_preset_path(),
                  "obsidian-appearance.obslook") == 0);
}
int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) sdfSSBOMapped[i] = gpu[i];
    const uint32_t sphere = create(SDF_TYPE_SPHERE, -2);
    const uint32_t box = create(SDF_TYPE_BOX, 2);
    const uint32_t terrain = create(SDF_TYPE_TERRAIN, 5);
    obs_sdf_appearance_clear();
    assert(!obs_sdf_appearance_ready());
    assert(obs_sdf_appearance_paste(box) == OBS_SDF_EDIT_EMPTY);

    change(sphere, .75, .25, 8);
    ObsSdfEditValues source = {0}, box_before = {0};
    read_appearance(sphere, &source);
    read_appearance(box, &box_before);
    const SdfPrimitive box_geometry = scene->sdfs[box];
    assert(obs_sdf_appearance_copy(sphere) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_appearance_ready());
    assert(obs_sdf_appearance_copy(UINT32_MAX) == OBS_SDF_EDIT_STALE);
    assert(obs_sdf_appearance_paste(UINT32_MAX) == OBS_SDF_EDIT_STALE);
    change(sphere, .12, .32, 2);
    assert(obs_sdf_appearance_paste(box) == OBS_SDF_EDIT_OK);
    assert_appearance(box, &source);
    assert(scene->sdfs[box].type == box_geometry.type);
    assert(memcmp(scene->sdfs[box].size, box_geometry.size,
                  sizeof(box_geometry.size)) == 0);
    assert(memcmp(scene->sdfs[box].inverseTransform, box_geometry.inverseTransform,
                  sizeof(box_geometry.inverseTransform)) == 0);
    assert(scene->sdfs[box].operation == box_geometry.operation &&
           scene->sdfs[box].smoothness == box_geometry.smoothness);
    assert(obs_sdf_sync_frame(0) && gpu[0][box].color[0] == .75f);
    assert(gpu[1][box].color[0] != .75f);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert_appearance(box, &box_before);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert_appearance(box, &source);
    assert(obs_sdf_appearance_paste(terrain) == OBS_SDF_EDIT_OK);
    assert_appearance(terrain, &source);
    assert(scene->sdfs[terrain].type == SDF_TYPE_TERRAIN &&
           scene->sdfs[terrain]._pad1 == 8 &&
           scene->sdfs[terrain].size[0] == .5f);

    /* Paste onto an already matching appearance creates no undo entry. */
    assert(obs_sdf_inspector_clear() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_appearance_paste(terrain) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == 0);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_appearance_copy(box) == OBS_SDF_EDIT_CONFLICT);
    assert(obs_sdf_appearance_paste(box) == OBS_SDF_EDIT_CONFLICT);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);

    const float old_metallic = scene->sdfs[sphere].metallic;
    scene->sdfs[sphere].metallic = NAN;
    assert(obs_sdf_appearance_copy(sphere) == OBS_SDF_EDIT_STALE);
    scene->sdfs[sphere].metallic = old_metallic;
    const uint64_t old_ticket = obs_sculpt_ticket_any_at(sphere);
    reset();
    ObsSdfEditValues unchanged = {.rgba = {9, 9, 9, 9}};
    assert(obs_sdf_inspector_read_appearance(old_ticket, &unchanged) ==
           OBS_SDF_EDIT_STALE);
    assert(unchanged.rgba[0] == 9);
    const uint32_t fresh = create(SDF_TYPE_BOX, 1);
    assert(obs_sdf_appearance_paste(fresh) == OBS_SDF_EDIT_OK);
    assert_appearance(fresh, &source);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_sync_frame(1));
    assert(gpu[1][fresh].type == SDF_TYPE_BOX);
    obs_sdf_appearance_clear();
    assert(obs_sdf_appearance_paste(fresh) == OBS_SDF_EDIT_EMPTY);
    preset_roundtrip(fresh);
    const char *external = getenv("OBSIDIAN_C62_PRESET_INPUT");
    if (external) {
        assert(obs_sdf_appearance_load_file(external));
        const char *capture = getenv("OBSIDIAN_C62_CAPTURE_FILE");
        if (capture) assert(obs_sdf_appearance_save_file(capture));
        const uint32_t target = create(SDF_TYPE_SPHERE, 4);
        assert(obs_sdf_appearance_paste(target) == OBS_SDF_EDIT_OK);
        ObsSdfEditValues loaded;
        read_appearance(target, &loaded);
        assert(fabs(loaded.rgba[0] - .2) < 1e-6 && loaded.rgba[3] == 1. &&
               fabs(loaded.roughness - .7) < 1e-6 &&
               fabs(loaded.metallic - .6) < 1e-6 &&
               fabs(loaded.emissive[2] - .3) < 1e-6 &&
               loaded.emissive_strength == 99.);
        assert(obs_sdf_sync_frame(1) && gpu[1][target].color[0] == .2f);
        assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
        assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    }
    reset();
    puts("Cycle 61 + 62: portable appearance presets, scene undo and GPU slots PASS");
    return 0;
}

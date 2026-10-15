#include "skin_override_index.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void multiply(const float left[16], const float right[16],
                     float result[16]) {
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            float value = 0;
            for (unsigned k = 0; k < 4; ++k)
                value += left[row * 4 + k] * right[k * 4 + column];
            result[row * 4 + column] = value;
        }
    }
}

static void ordered_application(const ObsSkinJointRef *source,
                                size_t source_count,
                                const ObsSkinOverrideIndex *index) {
    bool active[3][4] = {{false}};
    float overrides[3][4][16] = {{{0}}};
    float old_world[3][16] = {{0}};
    float new_world[3][16] = {{0}};
    for (unsigned mesh = 0; mesh < 3; ++mesh) {
        for (unsigned joint = 0; joint < 4; ++joint) {
            active[mesh][joint] = (mesh + joint) % 2u == 0u;
            for (unsigned i = 0; i < 16; ++i)
                overrides[mesh][joint][i] = i % 5u == 0u ? 1.0f : 0.0f;
            overrides[mesh][joint][1] = 0.1f * (float)(mesh + 1);
            overrides[mesh][joint][4] = 0.07f * (float)(joint + 1);
            overrides[mesh][joint][12] = (float)(mesh + joint + 1);
        }
    }
    for (unsigned node = 0; node < 3; ++node) {
        old_world[node][0] = old_world[node][5] =
            old_world[node][10] = old_world[node][15] = 1.0f;
        old_world[node][12] = new_world[node][12] = (float)node;
        new_world[node][0] = new_world[node][5] =
            new_world[node][10] = new_world[node][15] = 1.0f;
    }

    for (unsigned node = 0; node < 3; ++node) {
        for (unsigned mesh = 0; mesh < 3; ++mesh) {
            for (size_t i = 0; i < source_count; ++i) {
                if (source[i].mesh_index != mesh ||
                    source[i].node_index != node)
                    continue;
                const unsigned joint = source[i].joint_index;
                if (active[mesh][joint]) {
                    float next[16];
                    multiply(overrides[mesh][joint], old_world[node], next);
                    memcpy(old_world[node], next, sizeof(next));
                }
                break;
            }
        }
        for (size_t i = index->offsets[node];
             i < index->offsets[node + 1]; ++i) {
            const unsigned mesh = index->refs[i].mesh_index;
            const unsigned joint = index->refs[i].joint_index;
            if (active[mesh][joint]) {
                float next[16];
                multiply(overrides[mesh][joint], new_world[node], next);
                memcpy(new_world[node], next, sizeof(next));
            }
        }
    }
    assert(memcmp(old_world, new_world, sizeof(old_world)) == 0);
}

static void stable_buckets(void) {
    const ObsSkinJointRef source[] = {
        {0, 0, 0}, {2, 0, 1}, {2, 0, 2},
        {2, 1, 0}, {1, 1, 3}, {0, 2, 1}, {9, 2, 5},
    };
    ObsSkinOverrideIndex index = {0};
    assert(obs_skin_override_index_build(3, 3, source,
                                         sizeof(source) / sizeof(source[0]),
                                         &index));
    const size_t expected_offsets[] = {0, 2, 3, 5};
    assert(memcmp(index.offsets, expected_offsets, sizeof(expected_offsets)) == 0);
    const ObsSkinOverrideRef expected_refs[] = {
        {0, 0}, {2, 1},
        {1, 3},
        {0, 1}, {1, 0},
    };
    assert(index.ref_count == sizeof(expected_refs) / sizeof(expected_refs[0]));
    assert(memcmp(index.refs, expected_refs, sizeof(expected_refs)) == 0);
    ordered_application(source, sizeof(source) / sizeof(source[0]), &index);

    size_t *old_offsets = index.offsets;
    ObsSkinOverrideRef *old_refs = index.refs;
    assert(memcmp(index.refs, expected_refs, sizeof(expected_refs)) == 0);

    const ObsSkinJointRef invalid_mesh[] = {{0, 3, 0}};
    assert(!obs_skin_override_index_build(3, 3, invalid_mesh, 1, &index));
    assert(index.offsets == old_offsets && index.refs == old_refs);
    const ObsSkinJointRef unordered[] = {{1, 0, 1}, {0, 0, 0}};
    assert(!obs_skin_override_index_build(3, 3, unordered, 2, &index));
    assert(index.offsets == old_offsets && index.refs == old_refs);
    obs_skin_override_index_destroy(&index);
    assert(!index.offsets && !index.refs && index.ref_count == 0);
}

static void empty_and_invalid_inputs(void) {
    ObsSkinOverrideIndex index = {0};
    assert(obs_skin_override_index_build(2, 0, NULL, 0, &index));
    assert(index.node_count == 2 && index.ref_count == 0 && index.refs == NULL);
    assert(index.offsets[0] == 0 && index.offsets[1] == 0 &&
           index.offsets[2] == 0);
    assert(!obs_skin_override_index_build(1, 1, NULL, 1, &index));
    assert(index.node_count == 2 && index.ref_count == 0);
    assert(!obs_skin_override_index_build(SIZE_MAX, 1, NULL, 0, &index));
    obs_skin_override_index_destroy(&index);
    assert(obs_skin_override_index_build(0, 0, NULL, 0, &index));
    assert(index.offsets[0] == 0);
    obs_skin_override_index_destroy(&index);
}

int main(void) {
    stable_buckets();
    empty_and_invalid_inputs();
    puts("skin override index: stable buckets, bounds, and failure atomicity PASS");
    return 0;
}

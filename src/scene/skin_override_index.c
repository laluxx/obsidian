#include "skin_override_index.h"
#include <stdlib.h>
#include <string.h>

void obs_skin_override_index_destroy(ObsSkinOverrideIndex *index) {
    if (!index)
        return;
    free(index->offsets);
    free(index->refs);
    *index = (ObsSkinOverrideIndex){0};
}

bool obs_skin_override_index_build(size_t node_count, size_t mesh_count,
                                   const ObsSkinJointRef *source,
                                   size_t source_count,
                                   ObsSkinOverrideIndex *index) {
    if (!index || (source_count && !source) ||
        node_count > UINT32_MAX || mesh_count > UINT32_MAX ||
        node_count == SIZE_MAX ||
        node_count + 1 > SIZE_MAX / sizeof(size_t) ||
        node_count > SIZE_MAX / sizeof(size_t))
        return false;

    ObsSkinOverrideIndex next = {
        .node_count = node_count,
        .offsets = calloc(node_count + 1, sizeof(size_t)),
    };
    size_t *cursor =
        node_count ? malloc(node_count * sizeof(size_t)) : NULL;
    size_t *seen_mesh =
        node_count ? calloc(node_count, sizeof(size_t)) : NULL;
    if (!next.offsets || (node_count && (!cursor || !seen_mesh))) {
        obs_skin_override_index_destroy(&next);
        free(cursor);
        free(seen_mesh);
        return false;
    }

    uint32_t previous_mesh = 0, previous_joint = 0;
    for (size_t i = 0; i < source_count; ++i) {
        const ObsSkinJointRef ref = source[i];
        if (ref.mesh_index >= mesh_count ||
            (i && (ref.mesh_index < previous_mesh ||
                   (ref.mesh_index == previous_mesh &&
                    ref.joint_index < previous_joint)))) {
            obs_skin_override_index_destroy(&next);
            free(cursor);
            free(seen_mesh);
            return false;
        }
        previous_mesh = ref.mesh_index;
        previous_joint = ref.joint_index;
        if (ref.node_index >= node_count)
            continue;
        const size_t marker = (size_t)ref.mesh_index + 1u;
        if (seen_mesh[ref.node_index] == marker)
            continue;
        seen_mesh[ref.node_index] = marker;
        ++next.offsets[(size_t)ref.node_index + 1];
    }
    for (size_t n = 0; n < node_count; ++n) {
        next.offsets[n + 1] += next.offsets[n];
        cursor[n] = next.offsets[n];
    }
    next.ref_count = next.offsets[node_count];
    if (next.ref_count > SIZE_MAX / sizeof(ObsSkinOverrideRef)) {
        obs_skin_override_index_destroy(&next);
        free(cursor);
        free(seen_mesh);
        return false;
    }
    next.refs = next.ref_count
                    ? malloc(next.ref_count * sizeof(ObsSkinOverrideRef))
                    : NULL;
    if (next.ref_count && !next.refs) {
        obs_skin_override_index_destroy(&next);
        free(cursor);
        free(seen_mesh);
        return false;
    }
    if (node_count)
        memset(seen_mesh, 0, node_count * sizeof(size_t));
    for (size_t i = 0; i < source_count; ++i) {
        const ObsSkinJointRef ref = source[i];
        if (ref.node_index >= node_count)
            continue;
        const size_t marker = (size_t)ref.mesh_index + 1u;
        if (seen_mesh[ref.node_index] == marker)
            continue;
        seen_mesh[ref.node_index] = marker;
        const size_t at = cursor[ref.node_index]++;
        next.refs[at] = (ObsSkinOverrideRef){
            .mesh_index = ref.mesh_index,
            .joint_index = ref.joint_index,
        };
    }
    free(cursor);
    free(seen_mesh);

    obs_skin_override_index_destroy(index);
    *index = next;
    return true;
}

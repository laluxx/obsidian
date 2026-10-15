#include "animation_editor.h"
#include "context.h"
#include "gltf_loader.h"
#include "vulkan_setup.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    NODE_COUNT = 65,
    MESH_COUNT = 3,
    JOINT_COUNT = 5,
};

VulkanContext context;
AnimEditorState g_anim_editor;
mat4 *jointSSBOMapped[MAX_FRAMES_IN_FLIGHT];
static Scene fixture_scene;
Scene *scene = &fixture_scene;
static OmdlSceneGraph graph;
static GLTFInstance instance;
static OmdlNode nodes[NODE_COUNT];
static mat4 local[NODE_COUNT];
static mat4 world[NODE_COUNT];
static uint32_t topology[NODE_COUNT];
static OmdlSkin skins[1];
static uint32_t skin_joints[JOINT_COUNT] = {1, 2, 2, 3, 4};
static Mesh meshes[MESH_COUNT];
static MeshBoneOverride overrides[MESH_COUNT][JOINT_COUNT];

void markMeshDirty(VulkanContext *ctx, uint32_t mesh_index) {
    (void)ctx;
    (void)mesh_index;
}

static void initialize_fixture(void) {
    obs_skin_override_index_destroy(&graph.bone_override_index);
    obs_animation_channel_index_destroy(&graph.animation_channel_index);
    fixture_scene = (Scene){0};
    graph = (OmdlSceneGraph){0};
    instance = (GLTFInstance){0};
    memset(meshes, 0, sizeof(meshes));
    memset(overrides, 0, sizeof(overrides));

    graph.node_count = NODE_COUNT;
    graph.nodes = nodes;
    graph.local_transforms = local;
    graph.world_transforms = world;
    graph.topology_order = topology;
    graph.skin_count = 1;
    graph.skins = skins;
    graph.skin_joints = skin_joints;
    graph.script_translations = NULL;
    skins[0] = (OmdlSkin){.joints_count = JOINT_COUNT};

    for (unsigned n = 0; n < NODE_COUNT; ++n) {
        int32_t parent = n == 0 ? -1 : (int32_t)n - 1;
        if (n == 3)
            parent = 1;
        else if (n == 4)
            parent = 2;
        nodes[n] = (OmdlNode){.parent = parent,
                              .mesh_idx = -1,
                              .skin_idx = -1};
        topology[n] = n;
        glm_mat4_identity(nodes[n].base_matrix);
        nodes[n].base_matrix[3][0] = (float)n * 0.25f;
        nodes[n].base_matrix[3][1] = (float)n * -0.125f;
    }
    topology[3] = 4;
    topology[4] = 3;
    nodes[1].skin_idx = 0;

    for (unsigned m = 0; m < MESH_COUNT; ++m) {
        Mesh *mesh = &meshes[m];
        mesh->node = (void *)(uintptr_t)1;
        mesh->jointCount = JOINT_COUNT;
        mesh->bone_overrides = overrides[m];
        mesh->bone_override_capacity = JOINT_COUNT;
        glm_mat4_identity(mesh->local_transform);
        for (unsigned j = 0; j < JOINT_COUNT; ++j) {
            MeshBoneOverride *override = &overrides[m][j];
            glm_mat4_identity(override->world_offset);
            override->world_offset[0][1] = 0.03f * (float)(m + 1);
            override->world_offset[3][0] = 0.1f * (float)(m + j + 1);
            override->active = (m + j) % 2u == 0u;
        }
    }

    instance.gltf_data = &graph;
    instance.mesh_start_index = 0;
    instance.mesh_count = MESH_COUNT;
    glm_mat4_identity(instance.instance_transform);
    fixture_scene.gltf_instances = &instance;
    fixture_scene.gltf_instance_count = 1;
    fixture_scene.meshes.items = meshes;
    fixture_scene.meshes.count = MESH_COUNT;
    context.currentFrame = 0;
    memset(jointSSBOMapped, 0, sizeof(jointSSBOMapped));
    g_anim_editor.time = 0;
}

static void legacy_reference(mat4 result[NODE_COUNT]) {
    for (uint32_t k = 0; k < NODE_COUNT; ++k) {
        const uint32_t curr = topology[k];
        const int32_t parent = nodes[curr].parent;
        if (parent >= 0) {
            glm_mat4_mul(result[parent], local[curr], result[curr]);
        } else {
            glm_mat4_mul(instance.instance_transform, local[curr], result[curr]);
        }

        for (size_t m = 0; m < instance.mesh_count; ++m) {
            Mesh *mesh = &meshes[m];
            if (!mesh->node || mesh->jointCount <= 0)
                continue;
            const uint32_t node_index = (uint32_t)(uintptr_t)mesh->node;
            if (node_index >= graph.node_count)
                continue;
            const int32_t skin_index = nodes[node_index].skin_idx;
            if (skin_index < 0 || (uint32_t)skin_index >= graph.skin_count)
                continue;
            const OmdlSkin *skin = &skins[skin_index];
            for (uint32_t j = 0; j < skin->joints_count; ++j) {
                if (skin_joints[skin->joints_offset + j] != curr)
                    continue;
                MeshBoneOverride *override =
                    mesh_bone_override_get(mesh, j, false);
                if (override && override->active) {
                    mat4 next;
                    glm_mat4_mul(override->world_offset, result[curr], next);
                    glm_mat4_copy(next, result[curr]);
                }
                break;
            }
        }
    }
}

static void assert_world_matches_reference(void) {
    mat4 expected[NODE_COUNT];
    legacy_reference(expected);
    for (unsigned n = 0; n < NODE_COUNT; ++n) {
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned r = 0; r < 4; ++r)
                assert(fabsf(world[n][c][r] - expected[n][c][r]) < 1e-6f);
        }
    }
}

static void production_indexed_path(void) {
    initialize_fixture();
    ObsSkinJointRef source[MESH_COUNT * JOINT_COUNT];
    size_t count = 0;
    for (uint32_t m = 0; m < MESH_COUNT; ++m) {
        for (uint32_t j = 0; j < JOINT_COUNT; ++j) {
            source[count++] = (ObsSkinJointRef){
                .node_index = skin_joints[j],
                .mesh_index = m,
                .joint_index = j,
            };
        }
    }
    assert(obs_skin_override_index_build(
        graph.node_count, instance.mesh_count, source, count,
        &graph.bone_override_index));
    graph.bone_override_index_attempted = true;
    graph.bone_override_index_ready = true;
    animate_scene(&fixture_scene, 0);
    assert_world_matches_reference();
}

static void production_fallback_path(void) {
    initialize_fixture();
    animate_scene(&fixture_scene, 0);
    assert_world_matches_reference();
}

static void production_animation_sampling(void) {
    initialize_fixture();
    enum { CHANNEL_COUNT = 65, FLOATS_PER_CHANNEL = 10 };
    OmdlAnimation animation = {
        .duration = 1.0f,
        .channel_count = CHANNEL_COUNT,
        .channel_offset = 0,
    };
    OmdlChannel channels[CHANNEL_COUNT] = {0};
    float anim_floats[CHANNEL_COUNT * FLOATS_PER_CHANNEL] = {0};
    float weights[2] = {0};
    MorphData morph_data = {.weights = weights, .target_count = 2};
    meshes[0].node = (void *)(uintptr_t)2;
    meshes[0].morph_data = &morph_data;
    for (uint32_t c = 0; c < CHANNEL_COUNT; ++c) {
        const uint32_t offset = c * FLOATS_PER_CHANNEL;
        OmdlChannel *channel = &channels[c];
        channel->target_node = 2;
        channel->keyframe_count = 2;
        channel->times_offset = offset;
        channel->values_offset = offset + 2;
        anim_floats[offset] = 0.0f;
        anim_floats[offset + 1] = 1.0f;
        if (c == 0) {
            channel->path_type = cgltf_animation_path_type_weights;
            anim_floats[offset + 2] = 0.0f;
            anim_floats[offset + 3] = 1.0f;
            anim_floats[offset + 4] = 2.0f;
            anim_floats[offset + 5] = 3.0f;
        } else if (c < CHANNEL_COUNT - 2) {
            channel->path_type = cgltf_animation_path_type_translation;
            anim_floats[offset + 2] = 0.0f;
            anim_floats[offset + 5] = 2.0f * (float)c;
            if (c == 1)
                channel->target_node = 3;
        } else if (c == CHANNEL_COUNT - 2) {
            channel->path_type = cgltf_animation_path_type_rotation;
            anim_floats[offset + 5] = 1.0f;
            anim_floats[offset + 8] = 1.0f;
        } else {
            channel->path_type = cgltf_animation_path_type_scale;
            anim_floats[offset + 2] = 1.0f;
            anim_floats[offset + 3] = 1.0f;
            anim_floats[offset + 4] = 1.0f;
            anim_floats[offset + 5] = 3.0f;
            anim_floats[offset + 6] = 3.0f;
            anim_floats[offset + 7] = 3.0f;
        }
    }
    graph.anim_count = 1;
    graph.anims = &animation;
    graph.channels = channels;
    graph.anim_floats = anim_floats;
    g_anim_editor.time = 0.5f;

    animate_scene(&fixture_scene, 0);

    assert(graph.animation_channel_index.ready);
    assert(graph.animation_channel_index.channel_index_count == 64);
    assert(graph.animation_channel_index.weight_index_count == 1);
    assert(fabsf(local[2][3][0] - 62.0f) < 1e-6f);
    assert(fabsf(local[3][3][0] - 1.0f) < 1e-6f);
    assert(fabsf(local[2][0][0]) < 1e-5f);
    assert(fabsf(fabsf(local[2][0][1]) - 2.0f) < 1e-5f);
    assert(fabsf(weights[0] - 1.0f) < 1e-6f);
    assert(fabsf(weights[1] - 2.0f) < 1e-6f);
    assert_world_matches_reference();
}

int main(void) {
    production_indexed_path();
    production_fallback_path();
    production_animation_sampling();
    obs_skin_override_index_destroy(&graph.bone_override_index);
    obs_animation_channel_index_destroy(&graph.animation_channel_index);
    puts("animate_scene: indexed/fallback bone matrices and indexed T/R/S/weights PASS");
    return 0;
}
